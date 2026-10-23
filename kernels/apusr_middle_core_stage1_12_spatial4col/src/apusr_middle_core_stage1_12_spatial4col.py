#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""4-column spatial middle-core generator."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


PROFILE = "spatial4col"


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _load_spatial_plan():
    repo_root = _repo_root()
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    from tools.analysis.middle_core_spatial_plan import (
        build_frame_plan,
        build_frame_summary,
        build_resource_budget,
        build_summary,
        build_cell_offset_examples,
    )
    from tools.analysis.middle_core_spatial_graph import (
        build_cell_conveyor_program,
        build_c16_halo_program,
        build_front_c16_row_program,
        build_spatial_core_program,
        build_stage1_resident_program,
        build_stage2_resident_program,
        build_stage2_plane_window_resident_program,
        build_stage1_2_linked_resident_program,
        build_stage1_2_fused_overlap_resident_program,
        build_stage1_2_worker_ring_resident_program,
        build_stage1_4_worker_ring_resident_program,
        build_stage1_2_to_stage3_4_worker_ring_resident_program,
        build_stage1_2_to_stage3_paired_macroblock_resident_program,
        build_stage3_down_resident_program,
        build_stage3_4_fused_resident_program,
        build_stage3_4_product_resident_program,
        build_graph_phase_plan,
        build_graph_phase_summary,
    )

    return (
        build_frame_plan,
        build_frame_summary,
        build_resource_budget,
        build_summary,
        build_cell_offset_examples,
        build_cell_conveyor_program,
        build_c16_halo_program,
        build_front_c16_row_program,
        build_spatial_core_program,
        build_stage1_resident_program,
        build_stage2_resident_program,
        build_stage2_plane_window_resident_program,
        build_stage1_2_linked_resident_program,
        build_stage1_2_fused_overlap_resident_program,
        build_stage1_2_worker_ring_resident_program,
        build_stage1_4_worker_ring_resident_program,
        build_stage1_2_to_stage3_4_worker_ring_resident_program,
        build_stage1_2_to_stage3_paired_macroblock_resident_program,
        build_stage3_down_resident_program,
        build_stage3_4_fused_resident_program,
        build_stage3_4_product_resident_program,
        build_graph_phase_plan,
        build_graph_phase_summary,
    )


def _parse_worker_groups(raw: str) -> str | int:
    if raw == "auto":
        return raw
    try:
        value = int(raw)
    except ValueError as exc:
        raise SystemExit("--worker-groups must be auto or a positive integer") from exc
    if value <= 0:
        raise SystemExit("--worker-groups must be auto or a positive integer")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--model-resolution", choices=("1080", "2160", "4320"), required=True)
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument(
        "--dump-plan",
        choices=("summary", "json", "frame", "frame-json", "graph", "graph-json"),
        help="Print the canonical spatial or graph-phase plan instead of generating MLIR.",
    )
    parser.add_argument(
        "--emit-graph-phase",
        choices=("spatial-core", "cell-conveyor", "c16-halo", "front-c16-row", "stage1-resident", "stage2-resident", "stage2-plane-window-resident", "stage1_2-linked-resident", "stage1_2-fused-overlap-resident", "stage1_2-worker-ring-resident", "stage1_4-worker-ring-resident", "stage1_2-to-stage3_4-worker-ring-resident", "stage1_2-to-stage3-paired-macroblock-resident", "stage3-down-resident", "stage3_4-fused-resident", "stage3_4-product-resident"),
        help="Emit the current spatial core graph or an explicit diagnostic graph.",
    )
    args = parser.parse_args()

    raw_worker_groups = args.worker_groups or args.lane_groups
    if raw_worker_groups is None:
        raise SystemExit("--worker-groups is required")
    lane_groups = _parse_worker_groups(raw_worker_groups)
    (
        build_frame_plan,
        build_frame_summary,
        build_resource_budget,
        build_summary,
        build_cell_offset_examples,
        build_cell_conveyor_program,
        build_c16_halo_program,
        build_front_c16_row_program,
        build_spatial_core_program,
        build_stage1_resident_program,
        build_stage2_resident_program,
        build_stage2_plane_window_resident_program,
        build_stage1_2_linked_resident_program,
        build_stage1_2_fused_overlap_resident_program,
        build_stage1_2_worker_ring_resident_program,
        build_stage1_4_worker_ring_resident_program,
        build_stage1_2_to_stage3_4_worker_ring_resident_program,
        build_stage1_2_to_stage3_paired_macroblock_resident_program,
        build_stage3_down_resident_program,
        build_stage3_4_fused_resident_program,
        build_stage3_4_product_resident_program,
        build_graph_phase_plan,
        build_graph_phase_summary,
    ) = _load_spatial_plan()
    budget = build_resource_budget(PROFILE)
    frame = build_frame_plan(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups)

    if args.dump_plan == "summary":
        print(build_summary(PROFILE))
        print(build_frame_summary(frame))
        return 0
    if args.dump_plan == "frame":
        print(build_frame_summary(frame))
        return 0
    if args.dump_plan == "frame-json":
        print(json.dumps({"frame_plan": frame.to_json(), "cell_offset_examples": build_cell_offset_examples(frame)}, indent=2))
        return 0
    if args.dump_plan == "graph":
        print(build_graph_phase_summary(build_graph_phase_plan(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups, graph_phase=args.emit_graph_phase or "spatial-core")))
        return 0
    if args.dump_plan == "graph-json":
        print(json.dumps(build_graph_phase_plan(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups, graph_phase=args.emit_graph_phase or "spatial-core").to_json(), indent=2))
        return 0
    if args.dump_plan == "json":
        print(
            json.dumps(
                {
                    "device": args.device,
                    "model_resolution": args.model_resolution,
                    "lane_groups": frame.lane_groups,
                    "resource_budget": budget,
                    "frame_plan": frame.to_json(),
                    "graph_phase_plan": build_graph_phase_plan(
                        PROFILE,
                        device=args.device,
                        model_resolution=args.model_resolution,
                        lane_groups=lane_groups,
                        graph_phase=args.emit_graph_phase or "spatial-core",
                    ).to_json(),
                },
                indent=2,
            )
        )
        return 0

    if args.emit_graph_phase in (None, "spatial-core"):
        print(build_spatial_core_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "cell-conveyor":
        print(build_cell_conveyor_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "c16-halo":
        print(build_c16_halo_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "front-c16-row":
        print(build_front_c16_row_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1-resident":
        print(build_stage1_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage2-resident":
        print(build_stage2_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage2-plane-window-resident":
        print(build_stage2_plane_window_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1_2-linked-resident":
        print(build_stage1_2_linked_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1_2-fused-overlap-resident":
        print(build_stage1_2_fused_overlap_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1_2-worker-ring-resident":
        print(build_stage1_2_worker_ring_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1_4-worker-ring-resident":
        print(build_stage1_4_worker_ring_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1_2-to-stage3_4-worker-ring-resident":
        print(build_stage1_2_to_stage3_4_worker_ring_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage1_2-to-stage3-paired-macroblock-resident":
        print(build_stage1_2_to_stage3_paired_macroblock_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage3-down-resident":
        print(build_stage3_down_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage3_4-fused-resident":
        print(build_stage3_4_fused_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    if args.emit_graph_phase == "stage3_4-product-resident":
        print(build_stage3_4_product_resident_program(PROFILE, device=args.device, model_resolution=args.model_resolution, lane_groups=lane_groups))
        return 0
    raise SystemExit("unsupported middle-core graph selection")


if __name__ == "__main__":
    raise SystemExit(main())
