from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

import numpy as np

from dataset import Dataset, assert_same_profile, load_dataset, sha256_file, usable_vectors
from evaluate import evaluate_datasets
from model import Baselines, train_candidates
from schema import DatasetError, feature_schema, fixed_point_normalization, normalize, schema_hash
from version import TOOL_VERSION


def _write_json(path: Path, payload: dict) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(payload, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")


def _split_sessions(datasets: list[Dataset], seed: int) -> tuple[list[Dataset], Dataset, Dataset]:
    if len(datasets) < 3:
        raise DatasetError("at least three independent normal sessions are required")
    ordered = sorted(datasets, key=lambda dataset: dataset.session_id)
    random.Random(seed).shuffle(ordered)
    return ordered[:-2], ordered[-2], ordered[-1]


def _matrix(datasets: list[Dataset], minimum_frames: int) -> tuple[np.ndarray, list[dict]]:
    vectors: list[list[int]] = []
    details: list[dict] = []
    for dataset in datasets:
        if dataset.manifest.get("partial"):
            raise DatasetError(f"{dataset.path}: partial dataset cannot train a model")
        if any(row["scenario_origin"] != "natural" or row["command"] != "record"
               for row in dataset.rows):
            raise DatasetError(f"{dataset.path}: injected/labeled data is forbidden in training")
        usable, _ = usable_vectors(dataset)
        if len(usable) < minimum_frames:
            raise DatasetError(f"{dataset.path}: {len(usable)} usable frames; need {minimum_frames}")
        vectors.extend(usable)
        details.append({"session_id": dataset.session_id, "card_id": dataset.card_id,
                        "usable_frames": len(usable), "dataset_sha256": sha256_file(dataset.path)})
    array = np.asarray(vectors, dtype=np.float64)
    if array.ndim != 2 or array.shape[1] != 24 or not np.all(np.isfinite(array)):
        raise DatasetError("invalid encoded training matrix")
    return array, details


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Train a target-specific Sentinel anomaly model")
    parser.add_argument("--dataset", type=Path, action="append", required=True,
                        help="normal record dataset; repeat for each session")
    parser.add_argument("--evaluation-dataset", type=Path, action="append", default=[])
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=4303)
    parser.add_argument("--epochs", type=int, default=200)
    parser.add_argument("--minimum-session-frames", type=int, default=200)
    return parser


def run(args: argparse.Namespace) -> dict:
    if args.seed <= 0 or args.epochs <= 0 or args.minimum_session_frames <= 0:
        raise DatasetError("seed, epochs, and minimum frame count must be positive")
    if args.output_dir.exists():
        raise DatasetError("output directory already exists; refusing overwrite")
    datasets = [load_dataset(path) for path in args.dataset]
    target, transport = assert_same_profile(datasets)
    build_type = datasets[0].build_type
    train_sessions, validation_session, test_session = _split_sessions(datasets, args.seed)
    train_raw, train_details = _matrix(train_sessions, args.minimum_session_frames)
    validation_raw, validation_details = _matrix([validation_session], args.minimum_session_frames)
    test_raw, test_details = _matrix([test_session], args.minimum_session_frames)
    mean = np.mean(train_raw, axis=0)
    std = np.std(train_raw, axis=0)
    std = np.where(std < 1.0, 1.0, std)
    train_values = normalize(train_raw, mean, std)
    validation_values = normalize(validation_raw, mean, std)
    test_values = normalize(test_raw, mean, std)
    model, candidate_report = train_candidates(train_values, validation_values,
                                                args.seed, args.epochs)
    validation_scores = model.scores(validation_values)
    try:
        threshold = float(np.quantile(validation_scores, 0.95, method="higher"))
    except TypeError:
        threshold = float(np.quantile(validation_scores, 0.95, interpolation="higher"))
    baselines = Baselines.train(train_values, validation_values)
    fixed = fixed_point_normalization(mean, std)
    normalization = {
        "format": "mtfs-sentinel-normalization-v1",
        "feature_schema_id": "mtfs-storage-sentinel-model-input",
        "feature_schema_version": 1,
        "target_id": target,
        "transport_id": transport,
        "build_type": build_type,
        "mean": mean.tolist(),
        "std": std.tolist(),
        "clamp_zscore": 8.0,
        **fixed,
    }
    threshold_payload = {
        "format": "mtfs-sentinel-threshold-v1",
        "selection_data": "normal-validation-only",
        "quantile": 0.95,
        "comparison": "score > anomaly_threshold",
        "anomaly_threshold": threshold,
        "reference_anomaly_score": float(np.median(validation_scores)),
    }
    test_scores = model.scores(test_values)
    base_payload = baselines.as_dict()
    training_manifest = {
        "format": "mtfs-sentinel-training-manifest-v1",
        "tool_version": TOOL_VERSION,
        "seed": args.seed,
        "epochs": args.epochs,
        "target_id": target,
        "transport_id": transport,
        "build_type": build_type,
        "feature_schema_sha256": schema_hash(),
        "topology_policy": "common-fixed-24-12-4-12-24",
        "candidate_comparison": candidate_report,
        "split_unit": "session",
        "train_sessions": train_details,
        "validation_sessions": validation_details,
        "test_sessions": test_details,
        "threshold_source": validation_session.session_id,
        "held_out_normal_false_warning_rate": float(np.mean(test_scores > threshold)),
        "held_out_normal_score_median": float(np.median(test_scores)),
        "card_count": len({dataset.card_id for dataset in datasets}),
        "limitations": [
            "Threshold uses normal validation only; injected evaluation data never tunes it.",
            "A single card_id means within-card evidence only, not card-generalization evidence."
        ],
    }
    negative_index = int(np.argmin(test_scores))
    test_vectors = {
        "format": "mtfs-sentinel-test-vectors-v1",
        "negative": {"raw_vector": test_raw[negative_index].astype(int).tolist(),
                     "normalized_vector": test_values[negative_index].tolist(),
                     "reference_score": float(test_scores[negative_index]),
                     "expected_anomaly": False},
        "positive": None,
        "note": "positive vector is populated only from a real injected evaluation dataset",
    }
    evaluation_report = None
    if args.evaluation_dataset:
        evaluation_datasets = [load_dataset(path) for path in args.evaluation_dataset]
        eval_target, eval_transport = assert_same_profile(evaluation_datasets)
        if (eval_target, eval_transport) != (target, transport):
            raise DatasetError("evaluation dataset transport profile mismatch")
        evaluation_report, evaluated_vectors = evaluate_datasets(
            evaluation_datasets + [test_session], model, normalization,
            threshold_payload, baselines)
        if evaluated_vectors["positive"] is not None:
            positive = evaluated_vectors["positive"]
            test_vectors["positive"] = {
                "raw_vector": positive["raw_vector"],
                "normalized_vector": positive["normalized_vector"],
                "reference_score": positive["autoencoder"],
                "expected_anomaly": True,
            }
    args.output_dir.mkdir(parents=True, exist_ok=False)
    _write_json(args.output_dir / "feature_schema_v1.json", feature_schema())
    _write_json(args.output_dir / "model.json", model.as_dict())
    _write_json(args.output_dir / "normalization.json", normalization)
    _write_json(args.output_dir / "threshold.json", threshold_payload)
    _write_json(args.output_dir / "baselines.json", base_payload)
    _write_json(args.output_dir / "training_manifest.json", training_manifest)
    _write_json(args.output_dir / "test_vectors.json", test_vectors)
    if evaluation_report is not None:
        _write_json(args.output_dir / "evaluation_report.json", evaluation_report)
    hashes = {path.name: sha256_file(path) for path in sorted(args.output_dir.iterdir()) if path.is_file()}
    artifact_index = {
        "format": "mtfs-sentinel-artifact-index-v1",
        "tool_version": TOOL_VERSION,
        "target_id": target,
        "transport_id": transport,
        "build_type": build_type,
        "files_sha256": hashes,
        "dataset_sha256": {str(dataset.path): sha256_file(dataset.path) for dataset in datasets},
    }
    _write_json(args.output_dir / "artifact_index.json", artifact_index)
    return {"artifact": str(args.output_dir), "threshold": threshold,
            "topology": model.dimensions, "parameter_count": model.parameter_count,
            "multiply_accumulate_count": model.multiply_accumulate_count,
            "held_out_normal_false_warning_rate": training_manifest["held_out_normal_false_warning_rate"],
            "hashes": hashes}


def main(argv: list[str] | None = None) -> int:
    try:
        result = run(build_parser().parse_args(argv))
        print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
        return 0
    except (DatasetError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
