#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Plan the worker-local memory-tile ring schedule for early main-path fusion.

This is a physical scheduling contract, not a timing model. It describes the
Stage2-output C16 ring used by fused Stage3/4 after Stage1+2 expanded-footprint
fusion deleted the Stage1->Stage2 ring. The key product rule is that ring
updates are row-block operations owned by an independent four-column worker.
The long-lived SkipA store is a production-time tee of the same Stage2 rows,
not a per-cell eviction callback.
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
    LIMITS,
    TRANSPORT_CELL_ROWS,
    TRANSPORT_CELL_WIDTH,
    TRANSPORT_PIXEL_BYTES,
    build_frame_plan,
)


@dataclass(frozen=True)
class Stage2Stage34RingPolicy:
    name: str = "stage2_to_stage3_4_c16"
    producer: str = "stage1_2_fused_overlap"
    consumer: str = "stage3_4_fused"
    ring_rows: int = 20
    consumer_window_rows: int = 20
    consumer_window_columns: int = 40
    consumer_stride_rows: int = 16
    horizontal_guard_pixels_each_side: int = 2
    element_bytes: int = TRANSPORT_PIXEL_BYTES
    skip_tap: str = "skipA_c16_stage2_to_stage11_ddr_spill"


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


def _ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def _build_shard_schedule(frame: Any, worker: Any, worker_local_column: int, first_cell: int, cell_columns: int, policy: Stage2Stage34RingPolicy) -> dict[str, Any]:
    owned_pixels = cell_columns * TRANSPORT_CELL_WIDTH
    stored_pixels = owned_pixels + 2 * policy.horizontal_guard_pixels_each_side
    row_bytes = stored_pixels * policy.element_bytes
    ring_bytes = row_bytes * policy.ring_rows
    output_cell_rows = max(0, _ceil_div(frame.cell_rows - 1, 2))
    row_blocks = output_cell_rows
    produced_rows_per_step = policy.consumer_stride_rows
    retained_rows_per_step = policy.ring_rows - produced_rows_per_step
    # The Stage3/4 window stride is one C32 cell row. In C16 source-domain rows
    # that advances 16 rows and reuses the 4 halo rows retained in the 20-row ring.
    return {
        "worker_index": worker.lane_index,
        "worker_local_column": worker_local_column,
        "physical_column": worker.lane_index * frame.macro_lane_columns + worker_local_column,
        "first_worker_local_cell_column": first_cell,
        "owned_cell_columns": cell_columns,
        "owned_pixels": owned_pixels,
        "stored_pixels_with_horizontal_guard": stored_pixels,
        "row_bytes": row_bytes,
        "ring_rows": policy.ring_rows,
        "ring_bytes": ring_bytes,
        "fits_one_memory_tile": ring_bytes <= LIMITS["memory_tile_data_memory_bytes"],
        "consumer_window": {
            "rows": policy.consumer_window_rows,
            "columns": policy.consumer_window_columns,
            "bytes": policy.consumer_window_rows * policy.consumer_window_columns * policy.element_bytes,
            "source": "memory_tile_ring_mm2s_tensor_view",
        },
        "row_step": {
            "stage3_4_output_cell_rows": output_cell_rows,
            "new_stage2_rows_per_step": produced_rows_per_step,
            "retained_stage2_rows_per_step": retained_rows_per_step,
            "ring_slot_policy": "logical_row_mod_ring_rows",
            "wrap_policy": "split_wrapped_mm2s_window_into_at_most_two_row_segments",
        },
        "control_work": {
            "ring_s2mm_tasks_per_row_step": 1,
            "stage3_4_mm2s_window_tasks_per_row_step": "one row-block tensor task per consumer shard; wrap may split it into two row segments",
            "skip_spill_tasks_per_row_step": 1,
            "per_cell_dma_tasks": 0,
            "per_cell_eviction_callbacks": 0,
            "scalar_eviction_kernel": False,
        },
        "skip_tap": {
            "name": policy.skip_tap,
            "timing": "tee_on_stage2_row_production",
            "reason": "The long-lived skip needs the same Stage2 rows, so writing it while the rows are produced avoids a later eviction callback and avoids keeping extra live rows just to spill them.",
            "ddr_payload_bytes_per_row_step": row_bytes * produced_rows_per_step,
            "fallback_timing": "spill_before_slot_reuse_only_if_runtime_cannot_tee_the_producer_stream",
        },
    }


def build_schedule(profile: str, device: str, capacity: str, worker_groups: str | int = "auto") -> dict[str, Any]:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=worker_groups)
    policy = Stage2Stage34RingPolicy()
    workers: list[dict[str, Any]] = []
    for worker in frame.lane_stripes:
        shards = [
            _build_shard_schedule(frame, worker, column_index, first_cell, cell_columns, policy)
            for column_index, (first_cell, cell_columns) in enumerate(_split_extent(worker.cell_columns, frame.macro_lane_columns))
        ]
        workers.append(
            {
                "worker_index": worker.lane_index,
                "owned_cell_columns": worker.cell_columns,
                "owned_pixels": worker.cell_columns * TRANSPORT_CELL_WIDTH,
                "cell_rows": worker.cell_rows,
                "memory_tile_shards": shards,
                "max_ring_bytes_per_memory_tile": max((s["ring_bytes"] for s in shards), default=0),
                "all_memory_tile_shards_fit": all(s["fits_one_memory_tile"] for s in shards),
            }
        )
    max_shard = max((w["max_ring_bytes_per_memory_tile"] for w in workers), default=0)
    return {
        "schema_version": 1,
        "profile": profile,
        "device": device,
        "capacity": capacity,
        "worker_groups": frame.lane_groups,
        "worker_columns": frame.macro_lane_columns,
        "physical_columns_used": frame.lane_groups * frame.macro_lane_columns,
        "product_floor": "npu1 Phoenix is one independent spatial4col worker covering the full assigned seam width",
        "stage_boundary": policy.name,
        "producer": policy.producer,
        "consumer": policy.consumer,
        "implementation_status": "product_schedule_contract_with_compile_carrier_already_proven",
        "ring": {
            "rows": policy.ring_rows,
            "consumer_window_rows": policy.consumer_window_rows,
            "consumer_window_columns": policy.consumer_window_columns,
            "consumer_stride_rows": policy.consumer_stride_rows,
            "retained_rows_between_steps": policy.ring_rows - policy.consumer_stride_rows,
            "horizontal_guard_pixels_each_side": policy.horizontal_guard_pixels_each_side,
            "element_bytes": policy.element_bytes,
            "storage": "worker_local_memory_tile_shards",
            "addressing": "row_major_circular_rows_per_memory_tile_shard",
        },
        "control_policy": {
            "no_per_cell_dma_tasks": True,
            "no_scalar_eviction_kernel": True,
            "skipA_spill_is_tee_on_production": True,
            "eviction_is_slot_reuse_not_a_control_callback": True,
            "wrapped_windows_split_by_row_segment": True,
        },
        "workers": workers,
        "max_ring_bytes_per_memory_tile": max_shard,
        "all_memory_tile_shards_fit": all(w["all_memory_tile_shards_fit"] for w in workers),
        "memory_tile_bytes": LIMITS["memory_tile_data_memory_bytes"],
        "reserve_note": "Static reservation and other rings must be checked by the row-lifetime budget; this schedule only proves the Stage2->Stage3/4 ring storage and control shape.",
    }


def _print_summary(schedule: dict[str, Any]) -> None:
    print(
        f"{schedule['profile']} {schedule['device']} {schedule['capacity']}: "
        f"workers={schedule['worker_groups']}, worker_columns={schedule['worker_columns']}, "
        f"stage_boundary={schedule['stage_boundary']}"
    )
    ring = schedule["ring"]
    print(
        f"  ring: rows={ring['rows']}, stride_rows={ring['consumer_stride_rows']}, "
        f"retained={ring['retained_rows_between_steps']}, storage={ring['storage']}"
    )
    print(
        "  control: tee SkipA on production; eviction is slot reuse; no per-cell DMA tasks; "
        "wrapped windows split into row segments"
    )
    print(
        f"  max ring bytes per memory tile: {schedule['max_ring_bytes_per_memory_tile']} / {schedule['memory_tile_bytes']}"
    )
    for worker in schedule["workers"]:
        print(
            f"  worker {worker['worker_index']}: owned_pixels={worker['owned_pixels']}, "
            f"max_shard={worker['max_ring_bytes_per_memory_tile']}, shards_fit={worker['all_memory_tile_shards_fit']}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default="auto")
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    parser.add_argument("--require-ring-fit", action="store_true")
    parser.add_argument("--require-tee-skip", action="store_true")
    parser.add_argument("--require-no-per-cell-dma", action="store_true")
    args = parser.parse_args()

    raw_worker_groups = args.lane_groups if args.lane_groups is not None else args.worker_groups
    worker_groups: str | int = raw_worker_groups
    if worker_groups != "auto":
        worker_groups = int(worker_groups)
    schedule = build_schedule(args.profile, args.device, args.capacity, worker_groups)

    if args.require_ring_fit and not schedule["all_memory_tile_shards_fit"]:
        print(json.dumps(schedule, indent=2), file=sys.stderr)
        return 1
    if args.require_tee_skip and schedule["control_policy"].get("skipA_spill_is_tee_on_production") is not True:
        print(json.dumps(schedule, indent=2), file=sys.stderr)
        return 1
    if args.require_no_per_cell_dma and schedule["control_policy"].get("no_per_cell_dma_tasks") is not True:
        print(json.dumps(schedule, indent=2), file=sys.stderr)
        return 1

    if args.dump == "json":
        print(json.dumps(schedule, indent=2))
    elif args.dump == "check":
        print("memtile ring schedule validated")
    else:
        _print_summary(schedule)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
