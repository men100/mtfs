from __future__ import annotations

import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

MAGIC = "mtfs-sentinel-dataset-v1"
UINT64_MAX = (1 << 64) - 1
REQUIRED_VALIDITY = 0xFF
REJECTED_FLAGS = 0x1F
RAW_HISTOGRAM_BUCKETS_PER_OPERATION = 22
RAW_HISTOGRAM_OPERATION_COUNT = 3
RAW_HISTOGRAM_BUCKETS = (RAW_HISTOGRAM_BUCKETS_PER_OPERATION *
                         RAW_HISTOGRAM_OPERATION_COUNT)
HISTOGRAM_FEATURE_GROUPS = ((0, 3), (4, 7), (8, 11), (12, 14), (15, 21))

HEADER = [
    MAGIC, "feature_schema_version", "size", "target", "transport",
    "timestamp_us", "interval_us", "media_generation", "validity", "flags",
    "samples", "label", "marker", "build_type", "command",
    "scenario_origin", "stage", "severity", "injection_kind",
    "injection_operation_mask", "injection_rate_permille",
    "requested_delay_us", "actual_injection_count", "random_seed", "sequence",
    "read_calls", "read_sectors", "read_ok", "read_fail", "read_timing",
    "read_invalid", "read_total_us", "read_avg_us", "write_calls",
    "write_sectors", "write_ok", "write_fail", "write_timing", "write_invalid",
    "write_total_us", "write_avg_us", "sync_calls", "sync_sectors", "sync_ok",
    "sync_fail", "sync_timing", "sync_invalid", "sync_total_us", "sync_avg_us",
    "io_error", "not_ready", "no_media", "timeout", "insert", "remove",
    "media_error", "transport_validity", "transport_flags",
    "transport_reset_epoch", "transport_errors", "transfer_timeouts",
    "ready_timeouts", "aborts", "clock_errors", "histogram_r_w_s",
]

TEXT_COLUMNS = {"label", "build_type", "command", "scenario_origin", "stage", "injection_kind"}
HARD_FAULT_COLUMNS = (
    "io_error", "not_ready", "no_media", "timeout", "transport_errors",
    "transfer_timeouts", "ready_timeouts", "aborts", "clock_errors",
)


class DatasetError(ValueError):
    pass


def _schema_path() -> Path:
    source_tree = Path(__file__).with_name("feature_schema_v1.json")
    if source_tree.is_file():
        return source_tree
    installed = (Path(sys.prefix) / "share" / "mtfs-storage-sentinel" /
                 "feature_schema_v1.json")
    if installed.is_file():
        return installed
    raise DatasetError("feature_schema_v1.json not found")


def feature_schema() -> dict:
    return json.loads(_schema_path().read_text(encoding="utf-8"))


def canonical_json_bytes(payload: object) -> bytes:
    return json.dumps(payload, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False).encode("utf-8")


def canonical_json_sha256(payload: object) -> str:
    import hashlib
    return hashlib.sha256(canonical_json_bytes(payload)).hexdigest()


def schema_canonical_hash() -> str:
    return canonical_json_sha256(feature_schema())


def _uint(text: str, column: str) -> int:
    if not text or not text.isascii() or not text.isdigit():
        raise DatasetError(f"{column}: invalid unsigned integer {text!r}")
    value = int(text, 10)
    if value > UINT64_MAX:
        raise DatasetError(f"{column}: integer overflow")
    return value


def parse_row(fields: list[str], line_number: int) -> dict:
    if len(fields) != len(HEADER):
        raise DatasetError(f"line {line_number}: expected {len(HEADER)} columns, got {len(fields)}")
    if fields == HEADER:
        raise DatasetError(f"line {line_number}: duplicate header")
    if fields[0] != MAGIC:
        raise DatasetError(f"line {line_number}: schema magic mismatch")
    row: dict[str, object] = {}
    for name, value in zip(HEADER[1:], fields[1:]):
        if name in TEXT_COLUMNS:
            if not value or any(c in value for c in "\r\n,"):
                raise DatasetError(f"line {line_number}: invalid {name}")
            row[name] = value
        elif name == "histogram_r_w_s":
            parts = value.split(":")
            if len(parts) != RAW_HISTOGRAM_BUCKETS:
                raise DatasetError(
                    f"line {line_number}: histogram needs "
                    f"{RAW_HISTOGRAM_BUCKETS} buckets")
            row[name] = [_uint(part, name) for part in parts]
        else:
            row[name] = _uint(value, name)
    if row["feature_schema_version"] != 1:
        raise DatasetError(f"line {line_number}: unsupported feature schema")
    if row["scenario_origin"] not in {"natural", "injected"}:
        raise DatasetError(f"line {line_number}: invalid scenario_origin")
    if row["injection_rate_permille"] > 1000:
        raise DatasetError(f"line {line_number}: injection rate out of range")
    return row


def parse_lines(lines: Iterable[str]) -> list[dict]:
    rows: list[dict] = []
    header_seen = False
    for line_number, raw in enumerate(lines, 1):
        line = raw.strip("\r\n")
        if not line or line.startswith("#") or line.startswith(">"):
            continue
        fields = next(csv.reader([line], strict=True))
        if fields == HEADER:
            if header_seen:
                raise DatasetError(f"line {line_number}: duplicate header")
            header_seen = True
            continue
        if not header_seen:
            continue
        rows.append(parse_row(fields, line_number))
    if not header_seen:
        raise DatasetError("CSV header not found")
    if not rows:
        raise DatasetError("dataset contains no frames")
    validate_sequence(rows)
    validate_identity(rows)
    return rows


def validate_sequence(rows: list[dict]) -> None:
    previous = None
    previous_generation = None
    for row in rows:
        sequence = int(row["sequence"])
        if previous is not None and sequence != previous + 1:
            raise DatasetError(f"sequence discontinuity: {previous} -> {sequence}")
        previous = sequence
        generation = int(row["media_generation"])
        row["_media_generation_changed"] = (
            previous_generation is not None and generation != previous_generation)
        previous_generation = generation


def validate_identity(rows: list[dict]) -> None:
    targets = {int(row["target"]) for row in rows}
    transports = {int(row["transport"]) for row in rows}
    build_types = {str(row["build_type"]) for row in rows}
    if len(targets) != 1:
        raise DatasetError("mixed target dataset")
    if len(transports) != 1:
        raise DatasetError("mixed transport dataset")
    if len(build_types) != 1:
        raise DatasetError("mixed build_type dataset")


def _permille(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        raise DatasetError("division by zero in feature encoder")
    return min(1000, (numerator * 1000 + denominator // 2) // denominator)


def deterministic_rule(row: dict) -> list[str]:
    reasons: list[str] = []
    if (int(row["validity"]) & REQUIRED_VALIDITY) != REQUIRED_VALIDITY:
        reasons.append("invalid-feature")
    flags = int(row["flags"])
    if flags & 0x01:
        reasons.append("no-activity")
    if flags & 0x02:
        reasons.append("insufficient-data")
    if flags & 0x04:
        reasons.append("discontinuity")
    if flags & 0x08:
        reasons.append("counter-saturated")
    if flags & 0x10:
        reasons.append("timing-unavailable")
    if bool(row.get("_media_generation_changed", False)):
        reasons.append("media-generation-change")
    for column in HARD_FAULT_COLUMNS:
        if int(row[column]) != 0:
            reasons.append(column.replace("_", "-"))
    return reasons


def encode_row(row: dict) -> list[int]:
    reasons = deterministic_rule(row)
    if reasons:
        raise DatasetError("frame must not enter AI path: " + ",".join(reasons))
    histogram = row["histogram_r_w_s"]
    vector: list[int] = []
    timings: list[int] = []
    for op_index, op in enumerate(("read", "write", "sync")):
        timing = int(row[f"{op}_timing"])
        invalid = int(row[f"{op}_invalid"])
        start = op_index * RAW_HISTOGRAM_BUCKETS_PER_OPERATION
        end = start + RAW_HISTOGRAM_BUCKETS_PER_OPERATION
        buckets = [int(v) for v in histogram[start:end]]
        if timing < 1 or sum(buckets) != timing:
            raise DatasetError(f"{op}: insufficient or inconsistent histogram")
        avg = min(1_000_000, int(row[f"{op}_avg_us"]))
        vector.extend([avg, _permille(invalid, timing + invalid)])
        vector.extend(_permille(sum(buckets[first:last + 1]), timing)
                      for first, last in HISTOGRAM_FEATURE_GROUPS)
        timings.append(timing)
    total_timing = sum(timings)
    vector.extend(_permille(value, total_timing) for value in timings)
    if len(vector) != 24:
        raise AssertionError("feature schema dimension mismatch")
    return vector


def normalize(values, mean, std):
    import numpy as np
    array = np.asarray(values, dtype=np.float64)
    result = (array - mean) / std
    if not np.all(np.isfinite(result)):
        raise DatasetError("NaN/Inf during normalization")
    return np.clip(result, -8.0, 8.0)


def fixed_point_normalization(mean, std) -> dict:
    means = [int(round(float(value) * 65536.0)) for value in mean]
    inverses = [int(round((1.0 / float(value)) * (1 << 20))) for value in std]
    if any(value < -(1 << 63) or value >= (1 << 63) for value in means + inverses):
        raise DatasetError("fixed-point normalization overflow")
    return {"mean_q16": means, "inverse_std_q20": inverses,
            "output_q": 16, "int8_scale": 16}
