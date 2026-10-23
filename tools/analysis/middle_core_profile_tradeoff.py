#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Compare active on-chip boundary rings against DDR spill pressure."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.analysis.middle_core_activation_ring_budget import build_budget  # noqa: E402
from tools.analysis.middle_core_spatial_plan import (  # noqa: E402
    CELL_SHAPES,
    LIMITS,
    TRANSPORT_CELL_BYTES,
    TRANSPORT_PIXEL_BYTES,
    build_frame_plan,
    resolve_lane_groups,
)

FPS_POINTS = (60, 120, 144)
FRONT_MEMTILE_STATIC_BYTES = 64 * 1024


def _profile_role_model(profile: str) -> dict[str, Any]:
    if profile == "spatial2col":
        return {
            "profile_status": "diagnostic",
            "memory_tiles_per_macro_lane": 2,
            "primary_ring_owner": "shared_worker_memtile_shards",
            "primary_ring_role": "stage2_to_stage3_4_context",
            "skipa_role_isolated": False,
            "skipb_role_isolated": False,
            "egress_role_isolated": False,
            "role_pressure": [
                "active C16 transition ring shares scarce 2-column resources with skip/egress roles",
                "SkipB shares placement with egress",
            ],
        }
    return {
        "profile_status": "product_candidate",
        "memory_tiles_per_macro_lane": 4,
        "primary_ring_owner": "worker_memtile_shards",
        "primary_ring_role": "stage2_to_stage3_4_context",
        "skipa_role_isolated": True,
        "skipb_role_isolated": True,
        "egress_role_isolated": True,
        "role_pressure": [
            "active Stage2->Stage3/4 C16 context ring is sharded across the 4-column worker memory tiles",
            "SkipA, SkipB, and egress have separate memory-tile roles",
        ],
    }


def _bandwidth_table(bytes_per_frame: int) -> dict[str, float]:
    return {f"gb_s_at_{fps}fps": bytes_per_frame * fps / 1_000_000_000 for fps in FPS_POINTS}


def build_tradeoff(profile: str, device: str, capacity: str, lane_groups: str | int = "auto") -> dict[str, Any]:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=lane_groups)
    ring_budget = build_budget(profile, device, capacity, lane_groups)
    role_model = _profile_role_model(profile)

    c16_frame_bytes = frame.seam_width * frame.seam_rows * TRANSPORT_PIXEL_BYTES
    stage2_patch_read_bytes = CELL_SHAPES["c16_halo_cell"].element_bytes * frame.cell_columns * frame.cell_rows
    c16_cell_read_bytes = TRANSPORT_CELL_BYTES * frame.cell_columns * frame.cell_rows
    ddr_minimal_spill_bytes = c16_frame_bytes * 2
    ddr_patch_spill_bytes = c16_frame_bytes + stage2_patch_read_bytes

    stripes = []
    for stripe in ring_budget["stripes"]:
        one_ring_with_static = stripe["ring_bytes"] + FRONT_MEMTILE_STATIC_BYTES
        two_rings_with_static = stripe["ring_bytes"] * 2 + FRONT_MEMTILE_STATIC_BYTES
        stripes.append(
            {
                **stripe,
                "front_memtile_static_reserved_bytes": FRONT_MEMTILE_STATIC_BYTES,
                "one_ring_plus_static_bytes": one_ring_with_static,
                "two_rings_plus_static_bytes": two_rings_with_static,
                "one_ring_plus_static_fits": one_ring_with_static <= LIMITS["memory_tile_data_memory_bytes"],
                "two_rings_plus_static_fits": two_rings_with_static <= LIMITS["memory_tile_data_memory_bytes"],
                "headroom_after_one_ring_plus_static_bytes": LIMITS["memory_tile_data_memory_bytes"] - one_ring_with_static,
                "headroom_after_two_rings_plus_static_bytes": LIMITS["memory_tile_data_memory_bytes"] - two_rings_with_static,
            }
        )

    return {
        "schema_version": 2,
        "purpose": "Profile tradeoff model for active C16 boundary rings and deleted Stage1->Stage2 ring",
        "deleted_boundary": {
            "name": "stage1_to_stage2_c16",
            "replacement": "stage1_2-fused-overlap-resident",
            "status": "deleted_from_product_ring_budget",
            "avoided_ddr_minimal_write_read_bytes_per_frame": ddr_minimal_spill_bytes,
            "avoided_ddr_patch_read_write_bytes_per_frame": ddr_patch_spill_bytes,
            "avoided_ddr_minimal_bandwidth": _bandwidth_table(ddr_minimal_spill_bytes),
            "avoided_ddr_patch_bandwidth": _bandwidth_table(ddr_patch_spill_bytes),
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
        "auto_worker_policy": "auto means as many independent worker groups as fit: target_columns // worker_columns",
        "worker_independence_policy": ring_budget.get("worker_independence_policy"),
        "seam_width_pixels": frame.seam_width,
        "seam_rows": frame.seam_rows,
        "cell_columns": frame.cell_columns,
        "cell_rows": frame.cell_rows,
        "role_model": role_model,
        "memory_tile_bytes": LIMITS["memory_tile_data_memory_bytes"],
        "primary_ring": {
            "ring_rows": ring_budget["ring_rows"],
            "horizontal_guard_pixels_each_side": ring_budget["horizontal_guard_pixels_each_side"],
            "all_lanes_one_ring_plus_static_fit": all(s["one_ring_plus_static_fits"] for s in stripes),
            "all_lanes_two_rings_plus_static_fit": all(s["two_rings_plus_static_fits"] for s in stripes),
            "max_one_ring_plus_static_bytes": max(s["one_ring_plus_static_bytes"] for s in stripes),
            "max_two_rings_plus_static_bytes": max(s["two_rings_plus_static_bytes"] for s in stripes),
            "min_headroom_after_one_ring_plus_static_bytes": min(s["headroom_after_one_ring_plus_static_bytes"] for s in stripes),
            "min_headroom_after_two_rings_plus_static_bytes": min(s["headroom_after_two_rings_plus_static_bytes"] for s in stripes),
            "stripes": stripes,
        },
        "ddr_spill_estimate": {
            "c16_full_intermediate_bytes": c16_frame_bytes,
            "stage2_owned_cell_read_bytes": c16_cell_read_bytes,
            "stage2_halo_patch_read_bytes": stage2_patch_read_bytes,
            "minimal_write_then_read_bytes_per_frame": ddr_minimal_spill_bytes,
            "patch_read_write_bytes_per_frame": ddr_patch_spill_bytes,
            "minimal_write_then_read_bandwidth": _bandwidth_table(ddr_minimal_spill_bytes),
            "patch_read_write_bandwidth": _bandwidth_table(ddr_patch_spill_bytes),
            "notes": [
                "The minimal estimate assumes Stage2 can read the spilled C16 plane with near-perfect row reuse.",
                "The patch estimate matches the standalone halo-window proof shape and over-reads overlap per output cell.",
                "Both estimates ignore latency, NoC contention, cache behavior, and other model stages.",
            ],
        },
        "verdict": _verdict(profile, role_model, stripes),
    }


def _verdict(profile: str, role_model: dict[str, Any], stripes: list[dict[str, Any]]) -> dict[str, Any]:
    one_fits = all(s["one_ring_plus_static_fits"] for s in stripes)
    two_fits = all(s["two_rings_plus_static_fits"] for s in stripes)
    if profile == "spatial4col" and one_fits and role_model["skipa_role_isolated"]:
        return {
            "product_direction": "prefer_4col_on_chip_transition_ring_before_ddr_spill",
            "confidence": "planning_model_only_needs_timing_evidence",
            "reason": "The remaining active C16 transition ring fits when sharded across the 4-column worker memory tiles while Stage1->Stage2 is deleted by fused-overlap evaluation.",
            "double_buffer_margin": "available" if two_fits else "not_available_at_this_capacity_or_lane_split",
        }
    if one_fits:
        return {
            "product_direction": "fits_capacity_but_role_coupling_is_high",
            "confidence": "diagnostic_only",
            "reason": "The ring fits, but it competes with other long-lived roles in the same macro-lane placement.",
            "double_buffer_margin": "available" if two_fits else "not_available_at_this_capacity_or_lane_split",
        }
    return {
        "product_direction": "requires_smaller_stripes_or_ddr_fallback",
        "confidence": "capacity_model_only",
        "reason": "The producer ring does not fit with the current lane split and static reserve.",
        "double_buffer_margin": "not_available",
    }


def _parse_lane_groups(value: str) -> str | int:
    if value == "auto":
        return value
    return int(value)


def _summary(tradeoff: dict[str, Any]) -> str:
    front = tradeoff["primary_ring"]
    ddr = tradeoff["ddr_spill_estimate"]
    verdict = tradeoff["verdict"]
    lines = [
        f"{tradeoff['profile']} {tradeoff['device']} {tradeoff['capacity']} worker_groups={tradeoff['lane_groups']}:",
        f"  deleted boundary: {tradeoff['deleted_boundary']['name']} -> {tradeoff['deleted_boundary']['replacement']}",
        f"  active ring role: {tradeoff['role_model']['primary_ring_role']} ({tradeoff['role_model']['profile_status']})",
        f"  one active ring+static max: {front['max_one_ring_plus_static_bytes']} bytes, headroom: {front['min_headroom_after_one_ring_plus_static_bytes']} bytes",
        f"  two active rings+static max: {front['max_two_rings_plus_static_bytes']} bytes, headroom: {front['min_headroom_after_two_rings_plus_static_bytes']} bytes",
        f"  separate SkipA/SkipB/egress roles: {tradeoff['role_model']['skipa_role_isolated']}/{tradeoff['role_model']['skipb_role_isolated']}/{tradeoff['role_model']['egress_role_isolated']}",
        "  cross-worker hot-path dependencies: forbidden",
        f"  avoided Stage1->Stage2 DDR minimal spill: {ddr['minimal_write_then_read_bytes_per_frame']} bytes/frame ({ddr['minimal_write_then_read_bandwidth']['gb_s_at_120fps']:.2f} GB/s @120fps)",
        f"  avoided Stage1->Stage2 DDR patch-shaped spill: {ddr['patch_read_write_bytes_per_frame']} bytes/frame ({ddr['patch_read_write_bandwidth']['gb_s_at_120fps']:.2f} GB/s @120fps)",
        f"  verdict: {verdict['product_direction']} ({verdict['double_buffer_margin']})",
    ]
    for pressure in tradeoff["role_model"]["role_pressure"]:
        lines.append(f"    - {pressure}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    parser.add_argument("--require-product-ring-margin", action="store_true")
    args = parser.parse_args()

    raw_worker_groups = args.worker_groups or args.lane_groups or "auto"
    lane_groups = _parse_lane_groups(raw_worker_groups)
    # Validate explicit lane group before building the larger report so errors stay direct.
    resolve_lane_groups(args.profile, args.device, lane_groups)
    tradeoff = build_tradeoff(args.profile, args.device, args.capacity, lane_groups)

    if args.require_product_ring_margin:
        front = tradeoff["primary_ring"]
        roles = tradeoff["role_model"]
        if not front["all_lanes_one_ring_plus_static_fit"] or not roles["skipa_role_isolated"]:
            print(json.dumps(tradeoff, indent=2), file=sys.stderr)
            return 1

    if args.dump == "json":
        print(json.dumps(tradeoff, indent=2))
    elif args.dump == "check":
        print("profile tradeoff budget validated")
    else:
        print(_summary(tradeoff))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
