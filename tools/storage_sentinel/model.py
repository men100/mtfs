from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from schema import DatasetError


def _quantile_higher(values: np.ndarray, probability: float) -> float:
    if values.size == 0:
        raise DatasetError("cannot choose threshold from an empty validation set")
    try:
        return float(np.quantile(values, probability, method="higher"))
    except TypeError:  # NumPy < 1.22
        return float(np.quantile(values, probability, interpolation="higher"))


@dataclass
class DenseAutoencoder:
    dimensions: list[int]
    weights: list[np.ndarray]
    biases: list[np.ndarray]

    @classmethod
    def create(cls, dimensions: list[int], seed: int) -> "DenseAutoencoder":
        rng = np.random.default_rng(seed)
        weights, biases = [], []
        for inputs, outputs in zip(dimensions[:-1], dimensions[1:]):
            limit = np.sqrt(6.0 / (inputs + outputs))
            weights.append(rng.uniform(-limit, limit, (inputs, outputs)).astype(np.float64))
            biases.append(np.zeros(outputs, dtype=np.float64))
        return cls(dimensions, weights, biases)

    def forward(self, values: np.ndarray, cache: bool = False):
        activation = values
        activations = [activation]
        preactivations = []
        for index, (weight, bias) in enumerate(zip(self.weights, self.biases)):
            linear = activation @ weight + bias
            preactivations.append(linear)
            activation = linear if index == len(self.weights) - 1 else np.maximum(linear, 0.0)
            activations.append(activation)
        return (activation, activations, preactivations) if cache else activation

    def scores(self, values: np.ndarray) -> np.ndarray:
        reconstructed = self.forward(values)
        scores = np.mean(np.square(reconstructed - values), axis=1)
        if not np.all(np.isfinite(scores)):
            raise DatasetError("NaN/Inf anomaly score")
        return scores

    @property
    def parameter_count(self) -> int:
        return int(sum(weight.size + bias.size for weight, bias in zip(self.weights, self.biases)))

    @property
    def multiply_accumulate_count(self) -> int:
        return int(sum(a * b for a, b in zip(self.dimensions[:-1], self.dimensions[1:])))

    def as_dict(self) -> dict:
        return {
            "format": "mtfs-sentinel-dense-autoencoder-v1",
            "topology": self.dimensions,
            "activation": ["relu"] * (len(self.dimensions) - 2) + ["linear"],
            "score": "mean_squared_reconstruction_error",
            "weights": [weight.tolist() for weight in self.weights],
            "biases": [bias.tolist() for bias in self.biases],
            "parameter_count": self.parameter_count,
            "parameter_bytes_float32": self.parameter_count * 4,
            "multiply_accumulate_count": self.multiply_accumulate_count,
        }

    @classmethod
    def from_dict(cls, payload: dict) -> "DenseAutoencoder":
        if payload.get("format") != "mtfs-sentinel-dense-autoencoder-v1":
            raise DatasetError("unsupported model format")
        dimensions = [int(value) for value in payload["topology"]]
        weights = [np.asarray(value, dtype=np.float64) for value in payload["weights"]]
        biases = [np.asarray(value, dtype=np.float64) for value in payload["biases"]]
        model = cls(dimensions, weights, biases)
        for index, (weight, bias) in enumerate(zip(weights, biases)):
            if weight.shape != (dimensions[index], dimensions[index + 1]) or bias.shape != (dimensions[index + 1],):
                raise DatasetError("model tensor shape mismatch")
            if not np.all(np.isfinite(weight)) or not np.all(np.isfinite(bias)):
                raise DatasetError("NaN/Inf model tensor")
        return model


def train_autoencoder(values: np.ndarray, dimensions: list[int], seed: int,
                      epochs: int = 200, batch_size: int = 64,
                      learning_rate: float = 1e-3) -> DenseAutoencoder:
    if values.ndim != 2 or values.shape[1] != dimensions[0] or values.shape[0] < 2:
        raise DatasetError("invalid autoencoder training matrix")
    if not np.all(np.isfinite(values)):
        raise DatasetError("NaN/Inf training matrix")
    model = DenseAutoencoder.create(dimensions, seed)
    rng = np.random.default_rng(seed)
    m_w = [np.zeros_like(value) for value in model.weights]
    v_w = [np.zeros_like(value) for value in model.weights]
    m_b = [np.zeros_like(value) for value in model.biases]
    v_b = [np.zeros_like(value) for value in model.biases]
    step = 0
    for _ in range(epochs):
        indices = rng.permutation(values.shape[0])
        for offset in range(0, values.shape[0], batch_size):
            batch = values[indices[offset:offset + batch_size]]
            output, activations, preactivations = model.forward(batch, cache=True)
            delta = (2.0 / (batch.shape[0] * batch.shape[1])) * (output - batch)
            gradients_w: list[np.ndarray] = [np.empty(0)] * len(model.weights)
            gradients_b: list[np.ndarray] = [np.empty(0)] * len(model.biases)
            for layer in range(len(model.weights) - 1, -1, -1):
                gradients_w[layer] = activations[layer].T @ delta
                gradients_b[layer] = np.sum(delta, axis=0)
                if layer:
                    delta = (delta @ model.weights[layer].T) * (preactivations[layer - 1] > 0.0)
            step += 1
            for layer in range(len(model.weights)):
                for parameter, gradient, first, second in (
                    (model.weights[layer], gradients_w[layer], m_w[layer], v_w[layer]),
                    (model.biases[layer], gradients_b[layer], m_b[layer], v_b[layer]),
                ):
                    first *= 0.9
                    first += 0.1 * gradient
                    second *= 0.999
                    second += 0.001 * np.square(gradient)
                    corrected_first = first / (1.0 - 0.9 ** step)
                    corrected_second = second / (1.0 - 0.999 ** step)
                    parameter -= learning_rate * corrected_first / (np.sqrt(corrected_second) + 1e-8)
    if any(not np.all(np.isfinite(value)) for value in model.weights + model.biases):
        raise DatasetError("training produced NaN/Inf")
    return model


def train_candidates(train: np.ndarray, validation: np.ndarray, seed: int,
                     epochs: int) -> tuple[DenseAutoencoder, list[dict]]:
    candidates = ([24, 12, 4, 12, 24], [24, 12, 8, 12, 24])
    results: list[tuple[float, DenseAutoencoder, dict]] = []
    for index, topology in enumerate(candidates):
        model = train_autoencoder(train, list(topology), seed + index, epochs=epochs)
        scores = model.scores(validation)
        summary = {
            "topology": list(topology),
            "validation_median_score": float(np.median(scores)),
            "validation_p95_score": _quantile_higher(scores, 0.95),
            "parameter_count": model.parameter_count,
            "multiply_accumulate_count": model.multiply_accumulate_count,
        }
        results.append((summary["validation_median_score"], model, summary))
    # Topology is deliberately common across targets.  The 8-unit candidate is
    # reported, but threshold/weights use the fixed 4-unit bottleneck.
    return results[0][1], [item[2] for item in results]


@dataclass
class Baselines:
    simple_threshold: float
    mahalanobis_threshold: float
    covariance_inverse: np.ndarray

    def simple_scores(self, normalized: np.ndarray) -> np.ndarray:
        return np.max(np.abs(normalized), axis=1)

    def mahalanobis_scores(self, normalized: np.ndarray) -> np.ndarray:
        return np.einsum("ij,jk,ik->i", normalized, self.covariance_inverse, normalized)

    def as_dict(self) -> dict:
        return {
            "simple": {"score": "max_absolute_normalized_feature",
                       "threshold": self.simple_threshold},
            "mahalanobis": {"regularization": 1e-3,
                            "threshold": self.mahalanobis_threshold,
                            "covariance_inverse": self.covariance_inverse.tolist()},
        }

    @classmethod
    def train(cls, train: np.ndarray, validation: np.ndarray) -> "Baselines":
        covariance = np.cov(train, rowvar=False)
        covariance = covariance + np.eye(train.shape[1]) * 1e-3
        inverse = np.linalg.pinv(covariance, hermitian=True)
        simple = np.max(np.abs(validation), axis=1)
        mahalanobis = np.einsum("ij,jk,ik->i", validation, inverse, validation)
        return cls(_quantile_higher(simple, 0.95),
                   _quantile_higher(mahalanobis, 0.95), inverse)

    @classmethod
    def from_dict(cls, payload: dict) -> "Baselines":
        return cls(float(payload["simple"]["threshold"]),
                   float(payload["mahalanobis"]["threshold"]),
                   np.asarray(payload["mahalanobis"]["covariance_inverse"], dtype=np.float64))


def canonical_sha256(payload: dict) -> str:
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def load_artifact(directory: Path):
    model_payload = json.loads((directory / "model.json").read_text(encoding="utf-8"))
    normalization = json.loads((directory / "normalization.json").read_text(encoding="utf-8"))
    threshold = json.loads((directory / "threshold.json").read_text(encoding="utf-8"))
    baselines_payload = json.loads((directory / "baselines.json").read_text(encoding="utf-8"))
    return (DenseAutoencoder.from_dict(model_payload), normalization, threshold,
            Baselines.from_dict(baselines_payload))
