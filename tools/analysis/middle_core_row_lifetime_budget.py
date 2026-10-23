#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Estimate simultaneous activation-row residency pressure for the middle core.

This is deliberately a planning tool, not a timing model. It answers whether a
product-shaped 4-column worker can afford to keep every producer boundary as a
memory-tile row ring. The answer is no for the Phoenix floor. The useful output
is the row budget and the amount of boundary context that must be removed by
expanded-footprint fusion, smaller micro-row kernels, or an explicit spill.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.analysis.middle_core_spatial_plan import (  # noqa: E402
    LIMITS,
    build_frame_plan,
)


@dataclass(frozen=True)
class BoundarySpec:
    name: str
    producer: str
    consumer: str
    scale_divisor: int
    channel_bytes: int
    row_window_rows: int
    category: str
    product_policy: str
    notes: tuple[str, ...] = ()


# A logical activation row is intentionally normalized to bytes at the W/2 C16
# seam. C16 at W/2, C32 at W/4, and C64 at W/8 all have the same row byte count.
BOUNDARIES: tuple[BoundarySpec, ...] = (
    BoundarySpec(
        name="stage1_to_stage2_c16",
        producer="stage1",
        consumer="stage2",
        scale_divisor=1,
        channel_bytes=16,
        row_window_rows=10,
        category="proven_expanded_footprint_fusion",
        product_policy="deleted_by_stage1_2_fused_overlap",
        notes=(
            "Stage2 needs a 10-row view of Stage1 outputs for the current 16x8 cell shape.",
            "The compiled stage1_2 fused-overlap proof computes that Stage1-domain temporary from a 20x12 source patch and runs Stage2 locally.",
            "This boundary is no longer part of the product row-ring budget.",
        ),
    ),
    BoundarySpec(
        name="stage2_to_stage3_4_c16",
        producer="stage2",
        consumer="stage3/4",
        scale_divisor=1,
        channel_bytes=16,
        row_window_rows=20,
        category="scale_transition_and_skip_source",
        product_policy="keep_short_lived_ring_and_tee_skipA_to_ddr_on_production",
        notes=(
            "Stage3/4 needs a 20-row C16 window for the fused downsample+FNB32 proof.",
            "Stage2 output is also the long-lived SkipA source, so the skip path should be teed as Stage2 rows are produced.",
        ),
    ),
    BoundarySpec(
        name="stage4_to_stage5_c32",
        producer="stage4",
        consumer="stage5",
        scale_divisor=2,
        channel_bytes=32,
        row_window_rows=10,
        category="same_scale_spatial",
        product_policy="prefer_expanded_footprint_fusion_over_boundary_ring",
    ),
    BoundarySpec(
        name="stage5_to_stage67_c32",
        producer="stage5",
        consumer="stage6/7",
        scale_divisor=2,
        channel_bytes=32,
        row_window_rows=20,
        category="scale_transition_and_skip_source",
        product_policy="keep_short_lived_ring_and_tee_skipB_to_ddr_on_production",
        notes=("Stage5 output is the long-lived SkipB source.",),
    ),
    BoundarySpec(
        name="stage7_to_stage8_c64",
        producer="stage7",
        consumer="stage8",
        scale_divisor=4,
        channel_bytes=64,
        row_window_rows=10,
        category="same_scale_spatial",
        product_policy="prefer_expanded_footprint_fusion_over_boundary_ring",
    ),
    BoundarySpec(
        name="stage8_to_stage9_c64",
        producer="stage8",
        consumer="stage9",
        scale_divisor=4,
        channel_bytes=64,
        row_window_rows=10,
        category="decoder_transition_candidate",
        product_policy="prove_ct2d_window_needs_before_promoting_to_full_ring",
        notes=("This remains a planning placeholder until the stage9 CT2D input contract is compiled.",),
    ),
    BoundarySpec(
        name="stage9_to_stage10_c32",
        producer="stage9",
        consumer="stage10",
        scale_divisor=2,
        channel_bytes=32,
        row_window_rows=10,
        category="same_scale_spatial",
        product_policy="prefer_expanded_footprint_fusion_over_boundary_ring",
    ),
    BoundarySpec(
        name="stage10_to_stage11_c32",
        producer="stage10",
        consumer="stage11",
        scale_divisor=2,
        channel_bytes=32,
        row_window_rows=10,
        category="decoder_transition_candidate",
        product_policy="prove_ct2d_window_needs_before_promoting_to_full_ring",
    ),
    BoundarySpec(
        name="stage11_to_stage12_c16",
        producer="stage11",
        consumer="stage12",
        scale_divisor=1,
        channel_bytes=16,
        row_window_rows=10,
        category="same_scale_spatial",
        product_policy="prefer_expanded_footprint_fusion_over_boundary_ring",
    ),
)


def _split_pixels(pixels: int, parts: int) -> list[int]:
    base = pixels // parts
    extra = pixels % parts
    return [base + (1 if index < extra else 0) for index in range(parts)]


def _worker_boundary_rows(worker: Any, worker_columns: int, boundary: BoundarySpec) -> dict[str, Any]:
    c16_width = worker.cell_columns * 16
    scale_width = math.ceil(c16_width / boundary.scale_divisor)
    shard_pixels = _split_pixels(scale_width, worker_columns)
    shard_row_bytes = [pixels * boundary.channel_bytes for pixels in shard_pixels]
    shard_ring_bytes = [row_bytes * boundary.row_window_rows for row_bytes in shard_row_bytes]
    return {
        "name": boundary.name,
        "producer": boundary.producer,
        "consumer": boundary.consumer,
        "category": boundary.category,
        "product_policy": boundary.product_policy,
        "scale_divisor": boundary.scale_divisor,
        "channel_bytes": boundary.channel_bytes,
        "row_window_rows": boundary.row_window_rows,
        "worker_scale_width_pixels": scale_width,
        "max_row_bytes_per_memory_tile_shard": max(shard_row_bytes, default=0),
        "max_ring_bytes_per_memory_tile_shard": max(shard_ring_bytes, default=0),
        "notes": list(boundary.notes),
    }


def build_budget(profile: str, device: str, capacity: str, worker_groups: str | int = "auto", static_reserve_bytes: int = 64 * 1024) -> dict[str, Any]:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=worker_groups)
    worker_columns = frame.macro_lane_columns
    workers: list[dict[str, Any]] = []
    for worker in frame.lane_stripes:
        boundary_rows = [_worker_boundary_rows(worker, worker_columns, boundary) for boundary in BOUNDARIES]
        row_bytes = max((b["max_row_bytes_per_memory_tile_shard"] for b in boundary_rows), default=0)
        naive_rows = sum(b["row_window_rows"] for b in boundary_rows)
        naive_bytes = sum(b["max_ring_bytes_per_memory_tile_shard"] for b in boundary_rows)
        budget_bytes = LIMITS["memory_tile_data_memory_bytes"] - static_reserve_bytes
        budget_rows = budget_bytes // row_bytes if row_bytes else 0
        proven_fused_rows = sum(b["row_window_rows"] for b in boundary_rows if b["category"] == "proven_expanded_footprint_fusion")
        same_scale_rows = sum(b["row_window_rows"] for b in boundary_rows if b["category"] == "same_scale_spatial")
        decoder_candidate_rows = sum(b["row_window_rows"] for b in boundary_rows if b["category"] == "decoder_transition_candidate")
        must_eliminate_rows = max(0, naive_rows - budget_rows)
        transition_rows = sum(b["row_window_rows"] for b in boundary_rows if b["category"] not in {"same_scale_spatial", "proven_expanded_footprint_fusion"})
        stage12_fused_rows = naive_rows - proven_fused_rows
        fused_same_scale_rows = naive_rows - proven_fused_rows - same_scale_rows
        minimal_transition_rows = naive_rows - proven_fused_rows - same_scale_rows - decoder_candidate_rows
        workers.append(
            {
                "worker_index": worker.lane_index,
                "owned_c16_pixels": worker.cell_columns * 16,
                "memory_tile_shards_in_worker": worker_columns,
                "static_reserve_bytes_per_memory_tile": static_reserve_bytes,
                "row_bytes_per_memory_tile_shard": row_bytes,
                "row_budget_per_memory_tile_shard_after_reserve": budget_rows,
                "naive_all_boundary_rows": naive_rows,
                "naive_all_boundary_bytes_per_memory_tile_shard": naive_bytes,
                "naive_all_boundaries_fit": naive_bytes <= budget_bytes,
                "proven_fused_boundary_rows": proven_fused_rows,
                "stage12_fused_policy_rows": stage12_fused_rows,
                "same_scale_ring_rows": same_scale_rows,
                "transition_or_decoder_ring_rows": transition_rows,
                "decoder_candidate_rows": decoder_candidate_rows,
                "rows_that_must_be_removed_for_naive_plan_to_fit": must_eliminate_rows,
                "stage12_fused_policy_bytes_per_memory_tile_shard": stage12_fused_rows * row_bytes,
                "stage12_fused_policy_fits": stage12_fused_rows * row_bytes <= budget_bytes,
                "fused_same_scale_policy_rows": fused_same_scale_rows,
                "fused_same_scale_policy_bytes_per_memory_tile_shard": fused_same_scale_rows * row_bytes,
                "fused_same_scale_policy_fits": fused_same_scale_rows * row_bytes <= budget_bytes,
                "minimal_transition_policy_rows": minimal_transition_rows,
                "minimal_transition_policy_bytes_per_memory_tile_shard": minimal_transition_rows * row_bytes,
                "minimal_transition_policy_fits": minimal_transition_rows * row_bytes <= budget_bytes,
                "boundaries": boundary_rows,
            }
        )

    worst = max(workers, key=lambda item: item["naive_all_boundary_bytes_per_memory_tile_shard"], default=None)
    return {
        "schema_version": 1,
        "purpose": "simultaneous row-residency pressure for one independent middle-core worker",
        "profile": profile,
        "device": device,
        "capacity": capacity,
        "worker_groups": frame.lane_groups,
        "worker_columns": worker_columns,
        "physical_columns_used": frame.lane_groups * frame.macro_lane_columns,
        "memory_tile_bytes": LIMITS["memory_tile_data_memory_bytes"],
        "static_reserve_bytes_per_memory_tile": static_reserve_bytes,
        "policy": {
            "one_ring_per_boundary_is_not_product_viable": True,
            "reuse_by_phase_is_not_the_primary_answer": True,
            "required_direction": "remove boundary rings with expanded-footprint fusion or smaller row-block kernels; keep long-lived skips as explicit spill taps",
            "stage1_to_stage2_ring_deleted_by_fused_overlap": True,
            "same_scale_fusion_alone_is_not_enough_on_phoenix_floor": True,
            "decoder_transition_candidates_must_not_be_promoted_to_full_rings_without_compile_evidence": True,
            "first_fusion_candidate": "stage1_plus_stage2_expanded_footprint",
            "first_fusion_candidate_status": "compiled_cleanly_static_audit_acceptable",
        },
        "workers": workers,
        "worst_worker": worst,
    }


def _print_human(payload: dict[str, Any]) -> None:
    print(f"profile={payload['profile']} device={payload['device']} capacity={payload['capacity']} worker_groups={payload['worker_groups']}")
    print(f"memory_tile_bytes={payload['memory_tile_bytes']} static_reserve={payload['static_reserve_bytes_per_memory_tile']}")
    for worker in payload["workers"]:
        print(f"worker {worker['worker_index']}: owned_c16_pixels={worker['owned_c16_pixels']} shards={worker['memory_tile_shards_in_worker']}")
        print(f"  row_bytes/shard={worker['row_bytes_per_memory_tile_shard']}")
        print(f"  row_budget/shard={worker['row_budget_per_memory_tile_shard_after_reserve']}")
        print(f"  naive rows={worker['naive_all_boundary_rows']} bytes/shard={worker['naive_all_boundary_bytes_per_memory_tile_shard']} fit={worker['naive_all_boundaries_fit']}")
        print(f"  rows to remove={worker['rows_that_must_be_removed_for_naive_plan_to_fit']}")
        print(f"  stage1+2 fused rows={worker['stage12_fused_policy_rows']} bytes/shard={worker['stage12_fused_policy_bytes_per_memory_tile_shard']} fit={worker['stage12_fused_policy_fits']}")
        print(f"  after same-scale fusion rows={worker['fused_same_scale_policy_rows']} bytes/shard={worker['fused_same_scale_policy_bytes_per_memory_tile_shard']} fit={worker['fused_same_scale_policy_fits']}")
        print(f"  transition-only rows={worker['minimal_transition_policy_rows']} bytes/shard={worker['minimal_transition_policy_bytes_per_memory_tile_shard']} fit={worker['minimal_transition_policy_fits']}")
        print("  boundary rows:")
        for boundary in worker["boundaries"]:
            print(f"    {boundary['name']}: rows={boundary['row_window_rows']} category={boundary['category']} policy={boundary['product_policy']}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), default="spatial4col")
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu1")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", "--lane-groups", dest="worker_groups", default="auto")
    parser.add_argument("--static-reserve-bytes", type=int, default=64 * 1024)
    parser.add_argument("--dump", choices=("text", "json", "check"), default="text")
    parser.add_argument("--require-naive-over-budget", action="store_true")
    parser.add_argument("--require-stage1_2-fused-policy-recorded", action="store_true")
    parser.add_argument("--require-same-scale-fusion-plan-fits", action="store_true")
    parser.add_argument("--require-transition-only-plan-fits", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    payload = build_budget(args.profile, args.device, args.capacity, args.worker_groups, args.static_reserve_bytes)
    if args.dump == "json":
        print(json.dumps(payload, indent=2, sort_keys=True))
    elif args.dump == "text":
        _print_human(payload)

    workers = payload["workers"]
    if args.require_naive_over_budget and any(worker["naive_all_boundaries_fit"] for worker in workers):
        print("expected naive all-boundary ring plan to exceed at least one worker budget", file=sys.stderr)
        return 1
    if args.require_stage1_2_fused_policy_recorded and not all(worker["proven_fused_boundary_rows"] >= 10 for worker in workers):
        print("stage1+2 fused-overlap policy was not recorded", file=sys.stderr)
        return 1
    if args.require_same_scale_fusion_plan_fits and not all(worker["fused_same_scale_policy_fits"] for worker in workers):
        print("same-scale fusion policy still exceeds the memory-tile shard budget", file=sys.stderr)
        return 1
    if args.require_transition_only_plan_fits and not all(worker["minimal_transition_policy_fits"] for worker in workers):
        print("transition-only policy still exceeds the memory-tile shard budget", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
