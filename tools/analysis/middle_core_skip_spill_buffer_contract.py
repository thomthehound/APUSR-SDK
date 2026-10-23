#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Report the persistent BO contract for long-lived middle-core skip spills.

Short-lived activation context rings live in worker-local memory tiles. SkipA
and SkipB survive for many logical stages, so the product path needs an
explicit persistent NPU-visible spill arena rather than relying on the immediate
frame ingress/egress BOs or on memory-tile rings.
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

ALIGNMENT_BYTES = 4096


@dataclass(frozen=True)
class SkipSliceSpec:
    name: str
    producer_stage: int
    consumer_stage: int
    width_divisor: int
    height_divisor: int
    channels: int
    bytes_per_sample: int
    write_policy: str
    read_policy: str

    @property
    def element_bytes(self) -> int:
        return self.channels * self.bytes_per_sample


SKIP_SLICES: tuple[SkipSliceSpec, ...] = (
    SkipSliceSpec(
        name="skipA_c16_stage2_to_stage11",
        producer_stage=2,
        consumer_stage=11,
        width_divisor=2,
        height_divisor=2,
        channels=16,
        bytes_per_sample=1,
        write_policy="tee Stage-2 C16 rows to the spill arena as they are produced for the Stage3/4 boundary ring",
        read_policy="Stage 11 reads the same frame's SkipA slice in stripe/row order for the C16 skip add",
    ),
    SkipSliceSpec(
        name="skipB_c32_stage5_to_stage9",
        producer_stage=5,
        consumer_stage=9,
        width_divisor=4,
        height_divisor=4,
        channels=32,
        bytes_per_sample=1,
        write_policy="tee Stage-5 C32 rows to the spill arena as they are produced for the downstream C32/C64 path",
        read_policy="Stage 9 reads the same frame's SkipB slice in stripe/row order for the C32 skip add",
    ),
)


def align_up(value: int, alignment: int = ALIGNMENT_BYTES) -> int:
    return value if value % alignment == 0 else value + (alignment - value % alignment)


def build_contract(profile: str, device: str, capacity: str, worker_groups: str | int) -> dict[str, Any]:
    frame = build_frame_plan(profile, device=device, model_resolution=capacity, lane_groups=worker_groups)
    offset = 0
    slices = []
    for spec in SKIP_SLICES:
        width = frame.model_output_width // spec.width_divisor
        rows = frame.model_output_rows // spec.height_divisor
        payload_bytes = width * rows * spec.element_bytes
        aligned_bytes = align_up(payload_bytes)
        slices.append(
            {
                "name": spec.name,
                "producer_stage": spec.producer_stage,
                "consumer_stage": spec.consumer_stage,
                "shape": {
                    "width_pixels": width,
                    "rows": rows,
                    "channels": spec.channels,
                    "bytes_per_sample": spec.bytes_per_sample,
                    "element_bytes": spec.element_bytes,
                },
                "arena_offset_bytes": offset,
                "payload_bytes_per_frame": payload_bytes,
                "aligned_bytes_per_frame": aligned_bytes,
                "write_policy": spec.write_policy,
                "read_policy": spec.read_policy,
            }
        )
        offset += aligned_bytes

    required_bytes = align_up(offset)
    return {
        "schema_version": 2,
        "purpose": "Persistent NPU-visible spill arena for one frame's long-lived skip tensors",
        "profile": profile,
        "device": device,
        "capacity": capacity,
        "worker_groups": frame.lane_groups,
        "worker_columns": frame.macro_lane_columns,
        "model_output_width": frame.model_output_width,
        "model_output_rows": frame.model_output_rows,
        "arena": {
            "allocation": "one persistent skip-spill BO shared by SkipA and SkipB",
            "alignment_bytes": ALIGNMENT_BYTES,
            "required_bytes": required_bytes,
            "total_bytes": required_bytes,
            "kernel_arg": "optional kSkipSpillBufferArgIndex / AmdXrtKernelAbi::skipSpillBufferArgIndex",
            "ownership": "NPU writes and later reads within the same frame; GPU does not consume this BO in the normal hot path",
        },
        "slices": slices,
        "policy": {
            "requires_explicit_bo": True,
            "one_npu_frame_in_flight": True,
            "do_not_reserve_hidden_skip_slots": True,
            "future_scratch_must_be_explicit": True,
            "do_not_alias_frame_ingress_or_egress": True,
            "do_not_store_long_lived_skip_in_memtile_ring": True,
            "tee_on_production": True,
            "ring_eviction_is_slot_reuse_not_callback": True,
        },
    }


def _summary(contract: dict[str, Any]) -> str:
    arena = contract["arena"]
    lines = [
        f"{contract['profile']} {contract['device']} {contract['capacity']} skip-spill BO contract:",
        f"  required BO bytes: {arena['required_bytes']} bytes",
        f"  kernel arg: {arena['kernel_arg']}",
        "  in-flight policy: one NPU frame; no hidden skip-frame slots",
    ]
    for slc in contract["slices"]:
        shape = slc["shape"]
        lines.append(
            f"  {slc['name']}: offset={slc['arena_offset_bytes']} bytes, "
            f"payload={slc['payload_bytes_per_frame']} bytes/frame, "
            f"shape={shape['width_pixels']}x{shape['rows']}xC{shape['channels']}"
        )
    return "\n".join(lines)


def _parse_worker_groups(value: str) -> str | int:
    return value if value == "auto" else int(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("spatial2col", "spatial4col"), required=True)
    parser.add_argument("--device", choices=("npu1", "npu2"), default="npu1")
    parser.add_argument("--capacity", choices=("1080", "2160", "4320"), default="2160")
    parser.add_argument("--worker-groups", default=None)
    parser.add_argument("--lane-groups", default=None, help="Deprecated alias for --worker-groups")
    parser.add_argument("--dump", choices=("summary", "json", "check"), default="summary")
    parser.add_argument("--require-explicit-bo", action="store_true")
    parser.add_argument("--require-combined-skip-arena", action="store_true")
    args = parser.parse_args()

    raw_worker_groups = args.worker_groups or args.lane_groups or "auto"
    contract = build_contract(args.profile, args.device, args.capacity, _parse_worker_groups(raw_worker_groups))

    if args.require_explicit_bo and not contract["policy"].get("requires_explicit_bo"):
        print(json.dumps(contract, indent=2), file=sys.stderr)
        return 1
    if args.require_combined_skip_arena:
        arena = contract["arena"]
        if not arena.get("allocation", "").startswith("one persistent skip-spill BO") or len(contract["slices"]) < 2:
            print(json.dumps(contract, indent=2), file=sys.stderr)
            return 1
        if arena["total_bytes"] != arena["required_bytes"]:
            print(json.dumps(contract, indent=2), file=sys.stderr)
            return 1
        if not contract["policy"].get("one_npu_frame_in_flight"):
            print(json.dumps(contract, indent=2), file=sys.stderr)
            return 1

    if args.dump == "json":
        print(json.dumps(contract, indent=2))
    elif args.dump == "summary":
        print(_summary(contract))
    else:
        print("skip spill BO contract validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
