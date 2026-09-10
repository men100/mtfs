#!/usr/bin/env python3
"""Safely flash caller-supplied STM32N657 trusted-header images."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence

import tool_discovery


UINT32_LIMIT = 1 << 32
TRUSTED_HEADER_MAGIC = b"STM2"
BOARD_FLASH_BASE = 0x70000000
BOARD_FLASH_SIZE = 0x08000000
BOARD_IMAGE_ADDRESSES = {"fsbl": 0x70000000, "appli": 0x70100000}
BOARD_KEY_RANGES = {(0x77FFE000, 0x1000), (0x77FFF000, 0x1000)}


class FlashError(RuntimeError):
    """Raised before an unsafe or ambiguous flash operation."""


@dataclass(frozen=True)
class Region:
    name: str
    address: int
    size: int

    @property
    def end(self) -> int:
        return checked_end(self.address, self.size, self.name)


@dataclass(frozen=True)
class Layout:
    board_id: str
    flash: Region
    loader_name: str
    images: dict[str, Region]
    prohibited: tuple[Region, ...]


@dataclass(frozen=True)
class FlashImage:
    role: str
    path: Path
    address: int


def address(value: str) -> str:
    if re.fullmatch(r"0x[0-9a-fA-F]{8}", value) is None:
        raise argparse.ArgumentTypeError("address must be an eight-digit hexadecimal value")
    return value.lower()


def _uint(value: object, field: str) -> int:
    if isinstance(value, bool):
        raise FlashError(f"{field} must be an unsigned integer")
    if isinstance(value, int):
        result = value
    elif isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]+", value):
        result = int(value, 16)
    else:
        raise FlashError(f"{field} must be an integer or 0x-prefixed hex string")
    if result < 0 or result >= UINT32_LIMIT:
        raise FlashError(f"{field} is outside the 32-bit address space")
    return result


def checked_end(start: int, size: int, name: str) -> int:
    if start < 0 or start >= UINT32_LIMIT or size <= 0:
        raise FlashError(f"invalid range for {name}")
    end = start + size
    if end > UINT32_LIMIT or end <= start:
        raise FlashError(f"address overflow for {name}")
    return end


def overlaps(left: Region, right: Region) -> bool:
    return left.address < right.end and right.address < left.end


def _inside(inner: Region, outer: Region) -> bool:
    return inner.address >= outer.address and inner.end <= outer.end


def load_layout(path: Path) -> Layout:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("schema_version") != 1:
            raise FlashError("unsupported layout schema_version")
        external = data["external_flash"]
        flash = Region("external-flash", _uint(external["base"], "external_flash.base"),
                       _uint(external["size"], "external_flash.size"))
        images = {
            role: Region(role, _uint(item["address"], f"images.{role}.address"),
                         _uint(item["region_size"], f"images.{role}.region_size"))
            for role, item in data["images"].items()
        }
        if set(images) != {"fsbl", "appli"}:
            raise FlashError("layout must define exactly fsbl and appli image regions")
        prohibited = tuple(
            Region(str(item["name"]), _uint(item["address"], "prohibited.address"),
                   _uint(item["size"], "prohibited.size"))
            for item in data["prohibited_ranges"])
        layout = Layout(str(data["board_id"]), flash,
                        str(external["external_loader"]), images, prohibited)
    except (AttributeError, KeyError, TypeError, ValueError,
            json.JSONDecodeError) as error:
        raise FlashError(f"malformed layout: {error}") from error

    if (flash.address, flash.size) != (BOARD_FLASH_BASE, BOARD_FLASH_SIZE):
        raise FlashError("layout does not match STM32N6570-DK external flash")
    for role, expected_address in BOARD_IMAGE_ADDRESSES.items():
        if images[role].address != expected_address:
            raise FlashError(f"layout has an unexpected {role} address")
    protected = {(item.address, item.size) for item in prohibited}
    if not BOARD_KEY_RANGES.issubset(protected):
        raise FlashError("layout does not protect both fleet key slots")

    all_regions = list(images.values()) + list(prohibited)
    for region in all_regions:
        if not _inside(region, flash):
            raise FlashError(f"{region.name} is outside external flash")
    for index, left in enumerate(all_regions):
        for right in all_regions[index + 1:]:
            if overlaps(left, right):
                raise FlashError(f"layout regions overlap: {left.name} and {right.name}")
    return layout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _trusted_header(path: Path) -> bool:
    with path.open("rb") as stream:
        return stream.read(4) == TRUSTED_HEADER_MAGIC


def _project_trusted_binary(project: Path, subproject: str) -> Path:
    directory = project / subproject / "Release"
    if not directory.is_dir():
        raise FlashError(f"{subproject} Release directory not found: {directory}")
    candidates = sorted(path.resolve() for path in directory.glob("*.bin")
                        if "trusted" in path.stem.casefold())
    if not candidates:
        raise FlashError(f"{subproject} trusted-header binary not found: {directory}")
    if len(candidates) > 1:
        joined = "\n  ".join(str(path) for path in candidates)
        raise FlashError(
            f"multiple {subproject} trusted binaries; use --{subproject.lower()}:\n  {joined}")
    return candidates[0]


def resolve_images(args: argparse.Namespace, layout: Layout) -> list[FlashImage]:
    legacy = args.binary is not None or args.address is not None
    if legacy:
        if args.binary is None or args.address is None:
            raise FlashError("--binary and --address must be specified together")
        if any(value is not None for value in (args.project, args.fsbl, args.appli)):
            raise FlashError("single --binary mode cannot be combined with project options")
        return [FlashImage("image", args.binary.expanduser().resolve(),
                           int(args.address, 16))]

    project = args.project.expanduser().resolve() if args.project else None
    if project is not None and not project.is_dir():
        raise FlashError(f"project directory not found: {project}")
    explicit = {
        "fsbl": args.fsbl.expanduser().resolve() if args.fsbl else None,
        "appli": args.appli.expanduser().resolve() if args.appli else None,
    }
    roles = ("fsbl", "appli") if args.target == "all" else (args.target,)
    result = []
    for role in roles:
        path = explicit[role]
        if path is None and project is not None:
            subproject = "FSBL" if role == "fsbl" else "Appli"
            path = _project_trusted_binary(project, subproject)
        if path is None:
            raise FlashError(f"{role} image is required for --target {args.target}")
        result.append(FlashImage(role, path, layout.images[role].address))
    return result


def validate_images(images: Sequence[FlashImage], layout: Layout) -> None:
    actual: list[Region] = []
    for image in images:
        if not image.path.is_file():
            raise FlashError(f"binary not found: {image.path}")
        if not _trusted_header(image.path):
            raise FlashError(f"trusted-header magic not found: {image.path}")
        region = Region(image.role, image.address, image.path.stat().st_size)
        if not _inside(region, layout.flash):
            raise FlashError(f"{image.role} image is outside external flash")
        if image.role in layout.images:
            allowed = layout.images[image.role]
            if image.address != allowed.address or not _inside(region, allowed):
                raise FlashError(f"{image.role} image exceeds its configured region")
        for reserved in layout.prohibited:
            if overlaps(region, reserved):
                raise FlashError(
                    f"{image.role} image overlaps prohibited range {reserved.name}")
        actual.append(region)
    for index, left in enumerate(actual):
        for right in actual[index + 1:]:
            if overlaps(left, right):
                raise FlashError(f"images overlap: {left.name} and {right.name}")


def _connection(args: argparse.Namespace, serial: str | None = None) -> list[str]:
    result = ["-c", args.connection, f"mode={args.mode}", f"ap={args.access_port}"]
    if serial:
        result.append(f"sn={serial}")
    return result


def flash_command(
    programmer_or_args: Path | argparse.Namespace,
    image: FlashImage | None = None,
    loader: Path | None = None,
    connection: Sequence[str] | None = None,
) -> list[str]:
    """Build one verified write command; supports the former Namespace API."""
    if isinstance(programmer_or_args, argparse.Namespace):
        args = programmer_or_args
        return [str(args.programmer), "-c", args.connection, f"mode={args.mode}",
                f"ap={args.access_port}", "-el", str(args.external_loader),
                "-hardRst", "-w", str(args.binary), args.address, "-v"]
    if image is None or loader is None or connection is None:
        raise FlashError("incomplete flash command arguments")
    return ([str(programmer_or_args), *connection, "-el", str(loader),
             "-hardRst", "-w", str(image.path), f"0x{image.address:08x}", "-v"])


def probe_serials(
    programmer: Path,
    timeout: float,
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[str]:
    completed = runner([str(programmer), "-l", "st-link"], check=True,
                       capture_output=True, text=True, errors="replace",
                       timeout=timeout, shell=False)
    output = completed.stdout + completed.stderr
    values = re.findall(r"(?:ST-?LINK\s+SN|Serial Number)\s*:\s*([^\s]+)",
                        output, flags=re.IGNORECASE)
    result = []
    for value in values:
        if value not in result:
            result.append(value)
    return result


def select_probe(detected: Sequence[str], requested: str | None) -> str:
    if requested:
        matches = [value for value in detected
                   if value.casefold() == requested.casefold()]
        if not matches:
            raise FlashError("requested ST-LINK probe was not detected")
        return matches[0]
    if not detected:
        raise FlashError("no ST-LINK probe detected")
    if len(detected) > 1:
        raise FlashError("multiple ST-LINK probes detected; use --probe-serial")
    return detected[0]


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "STM32N6570-DKへcaller-supplied trusted-header imageを書込む。"
            "mass erase、option byte、OTP、key provisioningは行わない。"))
    result.add_argument("--project", type=Path,
                        help="FSBL/ReleaseとAppli/Releaseを含むproject root")
    result.add_argument("--fsbl", type=Path, help="FSBL trusted-header binary")
    result.add_argument("--appli", type=Path, help="Appli trusted-header binary")
    result.add_argument("--target", choices=("fsbl", "appli", "all"), default="all")
    result.add_argument("--binary", type=Path, help="単一image互換modeのbinary")
    result.add_argument("--address", type=address, help="単一image互換modeのaddress")
    result.add_argument("--programmer", type=Path,
                        help="STM32_Programmer_CLI（省略時は安全に自動検出）")
    result.add_argument("--external-loader", type=Path,
                        help="board external loader（省略時はprogrammer packageから検出）")
    result.add_argument("--layout", type=Path,
                        default=Path(__file__).with_name("layout.json"))
    result.add_argument("--connection", default="port=SWD")
    result.add_argument("--mode", default="HOTPLUG")
    result.add_argument("--access-port", default="1")
    result.add_argument("--probe-serial")
    result.add_argument("--timeout", type=float, default=120.0)
    result.add_argument("--dry-run", action="store_true",
                        help="boardへ接続せず検査とcommand表示だけを行う")
    result.add_argument("--confirm-flash", action="store_true",
                        help="board変更の明示確認。実書込みでは必須")
    result.add_argument("--private-manifest", type=Path,
                        help="成功後にpath-neutral private manifestを新規作成")
    result.add_argument("--fleet-test-key-used", action="store_true")
    return result


def _manifest(
    args: argparse.Namespace,
    layout: Layout,
    programmer_version: str,
    loader: Path,
    images: Sequence[FlashImage],
) -> dict[str, object]:
    result: dict[str, object] = {
        "schema_version": 1,
        "board_id": layout.board_id,
        "flashed_at_utc": datetime.now(timezone.utc).isoformat(),
        "programmer_version": programmer_version,
        "external_loader_name": loader.name,
        "verify_required": True,
        "mass_erase": False,
        "key_slots_preserved": True,
        "fleet_test_key_used": args.fleet_test_key_used,
        "images": [{"role": image.role, "binary_name": image.path.name,
                    "address": f"0x{image.address:08x}",
                    "size": image.path.stat().st_size,
                    "sha256": sha256(image.path), "result": "program-and-verify-pass"}
                   for image in images],
    }
    if args.fleet_test_key_used:
        result["key_notice"] = (
            "TEST/DEMO KEY - PUBLIC AND NOT SECRET - DO NOT USE IN PRODUCTION")
    return result


def _write_manifest(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, sort_keys=True, indent=2)
            stream.write("\n")
        os.replace(temporary_name, path)
    finally:
        try:
            Path(temporary_name).unlink()
        except FileNotFoundError:
            pass


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.timeout <= 0:
            raise FlashError("--timeout must be positive")
        manifest_path = (args.private_manifest.expanduser().resolve()
                         if args.private_manifest else None)
        if manifest_path is not None and manifest_path.exists():
            raise FlashError(f"private manifest already exists: {manifest_path}")
        layout_path = args.layout.expanduser().resolve()
        layout = load_layout(layout_path)
        images = resolve_images(args, layout)
        validate_images(images, layout)
        programmer, programmer_source = tool_discovery.resolve_executable(
            tool_discovery.PROGRAMMER, args.programmer)
        loader, loader_source = tool_discovery.resolve_loader(
            args.external_loader, programmer)
        if loader.name.casefold() != layout.loader_name.casefold():
            raise FlashError(
                f"external loader does not match layout: {loader.name}")
        version = tool_discovery.tool_version(programmer, args.timeout)
        print(f"board={layout.board_id}")
        print(f"programmer={programmer} source={programmer_source}")
        print(f"external-loader={loader} source={loader_source}")
        print(f"tool-version={version}")
        for image in images:
            print(f"[{image.role}] address=0x{image.address:08x} "
                  f"size={image.path.stat().st_size} sha256={sha256(image.path)} "
                  f"file={image.path.name}")

        if args.dry_run:
            connection = _connection(args, args.probe_serial)
            for image in images:
                print(subprocess.list2cmdline(
                    flash_command(programmer, image, loader, connection)))
            print(subprocess.list2cmdline([str(programmer), *connection, "-rst"]))
            print("DRY-RUN: board was not opened or modified")
            return 0
        if not args.confirm_flash:
            raise FlashError("refusing to flash without --confirm-flash")

        probe = select_probe(probe_serials(programmer, args.timeout),
                             args.probe_serial)
        connection = _connection(args, probe)
        for image in images:
            subprocess.run(flash_command(programmer, image, loader, connection),
                           check=True, timeout=args.timeout, shell=False)
        subprocess.run([str(programmer), *connection, "-rst"], check=True,
                       timeout=args.timeout, shell=False)
        if manifest_path is not None:
            _write_manifest(manifest_path, _manifest(
                args, layout, version, loader, images))
        return 0
    except KeyboardInterrupt:
        print("interrupted; no further flash command was started", file=sys.stderr)
        return 130
    except subprocess.TimeoutExpired as error:
        print(f"error: programmer timed out: {error}", file=sys.stderr)
        return 124
    except subprocess.CalledProcessError as error:
        print(f"error: programmer failed with exit code {error.returncode}",
              file=sys.stderr)
        return error.returncode or 1
    except (OSError, FlashError, tool_discovery.DiscoveryError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
