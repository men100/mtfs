from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

from dataset import Dataset, assert_same_profile, load_dataset, sha256_file
from model import Baselines, DenseAutoencoder, load_artifact
from schema import DatasetError, deterministic_rule, encode_row, normalize
from version import TOOL_VERSION


def _distribution(values: list[float]) -> dict | None:
    if not values:
        return None
    array = np.asarray(values, dtype=np.float64)
    return {
        "count": int(array.size),
        "min": float(np.min(array)),
        "median": float(np.median(array)),
        "p95": float(np.quantile(array, 0.95)),
        "max": float(np.max(array)),
    }


def _method_metrics(scores: list[dict], key: str, threshold: float) -> dict:
    normal = [item[key] for item in scores if item["normal"]]
    anomalous = [item[key] for item in scores if item["anomalous"]]
    strong = [item[key] for item in scores if item["stage"] == "strong"]
    tp = sum(value > threshold for value in anomalous)
    fp = sum(value > threshold for value in normal)
    return {
        "threshold": threshold,
        "confusion": {"true_positive": tp, "false_negative": len(anomalous) - tp,
                      "false_positive": fp, "true_negative": len(normal) - fp},
        "held_out_normal_false_warning_rate": fp / len(normal) if normal else None,
        "delay_detection_rate": tp / len(anomalous) if anomalous else None,
        "strong_delay_detection_rate": (sum(value > threshold for value in strong) /
                                        len(strong)) if strong else None,
        "normal_score_distribution": _distribution(normal),
        "anomaly_score_distribution": _distribution(anomalous),
    }


def evaluate_datasets(datasets: list[Dataset], model: DenseAutoencoder,
                      normalization: dict, threshold_payload: dict,
                      baselines: Baselines) -> tuple[dict, dict]:
    target, transport = assert_same_profile(datasets)
    if target != int(normalization["target_id"]) or transport != int(normalization["transport_id"]):
        raise DatasetError("artifact transport profile mismatch (fail-closed)")
    mean = np.asarray(normalization["mean"], dtype=np.float64)
    std = np.asarray(normalization["std"], dtype=np.float64)
    if mean.shape != (24,) or std.shape != (24,) or np.any(std <= 0.0):
        raise DatasetError("invalid normalization artifact")
    scored: list[dict] = []
    stage_scores: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: {"autoencoder": [], "simple": [], "mahalanobis": []})
    hard_total = hard_detected = 0
    sessions: dict[str, list[dict]] = defaultdict(list)
    for dataset in datasets:
        for row in dataset.rows:
            rules = deterministic_rule(row)
            if row["command"] == "pseudo-collect-hard-fault":
                hard_total += 1
                if rules:
                    hard_detected += 1
                continue
            try:
                raw = np.asarray([encode_row(row)], dtype=np.float64)
            except DatasetError:
                continue
            values = normalize(raw, mean, std)
            ae_score = float(model.scores(values)[0])
            simple_score = float(baselines.simple_scores(values)[0])
            mahalanobis_score = float(baselines.mahalanobis_scores(values)[0])
            stage = str(row["stage"])
            origin = str(row["scenario_origin"])
            severity = int(row["severity"])
            item = {
                "session_id": dataset.session_id,
                "card_id": dataset.card_id,
                "stage": stage,
                "severity": severity,
                "normal": origin == "natural" and row["command"] == "record",
                "anomalous": origin == "injected" and severity > 0 and stage not in {"recovery", "baseline"},
                "recovery": stage == "recovery",
                "raw_vector": raw[0].astype(int).tolist(),
                "normalized_vector": values[0].tolist(),
                "autoencoder": ae_score,
                "simple": simple_score,
                "mahalanobis": mahalanobis_score,
            }
            scored.append(item)
            sessions[dataset.session_id].append(item)
            for key, value in (("autoencoder", ae_score), ("simple", simple_score),
                               ("mahalanobis", mahalanobis_score)):
                stage_scores[stage][key].append(value)
    ae_threshold = float(threshold_payload["anomaly_threshold"])
    methods = {
        "simple_feature_threshold": _method_metrics(scored, "simple", baselines.simple_threshold),
        "mahalanobis": _method_metrics(scored, "mahalanobis", baselines.mahalanobis_threshold),
        "dense_autoencoder": _method_metrics(scored, "autoencoder", ae_threshold),
    }
    ae_metrics = methods["dense_autoencoder"]
    baseline_metrics = [methods["simple_feature_threshold"], methods["mahalanobis"]]
    if (ae_metrics["held_out_normal_false_warning_rate"] is None or
        ae_metrics["strong_delay_detection_rate"] is None):
        comparison = "not-established-insufficient-normal-or-strong-delay-data"
    else:
        ae_pair = (ae_metrics["strong_delay_detection_rate"],
                   -ae_metrics["held_out_normal_false_warning_rate"])
        baseline_pairs = [(item["strong_delay_detection_rate"],
                           -item["held_out_normal_false_warning_rate"])
                          for item in baseline_metrics]
        comparison = ("autoencoder-better-on-strong-detection-fpr-pair" if
                      ae_pair > max(baseline_pairs) else
                      "autoencoder-not-better-than-best-baseline")
    stages = {
        stage: {key: _distribution(values) for key, values in method_values.items()}
        for stage, method_values in sorted(stage_scores.items())
    }
    strong = [item["autoencoder"] for item in scored if item["stage"] == "strong"]
    recovery = [item["autoencoder"] for item in scored if item["recovery"]]
    severity_medians = []
    for severity in (1, 2, 3):
        values = [item["autoencoder"] for item in scored if item["severity"] == severity]
        if values:
            severity_medians.append((severity, float(np.median(values))))
    session_metrics = {}
    for session_id, items in sessions.items():
        normal = [item for item in items if item["normal"]]
        session_metrics[session_id] = {
            "card_id": normal[0]["card_id"] if normal else items[0]["card_id"],
            "normal_frames": len(normal),
            "false_warning_rate": (sum(item["autoencoder"] > ae_threshold for item in normal) / len(normal)) if normal else None,
        }
    report = {
        "format": "mtfs-sentinel-evaluation-v1",
        "tool_version": TOOL_VERSION,
        "target_id": target,
        "transport_id": transport,
        "methods": methods,
        "baseline_comparison_conclusion": comparison,
        "stage_score_distributions": stages,
        "strong_delay_detection_rate": (sum(value > ae_threshold for value in strong) / len(strong)) if strong else None,
        "recovery_normal_rate": (sum(value <= ae_threshold for value in recovery) / len(recovery)) if recovery else None,
        "severity_medians": [{"severity": severity, "median": median} for severity, median in severity_medians],
        "severity_monotonic_non_decreasing": all(a[1] <= b[1] for a, b in zip(severity_medians, severity_medians[1:])),
        "hard_fault_rule_detection_rate": hard_detected / hard_total if hard_total else None,
        "hard_fault_frames": hard_total,
        "session_metrics": session_metrics,
        "limitations": [
            "Scores demonstrate only the supplied cards, sessions, workloads, targets, and transports.",
            "The detector identifies statistical I/O deviation; it does not predict media lifetime or failure time."
        ],
    }
    normal_candidates = [item for item in scored if item["normal"]]
    positive_candidates = [item for item in scored if item["anomalous"]]
    vectors = {
        "format": "mtfs-sentinel-test-vectors-v1",
        "negative": min(normal_candidates, key=lambda item: item["autoencoder"]) if normal_candidates else None,
        "positive": max(positive_candidates, key=lambda item: item["autoencoder"]) if positive_candidates else None,
    }
    return report, vectors


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Evaluate a target-specific Sentinel model")
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--test-vectors", type=Path)
    return parser


def run(args: argparse.Namespace) -> dict:
    model, normalization, threshold, baselines = load_artifact(args.artifact)
    datasets = [load_dataset(path) for path in args.dataset]
    report, vectors = evaluate_datasets(datasets, model, normalization, threshold, baselines)
    if args.output:
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(report, output, indent=2, sort_keys=True, allow_nan=False)
            output.write("\n")
    if args.test_vectors:
        with args.test_vectors.open("x", encoding="utf-8") as output:
            json.dump(vectors, output, indent=2, sort_keys=True, allow_nan=False)
            output.write("\n")
    return report


def main(argv: list[str] | None = None) -> int:
    try:
        report = run(build_parser().parse_args(argv))
        print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
        return 0
    except (DatasetError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
