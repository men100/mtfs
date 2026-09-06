"""Build an identity-bound RA validation or post-freeze held-out corpus."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from build_canonical_int8 import golden_rows, write_new
from bundle import _artifact_files, _higher
from canonical_int8 import (deserialize_cpu_model, extract_tflite,
                            rational_scale, serialize_cpu_model)
from schema import canonical_json_bytes


MAGIC = b"MTFSRAV1"
VERSION = 1
HEADER_SIZE = 192
RECORD_SIZE = 128
FEATURE_COUNT = 24


def _dataset_path(artifact: Path, files: dict[str, dict],
                  split: str) -> tuple[Path, str, str]:
    manifest = files["training_manifest.json"]
    validation = manifest.get("validation_sessions")
    held_out = manifest.get("test_sessions")
    training = manifest.get("train_sessions")
    if not isinstance(validation, list) or len(validation) != 1 or \
            not isinstance(held_out, list) or len(held_out) != 1 or \
            not isinstance(training, list) or not training:
        raise ValueError("exactly one validation session and a held-out split are required")
    entry = validation[0] if split == "validation" else held_out[0]
    expected_id = (manifest.get("threshold_source") if split == "validation"
                   else entry.get("session_id"))
    expected_hash = entry.get("dataset_sha256")
    if entry.get("session_id") != expected_id or not isinstance(expected_hash, str):
        raise ValueError(f"{split} identity does not match provenance")
    role_hashes = {
        "validation": {item.get("dataset_sha256") for item in validation},
        "held-out": {item.get("dataset_sha256") for item in held_out},
        "training": {item.get("dataset_sha256") for item in training},
    }
    if any(expected_hash in hashes for role, hashes in role_hashes.items()
           if role != split):
        raise ValueError("training/validation/held-out leakage")
    matches = [Path(name) for name, digest in
               files["artifact_index.json"].get("dataset_sha256", {}).items()
               if digest == expected_hash]
    if len(matches) != 1:
        raise ValueError("validation dataset cannot be resolved by SHA-256")
    path = matches[0].resolve()
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
        raise ValueError(f"{split} dataset SHA-256 mismatch")
    return path, expected_hash, str(entry["session_id"])


def build(artifact: Path, canonical_path: Path,
          optimized_path: Path, split: str = "validation",
          frozen_threshold_q8: int | None = None) -> tuple[bytes, dict]:
    if split not in {"validation", "held-out"}:
        raise ValueError("split must be validation or held-out")
    files = _artifact_files(artifact)
    dataset_path, dataset_hash, session_id = _dataset_path(artifact, files,
                                                           split)
    canonical = extract_tflite(canonical_path)
    cpu = deserialize_cpu_model(serialize_cpu_model(canonical))
    row_role = "validation" if split == "validation" else "held-out-test"
    rows = golden_rows(dataset_path, row_role,
                       files["normalization.json"], canonical_path,
                       canonical, cpu)
    if not rows or len(rows) > 0xffffffff:
        raise ValueError(f"{split} corpus is empty or too large")
    if split == "validation":
        threshold = int(_higher([row["score_q8"] for row in rows]))
        if frozen_threshold_q8 is not None and threshold != frozen_threshold_q8:
            raise ValueError("validation threshold does not match frozen value")
    else:
        if frozen_threshold_q8 is None or frozen_threshold_q8 < 0:
            raise ValueError("held-out build requires a frozen threshold")
        threshold = frozen_threshold_q8
    records = bytearray()
    for row in rows:
        record = bytearray(RECORD_SIZE)
        struct.pack_into("<II", record, 0, int(row["sequence"]), 0)
        record[8:32] = bytes((int(value) & 0xff)
                             for value in row["common_q4_input"])
        record[32:56] = bytes((int(value) & 0xff)
                              for value in row["tflite_int8_input"])
        record[56:80] = bytes((int(value) & 0xff)
                              for value in row["tflite_raw_int8_output"])
        record[80:104] = bytes((int(value) & 0xff)
                               for value in row["common_q4_reconstruction"])
        struct.pack_into("<QB", record, 104, int(row["score_q8"]),
                         int(row["score_q8"] > threshold))
        records.extend(record)
    input_num, input_shift = rational_scale(canonical.input_scale)
    output_num, output_shift = rational_scale(canonical.output_scale)
    optimized = optimized_path.read_bytes()
    optimized_hash = hashlib.sha256(optimized).digest()
    header = bytearray(HEADER_SIZE)
    header[:8] = MAGIC
    struct.pack_into("<HHHHIIIIIIbb2xII4xQ", header, 8,
                     VERSION, HEADER_SIZE, RECORD_SIZE, FEATURE_COUNT,
                     len(rows), int(files["training_manifest.json"]["target_id"]),
                     int(files["training_manifest.json"]["transport_id"]),
                     int.from_bytes(canonical.canonical_sha256[:4], "little") or 1,
                     input_num, input_shift, canonical.input_zero_point,
                     canonical.output_zero_point, output_num, output_shift,
                     threshold)
    header[64:96] = canonical.canonical_sha256
    header[96:128] = optimized_hash
    header[128:160] = bytes.fromhex(dataset_hash)
    header[160:192] = hashlib.sha256(records).digest()
    corpus = bytes(header + records)
    report = {
        "format": ("mtfs-ra-ethosu-validation-corpus-v1" if split == "validation"
                   else "mtfs-ra-ethosu-held-out-corpus-v1"),
        "role": ("validation-only-pre-acceptance-freeze" if split == "validation"
                 else "held-out-post-acceptance-freeze"),
        "held_out_read": split == "held-out",
        "canonical_tflite_sha256": canonical.canonical_sha256.hex(),
        "optimized_tflite_sha256": optimized_hash.hex(),
        "profile_id": int.from_bytes(canonical.canonical_sha256[:4], "little") or 1,
        "threshold_q8": threshold,
        "record_count": len(rows),
        "record_size": RECORD_SIZE,
        "corpus_sha256": hashlib.sha256(corpus).hexdigest(),
        "cpu_canonical_tflite_bit_exact": True,
    }
    if split == "validation":
        report["validation_session"] = session_id
        report["validation_dataset_sha256"] = dataset_hash
    else:
        report["held_out_session"] = session_id
        report["held_out_dataset_sha256"] = dataset_hash
        report["acceptance_contract_version"] = 2
        report["frozen_limits"] = {
            "decision_disagreements": 0,
            "maximum_q4_error": 0,
            "maximum_raw_int8_error": 0,
            "maximum_score_q8_error": 0,
            "repeatability_failures": 0,
            "score_interval_violations": 0,
        }
    return corpus, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--canonical-tflite", required=True, type=Path)
    parser.add_argument("--optimized-tflite", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--split", choices=("validation", "held-out"),
                        default="validation")
    parser.add_argument("--frozen-threshold-q8", type=int)
    args = parser.parse_args()
    corpus, report = build(args.artifact.resolve(),
                           args.canonical_tflite.resolve(),
                           args.optimized_tflite.resolve(), args.split,
                           args.frozen_threshold_q8)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_new(args.output.resolve(), corpus)
    write_new(args.output.with_suffix(args.output.suffix + ".json").resolve(),
              canonical_json_bytes(report))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
