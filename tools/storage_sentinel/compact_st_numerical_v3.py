"""Create the bounded authenticated summary of a full ST numerical v3 report."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from bundle import BundleError, _validate_npu_acceptance
from schema import DatasetError, canonical_json_bytes


FORMAT = "mtfs-sentinel-neural-art-acceptance-v3-compact"
RESULT_FIELDS = (
    "status", "vectors", "raw_elements", "raw_error_elements",
    "maximum_vendor_raw_int8_error", "maximum_raw_error_by_output_index",
    "maximum_common_q4_output_error", "maximum_score_error_q8",
    "score_interval_violations", "ambiguous_cpu_arbitrations",
    "direct_npu_decision_disagreements", "decision_disagreements",
    "repeatability_failures", "invoke_failures",
    "target_airunner_mismatches",
)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def _compact_result(result: object) -> dict:
    if not isinstance(result, dict) or any(name not in result
                                           for name in RESULT_FIELDS):
        raise DatasetError("full acceptance result is incomplete")
    return {name: result[name] for name in RESULT_FIELDS}


def compact_acceptance(full: dict, full_file_sha256: str) -> dict:
    core = full.get("contract_core")
    held_out = full.get("held_out_test")
    if full.get("format") != "mtfs-sentinel-neural-art-acceptance-v3" or \
            not isinstance(core, dict) or not isinstance(held_out, dict):
        raise DatasetError("not a full ST numerical v3 acceptance report")
    specification = core.get("specification")
    corpora = core.get("corpora")
    if not isinstance(specification, dict) or not isinstance(corpora, dict):
        raise DatasetError("full acceptance contract is incomplete")
    summary = {
        "contract_version": core.get("contract_version"),
        "canonical_full_int8_tflite_sha256":
            specification.get("canonical_full_int8_tflite_sha256"),
        "npu_runtime_binary_sha256":
            specification.get("npu_runtime_binary_sha256"),
        "conversion_manifest_sha256":
            specification.get("conversion_manifest_sha256"),
        "fixed_limits": core.get("fixed_limits"),
        "responsibility_boundary": core.get("responsibility_boundary"),
        "score_interval": core.get("score_interval"),
        "decision_policy": core.get("decision_policy"),
        "input_index_file_sha256": core.get("input_index_file_sha256"),
        "expected_index_file_sha256": core.get("expected_index_file_sha256"),
        "corpora": {
            "characterization": {
                "samples": corpora.get("characterization", {}).get("samples")},
            "operational": {
                "samples": corpora.get("operational", {}).get("samples")},
            "stress": {"samples": corpora.get("stress", {}).get("samples")},
            "overlap": corpora.get("overlap"),
            "seeds": corpora.get("seeds"),
        },
    }
    compact = {
        "format": FORMAT,
        "status": full.get("status"),
        "canonical_full_int8_tflite_sha256":
            full.get("canonical_full_int8_tflite_sha256"),
        "npu_runtime_binary_sha256":
            full.get("npu_runtime_binary_sha256"),
        "conversion_manifest_sha256":
            full.get("conversion_manifest_sha256"),
        "contract_core_sha256": full.get("contract_core_sha256"),
        "full_acceptance_file_sha256": full_file_sha256,
        "contract_summary": summary,
        "characterization": _compact_result(full.get("characterization")),
        "held_out_test": {
            "contract_core_sha256": held_out.get("contract_core_sha256"),
            "vectors": held_out.get("vectors"),
            "violations": held_out.get("violations"),
            "operational": _compact_result(held_out.get("operational")),
            "stress": _compact_result(held_out.get("stress")),
        },
    }
    return compact


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        input_path = args.input.resolve()
        output_path = args.output.resolve()
        if output_path.exists():
            raise DatasetError(f"refusing overwrite: {output_path}")
        full = json.loads(input_path.read_text(encoding="utf-8"))
        compact = compact_acceptance(full, _sha256_file(input_path))
        canonical = bytes.fromhex(compact[
            "canonical_full_int8_tflite_sha256"])
        npu_info = {
            "runtime_binary_sha256": compact["npu_runtime_binary_sha256"],
            "conversion_manifest_sha256":
                compact["conversion_manifest_sha256"],
        }
        threshold = compact["contract_summary"]["decision_policy"][
            "threshold_q8"]
        _validate_npu_acceptance(compact, canonical, npu_info, threshold)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(canonical_json_bytes(compact) + b"\n")
        print(json.dumps({"status": "PASS", "output": str(output_path),
                          "bytes": output_path.stat().st_size,
                          "sha256": _sha256_file(output_path)},
                         sort_keys=True))
        return 0
    except (BundleError, DatasetError, OSError, ValueError, KeyError,
            TypeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
