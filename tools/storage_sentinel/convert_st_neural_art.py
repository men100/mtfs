"""Create an ST Neural-ART runtime-loadable model outside the source tree."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

import numpy as np

from bundle import (PLACEMENT_CALLER_RELATIVE, PLACEMENT_FIXED_ABSOLUTE,
                    REGION_ACTIVATION, REGION_EXECUTABLE_COPY,
                    REGION_LIFETIME_INSTANCE, REGION_PARAMETERS)
from canonical_int8 import extract_tflite, rational_scale
from dataset import load_dataset, usable_vectors
from model import load_artifact
from schema import canonical_json_bytes, normalize

PROVIDER_ST_NEURAL_ART_RELOC = 0x53544E52  # STNR
ACCELERATOR_NEURAL_ART = 0x4E415254  # NART
MODEL_FORMAT_ST_RELOC = 0x5354524C  # STRL


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export_tflite(artifact: Path, training_dataset: Path, destination: Path) -> dict:
    try:
        import tensorflow as tf
    except ImportError as error:
        raise RuntimeError("TensorFlow is required for ST full-int8 export") from error
    reference, normalization, _, _ = load_artifact(artifact)
    if reference.dimensions != [24, 12, 4, 12, 24]:
        raise RuntimeError("unexpected Sentinel topology")
    dataset = load_dataset(training_dataset)
    raw, _ = usable_vectors(dataset)
    representative = normalize(np.asarray(raw, dtype=np.float64),
        np.asarray(normalization["mean"], dtype=np.float64),
        np.asarray(normalization["std"], dtype=np.float64)).astype(np.float32)
    tf.keras.utils.set_random_seed(4303)
    inputs = tf.keras.Input(shape=(24,), batch_size=1, dtype=tf.float32,
                            name="sentinel_input")
    value = inputs
    layers = []
    for index, units in enumerate(reference.dimensions[1:]):
        layer = tf.keras.layers.Dense(units,
            activation="relu" if index != 3 else None, use_bias=True,
            name=f"dense_{index}")
        value = layer(value); layers.append(layer)
    model = tf.keras.Model(inputs=inputs, outputs=value)
    for layer, weight, bias in zip(layers, reference.weights, reference.biases):
        layer.set_weights([np.asarray(weight, dtype=np.float32),
                           np.asarray(bias, dtype=np.float32)])

    def calibration_rows():
        for row in representative:
            yield [row.reshape(1, 24)]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = calibration_rows
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    converted = converter.convert()
    destination.write_bytes(converted)
    interpreter = tf.lite.Interpreter(model_content=converted)
    interpreter.allocate_tensors()
    tensors = (interpreter.get_input_details()[0], interpreter.get_output_details()[0])
    return {"calibration_rows": int(representative.shape[0]),
            "input_scale": float(tensors[0]["quantization"][0]),
            "input_zero_point": int(tensors[0]["quantization"][1]),
            "output_scale": float(tensors[1]["quantization"][0]),
            "output_zero_point": int(tensors[1]["quantization"][1])}


def report_number(text: str, label: str) -> int:
    match = re.search(rf"^\s*{re.escape(label)}\s*=\s*([0-9,]+)", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing relocation report field: {label}")
    return int(match.group(1).replace(",", ""))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--training-dataset", required=True, type=Path)
    parser.add_argument("--input-tflite", type=Path,
        help="reuse a previously exported full-int8 TFLite after training-split checks")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--stedgeai", type=Path,
        default=Path(os.environ.get("STEDGEAI", "stedgeai")))
    parser.add_argument("--reloc-profile", required=True, type=Path)
    parser.add_argument("--reloc-profile-name", default="test-int2",
        help="named ST relocation profile in the profile JSON")
    parser.add_argument("--tool-path", action="append", default=[], type=Path,
        help="directory prepended to PATH (for GNU make/compiler/Git)")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit("output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)
    training_manifest = json.loads((args.artifact.resolve() /
        "training_manifest.json").read_text(encoding="utf-8"))
    training_hash = sha256(args.training_dataset.resolve())
    allowed_training_hashes = {str(item.get("dataset_sha256"))
        for item in training_manifest.get("train_sessions", [])}
    forbidden_hashes = {str(item.get("dataset_sha256")) for key in
        ("validation_sessions", "test_sessions", "evaluation_datasets")
        for item in training_manifest.get(key, [])}
    if training_hash not in allowed_training_hashes or training_hash in forbidden_hashes:
        raise SystemExit("representative dataset is not an artifact training session")
    model_path = output / "sentinel_st_int8.tflite"
    if args.input_tflite is None:
        tensor = export_tflite(args.artifact.resolve(),
                               args.training_dataset.resolve(), model_path)
        calibration_identity = {"sha256": training_hash,
            "split": "training", "session": next((item.get("session_id")
            for item in training_manifest.get("train_sessions", []) if
            item.get("dataset_sha256") == training_hash), None)}
    else:
        source_tflite = args.input_tflite.resolve()
        if not source_tflite.is_file():
            raise SystemExit("input TFLite does not exist")
        source_manifest_path = source_tflite.with_suffix(
            source_tflite.suffix + ".manifest.json")
        if not source_manifest_path.is_file():
            raise SystemExit("input TFLite manifest does not exist")
        source_manifest = json.loads(source_manifest_path.read_text(
            encoding="utf-8"))
        artifact_index = args.artifact.resolve() / "artifact_index.json"
        if source_manifest.get("heldout_read") is not False or \
                source_manifest.get("canonical_full_int8_tflite_sha256") != \
                sha256(source_tflite) or \
                source_manifest.get("source_artifact_index_sha256") != \
                sha256(artifact_index) or \
                source_manifest.get("source_model_sha256") != \
                sha256(args.artifact.resolve() / "model.json") or \
                source_manifest.get("preprocessing_policy_sha256") != \
                training_manifest.get("preprocessing_policy_sha256"):
            raise SystemExit("input TFLite provenance mismatch")
        shutil.copyfile(source_tflite, model_path)
        tensor = {"calibration_rows": int(source_manifest["calibration_rows"])}
        calibration_identity = {
            "artifact_index_sha256": sha256(artifact_index),
            "split": "training-artifact",
            "sessions": [item.get("session_id") for item in
                         training_manifest.get("train_sessions", [])],
        }
    generated = output / "generated"
    workspace = output / "workspace"
    command = [str(args.stedgeai), "generate", "-m", str(model_path),
        "--target", "stm32n6", "--st-neural-art",
        f"{args.reloc_profile_name}@{args.reloc_profile.resolve()}", "--reloc", "--workspace",
        str(workspace), "--output", str(generated), "--verbosity", "2"]
    environment = os.environ.copy()
    if args.tool_path:
        environment["PATH"] = os.pathsep.join(str(path.resolve())
            for path in args.tool_path) + os.pathsep + environment.get("PATH", "")
    version = subprocess.run([str(args.stedgeai), "--version"], check=True,
        capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
    if re.search(r"ST Edge AI Core v4\.0\.1-", version) is None:
        raise RuntimeError("ST Edge AI Core 4.0.1 is required")
    subprocess.run(command, check=True, env=environment)
    binary = generated / "network_rel.bin"
    reloc_json = generated / "network_generate_rel.json"
    quant_files = list(generated.glob("*_Q.json"))
    build = workspace / "network_npu_reloc_build"
    report_path = build / "network_binary_report.txt"
    params_raw = build / "network_reloc_mempools.raw"
    if not binary.is_file() or not reloc_json.is_file() or len(quant_files) != 1 or \
            not report_path.is_file() or not params_raw.is_file():
        raise RuntimeError("ST Edge AI did not emit the required relocation artifacts")
    report = report_path.read_text(encoding="utf-8", errors="replace")
    copy_size = report_number(report, "COPY size")
    params_size = report_number(report, "params size")
    acts_size = report_number(report, "acts size")
    binary_size = report_number(report, "binary file size")
    binary_bytes = binary.read_bytes()
    raw_bytes = params_raw.read_bytes()
    params_offset = binary_bytes.find(raw_bytes)
    if binary_size != len(binary_bytes) or params_offset < 0 or \
            binary_bytes.find(raw_bytes, params_offset + 1) >= 0:
        raise RuntimeError("cannot uniquely locate parameters in runtime binary")
    generated_info = json.loads(reloc_json.read_text(encoding="utf-8"))
    compiler_version = generated_info.get("compiler", {}).get("version")
    if not isinstance(compiler_version, dict) or tuple(compiler_version.get(key)
            for key in ("major", "minor", "patch")) != (1, 1, 3):
        raise RuntimeError("atonn 1.1.3 is required")
    descriptors = generated_info["reloc_binary_image"]["mempool_c_descriptors"]
    parameter_pools = [pool for pool in descriptors
        if pool["flags_desc"].startswith("COPY.PARAM.")]
    activation_pools = [pool for pool in descriptors
        if pool["flags_desc"].startswith("RESET.ACTIV.")]
    if len(parameter_pools) != 1 or not activation_pools or \
            sum(int(pool["size"]) for pool in activation_pools) != acts_size:
        raise RuntimeError("unexpected internal relocation memory-pool contract")
    parameter_pool = parameter_pools[0]
    if int(parameter_pool["size"]) != params_size or int(parameter_pool["foff"]) != 0:
        raise RuntimeError("unexpected copied parameter descriptor")
    quant = json.loads(quant_files[0].read_text(encoding="utf-8"))["model_info"]
    input_format = quant["original_inputs"][0]["data_format"]
    output_format = quant["original_outputs"][0]["data_format"]
    input_num, input_shift = rational_scale(float(input_format["scale"][0]))
    output_num, output_shift = rational_scale(float(output_format["scale"][0]))
    canonical = extract_tflite(model_path)
    boundary_tensors = []
    for index, layer in enumerate(canonical.layers):
        boundary_tensors.append({"tensor": index, "shape": [int(layer.weights.shape[1])],
            "dtype": "int8", "scale_float32_hex": struct.pack("<f", layer.input_scale).hex(),
            "zero_point": layer.input_zero_point})
    boundary_tensors.append({"tensor": len(canonical.layers), "shape": [24],
        "dtype": "int8", "scale_float32_hex":
        struct.pack("<f", canonical.output_scale).hex(),
        "zero_point": canonical.output_zero_point})
    source_float_hash = sha256(args.artifact.resolve() / "model.json")
    threshold_document = json.loads((args.artifact.resolve() /
        "threshold.json").read_text(encoding="utf-8"))
    threshold_quantile = float(threshold_document.get("quantile", 0.95))
    if not 0.0 < threshold_quantile <= 1.0 or \
            threshold_document.get("selection_data") != "normal-validation-only":
        raise RuntimeError("invalid threshold provenance")
    manifest = {
        "format": "mtfs-sentinel-st-neural-art-reloc-v2",
        "provider_id": PROVIDER_ST_NEURAL_ART_RELOC,
        "accelerator_id": ACCELERATOR_NEURAL_ART,
        "model_format": MODEL_FORMAT_ST_RELOC, "model_version": 1,
        "runtime_abi": 0x00080000,
        "runtime_version_major": int(generated_info["compiler"]["version"]["major"]),
        "runtime_version_minor": int(generated_info["compiler"]["version"]["minor"]),
        "runtime_variant": struct.unpack_from("<I", binary_bytes, 4)[0],
        "runtime_extra": int(generated_info["compiler"]["version"]["build"]),
        "required_alignment": 8, "persistent_memory": copy_size,
        "scratch_memory": 0, "canonical_model_sha256": canonical.canonical_sha256.hex(),
        "runtime_binary_sha256": sha256(binary),
        "input_shape": [24], "output_shape": [24],
        "input_dtype": "int8", "output_dtype": "int8",
        "input_scale_numerator": input_num, "input_scale_shift": input_shift,
        "input_zero_point": int(input_format["zero"][0]),
        "output_scale_numerator": output_num, "output_scale_shift": output_shift,
        "output_zero_point": int(output_format["zero"][0]),
        "external_ram_size": 0,
        "memory_regions": [
            {"kind": REGION_EXECUTABLE_COPY, "placement": PLACEMENT_CALLER_RELATIVE,
             "logical_size": copy_size, "storage_size": copy_size, "alignment": 8,
             "address_or_offset": 0, "lifetime": REGION_LIFETIME_INSTANCE,
             "install_access": 3, "inference_access": 5, "requirements": 7},
            {"kind": REGION_PARAMETERS, "placement": PLACEMENT_FIXED_ABSOLUTE,
             "logical_size": params_size, "storage_size": params_size, "alignment": 8,
             "provider_pool_id": int(parameter_pool["flags_raw"], 16) & 0xff,
             "address_or_offset": int(parameter_pool["dst"], 16),
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 3,
             "inference_access": 1, "requirements": 7},
        ],
        "tool_versions": {"stedgeai": version,
            "atonn": generated_info["compiler"]["description"]},
        "source_tflite_sha256": sha256(model_path),
        "source_float_model_sha256": source_float_hash,
        "training_dataset_sha256": training_hash,
        "calibration_rows": tensor["calibration_rows"],
        "calibration_dataset_identity": calibration_identity,
        "input_output_shape": {"input": [24], "output": [24]},
        "tensor_dtype": "int8", "boundary_tensors": boundary_tensors,
        "quantization_contract_version": 2,
        "rounding_saturation_contract":
            "TFLite-int8-gemmlowp-rounding-fused-activation-signed-int8-saturation",
        "score_contract": "common-Q4-reconstruction-mean-squared-error-Q8",
        "threshold_provenance": {"source": training_manifest.get("threshold_source"),
            "selection": f"normal-validation-p{threshold_quantile * 100:g}-higher; "
                         "value supplied by bundle"},
        "generation_command": ["stedgeai", "generate", "-m", "sentinel_st_int8.tflite",
            "--target", "stm32n6", "--st-neural-art",
            f"{args.reloc_profile_name}@<reloc-profile>",
            "--reloc", "--workspace", "<workspace>", "--output", "<output>",
            "--verbosity", "2"],
    }
    for activation_pool in activation_pools:
        manifest["memory_regions"].append(
            {"kind": REGION_ACTIVATION, "placement": PLACEMENT_FIXED_ABSOLUTE,
             "logical_size": int(activation_pool["size"]),
             "storage_size": int(activation_pool["size"]), "alignment": 8,
             "provider_pool_id": int(activation_pool["flags_raw"], 16) & 0xff,
             "address_or_offset": int(activation_pool["dst"], 16),
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 2,
             "inference_access": 3, "requirements": 7})
    manifest["conversion_manifest_sha256"] = hashlib.sha256(
        canonical_json_bytes(manifest)).hexdigest()
    manifest_path = output / "sentinel_st_neural_art_manifest.json"
    manifest_path.write_bytes(canonical_json_bytes(manifest))
    print(json.dumps({"binary": str(binary), "manifest": str(manifest_path),
        "binary_size": len(binary_bytes), "binary_sha256": sha256(binary)},
        sort_keys=True))


if __name__ == "__main__":
    main()
