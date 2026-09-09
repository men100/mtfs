"""Publish one captured dataset without leaking its private source path.

CSV/NPY/NPZ/BIN payloads are copied byte-for-byte.  JSON capture sidecars are
rewritten only at ``source`` and the before/after hashes are emitted as a
machine-readable transformation record.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

from provenance import normalize_logical_path
from schema import DatasetError


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode(
        "utf-8")


def publish_capture(source: Path, destination: Path, logical_source: str) -> dict:
    source = source.resolve()
    sidecar = source.with_suffix(source.suffix + ".manifest.json")
    if not source.is_file() or not sidecar.is_file():
        raise DatasetError(f"capture or sidecar does not exist: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.with_suffix(
            destination.suffix + ".manifest.json").exists():
        raise DatasetError(f"refusing to overwrite published capture: {destination}")

    shutil.copyfile(source, destination)
    if _sha256(source) != _sha256(destination):
        raise DatasetError(f"payload copy changed bytes: {source}")

    sidecar_payload = json.loads(sidecar.read_text(encoding="utf-8"))
    if not isinstance(sidecar_payload, dict) or "source" not in sidecar_payload:
        raise DatasetError(f"capture sidecar has no source field: {sidecar}")
    original_source = sidecar_payload["source"]
    sidecar_payload["source"] = normalize_logical_path(logical_source)
    destination_sidecar = destination.with_suffix(destination.suffix + ".manifest.json")
    destination_sidecar.write_bytes(_json_bytes(sidecar_payload))

    return {
        "destination_name": destination.name,
        "payload": {
            "operation": "byte-for-byte copy",
            "source_sha256": _sha256(source),
            "published_sha256": _sha256(destination),
        },
        "sidecar": {
            "operation": "replace source path only; canonical JSON serialization",
            "original_source_value_sha256": hashlib.sha256(
                str(original_source).encode("utf-8")).hexdigest(),
            "published_source": sidecar_payload["source"],
            "source_sha256": _sha256(sidecar),
            "published_sha256": _sha256(destination_sidecar),
        },
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("source", type=Path)
    result.add_argument("destination", type=Path)
    result.add_argument("--logical-source", required=True)
    result.add_argument("--report", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
        report = publish_capture(args.source, args.destination, args.logical_source)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_bytes(_json_bytes(report))
    except (DatasetError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"publish-dataset: {error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
