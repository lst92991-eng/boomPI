#!/usr/bin/env python3
"""Generate and validate the boomPI 48 kHz to 16 kHz FIR coefficients."""

import argparse
import math
from pathlib import Path
import re
import sys


SAMPLE_RATE_HZ = 48_000
TAP_COUNT = 211
CUTOFF_HZ = 7_500
KAISER_BETA = 6.76
Q15_SCALE = 1 << 15
PASSBAND_EDGE_HZ = 7_000
STOPBAND_EDGE_HZ = 8_000
MAX_PASSBAND_RIPPLE_DB = 0.01
MIN_STOPBAND_ATTENUATION_DB = 60.0


def bessel_i0(value: float) -> float:
    """Return I0(value) using a deterministic standard-library series."""
    term = 1.0
    total = 1.0
    scaled = value * value / 4.0
    for index in range(1, 100):
        term *= scaled / (index * index)
        total += term
        if abs(term) <= abs(total) * 1.0e-18:
            break
    return total


def round_nearest_away_from_zero(value: float) -> int:
    if value >= 0.0:
        return math.floor(value + 0.5)
    return math.ceil(value - 0.5)


def generate_coefficients() -> list[int]:
    midpoint = (TAP_COUNT - 1) // 2
    normalized_cutoff = CUTOFF_HZ / SAMPLE_RATE_HZ
    window_denominator = bessel_i0(KAISER_BETA)
    floating = [0.0] * TAP_COUNT

    for tap in range(midpoint + 1):
        offset = tap - midpoint
        if offset == 0:
            ideal = 2.0 * normalized_cutoff
        else:
            ideal = math.sin(2.0 * math.pi * normalized_cutoff * offset)
            ideal /= math.pi * offset
        window_position = 2.0 * tap / (TAP_COUNT - 1) - 1.0
        window = bessel_i0(
            KAISER_BETA
            * math.sqrt(max(0.0, 1.0 - window_position * window_position))
        ) / window_denominator
        floating[tap] = ideal * window
        floating[TAP_COUNT - 1 - tap] = floating[tap]

    normalization = sum(floating)
    floating = [coefficient / normalization for coefficient in floating]

    coefficients = [0] * TAP_COUNT
    for tap in range(midpoint):
        quantized = round_nearest_away_from_zero(floating[tap] * Q15_SCALE)
        coefficients[tap] = quantized
        coefficients[TAP_COUNT - 1 - tap] = quantized
    coefficients[midpoint] = round_nearest_away_from_zero(
        floating[midpoint] * Q15_SCALE
    )

    # Preserve symmetry while making unity DC gain exact in Q15.
    coefficients[midpoint] += Q15_SCALE - sum(coefficients)
    return coefficients


def response_magnitude(coefficients: list[int], frequency_hz: int) -> float:
    midpoint = (len(coefficients) - 1) // 2
    radians = 2.0 * math.pi * frequency_hz / SAMPLE_RATE_HZ
    centered = float(coefficients[midpoint])
    for distance in range(1, midpoint + 1):
        centered += 2.0 * coefficients[midpoint - distance] * math.cos(
            radians * distance
        )
    return abs(centered) / Q15_SCALE


def validate(coefficients: list[int]) -> tuple[float, float]:
    if len(coefficients) != TAP_COUNT:
        raise ValueError("unexpected coefficient count")
    if coefficients != list(reversed(coefficients)):
        raise ValueError("coefficient table is not symmetric")
    if sum(coefficients) != Q15_SCALE:
        raise ValueError("coefficient table does not have exact unity DC gain")
    if any(value < -32768 or value > 32767 for value in coefficients):
        raise ValueError("coefficient is outside the signed Q15 range")

    passband = [
        response_magnitude(coefficients, frequency_hz)
        for frequency_hz in range(0, PASSBAND_EDGE_HZ + 1)
    ]
    stopband = [
        response_magnitude(coefficients, frequency_hz)
        for frequency_hz in range(STOPBAND_EDGE_HZ, SAMPLE_RATE_HZ // 2 + 1)
    ]
    ripple_db = 20.0 * math.log10(max(passband) / min(passband))
    stopband_db = 20.0 * math.log10(max(stopband))
    if ripple_db > MAX_PASSBAND_RIPPLE_DB:
        raise ValueError(f"passband ripple {ripple_db:.6f} dB exceeds limit")
    if stopband_db > -MIN_STOPBAND_ATTENUATION_DB:
        raise ValueError(f"stopband peak {stopband_db:.6f} dB exceeds limit")
    return ripple_db, stopband_db


def emit_cpp(coefficients: list[int]) -> None:
    print("constexpr std::array<std::int16_t, 211U> kCoefficientsQ15{{")
    for offset in range(0, len(coefficients), 8):
        values = ", ".join(str(value) for value in coefficients[offset : offset + 8])
        suffix = "," if offset + 8 < len(coefficients) else ""
        print(f"    {values}{suffix}")
    print("}};")


def read_cpp_coefficients(source_path: Path) -> list[int]:
    source = source_path.read_text(encoding="utf-8")
    match = re.search(
        r"kCoefficientsQ15\s*\{\{(?P<table>.*?)\}\};",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"coefficient table not found in {source_path}")
    return [int(value) for value in re.findall(r"[-+]?\d+", match.group("table"))]


def check_cpp_table(expected: list[int], source_path: Path) -> None:
    actual = read_cpp_coefficients(source_path)
    if len(actual) != len(expected):
        raise ValueError(
            f"{source_path} contains {len(actual)} coefficients; "
            f"expected {len(expected)}"
        )
    for index, (actual_value, expected_value) in enumerate(zip(actual, expected)):
        if actual_value != expected_value:
            raise ValueError(
                f"{source_path} coefficient {index} is {actual_value}; "
                f"expected {expected_value}"
            )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        type=Path,
        metavar="CPP_SOURCE",
        help="verify that CPP_SOURCE contains the generated coefficient table",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="validate without writing the generated C++ table to stdout",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    coefficients = generate_coefficients()
    ripple_db, stopband_db = validate(coefficients)
    if arguments.check is not None:
        check_cpp_table(coefficients, arguments.check)
    if not arguments.quiet:
        emit_cpp(coefficients)
    print(
        f"validated: taps={TAP_COUNT}, sum={sum(coefficients)}, "
        f"passband_ripple_db={ripple_db:.6f}, "
        f"stopband_peak_db={stopband_db:.6f}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
