#!/usr/bin/env python3
"""Create STM32N657 no-key trusted-header development images safely."""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

import tool_discovery


TRUSTED_HEADER_MAGIC = b"STM2"


@dataclass(frozen=True)
class Image:
    role: str
    source: Path
    output: Path


class PreparationError(RuntimeError):
    """Raised before an unsafe or ambiguous generation operation."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def payload_matches(source: Path, trusted_image: Path) -> bool:
    """Return whether the trusted image ends with the exact source payload."""
    source_size = source.stat().st_size
    output_size = trusted_image.stat().st_size
    if source_size <= 0 or output_size <= source_size:
        return False
    with source.open("rb") as expected, trusted_image.open("rb") as actual:
        actual.seek(output_size - source_size)
        while True:
            left = expected.read(1024 * 1024)
            right = actual.read(len(left))
            if left != right:
                return False
            if not left:
                return True


def _make_writable(path: Path) -> None:
    path.chmod(path.stat().st_mode | stat.S_IWRITE)


def _remove_tree(path: Path) -> None:
    if not path.exists():
        return
    for child in path.rglob("*"):
        try:
            _make_writable(child)
        except OSError:
            pass
    shutil.rmtree(path)


def signing_command(
    signing_tool: Path,
    source: Path,
    output: Path,
    *,
    option_flags: str = "0x80000000",
    load_address: str | None = None,
    header_type: str = "fsbl",
    header_version: str = "2.3",
) -> list[str]:
    """Build vendor argv without a shell.

    On STM32N6, 0x80000000 is an option flag, not a load address. An explicit
    load address is intentionally opt-in and is passed with -la.
    """
    result = [str(signing_tool), "-bin", str(source), "-nk", "-of",
              option_flags]
    if load_address is not None:
        result.extend(["-la", load_address])
    result.extend(["-t", header_type, "-hv", header_version, "-align",
                   "-o", str(output), "-dump", str(output)])
    return result


def command(args: argparse.Namespace) -> list[str]:
    """Compatibility entry point used by existing callers and tests."""
    return signing_command(
        args.signing_tool, args.input, args.output,
        option_flags=args.option_flags, load_address=args.load_address,
        header_type=args.header_type, header_version=args.header_version)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "STM32N657 no-key trusted-header development imageを生成する。"
            "暗号署名済みimageは生成しない。"))
    result.add_argument("--project", type=Path,
                        help="FSBL/ReleaseとAppli/Releaseを含むproject root")
    result.add_argument("--fsbl", type=Path, help="FSBL raw binary")
    result.add_argument("--appli", type=Path, help="Appli raw binary")
    result.add_argument("--output-dir", type=Path,
                        help="FSBL/Appli trusted image出力directory")
    result.add_argument("--input", type=Path,
                        help="単一image互換modeの入力binary")
    result.add_argument("--output", type=Path,
                        help="単一image互換modeの出力binary")
    result.add_argument("--signing-tool", type=Path,
                        help="STM32_SigningTool_CLI（省略時は安全に自動検出）")
    result.add_argument("--option-flags", default="0x80000000",
                        help="-ofへ渡すtrusted-header option flags")
    result.add_argument("--load-address",
                        help="特殊なflowでのみ-laへ渡す明示load address")
    result.add_argument("--header-type", default="fsbl")
    result.add_argument("--header-version", default="2.3")
    result.add_argument("--force", "--overwrite", dest="force",
                        action="store_true", help="既存出力をatomic replaceする")
    result.add_argument("--dry-run", action="store_true",
                        help="検査とcommand表示だけを行う")
    result.add_argument("--verbose", action="store_true")
    result.add_argument("--timeout", type=float, default=60.0,
                        help="vendor command timeout（秒）")
    return result


def _project_binary(project: Path, subproject: str) -> Path:
    directory = project / subproject / "Release"
    if not directory.is_dir():
        raise PreparationError(f"{subproject} Release directory not found: {directory}")
    candidates = sorted(
        path.resolve() for path in directory.glob("*.bin")
        if "trusted" not in path.stem.casefold())
    if not candidates:
        raise PreparationError(f"{subproject} raw binary not found: {directory}")
    if len(candidates) > 1:
        joined = "\n  ".join(str(path) for path in candidates)
        raise PreparationError(
            f"multiple {subproject} binaries; use --{subproject.lower()}:\n  {joined}")
    return candidates[0]


def resolve_images(args: argparse.Namespace) -> list[Image]:
    single_mode = args.input is not None or args.output is not None
    batch_options = (args.project, args.fsbl, args.appli, args.output_dir)
    if single_mode:
        if args.input is None or args.output is None:
            raise PreparationError("--input and --output must be specified together")
        if any(value is not None for value in batch_options):
            raise PreparationError(
                "single --input/--output mode cannot be combined with project options")
        return [Image("image", args.input.expanduser().resolve(),
                      args.output.expanduser().resolve())]

    project = args.project.expanduser().resolve() if args.project else None
    if project is not None and not project.is_dir():
        raise PreparationError(f"project directory not found: {project}")
    fsbl = args.fsbl.expanduser().resolve() if args.fsbl else None
    appli = args.appli.expanduser().resolve() if args.appli else None
    if project is not None:
        if fsbl is None:
            fsbl = _project_binary(project, "FSBL")
        if appli is None:
            appli = _project_binary(project, "Appli")
    if fsbl is None and appli is None:
        raise PreparationError("specify --project, --fsbl, or --appli")
    if args.output_dir is None:
        raise PreparationError("--output-dir is required for FSBL/Appli mode")
    output_dir = args.output_dir.expanduser().resolve()
    images = []
    if fsbl is not None:
        images.append(Image("fsbl", fsbl,
                            output_dir / f"{fsbl.stem}-trusted.bin"))
    if appli is not None:
        images.append(Image("appli", appli,
                            output_dir / f"{appli.stem}-trusted.bin"))
    outputs = [str(image.output).casefold() for image in images]
    if len(outputs) != len(set(outputs)):
        raise PreparationError("FSBL and Appli resolve to the same output path")
    return images


def _preflight(images: Sequence[Image], force: bool) -> None:
    for image in images:
        if not image.source.is_file():
            raise PreparationError(f"input binary not found: {image.source}")
        if image.source.stat().st_size == 0:
            raise PreparationError(f"input binary is empty: {image.source}")
        if image.source == image.output:
            raise PreparationError(f"input and output paths are identical: {image.source}")
        if image.output.exists() and not force:
            raise PreparationError(
                f"output exists; use --force explicitly: {image.output}")


def prepare_images(
    images: Sequence[Image],
    signing_tool: Path,
    args: argparse.Namespace,
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[dict[str, object]]:
    """Generate every image in temporary directories, then publish together."""
    _preflight(images, args.force)
    reports: list[dict[str, object]] = []
    if args.dry_run:
        for image in images:
            argv = signing_command(
                signing_tool, image.source, image.output,
                option_flags=args.option_flags, load_address=args.load_address,
                header_type=args.header_type, header_version=args.header_version)
            reports.append({"role": image.role, "input": image.source.name,
                            "input_size": image.source.stat().st_size,
                            "input_sha256": sha256(image.source),
                            "output": image.output.name, "command": argv,
                            "dry_run": True})
        return reports

    temporary: list[tuple[Image, Path, Path]] = []
    try:
        for image in images:
            image.output.parent.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(
                prefix=".mtfs-header-", dir=str(image.output.parent)))
            temporary_output = directory / image.output.name
            temporary.append((image, directory, temporary_output))
            argv = signing_command(
                signing_tool, image.source, temporary_output,
                option_flags=args.option_flags, load_address=args.load_address,
                header_type=args.header_type, header_version=args.header_version)
            runner(argv, check=True, timeout=args.timeout, shell=False)
            if not temporary_output.is_file():
                raise PreparationError(
                    f"vendor tool did not create output: {temporary_output}")
            with temporary_output.open("rb") as stream:
                if stream.read(4) != TRUSTED_HEADER_MAGIC:
                    raise PreparationError(
                        f"vendor output has no trusted-header magic: {image.output.name}")
            if not payload_matches(image.source, temporary_output):
                raise PreparationError(
                    f"vendor output payload differs from input: {image.output.name}")
            output_size = temporary_output.stat().st_size
            reports.append({"role": image.role, "input": image.source.name,
                            "input_size": image.source.stat().st_size,
                            "input_sha256": sha256(image.source),
                            "output": image.output.name,
                            "output_size": output_size,
                            "header_size": output_size - image.source.stat().st_size,
                            "payload_matches_input": True,
                            "output_sha256": sha256(temporary_output),
                            "command": argv, "dry_run": False})
        published: list[tuple[Image, Path | None]] = []
        try:
            for image, directory, temporary_output in temporary:
                _make_writable(temporary_output)
                backup = None
                if image.output.exists():
                    _make_writable(image.output)
                    backup = directory / f"{image.output.name}.previous"
                    os.replace(image.output, backup)
                try:
                    os.replace(temporary_output, image.output)
                except BaseException:
                    if backup is not None:
                        os.replace(backup, image.output)
                    raise
                published.append((image, backup))
        except BaseException:
            for image, backup in reversed(published):
                if image.output.exists():
                    _make_writable(image.output)
                    image.output.unlink()
                if backup is not None and backup.exists():
                    _make_writable(backup)
                    os.replace(backup, image.output)
            raise
    finally:
        for _image, directory, _temporary_output in temporary:
            _remove_tree(directory)
    return reports


def _print_report(report: dict[str, object], verbose: bool) -> None:
    print(f"[{report['role']}] input={report['input']} "
          f"size={report['input_size']} sha256={report['input_sha256']}")
    if report["dry_run"]:
        print(f"[{report['role']}] output={report['output']} DRY-RUN")
    else:
        print(f"[{report['role']}] output={report['output']} "
              f"size={report['output_size']} sha256={report['output_sha256']} "
              f"header-bytes={report['header_size']} payload=byte-identical")
    if verbose or report["dry_run"]:
        print(subprocess.list2cmdline([str(value) for value in report["command"]]))


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.timeout <= 0:
            raise PreparationError("--timeout must be positive")
        images = resolve_images(args)
        signing_tool, source = tool_discovery.resolve_executable(
            tool_discovery.SIGNING_TOOL, args.signing_tool)
        version = tool_discovery.tool_version(signing_tool, args.timeout)
        print(f"signing-tool={signing_tool}")
        print(f"tool-source={source}")
        print(f"tool-version={version}")
        print("header=no-key trusted-header development image; not cryptographically signed")
        for report in prepare_images(images, signing_tool, args):
            _print_report(report, args.verbose)
        return 0
    except KeyboardInterrupt:
        print("interrupted; temporary outputs were cleaned", file=sys.stderr)
        return 130
    except subprocess.TimeoutExpired as error:
        print(f"error: vendor tool timed out: {error}", file=sys.stderr)
        return 124
    except subprocess.CalledProcessError as error:
        print(f"error: vendor tool failed with exit code {error.returncode}",
              file=sys.stderr)
        return error.returncode or 1
    except (OSError, PreparationError, tool_discovery.DiscoveryError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
