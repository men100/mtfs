from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

from dataset import manifest_path, sha256_file, write_dataset
from schema import (HEADER, MAGIC, DatasetError, parse_lines,
                    schema_canonical_hash)
from version import TOOL_VERSION


_WORKLOAD_STATUS = re.compile(r"^# workload-perf .* mtfs=(-?\d+)\s*$")


def _audit_workload_status(path: Path) -> tuple[int, int]:
    observed = 0
    failed = 0
    with path.open("r", encoding="utf-8", newline="") as source:
        for line in source:
            match = _WORKLOAD_STATUS.match(line.rstrip("\r\n"))
            if match is None:
                continue
            observed += 1
            if int(match.group(1)) != 0:
                failed += 1
    return observed, failed


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
    parser.add_argument("--card-manufacturer")
    parser.add_argument("--card-capacity",
                        help="operator-reported card capacity, for example 16GB")
    parser.add_argument("--target-name", required=True)
    parser.add_argument("--transport-name", required=True)
    parser.add_argument("--firmware-commit", required=True)
    parser.add_argument("--command", required=True)
    parser.add_argument("--expected-rows", type=int)
    parser.add_argument("--partial", action="store_true")
    parser.add_argument("--capture-contract", choices=("raw-v1", "baseline-relative-v2"),
                        default="raw-v1")
    parser.add_argument("--fat-type")
    parser.add_argument("--allocation-unit", type=int,
                        help="filesystem allocation unit in bytes")
    parser.add_argument("--power-cycle-id")
    parser.add_argument("--mount-session-id")
    parser.add_argument("--condition", choices=("normal", "pseudo"))
    parser.add_argument("--dataset-role", choices=("training-candidate",
                        "validation-candidate", "heldout-locked"))
    parser.add_argument("--baseline-window-start", type=int, default=1)
    parser.add_argument("--baseline-window-count", type=int)
    parser.add_argument("--capture-profile",
                        choices=("generic", "st-release-idma-v2"),
                        default="generic")
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
    capture_contract = getattr(args, "capture_contract", "raw-v1")
    capture_profile = getattr(args, "capture_profile", "generic")
    if capture_profile == "st-release-idma-v2" and \
            capture_contract != "baseline-relative-v2":
        raise DatasetError(
            "st-release-idma-v2 requires baseline-relative-v2 capture contract")
    capture_context = None
    workload_status_observed = 0
    workload_status_failed = 0
    if capture_profile == "st-release-idma-v2" and args.input:
        workload_status_observed, workload_status_failed = \
            _audit_workload_status(args.input)
        if workload_status_failed:
            raise DatasetError(
                "st-release-idma-v2 source contains nonzero workload status: "
                f"{workload_status_failed}/{workload_status_observed}")
    if capture_contract == "baseline-relative-v2":
        required = {
            "fat_type": getattr(args, "fat_type", None),
            "allocation_unit": getattr(args, "allocation_unit", None),
            "power_cycle_id": getattr(args, "power_cycle_id", None),
            "mount_session_id": getattr(args, "mount_session_id", None),
            "condition": getattr(args, "condition", None),
            "dataset_role": getattr(args, "dataset_role", None),
            "baseline_window_count": getattr(args, "baseline_window_count", None),
        }
        missing = sorted(name for name, value in required.items()
                         if value is None or value == "")
        if args.card_id == "unspecified":
            missing.append("card_id")
        if capture_profile == "st-release-idma-v2":
            if not getattr(args, "card_manufacturer", None):
                missing.append("card_manufacturer")
            if not getattr(args, "card_capacity", None):
                missing.append("card_capacity")
        if missing:
            raise DatasetError("baseline-relative-v2 capture metadata missing: " +
                               ",".join(missing))
        allocation_unit = int(required["allocation_unit"])
        baseline_start = int(getattr(args, "baseline_window_start", 1))
        baseline_count = int(required["baseline_window_count"])
        if allocation_unit <= 0 or allocation_unit & (allocation_unit - 1):
            raise DatasetError("allocation_unit must be a positive power of two")
        if baseline_start <= 0 or baseline_count <= 0 or \
                baseline_start + baseline_count - 1 > len(rows):
            raise DatasetError("baseline window range is outside captured rows")
        if capture_profile == "st-release-idma-v2":
            if args.target_name != "STM32N6570-DK" or \
                    args.transport_name != "SDMMC-IDMA" or \
                    int(rows[0]["target"]) != 0x53544E36 or \
                    int(rows[0]["transport"]) != 0x49444D41 or \
                    str(rows[0]["build_type"]) != "Release":
                raise DatasetError(
                    "st-release-idma-v2 requires STM32N6570-DK Release SDMMC-IDMA rows")
        capture_context = {
            "contract": "baseline-relative-v2",
            "operator_card_id": args.card_id,
            "capture_session_id": args.session_id,
            "fat_type": str(required["fat_type"]),
            "allocation_unit_bytes": allocation_unit,
            "power_cycle_id": str(required["power_cycle_id"]),
            "mount_session_id": str(required["mount_session_id"]),
            "condition": str(required["condition"]),
            "dataset_role": str(required["dataset_role"]),
            "baseline_windows": {
                "first_sequence": baseline_start,
                "count": baseline_count,
            },
        }
        if capture_profile == "st-release-idma-v2":
            capture_context.update({
                "capture_profile": capture_profile,
                "card_manufacturer": args.card_manufacturer,
                "card_capacity": args.card_capacity,
                "workload_status_audit": {
                    "observed": workload_status_observed,
                    "nonzero": workload_status_failed,
                },
            })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_dataset(args.output, rows)
    manifest = {
        "format": ("mtfs-sentinel-dataset-manifest-v2"
                   if capture_context is not None else
                   "mtfs-sentinel-dataset-manifest-v1"),
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
        "feature_schema_canonical_sha256": schema_canonical_hash(),
    }
    if capture_context is not None:
        manifest["capture_context"] = capture_context
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
