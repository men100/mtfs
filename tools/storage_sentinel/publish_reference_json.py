"""Create an audited path-neutral copy of a JSON reference artifact."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath

from provenance import assert_path_neutral_tree
from schema import DatasetError


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _published_dataset_index(repo_root: Path) -> dict[str, str]:
    dataset_root = repo_root / "artifacts" / "storage_sentinel" / "datasets"
    result: dict[str, str] = {}
    for path in dataset_root.rglob("*.csv"):
        if path.name in result:
            raise DatasetError(f"ambiguous published dataset basename: {path.name}")
        result[path.name] = path.relative_to(repo_root).as_posix()
    return result


def publish_reference(source: Path, destination: Path, repo_root: Path) -> dict:
    source_bytes = source.read_bytes()
    payload = json.loads(source_bytes.decode("utf-8"))
    dataset_index = _published_dataset_index(repo_root.resolve())
    replacements: list[dict] = []

    def visit(value: object, location: str) -> object:
        if isinstance(value, dict):
            return {key: visit(child, f"{location}.{key}")
                    for key, child in value.items()}
        if isinstance(value, list):
            return [visit(child, f"{location}[{index}]")
                    for index, child in enumerate(value)]
        if isinstance(value, str) and PureWindowsPath(value).is_absolute():
            basename = PureWindowsPath(value).name
            replacement = dataset_index.get(basename)
            if replacement is None:
                raise DatasetError(
                    f"absolute path is not a published dataset at {location}")
            replacements.append({
                "location": location,
                "original_value_sha256": _sha256_bytes(value.encode("utf-8")),
                "published_value": replacement,
            })
            return replacement
        return value

    published = visit(payload, "root")
    assert_path_neutral_tree(published)
    output_bytes = (json.dumps(published, sort_keys=True, indent=2,
                               ensure_ascii=False) + "\n").encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise DatasetError(f"refusing to overwrite reference: {destination}")
    destination.write_bytes(output_bytes)
    return {
        "operation": "replace absolute dataset paths; canonical JSON serialization",
        "source_name": source.name,
        "source_sha256": _sha256_bytes(source_bytes),
        "published_name": destination.name,
        "published_sha256": _sha256_bytes(output_bytes),
        "replacements": replacements,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("source", type=Path)
    result.add_argument("destination", type=Path)
    result.add_argument("--repo-root", type=Path, required=True)
    result.add_argument("--report", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        args = parser().parse_args(argv)
        report = publish_reference(args.source, args.destination, args.repo_root)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        if args.report.exists():
            raise DatasetError(f"refusing to overwrite report: {args.report}")
        args.report.write_bytes((json.dumps(report, sort_keys=True, indent=2) + "\n").encode(
            "utf-8"))
    except (DatasetError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"publish-reference-json: {error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
