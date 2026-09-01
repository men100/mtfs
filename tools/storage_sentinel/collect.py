from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

from dataset import manifest_path, sha256_file, write_dataset
from schema import HEADER, MAGIC, DatasetError, parse_lines, schema_hash
from version import TOOL_VERSION


def _serial_lines(port: str, baud: int, expected_rows: int | None) -> Iterable[str]:
    try:
        import serial  # type: ignore
    except ImportError as error:
        raise DatasetError("serial collection requires optional dependency pyserial") from error
    header = ",".join(HEADER)
    header_seen = False
    row_count = 0
    with serial.Serial(port, baudrate=baud, timeout=1.0) as stream:
        try:
            while True:
                raw = stream.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="strict")
                stripped = line.strip("\r\n")
                if stripped == header:
                    header_seen = True
                elif header_seen and stripped.startswith(MAGIC + ","):
                    row_count += 1
                yield line
                if expected_rows is not None and row_count >= expected_rows:
                    return
        except KeyboardInterrupt:
            return


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Collect/convert Sentinel Lab UART CSV")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--input", type=Path, help="saved UART log (default: stdin)")
    source.add_argument("--serial-port", help="serial port; requires pyserial")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--card-id", default="unspecified")
    parser.add_argument("--target-name", required=True)
    parser.add_argument("--transport-name", required=True)
    parser.add_argument("--firmware-commit", required=True)
    parser.add_argument("--command", required=True)
    parser.add_argument("--expected-rows", type=int)
    parser.add_argument("--partial", action="store_true")
    return parser


def run(args: argparse.Namespace) -> dict:
    if args.output.exists() or manifest_path(args.output).exists():
        raise DatasetError("output or manifest already exists; refusing overwrite")
    source_hash = None
    if args.serial_port:
        lines = _serial_lines(args.serial_port, args.baud, args.expected_rows)
        source = f"serial:{args.serial_port}@{args.baud}"
    elif args.input:
        source_hash = sha256_file(args.input)
        lines = args.input.open("r", encoding="utf-8", newline="")
        source = str(args.input)
    else:
        lines = sys.stdin
        source = "stdin"
    try:
        rows = parse_lines(lines)
    finally:
        if args.input:
            lines.close()
    if args.expected_rows is not None and len(rows) > args.expected_rows:
        raise DatasetError("dataset has more rows than expected")
    partial = bool(args.partial or
                   (args.expected_rows is not None and len(rows) < args.expected_rows))
    commands = {str(row["command"]) for row in rows}
    if commands != {args.command}:
        raise DatasetError(f"command mismatch: CSV has {sorted(commands)}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_dataset(args.output, rows)
    manifest = {
        "format": "mtfs-sentinel-dataset-manifest-v1",
        "tool_version": TOOL_VERSION,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "session_id": args.session_id,
        "card_id": args.card_id,
        "target_name": args.target_name,
        "transport_name": args.transport_name,
        "target_id": rows[0]["target"],
        "transport_id": rows[0]["transport"],
        "build_type": rows[0]["build_type"],
        "firmware_commit": args.firmware_commit,
        "command": args.command,
        "row_count": len(rows),
        "partial": partial,
        "source": source,
        "source_sha256": source_hash,
        "dataset_sha256": sha256_file(args.output),
        "feature_schema_sha256": schema_hash(),
    }
    with manifest_path(args.output).open("x", encoding="utf-8") as output:
        output.write(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main(argv: list[str] | None = None) -> int:
    try:
        manifest = run(build_parser().parse_args(argv))
        print(json.dumps(manifest, indent=2, sort_keys=True))
        return 0
    except (DatasetError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
