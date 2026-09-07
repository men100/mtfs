"""Evaluate frozen STM32 Neural-ART numerical backend contract v3 captures."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

from canonical_int8 import extract_tflite, requantize_int8_to_q4
from dataset import sha256_file
from schema import DatasetError, canonical_json_bytes
from st_numerical_v3 import CORE_FORMAT, _json, _load_npy, _load_npz


def _load_array(path: Path, dtype, shape: tuple[int, ...]) -> np.ndarray:
    values = np.asarray(np.load(path, allow_pickle=False))
    if values.dtype != dtype or values.shape != shape:
        raise DatasetError(f"{path}: expected {np.dtype(dtype)} {shape}")
    return values


def _capture(path: Path) -> dict[str, np.ndarray]:
    with np.load(path, allow_pickle=False) as archive:
        required = {"tflite_inputs_int8", "common_q4_inputs",
                    "tflite_outputs_int8", "npu_outputs_int8"}
        if not required.issubset(archive.files):
            raise DatasetError(f"{path}: incomplete AiRunner capture")
        return {name: np.asarray(archive[name]).copy() for name in archive.files}


def _verify_file(path: Path, entry: object) -> None:
    if not isinstance(entry, dict) or entry.get("bytes") != path.stat().st_size or \
            entry.get("sha256") != sha256_file(path):
        raise DatasetError(f"{path}: differs from frozen artifact")


def _verify_frozen_artifacts(core: dict, corpus_dir: Path,
                             expected_dir: Path) -> None:
    input_index = corpus_dir / "input_index.json"
    expected_index = expected_dir / "expected_index.json"
    if core.get("input_index_file_sha256") != sha256_file(input_index) or \
            core.get("expected_index_file_sha256") != sha256_file(expected_index):
        raise DatasetError("frozen input/expected index identity mismatch")
    for name, entry in core.get("input_files", {}).items():
        _verify_file(corpus_dir / name, entry)
    for name, entry in core.get("expected_files", {}).items():
        _verify_file(expected_dir / name, entry)


def _score(inputs: np.ndarray, outputs: np.ndarray) -> np.ndarray:
    difference = inputs.astype(np.int64) - outputs.astype(np.int64)
    return ((np.sum(difference * difference, axis=1, dtype=np.int64) + 12) // 24
            ).astype(np.uint64)


def _q4(model, values: np.ndarray) -> np.ndarray:
    return np.asarray([[requantize_int8_to_q4(int(item), model.output_scale,
                                              model.output_zero_point)
                        for item in row] for row in values], dtype=np.int8)


def evaluate_corpus(name: str, core: dict, model, corpus_dir: Path,
                    expected_dir: Path, official_output: Path,
                    capture1_path: Path, capture2_path: Path) -> dict:
    inputs = _load_npz(corpus_dir / f"{name}_inputs_int8.npz")
    input_q4 = _load_npy(corpus_dir / f"{name}_input_q4.npy", inputs.shape[0])
    expected_raw = _load_array(expected_dir / f"{name}_expected_raw_int8.npy",
                               np.int8, inputs.shape)
    expected_q4 = _load_array(expected_dir / f"{name}_expected_q4.npy",
                              np.int8, inputs.shape)
    expected_scores = _load_array(expected_dir / f"{name}_expected_score_q8.npy",
                                  np.uint64, (inputs.shape[0],))
    expected_decisions = _load_array(
        expected_dir / f"{name}_expected_decision.npy", np.bool_,
        (inputs.shape[0],))
    intervals = _load_array(expected_dir / f"{name}_score_interval_q8.npy",
                            np.uint64, (inputs.shape[0], 2))
    official = _load_array(official_output, np.int8, inputs.shape)
    first = _capture(capture1_path)
    for key, expected in (("tflite_inputs_int8", inputs),
                          ("common_q4_inputs", input_q4),
                          ("tflite_outputs_int8", expected_raw)):
        if not np.array_equal(first[key], expected):
            raise DatasetError(f"{name}: AiRunner {key} differs from frozen artifact")
    npu = first["npu_outputs_int8"]
    if npu.dtype != np.int8 or npu.shape != inputs.shape:
        raise DatasetError(f"{name}: malformed AiRunner NPU output")
    official_mismatches = int(np.count_nonzero(official != npu))
    second = _capture(capture2_path)
    for key, expected in (("tflite_inputs_int8", inputs),
                          ("common_q4_inputs", input_q4),
                          ("tflite_outputs_int8", expected_raw)):
        if not np.array_equal(second[key], expected):
            raise DatasetError(f"{name}: repeat {key} differs from frozen artifact")
    second_npu = second["npu_outputs_int8"]
    if second_npu.dtype != np.int8 or second_npu.shape != inputs.shape:
        raise DatasetError(f"{name}: malformed repeat AiRunner NPU output")
    repeatability_failures = int(np.count_nonzero(
        np.any(second_npu != npu, axis=1)))

    raw_abs = np.abs(npu.astype(np.int16) - expected_raw.astype(np.int16))
    npu_q4 = _q4(model, npu)
    q4_abs = np.abs(npu_q4.astype(np.int16) - expected_q4.astype(np.int16))
    scores = _score(input_q4, npu_q4)
    interval_violations = (scores < intervals[:, 0]) | (scores > intervals[:, 1])
    threshold = int(core["decision_policy"]["threshold_q8"])
    direct_decisions = scores > threshold
    ambiguous = (intervals[:, 0] <= threshold) & (intervals[:, 1] > threshold)
    final_decisions = np.where(ambiguous, expected_decisions, direct_decisions)
    decision_disagreements = int(np.count_nonzero(final_decisions != expected_decisions))
    direct_disagreements = int(np.count_nonzero(direct_decisions != expected_decisions))
    output_maxima = np.max(raw_abs, axis=0).astype(int).tolist()
    limits = core["fixed_limits"]
    status = "PASS" if (
        int(raw_abs.max(initial=0)) <= limits["maximum_vendor_raw_int8_error"] and
        int(q4_abs.max(initial=0)) <= limits["maximum_common_q4_output_error"] and
        not np.any(interval_violations) and decision_disagreements == 0 and
        repeatability_failures == 0 and official_mismatches == 0) else "STOP"
    return {
        "status": status,
        "vectors": int(inputs.shape[0]),
        "raw_elements": int(raw_abs.size),
        "raw_error_elements": {
            "0": int(np.count_nonzero(raw_abs == 0)),
            "1": int(np.count_nonzero(raw_abs == 1)),
            "2": int(np.count_nonzero(raw_abs == 2)),
            "3_or_more": int(np.count_nonzero(raw_abs >= 3)),
        },
        "maximum_vendor_raw_int8_error": int(raw_abs.max(initial=0)),
        "maximum_raw_error_by_output_index": output_maxima,
        "maximum_common_q4_output_error": int(q4_abs.max(initial=0)),
        "maximum_score_error_q8": int(np.max(np.abs(
            scores.astype(np.int64) - expected_scores.astype(np.int64)), initial=0)),
        "score_interval_violations": int(np.count_nonzero(interval_violations)),
        "ambiguous_cpu_arbitrations": int(np.count_nonzero(ambiguous)),
        "direct_npu_decision_disagreements": direct_disagreements,
        "decision_disagreements": decision_disagreements,
        "repeatability_failures": repeatability_failures,
        "invoke_failures": 0,
        "target_airunner_mismatches": official_mismatches,
        "official_target_output_sha256": sha256_file(official_output),
        "airunner_capture_run1_sha256": sha256_file(capture1_path),
        "airunner_capture_run2_sha256": sha256_file(capture2_path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract-core", type=Path, required=True)
    parser.add_argument("--freeze-record", type=Path, required=True)
    parser.add_argument("--canonical-tflite", type=Path, required=True)
    parser.add_argument("--corpus-dir", type=Path, required=True)
    parser.add_argument("--expected-dir", type=Path, required=True)
    parser.add_argument("--characterization-target", type=Path, required=True)
    parser.add_argument("--characterization-capture-run1", type=Path, required=True)
    parser.add_argument("--characterization-capture-run2", type=Path, required=True)
    parser.add_argument("--operational-target", type=Path, required=True)
    parser.add_argument("--operational-capture-run1", type=Path, required=True)
    parser.add_argument("--operational-capture-run2", type=Path, required=True)
    parser.add_argument("--stress-target", type=Path, required=True)
    parser.add_argument("--stress-capture-run1", type=Path, required=True)
    parser.add_argument("--stress-capture-run2", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        core = _json(args.contract_core.resolve())
        freeze = _json(args.freeze_record.resolve())
        if core.get("format") != CORE_FORMAT or core.get("status") != "FROZEN" or \
                freeze.get("status") != "FROZEN-BEFORE-NPU-ACCEPTANCE" or \
                freeze.get("contract_core_canonical_sha256") != \
                    hashlib.sha256(canonical_json_bytes(core)).hexdigest() or \
                freeze.get("npu_outputs_observed") is not False:
            raise DatasetError("frozen numerical v3 identity mismatch")
        model = extract_tflite(args.canonical_tflite.resolve())
        if model.canonical_sha256.hex() != core["specification"][
                "canonical_full_int8_tflite_sha256"]:
            raise DatasetError("canonical model differs from frozen contract")
        _verify_frozen_artifacts(core, args.corpus_dir.resolve(),
                                 args.expected_dir.resolve())
        characterization = evaluate_corpus("characterization", core, model,
            args.corpus_dir.resolve(), args.expected_dir.resolve(),
            args.characterization_target.resolve(),
            args.characterization_capture_run1.resolve(),
            args.characterization_capture_run2.resolve())
        operational = evaluate_corpus("operational", core, model,
            args.corpus_dir.resolve(), args.expected_dir.resolve(),
            args.operational_target.resolve(),
            args.operational_capture_run1.resolve(),
            args.operational_capture_run2.resolve())
        stress = evaluate_corpus("stress", core, model,
            args.corpus_dir.resolve(), args.expected_dir.resolve(),
            args.stress_target.resolve(), args.stress_capture_run1.resolve(),
            args.stress_capture_run2.resolve())
        accepted = all(item["status"] == "PASS" for item in
                       (characterization, operational, stress))
        held_out = {
            "contract_core_sha256": freeze["contract_core_canonical_sha256"],
            "vectors": operational["vectors"] + stress["vectors"],
            "violations": 0 if accepted else 1,
            "operational": operational,
            "stress": stress,
        }
        report = {
            "format": "mtfs-sentinel-neural-art-acceptance-v3",
            "status": "PASS" if accepted else "STOP",
            "canonical_full_int8_tflite_sha256":
                core["specification"]["canonical_full_int8_tflite_sha256"],
            "npu_runtime_binary_sha256":
                core["specification"]["npu_runtime_binary_sha256"],
            "conversion_manifest_sha256":
                core["specification"]["conversion_manifest_sha256"],
            "contract_core": core,
            "contract_core_sha256": freeze["contract_core_canonical_sha256"],
            "characterization": characterization,
            "held_out_test": held_out,
            "post_npu_change_policy": "immutable; any excess keeps v3 STOP",
        }
        if args.output.exists():
            raise DatasetError(f"refusing overwrite: {args.output}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(canonical_json_bytes(report) + b"\n")
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if accepted else 1
    except (DatasetError, OSError, ValueError, KeyError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
