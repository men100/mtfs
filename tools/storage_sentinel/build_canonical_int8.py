"""Build and independently validate canonical Sentinel int8 deployment artifacts."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import struct
import tempfile
from pathlib import Path

import numpy as np

from bundle import (_artifact_files, _find_datasets, _float32_normalize, _higher,
                    canonical_float32, deployment_evaluation, fixed_normalize,
                    quantize_model)
from canonical_int8 import (QUANTIZATION_CONTRACT_VERSION, deserialize_cpu_model,
    extract_tflite, infer_q4, requantize_int8_to_q4, requantize_q4_to_int8,
    serialize_cpu_model, tflite_reference)
from dataset import load_dataset
from model import DenseAutoencoder
from schema import canonical_json_bytes, deterministic_rule, encode_row
from version import TOOL_VERSION


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def write_new(path: Path, data: bytes) -> None:
    if path.exists():
        raise ValueError(f"output exists: {path}")
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.",
                                                   suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data); output.flush(); os.fsync(output.fileno())
        os.rename(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def score_q8(input_q4: np.ndarray, output_q4: np.ndarray) -> int:
    difference = input_q4.astype(np.int16) - output_q4.astype(np.int16)
    return (sum(int(value) * int(value) for value in difference) + 12) // 24


def golden_rows(path: Path, role: str, normalization: dict, model_path: Path, model,
                cpu_model) -> list[dict]:
    dataset = load_dataset(path)
    prepared = []
    canonical_inputs = []
    for sequence, row in enumerate(dataset.rows):
        if deterministic_rule(row):
            continue
        raw = np.asarray(encode_row(row), dtype=np.int64)
        normalized = _float32_normalize(raw[np.newaxis, :], normalization)[0]
        input_q4 = fixed_normalize(raw, normalization)
        canonical_input = np.asarray([requantize_q4_to_int8(
            int(value), model.input_scale, model.input_zero_point)
            for value in input_q4], dtype=np.int8)
        prepared.append((sequence, row, normalized, input_q4, canonical_input))
        canonical_inputs.append(canonical_input)
    reference_outputs = tflite_reference(path=model_path,
        canonical_inputs=np.asarray(canonical_inputs, dtype=np.int8))
    result = []
    for prepared_row, reference_output in zip(prepared, reference_outputs):
        sequence, row, normalized, input_q4, canonical_input = prepared_row
        cpu_raw = cpu_model.infer_raw(canonical_input)
        if not np.array_equal(cpu_raw, reference_output):
            raise ValueError(f"CPU/TFLite raw int8 mismatch at {dataset.session_id}:{sequence}")
        output_q4 = np.asarray([requantize_int8_to_q4(
            int(value), model.output_scale, model.output_zero_point)
            for value in reference_output], dtype=np.int8)
        result.append({
            "session_id": dataset.session_id, "sequence": sequence, "role": role,
            "stage": str(row.get("stage", "")),
            "normalized_feature": normalized.astype(float).tolist(),
            "common_q4_input": input_q4.astype(int).tolist(),
            "tflite_int8_input": canonical_input.astype(int).tolist(),
            "tflite_raw_int8_output": reference_output.astype(int).tolist(),
            "common_q4_reconstruction": output_q4.astype(int).tolist(),
            "score_q8": score_q8(input_q4, output_q4),
        })
    return result


def roc_auc(labels: list[bool], scores: list[int]) -> float | None:
    positive = sum(labels); negative = len(labels) - positive
    if positive == 0 or negative == 0:
        return None
    order = sorted(range(len(scores)), key=lambda index: (scores[index], index))
    rank_sum = 0.0; index = 0
    while index < len(order):
        end = index + 1
        while end < len(order) and scores[order[end]] == scores[order[index]]:
            end += 1
        average_rank = (index + 1 + end) / 2.0
        rank_sum += average_rank * sum(labels[order[item]] for item in range(index, end))
        index = end
    return (rank_sum - positive * (positive + 1) / 2) / (positive * negative)


def average_precision(labels: list[bool], scores: list[int]) -> float | None:
    positive = sum(labels)
    if positive == 0:
        return None
    order = sorted(range(len(scores)), key=lambda index: (-scores[index], index))
    true_positive = 0; total = 0; accumulated = 0.0
    for index in order:
        total += 1
        if labels[index]:
            true_positive += 1
            accumulated += true_positive / total
    return accumulated / positive


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--canonical-tflite", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--additional-dataset", action="append", default=[], type=Path)
    args = parser.parse_args()
    artifact = args.artifact.resolve()
    model_path = args.canonical_tflite.resolve(); output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit("output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)
    files = _artifact_files(artifact)
    normalization = files["normalization.json"]
    model = extract_tflite(model_path)
    binary = serialize_cpu_model(model)
    cpu_model = deserialize_cpu_model(binary)
    validation_path, test_path, normal_paths, eval_paths = _find_datasets(artifact, files)
    golden = []
    golden.extend(golden_rows(validation_path, "validation", normalization,
                              model_path, model, cpu_model))
    golden.extend(golden_rows(test_path, "held-out-test", normalization,
                              model_path, model, cpu_model))
    validation_scores = [row["score_q8"] for row in golden if row["role"] == "validation"]
    threshold = int(_higher(validation_scores))
    for row in golden:
        row["decision"] = row["score_q8"] > threshold

    evaluation_rows = list(golden)
    evaluated_paths = []
    for path in list(eval_paths) + [item.resolve() for item in args.additional_dataset]:
        evaluated_paths.append({"path": str(path), "sha256":
            hashlib.sha256(path.read_bytes()).hexdigest()})
        role = "evaluation"
        evaluation_rows.extend(golden_rows(path, role, normalization, model_path,
                                           model, cpu_model))
    for row in evaluation_rows:
        row["decision"] = row["score_q8"] > threshold
    labels = [row["role"] == "evaluation" and
              row["stage"] in {"light", "medium", "strong"}
              for row in evaluation_rows]
    scores = [row["score_q8"] for row in evaluation_rows]
    heldout = [row for row in golden if row["role"] == "held-out-test"]
    delay = [row for row in evaluation_rows if row["role"] == "evaluation" and
             row["stage"] in {"light", "medium", "strong"}]
    strong = [row for row in delay if row["stage"] == "strong"]
    session_metrics = {}
    for session_id in sorted({row["session_id"] for row in evaluation_rows}):
        selected = [row for row in evaluation_rows if row["session_id"] == session_id]
        positives = [row for row in selected if row["stage"] in
                     {"light", "medium", "strong"}]
        normals = [row for row in selected if row["stage"] in {"natural", "baseline"}]
        session_metrics[session_id] = {
            "rows": len(selected),
            "delay_detection": {"count": sum(row["decision"] for row in positives),
                                "total": len(positives)},
            "normal_false_positive": {"count": sum(row["decision"] for row in normals),
                                      "total": len(normals)},
        }
    reference_model = DenseAutoencoder.from_dict(files["model.json"])
    weights32, biases32, _ = canonical_float32(reference_model)
    legacy_evaluation, old_threshold, _ = deployment_evaluation(files, artifact,
        reference_model, weights32, biases32, quantize_model(weights32, biases32),
        normalization)
    old_quality = json.loads((artifact / "evaluation_report.json").read_text(
        encoding="utf-8"))["methods"]["dense_autoencoder"]
    st_delay = session_metrics.get("st-delay-ramp-20260901-01", {}).get(
        "delay_detection", {"count": 0, "total": 0})
    st_strong = [row for row in evaluation_rows
                 if row["session_id"] == "st-delay-ramp-20260901-01" and
                 row["stage"] == "strong"]
    report = {
        "format": "mtfs-sentinel-canonical-int8-offline-evaluation-v1",
        "source_float_model_sha256": hashlib.sha256(
            (artifact / "model.json").read_bytes()).hexdigest(),
        "canonical_full_int8_tflite_sha256": model.canonical_sha256.hex(),
        "cpu_runtime_binary_sha256": sha256_bytes(binary),
        "cpu_tflite_bit_exact": {
            "raw_int8_output": True, "common_q4_reconstruction": True,
            "score_q8": True, "decision": True,
            "validation_test_vectors": len(golden),
        },
        "threshold": {"old_q8": old_threshold, "new_q8": threshold,
            "source": load_dataset(validation_path).session_id,
            "method": "p95-higher-validation-only",
            "reason": "canonical deployed model changed from independent Q7/Q4 to full-int8 TFLite"},
        "held_out_test": {"false_positive": sum(row["decision"] for row in heldout),
                          "total_normal": len(heldout)},
        "pseudo_slow": {"detected": sum(row["decision"] for row in delay),
                        "total": len(delay),
                        "strong_detected": sum(row["decision"] for row in strong),
                        "strong_total": len(strong)},
        "roc_auc": roc_auc(labels, scores), "average_precision": average_precision(labels, scores),
        "session_metrics": session_metrics,
        "phase_4_3b_baseline_comparison": {
            "old_float_held_out_false_warning_rate":
                old_quality["held_out_normal_false_warning_rate"],
            "new_int8_held_out_false_warning_rate":
                (sum(row["decision"] for row in heldout) / len(heldout)),
            "old_float_st_delay_detection_rate": old_quality["delay_detection_rate"],
            "new_int8_st_delay_detection_rate":
                (st_delay["count"] / st_delay["total"] if st_delay["total"] else None),
            "old_float_st_strong_detection_rate": old_quality["strong_delay_detection_rate"],
            "new_int8_st_strong_detection_rate":
                (sum(row["decision"] for row in st_strong) / len(st_strong)
                 if st_strong else None),
            "legacy_independent_cpu_int8": legacy_evaluation["cpu_int8"],
        },
        "evaluated_datasets": evaluated_paths,
        "leakage_contract": {"training": "model training",
            "training_calibration_subset": "PTQ calibration",
            "validation": "threshold and numeric acceptance",
            "held_out_test": "final performance only"},
    }
    layers = []
    for index, layer in enumerate(model.layers):
        layers.append({"index": index, "input_shape": [int(layer.weights.shape[1])],
            "output_shape": [int(layer.weights.shape[0])], "activation":
            "relu" if layer.activation else "none", "input_scale_float32_hex":
            np.float32(layer.input_scale).tobytes().hex(),
            "input_zero_point": layer.input_zero_point,
            "output_scale_float32_hex": np.float32(layer.output_scale).tobytes().hex(),
            "output_zero_point": layer.output_zero_point,
            "weight_scales_float32_hex": [np.float32(value).tobytes().hex()
                for value in layer.weight_scales],
            "weight_zero_points": layer.weight_zero_points.astype(int).tolist(),
            "quantized_multipliers": layer.multipliers.astype(int).tolist(),
            "shifts": layer.shifts.astype(int).tolist()})
    cpu_manifest = {
        "format": "mtfs-sentinel-cpu-tflite-int8-runtime-v2",
        "generator_version": TOOL_VERSION,
        "quantization_contract_version": QUANTIZATION_CONTRACT_VERSION,
        "source_float_model_sha256": report["source_float_model_sha256"],
        "canonical_full_int8_tflite_sha256": model.canonical_sha256.hex(),
        "runtime_binary_sha256": sha256_bytes(binary),
        "input_output": {"input_shape": [24], "output_shape": [24], "dtype": "int8"},
        "layers": layers,
        "rounding": "TFLite gemmlowp double-rounding",
        "saturation": "fused activation then signed-int8 clamp",
        "score_contract": "common-Q4 MSE rounded by (sum+12)/24",
        "threshold_provenance": report["threshold"],
        "calibration_dataset_identity": files["training_manifest.json"].get("train_sessions", []),
    }
    golden_document = {"format": "mtfs-sentinel-canonical-int8-golden-v1",
        "canonical_full_int8_tflite_sha256": model.canonical_sha256.hex(),
        "threshold_q8": threshold, "vectors": golden}
    write_new(output / "sentinel_cpu_tflite_int8.bin", binary)
    write_new(output / "sentinel_cpu_tflite_int8_manifest.json",
              canonical_json_bytes(cpu_manifest))
    write_new(output / "canonical_golden_vectors.json",
              canonical_json_bytes(golden_document))
    write_new(output / "canonical_offline_evaluation.json",
              canonical_json_bytes(report))
    corpus = bytearray(b"MTFSCV1\0" + struct.pack("<II", len(golden), 112))
    for row in golden:
        corpus.extend(bytes((value & 0xff) for value in row["common_q4_input"]))
        corpus.extend(bytes((value & 0xff) for value in row["tflite_int8_input"]))
        corpus.extend(bytes((value & 0xff) for value in row["tflite_raw_int8_output"]))
        corpus.extend(bytes((value & 0xff) for value in row["common_q4_reconstruction"]))
        corpus.extend(struct.pack("<QB7x", row["score_q8"], row["decision"]))
    write_new(output / "canonical_corpus.bin", bytes(corpus))
    validation_rows = [row for row in golden if row["role"] == "validation"]
    for name, key in (("canonical_validation_input_int8.npy", "tflite_int8_input"),
                      ("canonical_validation_output_int8.npy", "tflite_raw_int8_output")):
        stream = io.BytesIO()
        np.save(stream, np.asarray([row[key] for row in validation_rows], dtype=np.int8),
                allow_pickle=False)
        write_new(output / name, stream.getvalue())
    index = {"format": "mtfs-sentinel-canonical-int8-artifact-index-v1",
        "files_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(output.iterdir())}}
    write_new(output / "artifact_index.json", canonical_json_bytes(index))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
