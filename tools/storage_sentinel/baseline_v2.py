from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

from dataset import load_dataset, usable_vectors
from schema import DatasetError, canonical_json_bytes, feature_schema

PREPROCESSING_VERSION = 2
FEATURE_COUNT = 24
LATENCY_FEATURES = {0, 7, 14}
WARMUP_CANDIDATES = (8, 16, 32)
MINIMUM_ACTIVE_SIGNAL = 16
MINIMUM_RELATIVE_SCALE = 16
RULE_OWNED_FEATURES = {1, 8, 15}


def _round_away(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        raise DatasetError("relative preprocessing denominator must be positive")
    magnitude = abs(numerator)
    rounded = (magnitude + denominator // 2) // denominator
    return -rounded if numerator < 0 else rounded


def median_baseline(vectors: list[list[int]], windows: int) -> list[int]:
    if windows not in WARMUP_CANDIDATES or len(vectors) < windows:
        raise DatasetError("invalid or unavailable baseline warmup window count")
    matrix = np.asarray(vectors[:windows], dtype=np.uint64)
    if matrix.shape != (windows, FEATURE_COUNT):
        raise DatasetError("baseline vector shape mismatch")
    result: list[int] = []
    for feature in range(FEATURE_COUNT):
        ordered = sorted(int(value) for value in matrix[:, feature])
        result.append((ordered[windows // 2 - 1] + ordered[windows // 2] + 1) // 2)
    return result


def relative_vector(raw: list[int], baseline: list[int],
                    latency_baseline_floor: tuple[int, int, int]) -> list[int]:
    if len(raw) != FEATURE_COUNT or len(baseline) != FEATURE_COUNT:
        raise DatasetError("relative vector shape mismatch")
    if len(latency_baseline_floor) != 3 or any(value <= 0
                                                for value in latency_baseline_floor):
        raise DatasetError("invalid latency baseline floor")
    slots = {0: 0, 7: 1, 14: 2}
    result: list[int] = []
    for index, (value, reference) in enumerate(zip(raw, baseline)):
        delta = int(value) - int(reference)
        if index in LATENCY_FEATURES:
            denominator = max(int(reference), latency_baseline_floor[slots[index]])
            delta = _round_away(delta * 1000, denominator)
        result.append(delta)
    return result


def scale_relative(relative: list[int], scale_floor: list[int], active_mask: int,
                   maximum_saturated_features: int) -> tuple[list[int], int, bool]:
    if len(relative) != FEATURE_COUNT or len(scale_floor) != FEATURE_COUNT:
        raise DatasetError("relative scale shape mismatch")
    if active_mask <= 0 or active_mask >> FEATURE_COUNT:
        raise DatasetError("invalid active feature mask")
    output: list[int] = []
    saturation_mask = 0
    for index, value in enumerate(relative):
        if not active_mask & (1 << index):
            output.append(0)
            continue
        scale = int(scale_floor[index])
        if scale <= 0:
            raise DatasetError("active relative scale floor must be positive")
        quantized = _round_away(int(value) * 16, scale)
        if quantized > 127:
            quantized = 127
            saturation_mask |= 1 << index
        elif quantized < -128:
            quantized = -128
            saturation_mask |= 1 << index
        output.append(quantized)
    count = saturation_mask.bit_count()
    return output, saturation_mask, count > maximum_saturated_features


def _context(dataset, role: str | None = None,
             condition: str | None = None) -> dict:
    context = dataset.manifest.get("capture_context")
    if not isinstance(context, dict) or context.get("contract") != \
            "baseline-relative-v2":
        raise DatasetError(f"{dataset.path}: baseline-relative-v2 manifest required")
    if role is not None and context.get("dataset_role") != role:
        raise DatasetError(f"{dataset.path}: expected dataset role {role}")
    if condition is not None and context.get("condition") != condition:
        raise DatasetError(f"{dataset.path}: expected {condition} capture")
    if context.get("operator_card_id") != dataset.card_id:
        raise DatasetError(f"{dataset.path}: card identity mismatch")
    return context


def relative_dataset(dataset, windows: int,
                     latency_floor: tuple[int, int, int] = (1, 1, 1)) \
        -> tuple[list[list[int]], list[dict], list[int]]:
    """Return post-warmup relative vectors, their rows, and the baseline."""
    _context(dataset)
    vectors, rows = usable_vectors(dataset)
    baseline = median_baseline(vectors, windows)
    relative = [relative_vector(vector, baseline, latency_floor)
                for vector in vectors[windows:]]
    if not relative:
        raise DatasetError(f"{dataset.path}: no post-baseline windows")
    return relative, rows[windows:], baseline


def q4_dataset(dataset, policy: dict) \
        -> tuple[list[list[int]], list[dict], list[dict]]:
    """Apply a frozen policy to one capture without crossing session state."""
    windows = int(policy["warmup_windows"])
    latency_floor = tuple(int(value) for value in
                          policy["latency_baseline_floor"])
    relative, rows, _ = relative_dataset(dataset, windows, latency_floor)
    scales = [int(value) for value in policy["relative_scale_floor"]]
    active_mask = int(policy["active_feature_mask"])
    maximum = int(policy["maximum_saturated_features"])
    inputs: list[list[int]] = []
    diagnostics: list[dict] = []
    for vector in relative:
        scaled, mask, ood = scale_relative(vector, scales, active_mask, maximum)
        inputs.append(scaled)
        diagnostics.append({"saturation_mask": mask,
                            "saturation_count": mask.bit_count(),
                            "out_of_distribution": ood})
    return inputs, rows, diagnostics


def _require_distinct_sessions(datasets: list) -> None:
    sessions = [dataset.session_id for dataset in datasets]
    if len(sessions) != len(set(sessions)):
        raise DatasetError("duplicate capture session")


def select_policy(training_normal_paths: list[Path],
                  validation_normal_paths: list[Path],
                  training_pseudo_paths: list[Path],
                  validation_pseudo_paths: list[Path],
                  candidates: tuple[int, ...] = WARMUP_CANDIDATES) -> dict:
    """Select the v2 preprocessing policy without reading held-out data."""
    training_normal = [load_dataset(path) for path in training_normal_paths]
    validation_normal = [load_dataset(path) for path in validation_normal_paths]
    training_pseudo = [load_dataset(path) for path in training_pseudo_paths]
    validation_pseudo = [load_dataset(path) for path in validation_pseudo_paths]
    supplied = training_normal + validation_normal + training_pseudo + validation_pseudo
    if not training_normal or not validation_normal or not validation_pseudo:
        raise DatasetError("training normal, validation normal, and validation pseudo data are required")
    _require_distinct_sessions(supplied)
    for dataset in training_normal:
        _context(dataset, "training-candidate", "normal")
        if dataset.manifest.get("command") != "record":
            raise DatasetError(f"{dataset.path}: normal training command mismatch")
    for dataset in validation_normal:
        _context(dataset, "validation-candidate", "normal")
        if dataset.manifest.get("command") != "record":
            raise DatasetError(f"{dataset.path}: normal validation command mismatch")
    for dataset in training_pseudo:
        _context(dataset, "training-candidate", "pseudo")
    for dataset in validation_pseudo:
        _context(dataset, "validation-candidate", "pseudo")
    training_cards = {dataset.card_id for dataset in training_normal + training_pseudo}
    validation_cards = {dataset.card_id for dataset in validation_normal + validation_pseudo}
    if not training_cards or len(validation_cards) != 1 or \
            training_cards & validation_cards:
        raise DatasetError("validation must be one identified card disjoint from all training cards")

    reports = []
    normal_datasets = training_normal + validation_normal
    pseudo_datasets = training_pseudo + validation_pseudo
    for windows in candidates:
        normal_relative: list[list[int]] = []
        pseudo_by_stage: dict[str, list[list[int]]] = {}
        for dataset in normal_datasets:
            relative, _, _ = relative_dataset(dataset, windows)
            normal_relative.extend(relative)
        for dataset in pseudo_datasets:
            relative, rows, _ = relative_dataset(dataset, windows)
            for vector, row in zip(relative, rows):
                pseudo_by_stage.setdefault(str(row["stage"]), []).append(vector)
        normal = np.abs(np.asarray(normal_relative, dtype=np.int64))
        signal = [0] * FEATURE_COUNT
        for feature in range(FEATURE_COUNT):
            for stage in ("medium", "strong"):
                values = pseudo_by_stage.get(stage, [])
                if values:
                    signal[feature] = max(signal[feature], abs(int(np.median(
                        np.asarray(values, dtype=np.int64)[:, feature]))))
        active = [feature for feature, value in enumerate(signal)
                  if value >= MINIMUM_ACTIVE_SIGNAL and
                  feature not in RULE_OWNED_FEATURES]
        if not active:
            raise DatasetError("no relative feature separates validation delay data")
        p95 = np.quantile(normal, 0.95, axis=0,
                          method="higher").astype(int).tolist()
        maximum = np.max(normal, axis=0).astype(int).tolist()
        reports.append({
            "warmup_windows": windows,
            "runtime_sample_ram_bytes": windows * FEATURE_COUNT * 4,
            "active_features": active,
            "active_feature_mask": sum(1 << feature for feature in active),
            "selection_objective_sum_active_normal_p95_abs":
                sum(p95[feature] for feature in active),
            "normal_feature_p95_abs": p95,
            "normal_feature_max_abs": maximum,
            "pseudo_medium_strong_median_signal_abs": signal,
        })
    selected = min(reports, key=lambda item: (
        item["selection_objective_sum_active_normal_p95_abs"],
        -item["warmup_windows"]))
    scales = [0] * FEATURE_COUNT
    for feature in selected["active_features"]:
        scales[feature] = max(MINIMUM_RELATIVE_SCALE,
                              selected["normal_feature_max_abs"][feature])
    policy = {
        "format": "mtfs-sentinel-baseline-relative-policy-v2",
        "preprocessing_contract_version": PREPROCESSING_VERSION,
        "feature_schema_identity": feature_schema()["schema_id"],
        "warmup_windows": selected["warmup_windows"],
        "baseline_estimator": "per-feature-even-median-midpoint-round-up",
        "latency_relative_transform":
            "round-away((raw-baseline)*1000/max(baseline,floor))",
        "share_relative_transform": "signed-raw-permille-difference",
        "latency_baseline_floor": [1, 1, 1],
        "relative_scale_floor": scales,
        "active_feature_mask": selected["active_feature_mask"],
        "maximum_saturated_features": 0,
        "saturation_action": "OOD-RULE-safe-anomaly",
        "ready_update_policy": "frozen-until-reset-or-generation-change",
        "inactive_feature_encoding": 0,
        "training_card_ids": sorted(training_cards),
        "validation_card_ids": sorted(validation_cards),
        "selection_rule": {
            "active": "validation/training pseudo medium-or-strong median absolute signal >= 16",
            "scale": "max(16, maximum absolute normal relative value across training+validation)",
            "warmup": "minimum sum of active-feature normal p95 absolute relative values; largest wins tie",
            "ood": "any int8 saturation",
        },
    }
    policy["canonical_sha256"] = hashlib.sha256(
        canonical_json_bytes(policy)).hexdigest()
    return {
        "format": "mtfs-sentinel-baseline-policy-freeze-candidate-v2",
        "status": "SELECTED-NOT-ARTIFACT-FROZEN",
        "heldout_read": False,
        "policy": policy,
        "candidate_comparison": reports,
        "source_sessions": [{
            "session_id": dataset.session_id,
            "card_id": dataset.card_id,
            "role": dataset.manifest["capture_context"]["dataset_role"],
            "condition": dataset.manifest["capture_context"]["condition"],
            "dataset_sha256": dataset.manifest["dataset_sha256"],
            "source_sha256": dataset.manifest.get("source_sha256"),
        } for dataset in supplied],
    }


def audit(datasets: list[Path], candidates: tuple[int, ...]) -> dict:
    loaded = [load_dataset(path) for path in datasets]
    sessions = []
    for dataset in loaded:
        manifest = dataset.manifest
        context = manifest.get("capture_context")
        if not isinstance(context, dict) or context.get("contract") != \
                "baseline-relative-v2":
            raise DatasetError(f"{dataset.path}: baseline-relative-v2 manifest required")
        vectors, _ = usable_vectors(dataset)
        sessions.append((dataset, vectors))
    reports = []
    for windows in candidates:
        per_session = []
        all_absolute: list[list[int]] = []
        for dataset, vectors in sessions:
            baseline = median_baseline(vectors, windows)
            relative = [relative_vector(vector, baseline, (1, 1, 1))
                        for vector in vectors[windows:]]
            if not relative:
                raise DatasetError(f"{dataset.path}: no post-baseline windows")
            absolute = np.abs(np.asarray(relative, dtype=np.int64))
            all_absolute.extend(absolute.tolist())
            per_session.append({
                "session_id": dataset.session_id,
                "card_id": dataset.card_id,
                "post_baseline_windows": len(relative),
                "feature_p95_abs": np.quantile(absolute, 0.95, axis=0,
                                                method="higher").astype(int).tolist(),
                "feature_max_abs": np.max(absolute, axis=0).astype(int).tolist(),
            })
        combined = np.asarray(all_absolute, dtype=np.int64)
        reports.append({
            "warmup_windows": windows,
            "runtime_sample_ram_bytes": windows * FEATURE_COUNT * 4,
            "combined_feature_p95_abs": np.quantile(combined, 0.95, axis=0,
                                                     method="higher").astype(int).tolist(),
            "combined_feature_max_abs": np.max(combined, axis=0).astype(int).tolist(),
            "sessions": per_session,
        })
    return {
        "format": "mtfs-sentinel-baseline-policy-audit-v1",
        "preprocessing_contract_version": PREPROCESSING_VERSION,
        "feature_schema_identity": feature_schema()["schema_id"],
        "candidate_status": "NOT-FROZEN",
        "candidates": reports,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Audit baseline-relative v2 warmup candidates")
    parser.add_argument("--dataset", type=Path, action="append")
    parser.add_argument("--training-normal", type=Path, action="append",
                        default=[])
    parser.add_argument("--training-pseudo", type=Path, action="append",
                        default=[])
    parser.add_argument("--validation-normal", type=Path, action="append",
                        default=[])
    parser.add_argument("--validation-pseudo", type=Path, action="append",
                        default=[])
    parser.add_argument("--candidate", type=int, action="append",
                        choices=WARMUP_CANDIDATES)
    parser.add_argument("--output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        args = build_parser().parse_args(argv)
        candidates = tuple(args.candidate or WARMUP_CANDIDATES)
        selecting = bool(args.training_normal or args.training_pseudo or
                         args.validation_normal or args.validation_pseudo)
        if selecting and args.dataset:
            raise DatasetError("--dataset audit cannot be mixed with policy selection")
        if selecting:
            report = select_policy(args.training_normal, args.validation_normal,
                                   args.training_pseudo,
                                   args.validation_pseudo, candidates)
        elif args.dataset:
            report = audit(args.dataset, candidates)
        else:
            raise DatasetError("supply --dataset or training/validation datasets")
        text = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as output:
                output.write(text)
        else:
            print(text, end="")
        return 0
    except (DatasetError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
