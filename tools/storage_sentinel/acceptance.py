"""Versioned Neural-ART numerical acceptance helpers."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence


VECTOR_SIZE = 24
INT8_MIN = -128
INT8_MAX = 127
DECISION_DEFINITELY_NORMAL = "definitely-normal"
DECISION_DEFINITELY_ANOMALY = "definitely-anomaly"
DECISION_AMBIGUOUS = "ambiguous-cpu-arbitration"


@dataclass(frozen=True)
class ScoreInterval:
    score_min_q8: int
    score_max_q8: int
    decision_class: str


def score_interval_q8(input_q4: Sequence[int], reference_output_q4: Sequence[int],
                      maximum_q4_error: int, threshold_q8: int) -> ScoreInterval:
    """Return the exact integer MSE interval induced by a uniform Q4 error bound."""
    if len(input_q4) != VECTOR_SIZE or len(reference_output_q4) != VECTOR_SIZE:
        raise ValueError("score interval requires exactly 24 input and output elements")
    if not 0 <= maximum_q4_error <= 255:
        raise ValueError("maximum Q4 error is outside the int8 distance domain")
    if threshold_q8 < 0:
        raise ValueError("threshold must be non-negative")

    minimum_sum = 0
    maximum_sum = 0
    for input_value, reference_value in zip(input_q4, reference_output_q4):
        if not INT8_MIN <= input_value <= INT8_MAX or \
                not INT8_MIN <= reference_value <= INT8_MAX:
            raise ValueError("score interval values must be int8")
        lower = max(INT8_MIN, reference_value - maximum_q4_error)
        upper = min(INT8_MAX, reference_value + maximum_q4_error)
        if input_value < lower:
            minimum_delta = lower - input_value
        elif input_value > upper:
            minimum_delta = input_value - upper
        else:
            minimum_delta = 0
        maximum_delta = max(abs(input_value - lower), abs(input_value - upper))
        minimum_sum += minimum_delta * minimum_delta
        maximum_sum += maximum_delta * maximum_delta

    score_min = (minimum_sum + VECTOR_SIZE // 2) // VECTOR_SIZE
    score_max = (maximum_sum + VECTOR_SIZE // 2) // VECTOR_SIZE
    if score_max <= threshold_q8:
        decision_class = DECISION_DEFINITELY_NORMAL
    elif score_min > threshold_q8:
        decision_class = DECISION_DEFINITELY_ANOMALY
    else:
        decision_class = DECISION_AMBIGUOUS
    return ScoreInterval(score_min, score_max, decision_class)
