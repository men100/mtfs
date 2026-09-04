from __future__ import annotations

import hashlib
import json
import random
import tempfile
import unittest
from pathlib import Path

import numpy as np

from bundle import (ACCELERATOR_CPU_REFERENCE, MODEL_FORMAT_CPU_INT8_V1,
                    PROVIDER_CPU_REFERENCE, SECTION_COMPATIBILITY, SECTION_CPU,
                    SECTION_DECISION, SECTION_NORMALIZATION, SECTION_NPU,
                    SECTION_PROVENANCE,
                    SECTION_REQUIRED, BundleError, QuantizedModel, Section,
                    _assemble, _compatibility, _cpu_binary, _decision,
                    _memory_plan, _normalization, _runtime_descriptor, atomic_write,
                    fixed_normalize, inspect_bundle, parse_bundle, verify_bundle, _npu_runtime,
                    _npu_runtime_descriptor_v2, REGION_EXECUTABLE_COPY,
                    REGION_ACTIVATION, REGION_PARAMETERS,
                    PLACEMENT_CALLER_RELATIVE, PLACEMENT_FIXED_ABSOLUTE,
                    PLACEMENT_BINARY_CONTAINED, REGION_LIFETIME_INSTANCE)
from schema import canonical_json_bytes


def fixture_bundle() -> bytes:
    shapes = ((24, 12), (12, 4), (4, 12), (12, 24))
    model = QuantizedModel(tuple(np.zeros(shape, dtype=np.int8) for shape in shapes),
                           tuple(np.zeros(shape[1], dtype=np.int32) for shape in shapes))
    binary = _cpu_binary(model)
    runtime = _runtime_descriptor(1, PROVIDER_CPU_REFERENCE,
        ACCELERATOR_CPU_REFERENCE, MODEL_FORMAT_CPU_INT8_V1, 1, 4, 32, 48,
        binary, bytes(32), bytes(32)) + binary
    normalization = {"mean_q16": [0] * 24, "inverse_std_q20": [1 << 20] * 24,
                     "output_q": 16, "int8_scale": 16}
    fixed = [
        Section(SECTION_COMPATIBILITY, SECTION_REQUIRED, 4, 0,
                _compatibility(0x52413850, 0x53504920, 7,
                               ACCELERATOR_CPU_REFERENCE), "compatibility"),
        Section(SECTION_NORMALIZATION, SECTION_REQUIRED, 8, 0,
                _normalization(normalization), "normalization"),
        Section(SECTION_DECISION, SECTION_REQUIRED, 8, 0, _decision(1), "decision"),
        Section(SECTION_CPU, SECTION_REQUIRED, 4, PROVIDER_CPU_REFERENCE,
                runtime, "cpu_runtime"),
    ]
    _, hashes = _assemble(fixed)
    provenance = canonical_json_bytes({
        "format": "mtfs-sentinel-provenance-v1",
        "schema_canonical_sha256":
            "01b0040533491d3f2d07248b345909447be004d4fcd8fc94719ec28d955094d8",
        "section_sha256": hashes,
        "test_vectors": {
            "equal": {"input_q4": [1] * 24, "output_q4": [0] * 24,
                      "score_q8": 1, "anomaly": False},
        },
    })
    result, _ = _assemble(fixed + [Section(SECTION_PROVENANCE, SECTION_REQUIRED,
                                          4, 0, provenance, "provenance")])
    return result


def fixture_dual_bundle(alignment: int = 16, persistent: int = 64,
                        scratch: int = 128, second_npu: bool = False,
                        include_cpu: bool = True,
                        npu_first: bool = False,
                        multiple_activations: bool = False) -> bytes:
    parsed = parse_bundle(fixture_bundle())
    fixed = []
    for section in parsed.sections:
        if section.type == SECTION_PROVENANCE:
            continue
        if section.type == SECTION_CPU and not include_cpu:
            continue
        payload = section.payload
        if section.type == SECTION_COMPATIBILITY:
            value = bytearray(payload)
            value[60:64] = (0x4E505531).to_bytes(4, "little")
            payload = bytes(value)
        fixed.append(Section(section.type, section.flags, section.alignment,
                             section.provider, payload, section.name))
    binary = b"synthetic-npu-v1"
    manifest = {
        "provider_id": 0x4E505250, "accelerator_id": 0x4E505531,
        "model_format": 0x564E4431, "model_version": 1,
        "runtime_abi": 0x00080000, "required_alignment": alignment,
        "persistent_memory": persistent, "scratch_memory": 0,
        "input_scale_numerator": 1, "input_scale_shift": 4,
        "input_zero_point": 0, "output_scale_numerator": 1,
        "output_scale_shift": 4, "output_zero_point": 0,
        "runtime_version_major": 8,
        "memory_regions": [
            {"kind": REGION_EXECUTABLE_COPY, "placement": PLACEMENT_CALLER_RELATIVE,
             "logical_size": persistent, "storage_size": persistent,
             "alignment": alignment, "address_or_offset": 0,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 3,
             "inference_access": 5, "requirements": 5},
            {"kind": REGION_ACTIVATION, "placement": PLACEMENT_FIXED_ABSOLUTE,
             "logical_size": 56, "alignment": 8,
             "address_or_offset": 0x342E0000,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 2,
             "inference_access": 3, "requirements": 21},
            {"kind": REGION_PARAMETERS, "placement": PLACEMENT_BINARY_CONTAINED,
             "logical_size": 8, "storage_size": 8, "alignment": 8,
             "address_or_offset": 8, "lifetime": REGION_LIFETIME_INSTANCE,
             "install_access": 1, "inference_access": 1},
        ],
    }
    if multiple_activations:
        manifest["memory_regions"].append(
            {"kind": REGION_ACTIVATION, "placement": PLACEMENT_FIXED_ABSOLUTE,
             "logical_size": 24, "alignment": 8,
             "address_or_offset": 0x34270000,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 2,
             "inference_access": 3, "requirements": 21})
    npu = _npu_runtime_descriptor_v2(manifest, binary, bytes(32), bytes(32)) + binary
    npu_section = Section(SECTION_NPU, SECTION_REQUIRED, alignment,
                          0x4E505250, npu, "npu_runtime_4e505250")
    if npu_first:
        runtime_index = next(index for index, section in enumerate(fixed)
                             if section.type == SECTION_CPU)
        fixed.insert(runtime_index, npu_section)
    else:
        fixed.append(npu_section)
    if second_npu:
        second_manifest = dict(manifest)
        second_manifest["provider_id"] = 0x4E505251
        fixed.append(Section(SECTION_NPU, SECTION_REQUIRED, alignment,
                             0x4E505251,
            _npu_runtime_descriptor_v2(second_manifest, binary, bytes(32),
                                       bytes(32)) + binary,
            "npu_runtime_4e505251"))
    _, hashes = _assemble(fixed)
    provenance = canonical_json_bytes({
        "format": "mtfs-sentinel-provenance-v1",
        "schema_canonical_sha256":
            "01b0040533491d3f2d07248b345909447be004d4fcd8fc94719ec28d955094d8",
        "section_sha256": hashes, "test_vectors": {},
    })
    result, _ = _assemble(fixed + [Section(SECTION_PROVENANCE,
        SECTION_REQUIRED, 4, 0, provenance, "provenance")])
    return result


class NumericContractTests(unittest.TestCase):
    def test_fixed_normalization_rounding_and_clamps(self):
        normalization = {"mean_q16": [0] * 24,
                         "inverse_std_q20": [1 << 20] * 24}
        raw = [0] * 24
        raw[0], raw[1] = 8, 9
        self.assertEqual(fixed_normalize(raw, normalization)[:2].tolist(), [127, 127])
        normalization["mean_q16"][2] = 8 << 16
        self.assertEqual(int(fixed_normalize(raw, normalization)[2]), -128)
        normalization["mean_q16"][3] = -(1 << 11)  # +0.5 Q4 tie => away to +1
        self.assertEqual(int(fixed_normalize(raw, normalization)[3]), 1)
        normalization["mean_q16"][4] = 1 << 11  # -0.5 Q4 tie => away to -1
        self.assertEqual(int(fixed_normalize(raw, normalization)[4]), -1)

    def test_cpu_score_threshold_and_output_saturation(self):
        parsed = parse_bundle(fixture_bundle())
        result = verify_bundle(parsed)
        self.assertEqual(result["cpu_test_vectors_verified"], 1)
        cpu = next(section for section in parsed.sections if section.type == SECTION_CPU)
        # The fixture model reconstructs zero, so 1 Q4 in every dimension is score 1 Q8.
        from bundle import _parse_cpu_binary, RUNTIME_DESCRIPTOR_SIZE
        binary_offset = int.from_bytes(cpu.payload[160:164], "little")
        model = _parse_cpu_binary(cpu.payload[binary_offset:])
        values = np.ones(24, dtype=np.int8)
        self.assertEqual(model.score(values), parsed.threshold)
        self.assertFalse(model.score(values) > parsed.threshold)
        values[0] = 4
        self.assertGreater(model.score(values), parsed.threshold)


class BundleParserTests(unittest.TestCase):
    def test_valid_and_outer_identity(self):
        raw = fixture_bundle()
        parsed = parse_bundle(raw, 0x52413850, 0x53504920,
                              ACCELERATOR_CPU_REFERENCE)
        self.assertEqual(parsed.profile, 7)
        with self.assertRaisesRegex(BundleError, "outer/inner"):
            parse_bundle(raw, expected_target=0x53544E36)

    def test_dual_runtime_identity_and_memory_plan(self):
        raw = fixture_dual_bundle()
        parsed = parse_bundle(raw, expected_accelerator=0x4E505531)
        runtimes = [Section(section.type, section.flags, section.alignment,
                            section.provider, section.payload, section.name)
                    for section in parsed.sections
                    if section.type in {SECTION_CPU, SECTION_NPU}]
        plan = _memory_plan(len(raw), runtimes)
        cursor = (len(raw) + 3) & ~3
        cpu_persistent = cursor
        cursor += 32
        cursor = (cursor + 15) & ~15
        npu_persistent = cursor
        cursor += 64
        cursor = (cursor + 15) & ~15
        scratch_offset = cursor
        cursor += 48
        self.assertEqual(plan, {
            "required_alignment": 16,
            "persistent_offsets": [cpu_persistent, npu_persistent],
            "scratch_offset": scratch_offset, "scratch_size": 48,
            "required_ram": cursor,
        })
        reversed_raw = fixture_dual_bundle(npu_first=True)
        reversed_bundle = parse_bundle(reversed_raw,
                                       expected_accelerator=0x4E505531)
        reversed_runtimes = [Section(
            section.type, section.flags, section.alignment, section.provider,
            section.payload, section.name) for section in reversed_bundle.sections
            if section.type in {SECTION_CPU, SECTION_NPU}]
        reversed_plan = _memory_plan(len(reversed_raw), reversed_runtimes)
        self.assertEqual([section.provider for section in reversed_runtimes],
                         [0x4E505250, PROVIDER_CPU_REFERENCE])
        reversed_cursor = (len(reversed_raw) + 15) & ~15
        npu_persistent = reversed_cursor
        reversed_cursor += 64
        cpu_persistent = reversed_cursor
        reversed_cursor += 32
        scratch_offset = reversed_cursor
        reversed_cursor += 48
        self.assertEqual(reversed_plan, {
            "required_alignment": 16,
            "persistent_offsets": [npu_persistent, cpu_persistent],
            "scratch_offset": scratch_offset, "scratch_size": 48,
            "required_ram": reversed_cursor,
        })
        with self.assertRaisesRegex(BundleError, "outer/inner"):
            parse_bundle(raw, expected_accelerator=ACCELERATOR_CPU_REFERENCE)
        npu_only = parse_bundle(fixture_dual_bundle(include_cpu=False),
                                expected_accelerator=0x4E505531)
        self.assertEqual(sum(section.type == SECTION_NPU
                             for section in npu_only.sections), 1)
        self.assertFalse(any(section.type == SECTION_CPU
                             for section in npu_only.sections))
        npu_only_runtimes = [Section(
            section.type, section.flags, section.alignment, section.provider,
            section.payload, section.name) for section in npu_only.sections
            if section.type in {SECTION_CPU, SECTION_NPU}]
        npu_only_plan = _memory_plan(len(npu_only.raw), npu_only_runtimes)
        self.assertEqual(npu_only_runtimes[0].provider, 0x4E505250)
        self.assertEqual(len(npu_only_plan["persistent_offsets"]), 1)
        larger = parse_bundle(fixture_dual_bundle(32, 100, 200))
        larger_runtimes = [Section(section.type, section.flags, section.alignment,
            section.provider, section.payload, section.name)
            for section in larger.sections if section.type in {SECTION_CPU, SECTION_NPU}]
        larger_plan = _memory_plan(len(larger.raw), larger_runtimes)
        self.assertEqual(larger_plan["required_alignment"], 32)
        self.assertEqual(larger_plan["scratch_size"], 48)
        self.assertGreater(larger_plan["required_ram"], plan["required_ram"])

    def test_distinct_fixed_activation_regions_are_valid(self):
        inspected = inspect_bundle(parse_bundle(
            fixture_dual_bundle(multiple_activations=True)))
        npu = next(runtime for runtime in inspected["runtimes"]
                   if runtime["runtime_type"] == "target-npu")
        activations = [region for region in npu["regions"]
                       if region["kind"] == REGION_ACTIVATION]
        self.assertEqual([region["address_or_offset"] for region in activations],
                         [0x342E0000, 0x34270000])

    def test_embedded_contract_runtime_negatives(self):
        raw = fixture_dual_bundle()
        npu_index = entry_index(raw, SECTION_NPU)
        npu_offset = next_offset(raw, SECTION_NPU)

        def changed(offset: int, data: bytes) -> bytes:
            value = bytearray(raw); value[offset:offset + len(data)] = data
            return bytes(value)

        duplicate_provider = bytearray(raw)
        duplicate_provider[32 + npu_index * 32 + 16:
                           32 + npu_index * 32 + 20] = \
            PROVIDER_CPU_REFERENCE.to_bytes(4, "little")
        duplicate_provider[npu_offset + 4:npu_offset + 8] = \
            PROVIDER_CPU_REFERENCE.to_bytes(4, "little")
        with self.assertRaisesRegex(BundleError, "duplicate runtime provider"):
            parse_bundle(bytes(duplicate_provider))
        with self.assertRaisesRegex(BundleError, "canonical model hash"):
            parse_bundle(changed(npu_offset + 64, b"\x01"))
        with self.assertRaisesRegex(BundleError, "runtime count"):
            parse_bundle(fixture_dual_bundle(second_npu=True))
        with self.assertRaisesRegex(BundleError, "runtime region table"):
            parse_bundle(changed(npu_offset + 164, b"\x01"))
        with self.assertRaisesRegex(BundleError, "runtime manifest"):
            parse_bundle(changed(npu_offset + 160, (193).to_bytes(4, "little")))
        with self.assertRaisesRegex(BundleError, "overflows"):
            parse_bundle(fixture_dual_bundle(persistent=0xffffffff))

    def test_npu_region_table_negative_matrix(self):
        raw = fixture_dual_bundle()
        runtime = next_offset(raw, SECTION_NPU)
        first = runtime + 192
        second = first + 64
        third = second + 64

        def changed(*updates: tuple[int, bytes]) -> bytes:
            value = bytearray(raw)
            for offset, data in updates:
                value[offset:offset + len(data)] = data
            return bytes(value)

        malformed = {
            "region-count-zero": changed((runtime + 168, b"\0\0")),
            "region-count-too-large": changed((runtime + 168, (17).to_bytes(2, "little"))),
            "entry-size": changed((runtime + 170, (56).to_bytes(2, "little"))),
            "table-offset": changed((runtime + 164, (200).to_bytes(4, "little"))),
            "binary-offset-before-table": changed((runtime + 160, (128).to_bytes(4, "little"))),
            "alignment": changed((first + 16, (3).to_bytes(4, "little"))),
            "fixed-overflow": changed(
                (second + 24, (0xfffffffffffffff8).to_bytes(8, "little")),
                (second + 48, (56).to_bytes(8, "little"))),
            "overlap": changed(
                (third + 4, PLACEMENT_CALLER_RELATIVE.to_bytes(2, "little")),
                (third + 24, (32).to_bytes(8, "little"))),
            "binary-contained-out-of-range": changed(
                (third + 24, (15).to_bytes(8, "little"))),
            "duplicate-fixed": changed(
                (third + 2, REGION_ACTIVATION.to_bytes(2, "little")),
                (third + 4, PLACEMENT_FIXED_ABSOLUTE.to_bytes(2, "little")),
                (third + 24, (0x342e0000).to_bytes(8, "little"))),
            "unknown-required-region": changed((third + 2, (0xffff).to_bytes(2, "little"))),
            "entry-reserved": changed((first + 56, b"\x01")),
            "header-reserved": changed((runtime + 188, b"\x01")),
        }
        for name, candidate in malformed.items():
            with self.subTest(name=name), self.assertRaises(BundleError):
                parse_bundle(candidate)

    def test_malformed_corpus(self):
        raw = fixture_bundle()
        cases: dict[str, bytes] = {}

        def changed(offset: int, data: bytes) -> bytes:
            value = bytearray(raw); value[offset:offset + len(data)] = data
            return bytes(value)

        cases["magic"] = changed(0, b"X")
        cases["version"] = changed(8, b"\x02\x00")
        cases["truncated"] = raw[:20]
        cases["total"] = changed(12, b"\xff\xff\xff\xff")
        cases["count"] = changed(16, b"\xff\xff")
        cases["nonzero reserved"] = changed(24, b"\x01")
        cases["overlap"] = changed(32 + 32 + 4, (193).to_bytes(4, "little"))
        cases["offset overflow"] = changed(32 + 4, (0xfffffff0).to_bytes(4, "little"))
        cases["misalignment"] = changed(32 + 4, (193).to_bytes(4, "little"))
        cases["zero length"] = changed(32 + 8, bytes(4))
        compat = next_offset(raw, SECTION_COMPATIBILITY)
        norm = next_offset(raw, SECTION_NORMALIZATION)
        cpu = next_offset(raw, SECTION_CPU)
        cases["schema version"] = changed(compat + 2, b"\x02")
        cases["schema hash"] = changed(compat + 8, b"\xff")
        cases["schema id"] = changed(compat + 72, b"X")
        cases["target"] = changed(compat + 40, (0x53544e36).to_bytes(4, "little"))
        cases["transport"] = changed(compat + 44, (0x49444d41).to_bytes(4, "little"))
        cases["topology"] = changed(compat + 48, bytes(4))
        cases["normalization count"] = changed(norm + 2, b"\x17\x00")
        cases["normalization scale"] = changed(norm + 208, bytes(4))
        cases["tensor dtype"] = changed(
            cpu + 28, b"\x02")
        cases["tensor zero point"] = changed(cpu + 30, b"\x01")
        cases["tensor scale"] = changed(cpu + 32, bytes(4))
        cases["tensor shape"] = changed(cpu + 192 + 12, b"\x17\x00")
        cases["runtime hash"] = changed(
            cpu + 192 + 100, b"\x01")
        cases["section hash"] = changed(norm + 16, b"\x01")
        cases["threshold"] = changed(
            next_offset(raw, SECTION_DECISION) + 24, bytes(8))
        for name, value in cases.items():
            with self.subTest(name=name), self.assertRaises(BundleError):
                parse_bundle(value)

    def test_unknown_required_and_optional(self):
        raw = fixture_bundle()
        value = bytearray(raw)
        # Rename compatibility to an unknown required section.
        value[32:34] = (0x7777).to_bytes(2, "little")
        with self.assertRaisesRegex(BundleError, "unknown mandatory"):
            parse_bundle(bytes(value))
        # An unknown optional entry is skipped safely when it replaces no mandatory section.
        # Replace the CPU section type and clear REQUIRED: no runtime remains and is rejected.
        value = bytearray(raw)
        cpu_entry = entry_index(raw, SECTION_CPU)
        value[32 + cpu_entry * 32:34 + cpu_entry * 32] = (0x7777).to_bytes(2, "little")
        value[34 + cpu_entry * 32:36 + cpu_entry * 32] = bytes(2)
        with self.assertRaisesRegex(BundleError, "runtime"):
            parse_bundle(bytes(value))
        parsed = parse_bundle(raw)
        rebuilt = [Section(section.type, section.flags, section.alignment,
                           section.provider, section.payload, section.name)
                   for section in parsed.sections[:-1]]
        rebuilt.append(Section(0x7777, 0, 1, 0, b"optional", "optional_7777"))
        provenance = parsed.sections[-1]
        rebuilt.append(Section(provenance.type, provenance.flags,
                               provenance.alignment, provenance.provider,
                               provenance.payload, provenance.name))
        optional, _ = _assemble(rebuilt)
        self.assertEqual(parse_bundle(optional).profile, 7)

    def test_duplicate_missing_and_truncated_sections(self):
        parsed = parse_bundle(fixture_bundle())
        sections = [Section(section.type, section.flags, section.alignment,
                            section.provider, section.payload, section.name)
                    for section in parsed.sections]
        duplicate, _ = _assemble(sections[:-1] + [sections[0], sections[-1]])
        with self.assertRaisesRegex(BundleError, "duplicate singleton"):
            parse_bundle(duplicate)
        missing, _ = _assemble([section for section in sections
                                if section.type != SECTION_NORMALIZATION])
        with self.assertRaisesRegex(BundleError, "missing mandatory"):
            parse_bundle(missing)
        no_runtime, _ = _assemble([section for section in sections
                                   if section.type != SECTION_CPU])
        with self.assertRaisesRegex(BundleError, "runtime"):
            parse_bundle(no_runtime)
        truncated_cpu = [Section(section.type, section.flags, section.alignment,
                           section.provider,
                           section.payload[:-1] if section.type == SECTION_CPU else section.payload,
                           section.name) for section in sections]
        malformed, _ = _assemble(truncated_cpu)
        with self.assertRaises(BundleError):
            parse_bundle(malformed)

    def test_deterministic_mutation_fuzz_smoke(self):
        original = fixture_bundle()
        generator = random.Random(4303)
        for _ in range(1000):
            value = bytearray(original)
            for _ in range(generator.randrange(1, 5)):
                offset = generator.randrange(len(value))
                value[offset] ^= 1 << generator.randrange(8)
            if generator.randrange(8) == 0:
                value = value[:generator.randrange(len(value) + 1)]
            try:
                verify_bundle(parse_bundle(bytes(value)))
            except BundleError:
                pass

    def test_no_overwrite_and_determinism(self):
        self.assertEqual(fixture_bundle(), fixture_bundle())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sentinel.bundle"
            atomic_write(path, fixture_bundle())
            self.assertEqual(hashlib.sha256(path.read_bytes()).digest(),
                             hashlib.sha256(fixture_bundle()).digest())
            with self.assertRaisesRegex(BundleError, "refusing overwrite"):
                atomic_write(path, fixture_bundle())

    def test_npu_runtime_manifest_contract_without_formal_dummy_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "test-only.bin"
            manifest_path = root / "test-only.json"
            binary.write_bytes(b"synthetic-parser-fixture")
            manifest = {
                "provider_id": 0x53544E50, "accelerator_id": 0x4E415254,
                "model_format": 0x56454E44, "model_version": 1,
                "runtime_abi": 0x00080000,
                "required_alignment": 16, "persistent_memory": 64,
                "scratch_memory": 0, "canonical_model_sha256": bytes(32).hex(),
                "runtime_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "input_shape": [24], "output_shape": [24],
                "input_dtype": "int8", "output_dtype": "int8",
                "input_scale_numerator": 1, "input_scale_shift": 4,
                "input_zero_point": 0, "output_scale_numerator": 1,
                "output_scale_shift": 4, "output_zero_point": 0,
                "runtime_version_major": 8,
                "memory_regions": [
                    {"kind": REGION_EXECUTABLE_COPY,
                     "placement": PLACEMENT_CALLER_RELATIVE,
                     "logical_size": 64, "alignment": 16,
                     "address_or_offset": 0, "lifetime": REGION_LIFETIME_INSTANCE,
                     "install_access": 3, "inference_access": 5},
                    {"kind": REGION_ACTIVATION,
                     "placement": PLACEMENT_FIXED_ABSOLUTE,
                     "logical_size": 56, "alignment": 8,
                     "address_or_offset": 0x342E0000,
                     "lifetime": REGION_LIFETIME_INSTANCE,
                     "install_access": 2, "inference_access": 3},
                    {"kind": REGION_PARAMETERS,
                     "placement": PLACEMENT_BINARY_CONTAINED,
                     "logical_size": 8, "alignment": 8,
                     "address_or_offset": 8, "lifetime": REGION_LIFETIME_INSTANCE,
                     "install_access": 1, "inference_access": 1},
                ],
            }
            manifest["conversion_manifest_sha256"] = hashlib.sha256(
                canonical_json_bytes(manifest)).hexdigest()
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            provider, runtime, returned = _npu_runtime(binary, manifest_path, bytes(32))
            self.assertEqual(provider, manifest["provider_id"])
            self.assertEqual(returned["input_shape"], [24])
            self.assertGreater(len(runtime), 192)
            manifest["input_shape"] = [23]
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(BundleError, "tensor descriptor"):
                _npu_runtime(binary, manifest_path, bytes(32))


def entry_index(raw: bytes, section_type: int) -> int:
    count = int.from_bytes(raw[16:18], "little")
    for index in range(count):
        if int.from_bytes(raw[32 + index * 32:34 + index * 32], "little") == section_type:
            return index
    raise AssertionError(section_type)


def next_offset(raw: bytes, section_type: int) -> int:
    index = entry_index(raw, section_type)
    return int.from_bytes(raw[32 + index * 32 + 4:32 + index * 32 + 8], "little")


if __name__ == "__main__":
    unittest.main()
