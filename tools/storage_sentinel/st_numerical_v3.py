"""Build and freeze the STM32 Neural-ART numerical backend contract v3.

The v3 contract keeps the application-facing common-Q4/score/decision boundary
separate from the vendor-specific raw int8 diagnostic guard.  Input corpora are
created and hashed before canonical expected outputs, and target outputs are not
accepted by this module until a freeze record exists.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import io
import json
import sys
import zipfile
from collections import Counter
from pathlib import Path
from typing import Iterable

import numpy as np

from acceptance import score_interval_q8
from canonical_int8 import (extract_tflite, requantize_int8_to_q4,
                            requantize_q4_to_int8, tflite_reference)
from dataset import sha256_file
from schema import DatasetError, canonical_json_bytes


VECTOR_SIZE = 24
SPEC_FORMAT = "mtfs-sentinel-neural-art-numerical-spec-v3"
CORE_FORMAT = "mtfs-sentinel-neural-art-acceptance-core-v3"
INPUT_INDEX_FORMAT = "mtfs-sentinel-neural-art-input-index-v3"
EXPECTED_INDEX_FORMAT = "mtfs-sentinel-neural-art-expected-index-v3"
GENERATOR_ID = "sha256-counter-operational-stress-v1"
SEED_DOMAIN = "microt-fs/storage-sentinel/st3.1/numerical-v3"
DEFAULT_OPERATIONAL_COUNT = 1200
DEFAULT_STRESS_COUNT = 1200


class HashStream:
    """Small counter-mode deterministic byte stream with an explicit identity."""

    def __init__(self, seed_hex: str):
        if len(seed_hex) != 64:
            raise ValueError("seed must be a SHA-256 hex digest")
        self._seed = bytes.fromhex(seed_hex)
        self._counter = 0
        self._buffer = bytearray()

    def take(self, count: int) -> bytes:
        while len(self._buffer) < count:
            self._buffer.extend(hashlib.sha256(
                self._seed + self._counter.to_bytes(8, "big")).digest())
            self._counter += 1
        result = bytes(self._buffer[:count])
        del self._buffer[:count]
        return result

    def below(self, upper: int) -> int:
        if upper <= 0:
            raise ValueError("random upper bound must be positive")
        return int.from_bytes(self.take(8), "big") % upper

    def signed(self, magnitude: int) -> int:
        return self.below(magnitude * 2 + 1) - magnitude


def _json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise DatasetError(f"{path}: JSON object required")
    return value


def _write_new(path: Path, content: bytes) -> None:
    if path.exists():
        raise DatasetError(f"refusing overwrite: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def _write_json(path: Path, value: dict) -> None:
    _write_new(path, canonical_json_bytes(value) + b"\n")


def _npy_bytes(values: np.ndarray) -> bytes:
    stream = io.BytesIO()
    np.save(stream, values, allow_pickle=False)
    return stream.getvalue()


def _npz_bytes(name: str, values: np.ndarray) -> bytes:
    payload = _npy_bytes(values)
    stream = io.BytesIO()
    info = zipfile.ZipInfo(f"{name}.npy", date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o600 << 16
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr(info, payload)
    return stream.getvalue()


def _load_npz(path: Path) -> np.ndarray:
    with np.load(path, allow_pickle=False) as archive:
        if archive.files != ["m_inputs_1"]:
            raise DatasetError(f"{path}: exactly m_inputs_1 is required")
        values = np.asarray(archive["m_inputs_1"])
    if values.dtype != np.int8 or values.ndim != 2 or values.shape[1] != VECTOR_SIZE:
        raise DatasetError(f"{path}: expected int8 (N, 24)")
    return values


def _load_npy(path: Path, samples: int | None = None) -> np.ndarray:
    values = np.asarray(np.load(path, allow_pickle=False))
    if values.dtype != np.int8 or values.ndim != 2 or values.shape[1] != VECTOR_SIZE:
        raise DatasetError(f"{path}: expected int8 (N, 24)")
    if samples is not None and values.shape[0] != samples:
        raise DatasetError(f"{path}: sample count mismatch")
    return values


def _decode_q4(value: str) -> np.ndarray:
    try:
        raw = base64.urlsafe_b64decode(value + "=" * ((4 - len(value) % 4) % 4))
    except (ValueError, TypeError) as error:
        raise DatasetError("invalid base64url common-Q4 vector") from error
    if len(raw) != VECTOR_SIZE:
        raise DatasetError("common-Q4 vector must contain exactly 24 bytes")
    return np.frombuffer(raw, dtype=np.int8).copy()


def canonical_identity(value: dict) -> str:
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def derive_seed(spec_identity: str, label: str) -> str:
    if len(spec_identity) != 64 or not label:
        raise ValueError("invalid seed identity")
    material = (SEED_DOMAIN.encode("ascii") + b"\0" + bytes.fromhex(spec_identity) +
                b"\0" + label.encode("ascii"))
    return hashlib.sha256(material).hexdigest()


def make_spec(canonical_path: Path, runtime_path: Path, manifest_path: Path,
              policy_path: Path, v2_core_path: Path, v2_result_path: Path) -> dict:
    manifest = _json(manifest_path)
    policy = _json(policy_path)
    v2_core = _json(v2_core_path)
    v2_result = _json(v2_result_path)
    canonical_hash = sha256_file(canonical_path)
    runtime_hash = sha256_file(runtime_path)
    if manifest.get("canonical_model_sha256") != canonical_hash or \
            manifest.get("runtime_binary_sha256") != runtime_hash:
        raise DatasetError("canonical model or Neural-ART runtime identity mismatch")
    if policy.get("preprocessing_contract_version") != 2 or \
            int(policy.get("active_feature_mask", 0)) == 0:
        raise DatasetError("baseline-relative preprocessing v2 policy required")
    if v2_core.get("contract_version") != 2 or \
            v2_result.get("status") != "STOP" or \
            v2_result.get("observed", {}).get("maximum_raw_output_error_int8") != 2:
        raise DatasetError("the immutable numerical contract v2 STOP evidence is missing")
    return {
        "format": SPEC_FORMAT,
        "contract_version": 3,
        "phase": "4.3C-ST.3.1",
        "status": "SPECIFIED-BEFORE-ACCEPTANCE-CORPUS",
        "classification_contract": "unchanged-from-ST.3",
        "training_quantization_compilation": "forbidden",
        "canonical_full_int8_tflite_sha256": canonical_hash,
        "npu_runtime_binary_sha256": runtime_hash,
        "conversion_manifest_sha256": manifest["conversion_manifest_sha256"],
        "runtime_memory_regions": manifest["memory_regions"],
        "preprocessing_policy_sha256": policy["canonical_sha256"],
        "active_feature_mask": int(policy["active_feature_mask"]),
        "threshold_q8": 1,
        "v2_stop": {
            "contract_core_sha256": canonical_identity(v2_core),
            "result_file_sha256": sha256_file(v2_result_path),
            "status": "STOP-IMMUTABLE",
            "observed_raw_int8_maximum_error": 2,
            "frozen_raw_int8_maximum_error": 1,
        },
        "responsibility_boundary": {
            "vendor_raw_int8": "Neural-ART backend-specific diagnostic guard",
            "common_q4": "normative microT-FS Sentinel runtime output boundary",
            "score_decision": "normative application-visible result",
        },
        "fixed_limits": {
            "maximum_vendor_raw_int8_error": 2,
            "maximum_common_q4_output_error": 1,
            "score_requirement": "sample-specific interval",
            "decision_disagreements": 0,
            "repeatability_failures": 0,
            "invoke_failures": 0,
            "target_airunner_mismatches": 0,
        },
        "decision_policy": {
            "threshold_q8": 1,
            "definitely_normal": "score_max_q8 <= threshold_q8",
            "definitely_anomaly": "score_min_q8 > threshold_q8",
            "ambiguous": "otherwise",
            "ambiguous_action": "canonical CPU arbitration",
        },
        "score_interval": {
            "version": 1,
            "candidate_component_interval": "[max(-128,y_i-e),min(127,y_i+e)]",
            "rounding": "(sum + 12) // 24",
            "candidate_score_requirement": "score_min_q8 <= score_q8 <= score_max_q8",
        },
        "acceptance_corpus": {
            "generator": GENERATOR_ID,
            "minimum_operational_vectors": 1000,
            "minimum_stress_vectors": 1000,
            "known_or_cross_corpus_duplicates": 0,
            "seed_derivation": "SHA-256(spec canonical identity, fixed domain label)",
            "post_npu_changes": "forbidden",
        },
        "toolchain": {
            "stedgeai": manifest["tool_versions"]["stedgeai"],
            "runtime_variant": manifest["runtime_variant"],
            "support": "optimized-Release-SDMMC2-4BIT-IDMA-IRQ-only",
        },
    }


def _vendor_from_q4(model, values: np.ndarray) -> np.ndarray:
    return np.asarray([[requantize_q4_to_int8(int(item), model.input_scale,
                                              model.input_zero_point)
                        for item in row] for row in values], dtype=np.int8)


def _q4_from_vendor(model, values: np.ndarray) -> np.ndarray:
    return np.asarray([[requantize_int8_to_q4(int(item), model.input_scale,
                                              model.input_zero_point)
                        for item in row] for row in values], dtype=np.int8)


def _active_indices(mask: int) -> list[int]:
    active = [index for index in range(VECTOR_SIZE) if mask & (1 << index)]
    if not active:
        raise DatasetError("active feature mask is empty")
    return active


def _anchors(golden: dict, card_d_q4: np.ndarray) -> dict[str, list[np.ndarray]]:
    pools: dict[str, list[np.ndarray]] = {name: [] for name in
        ("natural", "light", "medium", "strong", "threshold")}
    threshold = int(golden["threshold_q8"])
    for row in golden.get("normal", []):
        value = np.asarray(row["input_q4"], dtype=np.int8)
        pools["natural"].append(value)
        if abs(int(row["score_q8"]) - threshold) <= 1:
            pools["threshold"].append(value)
    for row in golden.get("pseudo", []):
        stage = str(row.get("stage", ""))
        value = np.asarray(row["input_q4"], dtype=np.int8)
        if stage in pools:
            pools[stage].append(value)
        if abs(int(row["score_q8"]) - threshold) <= 1:
            pools["threshold"].append(value)
    pools["natural"].extend(np.asarray(row, dtype=np.int8) for row in card_d_q4)
    for name, values in pools.items():
        if not values:
            raise DatasetError(f"no operational anchors for {name}")
    return pools


def _mutated_anchor(stream: HashStream, pool: list[np.ndarray], active: list[int],
                    magnitude: int) -> np.ndarray:
    row = pool[stream.below(len(pool))].copy().astype(np.int16)
    mutations = 1 + stream.below(min(4, len(active)))
    for _ in range(mutations):
        feature = active[stream.below(len(active))]
        delta = 0
        while delta == 0:
            delta = stream.signed(magnitude)
        row[feature] = max(-125, min(125, int(row[feature]) + delta))
    return row.astype(np.int8)


def _operational_candidate(category: str, stream: HashStream,
                           pools: dict[str, list[np.ndarray]],
                           active: list[int]) -> np.ndarray:
    if category == "natural-near":
        return _mutated_anchor(stream, pools["natural"], active, 2)
    if category.startswith("pseudo-"):
        stage = category.split("-")[1]
        return _mutated_anchor(stream, pools[stage], active, {"light": 4,
            "medium": 8, "strong": 12}[stage])
    if category == "threshold-near":
        return _mutated_anchor(stream, pools["threshold"], active, 2)
    row = np.zeros(VECTOR_SIZE, dtype=np.int8)
    feature = active[stream.below(len(active))]
    if category == "saturation-preboundary":
        row[feature] = 126 if stream.below(2) else -126
    elif category == "ood-preboundary":
        row[feature] = 127 if stream.below(2) else -127
        second = active[stream.below(len(active))]
        if second != feature:
            row[second] = 1 if stream.below(2) else -1
    else:
        raise DatasetError(f"unknown operational category: {category}")
    for _ in range(1 + stream.below(3)):
        companion = active[stream.below(len(active))]
        if companion != feature:
            row[companion] = stream.signed(8)
    return row


def _unique_operational(model, count: int, seed: str, known: set[bytes],
                        pools: dict[str, list[np.ndarray]], active: list[int]) \
        -> tuple[np.ndarray, np.ndarray, list[str]]:
    categories = ["natural-near", "pseudo-light-near", "pseudo-medium-near",
                  "pseudo-strong-near", "threshold-near",
                  "saturation-preboundary", "ood-preboundary"]
    stream = HashStream(seed)
    rows: list[np.ndarray] = []
    vendor_rows: list[np.ndarray] = []
    labels: list[str] = []
    used = set(known)
    for index in range(count):
        category = categories[index % len(categories)]
        for _ in range(100000):
            q4 = _operational_candidate(category, stream, pools, active)
            inactive = [item for item in range(VECTOR_SIZE) if item not in active]
            q4[inactive] = 0
            vendor = _vendor_from_q4(model, q4.reshape(1, VECTOR_SIZE))[0]
            identity = vendor.tobytes()
            if identity not in used:
                used.add(identity)
                rows.append(q4)
                vendor_rows.append(vendor)
                labels.append(category)
                break
        else:
            raise DatasetError(f"unable to create unique operational vector {index}")
    return (np.asarray(vendor_rows, dtype=np.int8),
            np.asarray(rows, dtype=np.int8), labels)


def _unique_stress(model, count: int, seed: str, excluded: set[bytes],
                   active: list[int]) -> tuple[np.ndarray, np.ndarray, list[str]]:
    stream = HashStream(seed)
    base = np.full(VECTOR_SIZE, model.input_zero_point, dtype=np.int8)
    candidates: list[tuple[np.ndarray, str]] = []
    for feature in active:
        for value in (-128, 127, 0, -1, 1):
            row = base.copy()
            row[feature] = value
            candidates.append((row, "active-feature-min-max-zero-pm1"))
        for delta in (-2, -1, 1, 2):
            row = base.copy().astype(np.int16)
            row[feature] = max(-128, min(127, model.input_zero_point + delta))
            candidates.append((row.astype(np.int8), "single-feature-perturbation"))
    for first_index, first in enumerate(active):
        for second in active[first_index + 1:]:
            row = base.copy()
            row[first] = -128 if (first + second) & 1 else 127
            row[second] = 127 if (first + second) & 1 else -128
            candidates.append((row, "multiple-feature-combination"))

    rows: list[np.ndarray] = []
    labels: list[str] = []
    used = set(excluded)

    def accept(row: np.ndarray, label: str) -> None:
        identity = row.tobytes()
        if identity not in used and len(rows) < count:
            used.add(identity)
            rows.append(row.copy())
            labels.append(label)

    for row, label in candidates:
        accept(row, label)
    while len(rows) < count:
        row = np.frombuffer(stream.take(VECTOR_SIZE), dtype=np.uint8).view(np.int8).copy()
        accept(row, "deterministic-pseudo-random")
    vendor = np.asarray(rows, dtype=np.int8)
    return vendor, _q4_from_vendor(model, vendor), labels


def _file_entry(path: Path) -> dict:
    return {"sha256": sha256_file(path), "bytes": path.stat().st_size}


def build_inputs(spec_path: Path, canonical_path: Path, golden_path: Path,
                 card_c_inputs_path: Path, card_d_inputs_path: Path,
                 card_d_q4_path: Path, legacy_repro: str, threshold_ambiguous: str,
                 output: Path, operational_count: int, stress_count: int) -> dict:
    if operational_count < 1000 or stress_count < 1000:
        raise DatasetError("operational and stress corpora each require at least 1000 vectors")
    spec = _json(spec_path)
    if spec.get("format") != SPEC_FORMAT or spec.get("status") != \
            "SPECIFIED-BEFORE-ACCEPTANCE-CORPUS":
        raise DatasetError("numerical v3 specification required")
    model = extract_tflite(canonical_path)
    if model.canonical_sha256.hex() != spec["canonical_full_int8_tflite_sha256"]:
        raise DatasetError("canonical model differs from specification")
    golden = _json(golden_path)
    card_c = _load_npz(card_c_inputs_path)
    card_d = _load_npz(card_d_inputs_path)
    card_d_q4 = _load_npy(card_d_q4_path, card_d.shape[0])
    mandatory_q4 = np.asarray([_decode_q4(legacy_repro),
                               _decode_q4(threshold_ambiguous)], dtype=np.int8)
    mandatory_vendor = _vendor_from_q4(model, mandatory_q4)
    characterization = np.concatenate((card_c, card_d, mandatory_vendor), axis=0)
    characterization_q4 = np.concatenate((
        np.asarray([row["input_q4"] for row in
                    golden.get("normal", []) + golden.get("pseudo", [])], dtype=np.int8),
        card_d_q4, mandatory_q4), axis=0)
    if characterization.shape != characterization_q4.shape:
        raise DatasetError("characterization input/common-Q4 shape mismatch")
    known = {row.tobytes() for row in characterization}
    spec_identity = canonical_identity(spec)
    operational_seed = derive_seed(spec_identity, "operational")
    stress_seed = derive_seed(spec_identity, "stress")
    active = _active_indices(int(spec["active_feature_mask"]))
    pools = _anchors(golden, card_d_q4)
    operational, operational_q4, operational_labels = _unique_operational(
        model, operational_count, operational_seed, known, pools, active)
    stress, stress_q4, stress_labels = _unique_stress(
        model, stress_count, stress_seed,
        known | {row.tobytes() for row in operational}, active)

    if len({row.tobytes() for row in operational}) != operational_count or \
            len({row.tobytes() for row in stress}) != stress_count or \
            any(row.tobytes() in known for row in operational) or \
            any(row.tobytes() in known for row in stress) or \
            set(row.tobytes() for row in operational) & set(row.tobytes() for row in stress):
        raise DatasetError("acceptance corpus overlap or duplication")

    files = {
        "characterization_inputs_int8.npz": _npz_bytes("m_inputs_1", characterization),
        "characterization_input_q4.npy": _npy_bytes(characterization_q4),
        "operational_inputs_int8.npz": _npz_bytes("m_inputs_1", operational),
        "operational_input_q4.npy": _npy_bytes(operational_q4),
        "stress_inputs_int8.npz": _npz_bytes("m_inputs_1", stress),
        "stress_input_q4.npy": _npy_bytes(stress_q4),
    }
    for name, content in files.items():
        _write_new(output / name, content)
    metadata = {
        "format": INPUT_INDEX_FORMAT,
        "status": "INPUTS-FROZEN-BEFORE-EXPECTED-OUTPUTS",
        "specification_canonical_sha256": spec_identity,
        "specification_file_sha256": sha256_file(spec_path),
        "generator": GENERATOR_ID,
        "seeds": {"operational": operational_seed, "stress": stress_seed},
        "active_features": active,
        "characterization": {
            "samples": int(characterization.shape[0]),
            "card_c_samples": int(card_c.shape[0]),
            "card_d_samples": int(card_d.shape[0]),
            "mandatory_regression": {
                "card_d_raw_error_2_samples": [166, 167],
                "legacy_st1_1_lsb_index": int(card_c.shape[0] + card_d.shape[0]),
                "threshold_ambiguous_index": int(card_c.shape[0] + card_d.shape[0] + 1),
                "legacy_st1_common_q4_base64url": legacy_repro,
                "threshold_ambiguous_common_q4_base64url": threshold_ambiguous,
            },
        },
        "operational": {
            "samples": operational_count,
            "categories": dict(sorted(Counter(operational_labels).items())),
            "baseline_relative_v2_reachable": True,
            "classification_accuracy_for_ood": "not-asserted",
        },
        "stress": {
            "samples": stress_count,
            "categories": dict(sorted(Counter(stress_labels).items())),
            "classification_accuracy": "not-asserted",
        },
        "overlap": {
            "within_operational": 0,
            "within_stress": 0,
            "operational_vs_characterization": 0,
            "stress_vs_characterization": 0,
            "operational_vs_stress": 0,
        },
        "source_files": {
            "golden": _file_entry(golden_path),
            "card_c_inputs": _file_entry(card_c_inputs_path),
            "card_d_inputs": _file_entry(card_d_inputs_path),
            "card_d_common_q4": _file_entry(card_d_q4_path),
        },
        "files": {name: _file_entry(output / name) for name in sorted(files)},
    }
    _write_json(output / "input_index.json", metadata)
    return metadata


def _score(input_q4: np.ndarray, output_q4: np.ndarray) -> int:
    difference = input_q4.astype(np.int16) - output_q4.astype(np.int16)
    return (sum(int(value) * int(value) for value in difference) + 12) // 24


def build_expected(spec_path: Path, input_index_path: Path, canonical_path: Path,
                   output: Path) -> dict:
    spec = _json(spec_path)
    inputs_index = _json(input_index_path)
    if inputs_index.get("format") != INPUT_INDEX_FORMAT or \
            inputs_index.get("specification_canonical_sha256") != canonical_identity(spec):
        raise DatasetError("input index does not match specification")
    model = extract_tflite(canonical_path)
    if model.canonical_sha256.hex() != spec["canonical_full_int8_tflite_sha256"]:
        raise DatasetError("canonical model differs from specification")
    generated = {}
    corpus_reports = {}
    for corpus in ("characterization", "operational", "stress"):
        inputs = _load_npz(input_index_path.parent / f"{corpus}_inputs_int8.npz")
        input_q4 = _load_npy(input_index_path.parent / f"{corpus}_input_q4.npy",
                             inputs.shape[0])
        tflite = tflite_reference(canonical_path, inputs)
        cpu = np.asarray([model.infer_raw(row) for row in inputs], dtype=np.int8)
        if not np.array_equal(tflite, cpu):
            locations = np.argwhere(tflite != cpu)
            raise DatasetError(f"CPU/TFLite mismatch in {corpus}: {locations[0].tolist()}")
        output_q4 = _q4_from_vendor(model, tflite)
        scores = np.asarray([_score(left, right) for left, right in
                             zip(input_q4, output_q4)], dtype=np.uint64)
        intervals = np.asarray([[interval.score_min_q8, interval.score_max_q8]
            for interval in (score_interval_q8(left.tolist(), right.tolist(), 1,
                                               int(spec["threshold_q8"]))
                             for left, right in zip(input_q4, output_q4))],
            dtype=np.uint64)
        decisions = scores > int(spec["threshold_q8"])
        values = {
            f"{corpus}_expected_raw_int8.npy": tflite,
            f"{corpus}_expected_q4.npy": output_q4,
            f"{corpus}_expected_score_q8.npy": scores,
            f"{corpus}_score_interval_q8.npy": intervals,
            f"{corpus}_expected_decision.npy": decisions,
        }
        for name, value in values.items():
            _write_new(output / name, _npy_bytes(value))
            generated[name] = _file_entry(output / name)
        corpus_reports[corpus] = {
            "samples": int(inputs.shape[0]),
            "ambiguous_intervals": int(np.count_nonzero(
                (intervals[:, 0] <= int(spec["threshold_q8"])) &
                (intervals[:, 1] > int(spec["threshold_q8"])))),
            "minimum_score_q8": int(scores.min()),
            "maximum_score_q8": int(scores.max()),
        }
    report = {
        "format": EXPECTED_INDEX_FORMAT,
        "status": "CANONICAL-EXPECTED-FROZEN-BEFORE-NPU",
        "specification_canonical_sha256": canonical_identity(spec),
        "input_index_file_sha256": sha256_file(input_index_path),
        "canonical_full_int8_tflite_sha256": sha256_file(canonical_path),
        "reference": "TensorFlow Lite BUILTIN_REF and canonical CPU int8 bit-exact",
        "corpora": corpus_reports,
        "files": generated,
    }
    _write_json(output / "expected_index.json", report)
    return report


def freeze(spec_path: Path, input_index_path: Path, expected_index_path: Path,
           output: Path) -> dict:
    spec = _json(spec_path)
    inputs = _json(input_index_path)
    expected = _json(expected_index_path)
    identity = canonical_identity(spec)
    if spec.get("format") != SPEC_FORMAT or inputs.get("format") != INPUT_INDEX_FORMAT or \
            expected.get("format") != EXPECTED_INDEX_FORMAT or \
            inputs.get("specification_canonical_sha256") != identity or \
            expected.get("specification_canonical_sha256") != identity or \
            expected.get("input_index_file_sha256") != sha256_file(input_index_path):
        raise DatasetError("v3 freeze input identity mismatch")
    if inputs.get("overlap") != {
            "within_operational": 0, "within_stress": 0,
            "operational_vs_characterization": 0,
            "stress_vs_characterization": 0, "operational_vs_stress": 0}:
        raise DatasetError("acceptance corpus overlap is nonzero")
    if inputs["operational"]["samples"] < 1000 or inputs["stress"]["samples"] < 1000:
        raise DatasetError("acceptance corpus is too small")
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    core = {
        "format": CORE_FORMAT,
        "contract_version": 3,
        "status": "FROZEN",
        "frozen_at_utc": now,
        "specification": spec,
        "specification_canonical_sha256": identity,
        "fixed_limits": spec["fixed_limits"],
        "responsibility_boundary": spec["responsibility_boundary"],
        "decision_policy": spec["decision_policy"],
        "score_interval": spec["score_interval"],
        "input_index_file_sha256": sha256_file(input_index_path),
        "expected_index_file_sha256": sha256_file(expected_index_path),
        "input_files": inputs["files"],
        "expected_files": expected["files"],
        "corpora": {
            "characterization": inputs["characterization"],
            "operational": inputs["operational"],
            "stress": inputs["stress"],
            "overlap": inputs["overlap"],
            "seeds": inputs["seeds"],
        },
        "post_npu_change_policy": "contract/corpus/seed/input/limit/interval changes forbidden",
    }
    core_path = output / "acceptance_contract_v3_core.json"
    _write_json(core_path, core)
    core_hash = canonical_identity(core)
    record = {
        "format": "mtfs-sentinel-neural-art-freeze-record-v3",
        "status": "FROZEN-BEFORE-NPU-ACCEPTANCE",
        "frozen_at_utc": now,
        "contract_core_canonical_sha256": core_hash,
        "contract_core_file_sha256": sha256_file(core_path),
        "specification_file_sha256": sha256_file(spec_path),
        "input_index_file_sha256": sha256_file(input_index_path),
        "expected_index_file_sha256": sha256_file(expected_index_path),
        "npu_outputs_observed": False,
    }
    _write_json(output / "freeze_record_v3.json", record)
    return {**record, "freeze_record_file_sha256":
            sha256_file(output / "freeze_record_v3.json")}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    spec = commands.add_parser("spec")
    spec.add_argument("--canonical-tflite", type=Path, required=True)
    spec.add_argument("--runtime", type=Path, required=True)
    spec.add_argument("--runtime-manifest", type=Path, required=True)
    spec.add_argument("--policy", type=Path, required=True)
    spec.add_argument("--v2-contract-core", type=Path, required=True)
    spec.add_argument("--v2-stop-result", type=Path, required=True)
    spec.add_argument("--output", type=Path, required=True)
    inputs = commands.add_parser("inputs")
    inputs.add_argument("--spec", type=Path, required=True)
    inputs.add_argument("--canonical-tflite", type=Path, required=True)
    inputs.add_argument("--golden", type=Path, required=True)
    inputs.add_argument("--card-c-inputs", type=Path, required=True)
    inputs.add_argument("--card-d-inputs", type=Path, required=True)
    inputs.add_argument("--card-d-common-q4", type=Path, required=True)
    inputs.add_argument("--legacy-repro", required=True)
    inputs.add_argument("--threshold-ambiguous", required=True)
    inputs.add_argument("--operational-count", type=int, default=DEFAULT_OPERATIONAL_COUNT)
    inputs.add_argument("--stress-count", type=int, default=DEFAULT_STRESS_COUNT)
    inputs.add_argument("--output-dir", type=Path, required=True)
    expected = commands.add_parser("expected")
    expected.add_argument("--spec", type=Path, required=True)
    expected.add_argument("--input-index", type=Path, required=True)
    expected.add_argument("--canonical-tflite", type=Path, required=True)
    expected.add_argument("--output-dir", type=Path, required=True)
    frozen = commands.add_parser("freeze")
    frozen.add_argument("--spec", type=Path, required=True)
    frozen.add_argument("--input-index", type=Path, required=True)
    frozen.add_argument("--expected-index", type=Path, required=True)
    frozen.add_argument("--output-dir", type=Path, required=True)
    return result


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "spec":
            value = make_spec(args.canonical_tflite.resolve(), args.runtime.resolve(),
                args.runtime_manifest.resolve(), args.policy.resolve(),
                args.v2_contract_core.resolve(), args.v2_stop_result.resolve())
            _write_json(args.output.resolve(), value)
        elif args.command == "inputs":
            value = build_inputs(args.spec.resolve(), args.canonical_tflite.resolve(),
                args.golden.resolve(), args.card_c_inputs.resolve(),
                args.card_d_inputs.resolve(), args.card_d_common_q4.resolve(),
                args.legacy_repro, args.threshold_ambiguous,
                args.output_dir.resolve(), args.operational_count, args.stress_count)
        elif args.command == "expected":
            value = build_expected(args.spec.resolve(), args.input_index.resolve(),
                args.canonical_tflite.resolve(), args.output_dir.resolve())
        else:
            value = freeze(args.spec.resolve(), args.input_index.resolve(),
                args.expected_index.resolve(), args.output_dir.resolve())
        print(json.dumps(value, indent=2, sort_keys=True))
        return 0
    except (DatasetError, OSError, ValueError, KeyError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
