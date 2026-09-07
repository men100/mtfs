"""Evaluate a locked held-out card once under a pre-existing v2 freeze."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

from dataset import load_dataset, sha256_file
from evaluate_baseline_v2_canonical import _infer, _prepare, _higher
from schema import DatasetError, canonical_json_bytes


def _load_locked(paths: list[Path], condition: str, card_id: str) -> list:
    datasets = [load_dataset(path.resolve()) for path in paths]
    if not datasets:
        raise DatasetError(f"no held-out {condition} datasets")
    for dataset in datasets:
        context = dataset.manifest.get("capture_context", {})
        if dataset.card_id != card_id or \
                context.get("dataset_role") != "heldout-locked" or \
                context.get("condition") != condition:
            raise DatasetError(f"{dataset.path}: held-out identity mismatch")
    return datasets


def run(args: argparse.Namespace) -> dict:
    output = args.output.resolve()
    if output.exists():
        raise DatasetError("held-out result exists; refusing a second evaluation")
    freeze_path = args.freeze.resolve()
    freeze_hash = sha256_file(freeze_path)
    if freeze_hash.lower() != args.freeze_sha256.lower():
        raise DatasetError("freeze record hash mismatch")
    freeze = json.loads(freeze_path.read_text(encoding="utf-8"))
    if freeze.get("status") != "FROZEN-BEFORE-HELDOUT" or \
            freeze.get("heldout_read") is not False:
        raise DatasetError("held-out gate is not valid")
    artifact = args.artifact.resolve()
    policy = json.loads((artifact / "preprocessing.json").read_text(
        encoding="utf-8"))
    canonical = args.canonical_tflite.resolve()
    if policy.get("canonical_sha256") != \
            freeze["preprocessing_policy_sha256"] or \
            sha256_file(canonical) != \
            freeze["canonical_full_int8_tflite_sha256"]:
        raise DatasetError("runtime artifact differs from freeze")
    card_id = freeze["heldout_gate"]["card_id"]
    normal = _load_locked(args.normal, "normal", card_id)
    pseudo = _load_locked(args.pseudo, "pseudo", card_id)
    normal_q4, normal_rows, normal_diag = _prepare(normal, policy)
    pseudo_q4, pseudo_rows, pseudo_diag = _prepare(pseudo, policy)
    combined = np.concatenate((normal_q4, pseudo_q4), axis=0)
    inferred, _ = _infer(canonical, combined)
    normal_inferred = inferred[:len(normal_q4)]
    pseudo_inferred = inferred[len(normal_q4):]
    threshold = int(freeze["threshold_q8"])
    normal_scores = [row["score_q8"] for row in normal_inferred]
    normal_ood = sum(bool(item["out_of_distribution"])
                     for item in normal_diag)
    false_warnings = sum(not normal_diag[index]["out_of_distribution"] and
                         score > threshold
                         for index, score in enumerate(normal_scores))
    stage_metrics = {}
    for stage in ("baseline", "light", "medium", "strong", "recovery"):
        indexes = [index for index, row in enumerate(pseudo_rows)
                   if str(row["stage"]) == stage]
        if not indexes:
            continue
        ood = int(sum(bool(pseudo_diag[index]["out_of_distribution"])
                      for index in indexes))
        anomalies = int(sum(bool(
            not pseudo_diag[index]["out_of_distribution"] and
            pseudo_inferred[index]["score_q8"] > threshold)
            for index in indexes))
        scores = [pseudo_inferred[index]["score_q8"] for index in indexes]
        stage_metrics[stage] = {
            "samples": len(indexes), "ood": ood,
            "ai_anomaly": anomalies,
            "safe_detection": ood + anomalies,
            "safe_detection_rate": (ood + anomalies) / len(indexes),
            "score_median_q8": float(np.median(scores)),
            "score_p95_q8": _higher(scores),
        }
    limits = freeze.get("classification_acceptance_limits",
                        freeze.get("acceptance_limits"))
    if not isinstance(limits, dict):
        raise DatasetError("freeze record has no classification acceptance limits")
    normal_fwr = false_warnings / len(normal_scores)
    normal_ood_rate = normal_ood / len(normal_scores)
    recovery_normal_rate = 1.0 - \
        stage_metrics["recovery"]["safe_detection_rate"]
    checks = {
        "normal_false_warning": normal_fwr <=
            float(limits["maximum_normal_false_warning_rate"]),
        "normal_ood": normal_ood_rate <=
            float(limits["maximum_normal_ood_rate"]),
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
    result = {
        "format": ("mtfs-sentinel-st-baseline-relative-heldout-v2"
                   if freeze.get("target") == "STM32N6570-DK"
                   else "mtfs-sentinel-ra-baseline-relative-heldout-v2"),
        "status": status,
        "freeze_record_sha256": freeze_hash,
        "threshold_q8_unchanged": threshold,
        "limits_unchanged": limits,
        "heldout_card_id": card_id,
        "heldout_opened": True,
        "normal": {
            "samples": len(normal_scores),
            "false_warnings": int(false_warnings),
            "false_warning_rate": normal_fwr,
            "ood": int(normal_ood), "ood_rate": normal_ood_rate,
            "score_histogram": {str(value): normal_scores.count(value)
                                for value in sorted(set(normal_scores))},
        },
        "pseudo": stage_metrics,
        "recovery_normal_rate": recovery_normal_rate,
        "checks": checks,
        "cpu_tflite_raw_int8_bit_exact": True,
        "evaluated_vectors": len(inferred),
        "datasets": [{
            "session_id": dataset.session_id,
            "condition": dataset.manifest["capture_context"]["condition"],
            "dataset_sha256": sha256_file(dataset.path),
            "source_sha256": dataset.manifest.get("source_sha256"),
        } for dataset in normal + pseudo],
        "post_heldout_policy_change": "forbidden",
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(canonical_json_bytes(result) + b"\n")
    return {**result, "result_sha256": sha256_file(output)}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Evaluate a held-out card under an immutable v2 freeze")
    result.add_argument("--freeze", type=Path, required=True)
    result.add_argument("--freeze-sha256", required=True)
    result.add_argument("--artifact", type=Path, required=True)
    result.add_argument("--canonical-tflite", type=Path, required=True)
    result.add_argument("--normal", type=Path, action="append", required=True)
    result.add_argument("--pseudo", type=Path, action="append", required=True)
    result.add_argument("--output", type=Path, required=True)
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
