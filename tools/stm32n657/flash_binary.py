#!/usr/bin/env python3
"""Flash one caller-supplied STM32N657 trusted-header image."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


def address(value: str) -> str:
    if re.fullmatch(r"0x[0-9a-fA-F]{8}", value) is None:
        raise argparse.ArgumentTypeError("address must be an eight-digit hexadecimal value")
    return value.lower()


def flash_command(args: argparse.Namespace) -> list[str]:
    return [str(args.programmer), "-c", args.connection, f"mode={args.mode}",
            f"ap={args.access_port}", "-el", str(args.external_loader), "-hardRst",
            "-w", str(args.binary), args.address, "-v"]


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--programmer", required=True, type=Path)
    result.add_argument("--external-loader", required=True, type=Path)
    result.add_argument("--binary", required=True, type=Path,
                        help="caller-supplied trusted-header image")
    result.add_argument("--address", required=True, type=address,
                        help="for example FSBL 0x70000000 or Appli 0x70100000")
    result.add_argument("--connection", default="port=SWD")
    result.add_argument("--mode", default="HOTPLUG")
    result.add_argument("--access-port", default="1")
    result.add_argument("--private-manifest", type=Path,
                        help="write a path-neutral record after successful flashing")
    result.add_argument("--fleet-test-key-used", action="store_true")
    result.add_argument("--confirm-flash", action="store_true",
                        help="required acknowledgement that the board will be modified")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    args.programmer = args.programmer.expanduser().resolve()
    args.external_loader = args.external_loader.expanduser().resolve()
    args.binary = args.binary.expanduser().resolve()
    missing = [path for path in (args.programmer, args.external_loader, args.binary)
               if not path.is_file()]
    if missing:
        print("error: programmer, loader, or binary does not exist", file=sys.stderr)
        return 2
    if not args.confirm_flash:
        print("refusing to flash without --confirm-flash", file=sys.stderr)
        return 2
    version = subprocess.run([str(args.programmer), "--version"], check=True,
                             capture_output=True, text=True, errors="replace").stdout.strip()
    try:
        subprocess.run(flash_command(args), check=True)
    except subprocess.CalledProcessError as error:
        return error.returncode or 1
    if args.private_manifest is not None:
        manifest = {
            "address": args.address,
            "binary_name": args.binary.name,
            "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            "external_loader_name": args.external_loader.name,
            "fleet_test_key_used": args.fleet_test_key_used,
            "programmer_version": version,
        }
        if args.fleet_test_key_used:
            manifest["key_notice"] = (
                "TEST/DEMO KEY - PUBLIC AND NOT SECRET - DO NOT USE IN PRODUCTION")
        output = args.private_manifest.expanduser().resolve()
        if output.exists():
            print("error: private manifest exists", file=sys.stderr)
            return 2
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n",
                          encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
