#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Estimate vertical-stripe multi-pass row-ring pressure.

A 4-column worker is the atomic product unit. On Phoenix the worker owns the
full seam width in one pass. This tool answers what happens if that same worker
walks the frame in multiple vertical stripes instead: row-ring byte pressure
falls roughly with stripe width, while pass count, boundary guards, scheduling
latency, and repeated ingress/egress overhead increase.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.analysis.middle_core_spatial_plan import LIMITS, build_frame_plan  # noqa: E402


def _split_width(width: int, parts: int) -> list[int]:
    base = width // parts
    extra = width % parts
    return [base + (1 if i < extra else 0) for i in range(parts)]


def build_budget(profile: str, device: str, capacity: str, passes: int, worker_groups: str | int = "auto", reserve_bytes: int = 64 * 1024) -> dict:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=worker_groups)
    usable = LIMITS["memory_tile_data_memory_bytes"] - reserve_bytes
    rows = {
        "stage1_to_stage2_c16_deleted_by_fused_overlap": 0,
        "stage2_to_stage3_4_c16": 20,
        "transition_only_target": 40,
    }
    workers = []
    for worker in frame.lane_stripes:
        worker_width = worker.cell_columns * 16
        pass_widths = _split_width(worker_width, passes)
        pass_payloads = []
        for index, width in enumerate(pass_widths):
            # Each vertical pass owns its local horizontal guards. Interior
            # pass seams are locally produced/owned; there is no cross-worker dependency.
            guarded_width = width + 2
            shard_widths = _split_width(guarded_width, frame.macro_lane_columns)
            max_row_bytes = max(shard_widths) * 16
            pass_payloads.append(
                {
                    "pass_index": index,
                    "owned_width_pixels": width,
                    "guarded_width_pixels": guarded_width,
                    "max_row_bytes_per_memory_tile_shard": max_row_bytes,
                    "stage1_to_stage2_c16_ring_bytes_per_shard": 0,
                    "stage1_to_stage2_c16_policy": "deleted_by_stage1_2_fused_overlap",
                    "stage2_to_stage3_4_c16_ring_bytes_per_shard": max_row_bytes * rows["stage2_to_stage3_4_c16"],
                    "transition_only_ring_bytes_per_shard": max_row_bytes * rows["transition_only_target"],
                    "transition_only_fits_after_reserve": max_row_bytes * rows["transition_only_target"] <= usable,
                }
            )
        workers.append(
            {
                "worker_index": worker.lane_index,
                "full_width_pixels": worker_width,
                "passes": pass_payloads,
                "max_transition_only_bytes_per_shard": max(p["transition_only_ring_bytes_per_shard"] for p in pass_payloads),
            }
        )
    return {
        "profile": profile,
        "device": device,
        "capacity": capacity,
        "worker_groups": frame.lane_groups,
        "worker_columns": frame.macro_lane_columns,
        "vertical_passes_per_worker": passes,
        "static_reserve_bytes_per_memory_tile": reserve_bytes,
        "usable_bytes_per_memory_tile_after_reserve": usable,
        "policy": "vertical passes reduce row length but serialize each independent worker across more frame stripes; Stage1->Stage2 is already deleted by fused-overlap and is not a ring budget item",
        "workers": workers,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), default="spatial4col")
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu1")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default="auto")
    parser.add_argument("--passes", type=int, default=2)
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    parser.add_argument("--require-transition-only-fit", action="store_true")
    args = parser.parse_args()
    if args.passes <= 0:
        raise SystemExit("--passes must be positive")
    budget = build_budget(args.profile, args.device, args.capacity, args.passes, args.worker_groups)
    ok = all(p["transition_only_fits_after_reserve"] for w in budget["workers"] for p in w["passes"])
    if args.dump == "json":
        print(json.dumps(budget, indent=2))
    elif args.dump == "summary":
        print(f"{budget['profile']} {budget['device']} {budget['capacity']} vertical passes={budget['vertical_passes_per_worker']}")
        print(f"usable bytes/memtile after reserve: {budget['usable_bytes_per_memory_tile_after_reserve']}")
        for worker in budget["workers"]:
            print(f"worker {worker['worker_index']}: full_width={worker['full_width_pixels']} max_transition_bytes/shard={worker['max_transition_only_bytes_per_shard']}")
        print(f"transition-only rings fit every pass: {ok}")
    else:
        print("vertical-pass row budget validated" if ok else json.dumps({"ok": ok, "budget": budget}, indent=2))
    if args.require_transition_only_fit and not ok:
        raise SystemExit("vertical-pass transition-only ring plan does not fit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
