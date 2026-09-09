"""Rebind RA acceptance metadata after a path-only conversion-manifest change.

This does not alter or rerun numerical results.  It fails unless the evaluated
and published conversion manifests differ only in calibration dataset paths and
the manifest's self-hash.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath

from bundle import canonical_json_bytes
from provenance import assert_path_neutral_tree
from schema import DatasetError


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _normalized_manifest(value: dict) -> dict:
    result = copy.deepcopy(value)
    result.pop("conversion_manifest_sha256", None)
    sessions = result.get("calibration_dataset_identity")
    if not isinstance(sessions, list):
        raise DatasetError("conversion manifest has no calibration identity")
    for session in sessions:
        if not isinstance(session, dict) or not isinstance(session.get("path"), str):
            raise DatasetError("conversion manifest has invalid calibration path")
        path = session["path"]
        session["path"] = PureWindowsPath(path).name if PureWindowsPath(
            path).is_absolute() else PurePosixPath(path).name
    return result


def rebind(evaluated_manifest_path: Path, published_manifest_path: Path,
           evaluated_acceptance_path: Path, output_path: Path) -> dict:
    evaluated_manifest_bytes = evaluated_manifest_path.read_bytes()
    published_manifest_bytes = published_manifest_path.read_bytes()
    evaluated_acceptance_bytes = evaluated_acceptance_path.read_bytes()
    evaluated_manifest = json.loads(evaluated_manifest_bytes.decode("utf-8"))
    published_manifest = json.loads(published_manifest_bytes.decode("utf-8"))
    acceptance = json.loads(evaluated_acceptance_bytes.decode("utf-8"))

    if _normalized_manifest(evaluated_manifest) != _normalized_manifest(
            published_manifest):
        raise DatasetError("conversion manifests differ beyond dataset paths/self-hash")
    old_identity = evaluated_manifest.get("conversion_manifest_sha256")
    new_identity = published_manifest.get("conversion_manifest_sha256")
    if not all(isinstance(value, str) and len(value) == 64
               for value in (old_identity, new_identity)):
        raise DatasetError("conversion manifest identity is missing")
    if acceptance.get("conversion_manifest_sha256") != old_identity or \
            acceptance.get("contract_core", {}).get(
                "conversion_manifest_sha256") != old_identity:
        raise DatasetError("evaluated acceptance is not bound to evaluated manifest")

    published = copy.deepcopy(acceptance)
    published["conversion_manifest_sha256"] = new_identity
    published["contract_core"]["conversion_manifest_sha256"] = new_identity
    core_hash = _sha256(canonical_json_bytes(published["contract_core"]))
    published["contract_core_sha256"] = core_hash
    published["held_out_test"]["contract_core_sha256"] = core_hash
    published["publication_note"] = (
        "Path-neutral provenance rebinding only; numerical acceptance was not rerun. "
        "Canonical model and NPU runtime hashes are unchanged.")
    assert_path_neutral_tree(published)
    output_bytes = (json.dumps(published, sort_keys=True, indent=2) + "\n").encode("utf-8")
    if output_path.exists():
        raise DatasetError(f"refusing to overwrite acceptance: {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(output_bytes)
    return {
        "operation": "path-only conversion-manifest identity rebinding",
        "numerical_acceptance_rerun": False,
        "canonical_model_sha256": published["canonical_full_int8_tflite_sha256"],
        "npu_runtime_binary_sha256": published["npu_runtime_binary_sha256"],
        "evaluated_conversion_manifest_file_sha256": _sha256(evaluated_manifest_bytes),
        "evaluated_conversion_manifest_identity_sha256": old_identity,
        "published_conversion_manifest_file_sha256": _sha256(published_manifest_bytes),
        "published_conversion_manifest_identity_sha256": new_identity,
        "evaluated_acceptance_sha256": _sha256(evaluated_acceptance_bytes),
        "published_acceptance_sha256": _sha256(output_bytes),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--evaluated-manifest", type=Path, required=True)
    result.add_argument("--published-manifest", type=Path, required=True)
    result.add_argument("--evaluated-acceptance", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--report", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
        report = rebind(args.evaluated_manifest, args.published_manifest,
                        args.evaluated_acceptance, args.output)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        if args.report.exists():
            raise DatasetError(f"refusing to overwrite report: {args.report}")
        args.report.write_bytes((json.dumps(report, sort_keys=True, indent=2) + "\n").encode(
            "utf-8"))
    except (DatasetError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"rebind-acceptance-provenance: {error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
