#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Report producer-owned activation ring budgets for chained spatial stages.

The product unit is one independent worker group. A worker group may contain
multiple physical columns, and those columns can shard a worker-local ring across
its memory tiles. Budget both levels explicitly: the Phoenix floor has one
4-column worker covering the full seam width, while Strix/Halo can instantiate
multiple independent 4-column workers that walk disjoint vertical stripes.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.analysis.middle_core_spatial_plan import (  # noqa: E402
    CELL_SHAPES,
    GUARD_PIXELS,
    LIMITS,
    TRANSPORT_CELL_WIDTH,
    TRANSPORT_PIXEL_BYTES,
    build_frame_plan,
)


@dataclass(frozen=True)
class RingSpec:
    name: str
    producer_stage: int
    consumer_stage: str
    element_bytes: int
    ring_rows: int
    horizontal_guard_pixels_each_side: int
    purpose: str
    boundary_kind: str
    skip_spill_tap: str | None = None
    notes: tuple[str, ...] = ()


ELIMINATED_BOUNDARIES: tuple[dict[str, Any], ...] = (
    {
        "name": "stage1_to_stage2_c16",
        "producer_stage": 1,
        "consumer_stage": "2",
        "former_ring_rows": CELL_SHAPES["c16_halo_cell"].rows,
        "former_horizontal_guard_pixels_each_side": GUARD_PIXELS,
        "replacement_graph_phase": "stage1_2-fused-overlap-resident",
        "replacement_kernel": "apusr_middle_core_stage1_2_fused_window20x12_to_c16_cell_i8",
        "product_policy": "deleted_by_expanded_footprint_fusion",
        "notes": [
            "Stage 1 and Stage 2 are still evaluated in model order with their original parameters and quantization semantics.",
            "The fused-overlap proof computes the Stage1-domain pixels Stage2 needs at the same time, then runs Stage2 locally.",
            "This removes the full-width Stage1->Stage2 activation ring from the product memory budget.",
        ],
    },
)


RING_SPECS: tuple[RingSpec, ...] = (
    RingSpec(
        name="stage2_to_stage3_4_c16",
        producer_stage=2,
        consumer_stage="3/4",
        element_bytes=TRANSPORT_PIXEL_BYTES,
        ring_rows=20,
        horizontal_guard_pixels_each_side=2,
        purpose="Stage-2 C16 producer ring feeding the fused Stage-3/4 20x40 C16 downsample/FNB32 window",
        boundary_kind="short_lived_next_stage_context_with_long_lived_skip_spill",
        skip_spill_tap="skipA_c16_stage2_to_stage11_ddr_spill_on_production_tee",
        notes=(
            "Stage 3/4 needs this rolling Stage-2-output context because the 20x40 window is too large to own in compute-tile L1 as a full-width stream buffer.",
            "The same Stage-2 output is also the SkipA source. Tee the row blocks to the long-lived skip store as Stage2 produces them; ring eviction is ordinary slot reuse, not the spill trigger.",
            "The ring is not a ping/pong latency-hiding buffer; it is the immediate main-path context window for Stage 3/4.",
        ),
    ),
)


def _split_extent(count: int, parts: int) -> list[tuple[int, int]]:
    base = count // parts
    extra = count % parts
    cursor = 0
    out: list[tuple[int, int]] = []
    for index in range(parts):
        width = base + (1 if index < extra else 0)
        out.append((cursor, width))
        cursor += width
    return out


def _ring_bytes(width_pixels: int, spec: RingSpec) -> int:
    stored_pixels = width_pixels + 2 * spec.horizontal_guard_pixels_each_side
    return stored_pixels * spec.ring_rows * spec.element_bytes


def _build_worker_ring(frame: Any, worker: Any, spec: RingSpec) -> dict[str, Any]:
    owned_pixels = worker.cell_columns * TRANSPORT_CELL_WIDTH
    stored_pixels = owned_pixels + 2 * spec.horizontal_guard_pixels_each_side
    ring_bytes = stored_pixels * spec.ring_rows * spec.element_bytes
    column_shards: list[dict[str, Any]] = []
    for column_index, (first_cell, cell_columns) in enumerate(_split_extent(worker.cell_columns, frame.macro_lane_columns)):
        shard_owned_pixels = cell_columns * TRANSPORT_CELL_WIDTH
        shard_stored_pixels = shard_owned_pixels + 2 * spec.horizontal_guard_pixels_each_side
        shard_bytes = shard_stored_pixels * spec.ring_rows * spec.element_bytes
        column_shards.append(
            {
                "worker_index": worker.lane_index,
                "worker_local_column": column_index,
                "physical_column": worker.lane_index * frame.macro_lane_columns + column_index,
                "first_worker_local_cell_column": first_cell,
                "owned_cell_columns": cell_columns,
                "owned_pixels": shard_owned_pixels,
                "stored_pixels_with_horizontal_guard": shard_stored_pixels,
                "ring_rows": spec.ring_rows,
                "ring_bytes": shard_bytes,
                "boundary_kind": spec.boundary_kind,
                "skip_spill_tap": spec.skip_spill_tap,
                "fits_one_memory_tile": shard_bytes <= LIMITS["memory_tile_data_memory_bytes"],
                "fits_two_rings_in_one_memory_tile": shard_bytes * 2 <= LIMITS["memory_tile_data_memory_bytes"],
                "role": "worker-local memory-tile shard",
            }
        )
    return {
        "worker_index": worker.lane_index,
        "owned_cell_columns": worker.cell_columns,
        "owned_pixels": owned_pixels,
        "stored_pixels_with_horizontal_guard": stored_pixels,
        "ring_rows": spec.ring_rows,
        "row_bytes": stored_pixels * spec.element_bytes,
        "ring_bytes": ring_bytes,
        "fits_one_memory_tile_if_unsharded": ring_bytes <= LIMITS["memory_tile_data_memory_bytes"],
        "column_shards": column_shards,
        "max_ring_bytes_per_memory_tile_shard": max((s["ring_bytes"] for s in column_shards), default=0),
        "all_column_shards_fit_one_memory_tile": all(s["fits_one_memory_tile"] for s in column_shards),
        "all_column_shards_fit_two_rings_one_memory_tile": all(s["fits_two_rings_in_one_memory_tile"] for s in column_shards),
        "worker_boundary_policy": "worker owns its assigned vertical stripe and its local guard pixels; no frame-hot cross-worker exchange",
    }


def build_budget(profile: str, device: str, capacity: str, lane_groups: str | int = "auto") -> dict[str, Any]:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=lane_groups)
    rings: list[dict[str, Any]] = []
    for spec in RING_SPECS:
        workers = [_build_worker_ring(frame, worker, spec) for worker in frame.lane_stripes]
        max_worker = max((w["ring_bytes"] for w in workers), default=0)
        max_shard = max((w["max_ring_bytes_per_memory_tile_shard"] for w in workers), default=0)
        rings.append(
            {
                "name": spec.name,
                "purpose": spec.purpose,
                "producer_stage": spec.producer_stage,
                "consumer_stage": spec.consumer_stage,
                "element_bytes": spec.element_bytes,
                "ring_rows": spec.ring_rows,
                "horizontal_guard_pixels_each_side": spec.horizontal_guard_pixels_each_side,
                "boundary_kind": spec.boundary_kind,
                "skip_spill_tap": spec.skip_spill_tap,
                "notes": list(spec.notes),
                "max_ring_bytes_per_independent_worker": max_worker,
                "max_ring_bytes_per_memory_tile_shard": max_shard,
                "all_workers_fit_one_memory_tile_if_unsharded": all(w["fits_one_memory_tile_if_unsharded"] for w in workers),
                "all_column_shards_fit_one_memory_tile": all(w["all_column_shards_fit_one_memory_tile"] for w in workers),
                "all_column_shards_fit_two_rings_one_memory_tile": all(w["all_column_shards_fit_two_rings_one_memory_tile"] for w in workers),
                "workers": workers,
            }
        )

    primary_ring = rings[0]
    return {
        "schema_version": 3,
        "purpose": "Worker-local producer-owned row-major activation rings for chained resident stages",
        "eliminated_boundaries": list(ELIMINATED_BOUNDARIES),
        "product_ring_policy": {
            "stage1_to_stage2_c16": "deleted_by_stage1_2_fused_overlap",
            "stage2_to_stage3_4_c16": "active_short_lived_memtile_ring_with_skipA_spill_tap",
        },
        "profile": profile,
        "device": device,
        "capacity": capacity,
        "lane_groups": frame.lane_groups,
        "worker_groups": frame.lane_groups,
        "macro_lane_columns": frame.macro_lane_columns,
        "worker_columns": frame.macro_lane_columns,
        "target_columns": frame.target_columns,
        "physical_columns_used": frame.lane_groups * frame.macro_lane_columns,
        "auto_worker_policy": "auto instantiates as many independent worker groups as fit: target_columns // worker_columns",
        "worker_independence_policy": {
            "worker_is_atomic_product_unit": True,
            "npu1_compatibility_floor": "one spatial4col worker on four physical columns covers the full assigned seam width",
            "npu2_auto_behavior": "two independent spatial4col workers that walk disjoint vertical frame stripes",
            "cross_worker_frame_hot_dependencies": False,
            "cross_worker_guard_exchange": False,
            "guard_source": "guards are local to each worker assignment; redundant guard work is preferred over cross-worker hot-path exchange",
        },
        "seam_width_pixels": frame.seam_width,
        "memory_tile_bytes": LIMITS["memory_tile_data_memory_bytes"],
        "rings": rings,
        # Compatibility fields used by older tradeoff scripts. These now refer
        # to the active primary main-path ring, not the deleted Stage1->Stage2 boundary.
        "ring_rows": primary_ring["ring_rows"],
        "horizontal_guard_pixels_each_side": primary_ring["horizontal_guard_pixels_each_side"],
        "max_ring_bytes_per_worker": primary_ring["max_ring_bytes_per_independent_worker"],
        "max_ring_bytes_per_memory_tile_shard": primary_ring["max_ring_bytes_per_memory_tile_shard"],
        "all_lanes_fit_one_memory_tile_each": primary_ring["all_column_shards_fit_one_memory_tile"],
        "all_lanes_fit_two_rings_one_memory_tile_each": primary_ring["all_column_shards_fit_two_rings_one_memory_tile"],
        "stripes": primary_ring["workers"],
    }


def _print_summary(budget: dict[str, Any]) -> None:
    print(
        f"{budget['profile']} {budget['device']} {budget['capacity']}: "
        f"worker_groups={budget['worker_groups']}, worker_columns={budget['worker_columns']}, "
        f"physical_columns_used={budget['physical_columns_used']}"
    )
    print("  worker model: independent vertical frame stripes; no cross-worker hot-path exchange")
    print("  product floor: budget npu1 as one 4-column worker covering the full seam width")
    for boundary in budget.get("eliminated_boundaries", []):
        print(
            f"  deleted {boundary['name']}: former_rows={boundary['former_ring_rows']}, "
            f"replacement={boundary['replacement_graph_phase']}"
        )
    for ring in budget["rings"]:
        print(
            f"  active {ring['name']}: rows={ring['ring_rows']}, guard={ring['horizontal_guard_pixels_each_side']}px, "
            f"kind={ring['boundary_kind']}, "
            f"max_worker={ring['max_ring_bytes_per_independent_worker']}, "
            f"max_memtile_shard={ring['max_ring_bytes_per_memory_tile_shard']}, "
            f"shards_fit={ring['all_column_shards_fit_one_memory_tile']}"
        )
        if ring.get("skip_spill_tap"):
            print(f"    skip spill tap: {ring['skip_spill_tap']}")
        for worker in ring["workers"]:
            print(
                f"    worker {worker['worker_index']}: owned_pixels={worker['owned_pixels']}, "
                f"ring_bytes={worker['ring_bytes']}, max_shard={worker['max_ring_bytes_per_memory_tile_shard']}, "
                f"unsharded_fit={worker['fits_one_memory_tile_if_unsharded']}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    parser.add_argument("--require-single-ring-fit", action="store_true")
    parser.add_argument("--require-all-product-rings-fit", action="store_true")
    parser.add_argument("--require-worker-independence", action="store_true")
    parser.add_argument("--require-stage1_2-ring-deleted", action="store_true")
    args = parser.parse_args()

    raw_worker_groups = args.worker_groups or args.lane_groups or "auto"
    lane_groups: str | int = raw_worker_groups
    if lane_groups != "auto":
        lane_groups = int(lane_groups)
    budget = build_budget(args.profile, args.device, args.capacity, lane_groups)

    if args.require_single_ring_fit and not budget["all_lanes_fit_one_memory_tile_each"]:
        print(json.dumps(budget, indent=2), file=sys.stderr)
        return 1
    if args.require_all_product_rings_fit:
        if not all(ring["all_column_shards_fit_one_memory_tile"] for ring in budget["rings"]):
            print(json.dumps(budget, indent=2), file=sys.stderr)
            return 1
    if args.require_worker_independence:
        policy = budget.get("worker_independence_policy") or {}
        if policy.get("worker_is_atomic_product_unit") is not True or policy.get("cross_worker_frame_hot_dependencies") is not False:
            print(json.dumps(budget, indent=2), file=sys.stderr)
            return 1
    if args.require_stage1_2_ring_deleted:
        deleted = {item.get("name"): item for item in budget.get("eliminated_boundaries", [])}
        if deleted.get("stage1_to_stage2_c16", {}).get("product_policy") != "deleted_by_expanded_footprint_fusion":
            print(json.dumps(budget, indent=2), file=sys.stderr)
            return 1

    if args.dump == "json":
        print(json.dumps(budget, indent=2))
    elif args.dump == "check":
        print("activation ring budget validated")
    else:
        _print_summary(budget)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
