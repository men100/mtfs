"""Create the immutable pre-held-out RA baseline-relative v2 freeze record."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from dataset import sha256_file
from schema import DatasetError, canonical_json_bytes


def _json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise DatasetError(f"{path}: JSON object required")
    return value


def run(args: argparse.Namespace) -> dict:
    output = args.output.resolve()
    if output.exists():
        raise DatasetError("freeze record exists; refusing overwrite")
    artifact = args.artifact.resolve()
    canonical = args.canonical_tflite.resolve()
    validation_dir = args.canonical_validation.resolve()
    optimized = args.optimized_tflite.resolve()
    optimized_manifest_path = args.optimized_manifest.resolve()
    training = _json(artifact / "training_manifest.json")
    policy = _json(artifact / "preprocessing.json")
    canonical_manifest = _json(canonical.with_suffix(
        canonical.suffix + ".manifest.json"))
    validation = _json(validation_dir / "canonical_validation.json")
    optimized_manifest = _json(optimized_manifest_path)
    if training.get("heldout_read") is not False or \
            training.get("heldout_sessions") != [] or \
            canonical_manifest.get("heldout_read") is not False or \
            validation.get("heldout_read") is not False:
        raise DatasetError("held-out gate was opened before freeze")
    if validation.get("status") != "PASS" or \
            not all(validation.get("checks", {}).values()):
        raise DatasetError("canonical validation did not pass")
    canonical_hash = sha256_file(canonical)
    optimized_hash = sha256_file(optimized)
    if canonical_manifest.get("canonical_full_int8_tflite_sha256") != \
            canonical_hash or \
            optimized_manifest.get("canonical_model_sha256") != canonical_hash or \
            optimized_manifest.get("runtime_binary_sha256") != optimized_hash or \
            optimized_manifest.get("generation_reproducible") is not True or \
            optimized_manifest.get("cpu_operator_count") != 0 or \
            optimized_manifest.get("ethos_u_custom_operator_count") != 1:
        raise DatasetError("canonical/Vela identity or policy mismatch")
    policy_hash = policy.get("canonical_sha256")
    if not isinstance(policy_hash, str) or \
            canonical_manifest.get("preprocessing_policy_sha256") != policy_hash or \
            validation.get("preprocessing_policy_sha256") != policy_hash or \
            training.get("preprocessing_policy_sha256") != policy_hash:
        raise DatasetError("preprocessing identity mismatch")
    freeze = {
        "format": "mtfs-sentinel-ra-baseline-relative-freeze-v2",
        "status": "FROZEN-BEFORE-HELDOUT",
        "heldout_read": False,
        "target": "EK-RA8P1",
        "transport": "SPI",
        "build_type": training["build_type"],
        "feature_schema_canonical_sha256":
            training["feature_schema_canonical_sha256"],
        "preprocessing_contract_version": 2,
        "preprocessing_policy_sha256": policy_hash,
        "preprocessing_artifact_sha256": sha256_file(
            artifact / "preprocessing.json"),
        "training_artifact_index_sha256": sha256_file(
            artifact / "artifact_index.json"),
        "source_float_model_sha256": sha256_file(artifact / "model.json"),
        "canonical_full_int8_tflite_sha256": canonical_hash,
        "canonical_manifest_sha256": sha256_file(canonical.with_suffix(
            canonical.suffix + ".manifest.json")),
        "canonical_validation_sha256": sha256_file(
            validation_dir / "canonical_validation.json"),
        "canonical_cpu_runtime_sha256": sha256_file(
            validation_dir / "sentinel_cpu_tflite_int8.bin"),
        "vela_optimized_tflite_sha256": optimized_hash,
        "vela_manifest_sha256": sha256_file(optimized_manifest_path),
        "vela_version": optimized_manifest["tool_versions"]["vela"],
        "vela_configuration": optimized_manifest["vela_configuration"],
        "threshold_q8": int(validation["threshold_q8"]),
        "threshold_selection": validation["threshold_selection"],
        "acceptance_limits": validation["acceptance_limits"],
        "numeric_contract": {
            "maximum_cpu_tflite_raw_int8_error": 0,
            "maximum_cpu_npu_raw_int8_error": 0,
            "maximum_common_q4_error": 0,
            "maximum_score_q8_error": 0,
            "threshold_or_limit_changes_after_heldout": "forbidden",
        },
        "training_sessions": training["train_sessions"],
        "validation_sessions": training["validation_sessions"],
        "validation_pseudo_sessions": training["validation_pseudo_sessions"],
        "validation_result": {
            "normal": validation["normal"],
            "pseudo": validation["pseudo"],
            "recovery_normal_rate": validation["recovery_normal_rate"],
        },
        "heldout_gate": {
            "card_id": args.heldout_card_id,
            "permitted_use": "one evaluation under this exact freeze",
            "policy_reselection": "forbidden",
        },
        "limitation": (
            "A card already slow when its baseline is established cannot be "
            "classified as degraded from absolute speed alone."),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(canonical_json_bytes(freeze) + b"\n")
    return {**freeze, "freeze_record_sha256": sha256_file(output)}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Freeze RA baseline-relative v2 before held-out access")
    result.add_argument("--artifact", type=Path, required=True)
    result.add_argument("--canonical-tflite", type=Path, required=True)
    result.add_argument("--canonical-validation", type=Path, required=True)
    result.add_argument("--optimized-tflite", type=Path, required=True)
    result.add_argument("--optimized-manifest", type=Path, required=True)
    result.add_argument("--heldout-card-id", required=True)
    result.add_argument("--output", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        result = run(parser().parse_args(argv))
        print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
        return 0
    except (DatasetError, OSError, ValueError, KeyError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
