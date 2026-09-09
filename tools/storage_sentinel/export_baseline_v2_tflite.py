"""Export the frozen-candidate baseline-relative model as canonical full-int8 TFLite."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

from canonical_int8 import extract_tflite
from dataset import sha256_file
from model import DenseAutoencoder
from schema import DatasetError, canonical_json_bytes


def _atomic_new(path: Path, payload: bytes) -> None:
    if path.exists():
        raise DatasetError(f"output exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp",
                                        dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.rename(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _verify_artifact(artifact: Path) -> dict:
    index = json.loads((artifact / "artifact_index.json").read_text(
        encoding="utf-8"))
    if index.get("format") != "mtfs-sentinel-artifact-index-v2" or \
            index.get("heldout_read") is not False:
        raise DatasetError("artifact is not an unopened v2 candidate")
    hashes = index.get("files_sha256")
    if not isinstance(hashes, dict):
        raise DatasetError("artifact hash registry missing")
    for name, expected in hashes.items():
        path = artifact / name
        if not path.is_file() or sha256_file(path) != expected:
            raise DatasetError(f"artifact hash mismatch: {name}")
    return index


def run(args: argparse.Namespace) -> dict:
    # Pin TensorFlow's host-side conversion behavior before importing it.
    os.environ.setdefault("TF_ENABLE_ONEDNN_OPTS", "0")
    try:
        import tensorflow as tf
    except ImportError as error:
        raise DatasetError("TensorFlow is required for canonical export") from error
    artifact = args.artifact.resolve()
    _verify_artifact(artifact)
    training = json.loads((artifact / "training_manifest.json").read_text(
        encoding="utf-8"))
    if training.get("heldout_read") is not False or \
            training.get("heldout_sessions") != []:
        raise DatasetError("held-out data entered the pre-freeze artifact")
    reference = DenseAutoencoder.from_dict(json.loads(
        (artifact / "model.json").read_text(encoding="utf-8")))
    if reference.dimensions != [24, 12, 4, 12, 24]:
        raise DatasetError("unexpected Sentinel topology")
    calibration = json.loads((artifact / "calibration_q4.json").read_text(
        encoding="utf-8"))
    q4 = np.asarray(calibration.get("input_q4"), dtype=np.int16)
    if q4.ndim != 2 or q4.shape[1] != 24 or np.any(q4 < -128) or \
            np.any(q4 > 127):
        raise DatasetError("invalid calibration q4 matrix")
    representative = (q4.astype(np.float32) / np.float32(16.0))
    tf.keras.utils.set_random_seed(int(training["seed"]))
    tf.config.experimental.enable_op_determinism()
    inputs = tf.keras.Input(shape=(24,), batch_size=1, dtype=tf.float32,
                            name="sentinel_baseline_relative_input")
    value = inputs
    layers = []
    for index, units in enumerate(reference.dimensions[1:]):
        layer = tf.keras.layers.Dense(
            units, activation="relu" if index != 3 else None,
            use_bias=True, name=f"dense_{index}")
        value = layer(value)
        layers.append(layer)
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
    _atomic_new(args.output.resolve(), converted)
    parsed = extract_tflite(args.output.resolve())
    manifest = {
        "format": "mtfs-sentinel-baseline-relative-canonical-int8-v2",
        "heldout_read": False,
        "source_artifact_index_sha256": sha256_file(
            artifact / "artifact_index.json"),
        "source_model_sha256": sha256_file(artifact / "model.json"),
        "preprocessing_policy_sha256": training["preprocessing_policy_sha256"],
        "calibration_rows": int(q4.shape[0]),
        "canonical_full_int8_tflite_sha256":
            hashlib.sha256(converted).hexdigest(),
        "canonical_identity_sha256": parsed.canonical_sha256.hex(),
        "input_scale_float32": parsed.input_scale,
        "input_zero_point": parsed.input_zero_point,
        "output_scale_float32": parsed.output_scale,
        "output_zero_point": parsed.output_zero_point,
    }
    manifest_path = args.output.resolve().with_suffix(
        args.output.resolve().suffix + ".manifest.json")
    _atomic_new(manifest_path, canonical_json_bytes(manifest) + b"\n")
    return manifest


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Export baseline-relative v2 canonical full-int8 TFLite")
    result.add_argument("--artifact", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        print(json.dumps(run(parser().parse_args(argv)), indent=2,
                         sort_keys=True, allow_nan=False))
        return 0
    except (DatasetError, OSError, ValueError, KeyError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
