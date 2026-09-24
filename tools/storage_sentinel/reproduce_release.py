"""Reproduce public Storage Sentinel training and canonical model artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import subprocess
import sys
import tempfile
from pathlib import Path

from provenance import assert_path_neutral_tree


EXPECTED = {
    "ek_ra8p1": {
        "model": "be3274a243dd29b48095edc078bba55d3b9e08c2483ed9bbba8a4b4e1040136f",
        "canonical": "bfa7ec20dc2056707ffaea572cd3353a4287a6cec4aa44b24e6057379e2541a8",
        "vela": "cacc6a87cb737dac257547dfb4f04db4fc74e61cd2190fb89c1688885ccd04a4",
        "bundle": "c5847706e211b0f231f3a346505652dda6122b201ef60480cdb7b38023eae830",
    },
    "stm32n6570_dk": {
        "model": "10033d658be9efa8c62a8f9fcac37a5f9502f701197f42979e2fdf4dcc14d246",
        "canonical": "6f49ab9572c4c160e9b00da8f679144263417124929424dbd65d1269a43344b2",
        "runtime_size": 8760,
        "runtime": "dfbe8918b53f6570b81d73b468a1ba6d78d9a250e4fb0c005e705d8f80140c6e",
        "artifact_index": "b27f6bde9199ea3cf9271c1533d0ee0e67a87c02860a2a186c0c353419fc19c9",
        "manifest": "a037c2365228fd7bca2d9d2d2deb37788c26bcde283aa944a6b98569446ac09b",
        "acceptance_file": "36d9651e58c43e4188131e254746f9afa2b9d53a8430a47c31207d11219043a3",
        "bundle": "c58d9f7a022f65ad586425d68a709389073c842987682c39b9c1c3d55a723926",
        "bundle_size": 24868,
        "required_ram": 32592,
    },
}

ST_TARGET = 0x53544E36
ST_TRANSPORT = 0x49444D41
ST_ACCELERATOR = 0x4E415254
SENTINEL_BUNDLE_FORMAT = 0x534E5431
ST_PROFILE_NAME = "test-int2"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def run_capture(command: list[str], cwd: Path,
                expect_success: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, cwd=cwd, check=False,
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if expect_success and completed.returncode != 0:
        raise RuntimeError(
            f"command failed with exit {completed.returncode}: {Path(command[0]).name}\n"
            f"{completed.stderr.strip()}")
    if not expect_success and completed.returncode == 0:
        raise RuntimeError(f"negative verification unexpectedly passed: {Path(command[0]).name}")
    return completed


def load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON object required: {path.name}")
    return value


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def verify_st_identity(canonical_path: Path, runtime_path: Path,
                       manifest_path: Path, acceptance_path: Path,
                       profile_name: str) -> dict:
    expected = EXPECTED["stm32n6570_dk"]
    if profile_name != ST_PROFILE_NAME:
        raise RuntimeError("only the accepted test-int2 relocation profile is supported")
    if sha256(canonical_path) != expected["canonical"]:
        raise RuntimeError("STM32 canonical TFLite hash mismatch")
    if runtime_path.stat().st_size != expected["runtime_size"] or \
            sha256(runtime_path) != expected["runtime"]:
        raise RuntimeError("STM32 accepted runtime size/hash mismatch")
    manifest = load_json(manifest_path)
    tool_versions = manifest.get("tool_versions", {})
    calibration_identity = manifest.get("calibration_dataset_identity", {})
    if manifest.get("canonical_model_sha256") != expected["canonical"] or \
            manifest.get("source_tflite_sha256") != expected["canonical"] or \
            manifest.get("runtime_binary_sha256") != expected["runtime"] or \
            manifest.get("conversion_manifest_sha256") != expected["manifest"] or \
            calibration_identity.get("artifact_index_sha256") != \
                expected["artifact_index"]:
        raise RuntimeError("STM32 conversion manifest identity mismatch")
    if not str(tool_versions.get("stedgeai", "")).startswith(
            "ST Edge AI Core v4.0.1-20581 ") or \
            not str(tool_versions.get("atonn", "")).startswith(
                "atonn-v1.1.3-275-") or \
            (manifest.get("runtime_version_major"),
             manifest.get("runtime_version_minor"),
             manifest.get("runtime_extra")) != (1, 1, 275):
        raise RuntimeError("STM32 accepted ST Edge AI Core/atonn version mismatch")
    command = manifest.get("generation_command", [])
    if f"{ST_PROFILE_NAME}@<reloc-profile>" not in command:
        raise RuntimeError("STM32 conversion manifest profile mismatch")
    assert_path_neutral_tree(manifest)
    if sha256(acceptance_path) != expected["acceptance_file"]:
        raise RuntimeError("public numerical acceptance contract file mismatch")
    acceptance = load_json(acceptance_path)
    if acceptance.get("status") != "PASS" or \
            acceptance.get("canonical_full_int8_tflite_sha256") != expected["canonical"] or \
            acceptance.get("npu_runtime_binary_sha256") != expected["runtime"] or \
            acceptance.get("conversion_manifest_sha256") != expected["manifest"]:
        raise RuntimeError("public numerical acceptance identity mismatch")
    assert_path_neutral_tree(acceptance)
    return {"runtime_size": runtime_path.stat().st_size,
            "runtime_sha256": expected["runtime"],
            "conversion_manifest_sha256": expected["manifest"],
            "acceptance_contract_sha256": expected["acceptance_file"],
            "stedgeai": "4.0.1-20581", "atonn": "1.1.3-275",
            "relocation_profile": ST_PROFILE_NAME}


def verify_bundle_summary(bundle: Path, pack_summary: dict,
                           inspected: dict) -> dict:
    expected = EXPECTED["stm32n6570_dk"]
    assert_path_neutral_tree(pack_summary)
    assert_path_neutral_tree(inspected)
    required = {
        "bundle_sha256": expected["bundle"],
        "bundle_size": expected["bundle_size"],
        "target_id": ST_TARGET,
        "transport_id": ST_TRANSPORT,
        "accelerator_id": ST_ACCELERATOR,
        "model_format": SENTINEL_BUNDLE_FORMAT,
        "runtime_count": 2,
        "minimum_required_ram_32bit": expected["required_ram"],
        "canonical_full_int8_tflite_sha256": expected["canonical"],
    }
    for key, value in required.items():
        if pack_summary.get(key) != value:
            raise RuntimeError(f"generated bundle {key} mismatch")
    inspected_required = {
        "target_id": ST_TARGET, "transport_id": ST_TRANSPORT,
        "outer_accelerator_id": ST_ACCELERATOR,
        "outer_model_format": SENTINEL_BUNDLE_FORMAT,
        "required_ram": expected["required_ram"],
    }
    for key, value in inspected_required.items():
        if inspected.get(key) != value:
            raise RuntimeError(f"inner bundle {key} mismatch")
    if len(inspected.get("runtimes", [])) != 2 or \
            sha256(bundle) != expected["bundle"]:
        raise RuntimeError("inner bundle runtime count/hash mismatch")
    npu = next((item for item in inspected["runtimes"]
        if item.get("runtime_type") == "target-npu"), None)
    if npu is None or npu.get("accelerator_id") != ST_ACCELERATOR or \
            npu.get("runtime_binary_sha256") != expected["runtime"] or \
            npu.get("conversion_manifest_sha256") != expected["manifest"]:
        raise RuntimeError("inner bundle NPU identity mismatch")
    return required


def seal_and_verify(repo: Path, seal_executable: Path, fleet_key: Path,
                    sealed_output: Path, bundle: Path, summary: dict) -> dict:
    seal = seal_executable.resolve()
    verify = seal.with_name("mtfs-verify" + seal.suffix)
    unseal = seal.with_name("mtfs-unseal" + seal.suffix)
    key = fleet_key.resolve()
    output = sealed_output.resolve()
    if not seal.is_file() or not verify.is_file() or not unseal.is_file():
        raise RuntimeError("mtfs-seal, mtfs-verify, and mtfs-unseal must be built together")
    if not key.is_file() or key.stat().st_size != 32:
        raise RuntimeError("fleet key must be an explicit 32-byte file")
    if output.exists():
        raise RuntimeError("sealed output exists; refusing overwrite")
    output.parent.mkdir(parents=True, exist_ok=True)
    policy = ["--target-id", str(summary["target_id"]),
              "--accelerator-id", str(summary["accelerator_id"]),
              "--model-format", str(summary["model_format"])]
    run_capture([str(seal), "--key", str(key), "--input", str(bundle),
        "--output", str(output), *policy, "--required-ram",
        str(summary["minimum_required_ram_32bit"])], repo)
    run_capture([str(verify), "--key", str(key), "--input", str(output),
        *policy], repo)
    temporary_paths: list[Path] = []
    try:
        def temporary_path(suffix: str) -> Path:
            descriptor, name = tempfile.mkstemp(prefix=".mtfs-reproduce-",
                suffix=suffix, dir=output.parent)
            os.close(descriptor)
            path = Path(name)
            path.unlink()
            temporary_paths.append(path)
            return path

        recovered = temporary_path(".bundle")
        wrong_key = temporary_path(".key")
        mutation = temporary_path(".mtfs")
        rejected_plaintext = temporary_path(".rejected")
        wrong_key.write_bytes(secrets.token_bytes(32))
        mutated = bytearray(output.read_bytes())
        mutated[-1] ^= 1
        mutation.write_bytes(mutated)
        run_capture([str(verify), "--key", str(wrong_key), "--input",
            str(output), *policy], repo, expect_success=False)
        run_capture([str(verify), "--key", str(key), "--input",
            str(mutation), *policy], repo, expect_success=False)
        run_capture([str(unseal), "--key", str(key), "--input",
            str(mutation), "--output", str(rejected_plaintext), *policy], repo,
            expect_success=False)
        if rejected_plaintext.exists():
            raise RuntimeError("authentication failure left partial plaintext")
        run_capture([str(unseal), "--key", str(key), "--input", str(output),
            "--output", str(recovered), *policy], repo)
        if recovered.read_bytes() != bundle.read_bytes():
            raise RuntimeError("unsealed payload differs from generated bundle")
    finally:
        for path in temporary_paths:
            try:
                path.unlink()
            except FileNotFoundError:
                pass
    return {"sealed_size": output.stat().st_size,
            "sealed_sha256": sha256(output),
            "seal_self_verify": "PASS", "verify": "PASS",
            "unseal_inner_comparison": "PASS", "wrong_key": "REJECT",
            "mutation": "REJECT", "partial_plaintext": "NONE"}


def train_command(tool: Path, repo: Path, target: str, output: Path) -> list[str]:
    root = repo / "artifacts" / "storage_sentinel"
    dataset = root / "datasets" / target
    reference = root / "reference" / target / "preprocessing-freeze-candidate.json"
    if target == "ek_ra8p1":
        training = [
            dataset / "training/card-a/ra-carda-normal-01.csv",
            dataset / "training/card-a/ra-carda-normal-02.csv",
            dataset / "training/card-a/ra-carda-normal-03.csv",
            dataset / "training/card-b/ra-cardb-normal-01.csv",
            dataset / "training/card-b/ra-cardb-normal-02.csv",
        ]
        validation = [
            dataset / "validation/card-c/ra-cardc-normal-01.csv",
            dataset / "validation/card-c/ra-cardc-normal-02.csv",
        ]
        pseudo = dataset / "validation/card-c/ra-cardc-pseudo-01.csv"
        limits = ["--threshold-quantile", "0.95"]
    else:
        training = [
            dataset / "training/card-a/st3-card-a-normal-01.csv",
            dataset / "training/card-a/st3-card-a-normal-02.csv",
            dataset / "training/card-b/st3-card-b-normal-01.csv",
            dataset / "training/card-b/st3-card-b-normal-02.csv",
        ]
        validation = [
            dataset / "validation/card-c/st3-card-c-normal-01.csv",
            dataset / "validation/card-c/st3-card-c-normal-02.csv",
        ]
        pseudo = dataset / "validation/card-c/st3-card-c-pseudo-01.csv"
        limits = [
            "--threshold-quantile", "0.99",
            "--maximum-training-ood-rate", "0.01",
            "--maximum-normal-ood-rate", "0.01",
            "--maximum-normal-false-warning-rate", "0.01",
            "--minimum-medium-safe-detection-rate", "0.5",
            "--minimum-strong-safe-detection-rate", "0.75",
            "--minimum-recovery-normal-rate", "0.95",
        ]
    command = [sys.executable, str(tool / "train_baseline_v2.py"),
               "--preprocessing", str(reference), "--output-dir", str(output),
               "--provenance-root", str(repo), "--seed", "4303", "--epochs", "200"]
    for path in training:
        command.extend(["--training-normal", str(path)])
    for path in validation:
        command.extend(["--validation-normal", str(path)])
    command.extend(["--validation-pseudo", str(pseudo), *limits])
    return command


def reproduce_workspace(repo: Path, tool: Path, workspace: Path) -> dict:
    result: dict[str, dict] = {}
    for target in ("ek_ra8p1", "stm32n6570_dk"):
        target_dir = workspace / target
        training = target_dir / "training"
        target_dir.mkdir(parents=True)
        run(train_command(tool, repo, target, training), repo)
        model_hash = sha256(training / "model.json")
        if model_hash != EXPECTED[target]["model"]:
            raise RuntimeError(f"{target} evaluated model hash mismatch: {model_hash}")
        canonical = target_dir / "sentinel_baseline_v2.tflite"
        run([sys.executable, str(tool / "export_baseline_v2_tflite.py"),
             "--artifact", str(training), "--output", str(canonical)], repo)
        canonical_hash = sha256(canonical)
        if canonical_hash != EXPECTED[target]["canonical"]:
            raise RuntimeError(f"{target} canonical TFLite hash mismatch: {canonical_hash}")
        result[target] = {"model_sha256": model_hash,
                          "canonical_tflite_sha256": canonical_hash}
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--output-root", type=Path, required=True)
    result.add_argument("--vela", type=Path)
    result.add_argument("--vela-python", type=Path,
                        help="Python environment containing ethos-u-vela 5.1.0")
    result.add_argument("--stedgeai", type=Path)
    result.add_argument("--st-reloc-profile", type=Path)
    result.add_argument("--st-reloc-profile-name", default="test-int2")
    result.add_argument("--st-tool-path", type=Path, action="append", default=[],
                        help="GNU rm/compiler directory prepended for ST relocation build")
    result.add_argument("--mtfs-seal", type=Path,
                        help="built mtfs-seal; sibling verify/unseal tools are used")
    result.add_argument("--fleet-key", type=Path,
                        help="explicit 32-byte fleet key used only by mtfs-seal tools")
    result.add_argument("--sealed-output", type=Path,
                        help="output SENTINEL.MTF path outside the repository")
    return result


def validate_st_inputs(repo: Path, output: Path, stedgeai: Path | None,
                       profile: Path | None, profile_name: str,
                       sealed_output: Path | None) -> None:
    if (stedgeai is None) != (profile is None):
        raise SystemExit("--stedgeai and --st-reloc-profile must be supplied together")
    if stedgeai is None:
        return
    if profile_name != ST_PROFILE_NAME:
        raise SystemExit("only --st-reloc-profile-name test-int2 is supported")
    if not stedgeai.resolve().is_file():
        raise SystemExit("ST Edge AI Core executable does not exist")
    resolved_profile = profile.resolve()
    expected_suffix = Path("scripts/N6_reloc/test/neural_art_reloc.json")
    suffix_parts = [part.lower() for part in expected_suffix.parts]
    if not resolved_profile.is_file() or \
            [part.lower() for part in
             resolved_profile.parts[-len(suffix_parts):]] != suffix_parts:
        raise SystemExit(
            "relocation profile must be the vendor-bundled "
            "scripts/N6_reloc/test/neural_art_reloc.json")
    if is_within(output, repo):
        raise SystemExit("ST generated output root must be outside the repository")
    if sealed_output is not None and is_within(sealed_output.resolve(), repo):
        raise SystemExit("SENTINEL.MTF output must be outside the repository")


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    tool = Path(__file__).resolve().parent
    repo = tool.parents[1]
    output = args.output_root.resolve()
    seal_options = (args.mtfs_seal, args.fleet_key, args.sealed_output)
    if any(value is not None for value in seal_options) and \
            not all(value is not None for value in seal_options):
        raise SystemExit(
            "--mtfs-seal, --fleet-key, and --sealed-output must be supplied together")
    if any(value is not None for value in seal_options) and args.stedgeai is None:
        raise SystemExit("sealed ST output requires --stedgeai and --st-reloc-profile")
    validate_st_inputs(repo, output, args.stedgeai, args.st_reloc_profile,
                       args.st_reloc_profile_name, args.sealed_output)
    if output.exists() and any(output.iterdir()):
        raise SystemExit("output root must be empty")
    output.mkdir(parents=True, exist_ok=True)
    first = reproduce_workspace(repo, tool, output / "workspace-1")
    second = reproduce_workspace(repo, tool, output / "workspace-2")
    if first != second:
        raise RuntimeError("independent workspace outputs are not byte-identical")

    if args.vela is not None:
        if args.vela_python is None:
            raise SystemExit("--vela-python is required with --vela")
        ra = output / "workspace-1" / "ek_ra8p1"
        vela_output = ra / "vela"
        run([str(args.vela_python.resolve()), str(tool / "convert_ra_ethosu.py"),
             "--artifact", str(ra / "training"), "--canonical-tflite",
             str(ra / "sentinel_baseline_v2.tflite"), "--output-dir",
             str(vela_output), "--vela", str(args.vela.resolve())], repo)
        if sha256(vela_output / "sentinel_ra_ethosu.tflite") != EXPECTED[
                "ek_ra8p1"]["vela"]:
            raise RuntimeError("RA Vela output hash mismatch")
        bundle = ra / "sentinel.bundle"
        run([sys.executable, str(tool / "pack.py"), "--artifact",
             str(ra / "training"), "--canonical-tflite",
             str(ra / "sentinel_baseline_v2.tflite"), "--output", str(bundle),
             "--npu-binary", str(vela_output / "sentinel_ra_ethosu.tflite"),
             "--npu-manifest", str(vela_output / "sentinel_ra_ethosu_manifest.json"),
             "--npu-acceptance", str(repo / "artifacts/storage_sentinel/reference/ek_ra8p1/freeze/ra-ethosu-acceptance-v2r2.path-neutral-rebind.json"),
             "--provenance-root", str(repo)], repo)
        if sha256(bundle) != EXPECTED["ek_ra8p1"]["bundle"]:
            raise RuntimeError("RA bundle hash mismatch")
        first["ek_ra8p1"].update({"vela_tflite_sha256": EXPECTED["ek_ra8p1"]["vela"],
                                  "bundle_sha256": sha256(bundle)})

    if args.stedgeai is not None:
        st = output / "workspace-1" / "stm32n6570_dk"
        training_dataset = repo / (
            "artifacts/storage_sentinel/datasets/stm32n6570_dk/training/"
            "card-a/st3-card-a-normal-01.csv")
        command = [sys.executable, str(tool / "convert_st_neural_art.py"),
             "--artifact", str(st / "training"), "--training-dataset",
             str(training_dataset), "--input-tflite",
             str(st / "sentinel_baseline_v2.tflite"), "--output-dir",
             str(st / "stedgeai-private-output"), "--stedgeai",
             str(args.stedgeai.resolve()), "--reloc-profile",
             str(args.st_reloc_profile.resolve()), "--reloc-profile-name",
             args.st_reloc_profile_name,
             "--accepted-artifact-index-sha256",
             EXPECTED["stm32n6570_dk"]["artifact_index"]]
        for path in args.st_tool_path:
            command.extend(["--tool-path", str(path.resolve())])
        run(command, repo)
        st_private = st / "stedgeai-private-output"
        runtime = st_private / "generated" / "network_rel.bin"
        manifest = st_private / "sentinel_st_neural_art_manifest.json"
        acceptance = repo / (
            "artifacts/storage_sentinel/reference/stm32n6570_dk/freeze/"
            "npu-acceptance-v3-compact.json")
        identity = verify_st_identity(st / "sentinel_baseline_v2.tflite",
            runtime, manifest, acceptance, args.st_reloc_profile_name)
        bundle = st / "sentinel.bundle"
        packed = run_capture([sys.executable, str(tool / "pack.py"),
            "--artifact", str(st / "training"), "--canonical-tflite",
            str(st / "sentinel_baseline_v2.tflite"), "--output", str(bundle),
            "--npu-binary", str(runtime), "--npu-manifest", str(manifest),
            "--npu-acceptance", str(acceptance), "--accelerator-id",
            hex(ST_ACCELERATOR), "--provenance-root", str(repo)], repo)
        pack_summary = json.loads(packed.stdout)
        inspected_result = run_capture([sys.executable,
            str(tool / "inspect_bundle_cli.py"), str(bundle)], repo)
        inspected = json.loads(inspected_result.stdout)
        bundle_identity = verify_bundle_summary(bundle, pack_summary, inspected)
        st_result = {**identity, **bundle_identity,
            "post_canonical":
                "vendor-generated output outside the public repository"}
        if args.mtfs_seal is not None:
            st_result["sealed_package"] = seal_and_verify(repo,
                args.mtfs_seal, args.fleet_key, args.sealed_output,
                bundle, pack_summary)
        first["stm32n6570_dk"].update(st_result)

    report = {"format": "mtfs-storage-sentinel-clean-reproduction-v1",
              "workspace_1": first, "workspace_2": second,
              "byte_identical_canonical_between_workspaces": True}
    (output / "reproduction-result.json").write_text(
        json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
