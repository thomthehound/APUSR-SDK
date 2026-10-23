#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Canonical spatial middle-core plan for the full-middle-core rewrite.

This module is deliberately data-oriented. The AIE graph generators,
resource-budget checker, and docs should agree on one profile contract instead of
copying shape numbers by hand.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from typing import Any, Literal


ProfileName = Literal["spatial2col", "spatial4col"]
DeviceName = Literal["npu1", "npu2"]
ResolutionName = Literal["1080", "2160", "4320"]


@dataclass(frozen=True)
class CellShape:
    name: str
    width: int
    rows: int
    channels: int
    bytes_per_sample: int = 1
    description: str = ""

    @property
    def element_bytes(self) -> int:
        return self.width * self.rows * self.channels * self.bytes_per_sample

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload["element_bytes"] = self.element_bytes
        return payload


@dataclass(frozen=True)
class ObjectFifoPlan:
    name: str
    producer: str
    consumer: str
    owner: str
    owner_type: Literal["compute_tile", "memory_tile", "shim"]
    cell_shape: str
    depth: int
    hot: bool
    purpose: str
    halo_policy: str | None = None

    def to_json(self, cell_shapes: dict[str, CellShape]) -> dict[str, Any]:
        payload = asdict(self)
        payload["element_bytes"] = cell_shapes[self.cell_shape].element_bytes
        return payload


@dataclass(frozen=True)
class ComputeTilePlan:
    name: str
    role: str
    row: int
    column: int
    static_reserved_bytes: int
    linked_kernel_count: int
    stage_group: str


@dataclass(frozen=True)
class MemoryTilePlan:
    name: str
    role: str
    column: int
    static_reserved_bytes: int
    arena_policy: str


@dataclass(frozen=True)
class CapacityClass:
    name: ResolutionName
    model_output_width: int
    model_output_rows: int


@dataclass(frozen=True)
class LaneStripePlan:
    lane_index: int
    first_cell_column: int
    cell_columns: int
    cell_rows: int

    @property
    def cell_count(self) -> int:
        return self.cell_columns * self.cell_rows

    @property
    def first_cell_index(self) -> int:
        return self.first_cell_column * self.cell_rows

    @property
    def frame_slot_first_byte(self) -> int:
        return self.first_cell_column * TRANSPORT_CELL_WIDTH * TRANSPORT_PIXEL_BYTES

    @property
    def semantic_bytes(self) -> int:
        return self.cell_count * TRANSPORT_CELL_BYTES

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload.update(
            {
                "worker_index": self.lane_index,
                "cell_count": self.cell_count,
                "first_cell_index": self.first_cell_index,
                "frame_slot_first_byte": self.frame_slot_first_byte,
                "semantic_bytes": self.semantic_bytes,
            }
        )
        return payload


@dataclass(frozen=True)
class FramePlan:
    profile: ProfileName
    device: DeviceName
    capacity: ResolutionName
    lane_groups: int
    target_columns: int
    macro_lane_columns: int
    model_output_width: int
    model_output_rows: int
    seam_width: int
    seam_rows: int
    cell_columns: int
    cell_rows: int
    frame_slot_bytes: int
    front_ingress_slot_bytes: int
    lane_stripes: tuple[LaneStripePlan, ...]

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload["worker_groups"] = self.lane_groups
        payload["worker_columns"] = self.macro_lane_columns
        payload["physical_columns_used"] = self.lane_groups * self.macro_lane_columns
        payload["auto_worker_policy"] = "auto means as many independent worker groups as fit: target_columns // worker_columns"
        payload["transport_layout"] = TRANSPORT_LAYOUT
        payload["front_ingress_layout"] = "row_major_c16_frame_with_one_pixel_zero_guard_border"
        payload["worker_stripes"] = [stripe.to_json() for stripe in self.lane_stripes]
        payload["lane_stripes"] = payload["worker_stripes"]
        return payload



CELL_SHAPES: dict[str, CellShape] = {
    "c16_cell": CellShape(
        "c16_cell",
        width=16,
        rows=8,
        channels=16,
        description="Owned C16 activation cell; normal ingress, egress, and SkipA unit.",
    ),
    "c16_halo_cell": CellShape(
        "c16_halo_cell",
        width=18,
        rows=10,
        channels=16,
        description="C16 3x3 consumer view: one-pixel spatial border around a 16x8 owned cell.",
    ),
    "c16_row_seam": CellShape(
        "c16_row_seam",
        width=1,
        rows=8,
        channels=16,
        description="One vertical C16 edge column from a 16x8 owned cell for left/right neighbor seam exchange.",
    ),
    "c32_cell": CellShape(
        "c32_cell",
        width=16,
        rows=8,
        channels=32,
        description="Owned C32 activation cell after the first downsample/encoder transition.",
    ),
    "c32_halo_cell": CellShape(
        "c32_halo_cell",
        width=18,
        rows=10,
        channels=32,
        description="C32 3x3 consumer view for spatial mixing without package-sized halos.",
    ),
    "c64_cell": CellShape(
        "c64_cell",
        width=16,
        rows=8,
        channels=64,
        description="Owned C64 bottleneck cell.",
    ),
    "c64_halo_cell": CellShape(
        "c64_halo_cell",
        width=18,
        rows=10,
        channels=64,
        description="C64 3x3 consumer view; large enough that FIFO depth must stay one.",
    ),
    "c8_quadrant_cell": CellShape(
        "c8_quadrant_cell",
        width=16,
        rows=8,
        channels=8,
        description="One grouped C8 decoder quadrant emitted by the CT2D/up path.",
    ),
    "c8_halo_chunk": CellShape(
        "c8_halo_chunk",
        width=18,
        rows=10,
        channels=8,
        description="One C8 halo chunk consumed by the decoder without NHWC package repack.",
    ),
}


CAPACITY_CLASSES: dict[ResolutionName, CapacityClass] = {
    "1080": CapacityClass("1080", model_output_width=1920, model_output_rows=1080),
    "2160": CapacityClass("2160", model_output_width=3840, model_output_rows=2160),
    "4320": CapacityClass("4320", model_output_width=7680, model_output_rows=4320),
}


TRANSPORT_LAYOUT = "row_major_c16_frame"
TRANSPORT_CELL_WIDTH = CELL_SHAPES["c16_cell"].width
TRANSPORT_CELL_ROWS = CELL_SHAPES["c16_cell"].rows
# Internal capacity rows are padded through both k2/s2 downsample boundaries.
# That gives integer C16->C32 and C32->C64 cell ladders for every capacity
# class instead of special-casing 2160p test fixtures.
DOWNSAMPLE_LADDER_CELL_ROW_ALIGNMENT = TRANSPORT_CELL_ROWS * 4
TRANSPORT_CELL_BYTES = CELL_SHAPES["c16_cell"].element_bytes
TRANSPORT_PIXEL_BYTES = CELL_SHAPES["c16_cell"].channels * CELL_SHAPES["c16_cell"].bytes_per_sample
GUARD_PIXELS = 1
C16_ROW_SEAM_BYTES = CELL_SHAPES["c16_row_seam"].element_bytes
RESIDENT_PARAMETER_ARENA_BYTES = 122_880
FRONT_C16_PARAMETER_SLICE_BYTES = 4_048
STAGE1_HOT_RECORD_BYTES = 5_776
ENCODER_C32_PARAMETER_SLICE_BYTES = 2_576
FRONT_C16_RING_ROWS = 3
FRONT_C16_SEAM_RING_ROWS = FRONT_C16_RING_ROWS
FRONT_C16_WINDOW_CELLS = 7
FRONT_C16_OUTPUT_CHUNK_CELLS = 5
FRONT_C16_WINDOW_STEP_CELLS = 4
MEMORY_TILE_BYTES = 512 * 1024
# Legacy package dimensions are retained for the quarantined graph and old
# diagnostic layouts. The product middle-core slot is not package-major.
LEGACY_MACRO_PACKAGE_WIDTH = 64
LEGACY_MACRO_PACKAGE_ROWS = 32


HARDWARE_FEATURES: dict[str, Any] = {
    "tile_dma_4d_tensor_address_generation": True,
    "memory_tile_dma_5d_tensor_address_generation": True,
    "memory_tile_mm2s_zero_padding": True,
    "memory_tile_neighbor_east_west_dma_channels_0_3": True,
    "neighbor_compute_tile_shared_memory": True,
    "semaphore_locks_for_dma_and_neighbor_exchange": True,
    "notes": [
        "Use shim/memory-tile tensor address generation to gather C16 halo patches directly from the row-major frame.",
        "Use the GPU-written one-pixel zero guard border for front frame-edge halo values.",
        "Use tensor address generation to gather compact halo patches from the zero-guarded row-major C16 ingress frame.",
    ],
}


LIMITS: dict[str, int] = {
    "compute_tile_data_memory_bytes": 64 * 1024,
    "compute_tile_program_memory_bytes": 16 * 1024,
    "memory_tile_data_memory_bytes": 512 * 1024,
    "max_hot_fifo_element_bytes": 16 * 1024,
    "preferred_hot_fifo_element_bytes": 4 * 1024,
    "large_fifo_element_bytes": 8 * 1024,
    "max_large_fifo_depth": 1,
    "max_compute_tile_reserved_bytes": 48 * 1024,
    "max_linked_kernel_count_per_compute_tile": 2,
}


BASE_FIFOS: tuple[ObjectFifoPlan, ...] = (
    ObjectFifoPlan(
        name="front_c16_halo_patch",
        producer="shim_or_memtile_tensor_gather",
        consumer="front_c16",
        owner="mem_ingress",
        owner_type="memory_tile",
        cell_shape="c16_halo_cell",
        depth=1,
        hot=True,
        purpose="DMA-gathered 18x10xC16 front patch; left/right/top/bottom halo is part of the tensor access view, not neighbor seam traffic.",
        halo_policy="dma_tensor_halo_patch",
    ),
    ObjectFifoPlan(
        name="skipa_c16_delay_cell",
        producer="front_c16",
        consumer="decoder_c16",
        owner="mem_skipa",
        owner_type="memory_tile",
        cell_shape="c16_cell",
        depth=2,
        hot=True,
        purpose="SkipA delay ring; kept in memory-tile arena instead of re-streaming full packages.",
    ),
    ObjectFifoPlan(
        name="c32_cell",
        producer="down_c32",
        consumer="mid_c32",
        owner="mid_c32",
        owner_type="compute_tile",
        cell_shape="c32_cell",
        depth=2,
        hot=True,
        purpose="C32 encoder cell handoff.",
    ),
    ObjectFifoPlan(
        name="c32_consumer_halo_view",
        producer="mid_c32",
        consumer="mid_c32",
        owner="mid_c32",
        owner_type="compute_tile",
        cell_shape="c32_halo_cell",
        depth=1,
        hot=True,
        purpose="C32 spatial-mix window formed at the consumer.",
        halo_policy="interior_neighbor_or_true_frame_edge",
    ),
    ObjectFifoPlan(
        name="skipb_c32_delay_cell",
        producer="mid_c32",
        consumer="bottleneck_c64",
        owner="mem_skipb",
        owner_type="memory_tile",
        cell_shape="c32_cell",
        depth=2,
        hot=True,
        purpose="SkipB delay ring in memory-tile arena.",
    ),
    ObjectFifoPlan(
        name="c64_cell",
        producer="down_c64",
        consumer="bottleneck_c64",
        owner="bottleneck_c64",
        owner_type="compute_tile",
        cell_shape="c64_cell",
        depth=1,
        hot=True,
        purpose="C64 bottleneck cell. Depth stays one to protect local memory.",
    ),
    ObjectFifoPlan(
        name="c64_consumer_halo_view",
        producer="bottleneck_c64",
        consumer="up_c32",
        owner="bottleneck_c64",
        owner_type="compute_tile",
        cell_shape="c64_halo_cell",
        depth=1,
        hot=True,
        purpose="C64 window consumed by the up path without package materialization.",
        halo_policy="interior_neighbor_or_true_frame_edge",
    ),
    ObjectFifoPlan(
        name="ct2d_c8_halo_chunk",
        producer="up_c32",
        consumer="decoder_c16",
        owner="decoder_c16",
        owner_type="compute_tile",
        cell_shape="c8_halo_chunk",
        depth=2,
        hot=True,
        purpose="C8 decoder quadrant halo handoff; no full NHWC package repack.",
        halo_policy="ct2d_direct_quadrant_halo",
    ),
    ObjectFifoPlan(
        name="egress_c16_cell",
        producer="decoder_c16",
        consumer="shim_egress",
        owner="mem_egress",
        owner_type="memory_tile",
        cell_shape="c16_cell",
        depth=2,
        hot=True,
        purpose="Output cells leave the spatial lane through the frame-slot egress ring.",
    ),
)


STAGE_GROUPS: tuple[dict[str, Any], ...] = (
    {
        "name": "front_c16",
        "model_role": "front C16 halo-patch tensor engine and SkipA producer",
        "channels": 16,
        "row_role": "C16 front encoder and SkipA producer",
        "state_policy": "one dense 18x10xC16 halo patch feeds each front cell; no row ring and no seam ObjectFifos",
    },
    {
        "name": "encoder_c32",
        "model_role": "C32 encoder and SkipB producer",
        "channels": 32,
        "row_role": "C32 encoder, FasterNet32, and SkipB producer",
        "state_policy": "C32 paired macroblock and local spatial-mix window",
    },
    {
        "name": "bottleneck_c64",
        "model_role": "C64 bottleneck and CT2D C8 fanout",
        "channels": 64,
        "row_role": "C64 bottleneck and CT2D C8 fanout",
        "state_policy": "C64 cells stay depth-one; CT2D emits grouped C8 chunks",
    },
    {
        "name": "decoder_c16",
        "model_role": "C16 decoder, SkipA join, and output pack",
        "channels": 16,
        "row_role": "decoder, SkipA join, and output pack",
        "state_policy": "C8 quadrant groups join locally with delayed C16 SkipA cells",
    },
)


HALO_POLICIES: tuple[dict[str, Any], ...] = (
    {
        "name": "dma_tensor_halo_patch",
        "materialization": "shim_or_memory_tile_tensor_access_view",
        "uses": [
            "tile_dma_4d_tensor_address_generation",
            "memory_tile_dma_5d_tensor_address_generation",
        ],
        "description": "The front stage consumes dense 18x10xC16 patches gathered by tensor-addressed DMA from the C16 frame. Interior halo pixels come from the frame itself; true frame edges are supplied by the zero-guarded C16 ingress frame path.",
    },

    {
        "name": "interior_neighbor_or_true_frame_edge",
        "materialization": "consumer_side_view",
        "uses": ["neighbor_compute_tile_shared_memory", "semaphore_locks_for_dma_and_neighbor_exchange"],
        "description": "Later C32/C64 spatial windows use local/neighbor consumer views; true frame-edge zeros are a boundary condition only.",
    },
    {
        "name": "ct2d_direct_quadrant_halo",
        "materialization": "producer_emits_micro_halo_chunk",
        "uses": ["tile_dma_4d_tensor_address_generation"],
        "description": "CT2D emits C8 quadrant halo chunks directly to the decoder path, avoiding a full NHWC package repack.",
    },
)


LINE_RING_POLICIES: tuple[dict[str, Any], ...] = (
    {
        "name": "c16_front_halo_patch_tap",
        "cell_shape": "c16_halo_cell",
        "ring_rows": 1,
        "owner": "shim_or_memtile_tensor_gather",
        "purpose": "C16 front spatial windows use tensor-addressed halo patch gathers, not row rings or seam exchange.",
    },
    {
        "name": "c32_three_line_ring",
        "cell_shape": "c32_cell",
        "ring_rows": 3,
        "owner": "mem_skipb",
        "purpose": "C32 spatial mix and SkipB delay use row rings.",
    },
    {
        "name": "c64_depth_one_ring",
        "cell_shape": "c64_cell",
        "ring_rows": 1,
        "owner": "compute_or_transition_memtile",
        "purpose": "C64 bottleneck cells stay depth-one unless compile evidence justifies deeper buffering.",
    },
)


def _align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def _ceil_half(value: int) -> int:
    return (value + 1) // 2


def _macro_lane_columns(profile: ProfileName) -> int:
    return 2 if profile == "spatial2col" else 4


def _target_columns(device: DeviceName) -> int:
    return 4 if device == "npu1" else 8


def resolve_worker_groups(profile: ProfileName, device: DeviceName, worker_groups: str | int) -> int:
    worker_columns = _macro_lane_columns(profile)
    target_columns = _target_columns(device)
    if worker_groups == "auto":
        return target_columns // worker_columns
    value = int(worker_groups)
    if value <= 0:
        raise ValueError("worker_groups must be positive")
    required_columns = value * worker_columns
    if required_columns > target_columns:
        raise ValueError(
            f"{profile} with {value} worker group(s) requires {required_columns} columns, but {device} exposes {target_columns}"
        )
    return value


def resolve_lane_groups(profile: ProfileName, device: DeviceName, lane_groups: str | int) -> int:
    return resolve_worker_groups(profile, device, lane_groups)


def _split_cell_columns(cell_columns: int, lane_groups: int) -> list[tuple[int, int]]:
    if lane_groups > cell_columns:
        raise ValueError("lane_groups cannot exceed cell columns for the selected capacity")
    base = cell_columns // lane_groups
    extra = cell_columns % lane_groups
    spans: list[tuple[int, int]] = []
    cursor = 0
    for lane_index in range(lane_groups):
        count = base + (1 if lane_index < extra else 0)
        spans.append((cursor, count))
        cursor += count
    return spans


def _split_physical_columns(cell_columns: int, physical_columns: int) -> list[tuple[int, int]]:
    if physical_columns <= 0:
        raise ValueError("physical_columns must be positive")
    base = cell_columns // physical_columns
    extra = cell_columns % physical_columns
    spans: list[tuple[int, int]] = []
    cursor = 0
    for index in range(physical_columns):
        width = base + (1 if index < extra else 0)
        spans.append((cursor, width))
        cursor += width
    return spans


def _max_physical_column_cell_columns(frame: FramePlan) -> int:
    max_columns = 0
    for stripe in frame.lane_stripes:
        for _local_first, local_columns in _split_physical_columns(stripe.cell_columns, frame.macro_lane_columns):
            max_columns = max(max_columns, local_columns)
    return max_columns


def _row_ring_budgets(profile: ProfileName) -> dict[str, Any]:
    budgets = []
    halo_bytes = CELL_SHAPES["c16_halo_cell"].element_bytes
    for device in ("npu1", "npu2"):
        for capacity in ("1080", "2160", "4320"):
            frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups="auto")
            max_cell_columns = _max_physical_column_cell_columns(frame)
            budgets.append(
                {
                    "device": device,
                    "capacity": capacity,
                    "lane_groups": frame.lane_groups,
                    "worker_groups": frame.lane_groups,
                    "macro_lane_columns": frame.macro_lane_columns,
                    "worker_columns": frame.macro_lane_columns,
                    "physical_columns": frame.lane_groups * frame.macro_lane_columns,
                    "physical_columns_used": frame.lane_groups * frame.macro_lane_columns,
                    "cell_rows": frame.cell_rows,
                    "max_physical_column_cell_columns": max_cell_columns,
                    "c16_halo_patch_bytes": halo_bytes,
                    "front_c16_active_fifo_bytes": halo_bytes,
                    "front_c16_patch_read_overhead_ratio": round(halo_bytes / TRANSPORT_CELL_BYTES, 6),
                    "fits_one_compute_tile_fifo": halo_bytes + FRONT_C16_PARAMETER_SLICE_BYTES < LIMITS["compute_tile_data_memory_bytes"],
                }
            )
    return {
        "schema_version": 2,
        "owner": "front_c16_halo_patch_engine",
        "storage": "dma_tensor_halo_patch_stream",
        "cell_shape": "c16_halo_cell",
        "ring_rows": 0,
        "capacity_policy": "The front worker sees one compact 18x10xC16 patch per output cell. Full shard rows and seam objects are not materialized.",
        "budgets": budgets,
    }


def _front_c16_row_engine_contract(profile: ProfileName) -> dict[str, Any]:
    return {
        "schema_version": 2,
        "name": "front_c16_halo_patch_engine",
        "status": "active_spatial_core_contract",
        "cell_shape": "c16_halo_cell",
        "compile_carrier": "dma_tensor_halo_patch",
        "owned_cell_bytes": TRANSPORT_CELL_BYTES,
        "halo_patch_bytes": CELL_SHAPES["c16_halo_cell"].element_bytes,
        "front_patch_source": "zero_guarded_row_major_c16_frame_tensor_tap",
        "front_patch_fifo_objects": 1,
        "front_parameter_slice_bytes": FRONT_C16_PARAMETER_SLICE_BYTES,
        "front_parameter_source": "tile_local_resident_buffer_loaded_at_model_epoch",
        "encoder_parameter_slice_bytes": ENCODER_C32_PARAMETER_SLICE_BYTES,
        "encoder_parameter_source": "tile_local_resident_buffer_loaded_at_model_epoch",
        "hot_path_parameter_streams": 0,
        "parameterized_math_status": "fused_vector_srs_c16_spatial3x3_expand_primitive_active",
        "owned_cell_policy": "the compute unit remains one 16x8 C16 cell, but the input object is the exact 18x10 C16 consumer view",
        "frame_edge_policy": "true frame edges are supplied by the zero-guarded C16 ingress frame path",
        "interior_edge_policy": "interior halos come from the row-major frame tensor tap; no interior zero-padding and no explicit seam exchange",
        "host_tap_policy": "bounded row-block tensor DMA tasks stream patch-major front inputs from a zero-guarded row-major C16 frame",
        "required_neighbor_paths": [],
        "seam_exchange": {
            "schema_version": 2,
            "status": "removed_from_active_spatial_core",
            "shape": "none",
            "seam_bytes": 0,
            "ring_rows": 0,
            "staging": "none",
            "consumer_input_dma_channels": 1,
            "order": "not_applicable",
            "shim_streams_added": 0,
            "full_row_fifos": 0,
            "per_cell_dma_tasks": 0,
        },
        "macro_lane_columns": _macro_lane_columns(profile),
        "front_to_encoder_path": "spatial_core_tile_local_front_params_feed_vector_srs_c16_halo_patch_to_c32_cells_then_cell_granular_c32_to_c64_encoder_row",
    }


def build_frame_plan(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
) -> FramePlan:
    capacity = CAPACITY_CLASSES[model_resolution]
    macro_lane_columns = _macro_lane_columns(profile)
    target_columns = _target_columns(device)
    resolved_lane_groups = resolve_lane_groups(profile, device, lane_groups)

    seam_width = _align_up(_ceil_half(capacity.model_output_width), TRANSPORT_CELL_WIDTH)
    seam_rows = _align_up(_ceil_half(capacity.model_output_rows), DOWNSAMPLE_LADDER_CELL_ROW_ALIGNMENT)
    cell_columns = seam_width // TRANSPORT_CELL_WIDTH
    cell_rows = seam_rows // TRANSPORT_CELL_ROWS
    frame_slot_bytes = seam_width * seam_rows * TRANSPORT_PIXEL_BYTES
    front_ingress_slot_bytes = (seam_width + 2 * GUARD_PIXELS) * (seam_rows + 2 * GUARD_PIXELS) * TRANSPORT_PIXEL_BYTES

    stripes = []
    for lane_index, (first_cell_column, cell_column_count) in enumerate(_split_cell_columns(cell_columns, resolved_lane_groups)):
        stripes.append(
            LaneStripePlan(
                lane_index=lane_index,
                first_cell_column=first_cell_column,
                cell_columns=cell_column_count,
                cell_rows=cell_rows,
            )
        )

    return FramePlan(
        profile=profile,
        device=device,
        capacity=model_resolution,
        lane_groups=resolved_lane_groups,
        target_columns=target_columns,
        macro_lane_columns=macro_lane_columns,
        model_output_width=seam_width * 2,
        model_output_rows=seam_rows * 2,
        seam_width=seam_width,
        seam_rows=seam_rows,
        cell_columns=cell_columns,
        cell_rows=cell_rows,
        frame_slot_bytes=frame_slot_bytes,
        front_ingress_slot_bytes=front_ingress_slot_bytes,
        lane_stripes=tuple(stripes),
    )


def build_capacity_class_summaries() -> list[dict[str, Any]]:
    summaries = []
    for name in ("1080", "2160", "4320"):
        frame = build_frame_plan("spatial2col", model_resolution=name, lane_groups=1)
        summaries.append(
            {
                "name": name,
                "model_output_width": frame.model_output_width,
                "model_output_rows": frame.model_output_rows,
                "seam_width": frame.seam_width,
                "seam_rows": frame.seam_rows,
                "cell_columns": frame.cell_columns,
                "cell_rows": frame.cell_rows,
                "frame_slot_bytes": frame.frame_slot_bytes,
                "front_ingress_slot_bytes": frame.front_ingress_slot_bytes,
            }
        )
    return summaries


def lane_stripe_for_cell(frame: FramePlan, cell_column: int) -> LaneStripePlan:
    for stripe in frame.lane_stripes:
        if cell_column >= stripe.first_cell_column and cell_column < stripe.first_cell_column + stripe.cell_columns:
            return stripe
    raise ValueError("cell_column is outside the frame plan")


def frame_cell_byte_offset(frame: FramePlan, cell_column: int, cell_row: int) -> int:
    """Return the row-major C16 frame byte offset for the owned cell origin."""

    if cell_column < 0 or cell_column >= frame.cell_columns or cell_row < 0 or cell_row >= frame.cell_rows:
        raise ValueError("cell coordinates must be inside the frame plan")
    pixel_x = cell_column * TRANSPORT_CELL_WIDTH
    pixel_y = cell_row * TRANSPORT_CELL_ROWS
    return (pixel_y * frame.seam_width + pixel_x) * TRANSPORT_PIXEL_BYTES


def build_cell_offset_examples(frame: FramePlan) -> list[dict[str, int]]:
    examples = [
        (0, 0),
        (1, 0),
        (0, 1),
        (frame.lane_stripes[1].first_cell_column if len(frame.lane_stripes) > 1 else frame.cell_columns - 1, 0),
        (frame.cell_columns - 1, frame.cell_rows - 1),
    ]
    return [
        {
            "cell_column": col,
            "cell_row": row,
            "byte_offset": frame_cell_byte_offset(frame, col, row),
        }
        for col, row in examples
    ]


def _compute_tiles(profile: ProfileName) -> list[ComputeTilePlan]:
    roles = [
        ("front_c16", 40 * 1024, 2, "front_c16"),
        ("down_c32", 36 * 1024, 1, "encoder_c32"),
        ("mid_c32", 36 * 1024, 1, "encoder_c32"),
        ("down_c64", 36 * 1024, 1, "bottleneck_c64"),
        ("bottleneck_c64", 36 * 1024, 1, "bottleneck_c64"),
        ("up_c32", 36 * 1024, 1, "decoder_c16"),
        ("decoder_c16", 40 * 1024, 2, "decoder_c16"),
        ("egress_pack", 36 * 1024, 1, "decoder_c16"),
    ]
    lane_columns = _macro_lane_columns(profile)
    slices = 1 if lane_columns == 2 else 2
    tiles: list[ComputeTilePlan] = []
    for slice_index in range(slices):
        column_base = slice_index * 2
        for local_index, (role, reserved, linked, stage_group) in enumerate(roles):
            row = local_index // 2
            column = column_base + (local_index % 2)
            suffix = local_index + slice_index * len(roles)
            tiles.append(
                ComputeTilePlan(
                    name=f"{role}_{suffix}",
                    role=role,
                    row=row,
                    column=column,
                    static_reserved_bytes=reserved,
                    linked_kernel_count=linked,
                    stage_group=stage_group,
                )
            )
    return tiles


def _memory_tiles(profile: ProfileName) -> list[MemoryTilePlan]:
    if profile == "spatial2col":
        definitions = [
            ("mem0", "ingress_skipa", 0, "Ingress cells and SkipA delay ring share one memory tile in the discipline profile."),
            ("mem1", "skipb_egress", 1, "SkipB delay and egress staging share one memory tile in the discipline profile."),
        ]
    else:
        definitions = [
            ("mem0", "ingress", 0, "Ingress paired macroblock."),
            ("mem1", "skipa", 1, "SkipA delay ring."),
            ("mem2", "skipb", 2, "SkipB delay ring and C32/C64 transition staging."),
            ("mem3", "egress", 3, "Decoder/egress ring."),
        ]
    return [
        MemoryTilePlan(
            name=name,
            role=role,
            column=column,
            static_reserved_bytes=64 * 1024,
            arena_policy=policy,
        )
        for name, role, column, policy in definitions
    ]


def _memory_tile_data_movement_budget(profile: ProfileName) -> list[dict[str, Any]]:
    if profile == "spatial2col":
        return [
            {
                "name": "mem0",
                "active_s2mm_channels": 1,
                "active_mm2s_channels": 2,
                "active_neighbor_dma_channels": 1,
                "locks_reserved": 4,
                "purpose": "Ingress C16 cells and delayed SkipA cells share the column-0 memory tile.",
            },
            {
                "name": "mem1",
                "active_s2mm_channels": 2,
                "active_mm2s_channels": 2,
                "active_neighbor_dma_channels": 1,
                "locks_reserved": 4,
                "purpose": "SkipB staging and C16 egress share the column-1 memory tile.",
            },
        ]

    return [
        {
            "name": "mem0",
            "active_s2mm_channels": 1,
            "active_mm2s_channels": 1,
            "active_neighbor_dma_channels": 1,
            "locks_reserved": 3,
            "purpose": "Ingress C16 cells feed the front C16 stage.",
        },
        {
            "name": "mem1",
            "active_s2mm_channels": 1,
            "active_mm2s_channels": 1,
            "active_neighbor_dma_channels": 2,
            "locks_reserved": 3,
            "purpose": "SkipA delay cells stay beside the decoder path.",
        },
        {
            "name": "mem2",
            "active_s2mm_channels": 1,
            "active_mm2s_channels": 1,
            "active_neighbor_dma_channels": 2,
            "locks_reserved": 3,
            "purpose": "SkipB and C32/C64 transition cells stay beside the bottleneck path.",
        },
        {
            "name": "mem3",
            "active_s2mm_channels": 1,
            "active_mm2s_channels": 1,
            "active_neighbor_dma_channels": 1,
            "locks_reserved": 3,
            "purpose": "Decoder output cells leave through the egress memory tile.",
        },
    ]


def _resource_sharing(profile: ProfileName) -> dict[str, Any]:
    memory_tiles = _memory_tiles(profile)
    compute_tiles = _compute_tiles(profile)
    memory_names = [tile.name for tile in memory_tiles]
    compute_names = [tile.name for tile in compute_tiles]

    return {
        "schema_version": 1,
        "memory_tile_capacity_model": "per_physical_tile_with_neighbor_access",
        "compute_tile_capacity_model": "per_physical_tile_with_neighbor_memory_access",
        "neighbor_access_does_not_increase_physical_tile_capacity": True,
        "memory_tile_arena_groups_are_not_per_column_capacity_multipliers": True,
        "compute_tile_neighbor_memory_is_a_data_movement_path": True,
        "memory_tile_arena_groups": [
            {
                "name": "macro_lane_memory_tiles",
                "tiles": memory_names,
                "max_aggregate_bytes": len(memory_names) * LIMITS["memory_tile_data_memory_bytes"],
                "capacity_policy": "Aggregate planning may span adjacent memory tiles, but each physical memory tile keeps its own hard 512 KiB budget.",
            }
        ],
        "memory_tile_neighbor_links": [
            {
                "name": tile.name,
                "column": tile.column,
                "neighbors": [other.name for other in memory_tiles if abs(other.column - tile.column) == 1],
                "path": "east_west_memory_tile_dma_channels_0_3",
            }
            for tile in memory_tiles
        ],
        "compute_tile_neighbor_groups": [
            {
                "name": "macro_lane_compute_tiles",
                "tiles": compute_names,
                "capacity_policy": "Neighbor memory visibility reduces copies and halo traffic; local data and program memory budgets remain per physical compute tile.",
            }
        ],
    }


def _owner_aliases(profile: ProfileName) -> dict[str, str]:
    if profile == "spatial2col":
        return {
            "mem_ingress": "mem0",
            "mem_skipa": "mem0",
            "mem_skipb": "mem1",
            "mem_egress": "mem1",
            "front_c16": "front_c16_0",
            "mid_c32": "mid_c32_2",
            "bottleneck_c64": "bottleneck_c64_4",
            "decoder_c16": "decoder_c16_6",
        }
    return {
        "mem_ingress": "mem0",
        "mem_skipa": "mem1",
        "mem_skipb": "mem2",
        "mem_egress": "mem3",
        "front_c16": "front_c16_0",
        "mid_c32": "mid_c32_2",
        "bottleneck_c64": "bottleneck_c64_4",
        "decoder_c16": "decoder_c16_6",
    }


def build_resource_budget(profile: ProfileName) -> dict[str, Any]:
    macro_lane_columns = _macro_lane_columns(profile)
    owner_aliases = _owner_aliases(profile)
    object_fifos = []
    for fifo in BASE_FIFOS:
        fifo_payload = fifo.to_json(CELL_SHAPES)
        fifo_payload["owner"] = owner_aliases.get(fifo.owner, fifo.owner)
        object_fifos.append(fifo_payload)

    return {
        "schema_version": 6,
        "macro_lane_columns": macro_lane_columns,
        "compute_tiles_per_macro_lane": 4 * macro_lane_columns,
        "memory_tiles_per_macro_lane": macro_lane_columns,
        "limits": LIMITS,
        "hardware_features": HARDWARE_FEATURES,
        "public_frame_slot": {
            "transport_layout": TRANSPORT_LAYOUT,
            "cell_width": TRANSPORT_CELL_WIDTH,
            "cell_rows": TRANSPORT_CELL_ROWS,
            "cell_bytes": TRANSPORT_CELL_BYTES,
            "lane_order": "placement_only_vertical_stripes",
            "cell_order_within_lane": "row_major_tensor_taps",
            "pixel_order": "row_major_c16_channel_inner",
            "front_ingress_guard_pixels": GUARD_PIXELS,
            "legacy_package_width": LEGACY_MACRO_PACKAGE_WIDTH,
            "legacy_package_rows": LEGACY_MACRO_PACKAGE_ROWS,
        },
        "capacity_classes": build_capacity_class_summaries(),
        "cell_shapes": [shape.to_json() for shape in CELL_SHAPES.values()],
        "halo_policies": list(HALO_POLICIES),
        "line_ring_policies": list(LINE_RING_POLICIES),
        "row_ring_budgets": _row_ring_budgets(profile),
        "front_c16_row_engine": _front_c16_row_engine_contract(profile),
        "front_c16_seam_exchange": {
            "schema_version": 2,
            "status": "removed_from_active_spatial_core",
            "shape": "none",
            "seam_bytes": 0,
            "ring_rows": 0,
            "staging": "none",
            "consumer_input_dma_channels": 1,
            "source": "zero-guarded row-major C16 frame tensor tap",
            "destination": "dense 18x10xC16 halo patch consumed by the front worker",
            "interior_edge_policy": "interior halo pixels are gathered from the source frame; never zero-pad interior edges",
            "frame_edge_policy": "zero-guarded C16 ingress frame handles true frame edges",
            "shim_streams_added": 0,
            "full_row_fifos": 0,
            "per_cell_dma_tasks": 0,
        },
        "stream_constraints": {
            "normal_shim_ingress_streams_per_physical_column": 1,
            "normal_shim_egress_streams_per_physical_column": 1,
            "compute_stage_activation_streams_per_consumer": 2,
            "forbid_shifted_host_frame_taps_for_line_rings": True,
            "memory_tile_owns_row_halo_and_joined_pair_seam_staging": False,
            "tensor_dma_owns_front_halo_patch_gather": True,
        },
        "resource_sharing": _resource_sharing(profile),
        "data_movement_budget": {
            "memory_tiles": _memory_tile_data_movement_budget(profile),
            "limits": {
                "tile_dma_s2mm_channels": 2,
                "tile_dma_mm2s_channels": 2,
                "memory_tile_dma_channels": 12,
                "memory_tile_neighbor_dma_channels": 8,
                "semaphore_locks_per_memory_module": 16,
            },
        },
        "implementation_phases": [
            {
                "name": "spatial_core_stage1_12_spine",
                "status": "current_stage1_2_to_stage3_paired_macroblock_with_skipa",
                "purpose": "Primary spatial-core topology for the Stage1-4 milestone: Stage1 and Stage2 are fused as a paired 36x20 C16 macroblock producer. One shared 34x18 Stage1 patch is reused to produce the four Stage2 C16 cells needed by one Stage3 k2/s2 output cell. Those cells are teed to the long-lived SkipA slice in the combined SkipA/SkipB arena and passed as a compact 2x2 C16 group to split Stage3. Stage4 is split off for the next C32 halo-ring boundary.",
                "product_complete": False,
                "emit_flag": "--emit-graph-phase=spatial-core",
            },
            {
                "name": "front_c16_row_engine",
                "status": "diagnostic",
                "purpose": "Diagnostic row-window front C16 isolation path retained for comparison only. The primary spatial-core now exercises the Stage1-4 carrier; this diagnostic still uses tensor-DMA halo patches instead of row rings or seam exchange.",
                "product_complete": False,
                "emit_flag": "--emit-graph-phase=front-c16-row",
                "parameterized_math_status": "fused_vector_srs_c16_spatial3x3_expand_primitive_active",
            },
            {
                "name": "cell_conveyor_envelope",
                "status": "diagnostic",
                "purpose": "Legacy lane-major C16 cell transport diagnostic retained for build isolation only; not the active spatial-core ABI.",
                "product_complete": False,
                "emit_flag": "--emit-graph-phase=cell-conveyor",
            },
            {
                "name": "c16_halo_envelope",
                "status": "diagnostic",
                "purpose": "Minimal owned-cell to C16-halo object diagnostic retained for build isolation only.",
                "product_complete": False,
                "emit_flag": "--emit-graph-phase=c16-halo",
            },
        ],
        "pipeline_stages": list(STAGE_GROUPS),
        "object_fifos": object_fifos,
        "compute_tiles": [asdict(tile) for tile in _compute_tiles(profile)],
        "memory_tiles": [asdict(tile) for tile in _memory_tiles(profile)],
    }


def build_design_contract(profile: ProfileName) -> dict[str, Any]:
    return {
        "schema_version": 8,
        "name": f"apusr_middle_core_stage1_12_{profile}",
        "purpose": "replacement middle-core design contract for the active spatial-core spine",
        "public_abi": "frame_conveyor",
        "resource_budget": build_resource_budget(profile),
    }


def build_summary(profile: ProfileName) -> str:
    budget = build_resource_budget(profile)
    shapes = {shape["name"]: shape["element_bytes"] for shape in budget["cell_shapes"]}
    lines = [
        f"profile: {profile}",
        f"macro-lane columns: {budget['macro_lane_columns']}",
        f"compute tiles per macro-lane: {budget['compute_tiles_per_macro_lane']}",
        f"memory tiles per macro-lane: {budget['memory_tiles_per_macro_lane']}",
        "cell shapes:",
    ]
    lines.extend(f"  {name}: {size} bytes" for name, size in sorted(shapes.items()))
    lines.append("halo policies:")
    lines.extend(f"  {policy['name']}: {policy['materialization']}" for policy in budget["halo_policies"])
    return "\n".join(lines)


def build_frame_summary(frame: FramePlan) -> str:
    lines = [
        f"profile: {frame.profile}",
        f"device: {frame.device}",
        f"capacity: {frame.capacity}",
        f"transport layout: {TRANSPORT_LAYOUT}",
        f"worker groups: {frame.lane_groups}",
        f"worker columns: {frame.macro_lane_columns}",
        f"physical columns used: {frame.lane_groups * frame.macro_lane_columns}",
        f"frame seam: {frame.seam_width}x{frame.seam_rows} C16",
        f"cells: {frame.cell_columns}x{frame.cell_rows}",
        f"frame slot bytes: {frame.frame_slot_bytes}",
        f"front zero-guarded ingress slot bytes: {frame.front_ingress_slot_bytes}",
        "worker stripes:"
    ]
    lines.extend(
        "  worker {lane}: cell columns {c0}..{c1}, cells={cells}, first_byte={byte}".format(
            lane=stripe.lane_index,
            c0=stripe.first_cell_column,
            c1=stripe.first_cell_column + stripe.cell_columns - 1,
            cells=stripe.cell_count,
            byte=stripe.frame_slot_first_byte,
        )
        for stripe in frame.lane_stripes
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--model-resolution", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--dump", choices=("budget", "contract", "summary", "frame", "frame-json"), default="summary")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.dump == "budget":
        print(json.dumps(build_resource_budget(args.profile), indent=2, sort_keys=False))
    elif args.dump == "contract":
        print(json.dumps(build_design_contract(args.profile), indent=2, sort_keys=False))
    elif args.dump == "frame":
        frame = build_frame_plan(args.profile, device=args.device, model_resolution=args.model_resolution, lane_groups=(args.worker_groups or args.lane_groups or "auto"))
        print(build_frame_summary(frame))
    elif args.dump == "frame-json":
        frame = build_frame_plan(args.profile, device=args.device, model_resolution=args.model_resolution, lane_groups=(args.worker_groups or args.lane_groups or "auto"))
        print(json.dumps({"frame_plan": frame.to_json(), "cell_offset_examples": build_cell_offset_examples(frame)}, indent=2))
    else:
        print(build_summary(args.profile))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
