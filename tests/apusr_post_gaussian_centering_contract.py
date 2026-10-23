# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
from __future__ import annotations

import math
import random
import struct
import sys
from pathlib import Path


def f32(value: float) -> float:
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def add(a: float, b: float) -> float:
    return f32(f32(a) + f32(b))


def mul(a: float, b: float) -> float:
    return f32(f32(a) * f32(b))


def expf(value: float) -> float:
    try:
        return f32(math.exp(f32(value)))
    except OverflowError:
        return math.inf


def normalize(weights: list[float]) -> list[float]:
    total = f32(0.0)
    for weight in weights:
        total = add(total, weight)
    return [f32(weight / total) for weight in weights]


def direct_weights(center_x: float, center_y: float, x_step: float, y_step: float,
                   correlation: float, scale_x: float, scale_y: float) -> list[float]:
    gaussian = f32(-0.5 / (0.47 * 0.47))
    scale_x2 = mul(scale_x, scale_x)
    scale_y2 = mul(scale_y, scale_y)
    scale_xy = mul(scale_x, scale_y)
    weights: list[float] = []
    for dy in (-1, 0, 1):
        y = add(center_y, mul(float(dy), y_step))
        y2 = mul(y, y)
        for dx in (-1, 0, 1):
            x = add(center_x, mul(float(dx), x_step))
            x2 = mul(x, x)
            cross = mul(mul(mul(f32(2.0), correlation), mul(x, y)), scale_xy)
            exponent = mul(gaussian, add(add(mul(x2, scale_x2), cross), mul(y2, scale_y2)))
            weights.append(expf(exponent))
    return normalize(weights)


def center_relative_weights(center_x: float, center_y: float, x_step: float, y_step: float,
                            correlation: float, scale_x: float, scale_y: float) -> list[float]:
    gaussian = f32(-0.5 / (0.47 * 0.47))
    scale_x2 = mul(scale_x, scale_x)
    scale_y2 = mul(scale_y, scale_y)
    cross_factor = mul(mul(f32(2.0), correlation), mul(scale_x, scale_y))

    linear_x = mul(
        gaussian,
        add(
            mul(mul(f32(2.0), scale_x2), mul(center_x, x_step)),
            mul(cross_factor, mul(center_y, x_step)),
        ),
    )
    linear_y = mul(
        gaussian,
        add(
            mul(mul(f32(2.0), scale_y2), mul(center_y, y_step)),
            mul(cross_factor, mul(center_x, y_step)),
        ),
    )
    quadratic_x = mul(gaussian, mul(scale_x2, mul(x_step, x_step)))
    quadratic_y = mul(gaussian, mul(scale_y2, mul(y_step, y_step)))
    cross_xy = mul(gaussian, mul(cross_factor, mul(x_step, y_step)))

    weights: list[float] = []
    for dy in (-1, 0, 1):
        fy = f32(float(dy))
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                weights.append(f32(1.0))
                continue
            fx = f32(float(dx))
            exponent_delta = add(
                add(mul(fx, linear_x), mul(fy, linear_y)),
                add(
                    add(mul(mul(fx, fx), quadratic_x), mul(mul(fy, fy), quadratic_y)),
                    mul(mul(fx, fy), cross_xy),
                ),
            )
            weights.append(expf(exponent_delta))
    return normalize(weights)


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    shader = repo / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4/post_common.hlsli"
    text = shader.read_text(encoding="utf-8")

    if "ApusrPostGaussian3x3Delta" not in text or "apusrPostGaussian3x3Recurrence" in text:
        print("post Gaussian center-relative contract is missing or stale recurrence code returned", file=sys.stderr)
        return 1
    weight_start = text.find("float apusrPostGaussianWeight(")
    weight_end = text.find("float3 apply_model_filter", weight_start)
    if weight_start < 0 or weight_end < 0:
        print("post Gaussian weight helper is missing", file=sys.stderr)
        return 2
    helper = text[weight_start:weight_end]
    if "? 1.0 : exp(exponentDelta)" not in helper:
        print("center tap is no longer factored out of the normalized Gaussian", file=sys.stderr)
        return 3

    rng = random.Random(0xA9E4F54)
    max_delta = 0.0
    for _ in range(16384):
        # APUSR's advertised quality modes span 1x through 3x. The center sample
        # is round(lrPos), so its unscaled displacement is always in [-0.5, 0.5].
        x_step = f32(rng.uniform(1.0, 3.0))
        y_step = f32(rng.uniform(1.0, 3.0))
        center_x = f32(rng.uniform(-0.5, 0.5) * x_step)
        center_y = f32(rng.uniform(-0.5, 0.5) * y_step)
        correlation = f32(rng.uniform(-0.999, 0.999))
        scale_x = f32(rng.uniform(0.02, 1.98))
        scale_y = f32(rng.uniform(0.02, 1.98))

        reference = direct_weights(center_x, center_y, x_step, y_step, correlation, scale_x, scale_y)
        centered = center_relative_weights(center_x, center_y, x_step, y_step, correlation, scale_x, scale_y)
        if not all(math.isfinite(v) for v in reference + centered):
            print("non-finite normalized Gaussian weight inside supported 1x-3x range", file=sys.stderr)
            return 4
        delta = max(abs(a - b) for a, b in zip(reference, centered))
        max_delta = max(max_delta, delta)

    if max_delta > 1.0e-5:
        print(f"center-relative normalized Gaussian drift too large: {max_delta:.9g}", file=sys.stderr)
        return 5

    print(f"APUSR post Gaussian center-relative contract passed; max normalized fp32 delta={max_delta:.9g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
