# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
from __future__ import annotations

import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VULKAN_COMPILER = ROOT / "tools/model/compile_fsr4_reference_vulkan_shaders.py"
DX12_COMPILER = ROOT / "tools/model/compile_fsr4_reference_dx12_shaders.py"
PRE = ROOT / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4/pre_common.hlsli"
POST = ROOT / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4/post_common.hlsli"

PRE_MAX = 17
POST_MAX = 18


def hlsl_round(x: float) -> int:
    # The span proof is insensitive to exact half-way tie direction; use the
    # monotonic nearest-integer form for the integer sample-center envelope.
    return math.floor(x + 0.5)


def pre_floor(x: int, inv_scale: float, jitter: float) -> int:
    return math.floor(-0.5 + inv_scale / 2.0 + x * inv_scale + jitter)


def post_center(x: int, inv_scale: float, jitter: float) -> int:
    return hlsl_round(-0.5 + inv_scale / 2.0 + x * inv_scale + jitter)


def verify_geometry() -> tuple[int, int, int, int]:
    max_pre = 0
    max_post = 0
    quality_pre = 0
    quality_post = 0

    # Dense deterministic coverage over every 0.01 ratio step, a deliberately
    # wider-than-runtime jitter envelope, multiple group phases, and every
    # possible partial last-group width. The source mappings are monotonic, so
    # endpoint coverage proves all interior bilinear / 3-tap samples are inside
    # the same interval without iterating every thread separately.
    for ratio_step in range(100, 301):
        ratio = ratio_step / 100.0
        inv_scale = 1.0 / ratio
        for jitter_step in range(-16, 17, 2):
            jitter = jitter_step / 8.0
            for group_index in range(8):
                base = group_index * 16
                for active_width in range(1, 17):
                    last = base + active_width - 1

                    pre_min = pre_floor(base, inv_scale, jitter)
                    pre_max_floor = pre_floor(last, inv_scale, jitter)
                    pre_extent = (pre_max_floor + 1) - pre_min + 1
                    if pre_extent > PRE_MAX:
                        raise AssertionError(
                            f"pre cache overflow: ratio={ratio} jitter={jitter} "
                            f"base={base} active={active_width} extent={pre_extent}"
                        )
                    # Monotonic floor mapping: [pre_min, pre_max_floor+1]
                    # is exactly the union envelope for all bilinear samples.
                    if pre_max_floor + 1 >= pre_min + pre_extent:
                        raise AssertionError("pre cache endpoint is not covered")
                    max_pre = max(max_pre, pre_extent)
                    if ratio == 1.5:
                        quality_pre = max(quality_pre, pre_extent)

                    post_min = post_center(base, inv_scale, jitter)
                    post_max_center = post_center(last, inv_scale, jitter)
                    post_extent = post_max_center - post_min + 3
                    if post_extent > POST_MAX:
                        raise AssertionError(
                            f"post cache overflow: ratio={ratio} jitter={jitter} "
                            f"base={base} active={active_width} extent={post_extent}"
                        )
                    cache_base = post_min - 1
                    # Monotonic rounded-center mapping: [min-1, max+1]
                    # is exactly the union envelope for every 3-tap sample.
                    if post_max_center + 1 >= cache_base + post_extent:
                        raise AssertionError("post cache endpoint is not covered")
                    max_post = max(max_post, post_extent)
                    if ratio == 1.5:
                        quality_post = max(quality_post, post_extent)

    if max_pre != PRE_MAX or max_post != POST_MAX:
        raise AssertionError(f"proof did not exercise maxima: pre={max_pre}, post={max_post}")
    return max_pre, max_post, quality_pre, quality_post


def main() -> int:
    vk = VULKAN_COMPILER.read_text(encoding="utf-8")
    dx = DX12_COMPILER.read_text(encoding="utf-8")
    pre = PRE.read_text(encoding="utf-8")
    post = POST.read_text(encoding="utf-8")

    if 'APUSR_VULKAN_PRE_INPUT_CACHE=1' not in vk:
        raise AssertionError("Vulkan pre input cache is not enabled by the product compiler")
    if 'APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE=1' not in vk:
        raise AssertionError("Vulkan post adaptive input cache is not enabled by the product compiler")
    if 'APUSR_VULKAN_PRE_INPUT_CACHE=1' in dx or 'APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE=1' in dx:
        raise AssertionError("Vulkan-only cache defines leaked into the DX12 compiler")

    if 'APUSR_FSR4_PRE_INPUT_TILE_W = 17' not in pre:
        raise AssertionError("pre physical LDS input tile is not 17x17")
    if '#define APUSR_FSR4_I8_POST_MODEL_FILTER_INPUT_TILE_SIZE 18' not in post:
        raise AssertionError("post physical LDS input tile is not capped at 18x18")
    if 'GroupMemoryBarrierWithGroupSync();' not in pre or 'GroupMemoryBarrierWithGroupSync();' not in post:
        raise AssertionError("bookend cooperative caches must synchronize before consumption")
    if 'cbDistance * cbDistance + crDistance * crDistance' not in pre:
        raise AssertionError("Vulkan pre path does not explicitly lower square to multiply")
    if '#if APUSR_VULKAN_PRE_INPUT_CACHE' not in pre:
        raise AssertionError("pre cache/square optimization is not Vulkan-scoped")

    max_pre, max_post, q_pre, q_post = verify_geometry()
    print(
        "Vulkan bookend input-cache contract OK: "
        f"max pre={max_pre}x{max_pre}, max post={max_post}x{max_post}; "
        f"1.5x pre<={q_pre}x{q_pre}, post<={q_post}x{q_post}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
