#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate the Stage1+2 paired-macroblock Stage2->Stage3 contract."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tools.analysis.middle_core_spatial_graph import (  # noqa: E402
    CELL_SHAPES,
    RESIDENT_PARAMETER_ARENA_BYTES,
    STAGE1_HOT_RECORD_BYTES,
    STAGE1_2_PAIRED_MACROBLOCK_SOURCE_BYTES,
    STAGE1_2_PAIRED_MACROBLOCK_STAGE1_PATCH_BYTES,
    COMPUTE_ROWS,
    CURRENT_PROGRAMMED_COLUMN,
    STAGE1_2_TILE_ROW,
    STAGE3_TILE_ROW,
    build_current_reserved_tile_map,
    build_stage1_2_to_stage3_programmed_shards,
    build_graph_phase_plan,
)
from tools.analysis.middle_core_spatial_plan import (  # noqa: E402
    DOWNSAMPLE_LADDER_CELL_ROW_ALIGNMENT,
    TRANSPORT_CELL_ROWS,
    build_frame_plan,
)

PROFILES = ("spatial2col", "spatial4col")
DEVICES = ("npu1", "npu2")
CAPACITIES = ("1080", "2160", "4320")


def _validate_one(profile: str, device: str, capacity: str) -> list[str]:
    errors: list[str] = []
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups="auto")
    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=capacity,
        lane_groups="auto",
        graph_phase="stage1_2-to-stage3-paired-macroblock-resident",
    )
    shards = build_stage1_2_to_stage3_programmed_shards(plan)

    label = f"{profile}/{device}/R{capacity}"
    if frame.seam_rows % DOWNSAMPLE_LADDER_CELL_ROW_ALIGNMENT != 0:
        errors.append(f"{label}: seam_rows must be padded to the full downsample ladder alignment")
    if frame.cell_rows % 4 != 0:
        errors.append(f"{label}: C16 cell rows must be divisible by four for Stage3 and Stage6")
    if frame.cell_columns % 2 != 0:
        errors.append(f"{label}: C16 cell columns must be even for Stage3 paired macroblocks")

    output_columns = sum(shard.output_cell_columns for shard in shards)
    if output_columns * 2 != frame.cell_columns:
        errors.append(f"{label}: Stage3 macroblock columns must cover every Stage2 C16 column exactly once")
    if not shards:
        errors.append(f"{label}: expected one programmed Stage1+2->Stage3 shard")
    elif len(shards) != 1:
        errors.append(f"{label}: current programmed graph must use exactly one physical Stage1+2->Stage3 shard, got {len(shards)}")
    elif shards[0].physical_column != CURRENT_PROGRAMMED_COLUMN:
        errors.append(f"{label}: current programmed shard must live on physical column {CURRENT_PROGRAMMED_COLUMN}")
    if any(shard.output_cell_rows * 2 != frame.cell_rows for shard in shards):
        errors.append(f"{label}: Stage3 macroblock rows must cover every Stage2 C16 row exactly once")

    tile_map = build_current_reserved_tile_map(plan)
    active_compute = [slot for slot in tile_map if slot.tile_kind == "compute" and slot.status == "active"]
    if len(active_compute) != 2:
        errors.append(f"{label}: current graph must instantiate exactly two active compute tiles, got {len(active_compute)}")
    active_coords = {(slot.physical_column, slot.row) for slot in active_compute}
    expected_coords = {(CURRENT_PROGRAMMED_COLUMN, STAGE1_2_TILE_ROW), (CURRENT_PROGRAMMED_COLUMN, STAGE3_TILE_ROW)}
    if active_coords != expected_coords:
        errors.append(f"{label}: active compute tiles are {sorted(active_coords)}, expected {sorted(expected_coords)}")
    expected_reserved_compute = plan.macro_lane_columns * len(COMPUTE_ROWS)
    actual_reserved_compute = sum(1 for slot in tile_map if slot.tile_kind == "compute")
    if actual_reserved_compute != expected_reserved_compute:
        errors.append(f"{label}: reserved compute-tile footprint is {actual_reserved_compute}, expected {expected_reserved_compute}")

    seen: set[tuple[int, int]] = set()
    for shard in shards:
        for out_y in range(shard.output_cell_rows):
            for out_x in range(shard.output_cell_columns):
                first_x = shard.input_first_cell_column + out_x * 2
                first_y = out_y * 2
                for dy in (0, 1):
                    for dx in (0, 1):
                        coord = (first_x + dx, first_y + dy)
                        if coord in seen:
                            errors.append(f"{label}: duplicate Stage2 C16 production at cell {coord}")
                        seen.add(coord)
    expected_cells = frame.cell_columns * frame.cell_rows
    if len(seen) != expected_cells:
        errors.append(f"{label}: paired macroblock coverage produced {len(seen)} cells, expected {expected_cells}")

    # Compile-visible L1 budget for the paired producer. Stage3 keeps a
    # double-buffered shared 2x2 handoff. SkipA is a sideband drain and stays
    # single-buffered; making it depth two repeats the Stage2 payload and exceeds
    # the 64 KiB compute-tile data memory budget.
    c16_window2x2 = 4 * CELL_SHAPES["c16_cell"].element_bytes
    producer_l1_bytes = (
        0x3000  # worker stack
        + STAGE1_2_PAIRED_MACROBLOCK_SOURCE_BYTES
        + 2 * c16_window2x2  # Stage2 -> Stage3 shared ping/pong
        + c16_window2x2  # single-buffered SkipA strided sideband
        + 2 * STAGE1_HOT_RECORD_BYTES
        + 16  # small allocator slop observed in lowered MLIR
    )
    if producer_l1_bytes >= 64 * 1024:
        errors.append(f"{label}: paired producer L1 footprint exceeds one 64 KiB compute tile: {producer_l1_bytes}")
    if RESIDENT_PARAMETER_ARENA_BYTES <= 0:
        errors.append(f"{label}: resident arena size must be positive")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", choices=("check", "summary"), default="check")
    args = parser.parse_args()

    all_errors: list[str] = []
    summaries: list[str] = []
    for profile in PROFILES:
        for device in DEVICES:
            for capacity in CAPACITIES:
                errors = _validate_one(profile, device, capacity)
                all_errors.extend(errors)
                if not errors:
                    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups="auto")
                    summaries.append(
                        f"{profile}/{device}/R{capacity}: padded_output={frame.model_output_width}x{frame.model_output_rows}, "
                        f"C16_cells={frame.cell_columns}x{frame.cell_rows}"
                    )
    if all_errors:
        for error in all_errors:
            print(error)
        return 1
    if args.dump == "summary":
        print("\n".join(summaries))
    else:
        print("paired macroblock contract validated across profiles/devices/capacities")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
