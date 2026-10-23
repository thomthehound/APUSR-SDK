#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

"""Budget the first DP4A tiled-fusion experiment.

This deliberately mirrors the Stage1+2->Stage3 paired-macroblock dataflow from
APUSR's NPU work, but keeps execution on the GPU DOT4/DP4A path.  The output is
semantic tensor traffic, not a cache-transaction profiler.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass


@dataclass(frozen=True)
class Tile:
    name: str
    width: int
    height: int
    channels: int

    @property
    def bytes(self) -> int:
        return self.width * self.height * self.channels


def mib(value: int) -> float:
    return value / (1024.0 * 1024.0)


def gbps(value: int, fps: float) -> float:
    return value * fps / 1_000_000_000.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=int, default=3840, help="display/output width")
    parser.add_argument("--height", type=int, default=2160, help="display/output height")
    parser.add_argument("--fps", type=float, default=120.0, help="target frame rate for bandwidth estimates")
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0:
        raise SystemExit("width and height must be positive")

    c16_w = args.width // 2
    c16_h = args.height // 2
    c32_w = c16_w // 2
    c32_h = c16_h // 2

    source = Tile("source C16 macro-tile feeding Stage1", 36, 20, 16)
    stage1_local = Tile("local exact Stage1 C16 patch", 34, 18, 16)
    stage2_local = Tile("local exact Stage2/SkipA C16 patch", 32, 16, 16)
    stage3_out = Tile("Stage3 C32 output tile", 16, 8, 32)

    c16_frame = c16_w * c16_h * 16
    c32_stage3_frame = c32_w * c32_h * 32
    stage1_to_stage2_deleted = 2 * c16_frame
    stage2_to_stage3_read_deleted = c16_frame
    avoided = stage1_to_stage2_deleted + stage2_to_stage3_read_deleted

    print("FSR4 INT8 DP4A tiled-fusion first-island budget")
    print(f"display: {args.width}x{args.height}")
    print(f"C16 model plane: {c16_w}x{c16_h}x16 = {c16_frame:,} bytes ({mib(c16_frame):.2f} MiB)")
    print(f"Stage3 C32 plane: {c32_w}x{c32_h}x32 = {c32_stage3_frame:,} bytes ({mib(c32_stage3_frame):.2f} MiB)")
    print()
    print("macro-tile contract:")
    for tile in (source, stage1_local, stage2_local, stage3_out):
        print(f"  {tile.name}: {tile.width}x{tile.height}x{tile.channels} = {tile.bytes:,} bytes")
    print()
    print("conservative deleted global traffic:")
    print(f"  Stage1 global write + Stage2 global read: {stage1_to_stage2_deleted:,} bytes ({mib(stage1_to_stage2_deleted):.2f} MiB/frame, {gbps(stage1_to_stage2_deleted, args.fps):.2f} GB/s @ {args.fps:g} FPS)")
    print(f"  Stage2 main-path global read for Stage3: {stage2_to_stage3_read_deleted:,} bytes ({mib(stage2_to_stage3_read_deleted):.2f} MiB/frame, {gbps(stage2_to_stage3_read_deleted, args.fps):.2f} GB/s @ {args.fps:g} FPS)")
    print(f"  total avoided: {avoided:,} bytes ({mib(avoided):.2f} MiB/frame, {gbps(avoided, args.fps):.2f} GB/s @ {args.fps:g} FPS)")
    print()
    print("required preserved traffic:")
    print(f"  SkipA sideband store remains: {c16_frame:,} bytes ({mib(c16_frame):.2f} MiB/frame)")
    print("  exact int8 quantization boundaries after Stage1 and Stage2 are preserved locally")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
