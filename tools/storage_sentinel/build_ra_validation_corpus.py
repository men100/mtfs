"""Build the RA pre-freeze validation corpus without reading held-out data."""
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


def _validation_path(artifact: Path, files: dict[str, dict]) -> tuple[Path, str]:
    manifest = files["training_manifest.json"]
    entries = manifest.get("validation_sessions")
    held_out = manifest.get("test_sessions")
    if not isinstance(entries, list) or len(entries) != 1 or \
            not isinstance(held_out, list) or not held_out:
        raise ValueError("exactly one validation session and a held-out split are required")
    entry = entries[0]
    expected_id = manifest.get("threshold_source")
    expected_hash = entry.get("dataset_sha256")
    if entry.get("session_id") != expected_id or not isinstance(expected_hash, str):
        raise ValueError("validation identity does not match threshold provenance")
    held_out_hashes = {item.get("dataset_sha256") for item in held_out}
    if expected_hash in held_out_hashes:
        raise ValueError("validation/held-out leakage")
    matches = [Path(name) for name, digest in
               files["artifact_index.json"].get("dataset_sha256", {}).items()
               if digest == expected_hash]
    if len(matches) != 1:
        raise ValueError("validation dataset cannot be resolved by SHA-256")
    path = matches[0].resolve()
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
        raise ValueError("validation dataset SHA-256 mismatch")
    return path, expected_hash


def build(artifact: Path, canonical_path: Path,
          optimized_path: Path) -> tuple[bytes, dict]:
    files = _artifact_files(artifact)
    validation_path, validation_hash = _validation_path(artifact, files)
    canonical = extract_tflite(canonical_path)
    cpu = deserialize_cpu_model(serialize_cpu_model(canonical))
    rows = golden_rows(validation_path, "validation",
                       files["normalization.json"], canonical_path,
                       canonical, cpu)
    if not rows or len(rows) > 0xffffffff:
        raise ValueError("validation corpus is empty or too large")
    threshold = int(_higher([row["score_q8"] for row in rows]))
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
    header[128:160] = bytes.fromhex(validation_hash)
    header[160:192] = hashlib.sha256(records).digest()
    corpus = bytes(header + records)
    report = {
        "format": "mtfs-ra-ethosu-validation-corpus-v1",
        "role": "validation-only-pre-acceptance-freeze",
        "held_out_read": False,
        "validation_session": rows[0]["session_id"],
        "validation_dataset_sha256": validation_hash,
        "canonical_tflite_sha256": canonical.canonical_sha256.hex(),
        "optimized_tflite_sha256": optimized_hash.hex(),
        "profile_id": int.from_bytes(canonical.canonical_sha256[:4], "little") or 1,
        "threshold_q8": threshold,
        "record_count": len(rows),
        "record_size": RECORD_SIZE,
        "corpus_sha256": hashlib.sha256(corpus).hexdigest(),
        "cpu_canonical_tflite_bit_exact": True,
    }
    return corpus, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--canonical-tflite", required=True, type=Path)
    parser.add_argument("--optimized-tflite", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    corpus, report = build(args.artifact.resolve(),
                           args.canonical_tflite.resolve(),
                           args.optimized_tflite.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_new(args.output.resolve(), corpus)
    write_new(args.output.with_suffix(args.output.suffix + ".json").resolve(),
              canonical_json_bytes(report))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
