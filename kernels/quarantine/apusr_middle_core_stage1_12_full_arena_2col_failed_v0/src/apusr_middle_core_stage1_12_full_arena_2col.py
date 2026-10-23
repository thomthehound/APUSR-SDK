#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the stage1->stage12 full-fabric graph using epoch-loaded parameters.

This is the product-shaped middle-core profile. Logical stages 1..12 are
instantiated as two-column macro-lane groups inside one frame-conveyor graph,
with resident parameters, memory-tile skip rings, C8-grouped internal C32/C64
seams, and zero hot-path parameter DMA.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np
from aie.extras.context import mlir_mod_ctx
from aie.helpers.taplib.tap import TensorAccessPattern

from aie.dialects.aie import *  # noqa: F403 - MLIR-AIE examples use this style.
from aie.dialects.aiex import *  # noqa: F403 - shim DMA task helpers.
from aie.iron.controlflow import range_

C8_CHUNKS = 4
FRONT_SKIP_TILES = 4
FRONT_SUBTILES_PER_SKIP = 4
FRONT_SUBTILES = FRONT_SKIP_TILES * FRONT_SUBTILES_PER_SKIP
STAGE5_QUADRANTS = 4
STAGE9_CHUNKS = 4
STAGE10_QUADRANTS = 4
STAGE11_QUADRANTS = 4
STAGE12_SUBTILES = 4

C16_HALO_BYTES = 18 * 10 * 16
C16_TILE_BYTES = 16 * 8 * 16
STAGE1_INPUT_PACKAGE_BYTES = FRONT_SUBTILES * C16_HALO_BYTES
STAGE1_PARAM_BYTES = 4048
STAGE1_OUTPUT_PACKAGE_BYTES = FRONT_SUBTILES * C16_TILE_BYTES
STAGE0_INPUT_PACKAGE_BYTES = STAGE1_OUTPUT_PACKAGE_BYTES
STAGE2_HALO_PACKAGE_BYTES = FRONT_SUBTILES * C16_HALO_BYTES
STAGE2_PARAM_BYTES = 4048
STAGE2_OUTPUT_PACKAGE_BYTES = FRONT_SUBTILES * C16_TILE_BYTES
STAGE3_DOWNSOURCE_TILE_BYTES = 40 * 20 * 16
STAGE3_DOWNSOURCE_PACKAGE_BYTES = FRONT_SKIP_TILES * STAGE3_DOWNSOURCE_TILE_BYTES
STAGE3_PARAM_BYTES = 10000
STAGE3_OUTPUT_TILE_BYTES = 16 * 8 * 32
STAGE3_OUTPUT_PACKAGE_BYTES = FRONT_SKIP_TILES * STAGE3_OUTPUT_TILE_BYTES
STAGE5_HALO_CHUNK_BYTES = 18 * 10 * 8
STAGE5_HALO_PACKAGE_BYTES = STAGE5_QUADRANTS * C8_CHUNKS * STAGE5_HALO_CHUNK_BYTES
STAGE5_PARAM_BYTES = 8192
STAGE5_OUTPUT_CHUNK_BYTES = 16 * 8 * 8
STAGE5_OUTPUT_TILE_BYTES = C8_CHUNKS * STAGE5_OUTPUT_CHUNK_BYTES
STAGE5_OUTPUT_PACKAGE_BYTES = STAGE5_QUADRANTS * STAGE5_OUTPUT_TILE_BYTES
STAGE6_DOWNSOURCE_BYTES = C8_CHUNKS * 40 * 20 * 8
STAGE6_PARAM_BYTES = 36880
STAGE6_OUTPUT_BYTES = 16 * 8 * 64
STAGE8_INPUT_BYTES = 18 * 10 * 64
STAGE8_PARAM_BYTES = 28176
STAGE8_OUTPUT_BYTES = 16 * 8 * 64
STAGE9_SKIP_CHUNK_BYTES = 32 * 16 * 8
STAGE9_PARAM_CHUNK_BYTES = 2224
STAGE9_HALO_CHUNK_BYTES = 18 * 10 * 8
STAGE10_PARAM_BYTES = 7696
STAGE10_OUTPUT_CHUNK_BYTES = 16 * 8 * 8
STAGE10_OUTPUT_TILE_BYTES = C8_CHUNKS * STAGE10_OUTPUT_CHUNK_BYTES
STAGE10_PACKAGE_BYTES = STAGE10_QUADRANTS * STAGE10_OUTPUT_TILE_BYTES
STAGE11_HALO_CHUNK_BYTES = 18 * 10 * 8
STAGE11_HALO_PACKAGE_BYTES = STAGE11_QUADRANTS * C8_CHUNKS * STAGE11_HALO_CHUNK_BYTES
STAGE11_SKIP_TILE_BYTES = 32 * 16 * 16
STAGE11_SKIP_PACKAGE_BYTES = STAGE11_QUADRANTS * STAGE11_SKIP_TILE_BYTES
STAGE11_PARAM_BYTES = 10064
STAGE11_OUTPUT_TILE_BYTES = 32 * 16 * 16
STAGE12_HALO_BYTES = 18 * 10 * 16
STAGE12_HALO_PACKAGE_BYTES = STAGE12_SUBTILES * STAGE12_HALO_BYTES
STAGE12_PARAM_BYTES = 4048
STAGE12_OUTPUT_TILE_BYTES = 16 * 8 * 16
STAGE12_OUTPUT_PACKAGE_BYTES = STAGE11_QUADRANTS * STAGE12_SUBTILES * STAGE12_OUTPUT_TILE_BYTES

PARAM_STAGE1_OFFSET = 0
PARAM_STAGE2_OFFSET = 4064
PARAM_STAGE3_OFFSET = 8128
PARAM_STAGE5_OFFSET = 18144
PARAM_STAGE6_OFFSET = 26336
PARAM_STAGE8_OFFSET = 63232
PARAM_STAGE9_OFFSET = 91424
PARAM_STAGE10_OFFSET = 100320
PARAM_STAGE11_OFFSET = 108032
PARAM_STAGE12_OFFSET = 118112
PARAM_ARENA_BYTES = 122880

# The public product seam is a complete frame slot. Internally the graph
# traverses that slot as a dense array of 64x32 C16 macro-lane packages.
# These are FSR4 model-family capacity classes, not the user's active
# resolution, quality preset, or local test setup.
MACRO_PACKAGE_WIDTH = 64
MACRO_PACKAGE_ROWS = 32
MODEL_CAPACITY_OUTPUTS = {
    "1080": (1920, 1080),
    "2160": (3840, 2160),
    "4320": (7680, 4320),
}
FRAME_STAGE0_INPUT_OFFSET = 0
FRAME_EGRESS_OFFSET = 0
FRAME_SEAM_WIDTH = 0
FRAME_SEAM_ROWS = 0
FRAME_PACKAGE_COLUMNS = 0
FRAME_PACKAGE_ROWS = 0
FRAME_PACKAGE_COUNT = 0
FRAME_LANE_GROUPS = 1
FRAME_INGRESS_BYTES = 0
FRAME_EGRESS_BYTES = 0


def _align_up(value: int, alignment: int) -> int:
    remainder = value % alignment
    return value if remainder == 0 else value + alignment - remainder


def _lane_first_package_column(package_columns: int, lane_groups: int, lane_index: int) -> int:
    base_columns = package_columns // lane_groups
    extra_columns = package_columns % lane_groups
    return lane_index * base_columns + min(lane_index, extra_columns)


def _lane_package_columns(package_columns: int, lane_groups: int, lane_index: int) -> int:
    base_columns = package_columns // lane_groups
    extra_columns = package_columns % lane_groups
    return base_columns + (1 if lane_index < extra_columns else 0)


def _lane_first_package_index(lane_index: int) -> int:
    return _lane_first_package_column(FRAME_PACKAGE_COLUMNS, FRAME_LANE_GROUPS, lane_index) * FRAME_PACKAGE_ROWS


def _lane_package_count(lane_index: int) -> int:
    return _lane_package_columns(FRAME_PACKAGE_COLUMNS, FRAME_LANE_GROUPS, lane_index) * FRAME_PACKAGE_ROWS


def _configure_frame_capacity(model_resolution: str, lane_groups: int) -> None:
    global FRAME_SEAM_WIDTH
    global FRAME_SEAM_ROWS
    global FRAME_PACKAGE_COLUMNS
    global FRAME_PACKAGE_ROWS
    global FRAME_PACKAGE_COUNT
    global FRAME_LANE_GROUPS
    global FRAME_INGRESS_BYTES
    global FRAME_EGRESS_BYTES

    model_output = MODEL_CAPACITY_OUTPUTS.get(model_resolution)
    if model_output is None:
        raise ValueError(f"unknown FSR4 model capacity: {model_resolution}")
    if lane_groups < 1:
        raise ValueError("lane_groups must be positive")
    model_output_width, model_output_rows = model_output
    FRAME_SEAM_WIDTH = _align_up((model_output_width + 1) // 2, MACRO_PACKAGE_WIDTH)
    FRAME_SEAM_ROWS = _align_up((model_output_rows + 1) // 2, MACRO_PACKAGE_ROWS)
    FRAME_PACKAGE_COLUMNS = FRAME_SEAM_WIDTH // MACRO_PACKAGE_WIDTH
    FRAME_PACKAGE_ROWS = FRAME_SEAM_ROWS // MACRO_PACKAGE_ROWS
    FRAME_PACKAGE_COUNT = FRAME_PACKAGE_COLUMNS * FRAME_PACKAGE_ROWS
    FRAME_LANE_GROUPS = lane_groups
    if lane_groups > FRAME_PACKAGE_COLUMNS:
        raise ValueError("lane_groups cannot exceed package columns for the selected capacity")
    for lane_index in range(lane_groups):
        if _lane_package_count(lane_index) == 0:
            raise ValueError("every compiled lane stripe must own at least one package")
    FRAME_INGRESS_BYTES = FRAME_PACKAGE_COUNT * STAGE0_INPUT_PACKAGE_BYTES
    FRAME_EGRESS_BYTES = FRAME_PACKAGE_COUNT * STAGE12_OUTPUT_PACKAGE_BYTES


_configure_frame_capacity("2160", 4)

FIFO_DEPTH = 2
PARAM_FIFO_DEPTH = 1


def _resolve_device(device_name: str, lane_groups: int):
    if device_name in {"npu", "npu1"}:
        if lane_groups == 1:
            return AIEDevice.npu1_2col  # noqa: F405
        if lane_groups == 2:
            return AIEDevice.npu1  # noqa: F405
        raise ValueError("npu1 targets support at most two two-column macro-lane groups")
    if device_name == "npu2":
        if lane_groups == 1:
            return AIEDevice.npu2_2col  # noqa: F405
        if lane_groups == 2:
            return AIEDevice.npu2_4col  # noqa: F405
        if lane_groups == 4:
            return AIEDevice.npu2  # noqa: F405
        raise ValueError("npu2 targets support one, two, or four two-column macro-lane groups")
    raise ValueError(f"unknown AIE device: {device_name}")


def _byte_tap(total_bytes: int, offset: int, length: int) -> TensorAccessPattern:
    return TensorAccessPattern((1, total_bytes), offset, [1, 1, 1, length], [0, 0, 0, 1])


def _strided_byte_tap(total_bytes: int, offset: int, count: int, stride: int, length: int) -> TensorAccessPattern:
    return TensorAccessPattern((1, total_bytes), offset, [1, 1, count, length], [0, 0, stride, 1])


def build_kernel(device_name: str, model_resolution: str, lane_groups: int):
    _configure_frame_capacity(model_resolution, lane_groups)
    selected_device = _resolve_device(device_name, lane_groups)

    with mlir_mod_ctx() as ctx:

        @device(selected_device)  # noqa: F405
        def device_body():
            c16_halo_ty = np.ndarray[(C16_HALO_BYTES,), np.dtype[np.int8]]
            c16_tile_ty = np.ndarray[(C16_TILE_BYTES,), np.dtype[np.int8]]
            stage1_input_package_ty = np.ndarray[(STAGE1_INPUT_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage1_param_ty = np.ndarray[(STAGE1_PARAM_BYTES,), np.dtype[np.int8]]
            stage1_output_package_ty = np.ndarray[(STAGE1_OUTPUT_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage2_halo_package_ty = np.ndarray[(STAGE2_HALO_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage2_param_ty = np.ndarray[(STAGE2_PARAM_BYTES,), np.dtype[np.int8]]
            stage2_output_package_ty = np.ndarray[(STAGE2_OUTPUT_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage3_downsource_ty = np.ndarray[(STAGE3_DOWNSOURCE_TILE_BYTES,), np.dtype[np.int8]]
            stage3_downsource_package_ty = np.ndarray[(STAGE3_DOWNSOURCE_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage3_param_ty = np.ndarray[(STAGE3_PARAM_BYTES,), np.dtype[np.int8]]
            stage3_output_tile_ty = np.ndarray[(STAGE3_OUTPUT_TILE_BYTES,), np.dtype[np.int8]]
            stage3_output_package_ty = np.ndarray[(STAGE3_OUTPUT_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage5_halo_package_ty = np.ndarray[(STAGE5_HALO_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage5_halo_chunk_ty = np.ndarray[(STAGE5_HALO_CHUNK_BYTES,), np.dtype[np.int8]]
            stage5_param_ty = np.ndarray[(STAGE5_PARAM_BYTES,), np.dtype[np.int8]]
            output_chunk_ty = np.ndarray[(STAGE5_OUTPUT_CHUNK_BYTES,), np.dtype[np.int8]]
            stage5_package_ty = np.ndarray[(STAGE5_OUTPUT_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage6_downsource_ty = np.ndarray[(STAGE6_DOWNSOURCE_BYTES,), np.dtype[np.int8]]
            stage6_param_ty = np.ndarray[(STAGE6_PARAM_BYTES,), np.dtype[np.int8]]
            stage6_output_ty = np.ndarray[(STAGE6_OUTPUT_BYTES,), np.dtype[np.int8]]
            stage8_input_ty = np.ndarray[(STAGE8_INPUT_BYTES,), np.dtype[np.int8]]
            stage8_param_ty = np.ndarray[(STAGE8_PARAM_BYTES,), np.dtype[np.int8]]
            stage8_output_ty = np.ndarray[(STAGE8_OUTPUT_BYTES,), np.dtype[np.int8]]
            skipb_chunk_ty = np.ndarray[(STAGE9_SKIP_CHUNK_BYTES,), np.dtype[np.int8]]
            skipb_all_ty = np.ndarray[(STAGE9_CHUNKS * STAGE9_SKIP_CHUNK_BYTES,), np.dtype[np.int8]]
            stage9_param_ty = np.ndarray[(STAGE9_PARAM_CHUNK_BYTES,), np.dtype[np.int8]]
            stage9_param_all_ty = np.ndarray[(STAGE9_CHUNKS * STAGE9_PARAM_CHUNK_BYTES,), np.dtype[np.int8]]
            halo_chunk_ty = np.ndarray[(STAGE9_HALO_CHUNK_BYTES,), np.dtype[np.int8]]
            stage10_param_ty = np.ndarray[(STAGE10_PARAM_BYTES,), np.dtype[np.int8]]
            stage10_package_ty = np.ndarray[(STAGE10_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage11_halo_package_ty = np.ndarray[(STAGE11_HALO_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage11_halo_chunk_ty = np.ndarray[(STAGE11_HALO_CHUNK_BYTES,), np.dtype[np.int8]]
            skipa_tile_ty = np.ndarray[(STAGE11_SKIP_TILE_BYTES,), np.dtype[np.int8]]
            skipa_package_ty = np.ndarray[(STAGE11_SKIP_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage11_param_ty = np.ndarray[(STAGE11_PARAM_BYTES,), np.dtype[np.int8]]
            stage11_output_ty = np.ndarray[(STAGE11_OUTPUT_TILE_BYTES,), np.dtype[np.int8]]
            stage12_halo_package_ty = np.ndarray[(STAGE12_HALO_PACKAGE_BYTES,), np.dtype[np.int8]]
            stage12_halo_ty = np.ndarray[(STAGE12_HALO_BYTES,), np.dtype[np.int8]]
            stage12_param_ty = np.ndarray[(STAGE12_PARAM_BYTES,), np.dtype[np.int8]]
            stage12_output_ty = np.ndarray[(STAGE12_OUTPUT_TILE_BYTES,), np.dtype[np.int8]]
            param_arena_ty = np.ndarray[(PARAM_ARENA_BYTES,), np.dtype[np.int8]]
            frame_ingress_ty = np.ndarray[(FRAME_INGRESS_BYTES,), np.dtype[np.int8]]
            frame_egress_ty = np.ndarray[(FRAME_EGRESS_BYTES,), np.dtype[np.int8]]

            convnext16_tile = external_func(  # noqa: F405
                "apusr_middle_core_convnext16_tile_i8",
                inputs=[c16_halo_ty, stage1_param_ty, c16_tile_ty],
                link_with="apusr_middle_core_convnext16_tile.o",
            )
            c16_halo16_bridge = external_func(  # noqa: F405
                "apusr_middle_core_c16_quadrants_stage_halo16_tile_i8",
                inputs=[stage1_output_package_ty, stage2_halo_package_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            stage2_bridge = external_func(  # noqa: F405
                "apusr_middle_core_stage2_c16_skipa_downsource4_tile_i8",
                inputs=[stage2_output_package_ty, skipa_package_ty, stage3_downsource_package_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            down_fnb32_tile = external_func(  # noqa: F405
                "apusr_middle_core_down2x2_fasternet32_tile_i8",
                inputs=[stage3_downsource_ty, stage3_param_ty, stage3_output_tile_ty],
                link_with="apusr_middle_core_down2x2_fasternet32_tile.o",
            )
            stage3_to_stage5_bridge = external_func(  # noqa: F405
                "apusr_middle_core_stage3_4_c32_stage5_halo4_c8group_tile_i8",
                inputs=[stage3_output_package_ty, stage5_halo_package_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            fnb32_c8group_tile = external_func(  # noqa: F405
                "apusr_middle_core_fasternet32_c8group_tile_i8",
                inputs=[stage5_halo_chunk_ty, stage5_halo_chunk_ty, stage5_halo_chunk_ty, stage5_halo_chunk_ty, stage5_param_ty, output_chunk_ty, output_chunk_ty, output_chunk_ty, output_chunk_ty],
                link_with="apusr_middle_core_fasternet32_c8group_tile.o",
            )
            stage5_bridge = external_func(  # noqa: F405
                "apusr_middle_core_stage5_c8group_skipb_downsource_tile_i8",
                inputs=[stage5_package_ty, skipb_all_ty, stage6_downsource_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            down_fnb64_tile = external_func(  # noqa: F405
                "apusr_middle_core_down2x2_fasternet64_tile_i8",
                inputs=[stage6_downsource_ty, stage6_param_ty, stage6_output_ty],
                link_with="apusr_middle_core_down2x2_fasternet64_tile.o",
            )
            stage6_to_stage8_bridge = external_func(  # noqa: F405
                "apusr_middle_core_stage6_7_c64_stage8_halo_tile_i8",
                inputs=[stage6_output_ty, stage8_input_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            fnb64_tile = external_func(  # noqa: F405
                "apusr_middle_core_fasternet64_tile_i8",
                inputs=[stage8_input_ty, stage8_param_ty, stage8_output_ty],
                link_with="apusr_middle_core_fasternet64_tile.o",
            )
            ct2d32_halo4_tile = external_func(  # noqa: F405
                "apusr_middle_core_ct2d32_ochunk_halo4_add_tile_i8",
                inputs=[stage8_output_ty, skipb_chunk_ty, stage9_param_ty, halo_chunk_ty, halo_chunk_ty, halo_chunk_ty, halo_chunk_ty],
                link_with="apusr_middle_core_ct2d32_ochunk_halo4_add_tile.o",
            )
            stage10_to_stage11_bridge = external_func(  # noqa: F405
                "apusr_middle_core_stage10_c8group_stage11_halo4_c8group_tile_i8",
                inputs=[stage10_package_ty, stage11_halo_package_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            stage11_join = external_func(  # noqa: F405
                "apusr_middle_core_fnb32_c8group_ct2d16_add_tile_i8",
                inputs=[stage11_halo_chunk_ty, stage11_halo_chunk_ty, stage11_halo_chunk_ty, stage11_halo_chunk_ty, skipa_tile_ty, stage11_param_ty, stage11_output_ty],
                link_with="apusr_middle_core_fnb32_c8group_ct2d16_add_tile.o",
            )
            stage11_to_stage12_bridge = external_func(  # noqa: F405
                "apusr_middle_core_stage11_c16_stage12_halo4_tile_i8",
                inputs=[stage11_output_ty, stage12_halo_package_ty],
                link_with="apusr_middle_core_queue_bridge_tiles.o",
            )
            stage12_convnext = external_func(  # noqa: F405
                "apusr_middle_core_convnext16_tile_i8",
                inputs=[stage12_halo_ty, stage12_param_ty, stage12_output_ty],
                link_with="apusr_middle_core_convnext16_tile.o",
            )

            def build_lane(lane_index: int):
                prefix = f"l{lane_index}_"
                column_base = lane_index * 2
                first_package_index = _lane_first_package_index(lane_index)
                package_count = _lane_package_count(lane_index)
                shim0 = tile(column_base + 0, 0)  # noqa: F405
                shim1 = tile(column_base + 1, 0)  # noqa: F405
                mem0 = tile(column_base + 0, 1)  # noqa: F405
                mem1 = tile(column_base + 1, 1)  # noqa: F405
                stage8_bridge10_tile = tile(column_base + 0, 2)  # noqa: F405
                stage12_upper_tile = tile(column_base + 1, 2)  # noqa: F405
                stage_worker_tiles = [tile(column_base + 0, 3), tile(column_base + 1, 3), tile(column_base + 0, 4), tile(column_base + 1, 4)]  # noqa: F405
                stage6_bridge11_tile = tile(column_base + 0, 5)  # noqa: F405
                stage5_bridge_stage10_stage12_lower_tile = tile(column_base + 1, 5)  # noqa: F405
                stage12_tiles = [stage12_upper_tile, stage5_bridge_stage10_stage12_lower_tile]


                stage0_package_l3l2 = object_fifo(f"{prefix}s1_12_s0_pkg_l3l2", shim0, mem0, FIFO_DEPTH, stage1_output_package_ty)  # noqa: F405
                stage0_package_l2l1 = object_fifo(f"{prefix}s1_12_s0_pkg_l2l1", mem0, stage8_bridge10_tile, FIFO_DEPTH, stage1_output_package_ty)  # noqa: F405
                object_fifo_link(stage0_package_l3l2, stage0_package_l2l1)  # noqa: F405

                stage1_halo_pkg_l1l2 = object_fifo(f"{prefix}s1_12_s1_halo_pkg_l1l2", stage8_bridge10_tile, mem0, FIFO_DEPTH, stage1_input_package_ty)  # noqa: F405
                stage1_halo_l2l1 = [[None for _ in range(FRONT_SUBTILES_PER_SKIP)] for _ in range(FRONT_SKIP_TILES)]
                for quadrant in range(FRONT_SKIP_TILES):
                    for subtile in range(FRONT_SUBTILES_PER_SKIP):
                        stage1_halo_l2l1[quadrant][subtile] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s1_halo_q{quadrant}_s{subtile}",
                            mem0,
                            stage_worker_tiles[quadrant],
                            FIFO_DEPTH,
                            c16_halo_ty,
                        )
                object_fifo_link(  # noqa: F405
                    stage1_halo_pkg_l1l2,
                    [stage1_halo_l2l1[quadrant][subtile] for quadrant in range(FRONT_SKIP_TILES) for subtile in range(FRONT_SUBTILES_PER_SKIP)],
                    [],
                    [(quadrant * FRONT_SUBTILES_PER_SKIP + subtile) * C16_HALO_BYTES for quadrant in range(FRONT_SKIP_TILES) for subtile in range(FRONT_SUBTILES_PER_SKIP)],
                )

                stage1_param_l3l2 = object_fifo(f"{prefix}s1_12_s1_param_l3l2", shim0, mem0, PARAM_FIFO_DEPTH, stage1_param_ty)  # noqa: F405
                stage1_param_l2l1 = object_fifo(f"{prefix}s1_12_s1_param_l2l1", mem0, stage_worker_tiles, PARAM_FIFO_DEPTH, stage1_param_ty)  # noqa: F405
                object_fifo_link(stage1_param_l3l2, stage1_param_l2l1)  # noqa: F405
                stage2_param_l3l2 = object_fifo(f"{prefix}s1_12_s2_param_l3l2", shim0, mem0, PARAM_FIFO_DEPTH, stage2_param_ty)  # noqa: F405
                stage2_param_l2l1 = object_fifo(f"{prefix}s1_12_s2_param_l2l1", mem0, stage_worker_tiles, PARAM_FIFO_DEPTH, stage2_param_ty)  # noqa: F405
                object_fifo_link(stage2_param_l3l2, stage2_param_l2l1)  # noqa: F405
                stage3_param_l3l2 = object_fifo(f"{prefix}s1_12_s3_param_l3l2", shim0, mem0, PARAM_FIFO_DEPTH, stage3_param_ty)  # noqa: F405
                stage3_param_l2l1 = object_fifo(f"{prefix}s1_12_s3_param_l2l1", mem0, stage_worker_tiles, PARAM_FIFO_DEPTH, stage3_param_ty)  # noqa: F405
                object_fifo_link(stage3_param_l3l2, stage3_param_l2l1)  # noqa: F405

                stage1_out_l1l2 = [[None for _ in range(FRONT_SUBTILES_PER_SKIP)] for _ in range(FRONT_SKIP_TILES)]
                stage1_out_sources = []
                stage1_out_offsets = []
                for quadrant in range(FRONT_SKIP_TILES):
                    for subtile in range(FRONT_SUBTILES_PER_SKIP):
                        stage1_out_l1l2[quadrant][subtile] = object_fifo(f"{prefix}s1_12_s1_out_q{quadrant}_s{subtile}", stage_worker_tiles[quadrant], mem0, FIFO_DEPTH, c16_tile_ty)  # noqa: F405
                        stage1_out_sources.append(stage1_out_l1l2[quadrant][subtile])
                        stage1_out_offsets.append((quadrant * FRONT_SUBTILES_PER_SKIP + subtile) * C16_TILE_BYTES)
                stage1_package_l2l1 = object_fifo(f"{prefix}s1_12_s1_pkg_l2l1", mem0, stage8_bridge10_tile, FIFO_DEPTH, stage1_output_package_ty)  # noqa: F405
                object_fifo_link(stage1_out_sources, stage1_package_l2l1, stage1_out_offsets, [])  # noqa: F405

                stage2_halo_pkg_l1l2 = object_fifo(f"{prefix}s1_12_s2_halo_pkg_l1l2", stage8_bridge10_tile, mem0, FIFO_DEPTH, stage2_halo_package_ty)  # noqa: F405
                stage2_halo_l2l1 = [[None for _ in range(FRONT_SUBTILES_PER_SKIP)] for _ in range(FRONT_SKIP_TILES)]
                for quadrant in range(FRONT_SKIP_TILES):
                    for subtile in range(FRONT_SUBTILES_PER_SKIP):
                        stage2_halo_l2l1[quadrant][subtile] = object_fifo(f"{prefix}s1_12_s2_halo_q{quadrant}_s{subtile}", mem0, stage_worker_tiles[quadrant], FIFO_DEPTH, c16_halo_ty)  # noqa: F405
                object_fifo_link(stage2_halo_pkg_l1l2, [stage2_halo_l2l1[quadrant][subtile] for quadrant in range(FRONT_SKIP_TILES) for subtile in range(FRONT_SUBTILES_PER_SKIP)], [], [(quadrant * FRONT_SUBTILES_PER_SKIP + subtile) * C16_HALO_BYTES for quadrant in range(FRONT_SKIP_TILES) for subtile in range(FRONT_SUBTILES_PER_SKIP)])  # noqa: F405

                stage2_out_l1l2 = [[None for _ in range(FRONT_SUBTILES_PER_SKIP)] for _ in range(FRONT_SKIP_TILES)]
                stage2_out_sources = []
                stage2_out_offsets = []
                for quadrant in range(FRONT_SKIP_TILES):
                    for subtile in range(FRONT_SUBTILES_PER_SKIP):
                        stage2_out_l1l2[quadrant][subtile] = object_fifo(f"{prefix}s1_12_s2_out_q{quadrant}_s{subtile}", stage_worker_tiles[quadrant], mem0, FIFO_DEPTH, c16_tile_ty)  # noqa: F405
                        stage2_out_sources.append(stage2_out_l1l2[quadrant][subtile])
                        stage2_out_offsets.append((quadrant * FRONT_SUBTILES_PER_SKIP + subtile) * C16_TILE_BYTES)
                stage2_package_l2l1 = object_fifo(f"{prefix}s1_12_s2_pkg_l2l1", mem0, stage8_bridge10_tile, FIFO_DEPTH, stage2_output_package_ty)  # noqa: F405
                object_fifo_link(stage2_out_sources, stage2_package_l2l1, stage2_out_offsets, [])  # noqa: F405

                skipa_l1l2 = object_fifo(f"{prefix}s1_12_skipa_l1l2", stage8_bridge10_tile, mem1, FIFO_DEPTH, skipa_package_ty)  # noqa: F405
                stage3_downsource_pkg_l1l2 = object_fifo(f"{prefix}s1_12_s3_downsource_pkg_l1l2", stage8_bridge10_tile, mem0, FIFO_DEPTH, stage3_downsource_package_ty)  # noqa: F405
                stage3_downsource_l2l1 = [object_fifo(f"{prefix}s1_12_s3_downsource_q{quadrant}", mem0, stage_worker_tiles[quadrant], FIFO_DEPTH, stage3_downsource_ty) for quadrant in range(FRONT_SKIP_TILES)]  # noqa: F405
                object_fifo_link(stage3_downsource_pkg_l1l2, stage3_downsource_l2l1, [], [quadrant * STAGE3_DOWNSOURCE_TILE_BYTES for quadrant in range(FRONT_SKIP_TILES)])  # noqa: F405

                stage3_out_l1l2 = [object_fifo(f"{prefix}s1_12_s3_out_q{quadrant}", stage_worker_tiles[quadrant], mem0, FIFO_DEPTH, stage3_output_tile_ty) for quadrant in range(FRONT_SKIP_TILES)]  # noqa: F405
                stage3_package_l2l1 = object_fifo(f"{prefix}s1_12_s3_pkg_l2l1", mem0, stage8_bridge10_tile, FIFO_DEPTH, stage3_output_package_ty)  # noqa: F405
                object_fifo_link(stage3_out_l1l2, stage3_package_l2l1, [quadrant * STAGE3_OUTPUT_TILE_BYTES for quadrant in range(FRONT_SKIP_TILES)], [])  # noqa: F405

                stage5_halo_l3l2 = object_fifo(f"{prefix}s1_12_s5_halo_l1l2", stage8_bridge10_tile, mem0, FIFO_DEPTH, stage5_halo_package_ty)  # noqa: F405
                stage5_halo_l2l1 = [[None for _ in range(C8_CHUNKS)] for _ in range(STAGE5_QUADRANTS)]
                for quadrant in range(STAGE5_QUADRANTS):
                    for chunk in range(C8_CHUNKS):
                        stage5_halo_l2l1[quadrant][chunk] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s5_halo_q{quadrant}_c{chunk}",
                            mem0,
                            stage_worker_tiles[quadrant],
                            FIFO_DEPTH,
                            stage5_halo_chunk_ty,
                        )
                object_fifo_link(  # noqa: F405
                    stage5_halo_l3l2,
                    [stage5_halo_l2l1[quadrant][chunk] for quadrant in range(STAGE5_QUADRANTS) for chunk in range(C8_CHUNKS)],
                    [],
                    [(quadrant * C8_CHUNKS + chunk) * STAGE5_HALO_CHUNK_BYTES for quadrant in range(STAGE5_QUADRANTS) for chunk in range(C8_CHUNKS)],
                )

                stage5_param_l3l2 = object_fifo(f"{prefix}s1_12_s5_param_l3l2", shim0, mem0, PARAM_FIFO_DEPTH, stage5_param_ty)  # noqa: F405
                stage5_param_l2l1 = object_fifo(f"{prefix}s1_12_s5_param_l2l1", mem0, stage_worker_tiles, PARAM_FIFO_DEPTH, stage5_param_ty)  # noqa: F405
                object_fifo_link(stage5_param_l3l2, stage5_param_l2l1)  # noqa: F405

                stage5_out_l1l2 = [[None for _ in range(C8_CHUNKS)] for _ in range(STAGE5_QUADRANTS)]
                stage5_package_inputs = []
                stage5_package_offsets = []
                for quadrant in range(STAGE5_QUADRANTS):
                    for chunk in range(C8_CHUNKS):
                        stage5_out_l1l2[quadrant][chunk] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s5_out_q{quadrant}_c{chunk}",
                            stage_worker_tiles[quadrant],
                            mem0,
                            FIFO_DEPTH,
                            output_chunk_ty,
                        )
                        stage5_package_inputs.append(stage5_out_l1l2[quadrant][chunk])
                        stage5_package_offsets.append(quadrant * STAGE5_OUTPUT_TILE_BYTES + chunk * STAGE5_OUTPUT_CHUNK_BYTES)
                stage5_package_l2l1 = object_fifo(f"{prefix}s1_12_s5_pkg_l2l1", mem0, stage5_bridge_stage10_stage12_lower_tile, FIFO_DEPTH, stage5_package_ty)  # noqa: F405
                object_fifo_link(stage5_package_inputs, stage5_package_l2l1, stage5_package_offsets, [])  # noqa: F405

                skipb_l1l2 = object_fifo(f"{prefix}s1_12_skipb_l1l2", stage5_bridge_stage10_stage12_lower_tile, mem1, FIFO_DEPTH, skipb_all_ty)  # noqa: F405
                skipb_l2l1 = [object_fifo(f"{prefix}s1_12_skipb_c8_{chunk}", mem1, stage_worker_tiles[chunk], FIFO_DEPTH, skipb_chunk_ty) for chunk in range(STAGE9_CHUNKS)]  # noqa: F405
                object_fifo_link(skipb_l1l2, skipb_l2l1, [], [chunk * STAGE9_SKIP_CHUNK_BYTES for chunk in range(STAGE9_CHUNKS)])  # noqa: F405

                stage6_downsource_l1l2 = object_fifo(f"{prefix}s1_12_s6_downsource_l1l2", stage5_bridge_stage10_stage12_lower_tile, mem1, FIFO_DEPTH, stage6_downsource_ty)  # noqa: F405
                stage6_downsource_l2l1 = object_fifo(f"{prefix}s1_12_s6_downsource_l2l1", mem1, stage6_bridge11_tile, FIFO_DEPTH, stage6_downsource_ty)  # noqa: F405
                object_fifo_link(stage6_downsource_l1l2, stage6_downsource_l2l1)  # noqa: F405

                stage6_param_l3l2 = object_fifo(f"{prefix}s1_12_s6_param_l3l2", shim0, mem0, PARAM_FIFO_DEPTH, stage6_param_ty)  # noqa: F405
                stage6_param_l2l1 = object_fifo(f"{prefix}s1_12_s6_param_l2l1", mem0, stage6_bridge11_tile, PARAM_FIFO_DEPTH, stage6_param_ty)  # noqa: F405
                object_fifo_link(stage6_param_l3l2, stage6_param_l2l1)  # noqa: F405

                stage6_out_l1l2 = object_fifo(f"{prefix}s1_12_s6_out_l1l2", stage6_bridge11_tile, mem1, FIFO_DEPTH, stage6_output_ty)  # noqa: F405
                stage6_out_l2l1 = object_fifo(f"{prefix}s1_12_s6_out_l2l1", mem1, stage12_upper_tile, FIFO_DEPTH, stage6_output_ty)  # noqa: F405
                object_fifo_link(stage6_out_l1l2, stage6_out_l2l1)  # noqa: F405

                stage8_in_l1l2 = object_fifo(f"{prefix}s1_12_s8_in_l1l2", stage12_upper_tile, mem0, FIFO_DEPTH, stage8_input_ty)  # noqa: F405
                stage8_in_l2l1 = object_fifo(f"{prefix}s1_12_s8_in_l2l1", mem0, stage8_bridge10_tile, FIFO_DEPTH, stage8_input_ty)  # noqa: F405
                object_fifo_link(stage8_in_l1l2, stage8_in_l2l1)  # noqa: F405

                stage8_param_l3l2 = object_fifo(f"{prefix}s1_12_s8_param_l3l2", shim0, mem0, PARAM_FIFO_DEPTH, stage8_param_ty)  # noqa: F405
                stage8_param_l2l1 = object_fifo(f"{prefix}s1_12_s8_param_l2l1", mem0, stage8_bridge10_tile, PARAM_FIFO_DEPTH, stage8_param_ty)  # noqa: F405
                object_fifo_link(stage8_param_l3l2, stage8_param_l2l1)  # noqa: F405

                stage8_l1l2 = object_fifo(f"{prefix}s1_12_s8_c64_l1l2", stage8_bridge10_tile, mem0, FIFO_DEPTH, stage8_output_ty)  # noqa: F405
                stage8_fanout_l2l1 = object_fifo(f"{prefix}s1_12_s8_c64_fanout", mem0, stage_worker_tiles, FIFO_DEPTH, stage8_output_ty)  # noqa: F405
                object_fifo_link(stage8_l1l2, stage8_fanout_l2l1)  # noqa: F405

                stage9_param_l3l2 = object_fifo(f"{prefix}s1_12_s9_param_l3l2", shim1, mem1, PARAM_FIFO_DEPTH, stage9_param_all_ty)  # noqa: F405
                stage9_param_l2l1 = [object_fifo(f"{prefix}s1_12_s9_param_c8_{chunk}", mem1, stage_worker_tiles[chunk], PARAM_FIFO_DEPTH, stage9_param_ty) for chunk in range(STAGE9_CHUNKS)]  # noqa: F405
                object_fifo_link(stage9_param_l3l2, stage9_param_l2l1, [], [chunk * STAGE9_PARAM_CHUNK_BYTES for chunk in range(STAGE9_CHUNKS)])  # noqa: F405

                stage9_halo_l1l2 = [[None for _ in range(STAGE10_QUADRANTS)] for _ in range(STAGE9_CHUNKS)]
                stage10_halo_l2l1 = [[None for _ in range(STAGE9_CHUNKS)] for _ in range(STAGE10_QUADRANTS)]
                for chunk in range(STAGE9_CHUNKS):
                    stage9_outputs = []
                    for quadrant in range(STAGE10_QUADRANTS):
                        stage9_halo_l1l2[chunk][quadrant] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s9_halo_c{chunk}_q{quadrant}_l1l2",
                            stage_worker_tiles[chunk],
                            mem0 if quadrant < 2 else mem1,
                            FIFO_DEPTH,
                            halo_chunk_ty,
                        )
                        stage10_halo_l2l1[quadrant][chunk] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s10_halo_q{quadrant}_c{chunk}_l2l1",
                            mem0,
                            stage5_bridge_stage10_stage12_lower_tile,
                            FIFO_DEPTH,
                            halo_chunk_ty,
                        )
                        object_fifo_link(stage9_halo_l1l2[chunk][quadrant], stage10_halo_l2l1[quadrant][chunk])  # noqa: F405

                stage10_param_l3l2 = object_fifo(f"{prefix}s1_12_s10_param_l3l2", shim1, mem1, PARAM_FIFO_DEPTH, stage10_param_ty)  # noqa: F405
                stage10_param_l2l1 = object_fifo(f"{prefix}s1_12_s10_param_l2l1", mem1, stage5_bridge_stage10_stage12_lower_tile, PARAM_FIFO_DEPTH, stage10_param_ty)  # noqa: F405
                object_fifo_link(stage10_param_l3l2, stage10_param_l2l1)  # noqa: F405

                stage10_out_l1l2 = [[None for _ in range(STAGE9_CHUNKS)] for _ in range(STAGE10_QUADRANTS)]
                stage10_package_inputs = []
                stage10_package_offsets = []
                for quadrant in range(STAGE10_QUADRANTS):
                    for chunk in range(STAGE9_CHUNKS):
                        stage10_out_l1l2[quadrant][chunk] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s10_out_q{quadrant}_c{chunk}",
                            stage5_bridge_stage10_stage12_lower_tile,
                            mem1,
                            FIFO_DEPTH,
                            output_chunk_ty,
                        )
                        stage10_package_inputs.append(stage10_out_l1l2[quadrant][chunk])
                        stage10_package_offsets.append(quadrant * STAGE10_OUTPUT_TILE_BYTES + chunk * STAGE10_OUTPUT_CHUNK_BYTES)
                stage10_package_l2l1 = object_fifo(f"{prefix}s1_12_s10_pkg_l2l1", mem1, stage8_bridge10_tile, FIFO_DEPTH, stage10_package_ty)  # noqa: F405
                object_fifo_link(stage10_package_inputs, stage10_package_l2l1, stage10_package_offsets, [])  # noqa: F405

                stage11_halo_pkg_l1l2 = object_fifo(f"{prefix}s1_12_s11_halo_pkg_l1l2", stage8_bridge10_tile, mem0, FIFO_DEPTH, stage11_halo_package_ty)  # noqa: F405
                stage11_halo_l2l1 = [[None for _ in range(C8_CHUNKS)] for _ in range(STAGE11_QUADRANTS)]
                for quadrant in range(STAGE11_QUADRANTS):
                    for chunk in range(C8_CHUNKS):
                        stage11_halo_l2l1[quadrant][chunk] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s11_halo_q{quadrant}_c{chunk}",
                            mem0,
                            stage_worker_tiles[quadrant],
                            FIFO_DEPTH,
                            stage11_halo_chunk_ty,
                        )
                object_fifo_link(  # noqa: F405
                    stage11_halo_pkg_l1l2,
                    [stage11_halo_l2l1[quadrant][chunk] for quadrant in range(STAGE11_QUADRANTS) for chunk in range(C8_CHUNKS)],
                    [],
                    [((quadrant * C8_CHUNKS + chunk) * STAGE11_HALO_CHUNK_BYTES) for quadrant in range(STAGE11_QUADRANTS) for chunk in range(C8_CHUNKS)],
                )

                skipa_l2l1 = [object_fifo(f"{prefix}s1_12_skipa_q{quadrant}", mem1, stage_worker_tiles[quadrant], FIFO_DEPTH, skipa_tile_ty) for quadrant in range(STAGE11_QUADRANTS)]  # noqa: F405
                object_fifo_link(skipa_l1l2, skipa_l2l1, [], [quadrant * STAGE11_SKIP_TILE_BYTES for quadrant in range(STAGE11_QUADRANTS)])  # noqa: F405

                stage11_param_l3l2 = object_fifo(f"{prefix}s1_12_s11_param_l3l2", shim1, mem1, PARAM_FIFO_DEPTH, stage11_param_ty)  # noqa: F405
                stage11_param_l2l1 = object_fifo(f"{prefix}s1_12_s11_param_l2l1", mem1, stage_worker_tiles, PARAM_FIFO_DEPTH, stage11_param_ty)  # noqa: F405
                object_fifo_link(stage11_param_l3l2, stage11_param_l2l1)  # noqa: F405

                stage11_out_l1l2 = []
                stage11_out_l2l1 = []
                for quadrant in range(STAGE11_QUADRANTS):
                    stage11_out_l1l2.append(object_fifo(f"{prefix}s1_12_s11_out_q{quadrant}_l1l2", stage_worker_tiles[quadrant], mem1, FIFO_DEPTH, stage11_output_ty))  # noqa: F405
                    stage11_out_l2l1.append(object_fifo(f"{prefix}s1_12_s11_out_q{quadrant}_l2l1", mem1, stage6_bridge11_tile, FIFO_DEPTH, stage11_output_ty))  # noqa: F405
                    object_fifo_link(stage11_out_l1l2[quadrant], stage11_out_l2l1[quadrant])  # noqa: F405

                stage12_halo_pkg_l1l2 = []
                stage12_halo_l2l1 = [[None for _ in range(STAGE12_SUBTILES)] for _ in range(STAGE11_QUADRANTS)]
                for quadrant in range(STAGE11_QUADRANTS):
                    owner_stage12_tile = stage12_tiles[0 if quadrant < 2 else 1]
                    stage12_halo_pkg_l1l2.append(object_fifo(f"{prefix}s1_12_s12_halo_pkg_q{quadrant}", stage6_bridge11_tile, mem1, FIFO_DEPTH, stage12_halo_package_ty))  # noqa: F405
                    for subtile in range(STAGE12_SUBTILES):
                        stage12_halo_l2l1[quadrant][subtile] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s12_halo_q{quadrant}_s{subtile}",
                            mem1,
                            owner_stage12_tile,
                            FIFO_DEPTH,
                            stage12_halo_ty,
                        )
                    object_fifo_link(  # noqa: F405
                        stage12_halo_pkg_l1l2[quadrant],
                        [stage12_halo_l2l1[quadrant][subtile] for subtile in range(STAGE12_SUBTILES)],
                        [],
                        [subtile * STAGE12_HALO_BYTES for subtile in range(STAGE12_SUBTILES)],
                    )

                stage12_param_l3l2 = object_fifo(f"{prefix}s1_12_s12_param_l3l2", shim1, mem1, PARAM_FIFO_DEPTH, stage12_param_ty)  # noqa: F405
                stage12_param_l2l1 = object_fifo(f"{prefix}s1_12_s12_param_l2l1", mem1, stage12_tiles, PARAM_FIFO_DEPTH, stage12_param_ty)  # noqa: F405
                object_fifo_link(stage12_param_l3l2, stage12_param_l2l1)  # noqa: F405

                stage12_out_l1l2 = [[None for _ in range(STAGE12_SUBTILES)] for _ in range(STAGE11_QUADRANTS)]
                stage12_out_l2l3 = [[None for _ in range(STAGE12_SUBTILES)] for _ in range(STAGE11_QUADRANTS)]
                for quadrant in range(STAGE11_QUADRANTS):
                    owner_stage12_tile = stage12_tiles[0 if quadrant < 2 else 1]
                    for subtile in range(STAGE12_SUBTILES):
                        stage12_out_l1l2[quadrant][subtile] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s12_out_q{quadrant}_s{subtile}_l1l2",
                            owner_stage12_tile,
                            mem1,
                            FIFO_DEPTH,
                            stage12_output_ty,
                        )
                        stage12_out_l2l3[quadrant][subtile] = object_fifo(  # noqa: F405
                            f"{prefix}s1_12_s12_out_q{quadrant}_s{subtile}_l2l3",
                            mem1,
                            shim1,
                            FIFO_DEPTH,
                            stage12_output_ty,
                        )
                        object_fifo_link(stage12_out_l1l2[quadrant][subtile], stage12_out_l2l3[quadrant][subtile])  # noqa: F405

                @core(stage8_bridge10_tile, stack_size=0x5000)  # noqa: F405
                def stage8_bridge10_core():
                    stage8_param = stage8_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                    for _ in range_(sys.maxsize):
                        stage0_package = stage0_package_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage1_halo_package = stage1_halo_pkg_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        c16_halo16_bridge(stage0_package, stage1_halo_package)
                        stage0_package_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage1_halo_pkg_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        stage1_package = stage1_package_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage2_halo_package = stage2_halo_pkg_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        c16_halo16_bridge(stage1_package, stage2_halo_package)
                        stage1_package_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage2_halo_pkg_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        stage2_package = stage2_package_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        skipa_package = skipa_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage3_downsource_package = stage3_downsource_pkg_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage2_bridge(stage2_package, skipa_package, stage3_downsource_package)
                        stage2_package_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        skipa_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage3_downsource_pkg_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        stage3_package = stage3_package_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage5_halo_package = stage5_halo_l3l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage3_to_stage5_bridge(stage3_package, stage5_halo_package)
                        stage3_package_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage5_halo_l3l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        stage8_input = stage8_in_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage8_output = stage8_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        fnb64_tile(stage8_input, stage8_param, stage8_output)
                        stage8_in_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage8_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        stage10_package = stage10_package_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage11_halo_package = stage11_halo_pkg_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage10_to_stage11_bridge(stage10_package, stage11_halo_package)
                        stage10_package_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage11_halo_pkg_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                def make_stage5_stage9_stage11_core(index: int):

                    @core(stage_worker_tiles[index], stack_size=0x5000)  # noqa: F405
                    def stage5_stage9_stage11_core():
                        stage1_param = stage1_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage2_param = stage2_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage3_param = stage3_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage5_param = stage5_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage9_param = stage9_param_l2l1[index].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage11_param = stage11_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        for _ in range_(sys.maxsize):
                            for subtile in range(FRONT_SUBTILES_PER_SKIP):
                                stage1_input = stage1_halo_l2l1[index][subtile].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage1_output = stage1_out_l1l2[index][subtile].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                                convnext16_tile(stage1_input, stage1_param, stage1_output)
                                stage1_halo_l2l1[index][subtile].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage1_out_l1l2[index][subtile].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                            for subtile in range(FRONT_SUBTILES_PER_SKIP):
                                stage2_input = stage2_halo_l2l1[index][subtile].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage2_output = stage2_out_l1l2[index][subtile].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                                convnext16_tile(stage2_input, stage2_param, stage2_output)
                                stage2_halo_l2l1[index][subtile].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage2_out_l1l2[index][subtile].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                            stage3_downsource = stage3_downsource_l2l1[index].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                            stage3_output = stage3_out_l1l2[index].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                            down_fnb32_tile(stage3_downsource, stage3_param, stage3_output)
                            stage3_downsource_l2l1[index].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                            stage3_out_l1l2[index].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                            stage5_inputs = [stage5_halo_l2l1[index][chunk].acquire(ObjectFifoPort.Consume, 1) for chunk in range(C8_CHUNKS)]  # noqa: F405
                            stage5_outputs = [stage5_out_l1l2[index][chunk].acquire(ObjectFifoPort.Produce, 1) for chunk in range(C8_CHUNKS)]  # noqa: F405
                            fnb32_c8group_tile(
                                stage5_inputs[0],
                                stage5_inputs[1],
                                stage5_inputs[2],
                                stage5_inputs[3],
                                stage5_param,
                                stage5_outputs[0],
                                stage5_outputs[1],
                                stage5_outputs[2],
                                stage5_outputs[3],
                            )
                            for chunk in range(C8_CHUNKS):
                                stage5_halo_l2l1[index][chunk].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage5_out_l1l2[index][chunk].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                            stage8_output = stage8_fanout_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                            skipb = skipb_l2l1[index].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                            stage9_halos = [stage9_halo_l1l2[index][quadrant].acquire(ObjectFifoPort.Produce, 1) for quadrant in range(STAGE10_QUADRANTS)]  # noqa: F405
                            ct2d32_halo4_tile(stage8_output, skipb, stage9_param, stage9_halos[0], stage9_halos[1], stage9_halos[2], stage9_halos[3])
                            stage8_fanout_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                            skipb_l2l1[index].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                            for quadrant in range(STAGE10_QUADRANTS):
                                stage9_halo_l1l2[index][quadrant].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                            stage11_halos = [stage11_halo_l2l1[index][chunk].acquire(ObjectFifoPort.Consume, 1) for chunk in range(C8_CHUNKS)]  # noqa: F405
                            skipa = skipa_l2l1[index].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                            stage11_output = stage11_out_l1l2[index].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                            stage11_join(stage11_halos[0], stage11_halos[1], stage11_halos[2], stage11_halos[3], skipa, stage11_param, stage11_output)
                            for chunk in range(C8_CHUNKS):
                                stage11_halo_l2l1[index][chunk].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                            skipa_l2l1[index].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                            stage11_out_l1l2[index].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                for index in range(STAGE5_QUADRANTS):
                    make_stage5_stage9_stage11_core(index)

                @core(stage5_bridge_stage10_stage12_lower_tile, stack_size=0x5000)  # noqa: F405
                def stage5_bridge_stage10_stage12_lower_core():
                    stage10_param = stage10_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                    stage12_param = stage12_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                    for _ in range_(sys.maxsize):
                        stage5_package = stage5_package_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        skipb_package = skipb_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage6_downsource = stage6_downsource_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage5_bridge(stage5_package, skipb_package, stage6_downsource)
                        stage5_package_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        skipb_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage6_downsource_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        for quadrant in range(STAGE10_QUADRANTS):
                            stage10_inputs = [stage10_halo_l2l1[quadrant][chunk].acquire(ObjectFifoPort.Consume, 1) for chunk in range(STAGE9_CHUNKS)]  # noqa: F405
                            stage10_outputs = [stage10_out_l1l2[quadrant][chunk].acquire(ObjectFifoPort.Produce, 1) for chunk in range(STAGE9_CHUNKS)]  # noqa: F405
                            fnb32_c8group_tile(
                                stage10_inputs[0],
                                stage10_inputs[1],
                                stage10_inputs[2],
                                stage10_inputs[3],
                                stage10_param,
                                stage10_outputs[0],
                                stage10_outputs[1],
                                stage10_outputs[2],
                                stage10_outputs[3],
                            )
                            for chunk in range(STAGE9_CHUNKS):
                                stage10_halo_l2l1[quadrant][chunk].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage10_out_l1l2[quadrant][chunk].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        for quadrant in range(2, 4):
                            for subtile in range(STAGE12_SUBTILES):
                                stage12_input = stage12_halo_l2l1[quadrant][subtile].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage12_output = stage12_out_l1l2[quadrant][subtile].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                                stage12_convnext(stage12_input, stage12_param, stage12_output)
                                stage12_halo_l2l1[quadrant][subtile].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage12_out_l1l2[quadrant][subtile].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                @core(stage6_bridge11_tile, stack_size=0x5000)  # noqa: F405
                def stage6_bridge11_core():
                    stage6_param = stage6_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                    for _ in range_(sys.maxsize):
                        stage6_downsource = stage6_downsource_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage6_output = stage6_out_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        down_fnb64_tile(stage6_downsource, stage6_param, stage6_output)
                        stage6_downsource_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage6_out_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        for quadrant in range(STAGE11_QUADRANTS):
                            stage11_elem = stage11_out_l2l1[quadrant].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                            halo_pkg = stage12_halo_pkg_l1l2[quadrant].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                            stage11_to_stage12_bridge(stage11_elem, halo_pkg)
                            stage11_out_l2l1[quadrant].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                            stage12_halo_pkg_l1l2[quadrant].release(ObjectFifoPort.Produce, 1)  # noqa: F405

                @core(stage12_upper_tile, stack_size=0x4000)  # noqa: F405
                def stage12_upper_core():
                    stage12_param = stage12_param_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                    for _ in range_(sys.maxsize):
                        stage6_output = stage6_out_l2l1.acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage8_input = stage8_in_l1l2.acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                        stage6_to_stage8_bridge(stage6_output, stage8_input)
                        stage6_out_l2l1.release(ObjectFifoPort.Consume, 1)  # noqa: F405
                        stage8_in_l1l2.release(ObjectFifoPort.Produce, 1)  # noqa: F405

                        for quadrant in range(0, 2):
                            for subtile in range(STAGE12_SUBTILES):
                                stage12_input = stage12_halo_l2l1[quadrant][subtile].acquire(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage12_output = stage12_out_l1l2[quadrant][subtile].acquire(ObjectFifoPort.Produce, 1)  # noqa: F405
                                stage12_convnext(stage12_input, stage12_param, stage12_output)
                                stage12_halo_l2l1[quadrant][subtile].release(ObjectFifoPort.Consume, 1)  # noqa: F405
                                stage12_out_l1l2[quadrant][subtile].release(ObjectFifoPort.Produce, 1)  # noqa: F405


                return {
                    "first_package_index": first_package_index,
                    "package_count": package_count,
                    "stage0_package_l3l2": stage0_package_l3l2,
                    "stage1_param_l3l2": stage1_param_l3l2,
                    "stage2_param_l3l2": stage2_param_l3l2,
                    "stage3_param_l3l2": stage3_param_l3l2,
                    "stage5_param_l3l2": stage5_param_l3l2,
                    "stage6_param_l3l2": stage6_param_l3l2,
                    "stage8_param_l3l2": stage8_param_l3l2,
                    "stage9_param_l3l2": stage9_param_l3l2,
                    "stage10_param_l3l2": stage10_param_l3l2,
                    "stage11_param_l3l2": stage11_param_l3l2,
                    "stage12_param_l3l2": stage12_param_l3l2,
                    "stage12_out_l2l3": stage12_out_l2l3,
                }

            lane_units = [build_lane(lane_index) for lane_index in range(FRAME_LANE_GROUPS)]

            @runtime_sequence(param_arena_ty, sym_name="load_params")  # noqa: F405
            def load_params(param_arena):
                tasks = []
                for lane in lane_units:
                    tasks.extend([
                        shim_dma_single_bd_task(lane["stage1_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE1_OFFSET, STAGE1_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage2_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE2_OFFSET, STAGE2_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage3_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE3_OFFSET, STAGE3_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage5_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE5_OFFSET, STAGE5_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage6_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE6_OFFSET, STAGE6_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage8_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE8_OFFSET, STAGE8_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage9_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE9_OFFSET, STAGE9_CHUNKS * STAGE9_PARAM_CHUNK_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage10_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE10_OFFSET, STAGE10_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage11_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE11_OFFSET, STAGE11_PARAM_BYTES), issue_token=True),  # noqa: F405
                        shim_dma_single_bd_task(lane["stage12_param_l3l2"], param_arena, tap=_byte_tap(PARAM_ARENA_BYTES, PARAM_STAGE12_OFFSET, STAGE12_PARAM_BYTES), issue_token=True),  # noqa: F405
                    ])
                dma_start_task(*tasks)  # noqa: F405
                dma_await_task(*tasks)  # noqa: F405

            @runtime_sequence(param_arena_ty, frame_ingress_ty, frame_egress_ty)  # noqa: F405
            def sequence(param_arena, ingress_frame, egress_frame):
                # The parameter arena argument preserves the stable XRT ABI.
                # Product hot execution must not schedule parameter DMA here.
                #
                # The host-visible work item is one complete frame slot. Each
                # two-column macro-lane group owns a contiguous lane-major package
                # stripe inside that slot. All lane groups start together and the
                # sequence waits once at the frame boundary.
                tasks = []
                for lane in lane_units:
                    first_package_index = lane["first_package_index"]
                    package_count = lane["package_count"]
                    ingress_offset = first_package_index * STAGE0_INPUT_PACKAGE_BYTES
                    egress_offset = first_package_index * STAGE12_OUTPUT_PACKAGE_BYTES
                    tasks.append(
                        shim_dma_single_bd_task(  # noqa: F405
                            lane["stage0_package_l3l2"],
                            ingress_frame,
                            tap=_byte_tap(FRAME_INGRESS_BYTES, ingress_offset, package_count * STAGE0_INPUT_PACKAGE_BYTES),
                            issue_token=True,
                        )
                    )
                    for quadrant in range(STAGE11_QUADRANTS):
                        for subtile in range(STAGE12_SUBTILES):
                            output_index = quadrant * STAGE12_SUBTILES + subtile
                            tasks.append(
                                shim_dma_single_bd_task(  # noqa: F405
                                    lane["stage12_out_l2l3"][quadrant][subtile],
                                    egress_frame,
                                    tap=_strided_byte_tap(
                                        FRAME_EGRESS_BYTES,
                                        egress_offset + output_index * STAGE12_OUTPUT_TILE_BYTES,
                                        package_count,
                                        STAGE12_OUTPUT_PACKAGE_BYTES,
                                        STAGE12_OUTPUT_TILE_BYTES,
                                    ),
                                    issue_token=True,
                                )
                            )
                dma_start_task(*tasks)  # noqa: F405
                dma_await_task(*tasks)  # noqa: F405

    verified = ctx.module.operation.verify()
    if verified is True:
        print(ctx.module)
    else:
        print(verified)
        raise RuntimeError("generated APUSR stage5->stage12 macro-lane MLIR did not verify")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="npu2", choices=["npu", "npu1", "npu2"])
    parser.add_argument("--model-resolution", default="2160", choices=sorted(MODEL_CAPACITY_OUTPUTS))
    parser.add_argument("--lane-groups", type=int, default=4)
    args = parser.parse_args()
    build_kernel(args.device, args.model_resolution, args.lane_groups)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
