"""Reproduce public Storage Sentinel training and canonical model artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


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
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


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
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    tool = Path(__file__).resolve().parent
    repo = tool.parents[1]
    output = args.output_root.resolve()
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

    if (args.stedgeai is None) != (args.st_reloc_profile is None):
        raise SystemExit("--stedgeai and --st-reloc-profile must be supplied together")
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
             args.st_reloc_profile_name]
        for path in args.st_tool_path:
            command.extend(["--tool-path", str(path.resolve())])
        run(command, repo)
        first["stm32n6570_dk"]["post_canonical"] = (
            "external proprietary tool required; output intentionally outside public artifacts")

    report = {"format": "mtfs-storage-sentinel-clean-reproduction-v1",
              "workspace_1": first, "workspace_2": second,
              "byte_identical_canonical_between_workspaces": True}
    (output / "reproduction-result.json").write_text(
        json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
