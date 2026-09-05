"""Create and inspect a reproducible RA TFLM/Ethos-U55 runtime artifact."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from bundle import (PLACEMENT_BINARY_CONTAINED, PLACEMENT_CALLER_RELATIVE,
                    PLACEMENT_PROVIDER_ASSIGNED, REGION_ACCESS_READ,
                    REGION_ACCESS_WRITE, REGION_ACCELERATOR_CONTEXT,
                    REGION_INTERPRETER, REGION_LIFETIME_INSTANCE,
                    REGION_LIFETIME_PROCESS, REGION_PROVIDER_CONTEXT,
                    REGION_PROVIDER_SYNC, REGION_REQUIRE_CACHE_COHERENCY,
                    REGION_REQUIRE_EXCLUSIVE, REGION_REQUIRE_INPUT_OUTPUT_SHARED,
                    REGION_REQUIRE_ZEROIZE, REGION_RESOLVER,
                    REGION_RUNTIME_BINARY, REGION_SEMAPHORE_POOL,
                    REGION_TENSOR_ARENA)
from canonical_int8 import rational_scale
from dataset import sha256_file
from schema import canonical_json_bytes

PROVIDER_RA_TFLM_ETHOSU = 0x52415446  # RATF
ACCELERATOR_ETHOS_U55 = 0x45553535  # EU55
MODEL_FORMAT_RA_TFLM_ETHOSU = 0x45555446  # EUTF
RUNTIME_ABI = 0x00190200  # TFLM/Ethos-U kernel 25.2
MODEL_VERSION = 2
INTERPRETER_LOGICAL = 196
INTERPRETER_STORAGE = 224
RESOLVER_LOGICAL = 48
RESOLVER_STORAGE = 64
TENSOR_ARENA = 4096
PROVIDER_CONTEXT = 512
ACCELERATOR_CONTEXT = 96
SEMAPHORE_POOL = 16
INTERPRETER_OFFSET = 0
RESOLVER_OFFSET = INTERPRETER_OFFSET + INTERPRETER_STORAGE
TENSOR_ARENA_OFFSET = RESOLVER_OFFSET + RESOLVER_STORAGE
PROVIDER_CONTEXT_OFFSET = TENSOR_ARENA_OFFSET + TENSOR_ARENA
PERSISTENT_MEMORY = PROVIDER_CONTEXT_OFFSET + PROVIDER_CONTEXT


def text(value) -> str:
    return value.decode("utf-8") if isinstance(value, bytes) else str(value)


def inspect_io(data: bytes) -> dict:
    try:
        from ethosu.vela.tflite.Model import Model
        from ethosu.vela.tflite.TensorType import TensorType
    except ImportError as error:
        raise RuntimeError("official ethos-u-vela 5.1.0 is required") from error
    model = Model.GetRootAsModel(data, 0)
    if model.Version() != 3 or model.SubgraphsLength() != 1:
        raise RuntimeError("TFLite schema/subgraph policy mismatch")
    graph = model.Subgraphs(0)
    if graph.InputsLength() != 1 or graph.OutputsLength() != 1:
        raise RuntimeError("TFLite input/output policy mismatch")
    input_tensor = graph.Tensors(graph.Inputs(0))
    output_tensor = graph.Tensors(graph.Outputs(0))
    if input_tensor.Type() != TensorType.INT8 or output_tensor.Type() != TensorType.INT8 or \
            input_tensor.ShapeAsNumpy().tolist() != [1, 24] or \
            output_tensor.ShapeAsNumpy().tolist() != [1, 24]:
        raise RuntimeError("TFLite input/output tensor policy mismatch")
    input_quant = input_tensor.Quantization()
    output_quant = output_tensor.Quantization()
    input_scales = input_quant.ScaleAsNumpy().tolist()
    output_scales = output_quant.ScaleAsNumpy().tolist()
    input_zero = input_quant.ZeroPointAsNumpy().tolist()
    output_zero = output_quant.ZeroPointAsNumpy().tolist()
    if len(input_scales) != 1 or len(output_scales) != 1 or \
            len(input_zero) != 1 or len(output_zero) != 1:
        raise RuntimeError("TFLite per-tensor quantization policy mismatch")
    return {"input_shape": [1, 24], "output_shape": [1, 24],
            "input_dtype": "int8", "output_dtype": "int8",
            "input_scale": float(input_scales[0]),
            "input_zero_point": int(input_zero[0]),
            "output_scale": float(output_scales[0]),
            "output_zero_point": int(output_zero[0])}


def inspect_optimized(data: bytes) -> dict:
    try:
        from ethosu.vela.tflite.BuiltinOperator import BuiltinOperator
        from ethosu.vela.tflite.Model import Model
    except ImportError as error:
        raise RuntimeError("official ethos-u-vela 5.1.0 is required") from error
    model = Model.GetRootAsModel(data, 0)
    if model.Version() != 3 or model.SubgraphsLength() != 1 or \
            model.OperatorCodesLength() != 1:
        raise RuntimeError("optimized TFLite schema/subgraph/operator-code policy mismatch")
    opcode = model.OperatorCodes(0)
    if opcode.BuiltinCode() != BuiltinOperator.CUSTOM or \
            text(opcode.CustomCode()) != "ethos-u":
        raise RuntimeError("optimized TFLite is not a single Ethos-U custom operator")
    graph = model.Subgraphs(0)
    if graph.OperatorsLength() != 1 or graph.InputsLength() != 1 or \
            graph.OutputsLength() != 1:
        raise RuntimeError("optimized TFLite graph policy mismatch")
    operation = graph.Operators(0)
    if operation.OpcodeIndex() != 0:
        raise RuntimeError("unexpected optimized TFLite opcode")
    io = inspect_io(data)
    named = {text(graph.Tensors(index).Name()): graph.Tensors(index)
             for index in range(graph.TensorsLength())}
    if "ethos_u_command_stream" not in named or "read_only" not in named or \
            "scratch" not in named:
        raise RuntimeError("optimized TFLite is missing Ethos-U buffers")
    command = model.Buffers(named["ethos_u_command_stream"].Buffer()).DataAsNumpy().tobytes()
    weights = model.Buffers(named["read_only"].Buffer()).DataAsNumpy().tobytes()
    scratch_shape = named["scratch"].ShapeAsNumpy().tolist()
    if not command or not weights or scratch_shape != [48]:
        raise RuntimeError("optimized TFLite command/weight/scratch policy mismatch")
    return {"schema_version": 3, "subgraph_count": 1, "operator_count": 1,
            "ethos_u_custom_operator_count": 1, "cpu_operator_count": 0,
            "command_stream_size": len(command), "weight_size": len(weights),
            "vela_scratch_size": 48, **io}


def run_vela(vela: Path, canonical: Path, output: Path) -> tuple[bytes, str, list[str]]:
    output.mkdir(parents=True)
    command = [str(vela), "--config", "Arm/vela.ini",
               "--accelerator-config=ethos-u55-256",
               "--optimise", "Performance", "--memory-mode", "Shared_Sram",
               "--system-config", "Ethos_U55_High_End_Embedded",
               "--output-dir", str(output), str(canonical)]
    completed = subprocess.run(command, check=True, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
    combined = completed.stdout + completed.stderr
    warnings = [line for line in combined.splitlines()
                if "warning" in line.lower() or "unsupported" in line.lower()]
    generated = output / f"{canonical.stem}_vela.tflite"
    if not generated.is_file():
        raise RuntimeError("Vela did not produce optimized TFLite")
    return generated.read_bytes(), combined, warnings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--canonical-tflite", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--vela", required=True, type=Path)
    args = parser.parse_args()
    artifact = args.artifact.resolve()
    canonical_path = args.canonical_tflite.resolve()
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit("output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)
    training = json.loads((artifact / "training_manifest.json").read_text("utf-8"))
    canonical_bytes = canonical_path.read_bytes()
    canonical_io = inspect_io(canonical_bytes)
    first, first_log, first_warnings = run_vela(args.vela.resolve(), canonical_path,
                                                output / "first")
    second, second_log, second_warnings = run_vela(args.vela.resolve(), canonical_path,
                                                   output / "second")
    if first != second:
        raise RuntimeError("independent Vela outputs are not byte-identical")
    if first_warnings or second_warnings:
        raise RuntimeError("Vela reported warning/unsupported output")
    structure = inspect_optimized(first)
    if any(structure[name] != canonical_io[name] for name in (
            "input_scale", "input_zero_point", "output_scale", "output_zero_point")):
        raise RuntimeError("Vela changed the external quantization contract")
    input_num, input_shift = rational_scale(canonical_io["input_scale"])
    output_num, output_shift = rational_scale(canonical_io["output_scale"])
    optimized_path = output / "sentinel_ra_ethosu.tflite"
    optimized_path.write_bytes(first)
    version = subprocess.run([str(args.vela.resolve()), "--version"], check=True,
        capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
    runtime_hash = hashlib.sha256(first).hexdigest()
    requirements = REGION_REQUIRE_ZEROIZE | REGION_REQUIRE_EXCLUSIVE
    manifest = {"format": "mtfs-sentinel-ra-tflm-ethosu-v1",
        "provider_id": PROVIDER_RA_TFLM_ETHOSU,
        "accelerator_id": ACCELERATOR_ETHOS_U55,
        "model_format": MODEL_FORMAT_RA_TFLM_ETHOSU,
        "model_version": MODEL_VERSION, "runtime_abi": RUNTIME_ABI,
        "runtime_version_major": 25, "runtime_version_minor": 2,
        "runtime_variant": 256, "runtime_extra": 0x00060500,
        "required_alignment": 32, "persistent_memory": PERSISTENT_MEMORY,
        "scratch_memory": 0,
        "canonical_model_sha256": hashlib.sha256(canonical_bytes).hexdigest(),
        "runtime_binary_sha256": runtime_hash,
        "input_shape": [1, 24], "output_shape": [1, 24],
        "input_dtype": "int8", "output_dtype": "int8",
        "input_scale_numerator": input_num, "input_scale_shift": input_shift,
        "input_zero_point": canonical_io["input_zero_point"],
        "output_scale_numerator": output_num, "output_scale_shift": output_shift,
        "output_zero_point": canonical_io["output_zero_point"],
        **structure,
        "arena_requirement": 580, "tensor_arena_size": TENSOR_ARENA,
        "acceptance_contract_version": 2,
        "tool_versions": {"vela": version, "tflm": "25.2.0",
                          "fsp": "6.5.0", "ethos_u_core_driver": "25.2.0"},
        "vela_configuration": {"accelerator_config": "ethos-u55-256",
            "optimise": "Performance", "memory_mode": "Shared_Sram",
            "system_config": "Ethos_U55_High_End_Embedded"},
        "source_float_model_sha256": sha256_file(artifact / "model.json"),
        "calibration_dataset_identity": training.get("train_sessions", []),
        "generation_reproducible": True,
        "memory_regions": [
            {"kind": REGION_RUNTIME_BINARY, "placement": PLACEMENT_BINARY_CONTAINED,
             "logical_size": len(first), "storage_size": len(first), "alignment": 32,
             "address_or_offset": 0, "lifetime": REGION_LIFETIME_INSTANCE,
             "install_access": REGION_ACCESS_READ, "inference_access": REGION_ACCESS_READ,
             "requirements": REGION_REQUIRE_ZEROIZE | REGION_REQUIRE_CACHE_COHERENCY},
            {"kind": REGION_INTERPRETER, "placement": PLACEMENT_CALLER_RELATIVE,
             "logical_size": INTERPRETER_LOGICAL, "storage_size": INTERPRETER_STORAGE,
             "alignment": 32, "address_or_offset": INTERPRETER_OFFSET,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 3,
             "inference_access": 3, "requirements": requirements},
            {"kind": REGION_RESOLVER, "placement": PLACEMENT_CALLER_RELATIVE,
             "logical_size": RESOLVER_LOGICAL, "storage_size": RESOLVER_STORAGE,
             "alignment": 32, "address_or_offset": RESOLVER_OFFSET,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 3,
             "inference_access": 3, "requirements": requirements},
            {"kind": REGION_TENSOR_ARENA, "placement": PLACEMENT_CALLER_RELATIVE,
             "logical_size": TENSOR_ARENA, "storage_size": TENSOR_ARENA,
             "alignment": 32, "address_or_offset": TENSOR_ARENA_OFFSET,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 3,
             "inference_access": 3, "requirements": requirements |
                 REGION_REQUIRE_CACHE_COHERENCY | REGION_REQUIRE_INPUT_OUTPUT_SHARED},
            {"kind": REGION_PROVIDER_CONTEXT, "placement": PLACEMENT_CALLER_RELATIVE,
             "logical_size": PROVIDER_CONTEXT, "storage_size": PROVIDER_CONTEXT,
             "alignment": 32, "address_or_offset": PROVIDER_CONTEXT_OFFSET,
             "lifetime": REGION_LIFETIME_INSTANCE, "install_access": 3,
             "inference_access": 3, "requirements": requirements},
            {"kind": REGION_ACCELERATOR_CONTEXT, "placement": PLACEMENT_PROVIDER_ASSIGNED,
             "logical_size": ACCELERATOR_CONTEXT, "storage_size": ACCELERATOR_CONTEXT,
             "alignment": 8, "provider_pool_id": 1, "address_or_offset": 0,
             "lifetime": REGION_LIFETIME_PROCESS, "install_access": 3,
             "inference_access": 3, "requirements": REGION_REQUIRE_EXCLUSIVE},
            {"kind": REGION_SEMAPHORE_POOL, "placement": PLACEMENT_PROVIDER_ASSIGNED,
             "logical_size": SEMAPHORE_POOL, "storage_size": SEMAPHORE_POOL,
             "alignment": 4, "provider_pool_id": 1, "address_or_offset": 0,
             "lifetime": REGION_LIFETIME_PROCESS, "install_access": 3,
             "inference_access": 3, "requirements": REGION_REQUIRE_EXCLUSIVE},
            {"kind": REGION_PROVIDER_SYNC, "placement": PLACEMENT_PROVIDER_ASSIGNED,
             "logical_size": 4, "storage_size": 4, "alignment": 4,
             "provider_pool_id": 1, "address_or_offset": 0,
             "lifetime": REGION_LIFETIME_PROCESS, "install_access": 3,
             "inference_access": 3, "requirements": REGION_REQUIRE_EXCLUSIVE},
        ],
    }
    manifest["conversion_manifest_sha256"] = hashlib.sha256(
        canonical_json_bytes(manifest)).hexdigest()
    (output / "sentinel_ra_ethosu_manifest.json").write_bytes(
        canonical_json_bytes(manifest))
    (output / "vela-first.log").write_text(first_log, encoding="utf-8")
    (output / "vela-second.log").write_text(second_log, encoding="utf-8")
    shutil.rmtree(output / "first")
    shutil.rmtree(output / "second")
    print(json.dumps({"optimized_tflite": str(optimized_path),
        "manifest": str(output / "sentinel_ra_ethosu_manifest.json"),
        "runtime_binary_sha256": runtime_hash, "structure": structure},
        indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
