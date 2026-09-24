from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import reproduce_release as reproduction


class StReproductionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.canonical = self.root / "sentinel_baseline_v2.tflite"
        self.runtime = self.root / "network_rel.bin"
        self.manifest = self.root / "sentinel_st_neural_art_manifest.json"
        self.acceptance = self.root / "npu-acceptance-v3-compact.json"
        self.canonical.write_bytes(b"canonical")
        self.runtime.write_bytes(bytes(8760))
        expected = reproduction.EXPECTED["stm32n6570_dk"]
        self.manifest_value = {
            "canonical_model_sha256": expected["canonical"],
            "source_tflite_sha256": expected["canonical"],
            "runtime_binary_sha256": expected["runtime"],
            "conversion_manifest_sha256": expected["manifest"],
            "calibration_dataset_identity": {
                "artifact_index_sha256": expected["artifact_index"]},
            "runtime_version_major": 1,
            "runtime_version_minor": 1,
            "runtime_extra": 275,
            "generation_command": [
                "stedgeai", "generate", "test-int2@<reloc-profile>"],
            "tool_versions": {
                "stedgeai": "ST Edge AI Core v4.0.1-20581 build",
                "atonn": "atonn-v1.1.3-275-g123"},
        }
        self.acceptance_value = {
            "status": "PASS",
            "canonical_full_int8_tflite_sha256": expected["canonical"],
            "npu_runtime_binary_sha256": expected["runtime"],
            "conversion_manifest_sha256": expected["manifest"],
        }
        self._write_json()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write_json(self) -> None:
        self.manifest.write_text(json.dumps(self.manifest_value), encoding="utf-8")
        self.acceptance.write_text(json.dumps(self.acceptance_value), encoding="utf-8")

    def _hash(self, path: Path) -> str:
        expected = reproduction.EXPECTED["stm32n6570_dk"]
        return {self.canonical: expected["canonical"],
                self.runtime: expected["runtime"],
                self.acceptance: expected["acceptance_file"]}[path]

    def test_accepted_identity_and_fail_closed_matrix(self) -> None:
        with mock.patch.object(reproduction, "sha256", side_effect=self._hash):
            result = reproduction.verify_st_identity(
                self.canonical, self.runtime, self.manifest, self.acceptance,
                "test-int2")
            self.assertEqual(result["runtime_size"], 8760)
            with self.assertRaisesRegex(RuntimeError, "test-int2"):
                reproduction.verify_st_identity(
                    self.canonical, self.runtime, self.manifest,
                    self.acceptance, "custom")

        with mock.patch.object(reproduction, "sha256", return_value="00" * 32):
            with self.assertRaisesRegex(RuntimeError, "canonical"):
                reproduction.verify_st_identity(
                    self.canonical, self.runtime, self.manifest,
                    self.acceptance, "test-int2")

        self.manifest_value["conversion_manifest_sha256"] = "00" * 32
        self._write_json()
        with mock.patch.object(reproduction, "sha256", side_effect=self._hash):
            with self.assertRaisesRegex(RuntimeError, "manifest identity"):
                reproduction.verify_st_identity(
                    self.canonical, self.runtime, self.manifest,
                    self.acceptance, "test-int2")

        self.manifest_value["conversion_manifest_sha256"] = \
            reproduction.EXPECTED["stm32n6570_dk"]["manifest"]
        self.acceptance_value["npu_runtime_binary_sha256"] = "11" * 32
        self._write_json()
        with mock.patch.object(reproduction, "sha256", side_effect=self._hash):
            with self.assertRaisesRegex(RuntimeError, "acceptance identity"):
                reproduction.verify_st_identity(
                    self.canonical, self.runtime, self.manifest,
                    self.acceptance, "test-int2")

    def test_absolute_path_is_rejected(self) -> None:
        self.manifest_value["private_path"] = "C:\\vendor\\secret"
        self._write_json()
        with mock.patch.object(reproduction, "sha256", side_effect=self._hash):
            with self.assertRaises(Exception):
                reproduction.verify_st_identity(
                    self.canonical, self.runtime, self.manifest,
                    self.acceptance, "test-int2")

    def test_st_tool_profile_preflight_is_fail_closed(self) -> None:
        repo = self.root / "repo"
        repo.mkdir()
        output = self.root / "output"
        tool = self.root / "stedgeai"
        tool.write_bytes(b"tool")
        profile = (self.root / "vendor" / "scripts" / "N6_reloc" /
                   "test" / "neural_art_reloc.json")
        profile.parent.mkdir(parents=True)
        profile.write_text("{}", encoding="utf-8")

        with self.assertRaisesRegex(SystemExit, "must be supplied together"):
            reproduction.validate_st_inputs(
                repo, output, tool, None, "test-int2", None)
        with self.assertRaisesRegex(SystemExit, "does not exist"):
            reproduction.validate_st_inputs(
                repo, output, self.root / "missing-stedgeai", profile,
                "test-int2", None)
        with self.assertRaisesRegex(SystemExit, "only .*test-int2"):
            reproduction.validate_st_inputs(
                repo, output, tool, profile, "custom", None)
        wrong_profile = self.root / "neural_art_reloc.json"
        wrong_profile.write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "vendor-bundled"):
            reproduction.validate_st_inputs(
                repo, output, tool, wrong_profile, "test-int2", None)
        reproduction.validate_st_inputs(
            repo, output, tool, profile, "test-int2", None)

    def test_bundle_identity_positive_and_mismatch(self) -> None:
        expected = reproduction.EXPECTED["stm32n6570_dk"]
        bundle = self.root / "sentinel.bundle"
        bundle.write_bytes(b"bundle")
        summary = {
            "bundle_sha256": expected["bundle"],
            "bundle_size": expected["bundle_size"],
            "target_id": reproduction.ST_TARGET,
            "transport_id": reproduction.ST_TRANSPORT,
            "accelerator_id": reproduction.ST_ACCELERATOR,
            "model_format": reproduction.SENTINEL_BUNDLE_FORMAT,
            "runtime_count": 2,
            "minimum_required_ram_32bit": expected["required_ram"],
            "canonical_full_int8_tflite_sha256": expected["canonical"],
        }
        inspected = {
            "target_id": reproduction.ST_TARGET,
            "transport_id": reproduction.ST_TRANSPORT,
            "outer_accelerator_id": reproduction.ST_ACCELERATOR,
            "outer_model_format": reproduction.SENTINEL_BUNDLE_FORMAT,
            "required_ram": expected["required_ram"],
            "runtimes": [
                {"runtime_type": "cpu-int8"},
                {"runtime_type": "target-npu",
                 "accelerator_id": reproduction.ST_ACCELERATOR,
                 "runtime_binary_sha256": expected["runtime"],
                 "conversion_manifest_sha256": expected["manifest"]},
            ],
        }
        with mock.patch.object(reproduction, "sha256", return_value=expected["bundle"]):
            reproduction.verify_bundle_summary(bundle, summary, inspected)
            changed = dict(summary)
            changed["target_id"] ^= 1
            with self.assertRaisesRegex(RuntimeError, "target_id"):
                reproduction.verify_bundle_summary(bundle, changed, inspected)

    def test_optional_seal_verifies_and_cleans_negative_artifacts(self) -> None:
        tool_dir = self.root / "tools"
        tool_dir.mkdir()
        for name in ("mtfs-seal", "mtfs-verify", "mtfs-unseal"):
            (tool_dir / name).write_bytes(b"tool")
        key = self.root / "fleet.key"
        key.write_bytes(bytes(range(32)))
        bundle = self.root / "sentinel.bundle"
        bundle.write_bytes(b"verified-inner-bundle")
        output = self.root / "SENTINEL.MTF"
        summary = {"target_id": reproduction.ST_TARGET,
            "accelerator_id": reproduction.ST_ACCELERATOR,
            "model_format": reproduction.SENTINEL_BUNDLE_FORMAT,
            "minimum_required_ram_32bit":
                reproduction.EXPECTED["stm32n6570_dk"]["required_ram"]}

        def fake_run(command, cwd, expect_success=True):
            executable = Path(command[0]).name
            if executable == "mtfs-seal":
                Path(command[command.index("--output") + 1]).write_bytes(b"sealed")
            elif executable == "mtfs-unseal" and expect_success:
                Path(command[command.index("--output") + 1]).write_bytes(
                    bundle.read_bytes())
            return subprocess.CompletedProcess(command, 0 if expect_success else 1,
                                               stdout="", stderr="")

        with mock.patch.object(reproduction, "run_capture", side_effect=fake_run):
            result = reproduction.seal_and_verify(
                self.root, tool_dir / "mtfs-seal", key, output,
                bundle, summary)
        self.assertEqual(result["unseal_inner_comparison"], "PASS")
        self.assertEqual(result["wrong_key"], "REJECT")
        self.assertFalse(any(path.name.startswith(".mtfs-reproduce-")
                             for path in self.root.iterdir()))
        with self.assertRaisesRegex(RuntimeError, "refusing overwrite"):
            reproduction.seal_and_verify(
                self.root, tool_dir / "mtfs-seal", key, output,
                bundle, summary)


if __name__ == "__main__":
    unittest.main()
