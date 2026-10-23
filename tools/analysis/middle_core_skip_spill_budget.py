#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Report long-lived skip spill budgets for the resident middle-core plan.

Short-lived activation context should stay in worker-local memory-tile rings.
The skip tensors have a much longer lifetime: Stage 2 feeds Stage 11, and Stage
5 feeds Stage 9. This tool models those as deliberate DDR spill/tap streams,
not as ordinary stage-boundary activation rings.
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

from tools.analysis.middle_core_spatial_plan import build_frame_plan  # noqa: E402

FPS_POINTS = (60, 120, 144)


@dataclass(frozen=True)
class SkipSpillSpec:
    name: str
    source_stage: int
    consumer_stage: int
    width_divisor: int
    height_divisor: int
    channels: int
    bytes_per_sample: int
    source_boundary: str
    spill_policy: str

    @property
    def element_bytes(self) -> int:
        return self.channels * self.bytes_per_sample


SKIP_SPILLS: tuple[SkipSpillSpec, ...] = (
    SkipSpillSpec(
        name="skipA_c16_stage2_to_stage11",
        source_stage=2,
        consumer_stage=11,
        width_divisor=2,
        height_divisor=2,
        channels=16,
        bytes_per_sample=1,
        source_boundary="stage2_to_stage3_4_c16 production tee",
        spill_policy=(
            "tee Stage-2 C16 output into the Stage3/4 boundary ring for immediate main-path use; "
            "tee them to the explicit SkipA spill arena for the Stage-11 skip join"
        ),
    ),
    SkipSpillSpec(
        name="skipB_c32_stage5_to_stage9",
        source_stage=5,
        consumer_stage=9,
        width_divisor=4,
        height_divisor=4,
        channels=32,
        bytes_per_sample=1,
        source_boundary="stage5 C32 output / Stage5->6 transition boundary",
        spill_policy=(
            "treat Stage-5 C32 output as the SkipB source; keep only short-lived main-path context on-chip, "
            "and spill the long-lived skip stream to DDR unless later placement proves an on-chip delay is affordable"
        ),
    ),
)


def _bandwidth_table(bytes_per_frame: int) -> dict[str, float]:
    return {f"gb_s_at_{fps}fps": bytes_per_frame * fps / 1_000_000_000 for fps in FPS_POINTS}


def _split_extent(count: int, parts: int) -> list[int]:
    base = count // parts
    extra = count % parts
    return [base + (1 if index < extra else 0) for index in range(parts)]


def _spill_payload(spec: SkipSpillSpec, frame: Any) -> dict[str, Any]:
    width = frame.model_output_width // spec.width_divisor
    rows = frame.model_output_rows // spec.height_divisor
    bytes_per_frame = width * rows * spec.element_bytes
    worker_widths = _split_extent(width, frame.lane_groups)
    workers = []
    for index, worker_width in enumerate(worker_widths):
        worker_bytes = worker_width * rows * spec.element_bytes
        workers.append(
            {
                "worker_index": index,
                "assigned_width_pixels": worker_width,
                "rows": rows,
                "channels": spec.channels,
                "bytes_per_sample": spec.bytes_per_sample,
                "write_bytes_per_frame": worker_bytes,
                "read_bytes_per_frame": worker_bytes,
                "round_trip_bytes_per_frame": worker_bytes * 2,
            }
        )
    return {
        "name": spec.name,
        "source_stage": spec.source_stage,
        "consumer_stage": spec.consumer_stage,
        "shape": {
            "width_pixels": width,
            "rows": rows,
            "channels": spec.channels,
            "bytes_per_sample": spec.bytes_per_sample,
            "element_bytes": spec.element_bytes,
        },
        "source_boundary": spec.source_boundary,
        "spill_policy": spec.spill_policy,
        "write_bytes_per_frame": bytes_per_frame,
        "read_bytes_per_frame": bytes_per_frame,
        "round_trip_bytes_per_frame": bytes_per_frame * 2,
        "write_bandwidth": _bandwidth_table(bytes_per_frame),
        "round_trip_bandwidth": _bandwidth_table(bytes_per_frame * 2),
        "workers": workers,
    }


def build_budget(profile: str, device: str, capacity: str, worker_groups: str | int = "auto") -> dict[str, Any]:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=worker_groups)
    skips = [_spill_payload(spec, frame) for spec in SKIP_SPILLS]
    total_write = sum(skip["write_bytes_per_frame"] for skip in skips)
    total_round_trip = sum(skip["round_trip_bytes_per_frame"] for skip in skips)
    return {
        "schema_version": 1,
        "purpose": "Long-lived skip spill budget; short-lived activation context rings are modeled separately",
        "profile": profile,
        "device": device,
        "capacity": capacity,
        "worker_groups": frame.lane_groups,
        "worker_columns": frame.macro_lane_columns,
        "physical_columns_used": frame.lane_groups * frame.macro_lane_columns,
        "model_output_width": frame.model_output_width,
        "model_output_rows": frame.model_output_rows,
        "policy": {
            "short_lived_context": "keep on-chip in worker-local memory-tile boundary rings",
            "long_lived_skip": "tee to an explicit skip-spill arena when the producer rows are generated unless a later capacity/timing proof keeps it on-chip",
            "skipA_source": "Stage 2 output, teed while producing the Stage2->Stage3/4 C16 ring rows",
            "skipB_source": "Stage 5 output, tapped from the Stage5/6 boundary path",
            "cross_worker_dependency": False,
        },
        "skip_spills": skips,
        "totals": {
            "write_bytes_per_frame": total_write,
            "read_bytes_per_frame": total_write,
            "round_trip_bytes_per_frame": total_round_trip,
            "write_bandwidth": _bandwidth_table(total_write),
            "round_trip_bandwidth": _bandwidth_table(total_round_trip),
        },
    }


def _summary(budget: dict[str, Any]) -> str:
    lines = [
        f"{budget['profile']} {budget['device']} {budget['capacity']} worker_groups={budget['worker_groups']}:",
        "  skip policy: short-lived main-path rings stay on-chip; long-lived skips tee to explicit spill storage",
    ]
    for skip in budget["skip_spills"]:
        rt = skip["round_trip_bandwidth"]
        lines.append(
            f"  {skip['name']}: {skip['shape']['width_pixels']}x{skip['shape']['rows']}xC{skip['shape']['channels']} "
            f"write={skip['write_bytes_per_frame']} bytes/frame, round_trip={skip['round_trip_bytes_per_frame']} bytes/frame "
            f"({rt['gb_s_at_120fps']:.2f} GB/s @120fps)"
        )
        lines.append(f"    source: {skip['source_boundary']}")
    totals = budget["totals"]
    lines.append(
        f"  total skip write: {totals['write_bytes_per_frame']} bytes/frame "
        f"({totals['write_bandwidth']['gb_s_at_120fps']:.2f} GB/s @120fps)"
    )
    lines.append(
        f"  total skip round-trip: {totals['round_trip_bytes_per_frame']} bytes/frame "
        f"({totals['round_trip_bandwidth']['gb_s_at_120fps']:.2f} GB/s @120fps)"
    )
    return "\n".join(lines)


def _parse_worker_groups(value: str) -> str | int:
    if value == "auto":
        return value
    return int(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu2")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    parser.add_argument("--require-ddr-skip-policy", action="store_true")
    args = parser.parse_args()

    raw_worker_groups = args.worker_groups or args.lane_groups or "auto"
    budget = build_budget(args.profile, args.device, args.capacity, _parse_worker_groups(raw_worker_groups))

    if args.require_ddr_skip_policy:
        policy = budget.get("policy") or {}
        skip_a = next((skip for skip in budget["skip_spills"] if skip["name"].startswith("skipA")), None)
        if policy.get("long_lived_skip", "").startswith("tee") is False or skip_a is None:
            print(json.dumps(budget, indent=2), file=sys.stderr)
            return 1
        if "production tee" not in skip_a.get("source_boundary", ""):
            print(json.dumps(budget, indent=2), file=sys.stderr)
            return 1

    if args.dump == "json":
        print(json.dumps(budget, indent=2))
    elif args.dump == "check":
        print("skip spill budget validated")
    else:
        print(_summary(budget))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
