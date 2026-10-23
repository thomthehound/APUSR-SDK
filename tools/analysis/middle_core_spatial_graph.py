#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""First real spatial graph construction helpers for the middle-core rewrite."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tools.analysis.middle_core_spatial_plan import (
    CELL_SHAPES,
    TRANSPORT_CELL_BYTES,
    TRANSPORT_PIXEL_BYTES,
    GUARD_PIXELS,
    C16_ROW_SEAM_BYTES,
    FRONT_C16_RING_ROWS,
    FRONT_C16_SEAM_RING_ROWS,
    MEMORY_TILE_BYTES,
    RESIDENT_PARAMETER_ARENA_BYTES,
    FRONT_C16_PARAMETER_SLICE_BYTES,
    STAGE1_HOT_RECORD_BYTES,
    ENCODER_C32_PARAMETER_SLICE_BYTES,
    DeviceName,
    FramePlan,
    ProfileName,
    ResolutionName,
    build_frame_plan,
    frame_cell_byte_offset,
)


DMA_HALO_TAP_WORD_BYTES = 8
# AIE DMA BD dimensions use bounded hardware fields. Keep generated tensor
# taps inside the range by splitting long frame walks and wide cell payloads.
HALO_TAP_MAX_BD_DIM_SIZE = 64
DMA_BD_MAX_DIM_SIZE = 1023
DMA_BD_MAX_LAYOUT_DIMS = 4
DMA_CELL_CHUNK_BYTES = 512
C16_HALO_PATCH_WORDS = CELL_SHAPES["c16_halo_cell"].element_bytes // DMA_HALO_TAP_WORD_BYTES
SKIP_SPILL_ALIGNMENT_BYTES = 4096
SKIPA_SPILL_OFFSET_BYTES = 0


def _align_up(value: int, alignment: int = SKIP_SPILL_ALIGNMENT_BYTES) -> int:
    return value if value % alignment == 0 else value + (alignment - value % alignment)


def _skipa_spill_bytes_per_frame(plan: GraphPhasePlan | None = None, frame: FramePlan | None = None) -> int:
    if frame is None:
        if plan is None:
            raise ValueError("SkipA spill sizing requires a graph plan or frame plan")
        frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    return frame.seam_width * frame.seam_rows * TRANSPORT_PIXEL_BYTES


def _skipb_spill_bytes_per_frame(plan: GraphPhasePlan | None = None, frame: FramePlan | None = None) -> int:
    if frame is None:
        if plan is None:
            raise ValueError("SkipB spill sizing requires a graph plan or frame plan")
        frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    return (frame.seam_width // 2) * (frame.seam_rows // 2) * CELL_SHAPES["c32_cell"].channels


def _skipb_spill_offset_bytes(plan: GraphPhasePlan | None = None, frame: FramePlan | None = None) -> int:
    return _align_up(_skipa_spill_bytes_per_frame(plan, frame))


def _skip_spill_payload_bytes(plan: GraphPhasePlan | None = None, frame: FramePlan | None = None) -> int:
    skipb_offset = _skipb_spill_offset_bytes(plan, frame)
    return _align_up(skipb_offset + _skipb_spill_bytes_per_frame(plan, frame))


def _skip_spill_arena_bytes(plan: GraphPhasePlan) -> int:
    return _skip_spill_payload_bytes(plan)


def _check_dma_bd_dimensions(label: str, dimensions: list[int]) -> None:
    if len(dimensions) > DMA_BD_MAX_LAYOUT_DIMS:
        raise ValueError(
            f"{label} DMA layout uses {len(dimensions)} dimensions; hardware supports at most "
            f"{DMA_BD_MAX_LAYOUT_DIMS}: {dimensions}"
        )
    bad = [dim for dim in dimensions if dim < 0 or dim > DMA_BD_MAX_DIM_SIZE]
    if bad:
        raise ValueError(f"{label} DMA dimensions exceed the hardware BD limit: {dimensions}")


def _check_dma_bd_layout(label: str, dimensions: list[int], strides: list[int]) -> None:
    _check_dma_bd_dimensions(label, dimensions)
    if len(dimensions) != len(strides):
        raise ValueError(f"{label} DMA dimensions/strides length mismatch: {dimensions} / {strides}")
    bad_strides = [stride for size, stride in zip(dimensions, strides, strict=True) if size > 1 and stride <= 0]
    if bad_strides:
        raise ValueError(f"{label} DMA strides must be positive for dimensions with size > 1: {dimensions} / {strides}")


def _cell_byte_chunks(cell_bytes: int) -> tuple[int, int]:
    if cell_bytes % DMA_CELL_CHUNK_BYTES != 0:
        raise ValueError(f"cell byte count {cell_bytes} is not divisible by DMA_CELL_CHUNK_BYTES")
    chunk_count = cell_bytes // DMA_CELL_CHUNK_BYTES
    _check_dma_bd_dimensions("cell payload", [chunk_count, DMA_CELL_CHUNK_BYTES])
    return chunk_count, DMA_CELL_CHUNK_BYTES


SHIM_ROW = 0
MEMTILE_ROW = 1
COMPUTE_ROWS = (2, 3, 4, 5)
STAGE1_2_TILE_ROW = 2
STAGE3_TILE_ROW = 3
CURRENT_PROGRAMMED_COLUMN = 0


@dataclass(frozen=True)
class PhysicalTileSlot:
    physical_column: int
    row: int
    tile_kind: str
    status: str
    role: str

    def to_json(self) -> dict[str, Any]:
        return asdict(self)


def build_current_reserved_tile_map(plan: "GraphPhasePlan") -> tuple[PhysicalTileSlot, ...]:
    """Return the explicit physical footprint for the current Stage1+2->Stage3 graph.

    The spatial4col profile reserves four physical columns. The current graph has
    only two programmed compute tiles inside that reserved footprint. Empty
    reserved tiles must stay empty; they are not implicit replicas.
    """

    slots: list[PhysicalTileSlot] = []
    for physical_column in range(plan.macro_lane_columns):
        slots.append(
            PhysicalTileSlot(
                physical_column=physical_column,
                row=SHIM_ROW,
                tile_kind="shim",
                status="reserved",
                role="interface/shim for this physical column",
            )
        )
        slots.append(
            PhysicalTileSlot(
                physical_column=physical_column,
                row=MEMTILE_ROW,
                tile_kind="memtile",
                status="reserved",
                role="memory-tile resource for this physical column",
            )
        )
        for row in COMPUTE_ROWS:
            role = "reserved-empty"
            status = "reserved-empty"
            if physical_column == CURRENT_PROGRAMMED_COLUMN and row == STAGE1_2_TILE_ROW:
                role = "Stage1+2 paired macroblock producer"
                status = "active"
            elif physical_column == CURRENT_PROGRAMMED_COLUMN and row == STAGE3_TILE_ROW:
                role = "Stage3 C16->C32 downsample consumer"
                status = "active"
            slots.append(
                PhysicalTileSlot(
                    physical_column=physical_column,
                    row=row,
                    tile_kind="compute",
                    status=status,
                    role=role,
                )
            )
    return tuple(slots)


def current_active_compute_tiles(plan: "GraphPhasePlan") -> tuple[PhysicalTileSlot, ...]:
    return tuple(slot for slot in build_current_reserved_tile_map(plan) if slot.tile_kind == "compute" and slot.status == "active")


def _format_current_tile_map(plan: "GraphPhasePlan") -> list[str]:
    by_coord = {(slot.row, slot.physical_column): slot for slot in build_current_reserved_tile_map(plan)}
    lines = ["physical tile map for current spatial-core footprint:"]
    for row in (5, 4, 3, 2, 1, 0):
        if row == SHIM_ROW:
            kind = "shim"
        elif row == MEMTILE_ROW:
            kind = "memtile"
        else:
            kind = "compute"
        cells = []
        for col in range(plan.macro_lane_columns):
            slot = by_coord[(row, col)]
            if slot.status == "active":
                cells.append(f"col{col}:{slot.role}")
            elif slot.tile_kind == "compute":
                cells.append(f"col{col}:reserved-empty")
            else:
                cells.append(f"col{col}:{slot.tile_kind}")
        lines.append(f"  row{row} {kind}: " + " | ".join(cells))
    return lines


@dataclass(frozen=True)
class ColumnShardPlan:
    lane_index: int
    macro_column_index: int
    physical_column: int
    first_cell_column: int
    cell_columns: int
    lane_cell_columns: int
    cell_rows: int
    first_byte: int
    owns_frame_left_edge: bool
    owns_frame_right_edge: bool
    needs_left_halo_exchange: bool
    needs_right_halo_exchange: bool

    @property
    def cell_count(self) -> int:
        return self.cell_columns * self.cell_rows

    @property
    def semantic_bytes(self) -> int:
        return self.cell_count * TRANSPORT_CELL_BYTES

    @property
    def lane_row_stride_bytes(self) -> int:
        return self.lane_cell_columns * TRANSPORT_CELL_BYTES

    @property
    def c16_row_bytes(self) -> int:
        return self.cell_columns * TRANSPORT_CELL_BYTES

    @property
    def front_c16_ring_rows(self) -> int:
        return 0

    @property
    def front_c16_ring_bytes(self) -> int:
        return 0

    @property
    def front_c16_ring_slack_bytes(self) -> int:
        return max(0, MEMORY_TILE_BYTES - self.front_c16_ring_bytes)

    @property
    def front_c16_seam_directions(self) -> int:
        return 0

    @property
    def front_c16_seam_staging_bytes(self) -> int:
        return 0

    @property
    def front_c16_seam_exchange(self) -> dict[str, Any]:
        return {
            "shape": "none",
            "seam_bytes": 0,
            "ring_rows": 0,
            "staging_bytes": 0,
            "staging": "removed_from_active_spatial_core",
            "consumer_input_dma_channels": 1,
            "needs_left_neighbor": False,
            "needs_right_neighbor": False,
            "interior_edge_policy": "interior halos are gathered by tensor DMA from the row-major frame",
            "shim_streams_added": 0,
            "full_row_fifos": 0,
            "per_cell_dma_tasks": 0,
        }

    @property
    def front_c16_row_engine(self) -> dict[str, Any]:
        halo_bytes = CELL_SHAPES["c16_halo_cell"].element_bytes
        return {
            "ring_rows": 0,
            "compile_carrier": "dma_tensor_halo_patch",
            "front_row_cells": 0,
            "front_row_bytes": 0,
            "front_row_fifo_objects": 0,
            "row_bytes": self.c16_row_bytes,
            "ring_bytes": 0,
            "memory_tile_slack_bytes": MEMORY_TILE_BYTES,
            "halo_patch_bytes": halo_bytes,
            "halo_patch_read_overhead_ratio": halo_bytes / TRANSPORT_CELL_BYTES,
            "cell_row_order": "row_major_c16_frame_tensor_tap",
            "cell_column_order": "row_major_c16_frame_tensor_tap",
            "top_bottom_frame_edges": "zero_guarded_ingress_frame",
            "vertical_window": "included_in_dma_gathered_18x10_patch",
            "left_right_interior_edges": "included_in_dma_gathered_18x10_patch",
            "seam_window": "none",
            "local_cell_boundary_policy": "no local seam synthesis; the patch is the consumer view",
            "uses_left_neighbor": False,
            "uses_right_neighbor": False,
            "front_parameter_slice_bytes": FRONT_C16_PARAMETER_SLICE_BYTES,
            "encoder_parameter_slice_bytes": ENCODER_C32_PARAMETER_SLICE_BYTES,
            "hot_path_parameter_streams": 0,
            "seam_exchange": self.front_c16_seam_exchange,
            "front_to_encoder_path": "dma_tensor_halo_patch_feeds_vector_srs_front_cell_then_tile_local_contract_params_feed_c32_to_c16_snapshot",
        }

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload.update(
            {
                "cell_count": self.cell_count,
                "semantic_bytes": self.semantic_bytes,
                "lane_row_stride_bytes": self.lane_row_stride_bytes,
                "c16_row_bytes": self.c16_row_bytes,
                "front_c16_ring_rows": self.front_c16_ring_rows,
                "front_c16_ring_bytes": self.front_c16_ring_bytes,
                "front_c16_ring_slack_bytes": self.front_c16_ring_slack_bytes,
                "front_c16_seam_staging_bytes": self.front_c16_seam_staging_bytes,
                "front_c16_seam_exchange": self.front_c16_seam_exchange,
                "front_c16_row_engine": self.front_c16_row_engine,
            }
        )
        return payload


@dataclass(frozen=True)
class GraphPhasePlan:
    name: str
    profile: ProfileName
    device: DeviceName
    capacity: ResolutionName
    lane_groups: int
    frame_slot_bytes: int
    macro_lane_columns: int
    column_shards: tuple[ColumnShardPlan, ...]

    @property
    def shard_count(self) -> int:
        return len(self.column_shards)

    @property
    def max_front_c16_ring_bytes(self) -> int:
        return max((shard.front_c16_ring_bytes for shard in self.column_shards), default=0)

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload["worker_groups"] = self.lane_groups
        payload["worker_columns"] = self.macro_lane_columns
        payload["reserved_physical_columns_per_worker"] = self.macro_lane_columns
        payload["physical_columns_used"] = self.lane_groups * self.macro_lane_columns
        payload["physical_column_shard_count"] = self.shard_count
        payload["shard_count"] = self.shard_count
        payload["max_front_c16_ring_bytes"] = self.max_front_c16_ring_bytes
        payload["column_shards"] = [shard.to_json() for shard in self.column_shards]
        if self.name in {"spatial_core_stage1_12_spine", "stage1_2_to_stage3_paired_macroblock_resident"}:
            payload["current_tile_map"] = [slot.to_json() for slot in build_current_reserved_tile_map(self)]
            payload["active_compute_tiles"] = [slot.to_json() for slot in current_active_compute_tiles(self)]
            payload["active_compute_tile_count"] = len(current_active_compute_tiles(self))
            payload["current_programmed_columns"] = [CURRENT_PROGRAMMED_COLUMN]
            payload["current_programmed_policy"] = "reserve the 4-column footprint, instantiate only programmed physical tiles"
        return payload


def _split_columns(count: int, parts: int) -> list[tuple[int, int]]:
    base = count // parts
    extra = count % parts
    spans: list[tuple[int, int]] = []
    cursor = 0
    for index in range(parts):
        width = base + (1 if index < extra else 0)
        spans.append((cursor, width))
        cursor += width
    return spans


def build_column_shards(frame: FramePlan) -> tuple[ColumnShardPlan, ...]:
    shards: list[ColumnShardPlan] = []
    for stripe in frame.lane_stripes:
        physical_column_base = stripe.lane_index * frame.macro_lane_columns
        for macro_column_index, (local_first_column, local_columns) in enumerate(
            _split_columns(stripe.cell_columns, frame.macro_lane_columns)
        ):
            first_cell_column = stripe.first_cell_column + local_first_column
            shards.append(
                ColumnShardPlan(
                    lane_index=stripe.lane_index,
                    macro_column_index=macro_column_index,
                    physical_column=physical_column_base + macro_column_index,
                    first_cell_column=first_cell_column,
                    cell_columns=local_columns,
                    lane_cell_columns=stripe.cell_columns,
                    cell_rows=frame.cell_rows,
                    first_byte=frame_cell_byte_offset(frame, first_cell_column, 0),
                    owns_frame_left_edge=first_cell_column == 0,
                    owns_frame_right_edge=first_cell_column + local_columns == frame.cell_columns,
                    needs_left_halo_exchange=first_cell_column != 0,
                    needs_right_halo_exchange=first_cell_column + local_columns != frame.cell_columns,
                )
            )
    return tuple(shards)


def _graph_phase_name(graph_phase: str) -> str:
    if graph_phase == "spatial-core":
        return "spatial_core_stage1_12_spine"
    if graph_phase == "cell-conveyor":
        return "cell_conveyor_envelope"
    if graph_phase == "c16-halo":
        return "c16_halo_envelope"
    if graph_phase == "front-c16-row":
        return "front_c16_row_engine"
    if graph_phase == "stage1-resident":
        return "stage1_resident_convnext16_proof"
    if graph_phase == "stage2-resident":
        return "stage2_resident_convnext16_producer_overlap_proof"
    if graph_phase == "stage2-plane-window-resident":
        return "stage2_resident_convnext16_plane_window_proof"
    if graph_phase == "stage1_2-linked-resident":
        return "stage1_2_linked_resident_tile_share_proof"
    if graph_phase == "stage1_2-fused-overlap-resident":
        return "stage1_2_fused_overlap_resident_proof"
    if graph_phase == "stage1_2-worker-ring-resident":
        return "stage1_2_worker_ring_resident"
    if graph_phase == "stage1_4-worker-ring-resident":
        return "stage1_4_worker_ring_resident"
    if graph_phase == "stage1_2-to-stage3_4-worker-ring-resident":
        return "stage1_2_to_stage3_4_worker_ring_resident"
    if graph_phase == "stage1_2-to-stage3-paired-macroblock-resident":
        return "stage1_2_to_stage3_paired_macroblock_resident"
    if graph_phase == "stage3-down-resident":
        return "stage3_downsample_c16_to_c32_proof"
    if graph_phase == "stage3_4-fused-resident":
        return "stage3_4_fused_downsample_fnb32_pressure_proof"
    if graph_phase == "stage3_4-product-resident":
        return "stage3_4_fused_downsample_fnb32_product_contract"
    raise ValueError(f"unknown graph phase: {graph_phase}")


def build_graph_phase_plan(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
    graph_phase: str = "spatial-core",
) -> GraphPhasePlan:
    frame = build_frame_plan(profile, device=device, model_resolution=model_resolution, lane_groups=lane_groups)
    return GraphPhasePlan(
        name=_graph_phase_name(graph_phase),
        profile=profile,
        device=device,
        capacity=model_resolution,
        lane_groups=frame.lane_groups,
        frame_slot_bytes=frame.frame_slot_bytes,
        macro_lane_columns=frame.macro_lane_columns,
        column_shards=build_column_shards(frame),
    )


def build_graph_phase_summary(plan: GraphPhasePlan) -> str:
    current_programmed = plan.name in {"spatial_core_stage1_12_spine", "stage1_2_to_stage3_paired_macroblock_resident"}
    lines = [
        f"graph phase: {plan.name}",
        f"profile: {plan.profile}",
        f"device: {plan.device}",
        f"capacity: {plan.capacity}",
        f"frame slot bytes: {plan.frame_slot_bytes}",
    ]
    if current_programmed:
        active_tiles = current_active_compute_tiles(plan)
        lines.extend(
            [
                f"capacity worker groups: {plan.lane_groups} (bookkeeping only; current graph is not cloned across groups)",
                f"reserved columns in current worker footprint: {plan.macro_lane_columns}",
                f"reserved compute-tile slots in current footprint: {plan.macro_lane_columns * len(COMPUTE_ROWS)}",
                f"active compute-tile slots in current graph: {len(active_tiles)}",
                "physicality policy: reserve the 4-column footprint, instantiate only explicitly programmed tiles",
            ]
        )
    else:
        lines.extend(
            [
                f"physical columns used: {plan.lane_groups * plan.macro_lane_columns}",
                f"physical column shards: {plan.shard_count}",
            ]
        )
    if plan.name == "spatial_core_stage1_12_spine":
        lines.extend(
            [
                "active milestone graph: fused Stage1+2 -> paired Stage2 2x2 C16 macroblock -> Stage3 downsample",
                "Stage2 output policy: produce every Stage2 C16 cell exactly once in paired 2x2 macroblocks, teeing the same cells to arg6 SkipA",
                "ring policy: no full Stage2 row ObjectFifos and no fused Stage3/4 20x40 C16 mega-window in the primary path",
                "Stage4 policy: split from Stage3; the C32 halo ring is the next boundary to wire",
                f"SkipA/SkipB spill arena carrier: {_skip_spill_arena_bytes(plan)} bytes, SkipA offset {SKIPA_SPILL_OFFSET_BYTES}",
                f"hot-path parameter streams: 0",
            ]
        )
    else:
        lines.extend(
            [
                "front C16 compile carrier: dma tensor halo patches",
                "max front row FIFO objects per physical column: 0",
                f"front parameter slice bytes: {FRONT_C16_PARAMETER_SLICE_BYTES}",
                f"encoder parameter slice bytes: {ENCODER_C32_PARAMETER_SLICE_BYTES}",
                "front C16 seam bytes: 0",
                "front C16 seam staging: removed_from_active_spatial_core",
                f"hot-path parameter streams: 0",
            ]
        )
    if current_programmed:
        lines.extend(_format_current_tile_map(plan))
        lines.append("programmed Stage1+2->Stage3 coverage:")
        lines.extend(
            "  physical col {physical}: Stage1+2 row{stage12_row} -> Stage3 row{stage3_row}; C32 cells x={x0}..{x1}, rows={rows}".format(
                physical=shard.physical_column,
                stage12_row=STAGE1_2_TILE_ROW,
                stage3_row=STAGE3_TILE_ROW,
                x0=shard.first_output_cell_column,
                x1=shard.first_output_cell_column + shard.output_cell_columns - 1,
                rows=shard.output_cell_rows,
            )
            for shard in build_stage1_2_to_stage3_programmed_shards(plan)
        )
    else:
        lines.append("physical column shards:")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: cells x={x0}..{x1}, rows={rows}, "
            "first_byte={first}, halo_patch=2880 bytes, seam_staging={seam} bytes, halo_exchange=tensor_dma".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_cell_column,
                x1=shard.first_cell_column + shard.cell_columns - 1,
                rows=shard.cell_rows,
                first=shard.first_byte,
                ring=shard.front_c16_ring_bytes,
                slack=shard.front_c16_ring_slack_bytes,
                seam=shard.front_c16_seam_staging_bytes,
                left=shard.needs_left_halo_exchange,
                right=shard.needs_right_halo_exchange,
            )
            for shard in plan.column_shards
        )
    if plan.name == "stage2_resident_convnext16_producer_overlap_proof":
        stage2_shards = build_stage2_shards(plan)
        lines.append("stage2 resident ConvNext16 shards:")
        lines.append("  activation source: Stage-1 C16 producer-overlap row ring; standalone carrier repeats one halo patch")
        lines.append(f"  C16 context window: {STAGE2_C16_CONTEXT_ROWS}x{STAGE2_C16_CONTEXT_COLUMNS}xC16, {STAGE2_C16_CONTEXT_BYTES} bytes")
        lines.append("  context policy: consume explicit producer-overlap halo patches; do not synthesize interior Stage2 halos from seams")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: output C16 cells x={x0}..{x1}, "
            "interior_rows={rows}, input C16 cells x={ix0}..{ix1}".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_output_cell_column,
                x1=shard.first_output_cell_column + shard.output_cell_columns - 1,
                rows=shard.output_cell_rows,
                ix0=shard.first_output_cell_column - 1,
                ix1=shard.first_output_cell_column + shard.output_cell_columns,
            )
            for shard in stage2_shards
        )
    if plan.name == "stage2_resident_convnext16_plane_window_proof":
        stage2_shards = build_stage2_shards(plan)
        lines.append("stage2 plane-window resident ConvNext16 shards:")
        lines.append("  activation source: one compact 18x10xC16 producer-plane window from a worker-local Stage-1 row-major ring")
        lines.append(f"  producer-plane window: {STAGE2_C16_CONTEXT_ROWS}x{STAGE2_C16_CONTEXT_COLUMNS}xC16, {STAGE2_C16_CONTEXT_BYTES} bytes")
        lines.append("  context policy: one input stream per compute tile; no three-input row-window carrier and no cross-worker hot-path dependency")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: output C16 cells x={x0}..{x1}, "
            "interior_rows={rows}, plane windows x={ix0}..{ix1}".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_output_cell_column,
                x1=shard.first_output_cell_column + shard.output_cell_columns - 1,
                rows=shard.output_cell_rows,
                ix0=shard.first_output_cell_column - 1,
                ix1=shard.first_output_cell_column + shard.output_cell_columns,
            )
            for shard in stage2_shards
        )
    if plan.name == "stage1_2_linked_resident_tile_share_proof":
        lines.append("stage1/2 linked resident shards:")
        lines.append("  activation source: zero-guarded ingress -> Stage-1 C16 cells -> tile-local Stage-2 carrier")
        lines.append(f"  Stage-2 carrier view: {STAGE2_C16_CONTEXT_ROWS}x{STAGE2_C16_CONTEXT_COLUMNS}xC16, {STAGE2_C16_CONTEXT_BYTES} bytes")
        lines.append("  context policy: no standalone Stage-2 DDR input; bridge is a tile-share compile carrier until the producer row ring is wired")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: Stage1 output cells x={x0}..{x1}, rows={rows}, "
            "Stage2 consumes linked C16 carrier patches".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_cell_column,
                x1=shard.first_cell_column + shard.cell_columns - 1,
                rows=shard.cell_rows,
            )
            for shard in plan.column_shards
        )
    if plan.name == "stage1_2_fused_overlap_resident_proof":
        stage2_shards = build_stage2_shards(plan)
        lines.append("stage1/2 fused-overlap resident proof shards:")
        lines.append("  activation source: 20x12xC16 source-domain window; Stage1 materializes only the 18x10 Stage1-domain patch needed by Stage2")
        lines.append("  ring policy: deletes the Stage1->Stage2 full-width boundary ring; computes the Stage1 pixels Stage2 would otherwise request from that ring")
        lines.append("  training/spec policy: same convolutional math and same resident parameters; only local evaluation footprint changes")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: output C16 cells x={x0}..{x1}, rows={rows}, source window {sy}x{sx}xC16".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_output_cell_column,
                x1=shard.first_output_cell_column + shard.output_cell_columns - 1,
                rows=shard.output_cell_rows,
                sy=STAGE1_2_FUSED_SOURCE_ROWS,
                sx=STAGE1_2_FUSED_SOURCE_COLUMNS,
            )
            for shard in stage2_shards
        )
    if plan.name == "stage1_2_to_stage3_paired_macroblock_resident":
        lines.append("Stage1+2 -> Stage3 compact paired-macroblock resident path:")
        lines.append("  Stage1+2 materializes one shared 34x18 Stage1 patch from one 36x20 C16 source macroblock")
        lines.append("  The shared Stage1 patch is reused to produce four Stage2 C16 cells per Stage3 output cell")
        lines.append("  Stage2->Stage3 boundary object: paired 2x2 C16 macroblock, 8192 bytes")
        lines.append("  SkipA policy: tee the same four Stage2 cells to long-lived arg6 SkipA while producing the Stage3 group")
        lines.append("  rejected path: no full Stage2 row ObjectFifos and no fused Stage3/4 20x40 C16 mega-window")
        lines.append("  Stage4 policy: split from Stage3; wire the C32 halo ring after this boundary is stable")
        lines.append(f"  SkipA/SkipB spill arena carrier: {_skip_spill_arena_bytes(plan)} bytes, SkipA offset {SKIPA_SPILL_OFFSET_BYTES}")
    if plan.name in {"stage1_2_worker_ring_resident", "stage1_4_worker_ring_resident", "stage1_2_to_stage3_4_worker_ring_resident"}:
        lines.append("worker-ring resident path:")
        if plan.name == "stage1_2_to_stage3_4_worker_ring_resident":
            lines.append("  Stage1+2 fused-overlap writes the Stage2-output C16 boundary ring carrier; fused Stage3/4 reads compact 20x40xC16 windows from it")
            lines.append("  ring note: the primary path uses worker-local on-array row-ring storage with a row-block tee to SkipA")
            lines.append("  ring policy: Stage1->Stage2 ring stays deleted; only the Stage2->Stage3/4 short-lived ring remains")
            lines.append("  SkipA policy: tee Stage2 rows to long-lived SkipA storage on production; ring eviction is slot reuse, not a per-cell callback")
            lines.append(f"  SkipA/SkipB spill arena carrier: {_skip_spill_arena_bytes(plan)} bytes, SkipA offset {SKIPA_SPILL_OFFSET_BYTES}")
        else:
            lines.append("  Stage1 writes a row-major C16 producer ring carrier; Stage2 reads compact 18x10xC16 plane windows from it")
            lines.append("  ring note: the worker-ring path uses worker-local on-array row-ring shards")
            if plan.name == "stage1_4_worker_ring_resident":
                lines.append("  Stage2 then writes a Stage2-output boundary ring carrier for the fused Stage3/4 20x40 C16 window")
                lines.append("  SkipA policy: tee Stage2 rows to long-lived SkipA storage on production; ring eviction is slot reuse, not a per-cell callback")
                lines.append(f"  SkipA/SkipB spill arena carrier: {_skip_spill_arena_bytes(plan)} bytes, SkipA offset {SKIPA_SPILL_OFFSET_BYTES}")
        lines.append("  worker ring policy: one independent 4-column worker owns its stripe, guards, rings, and memory-tile shards")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: cells x={x0}..{x1}, rows={rows}".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_cell_column,
                x1=shard.first_cell_column + shard.cell_columns - 1,
                rows=shard.cell_rows,
            )
            for shard in plan.column_shards
        )
    if plan.name == "stage3_downsample_c16_to_c32_proof":
        downsample_shards = build_downsample_shards(plan)
        lines.append("stage3 downsample proof shards:")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: output C32 cells x={x0}..{x1}, "
            "rows={rows}, input C16 cells x={ix0}..{ix1}, window_bytes={window}".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_output_cell_column,
                x1=shard.first_output_cell_column + shard.output_cell_columns - 1,
                rows=shard.output_cell_rows,
                ix0=shard.input_first_cell_column,
                ix1=shard.input_first_cell_column + shard.input_cell_columns - 1,
                window=shard.c16_input_window_bytes,
            )
            for shard in downsample_shards
        )
        if any(shard.input_cell_rows % 2 for shard in downsample_shards):
            lines.append("stage3 proof tail policy: complete 2x2 C16 cell windows only; odd C16 tail rows need the bounded zero-source path before product chaining")
    if plan.name in {"stage3_4_fused_downsample_fnb32_pressure_proof", "stage3_4_fused_downsample_fnb32_product_contract"}:
        fused_shards = build_stage3_4_fused_shards(plan)
        source = "upstream C16 producer-overlap row ring" if plan.name.endswith("product_contract") else "standalone row-major C16 carrier"
        lines.append("stage3/4 fused resident shards:")
        lines.append(f"  activation source: {source}")
        lines.append(f"  fused C16 context window: {STAGE3_4_FUSED_C16_WINDOW_ROWS}x{STAGE3_4_FUSED_C16_WINDOW_COLUMNS}xC16, {STAGE3_4_FUSED_C16_WINDOW_BYTES} bytes")
        lines.append("  context policy: reuse producer overlap; do not synthesize interior seam halos")
        lines.extend(
            "  worker {lane} col {macro_col} -> physical col {physical}: output C32 cells x={x0}..{x1}, "
            "interior_rows={rows}, input C16 pixels x={px0}..{px1} via producer overlap".format(
                lane=shard.lane_index,
                macro_col=shard.macro_column_index,
                physical=shard.physical_column,
                x0=shard.first_output_cell_column,
                x1=shard.first_output_cell_column + shard.output_cell_columns - 1,
                rows=shard.output_cell_rows,
                px0=shard.first_output_cell_column * CELL_SHAPES["c32_cell"].width * 2 - 2,
                px1=shard.first_output_cell_column * CELL_SHAPES["c32_cell"].width * 2 - 2 + STAGE3_4_FUSED_C16_WINDOW_COLUMNS - 1,
            )
            for shard in fused_shards
        )
    return "\n".join(lines)


def _transfer_tap(frame_slot_bytes: int, shard: ColumnShardPlan):
    return _transfer_row_span_tap(frame_slot_bytes, shard, first_cell_row=0, cell_rows=shard.cell_rows)


def _transfer_row_span_tap(
    frame_slot_bytes: int,
    shard: ColumnShardPlan,
    *,
    first_cell_row: int,
    cell_rows: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if first_cell_row < 0 or cell_rows <= 0 or first_cell_row + cell_rows > shard.cell_rows:
        raise ValueError("row span must be inside the column shard")

    first_byte = shard.first_byte + first_cell_row * shard.lane_row_stride_bytes
    return TensorAccessPattern(
        (1, frame_slot_bytes),
        first_byte,
        [1, cell_rows, shard.cell_columns, TRANSPORT_CELL_BYTES],
        [0, shard.lane_row_stride_bytes, TRANSPORT_CELL_BYTES, 1],
    )


def _transfer_shard_row_tap(frame_slot_bytes: int, shard: ColumnShardPlan):
    from aie.helpers.taplib.tap import TensorAccessPattern

    return TensorAccessPattern(
        (1, frame_slot_bytes),
        shard.first_byte,
        [1, shard.cell_rows, shard.c16_row_bytes],
        [0, shard.lane_row_stride_bytes, 1],
    )


def _front_halo_ingress_words(plan: GraphPhasePlan) -> int:
    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    return frame.front_ingress_slot_bytes // DMA_HALO_TAP_WORD_BYTES


def _zero_guarded_halo_patch_tap(
    plan: GraphPhasePlan,
    shard: ColumnShardPlan,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if row_count <= 0:
        raise ValueError("halo patch row_count must be positive")
    if row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("halo patch row_count exceeds the DMA BD dimension limit")
    if row_start < 0 or row_start + row_count > shard.cell_rows:
        raise ValueError("halo patch row block must stay inside the column shard")

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    halo_view_width = frame.seam_width + 2 * GUARD_PIXELS
    row_stride_words = halo_view_width * TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    pixel_stride_words = TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    patch_row_words = CELL_SHAPES["c16_halo_cell"].width * pixel_stride_words
    first_pixel_x = shard.first_cell_column * 16
    first_pixel_y = row_start * CELL_SHAPES["c16_cell"].rows
    first_word = (first_pixel_y * halo_view_width + first_pixel_x) * pixel_stride_words
    return TensorAccessPattern(
        (1, frame.front_ingress_slot_bytes // DMA_HALO_TAP_WORD_BYTES),
        first_word,
        [row_count, shard.cell_columns, CELL_SHAPES["c16_halo_cell"].rows, patch_row_words],
        [CELL_SHAPES["c16_cell"].rows * row_stride_words, 16 * pixel_stride_words, row_stride_words, 1],
    )


def _halo_patch_row_blocks(shard: ColumnShardPlan):
    for row_start in range(0, shard.cell_rows, HALO_TAP_MAX_BD_DIM_SIZE):
        row_count = min(HALO_TAP_MAX_BD_DIM_SIZE, shard.cell_rows - row_start)
        yield row_start, row_count


def _row_major_c16_cell_row_block_tap(
    plan: GraphPhasePlan,
    shard: ColumnShardPlan,
    *,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if row_count <= 0:
        raise ValueError("egress row_count must be positive")
    if row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("egress row_count exceeds the DMA BD dimension limit")
    if row_start < 0 or row_start + row_count > shard.cell_rows:
        raise ValueError("egress row block must stay inside the column shard")

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    frame_words = plan.frame_slot_bytes // DMA_HALO_TAP_WORD_BYTES
    row_stride_words = frame.seam_width * TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    pixel_stride_words = TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    cell_row_words = CELL_SHAPES["c16_cell"].width * pixel_stride_words
    first_pixel_x = shard.first_cell_column * CELL_SHAPES["c16_cell"].width
    first_pixel_y = row_start * CELL_SHAPES["c16_cell"].rows
    first_word = (first_pixel_y * frame.seam_width + first_pixel_x) * pixel_stride_words
    return TensorAccessPattern(
        (1, frame_words),
        first_word,
        [row_count, shard.cell_columns, CELL_SHAPES["c16_cell"].rows, cell_row_words],
        [CELL_SHAPES["c16_cell"].rows * row_stride_words, CELL_SHAPES["c16_cell"].width * pixel_stride_words, row_stride_words, 1],
    )


def _param_arena_slice_tap(offset_bytes: int, byte_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if offset_bytes < 0 or byte_count <= 0 or offset_bytes + byte_count > RESIDENT_PARAMETER_ARENA_BYTES:
        raise ValueError("resident parameter slice must stay inside the arena")
    return TensorAccessPattern(
        (1, RESIDENT_PARAMETER_ARENA_BYTES),
        offset_bytes,
        [1, byte_count],
        [0, 1],
    )


def _m4x8_c16_cell_row_block_tap(
    plan: GraphPhasePlan,
    shard: ColumnShardPlan,
    *,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if row_count <= 0:
        raise ValueError("egress row_count must be positive")
    if row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("egress row_count exceeds the DMA BD dimension limit")
    if row_start < 0 or row_start + row_count > shard.cell_rows:
        raise ValueError("egress row block must stay inside the column shard")

    cell_words = CELL_SHAPES["c16_cell"].element_bytes // DMA_HALO_TAP_WORD_BYTES
    frame_words = plan.frame_slot_bytes // DMA_HALO_TAP_WORD_BYTES
    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    first_cell = row_start * frame.cell_columns + shard.first_cell_column
    first_word = first_cell * cell_words
    return TensorAccessPattern(
        (1, frame_words),
        first_word,
        [row_count, shard.cell_columns, cell_words],
        [frame.cell_columns * cell_words, cell_words, 1],
    )


def _cell_row_blocks(shard: ColumnShardPlan):
    for row_start in range(0, shard.cell_rows, HALO_TAP_MAX_BD_DIM_SIZE):
        row_count = min(HALO_TAP_MAX_BD_DIM_SIZE, shard.cell_rows - row_start)
        yield row_start, row_count




def _transfer_shard_cell_column_tap(frame_slot_bytes: int, shard: ColumnShardPlan, local_cell_column: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if local_cell_column < 0 or local_cell_column >= shard.cell_columns:
        raise ValueError("cell column must be inside the column shard")
    first_byte = shard.first_byte + local_cell_column * TRANSPORT_CELL_BYTES
    return TensorAccessPattern(
        (1, frame_slot_bytes),
        first_byte,
        [1, shard.cell_rows, TRANSPORT_CELL_BYTES],
        [0, shard.lane_row_stride_bytes, 1],
    )

def _transfer_shard_cell_column_major_tap(frame_slot_bytes: int, shard: ColumnShardPlan):
    from aie.helpers.taplib.tap import TensorAccessPattern

    return TensorAccessPattern(
        (1, frame_slot_bytes),
        shard.first_byte,
        [1, shard.cell_columns, shard.cell_rows, TRANSPORT_CELL_BYTES],
        [0, TRANSPORT_CELL_BYTES, shard.lane_row_stride_bytes, 1],
    )


FRONT_C16_WINDOW_CELLS = 7
FRONT_C16_OUTPUT_CHUNK_CELLS = 5
FRONT_C16_WINDOW_STEP_CELLS = 4


@dataclass(frozen=True)
class FrontWindowChunk:
    output_first_cell_column: int
    output_cell_columns: int
    window_first_cell_column: int

    @property
    def first_center_index(self) -> int:
        return self.output_first_cell_column - self.window_first_cell_column

    @property
    def center_indices(self) -> tuple[int, ...]:
        return tuple(
            range(
                self.first_center_index,
                self.first_center_index + self.output_cell_columns,
            )
        )



@dataclass(frozen=True)
class Stage2ShardPlan:
    lane_index: int
    macro_column_index: int
    physical_column: int
    first_output_cell_column: int
    output_cell_columns: int
    output_cell_rows: int

    @property
    def output_cell_count(self) -> int:
        return self.output_cell_columns * self.output_cell_rows

    @property
    def c16_input_context_shape(self) -> str:
        return f"{STAGE2_C16_CONTEXT_ROWS}x{STAGE2_C16_CONTEXT_COLUMNS}xC16"

    @property
    def c16_input_context_bytes(self) -> int:
        return STAGE2_C16_CONTEXT_BYTES

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload.update(
            {
                "output_cell_count": self.output_cell_count,
                "c16_input_context_shape": self.c16_input_context_shape,
                "c16_input_context_bytes": self.c16_input_context_bytes,
            }
        )
        return payload


def build_stage2_shards(plan: GraphPhasePlan) -> tuple[Stage2ShardPlan, ...]:
    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    interior_columns = max(0, frame.cell_columns - 2)
    interior_rows = max(0, frame.cell_rows - 2)
    physical_columns = plan.lane_groups * plan.macro_lane_columns
    shards: list[Stage2ShardPlan] = []
    for physical_column, (interior_first_column, output_columns) in enumerate(_split_columns(interior_columns, physical_columns)):
        first_output_column = 1 + interior_first_column
        lane_index = physical_column // plan.macro_lane_columns
        macro_column_index = physical_column % plan.macro_lane_columns
        shards.append(
            Stage2ShardPlan(
                lane_index=lane_index,
                macro_column_index=macro_column_index,
                physical_column=physical_column,
                first_output_cell_column=first_output_column,
                output_cell_columns=output_columns,
                output_cell_rows=interior_rows,
            )
        )
    return tuple(shards)


def _stage2_window_row_blocks(shard: Stage2ShardPlan):
    for row_start in range(0, shard.output_cell_rows, HALO_TAP_MAX_BD_DIM_SIZE):
        row_count = min(HALO_TAP_MAX_BD_DIM_SIZE, shard.output_cell_rows - row_start)
        yield row_start, row_count


def _stage2_c16_halo_patch_row_block_tap(
    plan: GraphPhasePlan,
    shard: Stage2ShardPlan,
    row_start: int,
    row_count: int,
    source_words: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage2 row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage2 row block must stay inside the interior proof shard")

    # The standalone Stage-2 compile proof uses a compact carrier containing
    # explicit halo patches for one DMA row block. The product path should feed
    # equivalent 18x10xC16 views from the Stage-1 producer-overlap row ring
    # instead of asking DMA to synthesize 3x3 cell slabs.
    dimensions = [row_count, shard.output_cell_columns, STAGE2_C16_CONTEXT_WORDS]
    strides = [shard.output_cell_columns * STAGE2_C16_CONTEXT_WORDS, STAGE2_C16_CONTEXT_WORDS, 1]
    required_words = ((row_count - 1) * strides[0]) + ((shard.output_cell_columns - 1) * strides[1]) + STAGE2_C16_CONTEXT_WORDS
    if required_words > source_words:
        raise ValueError(f"stage2 c16 halo patch carrier too small: requires {required_words}, has {source_words}")
    _check_dma_bd_layout("stage2 c16 halo patch input", dimensions, strides)
    return TensorAccessPattern(
        (1, source_words),
        0,
        dimensions,
        strides,
    )


def _stage2_c16_plane_window_row_block_tap(
    plan: GraphPhasePlan,
    shard: Stage2ShardPlan,
    row_start: int,
    row_count: int,
    vertical_offset: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if vertical_offset not in (-1, 0, 1):
        raise ValueError("stage2 plane-window vertical offset must be -1, 0, or 1")
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage2 plane-window row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage2 plane-window row block must stay inside the interior proof shard")

    cell_words = CELL_SHAPES["c16_cell"].element_bytes // DMA_HALO_TAP_WORD_BYTES
    first_output_row = 1 + row_start
    first_source_row = first_output_row + vertical_offset
    first_source_column = shard.first_output_cell_column - 1
    if first_source_row < 0 or first_source_row >= frame.cell_rows:
        raise ValueError("stage2 plane-window source row is outside the C16 producer frame")
    if first_source_column < 0 or first_source_column + STAGE2_C16_ROW_WINDOW_CELLS > frame.cell_columns:
        raise ValueError("stage2 plane-window source columns are outside the C16 producer frame")

    first_word = (first_source_row * frame.cell_columns + first_source_column) * cell_words
    dimensions = [row_count, shard.output_cell_columns, STAGE2_C16_ROW_WINDOW_WORDS]
    strides = [frame.cell_columns * cell_words, cell_words, 1]
    _check_dma_bd_layout("stage2 c16 producer plane-window input", dimensions, strides)
    return TensorAccessPattern(
        (1, plan.frame_slot_bytes // DMA_HALO_TAP_WORD_BYTES),
        first_word,
        dimensions,
        strides,
    )


def _stage2_c16_output_row_block_tap(plan: GraphPhasePlan, shard: Stage2ShardPlan, row_start: int, row_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage2 output row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage2 output row block must stay inside the interior proof shard")

    cell_bytes = CELL_SHAPES["c16_cell"].element_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    first_output_row = 1 + row_start
    first_byte = (first_output_row * frame.cell_columns + shard.first_output_cell_column) * cell_bytes
    dimensions = [row_count, shard.output_cell_columns, chunk_count, chunk_bytes]
    strides = [frame.cell_columns * cell_bytes, cell_bytes, chunk_bytes, 1]
    _check_dma_bd_dimensions("stage2 c16 output cell", dimensions)
    return TensorAccessPattern(
        (1, plan.frame_slot_bytes),
        first_byte,
        dimensions,
        strides,
    )


def _stage2_skipa_spill_row_block_tap(
    plan: GraphPhasePlan,
    shard: Stage2ShardPlan,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("SkipA spill row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("SkipA spill row block must stay inside the Stage2 producer shard")

    cell_bytes = CELL_SHAPES["c16_cell"].element_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    first_output_row = 1 + row_start
    first_byte = (
        SKIPA_SPILL_OFFSET_BYTES
        + (first_output_row * frame.cell_columns + shard.first_output_cell_column) * cell_bytes
    )
    dimensions = [row_count, shard.output_cell_columns, chunk_count, chunk_bytes]
    strides = [frame.cell_columns * cell_bytes, cell_bytes, chunk_bytes, 1]
    _check_dma_bd_dimensions("stage2 SkipA spill row block", dimensions)
    return TensorAccessPattern(
        (1, _skip_spill_arena_bytes(plan)),
        first_byte,
        dimensions,
        strides,
    )


@dataclass(frozen=True)
class DownsampleShardPlan:
    lane_index: int
    macro_column_index: int
    physical_column: int
    first_output_cell_column: int
    output_cell_columns: int
    output_cell_rows: int
    input_first_cell_column: int
    input_cell_columns: int
    input_cell_rows: int
    input_lane_cell_columns: int
    input_first_byte: int

    @property
    def output_cell_count(self) -> int:
        return self.output_cell_columns * self.output_cell_rows

    @property
    def c16_input_window_cells(self) -> int:
        return 4

    @property
    def c16_input_window_bytes(self) -> int:
        return self.c16_input_window_cells * TRANSPORT_CELL_BYTES

    def to_json(self) -> dict[str, Any]:
        payload = asdict(self)
        payload.update(
            {
                "output_cell_count": self.output_cell_count,
                "c16_input_context_shape": self.c16_input_context_shape,
                "c16_input_context_bytes": self.c16_input_context_bytes,
                "stage3_output_cell_bytes": CELL_SHAPES["c32_cell"].element_bytes,
            }
        )
        return payload


def _downsample_cell_rows(input_cell_rows: int) -> int:
    # The first transition proof covers complete 2x2 C16 cell windows. Odd tail
    # cell rows need a bounded zero-source path, not speculative overread.
    return input_cell_rows // 2


def _downsample_cell_columns(input_cell_columns: int) -> int:
    input_pixels = input_cell_columns * CELL_SHAPES["c16_cell"].width
    output_pixels = (input_pixels + 1) // 2
    return (output_pixels + CELL_SHAPES["c32_cell"].width - 1) // CELL_SHAPES["c32_cell"].width


def build_downsample_shards(plan: GraphPhasePlan) -> tuple[DownsampleShardPlan, ...]:
    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_cell_columns = _downsample_cell_columns(frame.cell_columns)
    output_cell_rows = _downsample_cell_rows(frame.cell_rows)
    physical_columns = plan.lane_groups * plan.macro_lane_columns
    shards: list[DownsampleShardPlan] = []
    for physical_column, (first_output_column, output_columns) in enumerate(_split_columns(output_cell_columns, physical_columns)):
        input_first_column = first_output_column * 2
        input_columns = output_columns * 2
        lane_index = physical_column // plan.macro_lane_columns
        macro_column_index = physical_column % plan.macro_lane_columns
        shards.append(
            DownsampleShardPlan(
                lane_index=lane_index,
                macro_column_index=macro_column_index,
                physical_column=physical_column,
                first_output_cell_column=first_output_column,
                output_cell_columns=output_columns,
                output_cell_rows=output_cell_rows,
                input_first_cell_column=input_first_column,
                input_cell_columns=input_columns,
                input_cell_rows=frame.cell_rows,
                input_lane_cell_columns=frame.cell_columns,
                input_first_byte=frame_cell_byte_offset(frame, input_first_column, 0),
            )
        )
    return tuple(shards)


def build_stage1_2_to_stage3_programmed_shards(plan: GraphPhasePlan) -> tuple[DownsampleShardPlan, ...]:
    """Return the active physical shards for the current programmed graph.

    This is intentionally not a lane/column splitter. The spatial4col footprint
    reserves four physical columns, but the current implementation programs only
    the Stage1+2 tile at (col0,row2) and the Stage3 tile at (col0,row3). The
    full Stage2 lattice is assigned to that one programmed pair until more
    physical tiles are explicitly added to the manifest.
    """

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_cell_columns = _downsample_cell_columns(frame.cell_columns)
    output_cell_rows = _downsample_cell_rows(frame.cell_rows)
    return (
        DownsampleShardPlan(
            lane_index=0,
            macro_column_index=0,
            physical_column=CURRENT_PROGRAMMED_COLUMN,
            first_output_cell_column=0,
            output_cell_columns=output_cell_columns,
            output_cell_rows=output_cell_rows,
            input_first_cell_column=0,
            input_cell_columns=frame.cell_columns,
            input_cell_rows=frame.cell_rows,
            input_lane_cell_columns=frame.cell_columns,
            input_first_byte=frame_cell_byte_offset(frame, 0, 0),
        ),
    )


def _stage3_c16_input_window_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, output_cell_row: int, local_output_cell_column: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if output_cell_row < 0 or output_cell_row >= shard.output_cell_rows:
        raise ValueError("stage3 output row is outside the complete 2x2 input-window proof")
    if local_output_cell_column < 0 or local_output_cell_column >= shard.output_cell_columns:
        raise ValueError("stage3 output column is outside the downsample shard")

    cell_bytes = CELL_SHAPES["c16_cell"].element_bytes
    first_input_column = shard.input_first_cell_column + local_output_cell_column * 2
    first_input_row = output_cell_row * 2
    first_byte = (first_input_row * frame.cell_columns + first_input_column) * cell_bytes
    return TensorAccessPattern(
        (1, frame.frame_slot_bytes),
        first_byte,
        [2, 2, cell_bytes],
        [frame.cell_columns * cell_bytes, cell_bytes, 1],
    )


def _stage3_c32_output_frame_bytes(plan: GraphPhasePlan) -> int:
    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    return _downsample_cell_columns(frame.cell_columns) * _downsample_cell_rows(frame.cell_rows) * CELL_SHAPES["c32_cell"].element_bytes


def _stage3_c32_output_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, output_cell_row: int, local_output_cell_column: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_columns = _downsample_cell_columns(frame.cell_columns)
    output_rows = _downsample_cell_rows(frame.cell_rows)
    if output_cell_row < 0 or output_cell_row >= output_rows:
        raise ValueError("stage3 output row is outside the output frame")
    if local_output_cell_column < 0 or local_output_cell_column >= shard.output_cell_columns:
        raise ValueError("stage3 output column is outside the downsample shard")

    cell_bytes = CELL_SHAPES["c32_cell"].element_bytes
    output_cell_column = shard.first_output_cell_column + local_output_cell_column
    first_byte = (output_cell_row * output_columns + output_cell_column) * cell_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    dimensions = [chunk_count, chunk_bytes]
    strides = [chunk_bytes, 1]
    _check_dma_bd_dimensions("stage3 c32 output cell", dimensions)
    return TensorAccessPattern(
        (1, _stage3_c32_output_frame_bytes(plan)),
        first_byte,
        dimensions,
        strides,
    )




def _stage3_window_row_blocks(shard: DownsampleShardPlan):
    for row_start in range(0, shard.output_cell_rows, HALO_TAP_MAX_BD_DIM_SIZE):
        row_count = min(HALO_TAP_MAX_BD_DIM_SIZE, shard.output_cell_rows - row_start)
        yield row_start, row_count


def _stage3_c16_input_window_row_block_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, row_start: int, row_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage3 row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage3 row block must stay inside the downsample shard")

    cell_bytes = CELL_SHAPES["c16_cell"].element_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    first_input_row = row_start * 2
    first_byte = (first_input_row * frame.cell_columns + shard.input_first_cell_column) * cell_bytes
    dimensions = [row_count, shard.output_cell_columns, 2, 2, chunk_count, chunk_bytes]
    strides = [
        2 * frame.cell_columns * cell_bytes,
        2 * cell_bytes,
        frame.cell_columns * cell_bytes,
        cell_bytes,
        chunk_bytes,
        1,
    ]
    _check_dma_bd_dimensions("stage3 c16 input window", dimensions)
    return TensorAccessPattern(
        (1, frame.frame_slot_bytes),
        first_byte,
        dimensions,
        strides,
    )


def _stage3_c32_output_row_block_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, row_start: int, row_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_columns = _downsample_cell_columns(frame.cell_columns)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage3 output row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage3 output row block must stay inside the downsample shard")

    cell_bytes = CELL_SHAPES["c32_cell"].element_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    first_byte = (row_start * output_columns + shard.first_output_cell_column) * cell_bytes
    dimensions = [row_count, shard.output_cell_columns, chunk_count, chunk_bytes]
    strides = [output_columns * cell_bytes, cell_bytes, chunk_bytes, 1]
    _check_dma_bd_dimensions("stage3 c32 output row block", dimensions)
    return TensorAccessPattern(
        (1, _stage3_c32_output_frame_bytes(plan)),
        first_byte,
        dimensions,
        strides,
    )


def build_stage3_4_fused_shards(plan: GraphPhasePlan) -> tuple[DownsampleShardPlan, ...]:
    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_cell_columns = _downsample_cell_columns(frame.cell_columns)
    output_cell_rows = _downsample_cell_rows(frame.cell_rows)
    interior_columns = max(0, output_cell_columns - 2)
    interior_rows = max(0, output_cell_rows - 2)
    physical_columns = plan.lane_groups * plan.macro_lane_columns
    shards: list[DownsampleShardPlan] = []
    for physical_column, (interior_first_column, output_columns) in enumerate(_split_columns(interior_columns, physical_columns)):
        first_output_column = 1 + interior_first_column
        input_first_column = first_output_column * 2 - 1
        lane_index = physical_column // plan.macro_lane_columns
        macro_column_index = physical_column % plan.macro_lane_columns
        shards.append(
            DownsampleShardPlan(
                lane_index=lane_index,
                macro_column_index=macro_column_index,
                physical_column=physical_column,
                first_output_cell_column=first_output_column,
                output_cell_columns=output_columns,
                output_cell_rows=interior_rows,
                input_first_cell_column=input_first_column,
                input_cell_columns=output_columns * 2 + 2,
                input_cell_rows=frame.cell_rows,
                input_lane_cell_columns=frame.cell_columns,
                input_first_byte=frame_cell_byte_offset(frame, input_first_column, 1),
            )
        )
    return tuple(shards)


def _stage3_paired_macroblock_row_blocks(shard: DownsampleShardPlan):
    for row_start in range(0, shard.output_cell_rows, HALO_TAP_MAX_BD_DIM_SIZE):
        row_count = min(HALO_TAP_MAX_BD_DIM_SIZE, shard.output_cell_rows - row_start)
        yield row_start, row_count


def _stage3_4_c16_input_window20x40_row_block_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, row_start: int, row_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage3/4 fused row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage3/4 fused row block must stay inside the interior proof shard")

    row_stride_bytes = frame.seam_width * TRANSPORT_PIXEL_BYTES
    first_output_row = 1 + row_start
    first_pixel_y = first_output_row * CELL_SHAPES["c32_cell"].rows * 2 - 2
    first_pixel_x = shard.first_output_cell_column * CELL_SHAPES["c32_cell"].width * 2 - 2
    first_byte = (first_pixel_y * frame.seam_width + first_pixel_x) * TRANSPORT_PIXEL_BYTES
    window_row_bytes = STAGE3_4_FUSED_C16_WINDOW_COLUMNS * TRANSPORT_PIXEL_BYTES
    dimensions = [row_count, shard.output_cell_columns, STAGE3_4_FUSED_C16_WINDOW_ROWS, window_row_bytes]
    strides = [
        CELL_SHAPES["c32_cell"].rows * 2 * row_stride_bytes,
        CELL_SHAPES["c32_cell"].width * 2 * TRANSPORT_PIXEL_BYTES,
        row_stride_bytes,
        1,
    ]
    _check_dma_bd_dimensions("stage3/4 fused c16 input window", dimensions)
    return TensorAccessPattern(
        (1, plan.frame_slot_bytes),
        first_byte,
        dimensions,
        strides,
    )


def _stage3_4_c32_output_row_block_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, row_start: int, row_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_columns = _downsample_cell_columns(frame.cell_columns)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage3/4 fused output row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage3/4 fused output row block must stay inside the interior proof shard")

    cell_bytes = CELL_SHAPES["c32_cell"].element_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    first_byte = (row_start * output_columns + shard.first_output_cell_column) * cell_bytes
    dimensions = [row_count, shard.output_cell_columns, chunk_count, chunk_bytes]
    strides = [output_columns * cell_bytes, cell_bytes, chunk_bytes, 1]
    _check_dma_bd_dimensions("stage3/4 fused c32 output row block", dimensions)
    return TensorAccessPattern(
        (1, _stage3_c32_output_frame_bytes(plan)),
        first_byte,
        dimensions,
        strides,
    )



def _front_window_chunks(shard: ColumnShardPlan) -> tuple[FrontWindowChunk, ...]:
    if shard.cell_columns < FRONT_C16_WINDOW_CELLS:
        raise ValueError("front window stream requires at least seven C16 cells per physical shard")

    chunks: list[FrontWindowChunk] = []
    cursor = 0
    while cursor < shard.cell_columns:
        output_columns = min(FRONT_C16_OUTPUT_CHUNK_CELLS, shard.cell_columns - cursor)
        if cursor == 0:
            window_first = 0
        elif cursor + output_columns == shard.cell_columns:
            window_first = shard.cell_columns - FRONT_C16_WINDOW_CELLS
        else:
            window_first = cursor - 1
        chunks.append(
            FrontWindowChunk(
                output_first_cell_column=cursor,
                output_cell_columns=output_columns,
                window_first_cell_column=window_first,
            )
        )
        cursor += output_columns

    return tuple(chunks)


def _transfer_front_window_tap(frame_slot_bytes: int, shard: ColumnShardPlan):
    from aie.helpers.taplib.tap import TensorAccessPattern

    chunks = _front_window_chunks(shard)
    if not chunks:
        raise ValueError("front window stream needs at least one chunk")
    if any(chunk.output_cell_columns != FRONT_C16_OUTPUT_CHUNK_CELLS for chunk in chunks):
        raise ValueError("front window stream currently requires five output cells per chunk")
    if any(chunk.window_first_cell_column != index * FRONT_C16_WINDOW_STEP_CELLS for index, chunk in enumerate(chunks)):
        raise ValueError("front window stream currently requires uniform 7-cell windows stepped by four cells")

    return TensorAccessPattern(
        (1, frame_slot_bytes),
        shard.first_byte,
        [1, shard.cell_rows, len(chunks), FRONT_C16_WINDOW_CELLS * TRANSPORT_CELL_BYTES],
        [0, shard.lane_row_stride_bytes, FRONT_C16_WINDOW_STEP_CELLS * TRANSPORT_CELL_BYTES, 1],
    )


def _transfer_front_output_chunk_tap(frame_slot_bytes: int, shard: ColumnShardPlan):
    from aie.helpers.taplib.tap import TensorAccessPattern

    chunks = _front_window_chunks(shard)
    if not chunks:
        raise ValueError("front output stream needs at least one chunk")
    if any(chunk.output_cell_columns != FRONT_C16_OUTPUT_CHUNK_CELLS for chunk in chunks):
        raise ValueError("front output stream currently requires five output cells per chunk")

    return TensorAccessPattern(
        (1, frame_slot_bytes),
        shard.first_byte,
        [1, shard.cell_rows, len(chunks), FRONT_C16_OUTPUT_CHUNK_CELLS, TRANSPORT_CELL_BYTES],
        [0, shard.lane_row_stride_bytes, FRONT_C16_OUTPUT_CHUNK_CELLS * TRANSPORT_CELL_BYTES, TRANSPORT_CELL_BYTES, 1],
    )


def _transfer_shard_cell_tap(frame_slot_bytes: int, shard: ColumnShardPlan, cell_row: int, local_cell_column: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    if cell_row < 0 or cell_row >= shard.cell_rows:
        raise ValueError("cell row must be inside the column shard")
    if local_cell_column < 0 or local_cell_column >= shard.cell_columns:
        raise ValueError("cell column must be inside the column shard")
    first_byte = shard.first_byte + cell_row * shard.lane_row_stride_bytes + local_cell_column * TRANSPORT_CELL_BYTES
    return TensorAccessPattern(
        (1, frame_slot_bytes),
        first_byte,
        [1, TRANSPORT_CELL_BYTES],
        [0, 1],
    )


def _resolve_iron_device(device_name: DeviceName):
    from aie.iron.device import NPU1, NPU2

    if device_name == "npu1":
        return NPU1()
    if device_name == "npu2":
        return NPU2()
    raise ValueError(f"unknown AIE device: {device_name}")


def build_cell_conveyor_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the first non-product MLIR-AIE graph phase.

    This phase exercises the new public cell-slot layout, per-column shard taps,
    memory-tile forwarding, and one placed compute worker per physical column. It
    is a graph envelope for placement/debugging; it is not product-complete.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(profile, device=device, model_resolution=model_resolution, lane_groups=lane_groups)
    selected_device = _resolve_iron_device(device)

    cell_ty = np.ndarray[(TRANSPORT_CELL_BYTES,), np.dtype[np.int8]]
    frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]

    copy_cell = Kernel(
        "apusr_middle_core_copy_c16_cell_i8",
        "apusr_middle_core_c16_cell_copy.o",
        [cell_ty, cell_ty, np.int32],
    )

    ingress_fifos = []
    worker_in_fifos = []
    worker_out_fifos = []
    egress_fifos = []
    workers = []

    def make_copy_worker(cell_count: int):
        def copy_worker(of_in, of_out, copy_fn):
            for _ in range_(cell_count):
                elem_in = of_in.acquire(1)
                elem_out = of_out.acquire(1)
                copy_fn(elem_in, elem_out, TRANSPORT_CELL_BYTES)
                of_in.release(1)
                of_out.release(1)

        return copy_worker

    for shard in plan.column_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        compute_tile = Tile(shard.physical_column, 2)

        ingress = ObjectFifo(cell_ty, name=f"{name}_ingress_l3l2", depth=2)
        worker_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_ingress_l2l1")
        worker_out = ObjectFifo(cell_ty, name=f"{name}_egress_l1l2", depth=2)
        egress = worker_out.cons().forward(tile=mem_tile, name=f"{name}_egress_l2l3")

        ingress_fifos.append(ingress)
        worker_in_fifos.append(worker_in)
        worker_out_fifos.append(worker_out)
        egress_fifos.append(egress)
        workers.append(
            Worker(
                make_copy_worker(shard.cell_count),
                [worker_in.cons(), worker_out.prod(), copy_cell],
                tile=compute_tile,
                stack_size=0x800,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, frame_ty, frame_ty) as (_param_arena, ingress_frame, egress_frame):
        # The debug conveyor does not consume resident parameters, but it keeps the
        # hot sequence ABI shaped like the product middle-core.
        runtime.start(*workers)
        for shard, fifo in zip(plan.column_shards, ingress_fifos, strict=True):
            runtime.fill(fifo.prod(), ingress_frame, _transfer_tap(plan.frame_slot_bytes, shard), tile=Tile(shard.physical_column, 0))
        for shard, fifo in zip(plan.column_shards, egress_fifos, strict=True):
            runtime.drain(fifo.cons(), egress_frame, _transfer_tap(plan.frame_slot_bytes, shard), wait=True, tile=Tile(shard.physical_column, 0))

    return Program(selected_device, runtime).resolve_program()




def build_c16_halo_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the first C16 halo-shaped graph phase.

    This phase keeps the public frame-slot ABI and column-sharded ingress/egress
    shape from the conveyor, but adds the first consumer-side halo object in the
    hot path. Each physical column builds an 18x10xC16 halo view from one 16x8xC16
    owned cell, then consumes that halo by copying the center owned region back to
    the egress cell. It validates the real object sizes and row-to-row compute
    placement used by the front C16 stage without pretending to run Stage1 math.
    """

    import numpy as np
    from aie.iron import Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="c16-halo",
    )
    selected_device = _resolve_iron_device(device)

    cell_bytes = CELL_SHAPES["c16_cell"].element_bytes
    halo_bytes = CELL_SHAPES["c16_halo_cell"].element_bytes
    cell_ty = np.ndarray[(cell_bytes,), np.dtype[np.int8]]
    halo_ty = np.ndarray[(halo_bytes,), np.dtype[np.int8]]
    frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]

    build_halo = Kernel(
        "apusr_middle_core_c16_cell_to_halo_i8",
        "apusr_middle_core_c16_cell_copy.o",
        [cell_ty, halo_ty],
    )
    consume_halo = Kernel(
        "apusr_middle_core_c16_halo_to_cell_i8",
        "apusr_middle_core_c16_cell_copy.o",
        [halo_ty, cell_ty],
    )

    ingress_fifos = []
    egress_fifos = []
    workers = []

    def make_halo_worker(cell_count: int):
        def halo_worker(of_in, of_halo, halo_fn):
            for _ in range_(cell_count):
                elem_in = of_in.acquire(1)
                elem_halo = of_halo.acquire(1)
                halo_fn(elem_in, elem_halo)
                of_in.release(1)
                of_halo.release(1)

        return halo_worker

    def make_consume_worker(cell_count: int):
        def consume_worker(of_halo, of_out, consume_fn):
            for _ in range_(cell_count):
                elem_halo = of_halo.acquire(1)
                elem_out = of_out.acquire(1)
                consume_fn(elem_halo, elem_out)
                of_halo.release(1)
                of_out.release(1)

        return consume_worker

    for shard in plan.column_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        halo_tile = Tile(shard.physical_column, 2)
        consume_tile = Tile(shard.physical_column, 3)

        ingress = ObjectFifo(cell_ty, name=f"{name}_ingress_l3l2", depth=2)
        owned_cell = ingress.cons().forward(tile=mem_tile, name=f"{name}_owned_cell_l2l1")
        halo_view = ObjectFifo(halo_ty, name=f"{name}_c16_halo_l1l1", depth=1)
        owned_out = ObjectFifo(cell_ty, name=f"{name}_owned_out_l1l2", depth=2)
        egress = owned_out.cons().forward(tile=mem_tile, name=f"{name}_egress_l2l3")

        ingress_fifos.append(ingress)
        egress_fifos.append(egress)
        workers.append(
            Worker(
                make_halo_worker(shard.cell_count),
                [owned_cell.cons(), halo_view.prod(), build_halo],
                tile=halo_tile,
                stack_size=0x800,
            )
        )
        workers.append(
            Worker(
                make_consume_worker(shard.cell_count),
                [halo_view.cons(), owned_out.prod(), consume_halo],
                tile=consume_tile,
                stack_size=0x800,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, frame_ty, frame_ty) as (_param_arena, ingress_frame, egress_frame):
        runtime.start(*workers)
        for shard, fifo in zip(plan.column_shards, ingress_fifos, strict=True):
            runtime.fill(fifo.prod(), ingress_frame, _transfer_tap(plan.frame_slot_bytes, shard), tile=Tile(shard.physical_column, 0))
        for shard, fifo in zip(plan.column_shards, egress_fifos, strict=True):
            runtime.drain(fifo.cons(), egress_frame, _transfer_tap(plan.frame_slot_bytes, shard), wait=True, tile=Tile(shard.physical_column, 0))

    return Program(selected_device, runtime).resolve_program()





def _front_c16_row_kernel_names(cell_columns: int) -> tuple[str, str]:
    if cell_columns not in (7, 8, 15, 30, 60):
        raise ValueError(
            f"front-c16-row graph supports 7/8/15/30/60-cell physical shards; got {cell_columns}. "
            "Use lane-groups=auto for the current npu1/npu2 spatial profiles."
        )
    return (
        f"apusr_middle_core_front_c16_expand_srs_to_c32_cell_{cell_columns}c_i8",
        f"apusr_middle_core_c32_contract_srs_to_c16_cell_{cell_columns}c_i8",
    )



def _front_expand_identity_params():
    import numpy as np

    params = np.zeros((FRONT_C16_PARAMETER_SLICE_BYTES,), dtype=np.int8)
    view = params.view(np.int32)
    view[0] = 0
    view[1] = 0

    spatial_weights_offset = 16
    spatial_bias_offset = 2320
    spatial_scale_offset = 2384
    spatial_weights = params[spatial_weights_offset : spatial_weights_offset + 2304]
    center_tap = 4
    for output_channel in range(16):
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = output_channel >> 3
        input_lane = output_channel & 7
        block_base = ((center_tap * 2 + output_chunk) * 2 + input_group) * 64
        spatial_weights[block_base + input_lane * 8 + output_lane] = 1
    params[spatial_bias_offset : spatial_bias_offset + 64].view(np.int32)[:] = 0
    params[spatial_scale_offset : spatial_scale_offset + 64].view(np.int32)[:] = 1

    expand_weights_offset = 2448
    expand_bias_offset = 2960
    expand_scale_offset = 3088
    weights = params[expand_weights_offset : expand_weights_offset + 512]
    for output_channel in range(32):
        input_channel = output_channel & 15
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 2) + input_group) * 64
        weights[block_base + input_lane * 8 + output_lane] = 1
    params[expand_bias_offset : expand_bias_offset + 128].view(np.int32)[:] = 0
    params[expand_scale_offset : expand_scale_offset + 128].view(np.int32)[:] = 1
    return params


def _fill_i32_m4x8(dst, offset, values):
    import numpy as np

    for chunk in range(len(values) // 8):
        for pixel in range(4):
            for lane in range(8):
                dst.view(np.int32)[(offset // 4) + chunk * 32 + pixel * 8 + lane] = int(values[chunk * 8 + lane])


def _stage1_hot_identity_params():
    import numpy as np

    compact = _stage1_identity_params_compact()
    hot = np.zeros((STAGE1_HOT_RECORD_BYTES,), dtype=np.int8)
    hot[0:16] = compact[0:16]

    hot_spatial_weights_offset = 16
    hot_spatial_bias_blocks_offset = 2320
    hot_spatial_scale_blocks_offset = 2576
    hot_expand_weights_offset = 2832
    hot_expand_bias_blocks_offset = 3344
    hot_expand_scale_blocks_offset = 3856
    hot_contract_weights_offset = 4368
    hot_contract_bias_blocks_offset = 4880
    hot_contract_scale_blocks_offset = 5136
    hot_residual_scale_blocks_offset = 5392

    hot[hot_spatial_weights_offset : hot_spatial_weights_offset + 2304] = compact[16 : 16 + 2304]
    _fill_i32_m4x8(hot, hot_spatial_bias_blocks_offset, compact[2320 : 2320 + 64].view(np.int32))
    _fill_i32_m4x8(hot, hot_spatial_scale_blocks_offset, compact[2384 : 2384 + 64].view(np.int32))

    hot[hot_expand_weights_offset : hot_expand_weights_offset + 512] = compact[2448 : 2448 + 512]
    _fill_i32_m4x8(hot, hot_expand_bias_blocks_offset, compact[2960 : 2960 + 128].view(np.int32))
    _fill_i32_m4x8(hot, hot_expand_scale_blocks_offset, compact[3088 : 3088 + 128].view(np.int32))

    hot[hot_contract_weights_offset : hot_contract_weights_offset + 512] = compact[3216 : 3216 + 512]
    _fill_i32_m4x8(hot, hot_contract_bias_blocks_offset, compact[3728 : 3728 + 64].view(np.int32))
    _fill_i32_m4x8(hot, hot_contract_scale_blocks_offset, compact[3792 : 3792 + 64].view(np.int32))
    _fill_i32_m4x8(hot, hot_residual_scale_blocks_offset, compact[3856 : 3856 + 64].view(np.int32))
    return hot


def _stage1_identity_params():
    return _stage1_hot_identity_params()


def _stage2_identity_params():
    return _stage1_hot_identity_params()


def _stage1_identity_params_compact():
    import numpy as np

    params = _front_expand_identity_params()
    header = params.view(np.int32)
    header[0] = 0
    header[1] = 0
    header[2] = 0
    header[3] = 0

    contract_weights_offset = 3216
    contract_bias_offset = 3728
    contract_scale_offset = 3792
    residual_scale_offset = 3856
    residual_shift_offset = 3920

    weights = params[contract_weights_offset : contract_weights_offset + 512]
    weights[:] = 0
    for output_channel in range(16):
        input_channel = output_channel
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 4) + input_group) * 64
        weights[block_base + input_lane * 8 + output_lane] = 1
    params[contract_bias_offset : contract_bias_offset + 64].view(np.int32)[:] = 0
    params[contract_scale_offset : contract_scale_offset + 64].view(np.int32)[:] = 1
    params[residual_scale_offset : residual_scale_offset + 64].view(np.int32)[:] = 0
    params[residual_shift_offset : residual_shift_offset + 64].view(np.int32)[:] = 0
    params[3984:4048] = 0
    return params


STAGE3_4_HOT_RECORD_BYTES = 13_712
STAGE3_4_FUSED_C16_WINDOW_ROWS = 20
STAGE3_4_FUSED_C16_WINDOW_COLUMNS = 40
STAGE3_4_FUSED_C16_WINDOW_BYTES = STAGE3_4_FUSED_C16_WINDOW_ROWS * STAGE3_4_FUSED_C16_WINDOW_COLUMNS * 16
STAGE2_C16_CONTEXT_ROWS = CELL_SHAPES["c16_halo_cell"].rows
STAGE2_C16_CONTEXT_COLUMNS = CELL_SHAPES["c16_halo_cell"].width
STAGE2_C16_CONTEXT_BYTES = CELL_SHAPES["c16_halo_cell"].element_bytes
STAGE2_C16_CONTEXT_WORDS = STAGE2_C16_CONTEXT_BYTES // DMA_HALO_TAP_WORD_BYTES
STAGE1_2_FUSED_SOURCE_ROWS = 12
STAGE1_2_FUSED_SOURCE_COLUMNS = 20
STAGE1_2_FUSED_SOURCE_BYTES = STAGE1_2_FUSED_SOURCE_ROWS * STAGE1_2_FUSED_SOURCE_COLUMNS * 16
STAGE1_2_FUSED_SOURCE_WORDS = STAGE1_2_FUSED_SOURCE_BYTES // DMA_HALO_TAP_WORD_BYTES
STAGE1_2_PAIRED_MACROBLOCK_SOURCE_ROWS = STAGE1_2_FUSED_SOURCE_ROWS + CELL_SHAPES["c16_cell"].rows
STAGE1_2_PAIRED_MACROBLOCK_SOURCE_COLUMNS = STAGE1_2_FUSED_SOURCE_COLUMNS + CELL_SHAPES["c16_cell"].width
STAGE1_2_PAIRED_MACROBLOCK_SOURCE_BYTES = STAGE1_2_PAIRED_MACROBLOCK_SOURCE_ROWS * STAGE1_2_PAIRED_MACROBLOCK_SOURCE_COLUMNS * 16
STAGE1_2_PAIRED_MACROBLOCK_SOURCE_WORDS = STAGE1_2_PAIRED_MACROBLOCK_SOURCE_BYTES // DMA_HALO_TAP_WORD_BYTES
STAGE1_2_PAIRED_MACROBLOCK_STAGE1_PATCH_ROWS = 18
STAGE1_2_PAIRED_MACROBLOCK_STAGE1_PATCH_GROUPS_PER_ROW = 10
STAGE1_2_PAIRED_MACROBLOCK_STAGE1_PATCH_BYTES = STAGE1_2_PAIRED_MACROBLOCK_STAGE1_PATCH_ROWS * STAGE1_2_PAIRED_MACROBLOCK_STAGE1_PATCH_GROUPS_PER_ROW * 2 * 32
STAGE2_C16_ROW_WINDOW_CELLS = 3
STAGE2_C16_ROW_WINDOW_BYTES = STAGE2_C16_ROW_WINDOW_CELLS * CELL_SHAPES["c16_cell"].element_bytes
STAGE2_C16_ROW_WINDOW_WORDS = STAGE2_C16_ROW_WINDOW_BYTES // DMA_HALO_TAP_WORD_BYTES


def _stage3_4_down_fnb32_identity_params():
    import numpy as np

    params = np.zeros((STAGE3_4_HOT_RECORD_BYTES,), dtype=np.int8)
    header = params.view(np.int32)
    header[0] = 0
    header[1] = 0
    header[2] = 0
    header[3] = 0

    down_weights_offset = 16
    down_bias_blocks_offset = 2064
    down_scale_blocks_offset = 2576
    fnb_spatial_weights_offset = 3088
    fnb_spatial_bias_blocks_offset = 5392
    fnb_spatial_scale_blocks_offset = 5648
    fnb_expand_weights_offset = 5904
    fnb_expand_bias_blocks_offset = 7952
    fnb_expand_scale_blocks_offset = 8976
    fnb_contract_weights_offset = 10000
    fnb_contract_bias_blocks_offset = 12048
    fnb_contract_scale_blocks_offset = 12560
    fnb_residual_scale_blocks_offset = 13072

    down_weights = params[down_weights_offset : down_weights_offset + 2048]
    center_tap = 0
    for output_channel in range(32):
        input_channel = output_channel & 15
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((center_tap * 4 + output_chunk) * 2 + input_group) * 64
        down_weights[block_base + input_lane * 8 + output_lane] = 1
    _fill_i32_m4x8(params, down_bias_blocks_offset, [0] * 32)
    _fill_i32_m4x8(params, down_scale_blocks_offset, [1] * 32)

    spatial_weights = params[fnb_spatial_weights_offset : fnb_spatial_weights_offset + 2304]
    center_spatial_tap = 4
    for output_channel in range(16):
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = output_channel >> 3
        input_lane = output_channel & 7
        block_base = ((center_spatial_tap * 2 + output_chunk) * 2 + input_group) * 64
        spatial_weights[block_base + input_lane * 8 + output_lane] = 1
    _fill_i32_m4x8(params, fnb_spatial_bias_blocks_offset, [0] * 16)
    _fill_i32_m4x8(params, fnb_spatial_scale_blocks_offset, [1] * 16)

    expand_weights = params[fnb_expand_weights_offset : fnb_expand_weights_offset + 2048]
    for output_channel in range(64):
        input_channel = output_channel & 31
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 4) + input_group) * 64
        expand_weights[block_base + input_lane * 8 + output_lane] = 1
    _fill_i32_m4x8(params, fnb_expand_bias_blocks_offset, [0] * 64)
    _fill_i32_m4x8(params, fnb_expand_scale_blocks_offset, [1] * 64)

    contract_weights = params[fnb_contract_weights_offset : fnb_contract_weights_offset + 2048]
    for output_channel in range(32):
        input_channel = output_channel
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 8) + input_group) * 64
        contract_weights[block_base + input_lane * 8 + output_lane] = 1
    _fill_i32_m4x8(params, fnb_contract_bias_blocks_offset, [0] * 32)
    _fill_i32_m4x8(params, fnb_contract_scale_blocks_offset, [1] * 32)
    _fill_i32_m4x8(params, fnb_residual_scale_blocks_offset, [0] * 32)
    return params


def _c32_to_c16_identity_params():
    import numpy as np

    param_bytes = 16 + 512 + 64 + 64
    params = np.zeros((param_bytes,), dtype=np.int8)
    params.view(np.int32)[0] = 0
    weights = params[16 : 16 + 512]
    for output_channel in range(16):
        input_channel = output_channel
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 4) + input_group) * 64
        weights[block_base + input_lane * 8 + output_lane] = 1
    params[16 + 512 : 16 + 512 + 64].view(np.int32)[:] = 0
    params[16 + 512 + 64 : 16 + 512 + 64 + 64].view(np.int32)[:] = 1
    return params


def _c32_to_c64_identity_params():
    import numpy as np

    params = np.zeros((ENCODER_C32_PARAMETER_SLICE_BYTES,), dtype=np.int8)
    params.view(np.int32)[0] = 0
    weights = params[16 : 16 + 2048]
    for output_channel in range(64):
        input_channel = output_channel & 31
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 4) + input_group) * 64
        weights[block_base + input_lane * 8 + output_lane] = 1
    params[16 + 2048 : 16 + 2048 + 256].view(np.int32)[:] = 0
    params[16 + 2048 + 256 : 16 + 2048 + 256 + 256].view(np.int32)[:] = 1
    return params


def _c64_to_c32_identity_params():
    import numpy as np

    param_bytes = 16 + 2048 + 128 + 128
    params = np.zeros((param_bytes,), dtype=np.int8)
    params.view(np.int32)[0] = 0
    weights = params[16 : 16 + 2048]
    for output_channel in range(32):
        input_channel = output_channel
        output_chunk = output_channel >> 3
        output_lane = output_channel & 7
        input_group = input_channel >> 3
        input_lane = input_channel & 7
        block_base = ((output_chunk * 8) + input_group) * 64
        weights[block_base + input_lane * 8 + output_lane] = 1
    params[16 + 2048 : 16 + 2048 + 128].view(np.int32)[:] = 0
    params[16 + 2048 + 128 : 16 + 2048 + 128 + 128].view(np.int32)[:] = 1
    return params


def build_front_c16_row_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the front C16 graph through the first encoder cell path.

    This diagnostic path moves C16 activation cells through the shim and drains C16
    snapshot cells back out. It is retained for isolation only. The primary
    spatial-core graph now exercises the Stage1+2 -> Stage3/4 carrier with the
    SkipA tee; this diagnostic remains a front-row primitive proof.

    This compile path uses tile-local resident parameter buffers for the first
    AIE-vector/SRS primitive. It does not stream parameters through the shim.
    C32/C64 traffic is cell-granular.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="front-c16-row",
    )
    selected_device = _resolve_iron_device(device)

    frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    c16_cell_ty = np.ndarray[(TRANSPORT_CELL_BYTES,), np.dtype[np.int8]]
    c32_cell_ty = np.ndarray[(CELL_SHAPES["c32_cell"].element_bytes,), np.dtype[np.int8]]
    front_param_ty = np.ndarray[(FRONT_C16_PARAMETER_SLICE_BYTES,), np.dtype[np.int8]]
    contract_param_ty = np.ndarray[(656,), np.dtype[np.int8]]

    ingress_fifos = []
    egress_cell_fifos = []
    egress_cell_shards = []
    workers = []
    kernel_cache = {}

    def get_kernel(name, object_file, arg_types):
        kernel = kernel_cache.get(name)
        if kernel is None:
            kernel = Kernel(name, object_file, arg_types)
            kernel_cache[name] = kernel
        return kernel

    def make_front_worker(cell_rows: int, cell_columns: int, edge_flags: int):
        def front_worker(of_c16_cell, zero_cell, front_params, of_c32_cell, front_fn):
            # Keep column iteration in AIE control flow. Static-unrolling this loop
            # bloats front-core program memory for no dataflow benefit.
            for _cell_col in range_(cell_columns):
                cells_pre = of_c16_cell.acquire(2)
                out_pre = of_c32_cell.acquire(1)
                front_fn(zero_cell, cells_pre[0], cells_pre[1], front_params, out_pre)
                of_c32_cell.release(1)

                for _ in range_(cell_rows - 2):
                    cells = of_c16_cell.acquire(3)
                    out = of_c32_cell.acquire(1)
                    front_fn(cells[0], cells[1], cells[2], front_params, out)
                    of_c32_cell.release(1)
                    of_c16_cell.release(1)

                cells_post = of_c16_cell.acquire(2)
                out_post = of_c32_cell.acquire(1)
                front_fn(cells_post[0], cells_post[1], zero_cell, front_params, out_post)
                of_c32_cell.release(1)
                of_c16_cell.release(2)

        return front_worker

    def make_encoder_snapshot_worker(cell_count: int):
        def encoder_snapshot_worker(of_c32_cell, contract_params, of_c16_cell_out, encoder_snapshot_fn):
            for _ in range_(cell_count):
                elem_c32 = of_c32_cell.acquire(1)
                elem_out = of_c16_cell_out.acquire(1)
                encoder_snapshot_fn(elem_c32, contract_params, elem_out)
                of_c32_cell.release(1)
                of_c16_cell_out.release(1)

        return encoder_snapshot_worker

    for shard in plan.column_shards:
        front_name, encoder_snapshot_name = _front_c16_row_kernel_names(shard.cell_columns)
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        front_tile = Tile(shard.physical_column, 3)
        encoder_tile = Tile(shard.physical_column, 4)

        front_c16_cell = get_kernel(
            front_name,
            "apusr_middle_core_vector_primitives.o",
            [c16_cell_ty, c16_cell_ty, c16_cell_ty, front_param_ty, c32_cell_ty],
        )
        encoder_snapshot_cell = get_kernel(
            encoder_snapshot_name,
            "apusr_middle_core_vector_primitives.o",
            [c32_cell_ty, contract_param_ty, c16_cell_ty],
        )

        ingress = ObjectFifo(c16_cell_ty, name=f"{name}_front_cell_ingress_l3l2", depth=4)
        cell_column_stream = ingress.cons().forward(tile=mem_tile, name=f"{name}_front_cell_column_l2l1", depth=4)
        c32_cells = ObjectFifo(c32_cell_ty, name=f"{name}_front_c32_cells_l1l1", depth=2)
        c16_cells_out = ObjectFifo(c16_cell_ty, name=f"{name}_encoder_snapshot_c16_cells_l1l2", depth=4)
        egress_cells = c16_cells_out.cons().forward(tile=mem_tile, name=f"{name}_front_cell_egress_l2l3", depth=4)
        front_params = Buffer(front_param_ty, name=f"{name}_front_params_l1", initial_value=_front_expand_identity_params())
        contract_params = Buffer(contract_param_ty, name=f"{name}_contract_params_l1", initial_value=_c32_to_c16_identity_params())
        zero_cell = Buffer(c16_cell_ty, name=f"{name}_front_zero_cell_l1", initial_value=np.zeros((TRANSPORT_CELL_BYTES,), dtype=np.int8))

        ingress_fifos.append(ingress)
        egress_cell_fifos.append(egress_cells)
        egress_cell_shards.append(shard)
        edge_flags = (1 if shard.owns_frame_left_edge else 0) | (2 if shard.owns_frame_right_edge else 0)
        workers.append(
            Worker(
                make_front_worker(shard.cell_rows, shard.cell_columns, edge_flags),
                [cell_column_stream.cons(), zero_cell, front_params, c32_cells.prod(), front_c16_cell],
                tile=front_tile,
                stack_size=0x800,
            )
        )
        workers.append(
            Worker(
                make_encoder_snapshot_worker(shard.cell_count),
                [c32_cells.cons(), contract_params, c16_cells_out.prod(), encoder_snapshot_cell],
                tile=encoder_tile,
                stack_size=0x800,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, frame_ty, frame_ty) as (_param_arena, ingress_frame, egress_frame):
        runtime.start(*workers)
        for shard, fifo in zip(plan.column_shards, ingress_fifos, strict=True):
            runtime.fill(
                fifo.prod(),
                ingress_frame,
                _transfer_shard_cell_column_major_tap(plan.frame_slot_bytes, shard),
                tile=Tile(shard.physical_column, 0),
            )
        for shard, fifo in zip(egress_cell_shards, egress_cell_fifos, strict=True):
            runtime.drain(
                fifo.cons(),
                egress_frame,
                _transfer_shard_cell_column_major_tap(plan.frame_slot_bytes, shard),
                wait=True,
                tile=Tile(shard.physical_column, 0),
            )

    return Program(selected_device, runtime).resolve_program()





def build_stage2_plane_window_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the compact Stage-2 producer-plane window proof.

    The failed three-input row-window experiment mapped top, center, and bottom
    producer rows onto separate ObjectFIFOs and exceeded the compute tile input
    DMA channel budget. The physical product contract is a row-major Stage-1
    C16 activation band/ring. Stage 2 consumes one compact 18x10xC16 plane
    window from that band, using the existing halo-patch primitive shape.
    """

    return build_stage2_resident_program(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
    )


def build_stage2_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the standalone Stage-2 resident proof graph.

    Stage 2 consumes explicit C16 halo patches from the Stage-1 producer
    context. The standalone compile carrier repeats one halo patch so the proof
    stays focused on tile pressure and DMA legality; the product contract is a
    producer-overlap row ring that supplies the same 18x10xC16 view.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage2-resident",
    )
    selected_device = _resolve_iron_device(device)
    stage2_shards = build_stage2_shards(plan)

    max_stage2_source_rows = max(
        (row_count for shard in stage2_shards for _, row_count in _stage2_window_row_blocks(shard)),
        default=1,
    )
    max_stage2_source_columns = max((shard.output_cell_columns for shard in stage2_shards), default=1)
    stage2_source_words = max_stage2_source_rows * max_stage2_source_columns * STAGE2_C16_CONTEXT_WORDS

    c16_ty = np.ndarray[(CELL_SHAPES["c16_cell"].element_bytes,), np.dtype[np.int8]]
    c16_halo_ty = np.ndarray[(STAGE2_C16_CONTEXT_WORDS,), np.dtype[np.uint64]]
    stage2_source_ty = np.ndarray[(stage2_source_words,), np.dtype[np.uint64]]
    frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage2_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage2 = Kernel(
        "apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_halo_ty, stage2_param_ty, c16_ty],
    )

    ingress_fifos = []
    egress_fifos = []
    workers = []

    def make_stage2_worker(cell_count: int):
        def stage2_worker(of_input_patch, stage2_params, of_c16_cell, stage2_fn):
            for _ in range_(cell_count):
                patch = of_input_patch.acquire(1)
                out = of_c16_cell.acquire(1)
                stage2_fn(patch, stage2_params, out)
                of_input_patch.release(1)
                of_c16_cell.release(1)

        return stage2_worker

    for shard in stage2_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        compute_tile = Tile(shard.physical_column, 2)

        ingress = ObjectFifo(c16_halo_ty, name=f"{name}_stage2_c16_halo_patch_l3l2", depth=1)
        worker_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_stage2_c16_halo_patch_l2l1", depth=1)
        stage2_out = ObjectFifo(c16_ty, name=f"{name}_stage2_c16_l1l2", depth=2)
        egress = stage2_out.cons().forward(tile=mem_tile, name=f"{name}_stage2_c16_l2l3", depth=2)
        stage2_params = Buffer(stage2_param_ty, name=f"{name}_stage2_params_l1", initial_value=_stage2_identity_params())

        ingress_fifos.append((shard, ingress))
        egress_fifos.append((shard, egress))
        workers.append(
            Worker(
                make_stage2_worker(shard.output_cell_count),
                [worker_in.cons(), stage2_params, stage2_out.prod(), stage2],
                tile=compute_tile,
                stack_size=0x1800,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, stage2_source_ty, frame_ty) as (_param_arena, c16_source_patch, c16_stage2_frame):
        # c16_source_patch is a standalone compile carrier for one row block of
        # explicit halo patches. Product Stage 2 should receive equivalent views
        # from the Stage-1 producer-overlap row ring, not synthesize 3x3 cell
        # slabs in tile memory.
        runtime.start(*workers)
        for shard, fifo in ingress_fifos:
            for row_start, row_count in _stage2_window_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    c16_source_patch,
                    _stage2_c16_halo_patch_row_block_tap(plan, shard, row_start, row_count, stage2_source_words),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in egress_fifos:
            for row_start, row_count in _stage2_window_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    c16_stage2_frame,
                    _stage2_c16_output_row_block_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )


    return Program(selected_device, runtime).resolve_program()


def build_stage1_2_linked_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the first linked Stage-1 -> Stage-2 resident graph.

    This phase removes the standalone Stage-2 DDR input from the proof path.
    Stage 1 still consumes the zero-guarded ingress halo patch and produces C16
    cells. A tile-local bridge converts each produced C16 cell into the current
    Stage-2 18x10 C16 carrier object, and Stage 2 consumes that carrier without
    another shim fill. The bridge is intentionally narrow: it proves tile-share
    dataflow and residency before the full producer row-ring tap replaces the
    center-only carrier.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage1_2-linked-resident",
    )
    selected_device = _resolve_iron_device(device)

    c16_ty = np.ndarray[(CELL_SHAPES["c16_cell"].element_bytes,), np.dtype[np.int8]]
    c16_halo_ty = np.ndarray[(C16_HALO_PATCH_WORDS,), np.dtype[np.uint64]]
    ingress_frame_ty = np.ndarray[(_front_halo_ingress_words(plan),), np.dtype[np.uint64]]
    egress_frame_ty = np.ndarray[(plan.frame_slot_bytes // DMA_HALO_TAP_WORD_BYTES,), np.dtype[np.uint64]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage1 = Kernel(
        "apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_halo_ty, stage_param_ty, c16_ty],
    )
    c16_to_stage2_halo = Kernel(
        "apusr_middle_core_c16_cell_to_halo_i8",
        "apusr_middle_core_c16_cell_copy.o",
        [c16_ty, c16_halo_ty],
    )
    stage2 = Kernel(
        "apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_halo_ty, stage_param_ty, c16_ty],
    )

    ingress_fifos = []
    egress_fifos = []
    workers = []

    def make_stage1_worker(cell_count: int):
        def stage1_worker(of_halo_patch, stage1_params, of_c16_cell, stage1_fn):
            for _ in range_(cell_count):
                patch = of_halo_patch.acquire(1)
                out = of_c16_cell.acquire(1)
                stage1_fn(patch, stage1_params, out)
                of_halo_patch.release(1)
                of_c16_cell.release(1)

        return stage1_worker

    def make_bridge_worker(cell_count: int):
        def bridge_worker(of_c16_cell, of_stage2_patch, bridge_fn):
            for _ in range_(cell_count):
                cell = of_c16_cell.acquire(1)
                patch = of_stage2_patch.acquire(1)
                bridge_fn(cell, patch)
                of_c16_cell.release(1)
                of_stage2_patch.release(1)

        return bridge_worker

    def make_stage2_worker(cell_count: int):
        def stage2_worker(of_stage2_patch, stage2_params, of_c16_cell, stage2_fn):
            for _ in range_(cell_count):
                patch = of_stage2_patch.acquire(1)
                out = of_c16_cell.acquire(1)
                stage2_fn(patch, stage2_params, out)
                of_stage2_patch.release(1)
                of_c16_cell.release(1)

        return stage2_worker

    for shard in plan.column_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        stage1_tile = Tile(shard.physical_column, 2)
        bridge_tile = Tile(shard.physical_column, 3)
        stage2_tile = Tile(shard.physical_column, 4)

        ingress = ObjectFifo(c16_halo_ty, name=f"{name}_stage1_2_ingress_halo_l3l2", depth=1)
        stage1_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_stage1_2_ingress_halo_l2l1", depth=1)
        stage1_out = ObjectFifo(c16_ty, name=f"{name}_stage1_2_stage1_c16_l1l1", depth=2)
        stage2_patch = ObjectFifo(c16_halo_ty, name=f"{name}_stage1_2_stage2_patch_l1l1", depth=1)
        stage2_out = ObjectFifo(c16_ty, name=f"{name}_stage1_2_stage2_c16_l1l2", depth=2)
        egress = stage2_out.cons().forward(tile=mem_tile, name=f"{name}_stage1_2_stage2_c16_l2l3", depth=2)
        stage1_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_stage1_params_l1", initial_value=_stage1_identity_params())
        stage2_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_stage2_params_l1", initial_value=_stage2_identity_params())

        ingress_fifos.append((shard, ingress))
        egress_fifos.append((shard, egress))
        workers.append(
            Worker(
                make_stage1_worker(shard.cell_count),
                [stage1_in.cons(), stage1_params, stage1_out.prod(), stage1],
                tile=stage1_tile,
                stack_size=0x1000,
            )
        )
        workers.append(
            Worker(
                make_bridge_worker(shard.cell_count),
                [stage1_out.cons(), stage2_patch.prod(), c16_to_stage2_halo],
                tile=bridge_tile,
                stack_size=0x800,
            )
        )
        workers.append(
            Worker(
                make_stage2_worker(shard.cell_count),
                [stage2_patch.cons(), stage2_params, stage2_out.prod(), stage2],
                tile=stage2_tile,
                stack_size=0x1800,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, egress_frame_ty) as (_param_arena, ingress_frame, egress_frame):
        runtime.start(*workers)
        for shard, fifo in ingress_fifos:
            for row_start, row_count in _halo_patch_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    ingress_frame,
                    _zero_guarded_halo_patch_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in egress_fifos:
            for row_start, row_count in _cell_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    egress_frame,
                    _m4x8_c16_cell_row_block_tap(plan, shard, row_start=row_start, row_count=row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()



def _make_stage1_worker(cell_count: int):
    from aie.iron.controlflow import range_

    def stage1_worker(of_halo_patch, stage1_params, of_c16_cell, stage1_fn):
        for _ in range_(cell_count):
            patch = of_halo_patch.acquire(1)
            out = of_c16_cell.acquire(1)
            stage1_fn(patch, stage1_params, out)
            of_halo_patch.release(1)
            of_c16_cell.release(1)

    return stage1_worker


def _make_stage2_worker(cell_count: int):
    from aie.iron.controlflow import range_

    def stage2_worker(of_input_patch, stage2_params, of_c16_cell, stage2_fn):
        for _ in range_(cell_count):
            patch = of_input_patch.acquire(1)
            out = of_c16_cell.acquire(1)
            stage2_fn(patch, stage2_params, out)
            of_input_patch.release(1)
            of_c16_cell.release(1)

    return stage2_worker


def _make_stage2_tee_worker(cell_count: int):
    from aie.iron.controlflow import range_

    def stage2_worker(of_input_patch, stage2_params, of_ring_cell, of_skipa_cell, stage2_fn):
        for _ in range_(cell_count):
            patch = of_input_patch.acquire(1)
            ring_out = of_ring_cell.acquire(1)
            skipa_out = of_skipa_cell.acquire(1)
            stage2_fn(patch, stage2_params, ring_out, skipa_out)
            of_input_patch.release(1)
            of_ring_cell.release(1)
            of_skipa_cell.release(1)

    return stage2_worker


def _make_stage3_4_worker(cell_count: int):
    from aie.iron.controlflow import range_

    def stage3_4_worker(of_input_window, stage3_4_params, of_c32_cell, stage3_4_fn):
        for _ in range_(cell_count):
            window = of_input_window.acquire(1)
            out = of_c32_cell.acquire(1)
            stage3_4_fn(window, stage3_4_params, out)
            of_input_window.release(1)
            of_c32_cell.release(1)

    return stage3_4_worker


def _stage1_2_fused_source_window20x12_row_block_tap(
    plan: GraphPhasePlan,
    shard: Stage2ShardPlan,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("stage1/2 fused source row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("stage1/2 fused source row block must stay inside the interior proof shard")

    halo_view_width = frame.seam_width + 2 * GUARD_PIXELS
    row_stride_words = halo_view_width * TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    pixel_stride_words = TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    patch_row_words = STAGE1_2_FUSED_SOURCE_COLUMNS * pixel_stride_words
    first_output_row = 1 + row_start
    first_pixel_x = shard.first_output_cell_column * CELL_SHAPES["c16_cell"].width - 1
    first_pixel_y = first_output_row * CELL_SHAPES["c16_cell"].rows - 1
    if first_pixel_x < 0 or first_pixel_y < 0:
        raise ValueError("stage1/2 fused source window needs an interior output cell")
    dimensions = [row_count, shard.output_cell_columns, STAGE1_2_FUSED_SOURCE_ROWS, patch_row_words]
    strides = [CELL_SHAPES["c16_cell"].rows * row_stride_words, CELL_SHAPES["c16_cell"].width * pixel_stride_words, row_stride_words, 1]
    _check_dma_bd_layout("stage1/2 fused 20x12 source window", dimensions, strides)
    first_word = (first_pixel_y * halo_view_width + first_pixel_x) * pixel_stride_words
    return TensorAccessPattern(
        (1, frame.front_ingress_slot_bytes // DMA_HALO_TAP_WORD_BYTES),
        first_word,
        dimensions,
        strides,
    )


def build_stage1_2_fused_overlap_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the Stage1+Stage2 expanded-footprint fusion proof.

    This proof deletes the Stage1->Stage2 boundary ring by computing the exact
    Stage1-domain 18x10 patch required by Stage2 from a 20x12 source-domain
    window inside one kernel invocation. It does not change the model weights or
    quantization contract; it only groups the Stage1 pixels that Stage2 needs
    into the same local evaluation footprint.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage1_2-fused-overlap-resident",
    )
    selected_device = _resolve_iron_device(device)
    stage2_shards = build_stage2_shards(plan)

    source_window_ty = np.ndarray[(STAGE1_2_FUSED_SOURCE_WORDS,), np.dtype[np.uint64]]
    c16_ty = np.ndarray[(CELL_SHAPES["c16_cell"].element_bytes,), np.dtype[np.int8]]
    ingress_frame_ty = np.ndarray[(_front_halo_ingress_words(plan),), np.dtype[np.uint64]]
    frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage1_2 = Kernel(
        "apusr_middle_core_stage1_2_fused_window20x12_to_c16_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [source_window_ty, stage_param_ty, stage_param_ty, c16_ty],
    )

    ingress_fifos = []
    egress_fifos = []
    workers = []

    def make_stage1_2_worker(cell_count: int):
        def stage1_2_worker(of_source_window, stage1_params, stage2_params, of_c16_cell, stage1_2_fn):
            for _ in range_(cell_count):
                window = of_source_window.acquire(1)
                out = of_c16_cell.acquire(1)
                stage1_2_fn(window, stage1_params, stage2_params, out)
                of_source_window.release(1)
                of_c16_cell.release(1)

        return stage1_2_worker

    for shard in stage2_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        compute_tile = Tile(shard.physical_column, 2)

        ingress = ObjectFifo(source_window_ty, name=f"{name}_stage1_2_fused_source20x12_l3l2", depth=1)
        worker_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_stage1_2_fused_source20x12_l2l1", depth=1)
        stage1_2_out = ObjectFifo(c16_ty, name=f"{name}_stage1_2_fused_c16_l1l2", depth=2)
        egress = stage1_2_out.cons().forward(tile=mem_tile, name=f"{name}_stage1_2_fused_c16_l2l3", depth=2)
        stage1_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_fused_stage1_params_l1", initial_value=_stage1_identity_params())
        stage2_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_fused_stage2_params_l1", initial_value=_stage2_identity_params())

        ingress_fifos.append((shard, ingress))
        egress_fifos.append((shard, egress))
        workers.append(
            Worker(
                make_stage1_2_worker(shard.output_cell_count),
                [worker_in.cons(), stage1_params, stage2_params, stage1_2_out.prod(), stage1_2],
                tile=compute_tile,
                stack_size=0x3000,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, frame_ty) as (_param_arena, ingress_frame, c16_stage2_frame):
        runtime.start(*workers)
        for shard, fifo in ingress_fifos:
            for row_start, row_count in _stage2_window_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    ingress_frame,
                    _stage1_2_fused_source_window20x12_row_block_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in egress_fifos:
            for row_start, row_count in _stage2_window_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    c16_stage2_frame,
                    _stage2_c16_output_row_block_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()


def _stage1_2_macroblock36x20_for_stage3_row_block_tap(
    plan: GraphPhasePlan,
    shard: DownsampleShardPlan,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("Stage1+2 grouped source row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("Stage1+2 grouped source row block must stay inside the Stage3 shard")

    halo_view_width = frame.seam_width + 2 * GUARD_PIXELS
    row_stride_words = halo_view_width * TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    pixel_stride_words = TRANSPORT_PIXEL_BYTES // DMA_HALO_TAP_WORD_BYTES
    patch_row_words = STAGE1_2_FUSED_SOURCE_COLUMNS * pixel_stride_words
    first_stage2_row = row_start * 2
    first_stage2_column = shard.first_output_cell_column * 2
    first_pixel_x = first_stage2_column * CELL_SHAPES["c16_cell"].width
    first_pixel_y = first_stage2_row * CELL_SHAPES["c16_cell"].rows

    patch_row_words = STAGE1_2_PAIRED_MACROBLOCK_SOURCE_COLUMNS * pixel_stride_words
    dimensions = [row_count, shard.output_cell_columns, STAGE1_2_PAIRED_MACROBLOCK_SOURCE_ROWS, patch_row_words]
    strides = [
        2 * CELL_SHAPES["c16_cell"].rows * row_stride_words,
        2 * CELL_SHAPES["c16_cell"].width * pixel_stride_words,
        row_stride_words,
        1,
    ]
    _check_dma_bd_layout("Stage1+2 paired 36x20 macroblock source windows", dimensions, strides)
    first_word = (first_pixel_y * halo_view_width + first_pixel_x) * pixel_stride_words
    return TensorAccessPattern(
        (1, frame.front_ingress_slot_bytes // DMA_HALO_TAP_WORD_BYTES),
        first_word,
        dimensions,
        strides,
    )


def _stage2_skipa_spill_macroblock2x2_raster_tap(
    plan: GraphPhasePlan,
    shard: DownsampleShardPlan,
    row_start: int,
    row_count: int,
):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("SkipA 2x2 raster spill row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("SkipA 2x2 raster spill row block must stay inside the Stage3 shard")

    word_bytes = 8
    if TRANSPORT_PIXEL_BYTES % word_bytes != 0:
        raise ValueError("C16 pixel byte size must be word-aligned for SkipA raster DMA")

    c16 = CELL_SHAPES["c16_cell"].channels
    cell_width = CELL_SHAPES["c16_cell"].width
    cell_rows = CELL_SHAPES["c16_cell"].rows
    macroblock_width_pixels = 2 * cell_width
    macroblock_rows = 2 * cell_rows
    raster_row_stride_words = (frame.seam_width * c16) // word_bytes
    macroblock_column_stride_words = (macroblock_width_pixels * c16) // word_bytes
    macroblock_row_words = macroblock_column_stride_words
    macroblock_row_stride_words = macroblock_rows * raster_row_stride_words

    first_stage2_cell_row = row_start * 2
    first_stage2_cell_column = shard.first_output_cell_column * 2
    first_pixel_y = first_stage2_cell_row * cell_rows
    first_pixel_x = first_stage2_cell_column * cell_width
    first_word = (
        SKIPA_SPILL_OFFSET_BYTES // word_bytes
        + (first_pixel_y * frame.seam_width + first_pixel_x) * (c16 // word_bytes)
    )

    # SkipA is stored as the logical C16 tensor raster, not cell-major object
    # order. The producer emits each 2x2 Stage2 macroblock as a 32x16 C16
    # raster sideband. The innermost BD span is one raster row of the 2x2
    # macroblock: 32 pixels * 16 channels = 512 bytes = 64 64-bit words. This
    # avoids the invalid encoded d0 size produced by a 4096-byte cell-pair span.
    dimensions = [row_count, shard.output_cell_columns, macroblock_rows, macroblock_row_words]
    strides = [
        macroblock_row_stride_words,
        macroblock_column_stride_words,
        raster_row_stride_words,
        1,
    ]
    _check_dma_bd_layout("Stage2 SkipA 2x2 raster spill", dimensions, strides)
    return TensorAccessPattern(
        (1, _skip_spill_arena_bytes(plan) // word_bytes),
        first_word,
        dimensions,
        strides,
    )

def _stage3_c32_interior_output_row_block_tap(plan: GraphPhasePlan, shard: DownsampleShardPlan, row_start: int, row_count: int):
    from aie.helpers.taplib.tap import TensorAccessPattern

    frame = build_frame_plan(plan.profile, device=plan.device, model_resolution=plan.capacity, lane_groups=plan.lane_groups)
    output_columns = _downsample_cell_columns(frame.cell_columns)
    if row_count <= 0 or row_count > HALO_TAP_MAX_BD_DIM_SIZE:
        raise ValueError("Stage3 interior output row block must fit the DMA BD row-count limit")
    if row_start < 0 or row_start + row_count > shard.output_cell_rows:
        raise ValueError("Stage3 interior output row block must stay inside the Stage3 shard")

    cell_bytes = CELL_SHAPES["c32_cell"].element_bytes
    chunk_count, chunk_bytes = _cell_byte_chunks(cell_bytes)
    first_byte = (row_start * output_columns + shard.first_output_cell_column) * cell_bytes
    dimensions = [row_count, shard.output_cell_columns, chunk_count, chunk_bytes]
    strides = [output_columns * cell_bytes, cell_bytes, chunk_bytes, 1]
    _check_dma_bd_dimensions("Stage3 interior c32 output row block", dimensions)
    return TensorAccessPattern(
        (1, _stage3_c32_output_frame_bytes(plan)),
        first_byte,
        dimensions,
        strides,
    )


def build_stage1_2_to_stage3_paired_macroblock_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit fused Stage1+2 -> compact 2x2 C16 Stage2 ring -> Stage3.

    Stage1+2 produces the full Stage2 C16 lattice exactly once, but in
    non-overlapping 2x2 macroblocks matched to Stage3 k2/s2 consumption. Each
    macroblock is sourced from one 36x20 C16 bounding window, tees its four C16
    cells to SkipA, and forwards the same compact 2x2 C16 object to Stage3. The primary path no longer materializes full
    Stage2 rows in compute-tile memory and no longer fuses Stage4 into a 20x40
    C16 mega-window.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage1_2-to-stage3-paired-macroblock-resident",
    )
    selected_device = _resolve_iron_device(device)
    stage3_shards = build_stage1_2_to_stage3_programmed_shards(plan)

    source_macroblock_ty = np.ndarray[(STAGE1_2_PAIRED_MACROBLOCK_SOURCE_WORDS,), np.dtype[np.uint64]]
    c16_window2x2_ty = np.ndarray[(4 * CELL_SHAPES["c16_cell"].element_bytes,), np.dtype[np.int8]]
    c16_window2x2_words_ty = np.ndarray[((4 * CELL_SHAPES["c16_cell"].element_bytes) // 8,), np.dtype[np.uint64]]
    c16_window2x2_raster_words_ty = np.ndarray[((4 * CELL_SHAPES["c16_cell"].element_bytes) // 8,), np.dtype[np.uint64]]
    c32_ty = np.ndarray[(CELL_SHAPES["c32_cell"].element_bytes,), np.dtype[np.int8]]
    ingress_frame_ty = np.ndarray[(_front_halo_ingress_words(plan),), np.dtype[np.uint64]]
    c32_egress_frame_ty = np.ndarray[(_stage3_c32_output_frame_bytes(plan),), np.dtype[np.int8]]
    skip_spill_arena_ty = np.ndarray[((_skip_spill_arena_bytes(plan) // 8),), np.dtype[np.uint64]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]
    stage3_4_param_ty = np.ndarray[(STAGE3_4_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage1_2_to_window = Kernel(
        "apusr_middle_core_stage1_2_paired_macroblock36x20_to_c16_window2x2_i8_shared_skipa",
        "apusr_middle_core_vector_primitives.o",
        [
            source_macroblock_ty,
            stage_param_ty,
            stage_param_ty,
            c16_window2x2_ty,
            c16_window2x2_raster_words_ty,
        ],
    )
    stage3_downsample = Kernel(
        "apusr_middle_core_stage3_downsample_window_to_c32_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_window2x2_ty, stage3_4_param_ty, c32_ty],
    )

    ingress_fifos = []
    skipa_fifos = []
    egress_fifos = []
    workers = []

    def make_stage1_2_group_worker(cell_count: int):
        def stage1_2_worker(
            of_source_macroblock,
            stage1_params,
            stage2_params,
            of_stage2_window,
            of_skipa_raster_macroblock,
            stage1_2_fn,
        ):
            for _ in range_(cell_count):
                source = of_source_macroblock.acquire(1)
                stage2_window = of_stage2_window.acquire(1)
                skipa_macroblock = of_skipa_raster_macroblock.acquire(1)
                stage1_2_fn(source, stage1_params, stage2_params, stage2_window, skipa_macroblock)
                of_source_macroblock.release(1)
                of_stage2_window.release(1)
                of_skipa_raster_macroblock.release(1)

        return stage1_2_worker

    def make_stage3_worker(cell_count: int):
        def stage3_worker(of_stage2_window, stage3_4_params, of_c32_cell, stage3_fn):
            for _ in range_(cell_count):
                window = of_stage2_window.acquire(1)
                out = of_c32_cell.acquire(1)
                stage3_fn(window, stage3_4_params, out)
                of_stage2_window.release(1)
                of_c32_cell.release(1)

        return stage3_worker

    for shard in stage3_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        stage1_2_tile = Tile(shard.physical_column, 2)
        stage3_tile = Tile(shard.physical_column, 3)

        ingress = ObjectFifo(source_macroblock_ty, name=f"{name}_stage1_2_source36x20_macroblock_l3l2", depth=1)
        stage1_2_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_stage1_2_source36x20_macroblock_l2l1", depth=1)
        # Stage2 -> Stage3 uses neighboring compute-tile shared memory, not a
        # memtile DMA hop. The adjacent row-2/row-3 tiles synchronize through the
        # ObjectFifo lock protocol while the object lives in shared tile memory.
        # SkipA uses one ordinary outgoing tile-DMA channel. The sideband is a
        # raster 2x2 macroblock: the same four Stage2 cells arranged as 32x16
        # C16 tensor rows. That keeps the innermost S2MM dimension to 512 bytes
        # instead of a 4096-byte cell-pair span, and it preserves one effective
        # macroblock of sideband occupancy in the producer tile.
        stage2_window_shared = ObjectFifo(c16_window2x2_ty, name=f"{name}_stage2_c16_2x2_macroblock_shared", depth=2)
        skipa_macroblock_l1l3 = ObjectFifo(c16_window2x2_raster_words_ty, name=f"{name}_skipa_c16_2x2_raster_l1l3", depth=1)
        stage3_out = ObjectFifo(c32_ty, name=f"{name}_stage3_c32_l1l2", depth=1)
        egress = stage3_out.cons().forward(tile=mem_tile, name=f"{name}_stage3_c32_l2l3", depth=2)
        stage1_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_stage1_params_l1", initial_value=_stage1_identity_params())
        stage2_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_stage2_params_l1", initial_value=_stage2_identity_params())
        stage3_4_params = Buffer(stage3_4_param_ty, name=f"{name}_stage3_params_l1", initial_value=_stage3_4_down_fnb32_identity_params())

        ingress_fifos.append((shard, ingress))
        skipa_fifos.append((shard, skipa_macroblock_l1l3))
        egress_fifos.append((shard, egress))
        workers.append(
            Worker(
                make_stage1_2_group_worker(shard.output_cell_count),
                [
                    stage1_2_in.cons(),
                    stage1_params,
                    stage2_params,
                    stage2_window_shared.prod(),
                    skipa_macroblock_l1l3.prod(),
                    stage1_2_to_window,
                ],
                tile=stage1_2_tile,
                stack_size=0x3000,
            )
        )
        workers.append(
            Worker(
                make_stage3_worker(shard.output_cell_count),
                [stage2_window_shared.cons(), stage3_4_params, stage3_out.prod(), stage3_downsample],
                tile=stage3_tile,
                stack_size=0x1800,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, c32_egress_frame_ty, skip_spill_arena_ty) as (
        _param_arena,
        ingress_frame,
        c32_egress_frame,
        skip_spill_arena,
    ):
        runtime.start(*workers)
        for shard, fifo in ingress_fifos:
            for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    ingress_frame,
                    _stage1_2_macroblock36x20_for_stage3_row_block_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        skip_spill_arena_words = skip_spill_arena
        for shard, fifo in skipa_fifos:
            for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    skip_spill_arena_words,
                    _stage2_skipa_spill_macroblock2x2_raster_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in egress_fifos:
            for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    c32_egress_frame,
                    _stage3_c32_interior_output_row_block_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()

def build_stage1_2_to_stage3_4_worker_ring_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the rejected Stage1+2 row-ring -> fused Stage3/4 diagnostic path.

    This carrier is retained for comparison only. The primary spatial-core path
    uses the paired Stage1+2 macroblock producer and split Stage3. Do not route
    product validation through this fused Stage3/4 row-ring carrier.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage1_2-to-stage3_4-worker-ring-resident",
    )
    selected_device = _resolve_iron_device(device)
    stage2_shards = build_stage2_shards(plan)
    fused_shards = build_stage3_4_fused_shards(plan)
    if len(stage2_shards) != len(fused_shards):
        raise ValueError("Stage1+2 and Stage3/4 shard counts must match for the primary row-ring graph")

    source_window_ty = np.ndarray[(STAGE1_2_FUSED_SOURCE_WORDS,), np.dtype[np.uint64]]
    c32_ty = np.ndarray[(CELL_SHAPES["c32_cell"].element_bytes,), np.dtype[np.int8]]
    ingress_frame_ty = np.ndarray[(_front_halo_ingress_words(plan),), np.dtype[np.uint64]]
    c32_egress_frame_ty = np.ndarray[(_stage3_c32_output_frame_bytes(plan),), np.dtype[np.int8]]
    skip_spill_arena_ty = np.ndarray[(_skip_spill_arena_bytes(plan),), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]
    stage3_4_param_ty = np.ndarray[(STAGE3_4_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage2_row_cells = max(shard.output_cell_columns for shard in stage2_shards)
    stage2_row_bytes = stage2_row_cells * CELL_SHAPES["c16_cell"].element_bytes
    c16_stage2_row_ty = np.ndarray[(stage2_row_bytes,), np.dtype[np.int8]]

    stage1_2_row = Kernel(
        "apusr_middle_core_stage1_2_fused_window20x12_to_c16_row_i8_tee",
        "apusr_middle_core_vector_primitives.o",
        [source_window_ty, stage_param_ty, stage_param_ty, c16_stage2_row_ty, c16_stage2_row_ty, np.int32],
    )
    stage3_4_from_rows = Kernel(
        "apusr_middle_core_stage3_4_fused_stage2_rows_to_c32_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_stage2_row_ty, c16_stage2_row_ty, c16_stage2_row_ty, c16_stage2_row_ty, stage3_4_param_ty, c32_ty, np.int32],
    )

    stage1_2_ingress_fifos = []
    skipa_row_fifos = []
    stage3_4_egress_fifos = []
    workers = []

    def make_stage1_2_row_worker(row_count: int, column_count: int):
        def stage1_2_worker(of_source_window, stage1_params, stage2_params, of_stage2_row, of_skipa_row, stage1_2_fn):
            for _ in range_(row_count):
                stage2_row = of_stage2_row.acquire(1)
                skipa_row = of_skipa_row.acquire(1)
                for column_index in range(column_count):
                    window = of_source_window.acquire(1)
                    stage1_2_fn(window, stage1_params, stage2_params, stage2_row, skipa_row, column_index)
                    of_source_window.release(1)
                of_stage2_row.release(1)
                of_skipa_row.release(1)

        return stage1_2_worker

    def make_stage3_4_row_worker(output_row_count: int, output_column_count: int):
        def stage3_4_worker(of_stage2_rows, stage3_4_params, of_c32_cell, stage3_4_fn):
            for _ in range_(output_row_count):
                rows = of_stage2_rows.acquire(4)
                for column_index in range(output_column_count):
                    out = of_c32_cell.acquire(1)
                    stage3_4_fn(rows[0], rows[1], rows[2], rows[3], stage3_4_params, out, column_index)
                    of_c32_cell.release(1)
                of_stage2_rows.release(2)
            of_stage2_rows.release(2)

        return stage3_4_worker

    for stage2_shard, fused_shard in zip(stage2_shards, fused_shards, strict=True):
        name = f"l{stage2_shard.lane_index}_c{stage2_shard.macro_column_index}"
        mem_tile = Tile(stage2_shard.physical_column, 1)
        stage1_2_tile = Tile(stage2_shard.physical_column, 2)
        stage3_4_tile = Tile(stage2_shard.physical_column, 3)

        ingress = ObjectFifo(source_window_ty, name=f"{name}_stage1_2_source20x12_l3l2", depth=1)
        stage1_2_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_stage1_2_source20x12_l2l1", depth=1)
        stage2_row_l1l2 = ObjectFifo(c16_stage2_row_ty, name=f"{name}_stage2_row_ring_producer_l1l2", depth=1)
        skipa_row_l1l2 = ObjectFifo(c16_stage2_row_ty, name=f"{name}_skipa_row_producer_l1l2", depth=1)
        stage2_row_ring = stage2_row_l1l2.cons().forward(tile=mem_tile, name=f"{name}_stage2_c16_row_ring_l2l1", depth=8)
        skipa_spill = skipa_row_l1l2.cons().forward(tile=mem_tile, name=f"{name}_skipa_c16_row_l2l3", depth=2)
        stage3_4_out = ObjectFifo(c32_ty, name=f"{name}_stage3_4_c32_l1l2", depth=1)
        egress = stage3_4_out.cons().forward(tile=mem_tile, name=f"{name}_stage3_4_c32_l2l3", depth=2)
        stage1_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_stage1_params_l1", initial_value=_stage1_identity_params())
        stage2_params = Buffer(stage_param_ty, name=f"{name}_stage1_2_stage2_params_l1", initial_value=_stage2_identity_params())
        stage3_4_params = Buffer(stage3_4_param_ty, name=f"{name}_stage3_4_params_l1", initial_value=_stage3_4_down_fnb32_identity_params())

        stage1_2_ingress_fifos.append((stage2_shard, ingress))
        skipa_row_fifos.append((stage2_shard, skipa_spill))
        stage3_4_egress_fifos.append((fused_shard, egress))
        workers.append(
            Worker(
                make_stage1_2_row_worker(stage2_shard.output_cell_rows, stage2_shard.output_cell_columns),
                [stage1_2_in.cons(), stage1_params, stage2_params, stage2_row_l1l2.prod(), skipa_row_l1l2.prod(), stage1_2_row],
                tile=stage1_2_tile,
                stack_size=0x3000,
            )
        )
        workers.append(
            Worker(
                make_stage3_4_row_worker(fused_shard.output_cell_rows, fused_shard.output_cell_columns),
                [stage2_row_ring.cons(), stage3_4_params, stage3_4_out.prod(), stage3_4_from_rows],
                tile=stage3_4_tile,
                stack_size=0x6000,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, c32_egress_frame_ty, skip_spill_arena_ty) as (
        _param_arena,
        ingress_frame,
        c32_egress_frame,
        skip_spill_arena,
    ):
        runtime.start(*workers)
        for shard, fifo in stage1_2_ingress_fifos:
            for row_start, row_count in _stage2_window_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    ingress_frame,
                    _stage1_2_fused_source_window20x12_row_block_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in skipa_row_fifos:
            for row_start in range(shard.output_cell_rows):
                runtime.drain(
                    fifo.cons(),
                    skip_spill_arena,
                    _stage2_skipa_spill_row_block_tap(plan, shard, row_start, 1),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in stage3_4_egress_fifos:
            for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    c32_egress_frame,
                    _stage3_4_c32_output_row_block_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()

def build_stage1_2_worker_ring_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the Stage1 -> worker-local C16 ring -> Stage2 compile carrier.

    The product shape is a memory-tile row-major ring local to each independent
    4-column worker. IRON cannot yet express the final rolling memtile-resident
    ring in this proof, so this graph uses a sequence buffer as a compile carrier:
    Stage 1 drains row-major C16 cells into the carrier, and Stage 2 reads compact
    18x10xC16 producer-plane windows from the same carrier. This deliberately
    removes the scalar bridge and validates the physical tensor window contract.
    """

    return _build_worker_ring_resident_program(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        include_stage3_4=False,
    )


def build_stage1_4_worker_ring_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the Stage1 -> Stage2 -> Stage3/4 worker-ring compile carrier."""

    return _build_worker_ring_resident_program(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        include_stage3_4=True,
    )


def _build_worker_ring_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName,
    model_resolution: ResolutionName,
    lane_groups: str | int,
    include_stage3_4: bool,
):
    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.device import Tile

    graph_phase = "stage1_4-worker-ring-resident" if include_stage3_4 else "stage1_2-worker-ring-resident"
    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase=graph_phase,
    )
    selected_device = _resolve_iron_device(device)
    stage2_shards = build_stage2_shards(plan)
    fused_shards = build_stage3_4_fused_shards(plan) if include_stage3_4 else ()

    c16_ty = np.ndarray[(CELL_SHAPES["c16_cell"].element_bytes,), np.dtype[np.int8]]
    c16_halo_ty = np.ndarray[(C16_HALO_PATCH_WORDS,), np.dtype[np.uint64]]
    c16_stage2_window_ty = np.ndarray[(STAGE2_C16_CONTEXT_WORDS,), np.dtype[np.uint64]]
    c16_stage3_4_window_ty = np.ndarray[(STAGE3_4_FUSED_C16_WINDOW_BYTES,), np.dtype[np.int8]]
    c32_ty = np.ndarray[(CELL_SHAPES["c32_cell"].element_bytes,), np.dtype[np.int8]]
    ingress_frame_ty = np.ndarray[(_front_halo_ingress_words(plan),), np.dtype[np.uint64]]
    c16_stage1_ring_ty = np.ndarray[(plan.frame_slot_bytes // DMA_HALO_TAP_WORD_BYTES,), np.dtype[np.uint64]]
    c16_stage2_ring_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    c32_egress_frame_ty = np.ndarray[(_stage3_c32_output_frame_bytes(plan),), np.dtype[np.int8]]
    skip_spill_arena_ty = np.ndarray[(_skip_spill_arena_bytes(plan),), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]
    stage3_4_param_ty = np.ndarray[(STAGE3_4_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage1 = Kernel(
        "apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_halo_ty, stage_param_ty, c16_ty],
    )
    stage2 = Kernel(
        "apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8_tee",
        "apusr_middle_core_vector_primitives.o",
        [c16_stage2_window_ty, stage_param_ty, c16_ty, c16_ty],
    )
    stage3_4_fused = Kernel(
        "apusr_middle_core_stage3_4_fused_window20x40_to_c32_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_stage3_4_window_ty, stage3_4_param_ty, c32_ty],
    )

    stage1_ingress_fifos = []
    stage1_ring_fifos = []
    stage2_ingress_fifos = []
    stage2_ring_fifos = []
    stage2_skipa_spill_fifos = []
    stage3_4_ingress_fifos = []
    stage3_4_egress_fifos = []
    workers = []

    for shard in plan.column_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        stage1_tile = Tile(shard.physical_column, 2)

        ingress = ObjectFifo(c16_halo_ty, name=f"{name}_s12ring_ingress_halo_l3l2", depth=1)
        stage1_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_s12ring_ingress_halo_l2l1", depth=1)
        stage1_out = ObjectFifo(c16_ty, name=f"{name}_s12ring_stage1_c16_l1l2", depth=2)
        stage1_ring = stage1_out.cons().forward(tile=mem_tile, name=f"{name}_s12ring_stage1_c16_l2l3", depth=2)
        stage1_params = Buffer(stage_param_ty, name=f"{name}_s12ring_stage1_params_l1", initial_value=_stage1_identity_params())

        stage1_ingress_fifos.append((shard, ingress))
        stage1_ring_fifos.append((shard, stage1_ring))
        workers.append(
            Worker(
                _make_stage1_worker(shard.cell_count),
                [stage1_in.cons(), stage1_params, stage1_out.prod(), stage1],
                tile=stage1_tile,
                stack_size=0x1000,
            )
        )

    for shard in stage2_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        stage2_tile = Tile(shard.physical_column, 3)

        ingress = ObjectFifo(c16_stage2_window_ty, name=f"{name}_s12ring_stage2_window_l3l2", depth=1)
        stage2_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_s12ring_stage2_window_l2l1", depth=1)
        stage2_ring_out = ObjectFifo(c16_ty, name=f"{name}_s12ring_stage2_ring_c16_l1l2", depth=2)
        stage2_skipa_out = ObjectFifo(c16_ty, name=f"{name}_s12ring_stage2_skipa_c16_l1l2", depth=2)
        stage2_ring = stage2_ring_out.cons().forward(tile=mem_tile, name=f"{name}_s12ring_stage2_c16_l2l3", depth=2)
        skipa_spill = stage2_skipa_out.cons().forward(tile=mem_tile, name=f"{name}_s12ring_skipa_spill_c16_l2l3", depth=2)
        stage2_params = Buffer(stage_param_ty, name=f"{name}_s12ring_stage2_params_l1", initial_value=_stage2_identity_params())

        stage2_ingress_fifos.append((shard, ingress))
        stage2_ring_fifos.append((shard, stage2_ring))
        stage2_skipa_spill_fifos.append((shard, skipa_spill))
        workers.append(
            Worker(
                _make_stage2_tee_worker(shard.output_cell_count),
                [stage2_in.cons(), stage2_params, stage2_ring_out.prod(), stage2_skipa_out.prod(), stage2],
                tile=stage2_tile,
                stack_size=0x1800,
            )
        )

    if include_stage3_4:
        for shard in fused_shards:
            name = f"l{shard.lane_index}_c{shard.macro_column_index}"
            mem_tile = Tile(shard.physical_column, 1)
            stage3_4_tile = Tile(shard.physical_column, 4)

            ingress = ObjectFifo(c16_stage3_4_window_ty, name=f"{name}_s1_4ring_stage3_4_window_l3l2", depth=1)
            stage3_4_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_s1_4ring_stage3_4_window_l2l1", depth=1)
            stage3_4_out = ObjectFifo(c32_ty, name=f"{name}_s1_4ring_stage3_4_c32_l1l2", depth=1)
            egress = stage3_4_out.cons().forward(tile=mem_tile, name=f"{name}_s1_4ring_stage3_4_c32_l2l3", depth=2)
            stage3_4_params = Buffer(stage3_4_param_ty, name=f"{name}_s1_4ring_stage3_4_params_l1", initial_value=_stage3_4_down_fnb32_identity_params())

            stage3_4_ingress_fifos.append((shard, ingress))
            stage3_4_egress_fifos.append((shard, egress))
            workers.append(
                Worker(
                    _make_stage3_4_worker(shard.output_cell_count),
                    [stage3_4_in.cons(), stage3_4_params, stage3_4_out.prod(), stage3_4_fused],
                    tile=stage3_4_tile,
                    stack_size=0x2800,
                )
            )

    runtime = Runtime()
    if include_stage3_4:
        with runtime.sequence(param_arena_ty, ingress_frame_ty, c32_egress_frame_ty, skip_spill_arena_ty, c16_stage1_ring_ty, c16_stage2_ring_ty) as (
            _param_arena,
            ingress_frame,
            c32_egress_frame,
            skip_spill_arena,
            c16_stage1_ring,
            c16_stage2_ring,
        ):
            runtime.start(*workers)
            for shard, fifo in stage1_ingress_fifos:
                for row_start, row_count in _halo_patch_row_blocks(shard):
                    runtime.fill(fifo.prod(), ingress_frame, _zero_guarded_halo_patch_tap(plan, shard, row_start, row_count), tile=Tile(shard.physical_column, 0))
            for shard, fifo in stage1_ring_fifos:
                for row_start, row_count in _cell_row_blocks(shard):
                    runtime.drain(fifo.cons(), c16_stage1_ring, _m4x8_c16_cell_row_block_tap(plan, shard, row_start=row_start, row_count=row_count), wait=True, tile=Tile(shard.physical_column, 0))
            for shard, fifo in stage2_ingress_fifos:
                for row_start, row_count in _stage2_window_row_blocks(shard):
                    runtime.fill(fifo.prod(), c16_stage1_ring, _stage2_c16_plane_window_row_block_tap(plan, shard, row_start, row_count, 0), tile=Tile(shard.physical_column, 0))
            for (shard, ring_fifo), (_, skipa_fifo) in zip(stage2_ring_fifos, stage2_skipa_spill_fifos, strict=True):
                for row_start, row_count in _stage2_window_row_blocks(shard):
                    runtime.drain(ring_fifo.cons(), c16_stage2_ring, _stage2_c16_output_row_block_tap(plan, shard, row_start, row_count), wait=False, tile=Tile(shard.physical_column, 0))
                    runtime.drain(skipa_fifo.cons(), skip_spill_arena, _stage2_skipa_spill_row_block_tap(plan, shard, row_start, row_count), wait=True, tile=Tile(shard.physical_column, 0))
            for shard, fifo in stage3_4_ingress_fifos:
                for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                    runtime.fill(fifo.prod(), c16_stage2_ring, _stage3_4_c16_input_window20x40_row_block_tap(plan, shard, row_start, row_count), tile=Tile(shard.physical_column, 0))
            for shard, fifo in stage3_4_egress_fifos:
                for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                    runtime.drain(fifo.cons(), c32_egress_frame, _stage3_4_c32_output_row_block_tap(plan, shard, row_start, row_count), wait=True, tile=Tile(shard.physical_column, 0))
    else:
        with runtime.sequence(param_arena_ty, ingress_frame_ty, skip_spill_arena_ty, c16_stage1_ring_ty, c16_stage2_ring_ty) as (
            _param_arena,
            ingress_frame,
            skip_spill_arena,
            c16_stage1_ring,
            c16_stage2_ring,
        ):
            runtime.start(*workers)
            for shard, fifo in stage1_ingress_fifos:
                for row_start, row_count in _halo_patch_row_blocks(shard):
                    runtime.fill(fifo.prod(), ingress_frame, _zero_guarded_halo_patch_tap(plan, shard, row_start, row_count), tile=Tile(shard.physical_column, 0))
            for shard, fifo in stage1_ring_fifos:
                for row_start, row_count in _cell_row_blocks(shard):
                    runtime.drain(fifo.cons(), c16_stage1_ring, _m4x8_c16_cell_row_block_tap(plan, shard, row_start=row_start, row_count=row_count), wait=True, tile=Tile(shard.physical_column, 0))
            for shard, fifo in stage2_ingress_fifos:
                for row_start, row_count in _stage2_window_row_blocks(shard):
                    runtime.fill(fifo.prod(), c16_stage1_ring, _stage2_c16_plane_window_row_block_tap(plan, shard, row_start, row_count, 0), tile=Tile(shard.physical_column, 0))
            for (shard, ring_fifo), (_, skipa_fifo) in zip(stage2_ring_fifos, stage2_skipa_spill_fifos, strict=True):
                for row_start, row_count in _stage2_window_row_blocks(shard):
                    runtime.drain(ring_fifo.cons(), c16_stage2_ring, _stage2_c16_output_row_block_tap(plan, shard, row_start, row_count), wait=False, tile=Tile(shard.physical_column, 0))
                    runtime.drain(skipa_fifo.cons(), skip_spill_arena, _stage2_skipa_spill_row_block_tap(plan, shard, row_start, row_count), wait=True, tile=Tile(shard.physical_column, 0))

    return Program(selected_device, runtime).resolve_program()

def build_stage3_4_fused_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
    product_contract: bool = False,
):
    """Emit the fused Stage 3/4 resident graph.

    The compile carrier feeds each worker a minimal 20x40 C16 pixel window: the
    exact one-pixel C32 context needed after 2x downsampling, not a full 4x4
    C16 cell slab. The product contract treats that window as a view supplied
    by the upstream C16 producer row ring. The standalone proof may still bind
    a row-major source buffer so aiecc can pressure-test the fused kernel before
    Stage 2 is wired into the same graph.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage3_4-product-resident" if product_contract else "stage3_4-fused-resident",
    )
    selected_device = _resolve_iron_device(device)
    fused_shards = build_stage3_4_fused_shards(plan)

    c16_window_ty = np.ndarray[(STAGE3_4_FUSED_C16_WINDOW_BYTES,), np.dtype[np.int8]]
    c32_ty = np.ndarray[(CELL_SHAPES["c32_cell"].element_bytes,), np.dtype[np.int8]]
    ingress_frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    egress_frame_ty = np.ndarray[(_stage3_c32_output_frame_bytes(plan),), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage3_4_param_ty = np.ndarray[(STAGE3_4_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage3_4_fused = Kernel(
        "apusr_middle_core_stage3_4_fused_window20x40_to_c32_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_window_ty, stage3_4_param_ty, c32_ty],
    )

    source_fifo_name = "stage3_4_c16_producer_window20x40" if product_contract else "stage3_4_c16_window20x40"

    ingress_fifos = []
    egress_fifos = []
    workers = []

    def make_stage3_4_worker(cell_count: int):
        def stage3_4_worker(of_input_window, stage3_4_params, of_c32_cell, stage3_4_fn):
            for _ in range_(cell_count):
                window = of_input_window.acquire(1)
                out = of_c32_cell.acquire(1)
                stage3_4_fn(window, stage3_4_params, out)
                of_input_window.release(1)
                of_c32_cell.release(1)

        return stage3_4_worker

    for shard in fused_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        compute_tile = Tile(shard.physical_column, 3)

        ingress = ObjectFifo(c16_window_ty, name=f"{name}_{source_fifo_name}_l3l2", depth=1)
        worker_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_{source_fifo_name}_l2l1", depth=1)
        stage3_4_out = ObjectFifo(c32_ty, name=f"{name}_stage3_4_c32_l1l2", depth=1)
        egress = stage3_4_out.cons().forward(tile=mem_tile, name=f"{name}_stage3_4_c32_l2l3", depth=2)
        stage3_4_params = Buffer(stage3_4_param_ty, name=f"{name}_stage3_4_params_l1", initial_value=_stage3_4_down_fnb32_identity_params())

        ingress_fifos.append((shard, ingress))
        egress_fifos.append((shard, egress))
        workers.append(
            Worker(
                make_stage3_4_worker(shard.output_cell_count),
                [worker_in.cons(), stage3_4_params, stage3_4_out.prod(), stage3_4_fused],
                tile=compute_tile,
                stack_size=0x2400,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, egress_frame_ty) as (_param_arena, c16_source_frame, egress_frame):
        # In product form, c16_source_frame is the Stage-2 producer-overlap row
        # ring exposed to this graph phase. In the standalone compile proof it
        # remains a row-major C16 carrier so the fused body can be audited before
        # the Stage-2 producer is linked.
        runtime.start(*workers)
        for shard, fifo in ingress_fifos:
            for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    c16_source_frame,
                    _stage3_4_c16_input_window20x40_row_block_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in egress_fifos:
            for row_start, row_count in _stage3_paired_macroblock_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    egress_frame,
                    _stage3_4_c32_output_row_block_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()


def build_stage3_4_product_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the Stage 3/4 fused product-contract graph.

    This uses the same kernel body as the pressure proof, but the graph phase is
    named and checked as an upstream C16 producer-overlap consumer. It is the
    contract Stage 2 must satisfy when it is wired in: provide the 20x40 C16
    context window from its row ring instead of asking Stage 3/4 to synthesize
    interior halos from seams.
    """

    return build_stage3_4_fused_resident_program(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        product_contract=True,
    )


def build_stage3_down_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the first C16->C32 transition proof.

    The proof keeps the resident-record policy general: Stage 3 parameters are
    tile-local and hot-record shaped, while the frame sequence binds the arena
    only to preserve the product ABI. The graph uses tensor-DMA row-block taps
    for 2x2 C16 cell windows, not per-cell DMA tasks.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage3-down-resident",
    )
    selected_device = _resolve_iron_device(device)
    downsample_shards = build_downsample_shards(plan)

    c16_window_bytes = 4 * CELL_SHAPES["c16_cell"].element_bytes
    c32_bytes = CELL_SHAPES["c32_cell"].element_bytes
    c16_window_ty = np.ndarray[(c16_window_bytes,), np.dtype[np.int8]]
    c32_ty = np.ndarray[(c32_bytes,), np.dtype[np.int8]]
    ingress_frame_ty = np.ndarray[(plan.frame_slot_bytes,), np.dtype[np.int8]]
    egress_frame_ty = np.ndarray[(_stage3_c32_output_frame_bytes(plan),), np.dtype[np.int8]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage3_4_param_ty = np.ndarray[(STAGE3_4_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage3_downsample = Kernel(
        "apusr_middle_core_stage3_downsample_window_to_c32_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_window_ty, stage3_4_param_ty, c32_ty],
    )

    ingress_fifos = []
    egress_fifos = []
    workers = []

    def make_stage3_worker(cell_count: int):
        def stage3_worker(of_input_window, stage3_4_params, of_c32_cell, stage3_fn):
            for _ in range_(cell_count):
                window = of_input_window.acquire(1)
                out = of_c32_cell.acquire(1)
                stage3_fn(window, stage3_4_params, out)
                of_input_window.release(1)
                of_c32_cell.release(1)

        return stage3_worker

    for shard in downsample_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        compute_tile = Tile(shard.physical_column, 3)

        ingress = ObjectFifo(c16_window_ty, name=f"{name}_stage3_c16_window_l3l2", depth=1)
        worker_in = ingress.cons().forward(tile=mem_tile, name=f"{name}_stage3_c16_window_l2l1", depth=1)
        stage3_out = ObjectFifo(c32_ty, name=f"{name}_stage3_c32_l1l2", depth=2)
        egress = stage3_out.cons().forward(tile=mem_tile, name=f"{name}_stage3_c32_l2l3", depth=2)
        stage3_4_params = Buffer(stage3_4_param_ty, name=f"{name}_stage3_4_params_l1", initial_value=_stage3_4_down_fnb32_identity_params())

        ingress_fifos.append((shard, ingress))
        egress_fifos.append((shard, egress))
        workers.append(
            Worker(
                make_stage3_worker(shard.output_cell_count),
                [worker_in.cons(), stage3_4_params, stage3_out.prod(), stage3_downsample],
                tile=compute_tile,
                stack_size=0x1000,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, egress_frame_ty) as (_param_arena, ingress_frame, egress_frame):
        runtime.start(*workers)
        for shard, fifo in ingress_fifos:
            for row_start, row_count in _stage3_window_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    ingress_frame,
                    _stage3_c16_input_window_row_block_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in egress_fifos:
            for row_start, row_count in _stage3_window_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    egress_frame,
                    _stage3_c32_output_row_block_tap(plan, shard, row_start, row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()


def build_stage1_resident_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the standalone Stage-1 resident-parameter proof graph.

    This graph is the first product-shaped model-stage proof. The hot
    ``sequence`` binds the arena only for ABI stability, streams zero-guarded
    C16 halo patches, and drains Stage-1 C16 cells without scheduling parameter
    DMA. Current IRON Runtime.sequence does not expose a named-sequence keyword
    in the Windows wheel, so this compile proof seeds the Stage-1 record as a
    tile-local resident Buffer while the build still emits a placeholder
    model-epoch parameter-load instruction stream.
    """

    import numpy as np
    from aie.iron import Buffer, Kernel, ObjectFifo, Program, Runtime, Worker
    from aie.iron.controlflow import range_
    from aie.iron.device import Tile

    plan = build_graph_phase_plan(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
        graph_phase="stage1-resident",
    )
    selected_device = _resolve_iron_device(device)

    c16_bytes = CELL_SHAPES["c16_cell"].element_bytes
    c16_ty = np.ndarray[(c16_bytes,), np.dtype[np.int8]]
    c16_halo_ty = np.ndarray[(C16_HALO_PATCH_WORDS,), np.dtype[np.uint64]]
    ingress_frame_ty = np.ndarray[(_front_halo_ingress_words(plan),), np.dtype[np.uint64]]
    egress_frame_ty = np.ndarray[(plan.frame_slot_bytes // DMA_HALO_TAP_WORD_BYTES,), np.dtype[np.uint64]]
    param_arena_ty = np.ndarray[(RESIDENT_PARAMETER_ARENA_BYTES,), np.dtype[np.int8]]
    stage1_param_ty = np.ndarray[(STAGE1_HOT_RECORD_BYTES,), np.dtype[np.int8]]

    stage1 = Kernel(
        "apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8",
        "apusr_middle_core_vector_primitives.o",
        [c16_halo_ty, stage1_param_ty, c16_ty],
    )

    ingress_fifos = []
    egress_fifos = []
    egress_shards = []
    workers = []

    def make_stage1_worker(cell_count: int):
        def stage1_worker(of_halo_patch, stage1_params, of_c16_cell, stage1_fn):
            for _ in range_(cell_count):
                patch = of_halo_patch.acquire(1)
                out = of_c16_cell.acquire(1)
                stage1_fn(patch, stage1_params, out)
                of_halo_patch.release(1)
                of_c16_cell.release(1)

        return stage1_worker

    for shard in plan.column_shards:
        name = f"l{shard.lane_index}_c{shard.macro_column_index}"
        mem_tile = Tile(shard.physical_column, 1)
        front_tile = Tile(shard.physical_column, 2)

        ingress = ObjectFifo(c16_halo_ty, name=f"{name}_stage1_halo_patch_l3l2", depth=1)
        front_in = ingress.cons().forward(
            tile=mem_tile,
            name=f"{name}_stage1_halo_patch_l2l1",
            depth=1,
        )
        stage1_params = Buffer(stage1_param_ty, name=f"{name}_stage1_params_l1", initial_value=_stage1_identity_params())
        stage1_out = ObjectFifo(c16_ty, name=f"{name}_stage1_c16_l1l2", depth=4)
        egress = stage1_out.cons().forward(tile=mem_tile, name=f"{name}_stage1_c16_l2l3", depth=4)

        ingress_fifos.append(ingress)
        egress_fifos.append(egress)
        egress_shards.append(shard)
        workers.append(
            Worker(
                make_stage1_worker(shard.cell_count),
                [front_in.cons(), stage1_params, stage1_out.prod(), stage1],
                tile=front_tile,
                stack_size=0x1000,
            )
        )

    runtime = Runtime()
    with runtime.sequence(param_arena_ty, ingress_frame_ty, egress_frame_ty) as (_param_arena, ingress_frame, egress_frame):
        runtime.start(*workers)
        for shard, fifo in zip(plan.column_shards, ingress_fifos, strict=True):
            for row_start, row_count in _halo_patch_row_blocks(shard):
                runtime.fill(
                    fifo.prod(),
                    ingress_frame,
                    _zero_guarded_halo_patch_tap(plan, shard, row_start, row_count),
                    tile=Tile(shard.physical_column, 0),
                )
        for shard, fifo in zip(egress_shards, egress_fifos, strict=True):
            for row_start, row_count in _cell_row_blocks(shard):
                runtime.drain(
                    fifo.cons(),
                    egress_frame,
                    _m4x8_c16_cell_row_block_tap(plan, shard, row_start=row_start, row_count=row_count),
                    wait=True,
                    tile=Tile(shard.physical_column, 0),
                )

    return Program(selected_device, runtime).resolve_program()


def build_spatial_core_program(
    profile: ProfileName,
    *,
    device: DeviceName = "npu2",
    model_resolution: ResolutionName = "2160",
    lane_groups: str | int = "auto",
):
    """Emit the primary spatial-core graph for the current Stage1-4 milestone.

    The active spatial-core build must exercise the real early-pipeline
    contract, not an older standalone C16/C32/C64 spine. Stage1 and Stage2 are
    fused at the producer footprint, every Stage2 C16 cell is produced exactly
    once in a paired 2x2 macroblock, the same cells are teed to SkipA, and
    Stage3 consumes that compact 2x2 C16 group.
    Stage4 is deliberately split from this graph until the C32 halo ring is
    wired; the primary path does not use full Stage2 row FIFOs or a fused
    Stage3/4 C16 mega-window.
    """

    return build_stage1_2_to_stage3_paired_macroblock_resident_program(
        profile,
        device=device,
        model_resolution=model_resolution,
        lane_groups=lane_groups,
    )

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--model-resolution", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--graph-phase", choices=("spatial-core", "cell-conveyor", "c16-halo", "front-c16-row", "stage1-resident", "stage2-resident", "stage2-plane-window-resident", "stage1_2-linked-resident", "stage1_2-fused-overlap-resident", "stage1_2-worker-ring-resident", "stage1_4-worker-ring-resident", "stage1_2-to-stage3_4-worker-ring-resident", "stage1_2-to-stage3-paired-macroblock-resident", "stage3-down-resident", "stage3_4-fused-resident", "stage3_4-product-resident"), default="spatial-core")
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    args = parser.parse_args()
    return args


def main() -> int:
    args = parse_args()
    worker_groups = args.worker_groups or args.lane_groups or "auto"
    plan = build_graph_phase_plan(args.profile, device=args.device, model_resolution=args.model_resolution, lane_groups=worker_groups, graph_phase=args.graph_phase)
    if args.dump == "json":
        print(json.dumps(plan.to_json(), indent=2))
    elif args.dump == "check":
        if plan.name in {"spatial_core_stage1_12_spine", "stage1_2_to_stage3_paired_macroblock_resident"}:
            print(
                f"validated graph phase plan: {plan.profile} {plan.name} "
                f"reserved_columns={plan.macro_lane_columns} active_compute_tiles={len(current_active_compute_tiles(plan))}"
            )
        else:
            print(f"validated graph phase plan: {plan.profile} {plan.name} worker_groups={plan.lane_groups} physical_shards={plan.shard_count}")
    else:
        print(build_graph_phase_summary(plan))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
