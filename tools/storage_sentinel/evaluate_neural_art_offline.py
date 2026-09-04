"""Fix Neural-ART numeric acceptance limits before target execution."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from bundle import atomic_write
from canonical_int8 import extract_tflite, requantize_int8_to_q4
from schema import canonical_json_bytes


def score(input_q4: np.ndarray, output_q4: np.ndarray) -> int:
    difference = input_q4.astype(np.int16) - output_q4.astype(np.int16)
    return (sum(int(value) * int(value) for value in difference) + 12) // 24


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--canonical-tflite", required=True, type=Path)
    parser.add_argument("--compiler-onnx", required=True, type=Path)
    parser.add_argument("--npu-manifest", required=True, type=Path)
    parser.add_argument("--golden-vectors", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        import onnxruntime as ort
    except ImportError as error:
        raise SystemExit("onnxruntime is required for compiler frontend evaluation") from error
    model = extract_tflite(args.canonical_tflite.resolve())
    document = json.loads(args.golden_vectors.read_text(encoding="utf-8"))
    rows = document["vectors"]
    manifest = json.loads(args.npu_manifest.read_text(encoding="utf-8"))
    if document["canonical_full_int8_tflite_sha256"] != model.canonical_sha256.hex() or \
            manifest["canonical_model_sha256"] != model.canonical_sha256.hex():
        raise SystemExit("canonical model identity mismatch")
    session = ort.InferenceSession(str(args.compiler_onnx.resolve()),
                                   providers=["CPUExecutionProvider"])
    if len(session.get_inputs()) != 1 or len(session.get_outputs()) != 1:
        raise SystemExit("compiler ONNX I/O contract mismatch")
    input_name = session.get_inputs()[0].name
    observations = []
    for row in rows:
        canonical_input = np.asarray(row["tflite_int8_input"], dtype=np.int8)
        actual_raw = session.run(None, {input_name: canonical_input.reshape(1, 24)})[0][0]
        expected_raw = np.asarray(row["tflite_raw_int8_output"], dtype=np.int8)
        actual_q4 = np.asarray([requantize_int8_to_q4(int(value), model.output_scale,
            model.output_zero_point) for value in actual_raw], dtype=np.int8)
        expected_q4 = np.asarray(row["common_q4_reconstruction"], dtype=np.int8)
        input_q4 = np.asarray(row["common_q4_input"], dtype=np.int8)
        actual_score = score(input_q4, actual_q4)
        observations.append({"role": row["role"], "session_id": row["session_id"],
            "sequence": row["sequence"],
            "raw_int8_max_error": int(np.max(np.abs(actual_raw.astype(np.int16) -
                                                     expected_raw.astype(np.int16)))),
            "common_q4_max_error": int(np.max(np.abs(actual_q4.astype(np.int16) -
                                                      expected_q4.astype(np.int16)))),
            "reference_score_q8": row["score_q8"], "compiler_score_q8": actual_score,
            "score_error_q8": abs(actual_score - row["score_q8"]),
            "reference_decision": row["decision"],
            "compiler_decision": actual_score > document["threshold_q8"],
            "threshold_margin_q8": abs(row["score_q8"] - document["threshold_q8"]),
        })
    validation = [row for row in observations if row["role"] == "validation"]
    heldout = [row for row in observations if row["role"] == "held-out-test"]
    limits = {"raw_vendor_int8_max_absolute_error":
                  max(row["raw_int8_max_error"] for row in validation),
              "common_q4_max_absolute_error":
                  max(row["common_q4_max_error"] for row in validation),
              "score_q8_max_absolute_error":
                  max(row["score_error_q8"] for row in validation),
              "decision_must_match": True}
    heldout_violations = [row for row in heldout if
        row["raw_int8_max_error"] > limits["raw_vendor_int8_max_absolute_error"] or
        row["common_q4_max_error"] > limits["common_q4_max_absolute_error"] or
        row["score_error_q8"] > limits["score_q8_max_absolute_error"] or
        row["reference_decision"] != row["compiler_decision"]]
    near = [row for row in validation if row["threshold_margin_q8"] <= 1]
    report = {
        "format": "mtfs-sentinel-neural-art-acceptance-v1",
        "canonical_full_int8_tflite_sha256": model.canonical_sha256.hex(),
        "compiler_onnx_sha256": hashlib.sha256(args.compiler_onnx.read_bytes()).hexdigest(),
        "npu_runtime_binary_sha256": manifest["runtime_binary_sha256"],
        "conversion_manifest_sha256": manifest["conversion_manifest_sha256"],
        "method": "ST compiler frontend quantized ONNX executed with ONNX Runtime; hardware must not exceed these pre-fixed limits",
        "host_neural_art_validation_support": "unsupported-by-ST-Edge-AI-Core-for-NPU",
        "validation_vectors": len(validation), "held_out_test_vectors": len(heldout),
        "fixed_limits": limits,
        "validation_observed": {
            "raw_error_vectors": sum(row["raw_int8_max_error"] != 0 for row in validation),
            "q4_error_vectors": sum(row["common_q4_max_error"] != 0 for row in validation),
            "score_error_vectors": sum(row["score_error_q8"] != 0 for row in validation),
            "decision_disagreements": sum(row["reference_decision"] !=
                row["compiler_decision"] for row in validation),
        },
        "threshold_neighborhood": {"margin_q8": 1, "samples": len(near),
            "decision_disagreements": sum(row["reference_decision"] !=
                row["compiler_decision"] for row in near),
            "minimum_observed_margin_q8": min(row["threshold_margin_q8"]
                                               for row in validation)},
        "held_out_test": {"violations": len(heldout_violations),
            "decision_disagreements": sum(row["reference_decision"] !=
                row["compiler_decision"] for row in heldout),
            "maximum_raw_int8_error": max(row["raw_int8_max_error"] for row in heldout),
            "maximum_common_q4_error": max(row["common_q4_max_error"] for row in heldout),
            "maximum_score_q8_error": max(row["score_error_q8"] for row in heldout)},
        "status": "PASS" if not heldout_violations else "STOP",
        "post_target_rule": "limits are immutable after target execution; any excess or decision disagreement is a STOP",
    }
    atomic_write(args.output, canonical_json_bytes(report))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
