"""Train a baseline-relative Storage Sentinel model without held-out access."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

from baseline_v2 import q4_dataset
from dataset import load_dataset, sha256_file
from model import train_candidates
from schema import DatasetError, feature_schema, schema_canonical_hash
from version import TOOL_VERSION


def _write_json(path: Path, payload: object) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(payload, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")


def _load_policy(path: Path) -> tuple[dict, dict]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("format") != \
            "mtfs-sentinel-baseline-policy-freeze-candidate-v2" or \
            document.get("heldout_read") is not False:
        raise DatasetError("invalid or contaminated preprocessing candidate")
    policy = document.get("policy")
    if not isinstance(policy, dict) or policy.get("format") != \
            "mtfs-sentinel-baseline-relative-policy-v2":
        raise DatasetError("preprocessing candidate has no v2 policy")
    return document, policy


def _datasets(paths: list[Path], role: str, condition: str) -> list:
    result = [load_dataset(path.resolve()) for path in paths]
    if not result:
        raise DatasetError(f"no {role} {condition} datasets")
    for dataset in result:
        context = dataset.manifest.get("capture_context")
        if not isinstance(context, dict) or \
                context.get("contract") != "baseline-relative-v2" or \
                context.get("dataset_role") != role or \
                context.get("condition") != condition:
            raise DatasetError(f"{dataset.path}: dataset role/condition mismatch")
    return result


def _prepare(datasets: list, policy: dict) -> tuple[np.ndarray, list[dict], list[dict]]:
    inputs: list[list[int]] = []
    rows: list[dict] = []
    diagnostics: list[dict] = []
    for dataset in datasets:
        session_inputs, session_rows, session_diagnostics = q4_dataset(dataset, policy)
        inputs.extend(session_inputs)
        rows.extend(session_rows)
        diagnostics.extend(session_diagnostics)
    matrix = np.asarray(inputs, dtype=np.int8)
    if matrix.ndim != 2 or matrix.shape[1] != 24:
        raise DatasetError("invalid baseline-relative model matrix")
    return matrix, rows, diagnostics


def _entry(dataset) -> dict:
    context = dataset.manifest["capture_context"]
    return {
        "path": str(dataset.path.resolve()),
        "session_id": dataset.session_id,
        "card_id": dataset.card_id,
        "condition": context["condition"],
        "dataset_role": context["dataset_role"],
        "dataset_sha256": sha256_file(dataset.path),
        "source_sha256": dataset.manifest.get("source_sha256"),
        "usable_frames": len(dataset.rows),
    }


def run(args: argparse.Namespace) -> dict:
    if args.output_dir.exists():
        raise DatasetError("output directory already exists; refusing overwrite")
    candidate, policy = _load_policy(args.preprocessing)
    training = _datasets(args.training_normal, "training-candidate", "normal")
    validation = _datasets(args.validation_normal, "validation-candidate", "normal")
    validation_pseudo = _datasets(args.validation_pseudo,
                                  "validation-candidate", "pseudo")
    training_cards = {dataset.card_id for dataset in training}
    validation_cards = {dataset.card_id for dataset in validation + validation_pseudo}
    if not training_cards or len(validation_cards) != 1 or \
            training_cards & validation_cards:
        raise DatasetError("training and validation card split is invalid")
    train_q4, _, train_diag = _prepare(training, policy)
    validation_q4, _, validation_diag = _prepare(validation, policy)
    pseudo_q4, pseudo_rows, pseudo_diag = _prepare(validation_pseudo, policy)
    if any(item["out_of_distribution"] for item in train_diag + validation_diag):
        raise DatasetError("normal training/validation input saturated frozen scaling")
    train_values = train_q4.astype(np.float64) / 16.0
    validation_values = validation_q4.astype(np.float64) / 16.0
    model, candidate_report = train_candidates(train_values, validation_values,
                                                args.seed, args.epochs)
    validation_scores = model.scores(validation_values)
    threshold = float(np.quantile(validation_scores, 0.95, method="higher"))
    pseudo_scores = model.scores(pseudo_q4.astype(np.float64) / 16.0)
    stage_metrics = {}
    for stage in ("baseline", "light", "medium", "strong", "recovery"):
        indexes = [index for index, row in enumerate(pseudo_rows)
                   if str(row["stage"]) == stage]
        if not indexes:
            continue
        ood = int(sum(bool(pseudo_diag[index]["out_of_distribution"])
                      for index in indexes))
        anomaly = int(sum(bool(
            not pseudo_diag[index]["out_of_distribution"] and
            pseudo_scores[index] > threshold) for index in indexes))
        stage_metrics[stage] = {
            "samples": len(indexes), "ood": ood,
            "ai_anomaly": anomaly, "safe_detection": ood + anomaly,
            "safe_detection_rate": (ood + anomaly) / len(indexes),
            "score_median": float(np.median(pseudo_scores[indexes])),
            "score_p95": float(np.quantile(pseudo_scores[indexes], .95,
                                             method="higher")),
        }
    normal_false = int(np.count_nonzero(validation_scores > threshold))
    acceptance = {
        "format": "mtfs-sentinel-baseline-relative-validation-v2",
        "heldout_read": False,
        "normal": {
            "samples": int(validation_scores.size),
            "false_warnings": normal_false,
            "false_warning_rate": normal_false / int(validation_scores.size),
            "ood": 0,
        },
        "pseudo": stage_metrics,
        "candidate_limits_for_canonical_freeze": {
            "maximum_normal_false_warning_rate": 0.05,
            "maximum_normal_ood_rate": 0.0,
            "minimum_medium_safe_detection_rate": 0.5,
            "minimum_strong_safe_detection_rate": 0.7,
            "minimum_recovery_normal_rate": 0.95,
        },
    }
    negative_index = int(np.argmin(validation_scores))
    usable_positive = [index for index, row in enumerate(pseudo_rows)
                       if str(row["stage"]) in {"medium", "strong"} and
                       not pseudo_diag[index]["out_of_distribution"]]
    positive_index = max(usable_positive, key=lambda index: pseudo_scores[index]) \
        if usable_positive else None
    target = int(training[0].rows[0]["target"])
    transport = int(training[0].rows[0]["transport"])
    build_type = str(training[0].rows[0]["build_type"])
    identity_normalization = {
        "format": "mtfs-sentinel-normalization-v2",
        "preprocessing_contract_version": 2,
        "feature_schema_id": "mtfs-storage-sentinel-model-input",
        "feature_schema_version": 1,
        "target_id": target, "transport_id": transport,
        "build_type": build_type,
        "mean": [0.0] * 24, "std": [1.0] * 24,
        "mean_q16": [0] * 24,
        "inverse_std_q20": [1 << 20] * 24,
        "output_q": 16, "int8_scale": 16,
        "note": "identity after authenticated baseline-relative q4 preprocessing",
    }
    threshold_document = {
        "format": "mtfs-sentinel-threshold-v2-provisional-float",
        "selection_data": "normal-validation-only",
        "quantile": 0.95,
        "comparison": "score > anomaly_threshold",
        "anomaly_threshold": threshold,
        "canonical_integer_threshold_q8": None,
    }
    manifest = {
        "format": "mtfs-sentinel-training-manifest-v2",
        "tool_version": TOOL_VERSION,
        "seed": args.seed, "epochs": args.epochs,
        "target_id": target, "transport_id": transport,
        "build_type": build_type,
        "feature_schema_canonical_sha256": schema_canonical_hash(),
        "preprocessing_policy_sha256": policy["canonical_sha256"],
        "topology_policy": "common-fixed-24-12-4-12-24",
        "split_unit": "physical-card-and-session",
        "train_sessions": [_entry(dataset) for dataset in training],
        "validation_sessions": [_entry(dataset) for dataset in validation],
        "validation_pseudo_sessions": [_entry(dataset)
                                       for dataset in validation_pseudo],
        "heldout_sessions": [],
        "heldout_read": False,
        "threshold_source": [dataset.session_id for dataset in validation],
        "candidate_comparison": candidate_report,
    }
    test_vectors = {
        "format": "mtfs-sentinel-test-vectors-v2",
        "negative": {
            "origin": "normal-validation",
            "input_q4": validation_q4[negative_index].astype(int).tolist(),
            "reference_float_score": float(validation_scores[negative_index]),
        },
        "positive": None if positive_index is None else {
            "origin": "pseudo-validation",
            "stage": str(pseudo_rows[positive_index]["stage"]),
            "input_q4": pseudo_q4[positive_index].astype(int).tolist(),
            "reference_float_score": float(pseudo_scores[positive_index]),
        },
    }
    args.output_dir.mkdir(parents=True, exist_ok=False)
    _write_json(args.output_dir / "feature_schema_v1.json", feature_schema())
    _write_json(args.output_dir / "model.json", model.as_dict())
    _write_json(args.output_dir / "normalization.json", identity_normalization)
    _write_json(args.output_dir / "preprocessing.json", policy)
    _write_json(args.output_dir / "threshold.json", threshold_document)
    _write_json(args.output_dir / "training_manifest.json", manifest)
    _write_json(args.output_dir / "test_vectors.json", test_vectors)
    _write_json(args.output_dir / "validation_report.json", acceptance)
    _write_json(args.output_dir / "calibration_q4.json", {
        "format": "mtfs-sentinel-calibration-q4-v2",
        "input_q4": train_q4.astype(int).tolist(),
    })
    hashes = {path.name: sha256_file(path) for path in
              sorted(args.output_dir.iterdir()) if path.is_file()}
    _write_json(args.output_dir / "artifact_index.json", {
        "format": "mtfs-sentinel-artifact-index-v2",
        "tool_version": TOOL_VERSION,
        "target_id": target, "transport_id": transport,
        "build_type": build_type,
        "feature_schema_canonical_sha256": schema_canonical_hash(),
        "files_sha256": hashes,
        "heldout_read": False,
        "policy_selection_sha256": sha256_file(args.preprocessing),
    })
    return {
        "artifact": str(args.output_dir),
        "policy_sha256": policy["canonical_sha256"],
        "active_feature_mask": policy["active_feature_mask"],
        "warmup_windows": policy["warmup_windows"],
        "float_validation_threshold": threshold,
        "validation": acceptance,
        "heldout_read": False,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Train baseline-relative v2 without held-out access")
    result.add_argument("--preprocessing", type=Path, required=True)
    result.add_argument("--training-normal", type=Path, action="append",
                        required=True)
    result.add_argument("--validation-normal", type=Path, action="append",
                        required=True)
    result.add_argument("--validation-pseudo", type=Path, action="append",
                        required=True)
    result.add_argument("--output-dir", type=Path, required=True)
    result.add_argument("--seed", type=int, default=4303)
    result.add_argument("--epochs", type=int, default=200)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        print(json.dumps(run(parser().parse_args(argv)), indent=2,
                         sort_keys=True, allow_nan=False))
        return 0
    except (DatasetError, OSError, ValueError, KeyError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
