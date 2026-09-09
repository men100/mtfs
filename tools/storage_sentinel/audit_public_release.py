"""Audit the public Storage Sentinel release boundary and write hash indexes."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from dataset import load_dataset
from provenance import assert_path_neutral_tree
from schema import DatasetError, canonical_json_sha256


EXPECTED_SCHEMA = "b5f6ffb42f5ca2a5aebd14e1becb4153253bf143b0e115594bae0078a2523ef0"
EXPECTED_CANONICAL_SCHEMA = "01b0040533491d3f2d07248b345909447be004d4fcd8fc94719ec28d955094d8"
EXPECTED_SESSIONS = 23
FORBIDDEN_TEXT = re.compile(
    r"(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\|/home/|/tmp/|AppData[\\/]|"
    r"github\.com/[^\s]+/private)", re.IGNORECASE)
TEXT_SUFFIXES = {".json", ".md", ".txt", ".log", ".csv"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2,
                               ensure_ascii=False) + "\n", encoding="utf-8")


def audit(release: Path, repo: Path) -> dict:
    datasets = []
    session_ids = set()
    for csv in sorted((release / "datasets").rglob("*.csv")):
        captured = load_dataset(csv)
        manifest_path = csv.with_suffix(csv.suffix + ".manifest.json")
        manifest = captured.manifest
        source = manifest.get("source")
        if not isinstance(source, str) or not source.startswith("source-captures/"):
            raise DatasetError(f"non-neutral capture source: {manifest_path}")
        if captured.session_id in session_ids:
            raise DatasetError(f"duplicate session ID: {captured.session_id}")
        session_ids.add(captured.session_id)
        relative = csv.relative_to(repo).as_posix()
        parts = csv.relative_to(release / "datasets").parts
        if len(parts) < 4:
            raise DatasetError(f"dataset does not preserve split/card boundary: {csv}")
        target, split, card = parts[:3]
        expected_card = {"card-a": "a", "card-b": "b", "card-c": "c",
                         "card-d": "d"}.get(card)
        if expected_card is None or f"card{expected_card}" not in csv.stem.replace("-", "").lower():
            raise DatasetError(f"dataset card/path mismatch: {csv}")
        datasets.append({
            "card_id": captured.card_id,
            "condition": manifest["capture_context"]["condition"],
            "dataset_role": manifest["capture_context"]["dataset_role"],
            "payload_sha256": sha256(csv),
            "path": relative,
            "published_sidecar_sha256": sha256(manifest_path),
            "session_id": captured.session_id,
            "split": split,
            "target": target,
            "usable_rows": len(captured.rows),
        })
    if len(datasets) != EXPECTED_SESSIONS:
        raise DatasetError(f"expected {EXPECTED_SESSIONS} sessions, found {len(datasets)}")

    reports = list((release / "audit" / "capture-transforms").rglob("*.json"))
    if len(reports) != EXPECTED_SESSIONS:
        raise DatasetError("capture transformation report count mismatch")
    for path in reports:
        report = json.loads(path.read_text(encoding="utf-8"))
        if report.get("payload", {}).get("source_sha256") != report.get(
                "payload", {}).get("published_sha256"):
            raise DatasetError(f"dataset payload was changed: {path}")

    evaluated_path = release / "reference/common/feature_schema_v1.evaluated.json"
    public_path = repo / "tools/storage_sentinel/feature_schema_v1.json"
    evaluated_bytes = evaluated_path.read_bytes()
    public_bytes = public_path.read_bytes()
    evaluated_json = json.loads(evaluated_bytes.decode("utf-8"))
    public_json = json.loads(public_bytes.decode("utf-8"))
    if sha256(evaluated_path) != EXPECTED_SCHEMA or evaluated_json != public_json:
        raise DatasetError("evaluated/public feature schema semantic mismatch")
    if canonical_json_sha256(evaluated_json) != EXPECTED_CANONICAL_SCHEMA or \
            canonical_json_sha256(public_json) != EXPECTED_CANONICAL_SCHEMA:
        raise DatasetError("feature schema canonical identity mismatch")

    prohibited_st_names = {"network_rel.bin", "SENTINEL.MTF", "sentinel.bundle"}
    st_model = release / "models/stm32n6570_dk"
    leaked = [path for path in st_model.rglob("*") if path.is_file() and
              path.name in prohibited_st_names]
    if leaked:
        raise DatasetError("ST generated binary crossed the public boundary")

    for path in release.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="strict")
        match = FORBIDDEN_TEXT.search(text)
        if match:
            raise DatasetError(f"machine/private path token in {path}: {match.group(0)}")
        if path.suffix.lower() == ".json":
            assert_path_neutral_tree(json.loads(text))

    dataset_index = {
        "format": "mtfs-storage-sentinel-public-dataset-index-v1",
        "row_repartitioning": False,
        "session_count": len(datasets),
        "sessions": datasets,
        "split_unit": "physical-card-and-session",
    }
    write_json(release / "dataset-index.json", dataset_index)
    return {
        "dataset_payloads_byte_identical_to_sources": True,
        "feature_schema_byte_sha256": EXPECTED_SCHEMA,
        "feature_schema_canonical_sha256": EXPECTED_CANONICAL_SCHEMA,
        "feature_schema_semantically_equal_to_public_schema": True,
        "forbidden_path_or_identity_tokens": 0,
        "public_st_generated_binaries": 0,
        "session_count": len(datasets),
        "status": "PASS",
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repo-root", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    repo = args.repo_root.resolve()
    release = repo / "artifacts/storage_sentinel"
    result = audit(release, repo)
    write_json(release / "audit/audit-summary.json", result)
    lines = []
    for path in sorted(item for item in release.rglob("*") if item.is_file() and
                       item.name != "SHA256SUMS"):
        lines.append(f"{sha256(path)}  {path.relative_to(release).as_posix()}")
    (release / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(result, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
