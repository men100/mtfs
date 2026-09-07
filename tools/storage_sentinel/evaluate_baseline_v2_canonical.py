"""Freeze canonical-int8 validation metrics for baseline-relative v2."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np

from baseline_v2 import q4_dataset
from canonical_int8 import (deserialize_cpu_model, extract_tflite,
                            requantize_int8_to_q4, requantize_q4_to_int8,
                            serialize_cpu_model, tflite_reference)
from dataset import load_dataset, sha256_file
from schema import DatasetError, canonical_json_bytes


def _write_new(path: Path, payload: bytes) -> None:
    if path.exists():
        raise DatasetError(f"output exists: {path}")
    path.write_bytes(payload)


def _higher(values: list[int], probability: float = .95) -> int:
    if not values:
        raise DatasetError("empty validation score set")
    ordered = sorted(values)
    return int(ordered[math.ceil(probability * (len(ordered) - 1))])


def _score(input_q4: np.ndarray, output_q4: np.ndarray) -> int:
    difference = input_q4.astype(np.int16) - output_q4.astype(np.int16)
    return (sum(int(value) * int(value) for value in difference) + 12) // 24


def _artifact_files(artifact: Path) -> tuple[dict, dict, dict]:
    index = json.loads((artifact / "artifact_index.json").read_text(
        encoding="utf-8"))
    if index.get("format") != "mtfs-sentinel-artifact-index-v2" or \
            index.get("heldout_read") is not False:
        raise DatasetError("invalid or held-out-contaminated v2 artifact")
    for name, expected in index.get("files_sha256", {}).items():
        if sha256_file(artifact / name) != expected:
            raise DatasetError(f"artifact hash mismatch: {name}")
    manifest = json.loads((artifact / "training_manifest.json").read_text(
        encoding="utf-8"))
    policy = json.loads((artifact / "preprocessing.json").read_text(
        encoding="utf-8"))
    if manifest.get("heldout_read") is not False or \
            manifest.get("heldout_sessions") != []:
        raise DatasetError("held-out data entered canonical validation")
    return index, manifest, policy


def _load_entries(entries: list[dict], expected_role: str,
                  expected_condition: str) -> list:
    datasets = []
    for entry in entries:
        path = Path(entry["path"])
        dataset = load_dataset(path)
        if sha256_file(path) != entry["dataset_sha256"]:
            raise DatasetError(f"dataset hash mismatch: {path}")
        context = dataset.manifest.get("capture_context", {})
        if context.get("dataset_role") != expected_role or \
                context.get("condition") != expected_condition:
            raise DatasetError(f"dataset split mismatch: {path}")
        datasets.append(dataset)
    return datasets


def _prepare(datasets: list, policy: dict) -> tuple[np.ndarray, list[dict], list[dict]]:
    q4, rows, diagnostics = [], [], []
    for dataset in datasets:
        values, source_rows, source_diagnostics = q4_dataset(dataset, policy)
        q4.extend(values)
        rows.extend(source_rows)
        diagnostics.extend(source_diagnostics)
    return np.asarray(q4, dtype=np.int8), rows, diagnostics


def _infer(model_path: Path, input_q4: np.ndarray) -> tuple[list[dict], bytes]:
    model = extract_tflite(model_path)
    cpu_binary = serialize_cpu_model(model)
    cpu = deserialize_cpu_model(cpu_binary)
    canonical_inputs = np.asarray([[requantize_q4_to_int8(
        int(value), model.input_scale, model.input_zero_point) for value in row]
        for row in input_q4], dtype=np.int8)
    reference = tflite_reference(model_path, canonical_inputs)
    results = []
    for q4, canonical_input, expected_raw in zip(input_q4, canonical_inputs,
                                                  reference):
        actual_raw = cpu.infer_raw(canonical_input)
        if not np.array_equal(actual_raw, expected_raw):
            raise DatasetError("canonical CPU/TFLite raw int8 mismatch")
        output_q4 = np.asarray([requantize_int8_to_q4(
            int(value), model.output_scale, model.output_zero_point)
            for value in expected_raw], dtype=np.int8)
        results.append({
            "input_q4": q4.astype(int).tolist(),
            "tflite_input_int8": canonical_input.astype(int).tolist(),
            "raw_output_int8": expected_raw.astype(int).tolist(),
            "output_q4": output_q4.astype(int).tolist(),
            "score_q8": _score(q4, output_q4),
        })
    return results, cpu_binary


def run(args: argparse.Namespace) -> dict:
    artifact = args.artifact.resolve()
    model_path = args.canonical_tflite.resolve()
    if args.output_dir.exists():
        raise DatasetError("output directory already exists; refusing overwrite")
    index, manifest, policy = _artifact_files(artifact)
    canonical_manifest_path = model_path.with_suffix(
        model_path.suffix + ".manifest.json")
    canonical_manifest = json.loads(canonical_manifest_path.read_text(
        encoding="utf-8"))
    if canonical_manifest.get("heldout_read") is not False or \
            canonical_manifest.get("preprocessing_policy_sha256") != \
            policy["canonical_sha256"] or \
            canonical_manifest.get("canonical_full_int8_tflite_sha256") != \
            sha256_file(model_path):
        raise DatasetError("canonical model manifest mismatch")
    normal = _load_entries(manifest["validation_sessions"],
                           "validation-candidate", "normal")
    pseudo = _load_entries(manifest["validation_pseudo_sessions"],
                           "validation-candidate", "pseudo")
    normal_q4, normal_rows, normal_diag = _prepare(normal, policy)
    pseudo_q4, pseudo_rows, pseudo_diag = _prepare(pseudo, policy)
    if any(item["out_of_distribution"] for item in normal_diag):
        raise DatasetError("normal validation contains OOD input")
    combined = np.concatenate((normal_q4, pseudo_q4), axis=0)
    inferred, cpu_binary = _infer(model_path, combined)
    normal_inferred = inferred[:len(normal_q4)]
    pseudo_inferred = inferred[len(normal_q4):]
    normal_scores = [row["score_q8"] for row in normal_inferred]
    threshold_document = json.loads((artifact / "threshold.json").read_text(
        encoding="utf-8"))
    if threshold_document.get("selection_data") != "normal-validation-only":
        raise DatasetError("threshold source is not validation-only")
    threshold_quantile = float(threshold_document.get("quantile", 0.95))
    if not 0.0 < threshold_quantile <= 1.0:
        raise DatasetError("invalid threshold quantile")
    threshold = _higher(normal_scores, threshold_quantile)
    false_warnings = sum(score > threshold for score in normal_scores)
    stage_metrics = {}
    for stage in ("baseline", "light", "medium", "strong", "recovery"):
        indexes = [index for index, row in enumerate(pseudo_rows)
                   if str(row["stage"]) == stage]
        if not indexes:
            continue
        ood = sum(bool(pseudo_diag[index]["out_of_distribution"])
                  for index in indexes)
        anomalies = sum(not pseudo_diag[index]["out_of_distribution"] and
                        pseudo_inferred[index]["score_q8"] > threshold
                        for index in indexes)
        scores = [pseudo_inferred[index]["score_q8"] for index in indexes]
        stage_metrics[stage] = {
            "samples": len(indexes), "ood": int(ood),
            "ai_anomaly": int(anomalies),
            "safe_detection": int(ood + anomalies),
            "safe_detection_rate": float((ood + anomalies) / len(indexes)),
            "score_median_q8": float(np.median(scores)),
            "score_p95_q8": _higher(scores),
        }
    limits = json.loads((artifact / "validation_report.json").read_text(
        encoding="utf-8"))["candidate_limits_for_canonical_freeze"]
    recovery = stage_metrics["recovery"]
    recovery_normal_rate = 1.0 - recovery["safe_detection_rate"]
    checks = {
        "normal_false_warning": false_warnings / len(normal_scores) <=
            float(limits["maximum_normal_false_warning_rate"]),
        "normal_ood": True,
        "medium_safe_detection": stage_metrics["medium"]["safe_detection_rate"] >=
            float(limits["minimum_medium_safe_detection_rate"]),
        "strong_safe_detection": stage_metrics["strong"]["safe_detection_rate"] >=
            float(limits["minimum_strong_safe_detection_rate"]),
        "recovery_normal": recovery_normal_rate >=
            float(limits["minimum_recovery_normal_rate"]),
        "strong_score_not_below_medium":
            stage_metrics["strong"]["score_median_q8"] >=
            stage_metrics["medium"]["score_median_q8"],
    }
    status = "PASS" if all(checks.values()) else "STOP"
    validation = {
        "format": "mtfs-sentinel-baseline-relative-canonical-validation-v2",
        "status": status, "heldout_read": False,
        "canonical_full_int8_tflite_sha256": sha256_file(model_path),
        "preprocessing_policy_sha256": policy["canonical_sha256"],
        "threshold_q8": threshold,
        "threshold_selection":
            f"normal-validation-p{threshold_quantile * 100:g}-higher",
        "normal": {"samples": len(normal_scores),
                   "false_warnings": false_warnings,
                   "false_warning_rate": false_warnings / len(normal_scores),
                   "ood": 0,
                   "score_histogram": {str(value): normal_scores.count(value)
                                       for value in sorted(set(normal_scores))}},
        "pseudo": stage_metrics,
        "recovery_normal_rate": recovery_normal_rate,
        "acceptance_limits": limits,
        "checks": checks,
        "cpu_tflite_raw_int8_bit_exact": True,
        "evaluated_vectors": len(inferred),
    }
    args.output_dir.mkdir(parents=True, exist_ok=False)
    _write_new(args.output_dir / "sentinel_cpu_tflite_int8.bin", cpu_binary)
    _write_new(args.output_dir / "canonical_validation.json",
               canonical_json_bytes(validation) + b"\n")
    golden = {
        "format": "mtfs-sentinel-baseline-relative-golden-v2",
        "heldout_read": False, "threshold_q8": threshold,
        "normal": [{**row, "session_role": "normal-validation"}
                   for row in normal_inferred],
        "pseudo": [{**result, "session_role": "pseudo-validation",
                    "stage": str(source["stage"]),
                    "out_of_distribution":
                        bool(diagnostic["out_of_distribution"])}
                   for result, source, diagnostic in
                   zip(pseudo_inferred, pseudo_rows, pseudo_diag)],
    }
    _write_new(args.output_dir / "canonical_golden_vectors.json",
               canonical_json_bytes(golden) + b"\n")
    file_hashes = {path.name: sha256_file(path)
                   for path in sorted(args.output_dir.iterdir())}
    freeze = {
        "format": "mtfs-sentinel-baseline-relative-freeze-core-v2",
        "status": status,
        "heldout_read": False,
        "preprocessing_policy_sha256": policy["canonical_sha256"],
        "training_artifact_index_sha256": sha256_file(
            artifact / "artifact_index.json"),
        "canonical_tflite_sha256": sha256_file(model_path),
        "canonical_manifest_sha256": sha256_file(canonical_manifest_path),
        "threshold_q8": threshold,
        "validation_sha256": file_hashes["canonical_validation.json"],
        "cpu_runtime_sha256": file_hashes["sentinel_cpu_tflite_int8.bin"],
        "next_gate": ("Neural-ART artifact identity and acceptance limits"
                      if int(index["target_id"]) == 0x53544e36 else
                      "Vela artifact identity and acceptance limits"),
    }
    _write_new(args.output_dir / "freeze-core.json",
               canonical_json_bytes(freeze) + b"\n")
    index = {
        "format": "mtfs-sentinel-baseline-relative-canonical-index-v2",
        "heldout_read": False,
        "files_sha256": {path.name: sha256_file(path)
                         for path in sorted(args.output_dir.iterdir())},
    }
    _write_new(args.output_dir / "artifact_index.json",
               canonical_json_bytes(index) + b"\n")
    return validation


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Validate and freeze baseline-relative canonical int8")
    result.add_argument("--artifact", type=Path, required=True)
    result.add_argument("--canonical-tflite", type=Path, required=True)
    result.add_argument("--output-dir", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        result = run(parser().parse_args(argv))
        print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
        return 0 if result["status"] == "PASS" else 1
    except (DatasetError, OSError, ValueError, KeyError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
