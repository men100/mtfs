"""Canonical full-int8 TFLite extraction and heapless reference arithmetic.

TensorFlow is imported only by the artifact-generation/reference functions.  The
bundle parser and verifier can use the serialized CPU model without TensorFlow.
"""
from __future__ import annotations

import hashlib
import math
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np


DIMENSIONS = (24, 12, 4, 12, 24)
CPU_MAGIC = b"MTFSTI82"
CPU_FORMAT_VERSION = 2
QUANTIZATION_CONTRACT_VERSION = 2
CPU_HEADER_SIZE = 96
CPU_LAYER_DESCRIPTOR_SIZE = 48
ACTIVATION_NONE = 0
ACTIVATION_RELU = 1
ROUNDING_TFLITE_GEMMLOWP = 1
SATURATION_SIGNED_INT8 = 1


class CanonicalInt8Error(ValueError):
    pass


def _round_away_positive(value: float) -> int:
    if not math.isfinite(value) or value < 0:
        raise CanonicalInt8Error("invalid nonnegative value")
    return int(math.floor(value + 0.5))


def quantize_multiplier(real_multiplier: float) -> tuple[int, int]:
    """Equivalent to TFLite QuantizeMultiplier for a positive multiplier."""
    if not math.isfinite(real_multiplier) or real_multiplier <= 0:
        raise CanonicalInt8Error("invalid real multiplier")
    significand, shift = math.frexp(real_multiplier)
    multiplier = _round_away_positive(significand * (1 << 31))
    if multiplier == 1 << 31:
        multiplier //= 2
        shift += 1
    if shift < -31:
        return 0, 0
    if shift > 30 or not 0 <= multiplier <= 0x7fffffff:
        raise CanonicalInt8Error("quantized multiplier is outside TFLite range")
    return multiplier, shift


def _trunc_div(numerator: int, denominator: int) -> int:
    quotient = abs(numerator) // denominator
    return -quotient if numerator < 0 else quotient


def saturating_rounding_doubling_high_mul(a: int, b: int) -> int:
    if a == -(1 << 31) and b == -(1 << 31):
        return (1 << 31) - 1
    product = int(a) * int(b)
    nudge = (1 << 30) if product >= 0 else 1 - (1 << 30)
    return _trunc_div(product + nudge, 1 << 31)


def rounding_divide_by_pot(value: int, exponent: int) -> int:
    if not 0 <= exponent <= 31:
        raise CanonicalInt8Error("invalid right shift")
    if exponent == 0:
        return value
    mask = (1 << exponent) - 1
    remainder = value & mask
    threshold = (mask >> 1) + (1 if value < 0 else 0)
    return (value >> exponent) + (1 if remainder > threshold else 0)


def multiply_by_quantized_multiplier(value: int, multiplier: int, shift: int) -> int:
    left_shift = max(shift, 0)
    right_shift = max(-shift, 0)
    shifted = int(value) * (1 << left_shift)
    if not -(1 << 31) <= shifted < (1 << 31):
        raise CanonicalInt8Error("TFLite left shift overflow")
    return rounding_divide_by_pot(
        saturating_rounding_doubling_high_mul(shifted, multiplier), right_shift)


@dataclass(frozen=True)
class Layer:
    weights: np.ndarray
    biases: np.ndarray
    weight_scales: np.ndarray
    weight_zero_points: np.ndarray
    multipliers: np.ndarray
    shifts: np.ndarray
    input_scale: float
    input_zero_point: int
    output_scale: float
    output_zero_point: int
    activation: int


@dataclass(frozen=True)
class CanonicalInt8Model:
    canonical_sha256: bytes
    layers: tuple[Layer, ...]

    @property
    def input_scale(self) -> float:
        return self.layers[0].input_scale

    @property
    def input_zero_point(self) -> int:
        return self.layers[0].input_zero_point

    @property
    def output_scale(self) -> float:
        return self.layers[-1].output_scale

    @property
    def output_zero_point(self) -> int:
        return self.layers[-1].output_zero_point

    def infer_raw(self, values: np.ndarray) -> np.ndarray:
        current = np.asarray(values, dtype=np.int8)
        if current.shape != (DIMENSIONS[0],):
            raise CanonicalInt8Error("canonical input must contain 24 int8 values")
        for layer in self.layers:
            output_count, input_count = layer.weights.shape
            if current.size != input_count:
                raise CanonicalInt8Error("layer topology mismatch")
            result = np.empty(output_count, dtype=np.int8)
            for output_index in range(output_count):
                accumulator = int(layer.biases[output_index])
                weight_zero = int(layer.weight_zero_points[output_index])
                for input_index in range(input_count):
                    accumulator += ((int(current[input_index]) - layer.input_zero_point) *
                                    (int(layer.weights[output_index, input_index]) -
                                     weight_zero))
                if not -(1 << 31) <= accumulator < (1 << 31):
                    raise CanonicalInt8Error("int32 accumulator overflow")
                quantized = multiply_by_quantized_multiplier(
                    accumulator, int(layer.multipliers[output_index]),
                    int(layer.shifts[output_index])) + layer.output_zero_point
                if layer.activation == ACTIVATION_RELU:
                    quantized = max(quantized, layer.output_zero_point)
                result[output_index] = min(127, max(-128, quantized))
            current = result
        return current


def rational_scale(value: float) -> tuple[int, int]:
    if not math.isfinite(value) or value <= 0:
        raise CanonicalInt8Error("invalid tensor scale")
    shift = 31
    numerator = _round_away_positive(value * (1 << shift))
    if not 0 < numerator <= 0xffffffff:
        raise CanonicalInt8Error("tensor scale is outside descriptor range")
    return numerator, shift


def requantize_q4_to_int8(value: int, scale: float, zero_point: int) -> int:
    numerator, shift = rational_scale(scale)
    denominator = numerator * 16
    magnitude = abs(int(value)) << shift
    rounded = (magnitude + denominator // 2) // denominator
    signed = -rounded if value < 0 else rounded
    return min(127, max(-128, signed + zero_point))


def requantize_int8_to_q4(value: int, scale: float, zero_point: int) -> int:
    numerator, shift = rational_scale(scale)
    product = (int(value) - zero_point) * numerator * 16
    magnitude = abs(product)
    rounded = (magnitude + (1 << (shift - 1))) >> shift
    signed = -rounded if product < 0 else rounded
    return min(127, max(-128, signed))


def infer_q4(model: CanonicalInt8Model, input_q4: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    canonical_input = np.asarray([
        requantize_q4_to_int8(int(value), model.input_scale,
                              model.input_zero_point) for value in input_q4], dtype=np.int8)
    raw_output = model.infer_raw(canonical_input)
    output_q4 = np.asarray([
        requantize_int8_to_q4(int(value), model.output_scale,
                              model.output_zero_point) for value in raw_output], dtype=np.int8)
    return raw_output, output_q4


def _tensor_quantization(detail: dict, count: int) -> tuple[np.ndarray, np.ndarray]:
    quant = detail["quantization_parameters"]
    scales = np.asarray(quant["scales"], dtype=np.float32)
    zero_points = np.asarray(quant["zero_points"], dtype=np.int64)
    if scales.size == 1 and count != 1:
        scales = np.repeat(scales, count)
        zero_points = np.repeat(zero_points, count)
    if scales.shape != (count,) or zero_points.shape != (count,) or \
            np.any(~np.isfinite(scales)) or np.any(scales <= 0) or \
            np.any(zero_points < -128) or np.any(zero_points > 127):
        raise CanonicalInt8Error("unsupported tensor quantization")
    return scales, zero_points.astype(np.int8)


def extract_tflite(path: Path) -> CanonicalInt8Model:
    try:
        import tensorflow as tf
        from tensorflow.lite.python import schema_py_generated as schema
    except ImportError as error:
        raise CanonicalInt8Error("TensorFlow is required to extract canonical TFLite") from error
    content = path.read_bytes()
    interpreter = tf.lite.Interpreter(model_content=content,
        experimental_preserve_all_tensors=True,
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interpreter.allocate_tensors()
    details = {int(item["index"]): item for item in interpreter.get_tensor_details()}
    operations = interpreter._get_ops_details()  # stable TFLite diagnostic API
    flat_model = schema.Model.GetRootAsModel(content, 0)
    graph = flat_model.Subgraphs(0)
    if graph is None or len(operations) != 4 or graph.OperatorsLength() != 4:
        raise CanonicalInt8Error("canonical model must contain exactly four operations")
    layers = []
    observed_dimensions = []
    for index, operation in enumerate(operations):
        if operation["op_name"] != "FULLY_CONNECTED":
            raise CanonicalInt8Error("canonical graph contains a non-FULLY_CONNECTED op")
        inputs = [int(value) for value in operation["inputs"]]
        outputs = [int(value) for value in operation["outputs"]]
        if len(inputs) != 3 or len(outputs) != 1:
            raise CanonicalInt8Error("unsupported FULLY_CONNECTED signature")
        input_detail, weight_detail, bias_detail = (details[value] for value in inputs)
        output_detail = details[outputs[0]]
        weights = np.asarray(interpreter.get_tensor(inputs[1]), dtype=np.int8)
        biases = np.asarray(interpreter.get_tensor(inputs[2]), dtype=np.int32)
        if weights.ndim != 2 or biases.shape != (weights.shape[0],):
            raise CanonicalInt8Error("unsupported FULLY_CONNECTED constants")
        input_scales, input_zeros = _tensor_quantization(input_detail, 1)
        output_scales, output_zeros = _tensor_quantization(output_detail, 1)
        weight_scales, weight_zeros = _tensor_quantization(weight_detail, weights.shape[0])
        bias_scales = np.asarray(bias_detail["quantization_parameters"]["scales"],
                                 dtype=np.float32)
        expected_bias_scales = np.float32(input_scales[0]) * weight_scales
        if bias_scales.shape != expected_bias_scales.shape or \
                not np.array_equal(bias_scales, expected_bias_scales):
            raise CanonicalInt8Error("bias scale is not input_scale * weight_scale")
        multipliers, shifts = zip(*(quantize_multiplier(
            float(np.float64(input_scales[0]) * np.float64(scale) /
                  np.float64(output_scales[0]))) for scale in weight_scales))
        flat_operation = graph.Operators(index)
        options = schema.FullyConnectedOptions()
        options.Init(flat_operation.BuiltinOptions().Bytes,
                     flat_operation.BuiltinOptions().Pos)
        fused = int(options.FusedActivationFunction())
        if fused == schema.ActivationFunctionType.NONE:
            activation = ACTIVATION_NONE
        elif fused == schema.ActivationFunctionType.RELU:
            activation = ACTIVATION_RELU
        else:
            raise CanonicalInt8Error("unsupported fused activation")
        observed_dimensions.append(int(weights.shape[1]))
        layers.append(Layer(weights.copy(), biases.copy(), weight_scales.copy(),
            weight_zeros.copy(), np.asarray(multipliers, dtype=np.int32),
            np.asarray(shifts, dtype=np.int8), float(input_scales[0]),
            int(input_zeros[0]), float(output_scales[0]), int(output_zeros[0]),
            activation))
    observed_dimensions.append(int(layers[-1].weights.shape[0]))
    if tuple(observed_dimensions) != DIMENSIONS or \
            tuple(layer.activation for layer in layers) != (1, 1, 1, 0):
        raise CanonicalInt8Error("canonical topology or activations mismatch")
    return CanonicalInt8Model(hashlib.sha256(content).digest(), tuple(layers))


def tflite_reference(path: Path, canonical_inputs: np.ndarray) -> np.ndarray:
    try:
        import tensorflow as tf
    except ImportError as error:
        raise CanonicalInt8Error("TensorFlow is required for TFLite reference") from error
    interpreter = tf.lite.Interpreter(model_path=str(path),
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    outputs = []
    for row in np.asarray(canonical_inputs, dtype=np.int8):
        interpreter.set_tensor(input_detail["index"], row.reshape(1, 24))
        interpreter.invoke()
        outputs.append(interpreter.get_tensor(output_detail["index"])[0].copy())
    return np.asarray(outputs, dtype=np.int8)


def serialize_cpu_model(model: CanonicalInt8Model) -> bytes:
    header = bytearray(CPU_HEADER_SIZE)
    header[:8] = CPU_MAGIC
    struct.pack_into("<HH5HBBII", header, 8, CPU_FORMAT_VERSION,
        len(model.layers), *DIMENSIONS, 4, 4, QUANTIZATION_CONTRACT_VERSION, 0)
    struct.pack_into("<I", header, 32, CPU_HEADER_SIZE)
    input_num, input_shift = rational_scale(model.input_scale)
    output_num, output_shift = rational_scale(model.output_scale)
    struct.pack_into("<IBbBbI", header, 36, input_num, input_shift,
                     model.input_zero_point, output_shift,
                     model.output_zero_point, output_num)
    header[48:80] = model.canonical_sha256
    body = bytearray(CPU_LAYER_DESCRIPTOR_SIZE * len(model.layers))

    def append(data: bytes, alignment: int = 4) -> int:
        while (CPU_HEADER_SIZE + len(body)) & (alignment - 1):
            body.append(0)
        offset = CPU_HEADER_SIZE + len(body)
        body.extend(data)
        return offset

    descriptors = []
    for layer in model.layers:
        output_count, input_count = layer.weights.shape
        offsets = (
            append(layer.weights.astype(np.int8).tobytes(order="C"), 1),
            append(layer.biases.astype("<i4").tobytes(order="C")),
            append(layer.weight_scales.astype("<f4").tobytes(order="C")),
            append(layer.weight_zero_points.astype(np.int8).tobytes(order="C"), 1),
            append(layer.multipliers.astype("<i4").tobytes(order="C")),
            append(layer.shifts.astype(np.int8).tobytes(order="C"), 1),
        )
        descriptor = bytearray(CPU_LAYER_DESCRIPTOR_SIZE)
        struct.pack_into("<HHBBbbII6I", descriptor, 0, input_count, output_count,
            layer.activation, 0, layer.input_zero_point, layer.output_zero_point,
            struct.unpack("<I", struct.pack("<f", layer.input_scale))[0],
            struct.unpack("<I", struct.pack("<f", layer.output_scale))[0],
            *offsets)
        descriptors.append(descriptor)
    for index, descriptor in enumerate(descriptors):
        start = index * CPU_LAYER_DESCRIPTOR_SIZE
        body[start:start + CPU_LAYER_DESCRIPTOR_SIZE] = descriptor
    binary = header + body
    struct.pack_into("<I", binary, 28, len(binary))
    return bytes(binary)


def deserialize_cpu_model(binary: bytes) -> CanonicalInt8Model:
    if len(binary) < CPU_HEADER_SIZE or binary[:8] != CPU_MAGIC:
        raise CanonicalInt8Error("CPU model magic mismatch")
    version, layer_count, *dimensions = struct.unpack_from("<HH5H", binary, 8)
    if version != CPU_FORMAT_VERSION or layer_count != 4 or \
            tuple(dimensions) != DIMENSIONS or binary[22:24] != bytes((4, 4)) or \
            struct.unpack_from("<I", binary, 24)[0] != QUANTIZATION_CONTRACT_VERSION or \
            struct.unpack_from("<I", binary, 28)[0] != len(binary) or \
            struct.unpack_from("<I", binary, 32)[0] != CPU_HEADER_SIZE or \
            any(binary[80:96]):
        raise CanonicalInt8Error("CPU model header mismatch")
    input_num, input_shift, boundary_input_zero, output_shift, boundary_output_zero, output_num = \
        struct.unpack_from("<IBbBbI", binary, 36)
    if not input_num or input_shift > 31 or not output_num or output_shift > 31:
        raise CanonicalInt8Error("CPU boundary quantization mismatch")
    layers = []
    descriptor_end = CPU_HEADER_SIZE + layer_count * CPU_LAYER_DESCRIPTOR_SIZE
    for index in range(layer_count):
        offset = CPU_HEADER_SIZE + index * CPU_LAYER_DESCRIPTOR_SIZE
        values = struct.unpack_from("<HHBBbbII6I", binary, offset)
        input_count, output_count, activation, quantized_dimension = values[:4]
        input_zero, output_zero, input_bits, output_bits = values[4:8]
        offsets = values[8:]
        if (input_count, output_count) != (DIMENSIONS[index], DIMENSIONS[index + 1]) or \
                activation != (ACTIVATION_NONE if index == 3 else ACTIVATION_RELU) or \
                quantized_dimension != 0 or any(binary[offset + 40:offset + 48]):
            raise CanonicalInt8Error("CPU layer descriptor mismatch")
        sizes = (input_count * output_count, output_count * 4, output_count * 4,
                 output_count, output_count * 4, output_count)
        if any(value < descriptor_end or value > len(binary) or
               size > len(binary) - value for value, size in zip(offsets, sizes)):
            raise CanonicalInt8Error("CPU layer tensor is out of bounds")
        weights = np.frombuffer(binary, np.int8, input_count * output_count,
                                offsets[0]).reshape(output_count, input_count).copy()
        biases = np.frombuffer(binary, "<i4", output_count, offsets[1]).copy()
        weight_scales = np.frombuffer(binary, "<f4", output_count, offsets[2]).copy()
        weight_zeros = np.frombuffer(binary, np.int8, output_count, offsets[3]).copy()
        multipliers = np.frombuffer(binary, "<i4", output_count, offsets[4]).copy()
        shifts = np.frombuffer(binary, np.int8, output_count, offsets[5]).copy()
        input_scale = struct.unpack("<f", struct.pack("<I", input_bits))[0]
        output_scale = struct.unpack("<f", struct.pack("<I", output_bits))[0]
        if np.any(~np.isfinite(weight_scales)) or np.any(weight_scales <= 0) or \
                not math.isfinite(input_scale) or input_scale <= 0 or \
                not math.isfinite(output_scale) or output_scale <= 0 or \
                np.any(multipliers < 0):
            raise CanonicalInt8Error("CPU layer quantization is invalid")
        layers.append(Layer(weights, biases, weight_scales, weight_zeros,
            multipliers, shifts, input_scale, input_zero, output_scale,
            output_zero, activation))
    for first, second in zip(layers, layers[1:]):
        if struct.pack("<f", first.output_scale) != struct.pack("<f", second.input_scale) or \
                first.output_zero_point != second.input_zero_point:
            raise CanonicalInt8Error("CPU intermediate tensor contract mismatch")
    result = CanonicalInt8Model(bytes(binary[48:80]), tuple(layers))
    if rational_scale(result.input_scale) != (input_num, input_shift) or \
            result.input_zero_point != boundary_input_zero or \
            rational_scale(result.output_scale) != (output_num, output_shift) or \
            result.output_zero_point != boundary_output_zero:
        raise CanonicalInt8Error("CPU boundary quantization mismatch")
    return result
