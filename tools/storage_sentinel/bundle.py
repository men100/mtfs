from __future__ import annotations

import hashlib
import json
import math
import os
import struct
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np

from dataset import load_dataset, sha256_file, usable_vectors
from model import DenseAutoencoder
from schema import (DatasetError, canonical_json_bytes, canonical_json_sha256,
                    normalize, schema_canonical_hash)
from version import TOOL_VERSION

MAGIC = b"MTFSSB1\0"
BUNDLE_VERSION = 1
HEADER_SIZE = 32
DIRECTORY_SIZE = 32
MAX_SIZE = 1024 * 1024
MAX_SECTIONS = 16
SECTION_REQUIRED = 1
SECTION_COMPATIBILITY = 1
SECTION_NORMALIZATION = 2
SECTION_DECISION = 3
SECTION_PROVENANCE = 4
SECTION_CPU = 0x0100
SECTION_NPU = 0x0101
MODEL_FORMAT_SENTINEL_BUNDLE_V1 = 0x534E5431
ACCELERATOR_CPU_REFERENCE = 0x43505520
PROVIDER_CPU_REFERENCE = 0x43505552
MODEL_FORMAT_CPU_INT8_V1 = 0x51414531
TOPOLOGY_ID = 0x180C040C
TARGET_RA = 0x52413850
TARGET_ST = 0x53544E36
TRANSPORT_SPI = 0x53504920
TRANSPORT_IDMA = 0x49444D41
SCHEMA_ID = "mtfs-storage-sentinel-model-input"
SCHEMA_HASH = bytes.fromhex(
    "01b0040533491d3f2d07248b345909447be004d4fcd8fc94719ec28d955094d8")
DIMENSIONS = (24, 12, 4, 12, 24)
RUNTIME_DESCRIPTOR_SIZE = 192
RUNTIME_REGION_ENTRY_SIZE = 64
RUNTIME_REGION_ENTRY_VERSION = 1
RUNTIME_REGION_FLAG_REQUIRED = 1
REGION_EXECUTABLE_COPY = 1
REGION_ACTIVATION = 2
REGION_PARAMETERS = 3
REGION_EXTERNAL_RW = 4
REGION_PROVIDER_CONTEXT = 5
PLACEMENT_CALLER_RELATIVE = 1
PLACEMENT_FIXED_ABSOLUTE = 2
PLACEMENT_BINARY_CONTAINED = 3
PLACEMENT_PROVIDER_ASSIGNED = 4
REGION_ACCESS_READ = 1
REGION_ACCESS_WRITE = 2
REGION_ACCESS_EXECUTE = 4
REGION_LIFETIME_INSTALL = 1
REGION_LIFETIME_INSTANCE = 2
REGION_LIFETIME_INFERENCE = 3
REGION_REQUIRE_ZEROIZE = 1
REGION_REQUIRE_CACHE_COHERENCY = 2
REGION_REQUIRE_EXCLUSIVE = 4
REGION_REQUIRE_SHAREABLE = 8
REGION_REQUIRE_INPUT_OUTPUT_SHARED = 16
_REGION_KINDS = {REGION_EXECUTABLE_COPY, REGION_ACTIVATION, REGION_PARAMETERS,
                 REGION_EXTERNAL_RW, REGION_PROVIDER_CONTEXT}
_REGION_PLACEMENTS = {PLACEMENT_CALLER_RELATIVE, PLACEMENT_FIXED_ABSOLUTE,
                      PLACEMENT_BINARY_CONTAINED, PLACEMENT_PROVIDER_ASSIGNED}
_REGION_LIFETIMES = {REGION_LIFETIME_INSTALL, REGION_LIFETIME_INSTANCE,
                     REGION_LIFETIME_INFERENCE}
_REGION_ACCESS_MASK = REGION_ACCESS_READ | REGION_ACCESS_WRITE | REGION_ACCESS_EXECUTE
_REGION_REQUIREMENT_MASK = (REGION_REQUIRE_ZEROIZE | REGION_REQUIRE_CACHE_COHERENCY |
                            REGION_REQUIRE_EXCLUSIVE | REGION_REQUIRE_SHAREABLE |
                            REGION_REQUIRE_INPUT_OUTPUT_SHARED)
MAX_RUNTIMES = 2
MAX_REQUIRED_RAM = (1 << 32) - 1
CPU_MODEL_BINARY_SIZE = 912
CPU_WORK_SIZE = 48
CPU_PERSISTENT_SIZE_32 = 32


class BundleError(ValueError):
    pass


def _u32(value: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 < value <= 0xFFFFFFFF:
        raise BundleError(f"{name}: expected nonzero uint32")
    return value


def _json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BundleError(f"{path}: invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise BundleError(f"{path}: JSON root must be an object")
    return value


def _artifact_files(artifact: Path) -> dict[str, dict]:
    names = ("feature_schema_v1.json", "model.json", "normalization.json",
             "threshold.json", "training_manifest.json", "test_vectors.json",
             "artifact_index.json")
    values = {name: _json(artifact / name) for name in names}
    index = values["artifact_index.json"]
    expected = index.get("files_sha256")
    if not isinstance(expected, dict):
        raise BundleError("artifact index has no file hash registry")
    for name, digest in expected.items():
        path = artifact / name
        if not isinstance(digest, str) or len(digest) != 64 or not path.is_file():
            raise BundleError(f"artifact index contains invalid entry for {name}")
        if sha256_file(path) != digest:
            raise BundleError(f"artifact hash mismatch: {name}")
    if canonical_json_sha256(values["feature_schema_v1.json"]) != schema_canonical_hash():
        raise BundleError("canonical feature schema hash mismatch")
    return values


def _round_away(values: np.ndarray) -> np.ndarray:
    array = np.asarray(values)
    return np.copysign(np.floor(np.abs(array) + 0.5), array)


def _round_shift_away(value: int, shift: int) -> int:
    magnitude = abs(value)
    rounded = (magnitude + (1 << (shift - 1))) >> shift
    return -rounded if value < 0 else rounded


@dataclass(frozen=True)
class QuantizedModel:
    weights: tuple[np.ndarray, ...]
    biases: tuple[np.ndarray, ...]

    def infer(self, inputs: np.ndarray) -> np.ndarray:
        activation = np.asarray(inputs, dtype=np.int16)
        if activation.shape != (24,):
            raise BundleError("CPU inference input must have 24 elements")
        for layer, (weight, bias) in enumerate(zip(self.weights, self.biases)):
            result = np.empty(weight.shape[1], dtype=np.int16)
            for output_index in range(weight.shape[1]):
                accumulator = int(bias[output_index])
                for input_index in range(weight.shape[0]):
                    accumulator += (int(activation[input_index]) *
                                    int(weight[input_index, output_index]))
                if not -(1 << 31) <= accumulator < (1 << 31):
                    raise BundleError("CPU inference accumulator overflow")
                quantized = _round_shift_away(accumulator, 7)
                if layer != 3:
                    quantized = max(0, quantized)
                result[output_index] = min(127, max(-128, quantized))
            activation = result
        return activation.astype(np.int8)

    def score(self, inputs: np.ndarray) -> int:
        output = self.infer(inputs).astype(np.int16)
        difference = output - np.asarray(inputs, dtype=np.int16)
        total = sum(int(value) * int(value) for value in difference)
        return (total + 12) // 24


def canonical_float32(model: DenseAutoencoder) -> tuple[tuple[np.ndarray, ...],
                                                          tuple[np.ndarray, ...], bytes]:
    if tuple(model.dimensions) != DIMENSIONS:
        raise BundleError("topology mismatch; v1 requires 24-12-4-12-24")
    weights = tuple(np.asarray(value, dtype="<f4") for value in model.weights)
    biases = tuple(np.asarray(value, dtype="<f4") for value in model.biases)
    if any(not np.all(np.isfinite(value)) for value in weights + biases):
        raise BundleError("float32 conversion produced NaN/Inf")
    raw = b"".join(value.tobytes(order="C") for pair in zip(weights, biases)
                   for value in pair)
    return weights, biases, raw


def quantize_model(weights: tuple[np.ndarray, ...],
                   biases: tuple[np.ndarray, ...]) -> QuantizedModel:
    quant_weights = []
    quant_biases = []
    for weight, bias in zip(weights, biases):
        q_weight = np.clip(_round_away(weight.astype(np.float64) * 128.0),
                           -127, 127).astype(np.int8)
        q_bias = _round_away(bias.astype(np.float64) * 2048.0)
        if np.any(q_bias < -(1 << 31) + 400000) or np.any(q_bias > (1 << 31) - 1 - 400000):
            raise BundleError("quantized bias cannot guarantee int32 accumulation")
        quant_weights.append(q_weight)
        quant_biases.append(q_bias.astype(np.int32))
    return QuantizedModel(tuple(quant_weights), tuple(quant_biases))


def fixed_normalize(raw: Iterable[int], normalization: dict) -> np.ndarray:
    values = list(raw)
    means = normalization.get("mean_q16")
    inverses = normalization.get("inverse_std_q20")
    if len(values) != 24 or not isinstance(means, list) or not isinstance(inverses, list) or \
            len(means) != 24 or len(inverses) != 24:
        raise BundleError("normalization count mismatch")
    output = []
    for index, value in enumerate(values):
        if isinstance(value, bool) or not isinstance(value, (int, np.integer)) or not 0 <= int(value) <= 0xFFFFFFFF:
            raise BundleError(f"raw feature {index}: invalid uint32")
        mean = int(means[index])
        inverse = int(inverses[index])
        if not -(1 << 63) <= mean < (1 << 63) or not 0 < inverse < (1 << 31):
            raise BundleError("invalid fixed-point normalization parameter")
        product = ((int(value) << 16) - mean) * inverse
        if not -(1 << 63) <= product < (1 << 63):
            raise BundleError("fixed-point normalization overflow")
        quantized = _round_shift_away(product, 32)
        output.append(min(127, max(-128, quantized)))
    return np.asarray(output, dtype=np.int8)


def _higher(values: list[int] | np.ndarray, probability: float = .95):
    ordered = sorted(values)
    if not ordered:
        raise BundleError("cannot select threshold from empty validation data")
    return ordered[math.ceil(probability * (len(ordered) - 1))]


def _find_datasets(artifact: Path, files: dict[str, dict]) -> tuple[Path, Path, list[Path], list[Path]]:
    manifest = files["training_manifest.json"]
    index = files["artifact_index.json"]
    validation_id = manifest.get("threshold_source")
    test_entries = manifest.get("test_sessions")
    if not isinstance(test_entries, list) or len(test_entries) != 1:
        raise BundleError("training manifest has invalid held-out test session")
    test_id = test_entries[0].get("session_id")
    paths = [Path(value) for value in index.get("dataset_sha256", {})]
    eval_paths = [Path(value) for value in index.get("evaluation_dataset_sha256", {})]
    found: dict[str, Path] = {}
    for path in paths + eval_paths:
        if not path.is_file():
            raise BundleError(f"dataset referenced by artifact is unavailable: {path}")
        dataset = load_dataset(path)
        found[dataset.session_id] = path
    if validation_id not in found or test_id not in found:
        raise BundleError("validation or held-out dataset cannot be resolved")
    return found[validation_id], found[test_id], paths, eval_paths


def _float32_normalize(raw: np.ndarray, normalization: dict) -> np.ndarray:
    mean = np.asarray(normalization["mean"], dtype=np.float32)
    std = np.asarray(normalization["std"], dtype=np.float32)
    values = np.asarray(raw, dtype=np.float32)
    result = (values - mean) / std
    return np.clip(result, np.float32(-8), np.float32(8)).astype(np.float32)


def _float32_forward(values: np.ndarray, weights: tuple[np.ndarray, ...],
                     biases: tuple[np.ndarray, ...]) -> np.ndarray:
    activation = np.asarray(values, dtype=np.float32)
    for index, (weight, bias) in enumerate(zip(weights, biases)):
        activation = (activation @ weight + bias).astype(np.float32)
        if index != 3:
            activation = np.maximum(activation, np.float32(0)).astype(np.float32)
    return activation


def _score32(values: np.ndarray, weights: tuple[np.ndarray, ...],
             biases: tuple[np.ndarray, ...]) -> np.ndarray:
    difference = (_float32_forward(values, weights, biases) - values).astype(np.float32)
    return np.mean(np.square(difference, dtype=np.float32), axis=1, dtype=np.float32)


def _dataset_vectors(path: Path) -> tuple[object, np.ndarray]:
    dataset = load_dataset(path)
    vectors, _ = usable_vectors(dataset)
    return dataset, np.asarray(vectors, dtype=np.int64)


def deployment_evaluation(files: dict[str, dict], artifact: Path,
                          model64: DenseAutoencoder,
                          weights32: tuple[np.ndarray, ...],
                          biases32: tuple[np.ndarray, ...],
                          quantized: QuantizedModel,
                          normalization: dict) -> tuple[dict, int, dict]:
    validation_path, heldout_path, normal_paths, eval_paths = _find_datasets(artifact, files)
    validation, validation_raw = _dataset_vectors(validation_path)
    del validation
    fixed_validation = np.asarray([fixed_normalize(row, normalization)
                                   for row in validation_raw], dtype=np.int8)
    integer_validation_scores = [quantized.score(row) for row in fixed_validation]
    integer_threshold = int(_higher(integer_validation_scores))
    norm32_validation = _float32_normalize(validation_raw, normalization)
    float32_validation_scores = _score32(norm32_validation, weights32, biases32)
    threshold32 = float(_higher(float32_validation_scores.tolist()))
    threshold64 = float(files["threshold.json"]["anomaly_threshold"])

    rows = []
    sources = [("held-out-normal" if path == heldout_path else "normal-reference", path)
               for path in normal_paths]
    sources.extend(("evaluation", path) for path in eval_paths)
    for role, path in sources:
        dataset = load_dataset(path)
        for row in dataset.rows:
            from schema import deterministic_rule, encode_row
            reasons = deterministic_rule(row)
            if reasons:
                rows.append({"role": role, "stage": str(row.get("stage", "")),
                             "hard_fault": True})
                continue
            raw = np.asarray(encode_row(row), dtype=np.int64)
            norm64 = normalize(raw[np.newaxis, :],
                               np.asarray(normalization["mean"], dtype=np.float64),
                               np.asarray(normalization["std"], dtype=np.float64))
            score64 = float(model64.scores(norm64)[0])
            norm32 = _float32_normalize(raw[np.newaxis, :], normalization)
            score32 = float(_score32(norm32, weights32, biases32)[0])
            qinput = fixed_normalize(raw, normalization)
            score8 = quantized.score(qinput)
            rows.append({"role": role, "stage": str(row.get("stage", "")),
                         "hard_fault": False, "score64": score64, "score32": score32,
                         "score8": score8, "label64": score64 > threshold64,
                         "label32": score32 > threshold32,
                         "label8": score8 > integer_threshold})
    scored = [row for row in rows if not row["hard_fault"]]
    scores64 = np.asarray([row["score64"] for row in scored])
    scores32 = np.asarray([row["score32"] for row in scored])
    scores8 = np.asarray([row["score8"] for row in scored])
    correlation32 = float(np.corrcoef(scores64, scores32)[0, 1]) if len(scored) > 1 else 1.0
    correlation8 = float(np.corrcoef(scores64, scores8)[0, 1]) if len(scored) > 1 else 1.0

    def count(selector, label: str) -> tuple[int, int]:
        selected = [row for row in scored if selector(row)]
        return sum(bool(row[label]) for row in selected), len(selected)

    def metrics(label: str) -> dict:
        normal = count(lambda row: row["role"] == "held-out-normal", label)
        delay = count(lambda row: row["role"] == "evaluation" and
                      row["stage"] in {"light", "medium", "strong"}, label)
        strong = count(lambda row: row["stage"] == "strong", label)
        recovery = count(lambda row: row["stage"] == "recovery", label)
        return {
            "held_out_normal_false_warning": {"count": normal[0], "total": normal[1]},
            "delay_detection": {"count": delay[0], "total": delay[1]},
            "strong_delay_detection": {"count": strong[0], "total": strong[1]},
            "recovery_normal": {"count": recovery[1] - recovery[0], "total": recovery[1]},
        }

    report = {
        "all_normal_and_evaluation_rows": len(rows),
        "ai_scored_rows": len(scored),
        "float64_threshold": threshold64,
        "float32_threshold": threshold32,
        "integer_threshold_q8": integer_threshold,
        "float64": metrics("label64"),
        "float32": metrics("label32"),
        "cpu_int8": metrics("label8"),
        "hard_fault_rule": {"count": sum(row["hard_fault"] for row in rows),
                            "total": sum(row["hard_fault"] for row in rows)},
        "float32_max_absolute_score_difference": float(np.max(np.abs(scores64 - scores32))),
        "float32_score_correlation": correlation32,
        "cpu_int8_score_correlation": correlation8,
        "float32_label_agreement_rate": float(np.mean([
            row["label64"] == row["label32"] for row in scored])),
        "cpu_int8_label_agreement_rate": float(np.mean([
            row["label64"] == row["label8"] for row in scored])),
        "threshold_near_disagreement_count": sum(
            row["label64"] != row["label8"] and
            abs(row["score8"] - integer_threshold) <= 1 for row in scored),
        "nan_or_inf": False,
        "notes": [
            "All thresholds are selected from the normal validation session only.",
            "Injected evaluation data never calibrates scales or thresholds.",
        ],
    }
    source_vectors = files["test_vectors.json"]
    clear_vectors = {}
    for name in ("negative", "positive"):
        source = source_vectors.get(name)
        if source is not None:
            qinput = fixed_normalize(source["raw_vector"], normalization)
            qoutput = quantized.infer(qinput)
            score = quantized.score(qinput)
            clear_vectors[name] = {
                "origin": "measured-dataset",
                "raw": source["raw_vector"], "input_q4": qinput.astype(int).tolist(),
                "output_q4": qoutput.astype(int).tolist(), "score_q8": score,
                "anomaly": score > integer_threshold,
            }
    return report, integer_threshold, clear_vectors


def _compatibility(target: int, transport: int, profile: int, accelerator: int) -> bytes:
    data = bytearray(128)
    struct.pack_into("<HHHH", data, 0, 1, 1, len(SCHEMA_ID), 0)
    data[8:40] = SCHEMA_HASH
    struct.pack_into("<IIIIIII", data, 40, target, transport, TOPOLOGY_ID,
                     profile, 1, accelerator, MODEL_FORMAT_SENTINEL_BUNDLE_V1)
    data[72:72 + len(SCHEMA_ID)] = SCHEMA_ID.encode("ascii")
    return bytes(data)


def _normalization(normalization: dict) -> bytes:
    means = normalization.get("mean_q16")
    inverses = normalization.get("inverse_std_q20")
    if normalization.get("output_q") != 16 or normalization.get("int8_scale") != 16 or \
            not isinstance(means, list) or not isinstance(inverses, list) or \
            len(means) != 24 or len(inverses) != 24:
        raise BundleError("unsupported normalization contract")
    data = bytearray(struct.pack("<HHHHHHhh", 1, 24, 16, 20, 4, 1, -128, 127))
    for value in means:
        data.extend(struct.pack("<q", int(value)))
    for value in inverses:
        if not 0 < int(value) < (1 << 31):
            raise BundleError("invalid inverse standard deviation")
        data.extend(struct.pack("<i", int(value)))
    if len(data) != 304:
        raise AssertionError("normalization layout")
    return bytes(data)


def _decision(threshold: int) -> bytes:
    maximum = 255 * 255
    if not 0 <= threshold <= maximum:
        raise BundleError("invalid integer threshold")
    return struct.pack("<HHHHHHHHQQ", 1, 24, 4, 4, 8, 1, 1, 0,
                       threshold, maximum)


def _score_boundary_vector(score: int) -> dict:
    target = score * 24
    limit = math.isqrt(target)
    for a in range(limit, -1, -1):
        for b in range(a, -1, -1):
            for c in range(b, -1, -1):
                remaining = target - a * a - b * b - c * c
                if remaining < 0:
                    continue
                d = math.isqrt(remaining)
                if d <= c and d * d == remaining:
                    differences = [a, b, c, d] + [0] * 20
                    squared_sum = sum(value * value for value in differences)
                    return {"input_q4": [0] * 24,
                            "output_q4": differences,
                            "squared_sum": squared_sum,
                            "score_q8": (squared_sum + 12) // 24,
                            "anomaly": score > 0,
                            "origin": "synthetic-score-arithmetic-contract"}
    raise AssertionError("four-square construction failed")


def _cpu_binary(model: QuantizedModel) -> bytes:
    header = bytearray(32)
    header[:8] = b"MTFSQAE1"
    struct.pack_into("<HH5H", header, 8, 1, 4, *DIMENSIONS)
    header[22:26] = bytes((7, 4, 11, 1))
    weights = b"".join(value.astype(np.int8).tobytes(order="C")
                       for value in model.weights)
    biases = b"".join(value.astype("<i4").tobytes(order="C")
                      for value in model.biases)
    binary = bytes(header) + weights + biases
    if len(weights) != 672 or len(biases) != 208 or len(binary) != CPU_MODEL_BINARY_SIZE:
        raise AssertionError("CPU model layout")
    return binary


def _runtime_descriptor(runtime_type: int, provider: int, accelerator: int,
                        model_format: int, model_version: int,
                        alignment: int, persistent: int, scratch: int,
                        binary: bytes, canonical_hash: bytes,
                        conversion_hash: bytes) -> bytes:
    if len(canonical_hash) != 32 or len(conversion_hash) != 32:
        raise BundleError("runtime hash length mismatch")
    alignment = _u32(int(alignment), "required_alignment")
    persistent = int(persistent)
    scratch = int(scratch)
    if alignment > 4096 or alignment & (alignment - 1) or \
            not 0 <= persistent <= 0xffffffff or not 0 <= scratch <= 0xffffffff:
        raise BundleError("invalid runtime memory contract")
    binary_offset = (RUNTIME_DESCRIPTOR_SIZE + alignment - 1) & ~(alignment - 1)
    descriptor = bytearray(binary_offset)
    struct.pack_into("<HHIIIIIHHBBbbIIIIIIII", descriptor, 0,
        1, runtime_type, provider, accelerator, model_format, model_version, 1,
        24, 24, 1, 1, 0, 0, 1, 4, 1, 4, alignment, persistent, scratch, len(binary))
    descriptor[64:96] = canonical_hash
    descriptor[96:128] = hashlib.sha256(binary).digest()
    descriptor[128:160] = conversion_hash
    struct.pack_into("<I", descriptor, 160, binary_offset)
    return bytes(descriptor)


def _npu_runtime_descriptor_v2(manifest: dict, binary: bytes,
                               canonical_hash: bytes,
                               conversion_hash: bytes) -> bytes:
    regions = manifest.get("memory_regions")
    if not isinstance(regions, list) or not regions or len(regions) > 16:
        raise BundleError("invalid NPU runtime memory region table")
    table = bytearray()
    caller_end = 0
    caller_alignment = 1
    seen = set()
    placed = []
    for source in regions:
        if not isinstance(source, dict):
            raise BundleError("invalid NPU runtime memory region")
        try:
            kind = int(source["kind"]); placement = int(source["placement"])
            flags = int(source.get("flags", RUNTIME_REGION_FLAG_REQUIRED))
            logical_size = int(source["logical_size"])
            storage_size = int(source.get("storage_size", logical_size))
            alignment = int(source["alignment"])
            pool_id = int(source.get("provider_pool_id", 0))
            address = int(source.get("address_or_offset", 0))
            lifetime = int(source["lifetime"])
            install_access = int(source["install_access"])
            inference_access = int(source["inference_access"])
            requirements = int(source.get("requirements", 0))
        except (KeyError, TypeError, ValueError) as error:
            raise BundleError("invalid NPU runtime memory region") from error
        identity = (kind, placement)
        if identity in seen or kind not in _REGION_KINDS or \
                placement not in _REGION_PLACEMENTS or flags != 1 or \
                logical_size <= 0 or logical_size > 0xffffffffffffffff or \
                storage_size < logical_size or storage_size > 0xffffffffffffffff or \
                not 0 < alignment <= 4096 or alignment & (alignment - 1) or \
                not 0 <= pool_id <= 0xffffffff or \
                not 0 <= address <= 0xffffffffffffffff or \
                lifetime not in _REGION_LIFETIMES or \
                install_access & ~_REGION_ACCESS_MASK or \
                inference_access & ~_REGION_ACCESS_MASK or \
                requirements & ~_REGION_REQUIREMENT_MASK:
            raise BundleError("invalid NPU runtime memory region")
        if placement == PLACEMENT_CALLER_RELATIVE:
            if address & (alignment - 1) or address + storage_size > 0xffffffff:
                raise BundleError("invalid caller-relative NPU region")
            caller_end = max(caller_end, address + storage_size)
            caller_alignment = max(caller_alignment, alignment)
        elif placement == PLACEMENT_FIXED_ABSOLUTE:
            if address == 0 or address & (alignment - 1) or \
                    address + storage_size > 0xffffffffffffffff:
                raise BundleError("invalid fixed NPU region")
        elif placement == PLACEMENT_BINARY_CONTAINED:
            if address > len(binary) or storage_size > len(binary) - address:
                raise BundleError("invalid binary-contained NPU region")
        elif address != 0:
            raise BundleError("provider-assigned NPU region has an address")
        for prior_placement, prior_address, prior_storage in placed:
            if prior_placement == placement and placement != PLACEMENT_PROVIDER_ASSIGNED and \
                    address < prior_address + prior_storage and \
                    prior_address < address + storage_size:
                raise BundleError("overlapping NPU runtime memory regions")
        placed.append((placement, address, storage_size))
        seen.add(identity)
        entry = bytearray(RUNTIME_REGION_ENTRY_SIZE)
        struct.pack_into("<HHHHQIIQIIIIQ", entry, 0,
            RUNTIME_REGION_ENTRY_VERSION, kind, placement, flags, logical_size,
            alignment, pool_id, address, lifetime, install_access,
            inference_access, requirements, storage_size)
        table.extend(entry)
    declared_alignment = _u32(int(manifest["required_alignment"]),
                              "required_alignment")
    declared_persistent = _u32(int(manifest["persistent_memory"]),
                               "persistent_memory")
    declared_scratch = int(manifest["scratch_memory"])
    if not 0 <= declared_scratch <= 0xffffffff:
        raise BundleError("scratch_memory: expected uint32")
    if declared_alignment != caller_alignment or declared_persistent != caller_end or \
            declared_scratch != 0:
        raise BundleError("NPU memory summary does not match region table")
    binary_offset = (RUNTIME_DESCRIPTOR_SIZE + len(table) + declared_alignment - 1) & \
        ~(declared_alignment - 1)
    descriptor = bytearray(binary_offset)
    struct.pack_into("<HHIIIIIHHBBbbIIIIIIII", descriptor, 0,
        2, 2, _u32(int(manifest["provider_id"]), "provider_id"),
        _u32(int(manifest["accelerator_id"]), "accelerator_id"),
        _u32(int(manifest["model_format"]), "model_format"),
        _u32(int(manifest["model_version"]), "model_version"),
        _u32(int(manifest["runtime_abi"]), "runtime_abi"), 24, 24, 1, 1,
        int(manifest["input_zero_point"]), int(manifest["output_zero_point"]),
        _u32(int(manifest["input_scale_numerator"]), "input_scale_numerator"),
        _u32(int(manifest["input_scale_shift"]), "input_scale_shift"),
        _u32(int(manifest["output_scale_numerator"]), "output_scale_numerator"),
        _u32(int(manifest["output_scale_shift"]), "output_scale_shift"),
        declared_alignment, declared_persistent, declared_scratch, len(binary))
    descriptor[64:96] = canonical_hash
    descriptor[96:128] = hashlib.sha256(binary).digest()
    descriptor[128:160] = conversion_hash
    runtime_variant = int(manifest.get("runtime_variant", 0))
    runtime_extra = int(manifest.get("runtime_extra", 0))
    if not 0 <= runtime_variant <= 0xffffffff or \
            not 0 <= runtime_extra <= 0xffffffff:
        raise BundleError("invalid NPU runtime version metadata")
    struct.pack_into("<IIHHHHHHIII", descriptor, 160, binary_offset,
        RUNTIME_DESCRIPTOR_SIZE, len(regions), RUNTIME_REGION_ENTRY_SIZE,
        RUNTIME_DESCRIPTOR_SIZE, 0, int(manifest.get("runtime_version_major", 0)),
        int(manifest.get("runtime_version_minor", 0)),
        runtime_variant, runtime_extra, 0)
    descriptor[RUNTIME_DESCRIPTOR_SIZE:RUNTIME_DESCRIPTOR_SIZE + len(table)] = table
    return bytes(descriptor)


def _npu_runtime(binary_path: Path, manifest_path: Path,
                 canonical_hash: bytes) -> tuple[int, bytes, dict]:
    manifest = _json(manifest_path)
    binary = binary_path.read_bytes()
    if not binary:
        raise BundleError("NPU binary is empty; dummy runtime is forbidden")
    required = ("provider_id", "accelerator_id", "model_format", "model_version",
                "runtime_abi", "required_alignment", "persistent_memory",
                "scratch_memory", "canonical_model_sha256",
                "runtime_binary_sha256", "conversion_manifest_sha256",
                "input_shape", "output_shape", "input_dtype", "output_dtype",
                "input_scale_numerator", "input_scale_shift", "input_zero_point",
                "output_scale_numerator", "output_scale_shift", "output_zero_point",
                "memory_regions")
    if any(name not in manifest for name in required):
        raise BundleError("malformed NPU runtime manifest")
    if manifest["canonical_model_sha256"] != canonical_hash.hex() or \
            manifest["runtime_binary_sha256"] != hashlib.sha256(binary).hexdigest():
        raise BundleError("NPU runtime hash mismatch")
    if manifest["input_shape"] != [24] or manifest["output_shape"] != [24] or \
            manifest["input_dtype"] != "int8" or manifest["output_dtype"] != "int8" or \
            not 0 < int(manifest["input_scale_numerator"]) <= 0xffffffff or \
            not 0 <= int(manifest["input_scale_shift"]) <= 31 or \
            not -128 <= int(manifest["input_zero_point"]) <= 127 or \
            not 0 < int(manifest["output_scale_numerator"]) <= 0xffffffff or \
            not 0 <= int(manifest["output_scale_shift"]) <= 31 or \
            not -128 <= int(manifest["output_zero_point"]) <= 127:
        raise BundleError("NPU tensor descriptor mismatch")
    manifest_copy = dict(manifest)
    declared_conversion = manifest_copy.pop("conversion_manifest_sha256")
    conversion_hash = hashlib.sha256(canonical_json_bytes(manifest_copy)).digest()
    if declared_conversion != conversion_hash.hex():
        raise BundleError("NPU conversion manifest hash mismatch")
    provider = _u32(int(manifest["provider_id"]), "provider_id")
    descriptor = _npu_runtime_descriptor_v2(manifest, binary, canonical_hash,
                                            conversion_hash)
    return provider, descriptor + binary, manifest


@dataclass(frozen=True)
class Section:
    type: int
    flags: int
    alignment: int
    provider: int
    payload: bytes
    name: str


def _assemble(sections: list[Section]) -> tuple[bytes, dict[str, str]]:
    if not 1 <= len(sections) <= MAX_SECTIONS:
        raise BundleError("section count outside policy")
    header_size = HEADER_SIZE + len(sections) * DIRECTORY_SIZE
    directory = bytearray()
    body = bytearray()
    offset = header_size
    hashes = {}
    for section in sections:
        if section.alignment <= 0 or section.alignment > 4096 or \
                section.alignment & (section.alignment - 1):
            raise BundleError("invalid section alignment")
        aligned = (offset + section.alignment - 1) & ~(section.alignment - 1)
        body.extend(bytes(aligned - offset))
        offset = aligned
        directory.extend(struct.pack("<HHIIIIIII", section.type, section.flags,
                                     offset, len(section.payload), section.alignment,
                                     section.provider, 0, 0, 0))
        body.extend(section.payload)
        hashes[section.name] = hashlib.sha256(section.payload).hexdigest()
        offset += len(section.payload)
    if offset > MAX_SIZE:
        raise BundleError("bundle exceeds maximum size")
    header = struct.pack("<8sHHIHHIII", MAGIC, BUNDLE_VERSION, header_size,
                         offset, len(sections), DIRECTORY_SIZE, 0, 0, 0)
    return header + bytes(directory) + bytes(body), hashes


def _memory_plan(bundle_size: int, runtime_sections: Iterable[Section]) -> dict:
    cursor = int(bundle_size)
    maximum_alignment = 1
    scratch_size = 0
    persistent_offsets = []
    runtimes = list(runtime_sections)
    if not 1 <= len(runtimes) <= MAX_RUNTIMES:
        raise BundleError("runtime count outside V1 contract")
    for section in runtimes:
        values = struct.unpack_from("<HHIIIIIHHBBbbIIIIIIII", section.payload)
        alignment, persistent, scratch = values[17:20]
        maximum_alignment = max(maximum_alignment, alignment)
        scratch_size = max(scratch_size, scratch)
        if persistent:
            cursor = (cursor + alignment - 1) & ~(alignment - 1)
            persistent_offsets.append(cursor)
            cursor += persistent
        else:
            persistent_offsets.append(0)
        if cursor > MAX_REQUIRED_RAM:
            raise BundleError("runtime persistent memory overflows V1 RAM contract")
    if scratch_size:
        cursor = (cursor + maximum_alignment - 1) & ~(maximum_alignment - 1)
        scratch_offset = cursor
        cursor += scratch_size
    else:
        scratch_offset = 0
    if cursor > MAX_REQUIRED_RAM:
        raise BundleError("runtime scratch memory overflows V1 RAM contract")
    return {"required_alignment": maximum_alignment,
            "persistent_offsets": persistent_offsets,
            "scratch_offset": scratch_offset, "scratch_size": scratch_size,
            "required_ram": cursor}


def _parse_npu_regions(payload: bytes, binary_offset: int,
                       binary_len: int) -> list[dict]:
    table_offset, count, entry_size, header_size, reserved = struct.unpack_from(
        "<IHHHH", payload, 164)
    if table_offset != RUNTIME_DESCRIPTOR_SIZE or not 1 <= count <= 16 or \
            entry_size != RUNTIME_REGION_ENTRY_SIZE or \
            header_size != RUNTIME_DESCRIPTOR_SIZE or reserved != 0 or \
            any(payload[188:192]) or \
            count > (binary_offset - table_offset) // entry_size:
        raise BundleError("malformed NPU runtime region table")
    regions = []
    seen = set()
    caller_end = 0
    caller_alignment = 1
    for index in range(count):
        offset = table_offset + index * entry_size
        version, kind, placement, flags, logical_size, alignment, pool_id, address, \
            lifetime, install_access, inference_access, requirements, storage_size = \
            struct.unpack_from("<HHHHQIIQIIIIQ", payload, offset)
        identity = (kind, placement)
        if version != RUNTIME_REGION_ENTRY_VERSION or identity in seen or \
                kind not in _REGION_KINDS or placement not in _REGION_PLACEMENTS or \
                flags != RUNTIME_REGION_FLAG_REQUIRED or logical_size == 0 or \
                storage_size < logical_size or alignment <= 0 or \
                alignment & (alignment - 1) or \
                alignment > 4096 or lifetime not in _REGION_LIFETIMES or \
                install_access & ~_REGION_ACCESS_MASK or \
                inference_access & ~_REGION_ACCESS_MASK or \
                requirements & ~_REGION_REQUIREMENT_MASK or any(payload[offset + 56:offset + 64]):
            raise BundleError("malformed NPU runtime memory region")
        if placement == PLACEMENT_CALLER_RELATIVE:
            if address & (alignment - 1) or address + storage_size > 0xffffffff:
                raise BundleError("malformed caller-relative NPU region")
            caller_end = max(caller_end, address + storage_size)
            caller_alignment = max(caller_alignment, alignment)
        elif placement == PLACEMENT_FIXED_ABSOLUTE:
            if address == 0 or address & (alignment - 1) or \
                    address + storage_size > 0xffffffffffffffff:
                raise BundleError("malformed fixed NPU region")
        elif placement == PLACEMENT_BINARY_CONTAINED:
            if address > binary_len or storage_size > binary_len - address:
                raise BundleError("malformed binary-contained NPU region")
        elif address != 0:
            raise BundleError("malformed provider-assigned NPU region")
        for prior in regions:
            if prior["placement"] == placement and \
                    placement != PLACEMENT_PROVIDER_ASSIGNED:
                prior_start = prior["address_or_offset"]
                prior_end = prior_start + prior["storage_size"]
                if address < prior_end and prior_start < address + storage_size:
                    raise BundleError("overlapping NPU runtime memory regions")
        seen.add(identity)
        regions.append({"kind": kind, "placement": placement, "flags": flags,
            "logical_size": logical_size, "storage_size": storage_size,
            "alignment": alignment, "provider_pool_id": pool_id,
            "address_or_offset": address, "lifetime": lifetime,
            "install_access": install_access, "inference_access": inference_access,
            "requirements": requirements})
    table_end = table_offset + count * entry_size
    if any(payload[table_end:binary_offset]):
        raise BundleError("nonzero NPU runtime descriptor padding")
    persistent = struct.unpack_from("<I", payload, 52)[0]
    scratch = struct.unpack_from("<I", payload, 56)[0]
    required_alignment = struct.unpack_from("<I", payload, 48)[0]
    if (persistent, scratch, required_alignment) != (caller_end, 0, caller_alignment):
        raise BundleError("NPU memory summary does not match region table")
    return regions


def build_bundle(artifact: Path, include_cpu: bool = True,
                 npu_binary: Path | None = None,
                 npu_manifest: Path | None = None,
                 profile_id: int | None = None,
                 expected_accelerator: int | None = None) -> tuple[bytes, dict]:
    artifact = artifact.resolve()
    files = _artifact_files(artifact)
    manifest = files["training_manifest.json"]
    normalization = files["normalization.json"]
    target = _u32(int(manifest.get("target_id", 0)), "target_id")
    transport = _u32(int(manifest.get("transport_id", 0)), "transport_id")
    if (target, transport) not in ((TARGET_RA, TRANSPORT_SPI), (TARGET_ST, TRANSPORT_IDMA)):
        raise BundleError("target/transport mismatch")
    if int(normalization.get("target_id", 0)) != target or \
            int(normalization.get("transport_id", 0)) != transport:
        raise BundleError("normalization target/transport mismatch")
    if files["artifact_index.json"].get("feature_schema_canonical_sha256") != SCHEMA_HASH.hex():
        raise BundleError("artifact schema identity mismatch")
    model64 = DenseAutoencoder.from_dict(files["model.json"])
    weights32, biases32, canonical_bytes = canonical_float32(model64)
    canonical_hash = hashlib.sha256(canonical_bytes).digest()
    quantized = quantize_model(weights32, biases32)
    evaluation, threshold, clear_vectors = deployment_evaluation(
        files, artifact, model64, weights32, biases32, quantized, normalization)
    if profile_id is None:
        profile_id = int.from_bytes(canonical_hash[:4], "little") or 1
    profile_id = _u32(profile_id, "profile_id")
    cpu_binary = _cpu_binary(quantized)
    conversion = {
        "format": "mtfs-sentinel-cpu-int8-conversion-v1",
        "source_model_sha256": sha256_file(artifact / "model.json"),
        "canonical_float32_sha256": canonical_hash.hex(),
        "activation": "signed-int8-q4-symmetric-zero-point-0",
        "weight": "signed-int8-q7-per-tensor-symmetric-zero-point-0",
        "bias_accumulator": "signed-int32-q11",
        "requantization": "round-nearest-ties-away-from-zero-shift-7",
        "hidden": "relu-then-int8-saturate",
        "output": "linear-int8-saturate",
        "calibration": "normal-training-and-validation-only",
    }
    conversion_hash = hashlib.sha256(canonical_json_bytes(conversion)).digest()
    runtime_sections: list[Section] = []
    if include_cpu:
        descriptor = _runtime_descriptor(1, PROVIDER_CPU_REFERENCE,
            ACCELERATOR_CPU_REFERENCE, MODEL_FORMAT_CPU_INT8_V1, 1, 4,
            CPU_PERSISTENT_SIZE_32,
            CPU_WORK_SIZE, cpu_binary, canonical_hash, conversion_hash)
        runtime_sections.append(Section(SECTION_CPU, SECTION_REQUIRED, 4,
                                        PROVIDER_CPU_REFERENCE,
                                        descriptor + cpu_binary, "cpu_runtime"))
    npu_info = None
    if (npu_binary is None) != (npu_manifest is None):
        raise BundleError("NPU binary and manifest must be supplied together")
    if npu_binary is not None and npu_manifest is not None:
        provider, runtime, npu_info = _npu_runtime(npu_binary, npu_manifest,
                                                   canonical_hash)
        runtime_sections.append(Section(SECTION_NPU, SECTION_REQUIRED,
                                        int(npu_info["required_alignment"]),
                                        provider, runtime, f"npu_runtime_{provider:08x}"))
    if not runtime_sections:
        raise BundleError("at least one CPU or NPU runtime is required")
    if npu_info is not None:
        accelerator = _u32(int(npu_info["accelerator_id"]), "accelerator_id")
    else:
        accelerator = ACCELERATOR_CPU_REFERENCE
    if expected_accelerator is not None and expected_accelerator != accelerator:
        raise BundleError("requested outer accelerator does not match runtime composition")

    fixed_sections = [
        Section(SECTION_COMPATIBILITY, SECTION_REQUIRED, 4, 0,
                _compatibility(target, transport, profile_id, accelerator), "compatibility"),
        Section(SECTION_NORMALIZATION, SECTION_REQUIRED, 8, 0,
                _normalization(normalization), "normalization"),
        Section(SECTION_DECISION, SECTION_REQUIRED, 8, 0,
                _decision(threshold), "decision"),
        *runtime_sections,
    ]
    _, fixed_hashes = _assemble(fixed_sections)
    provenance = {
        "format": "mtfs-sentinel-provenance-v1",
        "tool_version": TOOL_VERSION,
        "schema_canonical_sha256": SCHEMA_HASH.hex(),
        "source_artifact": {name: sha256_file(artifact / name)
                            for name in sorted(files) if (artifact / name).is_file()},
        "canonical_float32_sha256": canonical_hash.hex(),
        "canonical_float32_bytes": len(canonical_bytes),
        "cpu_runtime_binary_sha256": hashlib.sha256(cpu_binary).hexdigest(),
        "cpu_conversion_manifest": conversion,
        "section_sha256": fixed_hashes,
        "evaluation": evaluation,
        "test_vectors": clear_vectors,
        "arithmetic_vectors": {
            "score_boundary": {
                "below": {**_score_boundary_vector(max(0, threshold - 1)),
                          "anomaly": False},
                "equal": {**_score_boundary_vector(threshold), "anomaly": False},
                "above": {**_score_boundary_vector(threshold + 1), "anomaly": True},
            },
            "input_clamp_negative_q4": -128,
            "input_clamp_positive_q4": 127,
            "output_saturation_limits_q4": [-128, 127],
            "accumulator_contract": "reject outside signed-int32 before requantization",
            "origin": "synthetic-arithmetic-contract-not-measured-dataset",
        },
        "integrity_contract": {
            "host": "canonical JSON, section hashes, runtime binary hashes, and provenance registry are verified",
            "embedded": "provenance is opaque authenticated metadata; no JSON parser or SHA-256 dependency",
            "plain_bundle": "integrity is not established without the outer sealed-package AEAD",
        },
        "limitations": [
            "NPU binary semantic equivalence is not established by hashes.",
            "NPU golden-vector execution is deferred to Phase 4.3C-ST/RA.",
            "A 672-MAC model is not assumed to run faster on an NPU than a CPU.",
            "Secure deletion of the plaintext bundle is not guaranteed or automatic.",
        ],
    }
    provenance_bytes = canonical_json_bytes(provenance)
    sections = fixed_sections + [Section(SECTION_PROVENANCE, SECTION_REQUIRED, 4,
                                         0, provenance_bytes, "provenance")]
    bundle, hashes = _assemble(sections)
    memory_plan = _memory_plan(len(bundle), runtime_sections)
    summary = {
        "bundle_sha256": hashlib.sha256(bundle).hexdigest(),
        "bundle_size": len(bundle), "target_id": target,
        "transport_id": transport, "accelerator_id": accelerator,
        "model_format": MODEL_FORMAT_SENTINEL_BUNDLE_V1,
        "profile_id": profile_id, "runtime_count": len(runtime_sections),
        "canonical_float32_sha256": canonical_hash.hex(),
        "cpu_runtime_binary_sha256": hashlib.sha256(cpu_binary).hexdigest(),
        "integer_threshold_q8": threshold, "section_sha256": hashes,
        "minimum_required_ram_32bit": memory_plan["required_ram"],
        "required_alignment": memory_plan["required_alignment"],
        "memory_plan": memory_plan,
        "evaluation": evaluation,
    }
    return bundle, summary


def atomic_write(path: Path, data: bytes) -> None:
    path = path.resolve()
    if path.exists():
        raise BundleError(f"output exists; refusing overwrite: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.",
                                                   suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if path.exists():
            raise BundleError(f"output appeared during commit: {path}")
        os.rename(temporary, path)
        if hasattr(os, "O_DIRECTORY"):
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


@dataclass
class ParsedSection:
    type: int
    flags: int
    offset: int
    length: int
    alignment: int
    provider: int
    payload: bytes
    name: str


@dataclass
class ParsedBundle:
    raw: bytes
    target: int
    transport: int
    topology: int
    profile: int
    accelerator: int
    model_format: int
    normalization: dict
    threshold: int
    sections: list[ParsedSection]
    provenance: dict


def parse_bundle(raw: bytes, expected_target: int | None = None,
                 expected_transport: int | None = None,
                 expected_accelerator: int | None = None,
                 expected_model_format: int = MODEL_FORMAT_SENTINEL_BUNDLE_V1) -> ParsedBundle:
    if not isinstance(raw, bytes) or not HEADER_SIZE <= len(raw) <= MAX_SIZE:
        raise BundleError("truncated or oversized bundle")
    magic, version, header_size, total, count, entry_size, flags, r0, r1 = \
        struct.unpack_from("<8sHHIHHIII", raw)
    if magic != MAGIC:
        raise BundleError("bad bundle magic")
    if version != 1:
        raise BundleError("unsupported bundle version")
    if not 1 <= count <= MAX_SECTIONS or entry_size != DIRECTORY_SIZE or \
            header_size != HEADER_SIZE + count * DIRECTORY_SIZE or \
            total != len(raw) or flags or r0 or r1 or header_size > total:
        raise BundleError("malformed bundle header")
    sections = []
    previous_end = header_size
    names = {SECTION_COMPATIBILITY: "compatibility", SECTION_NORMALIZATION: "normalization",
             SECTION_DECISION: "decision", SECTION_PROVENANCE: "provenance",
             SECTION_CPU: "cpu_runtime", SECTION_NPU: "npu_runtime"}
    known = set(names)
    singleton_seen = set()
    runtime_providers = set()
    runtime_count = 0
    for index in range(count):
        values = struct.unpack_from("<HHIIIIIII", raw, HEADER_SIZE + index * DIRECTORY_SIZE)
        section_type, section_flags, offset, length, alignment, provider, x, y, z = values
        if section_flags & ~SECTION_REQUIRED or x or y or z or not length or \
                not alignment or alignment > 4096 or alignment & (alignment - 1):
            raise BundleError("malformed section directory entry")
        aligned = (previous_end + alignment - 1) & ~(alignment - 1)
        if offset != aligned or offset < header_size or length > total - offset or \
                any(raw[previous_end:offset]):
            raise BundleError("overlap, range, alignment, or layout error")
        previous_end = offset + length
        if section_type not in known:
            if section_flags & SECTION_REQUIRED:
                raise BundleError("unknown mandatory section")
            name = f"optional_{section_type:04x}"
        else:
            name = names[section_type]
            if section_type in {SECTION_COMPATIBILITY, SECTION_NORMALIZATION,
                                SECTION_DECISION, SECTION_PROVENANCE, SECTION_CPU}:
                if section_type in singleton_seen:
                    raise BundleError("duplicate singleton section")
                singleton_seen.add(section_type)
            if section_type in {SECTION_CPU, SECTION_NPU}:
                runtime_count += 1
                if provider in runtime_providers:
                    raise BundleError("duplicate runtime provider")
                runtime_providers.add(provider)
                if section_type == SECTION_NPU:
                    name += f"_{provider:08x}"
        sections.append(ParsedSection(section_type, section_flags, offset, length,
                                      alignment, provider, raw[offset:offset + length], name))
    if previous_end != total:
        raise BundleError("trailing or layout inconsistency")
    mandatory = {SECTION_COMPATIBILITY, SECTION_NORMALIZATION,
                 SECTION_DECISION, SECTION_PROVENANCE}
    if not mandatory.issubset(singleton_seen) or runtime_count == 0:
        raise BundleError("missing mandatory section or runtime")
    for section in sections:
        if section.type in mandatory and not section.flags & SECTION_REQUIRED:
            raise BundleError("mandatory section is marked optional")
    by_type = {section.type: section for section in sections if section.type in known}
    compat = by_type[SECTION_COMPATIBILITY].payload
    if len(compat) != 128:
        raise BundleError("compatibility descriptor size mismatch")
    cv, sv, schema_len, reserved = struct.unpack_from("<HHHH", compat)
    target, transport, topology, profile, abi, accelerator, model_format = \
        struct.unpack_from("<IIIIIII", compat, 40)
    if cv != 1 or sv != 1 or schema_len != len(SCHEMA_ID) or reserved or \
            compat[8:40] != SCHEMA_HASH or compat[72:105] != SCHEMA_ID.encode() or \
            any(compat[105:]) or topology != TOPOLOGY_ID or abi != 1:
        raise BundleError("compatibility identity mismatch")
    if (target, transport) not in ((TARGET_RA, TRANSPORT_SPI), (TARGET_ST, TRANSPORT_IDMA)):
        raise BundleError("target/transport mismatch")
    if expected_target is not None and target != expected_target or \
            expected_transport is not None and transport != expected_transport or \
            expected_accelerator is not None and accelerator != expected_accelerator or \
            model_format != expected_model_format:
        raise BundleError("outer/inner policy mismatch")
    norm = by_type[SECTION_NORMALIZATION].payload
    if len(norm) != 304 or struct.unpack_from("<HHHHHHhh", norm) != \
            (1, 24, 16, 20, 4, 1, -128, 127):
        raise BundleError("normalization descriptor mismatch")
    means = list(struct.unpack_from("<24q", norm, 16))
    inverses = list(struct.unpack_from("<24i", norm, 208))
    if any(value <= 0 for value in inverses):
        raise BundleError("invalid normalization scale")
    decision = by_type[SECTION_DECISION].payload
    if len(decision) != 32:
        raise BundleError("decision descriptor size mismatch")
    dvalues = struct.unpack("<HHHHHHHHQQ", decision)
    if dvalues[:8] != (1, 24, 4, 4, 8, 1, 1, 0) or dvalues[9] != 65025 or \
            dvalues[8] > dvalues[9]:
        raise BundleError("invalid decision contract")
    npu_accelerator = None
    runtime_canonical_hashes = []
    runtime_sections = []
    cpu_runtime_count = 0
    npu_runtime_count = 0
    for section in sections:
        if section.type not in {SECTION_CPU, SECTION_NPU}:
            continue
        if len(section.payload) < RUNTIME_DESCRIPTOR_SIZE:
            raise BundleError("truncated runtime descriptor")
        descriptor = struct.unpack_from("<HHIIIIIHHBBbbIIIIIIII", section.payload)
        rv, runtime_type, provider, runtime_accel, runtime_format, model_version, runtime_abi, \
            input_count, output_count, input_dtype, output_dtype, input_zero, output_zero, \
            input_num, input_shift, output_num, output_shift, alignment, persistent, scratch, binary_len = descriptor
        binary_offset = struct.unpack_from("<I", section.payload, 160)[0]
        if provider != section.provider or not provider or not runtime_accel or \
                not runtime_format or not model_version or not runtime_abi or \
                (input_count, output_count, input_dtype, output_dtype) != (24, 24, 1, 1) or \
                not input_num or input_shift > 31 or not output_num or output_shift > 31 or \
                not alignment or \
                alignment & (alignment - 1) or alignment > 4096 or \
                section.alignment != alignment or \
                binary_offset < RUNTIME_DESCRIPTOR_SIZE or \
                binary_offset & (alignment - 1) or binary_offset > len(section.payload) or \
                binary_len != len(section.payload) - binary_offset or \
                not binary_len:
            raise BundleError("malformed runtime manifest")
        binary = section.payload[binary_offset:]
        runtime_sections.append(section)
        runtime_canonical_hashes.append(section.payload[64:96])
        if hashlib.sha256(binary).digest() != section.payload[96:128]:
            raise BundleError("runtime binary hash mismatch")
        if section.type == SECTION_CPU:
            cpu_runtime_count += 1
            if rv != 1 or runtime_type != 1 or runtime_abi != 1 or \
                    input_zero != 0 or output_zero != 0 or input_num != 1 or \
                    input_shift != 4 or output_num != 1 or output_shift != 4 or \
                    any(section.payload[164:192]) or \
                    any(section.payload[RUNTIME_DESCRIPTOR_SIZE:binary_offset]) or \
                    runtime_accel != ACCELERATOR_CPU_REFERENCE or \
                    runtime_format != MODEL_FORMAT_CPU_INT8_V1 or alignment != 4 or \
                    persistent != CPU_PERSISTENT_SIZE_32 or scratch != CPU_WORK_SIZE:
                raise BundleError("CPU runtime descriptor mismatch")
            _parse_cpu_binary(binary)
        else:
            npu_runtime_count += 1
            if rv != 2 or runtime_type != 2:
                raise BundleError("NPU runtime type mismatch")
            _parse_npu_regions(section.payload, binary_offset, binary_len)
            npu_accelerator = runtime_accel
    if cpu_runtime_count > 1 or npu_runtime_count > 1 or \
            cpu_runtime_count + npu_runtime_count > MAX_RUNTIMES:
        raise BundleError("runtime count outside V1 contract")
    if len(set(runtime_canonical_hashes)) != 1:
        raise BundleError("runtime canonical model hash mismatch")
    expected_composition_accelerator = (
        npu_accelerator if npu_accelerator is not None else
        ACCELERATOR_CPU_REFERENCE)
    if accelerator != expected_composition_accelerator:
        raise BundleError("runtime composition does not match outer accelerator identity")
    try:
        provenance = json.loads(by_type[SECTION_PROVENANCE].payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise BundleError("malformed provenance") from error
    if not isinstance(provenance, dict) or \
            provenance.get("format") != "mtfs-sentinel-provenance-v1" or \
            provenance.get("schema_canonical_sha256") != SCHEMA_HASH.hex():
        raise BundleError("provenance identity mismatch")
    canonical_digest = provenance.get("canonical_float32_sha256")
    if canonical_digest is not None and (
            not isinstance(canonical_digest, str) or len(canonical_digest) != 64 or
            any(value.hex() != canonical_digest for value in runtime_canonical_hashes)):
        raise BundleError("runtime canonical model hash mismatch")
    registered = provenance.get("section_sha256")
    if not isinstance(registered, dict):
        raise BundleError("provenance section hash registry missing")
    for section in sections:
        if section.type == SECTION_PROVENANCE or section.name.startswith("optional_"):
            continue
        if registered.get(section.name) != hashlib.sha256(section.payload).hexdigest():
            raise BundleError(f"section hash mismatch: {section.name}")
    _memory_plan(len(raw), [Section(section.type, section.flags,
        section.alignment, section.provider, section.payload, section.name)
        for section in runtime_sections])
    return ParsedBundle(raw, target, transport, topology, profile, accelerator,
                        model_format, {"mean_q16": means,
                        "inverse_std_q20": inverses}, dvalues[8], sections, provenance)


def _parse_cpu_binary(binary: bytes) -> QuantizedModel:
    if len(binary) != CPU_MODEL_BINARY_SIZE or binary[:8] != b"MTFSQAE1" or \
            struct.unpack_from("<HH5H", binary, 8) != (1, 4, *DIMENSIONS) or \
            binary[22:26] != bytes((7, 4, 11, 1)) or any(binary[26:32]):
        raise BundleError("CPU model tensor header mismatch")
    offset = 32
    weights = []
    for inputs, outputs in zip(DIMENSIONS[:-1], DIMENSIONS[1:]):
        count = inputs * outputs
        weights.append(np.frombuffer(binary[offset:offset + count], dtype=np.int8)
                       .reshape(inputs, outputs).copy())
        offset += count
    biases = []
    for outputs in DIMENSIONS[1:]:
        size = outputs * 4
        biases.append(np.frombuffer(binary[offset:offset + size], dtype="<i4").copy())
        offset += size
    if offset != len(binary):
        raise BundleError("CPU model tensor length mismatch")
    if any(np.any(value > (1 << 31) - 1 - 400000) or
           np.any(value < -(1 << 31) + 400000) for value in biases):
        raise BundleError("CPU model bias can overflow signed int32 accumulation")
    return QuantizedModel(tuple(weights), tuple(biases))


def verify_bundle(parsed: ParsedBundle) -> dict:
    cpu_sections = [section for section in parsed.sections if section.type == SECTION_CPU]
    vectors_verified = 0
    arithmetic_vectors_verified = 0
    if cpu_sections:
        cpu_payload = cpu_sections[0].payload
        cpu_binary_offset = struct.unpack_from("<I", cpu_payload, 160)[0]
        model = _parse_cpu_binary(cpu_payload[cpu_binary_offset:])
        vectors = parsed.provenance.get("test_vectors", {})
        if not isinstance(vectors, dict):
            raise BundleError("malformed provenance test-vector registry")
        for vector in vectors.values():
            if not isinstance(vector, dict) or not all(name in vector for name in
                    ("input_q4", "output_q4", "score_q8", "anomaly")) or \
                    not isinstance(vector["input_q4"], list) or \
                    not isinstance(vector["output_q4"], list) or \
                    len(vector["input_q4"]) != 24 or len(vector["output_q4"]) != 24 or \
                    isinstance(vector["score_q8"], bool) or \
                    not isinstance(vector["score_q8"], int) or \
                    not isinstance(vector["anomaly"], bool):
                raise BundleError("malformed provenance test vector")
            if any(isinstance(value, bool) or not isinstance(value, int)
                   for value in vector["input_q4"] + vector["output_q4"]):
                raise BundleError("malformed provenance test vector value")
            qinput_values = vector["input_q4"]
            output_values = vector["output_q4"]
            if any(not -128 <= value <= 127 for value in qinput_values + output_values):
                raise BundleError("provenance test vector outside int8 range")
            qinput = np.asarray(qinput_values, dtype=np.int8)
            expected_output = np.asarray(output_values, dtype=np.int8)
            if not np.array_equal(model.infer(qinput), expected_output) or \
                    model.score(qinput) != vector["score_q8"] or \
                    (model.score(qinput) > parsed.threshold) != vector["anomaly"]:
                raise BundleError("CPU test-vector inference mismatch")
            vectors_verified += 1
    arithmetic = parsed.provenance.get("arithmetic_vectors")
    if arithmetic is not None:
        boundary = arithmetic.get("score_boundary") if isinstance(arithmetic, dict) else None
        if not isinstance(boundary, dict) or set(boundary) != {"below", "equal", "above"}:
            raise BundleError("malformed score boundary registry")
        for name, vector in boundary.items():
            if not isinstance(vector, dict) or not isinstance(vector.get("input_q4"), list) or \
                    not isinstance(vector.get("output_q4"), list) or \
                    len(vector["input_q4"]) != 24 or len(vector["output_q4"]) != 24:
                raise BundleError("malformed score boundary vector")
            if any(isinstance(value, bool) or not isinstance(value, int) or
                   not -128 <= value <= 127
                   for value in vector["input_q4"] + vector["output_q4"]):
                raise BundleError("score boundary vector outside int8 range")
            differences = [output - input_value for input_value, output in
                           zip(vector["input_q4"], vector["output_q4"])]
            squared_sum = sum(value * value for value in differences)
            score = (squared_sum + 12) // 24
            anomaly = score > parsed.threshold
            if squared_sum != vector.get("squared_sum") or score != vector.get("score_q8") or \
                    anomaly != vector.get("anomaly") or \
                    (name == "below" and not score < parsed.threshold) or \
                    (name == "equal" and score != parsed.threshold) or \
                    (name == "above" and not score > parsed.threshold):
                raise BundleError("score boundary arithmetic mismatch")
            arithmetic_vectors_verified += 1
    runtime_sections = [Section(section.type, section.flags, section.alignment,
        section.provider, section.payload, section.name) for section in parsed.sections
        if section.type in {SECTION_CPU, SECTION_NPU}]
    memory_plan = _memory_plan(len(parsed.raw), runtime_sections)
    return {
        "status": "verified", "bundle_sha256": hashlib.sha256(parsed.raw).hexdigest(),
        "bundle_size": len(parsed.raw), "target_id": parsed.target,
        "transport_id": parsed.transport, "accelerator_id": parsed.accelerator,
        "model_format": parsed.model_format, "profile_id": parsed.profile,
        "runtime_count": sum(section.type in {SECTION_CPU, SECTION_NPU}
                             for section in parsed.sections),
        "required_ram": memory_plan["required_ram"],
        "required_alignment": memory_plan["required_alignment"],
        "cpu_test_vectors_verified": vectors_verified,
        "arithmetic_vectors_verified": arithmetic_vectors_verified,
        "npu_semantic_equivalence": "deferred-to-Phase-4.3C-ST/RA-hardware-golden-vectors",
    }


def inspect_bundle(parsed: ParsedBundle) -> dict:
    runtimes = []
    runtime_sections = []
    for section in parsed.sections:
        if section.type not in {SECTION_CPU, SECTION_NPU}:
            continue
        runtime_sections.append(Section(section.type, section.flags,
            section.alignment, section.provider, section.payload, section.name))
        values = struct.unpack_from("<HHIIIIIHHBBbbIIIIIIII", section.payload)
        binary_offset = struct.unpack_from("<I", section.payload, 160)[0]
        inspected = {
            "runtime_type": "cpu-int8" if section.type == SECTION_CPU else "target-npu",
            "descriptor_version": values[0],
            "provider_id": values[2], "accelerator_id": values[3],
            "model_format": values[4], "model_version": values[5],
            "runtime_abi": values[6], "input_shape": [values[7]],
            "output_shape": [values[8]], "input_dtype": "int8",
            "output_dtype": "int8", "input_scale": f"{values[13]}/2^{values[14]}",
            "output_scale": f"{values[15]}/2^{values[16]}",
            "zero_points": [values[11], values[12]], "required_alignment": values[17],
            "persistent_memory": values[18], "scratch_memory": values[19],
            "model_binary_length": values[20],
            "model_binary_offset": binary_offset,
            "canonical_model_sha256": section.payload[64:96].hex(),
            "runtime_binary_sha256": section.payload[96:128].hex(),
            "conversion_manifest_sha256": section.payload[128:160].hex(),
        }
        if section.type == SECTION_NPU:
            inspected["regions"] = _parse_npu_regions(
                section.payload, binary_offset, values[20])
            inspected["runtime_version"] = [
                struct.unpack_from("<H", section.payload, 176)[0],
                struct.unpack_from("<H", section.payload, 178)[0]]
            inspected["runtime_variant"] = struct.unpack_from(
                "<I", section.payload, 180)[0]
            inspected["runtime_extra"] = struct.unpack_from(
                "<I", section.payload, 184)[0]
        runtimes.append(inspected)
    memory_plan = _memory_plan(len(parsed.raw), runtime_sections)
    return {
        "bundle_version": 1, "schema_id": SCHEMA_ID, "schema_version": 1,
        "schema_canonical_sha256": SCHEMA_HASH.hex(), "target_id": parsed.target,
        "transport_id": parsed.transport, "topology": list(DIMENSIONS),
        "profile_id": parsed.profile, "outer_accelerator_id": parsed.accelerator,
        "outer_model_format": parsed.model_format, "score_format": "Q8-MSE",
        "threshold_q8": parsed.threshold, "runtimes": runtimes,
        "required_ram": memory_plan["required_ram"],
        "required_alignment": memory_plan["required_alignment"],
        "memory_plan": memory_plan,
        "provenance_sha256": hashlib.sha256(next(
            section.payload for section in parsed.sections
            if section.type == SECTION_PROVENANCE)).hexdigest(),
        "bundle_sha256": hashlib.sha256(parsed.raw).hexdigest(),
        "plaintext_notice": "Unsealed model artifact; no secure deletion guarantee.",
    }
