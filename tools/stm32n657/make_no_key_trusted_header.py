#!/usr/bin/env python3
"""Create a no-key trusted-header development image for STM32N657."""
from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


def command(args: argparse.Namespace) -> list[str]:
    result = [str(args.signing_tool)]
    if args.overwrite:
        result.append("-s")
    result.extend(["-bin", str(args.input), "-nk", "-of", args.option_flags])
    if args.load_address is not None:
        result.extend(["-la", args.load_address])
    result.extend(["-t", args.header_type, "-hv", args.header_version, "-align",
                   "-o", str(args.output), "-dump", str(args.output)])
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--signing-tool", required=True, type=Path,
                        help="vendor STM32_SigningTool_CLI executable")
    result.add_argument("--input", required=True, type=Path,
                        help="caller-supplied FSBL or application binary")
    result.add_argument("--output", required=True, type=Path)
    result.add_argument("--option-flags", default="0x80000000",
                        help="trusted-header option flags passed to -of")
    result.add_argument("--load-address",
                        help="optional explicit image load address passed to -la")
    result.add_argument("--header-type", default="fsbl")
    result.add_argument("--header-version", default="2.3")
    result.add_argument("--overwrite", action="store_true")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    args.signing_tool = args.signing_tool.expanduser().resolve()
    args.input = args.input.expanduser().resolve()
    args.output = args.output.expanduser().resolve()
    if not args.signing_tool.is_file() or not args.input.is_file():
        print("error: signing tool or input binary does not exist", file=sys.stderr)
        return 2
    if args.output.exists() and not args.overwrite:
        print("error: output exists; use --overwrite explicitly", file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    print("Creating no-key trusted-header development image; this is not cryptographic signing.")
    try:
        subprocess.run(command(args), check=True)
    except subprocess.CalledProcessError as error:
        return error.returncode or 1
    if not args.output.is_file():
        print("error: vendor tool did not create output", file=sys.stderr)
        return 2
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(f"output_sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
