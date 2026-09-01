from __future__ import annotations

import csv
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from schema import HEADER, DatasetError, encode_row, parse_lines


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class Dataset:
    path: Path
    rows: list[dict]
    manifest: dict

    @property
    def target(self) -> int:
        return int(self.rows[0]["target"])

    @property
    def transport(self) -> int:
        return int(self.rows[0]["transport"])

    @property
    def session_id(self) -> str:
        value = self.manifest.get("session_id")
        if not isinstance(value, str) or not value:
            raise DatasetError(f"{self.path}: manifest has no session_id")
        return value

    @property
    def card_id(self) -> str:
        value = self.manifest.get("card_id", "unspecified")
        return str(value)

    @property
    def build_type(self) -> str:
        return str(self.rows[0]["build_type"])


def manifest_path(dataset_path: Path) -> Path:
    return dataset_path.with_suffix(dataset_path.suffix + ".manifest.json")


def load_dataset(path: str | Path, require_manifest: bool = True) -> Dataset:
    dataset_path = Path(path)
    with dataset_path.open("r", encoding="utf-8", newline="") as source:
        rows = parse_lines(source)
    sidecar = manifest_path(dataset_path)
    if sidecar.exists():
        manifest = json.loads(sidecar.read_text(encoding="utf-8"))
        expected = manifest.get("dataset_sha256")
        actual = sha256_file(dataset_path)
        if expected != actual:
            raise DatasetError(f"{dataset_path}: dataset SHA-256 mismatch")
        if manifest.get("row_count") != len(rows):
            raise DatasetError(f"{dataset_path}: manifest row count mismatch")
    elif require_manifest:
        raise DatasetError(f"{dataset_path}: dataset manifest is missing")
    else:
        manifest = {"session_id": dataset_path.stem, "card_id": "unspecified"}
    return Dataset(dataset_path, rows, manifest)


def write_dataset(path: Path, rows: list[dict]) -> None:
    with path.open("x", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(HEADER)
        for row in rows:
            values: list[object] = [HEADER[0]]
            for name in HEADER[1:]:
                value = row[name]
                values.append(":".join(str(v) for v in value)
                              if name == "histogram_r_w_s" else value)
            writer.writerow(values)


def usable_vectors(dataset: Dataset) -> tuple[list[list[int]], list[dict]]:
    vectors: list[list[int]] = []
    rows: list[dict] = []
    for row in dataset.rows:
        try:
            vector = encode_row(row)
        except DatasetError:
            continue
        vectors.append(vector)
        rows.append(row)
    return vectors, rows


def assert_same_profile(datasets: list[Dataset]) -> tuple[int, int]:
    if not datasets:
        raise DatasetError("no datasets supplied")
    targets = {dataset.target for dataset in datasets}
    transports = {dataset.transport for dataset in datasets}
    build_types = {dataset.build_type for dataset in datasets}
    if len(targets) != 1:
        raise DatasetError("mixed target datasets")
    if len(transports) != 1:
        raise DatasetError("mixed transport datasets")
    if len(build_types) != 1:
        raise DatasetError("mixed build_type datasets")
    sessions = [dataset.session_id for dataset in datasets]
    if len(sessions) != len(set(sessions)):
        raise DatasetError("duplicate session_id; session leakage is ambiguous")
    return next(iter(targets)), next(iter(transports))
