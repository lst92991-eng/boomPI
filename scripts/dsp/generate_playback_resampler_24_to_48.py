#!/usr/bin/env python3
"""Generate and validate the boomPI 24 kHz to 48 kHz FIR coefficients."""

import argparse
import math
from pathlib import Path
import re
import sys


SAMPLE_RATE_HZ = 48_000
TAP_COUNT = 65
CUTOFF_HZ = 12_000
KAISER_BETA = 7.85726
Q31_SCALE = 1 << 31
PASSBAND_EDGE_HZ = 10_000
STOPBAND_EDGE_HZ = 14_000
MAX_PASSBAND_RIPPLE_DB = 0.005
MIN_STOPBAND_ATTENUATION_DB = 76.0
DEFAULT_CPP_SOURCE = (
    Path(__file__).resolve().parents[2]
    / "client"
    / "src"
    / "audio"
    / "playback_resampler_24_to_48.cpp"
)


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
    if CUTOFF_HZ * 4 != SAMPLE_RATE_HZ:
        raise ValueError("half-band design requires cutoff at one quarter of fs")
    window_denominator = bessel_i0(KAISER_BETA)
    floating = [0.0] * TAP_COUNT

    for tap in range(TAP_COUNT):
        offset = tap - midpoint
        if offset == 0:
            ideal = 4.0 * normalized_cutoff
        else:
            ideal = 2.0 * math.sin(
                2.0 * math.pi * normalized_cutoff * offset
            )
            ideal /= math.pi * offset
        window_position = 2.0 * tap / (TAP_COUNT - 1) - 1.0
        window = bessel_i0(
            KAISER_BETA
            * math.sqrt(max(0.0, 1.0 - window_position * window_position))
        ) / window_denominator
        floating[tap] = ideal * window
        if tap != midpoint and tap % 2 == 0:
            floating[tap] = 0.0

    # Keep the even phase as a single centre tap and make the 32-tap odd
    # phase exactly unity at DC before Q31 quantization.
    outer_sum = sum(floating) - floating[midpoint]
    for tap in range(TAP_COUNT):
        if tap != midpoint:
            floating[tap] /= outer_sum
    floating[midpoint] = 1.0

    coefficients = [
        round_nearest_away_from_zero(value * Q31_SCALE) for value in floating
    ]
    coefficients[midpoint] = Q31_SCALE - 1

    # Preserve symmetry. The target gain is 2 - 2^-31 because signed Q31
    # cannot represent +1 exactly at the centre.
    target_sum = 2 * Q31_SCALE - 1
    correction = target_sum - sum(coefficients)
    if correction % 2 != 0:
        raise ValueError("Q31 DC correction cannot preserve symmetry")
    coefficients[midpoint - 1] += correction // 2
    coefficients[midpoint + 1] += correction // 2
    return coefficients


def response_magnitude(coefficients: list[int], frequency_hz: int) -> float:
    midpoint = (len(coefficients) - 1) // 2
    radians = 2.0 * math.pi * frequency_hz / SAMPLE_RATE_HZ
    centered = float(coefficients[midpoint])
    for distance in range(1, midpoint + 1):
        centered += 2.0 * coefficients[midpoint - distance] * math.cos(
            radians * distance
        )
    # Zero stuffing halves the source spectrum, so gain-2 FIR magnitude is
    # divided by two when stated as source-to-output tone gain.
    return abs(centered) / (2.0 * Q31_SCALE)


def validate(coefficients: list[int]) -> tuple[float, float, float, float]:
    midpoint = (TAP_COUNT - 1) // 2
    if len(coefficients) != TAP_COUNT:
        raise ValueError("unexpected coefficient count")
    if coefficients != list(reversed(coefficients)):
        raise ValueError("coefficient table is not symmetric")
    if any(value < -(1 << 31) or value > (1 << 31) - 1 for value in coefficients):
        raise ValueError("coefficient is outside the signed Q31 range")
    if any(
        coefficients[tap] != 0
        for tap in range(0, TAP_COUNT, 2)
        if tap != midpoint
    ):
        raise ValueError("even non-centre half-band tap is not zero")

    even_phase_sum = sum(coefficients[0::2])
    odd_phase_sum = sum(coefficients[1::2])
    if even_phase_sum != Q31_SCALE - 1:
        raise ValueError("even phase is not the single Q31 centre tap")
    if odd_phase_sum != Q31_SCALE:
        raise ValueError("odd phase does not have exact Q31 unity DC gain")
    if sum(coefficients) != 2 * Q31_SCALE - 1:
        raise ValueError("combined phases do not have gain 2 - 2^-31")

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
    gain_10k_db = 20.0 * math.log10(
        response_magnitude(coefficients, PASSBAND_EDGE_HZ)
    )
    image_edge_db = 20.0 * math.log10(
        response_magnitude(coefficients, STOPBAND_EDGE_HZ)
    )
    if ripple_db > MAX_PASSBAND_RIPPLE_DB:
        raise ValueError(f"passband ripple {ripple_db:.6f} dB exceeds limit")
    if stopband_db > -MIN_STOPBAND_ATTENUATION_DB:
        raise ValueError(f"stopband peak {stopband_db:.6f} dB exceeds limit")
    return ripple_db, stopband_db, gain_10k_db, image_edge_db


def emit_cpp(coefficients: list[int]) -> None:
    print("constexpr std::array<std::int32_t, 65U> kCoefficientsQ31{{")
    for offset in range(0, len(coefficients), 5):
        values = ", ".join(str(value) for value in coefficients[offset : offset + 5])
        suffix = "," if offset + 5 < len(coefficients) else ""
        print(f"    {values}{suffix}")
    print("}};")


def read_cpp_coefficients(source_path: Path) -> list[int]:
    source = source_path.read_text(encoding="utf-8")
    match = re.search(
        r"kCoefficientsQ31\s*\{\{(?P<table>.*?)\}\};",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"coefficient table not found in {source_path}")
    return [int(value) for value in re.findall(r"[-+]?\d+", match.group("table"))]


def check_cpp_table(expected: list[int], source_path: Path) -> None:
    actual = read_cpp_coefficients(source_path)
    if actual != expected:
        mismatch = next(
            (
                index
                for index, (actual_value, expected_value) in enumerate(
                    zip(actual, expected)
                )
                if actual_value != expected_value
            ),
            min(len(actual), len(expected)),
        )
        raise ValueError(
            f"{source_path} coefficient table differs at index {mismatch}; "
            f"actual_count={len(actual)}, expected_count={len(expected)}"
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        nargs="?",
        type=Path,
        const=DEFAULT_CPP_SOURCE,
        metavar="CPP_SOURCE",
        help="verify CPP_SOURCE (default: production resampler source)",
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
    ripple_db, stopband_db, gain_10k_db, image_edge_db = validate(coefficients)
    if arguments.check is not None:
        check_cpp_table(coefficients, arguments.check)
    if not arguments.quiet and arguments.check is None:
        emit_cpp(coefficients)
    print(
        f"validated: taps={TAP_COUNT}, odd_sum={sum(coefficients[1::2])}, "
        f"even_sum={sum(coefficients[0::2])}, "
        f"passband_ripple_db={ripple_db:.6f}, "
        f"stopband_peak_db={stopband_db:.6f}, "
        f"gain_10khz_db={gain_10k_db:.6f}, "
        f"image_edge_14khz_db={image_edge_db:.6f}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
