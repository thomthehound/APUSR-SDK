#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the fixed AIE-ML tiling plan for the FSR4 INT8 middle core.

The plan is intentionally hardware-shaped rather than graph-framework-shaped:
spatial lanes own contiguous token columns, four pipeline segments own the
encoder/bottleneck/decoder stage groups, and skip tensors live in per-lane
memory-tile rings.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "reference"))
from middle_core_primitives import stage_macs  # noqa: E402
MODEL_ROOT = REPO_ROOT / "models" / "fsr4_int8"
CONTRACT_ROOT = MODEL_ROOT / "planning" / "middle_core_contracts"
LOGICAL_ROOT = MODEL_ROOT / "logical"
DEFAULT_OUT_ROOT = MODEL_ROOT / "planning" / "middle_core_tiling"

MACS_PER_AIE_TILE_CYCLE_I8 = 256
AIE_TILE_DATA_MEMORY_BYTES = 64 * 1024
MEMORY_TILE_BYTES = 512 * 1024
TOKEN_BYTES = 256
LANE_LOCAL_STAGING_TOKEN_CAP = 32
LINE_BUFFER_ROWS = 6
HORIZONTAL_HALO_TOKENS = 1
REFERENCE_CLOCK_HZ = 1_000_000_000

PIPELINE_SEGMENTS = (
    ("encoder2", (1, 2, 3)),
    ("encoder3", (4, 5, 6)),
    ("bottleneck_decoder3_up", (7, 8, 9)),
    ("decoder3_decoder2", (10, 11, 12)),
)

HARDWARE_PROFILES: dict[str, dict[str, Any]] = {
    "am020_4x4_base": {
        "compute_rows": 4,
        "lane_count": 4,
        "memory_tile_columns": 4,
        "role": "lowest-common-denominator 4-lane product topology",
    },
    "am020_8x4_wide": {
        "compute_rows": 4,
        "lane_count": 8,
        "memory_tile_columns": 8,
        "role": "wide topology for 8-column XDNA2-class parts and 4320p capacity",
    },
}


@dataclass(frozen=True)
class TensorShape:
    width: int
    height: int
    channels: int
    token_width_px: int
    tokens_per_row: int
    total_tokens: int


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def permutation_names() -> list[str]:
    names: list[str] = []
    for path in sorted(CONTRACT_ROOT.glob("*/middle_core_contract.json")):
        names.append(path.parent.name)
    return names


def parse_tensor(raw: dict[str, Any]) -> TensorShape:
    return TensorShape(
        width=int(raw["W"]),
        height=int(raw["H"]),
        channels=int(raw["effective_C"]),
        token_width_px=int(raw["token_width_px"]),
        tokens_per_row=int(raw["tokens_per_row"]),
        total_tokens=int(raw["total_tokens"]),
    )


def split_contiguous_ranges(total_items: int, lane_count: int) -> list[dict[str, int]]:
    base = total_items // lane_count
    remainder = total_items % lane_count
    ranges: list[dict[str, int]] = []
    cursor = 0
    for lane_index in range(lane_count):
        count = base + (1 if lane_index < remainder else 0)
        start = cursor
        end = cursor + count
        ranges.append({"lane": lane_index, "start_token_col": start, "end_token_col": end, "token_cols": count})
        cursor = end
    return ranges



def estimate_stage_macs(stage: dict[str, Any], logical_stage: dict[str, Any]) -> dict[str, Any]:
    return stage_macs(stage, logical_stage)

def build_stage_metrics(contract: dict[str, Any], logical_model: dict[str, Any]) -> list[dict[str, Any]]:
    logical_by_pass = {stage["pass_index"]: stage for stage in logical_model["stages"] if stage.get("pass_index") is not None}
    metrics: list[dict[str, Any]] = []
    for stage in contract["stages"]:
        pass_index = int(stage["pass"])
        mac_estimate = estimate_stage_macs(stage, logical_by_pass[pass_index])
        primary = parse_tensor(stage["primary"])
        output = parse_tensor(stage["output"])
        metrics.append(
            {
                "pass": pass_index,
                "op": stage["op_fn"],
                "kind": stage["kind"],
                "primary_shape": primary.__dict__,
                "output_shape": output.__dict__,
                "macs": mac_estimate,
            }
        )
    return metrics


def lane_ranges_for_profile(contract: dict[str, Any], lane_count: int) -> list[dict[str, Any]]:
    tokens_per_row = int(contract["ingress"]["tensor"]["tokens_per_row"])
    ranges = split_contiguous_ranges(tokens_per_row, lane_count)
    for item in ranges:
        token_cols = int(item["token_cols"])
        left_halo_tokens = HORIZONTAL_HALO_TOKENS if int(item["start_token_col"]) > 0 else 0
        right_halo_tokens = HORIZONTAL_HALO_TOKENS if int(item["end_token_col"]) < tokens_per_row else 0
        staging_token_cols = token_cols + left_halo_tokens + right_halo_tokens
        item["row_bytes"] = token_cols * TOKEN_BYTES
        item["left_halo_tokens"] = left_halo_tokens
        item["right_halo_tokens"] = right_halo_tokens
        item["staging_token_cols"] = staging_token_cols
        item["staging_row_bytes"] = staging_token_cols * TOKEN_BYTES
        item["line_buffer_bytes"] = item["staging_row_bytes"] * LINE_BUFFER_ROWS
        item["fits_64kb_local_staging"] = staging_token_cols <= LANE_LOCAL_STAGING_TOKEN_CAP
    return ranges

def segment_metrics(stage_metrics: list[dict[str, Any]], lane_count: int) -> list[dict[str, Any]]:
    by_pass = {int(stage["pass"]): stage for stage in stage_metrics}
    segments: list[dict[str, Any]] = []
    for row_index, (segment_name, passes) in enumerate(PIPELINE_SEGMENTS):
        stage_rows: list[dict[str, Any]] = []
        total_macs = 0
        for pass_index in passes:
            stage = by_pass[pass_index]
            total_macs += int(stage["macs"]["total_macs"])
            stage_rows.append(
                {
                    "pass": pass_index,
                    "op": stage["op"],
                    "kind": stage["kind"],
                    "macs_total": stage["macs"]["total_macs"],
                    "primitive_macs": stage["macs"]["primitive_macs"],
                }
            )
        lane_macs = math.ceil(total_macs / lane_count)
        lane_cycles = math.ceil(lane_macs / MACS_PER_AIE_TILE_CYCLE_I8)
        segments.append(
            {
                "segment": segment_name,
                "logical_row": row_index,
                "passes": list(passes),
                "stages": stage_rows,
                "total_macs": total_macs,
                "estimated_lane_macs": lane_macs,
                "estimated_lane_cycles_at_256_i8_macs_per_cycle": lane_cycles,
                "estimated_lane_ms_at_1ghz": lane_cycles / (REFERENCE_CLOCK_HZ / 1000),
            }
        )
    return segments


def skip_plan(contract: dict[str, Any], lane_count: int) -> list[dict[str, Any]]:
    plans: list[dict[str, Any]] = []
    for skip in contract["skip_fifos"]:
        per_lane_recommended_tokens = math.ceil(int(skip["fifo_recommended_tokens"]) / lane_count)
        per_lane_ring_bytes = per_lane_recommended_tokens * TOKEN_BYTES
        plans.append(
            {
                "name": skip["name"],
                "src_pass": skip["src_pass"],
                "dst_pass": skip["dst_pass"],
                "per_lane_recommended_tokens": per_lane_recommended_tokens,
                "per_lane_ring_bytes": per_lane_ring_bytes,
                "all_lanes_ring_bytes": per_lane_ring_bytes * lane_count,
                "memory_tile_bytes": MEMORY_TILE_BYTES,
                "fits_one_memory_tile_per_lane": per_lane_ring_bytes <= MEMORY_TILE_BYTES,
                "policy": "one logical memory-tile column per spatial lane; skips store owned tokens only; boundary halos are exchanged at consumers",
            }
        )
    return plans


def skip_memory_summary(skip_rings: list[dict[str, Any]]) -> dict[str, int | bool]:
    per_lane_total_bytes = sum(int(item["per_lane_ring_bytes"]) for item in skip_rings)
    per_lane_double_buffer_bytes = per_lane_total_bytes * 2
    return {
        "per_lane_total_ring_bytes": per_lane_total_bytes,
        "per_lane_double_buffer_ring_bytes": per_lane_double_buffer_bytes,
        "memory_tile_bytes": MEMORY_TILE_BYTES,
        "fits_one_memory_tile_per_lane": per_lane_double_buffer_bytes <= MEMORY_TILE_BYTES,
    }


def stage_group_tile_map(lanes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    tiles: list[dict[str, Any]] = []
    for segment_name, passes in PIPELINE_SEGMENTS:
        logical_row = next(index for index, (name, _passes) in enumerate(PIPELINE_SEGMENTS) if name == segment_name)
        compute_row = 2 + logical_row
        for lane in lanes:
            tiles.append(
                {
                    "logical_col": lane["lane"],
                    "logical_stage_group_row": logical_row,
                    "target_compute_row": compute_row,
                    "role": "stage_group",
                    "segment": segment_name,
                    "passes": list(passes),
                    "token_col_range": [lane["start_token_col"], lane["end_token_col"]],
                    "staging_token_range_with_halo": [
                        lane["start_token_col"] - lane["left_halo_tokens"],
                        lane["end_token_col"] + lane["right_halo_tokens"],
                    ],
                    "primary_handoff_policy": "same-lane stage groups are placed on adjacent compute rows and use nearest-neighbor shared-memory/lock handoff before falling back to routed streams",
                    "horizontal_halo_token_policy": "exchange or duplicate one neighboring token at lane boundaries for spatial kernels; no halo for pure pointwise legs",
                }
            )
    return tiles


def build_plan(permutation: str, profile_name: str) -> dict[str, Any]:
    if profile_name not in HARDWARE_PROFILES:
        raise ValueError(f"unknown hardware profile: {profile_name}")
    contract = load_json(CONTRACT_ROOT / permutation / "middle_core_contract.json")
    logical_model = load_json(LOGICAL_ROOT / permutation / "logical_model.json")
    profile = HARDWARE_PROFILES[profile_name]
    lane_count = int(profile["lane_count"])
    lanes = lane_ranges_for_profile(contract, lane_count)
    stages = build_stage_metrics(contract, logical_model)
    segments = segment_metrics(stages, lane_count)
    skip_rings = skip_plan(contract, lane_count)
    skip_summary = skip_memory_summary(skip_rings)
    local_memory_ok = all(lane["fits_64kb_local_staging"] for lane in lanes)
    max_segment = max(segments, key=lambda item: item["estimated_lane_cycles_at_256_i8_macs_per_cycle"])
    return {
        "schema": "com.apusr.fsr4_middle_core_tiling_plan.v1",
        "permutation": permutation,
        "hardware_profile": {"name": profile_name, **profile},
        "hardware_facts_used": {
            "aie_tile_data_memory_bytes": AIE_TILE_DATA_MEMORY_BYTES,
            "memory_tile_bytes": MEMORY_TILE_BYTES,
            "int8_macs_per_tile_cycle": MACS_PER_AIE_TILE_CYCLE_I8,
            "mac_accounting": "dense extracted INT8 weight tensors; grouped/depthwise shortcuts are forbidden unless the logical parameter pack proves that structure",
            "middle_core_token_bytes": TOKEN_BYTES,
            "local_staging_line_buffer_rows": LINE_BUFFER_ROWS,
            "horizontal_halo_tokens_per_side": HORIZONTAL_HALO_TOKENS,
            "lane_local_staging_token_cap_including_halo": LANE_LOCAL_STAGING_TOKEN_CAP,
        },
        "contract": {
            "token_bytes": TOKEN_BYTES,
            "tokens_per_row": int(contract["ingress"]["tensor"]["tokens_per_row"]),
            "tile_split": "contiguous spatial token-column lanes",
            "stage_pipeline": "four stage-group rows replicated across spatial lanes",
            "channel_split_policy": "deferred; split spatial lanes first because channel splits require cross-tile reductions/cascade and add stream pressure",
            "frame_bridge_policy": "4096-byte chunks stop at the frame transport boundary; compute lanes consume 256-byte row tokens",
        },
        "lanes": lanes,
        "stage_metrics": stages,
        "pipeline_segments": segments,
        "skip_rings": skip_rings,
        "skip_memory_summary": skip_summary,
        "tile_map": stage_group_tile_map(lanes),
        "physical_topology_note": "NPU logical rows are DMA row 0, memory-tile row 1, compute rows 2..5; stage-group rows map to compute rows 2..5 in the same spatial lane column.",
        "validation": {
            "local_memory_ok": local_memory_ok,
            "memory_tile_skip_rings_ok": bool(skip_summary["fits_one_memory_tile_per_lane"]),
            "estimated_bottleneck_segment": max_segment["segment"],
            "estimated_bottleneck_ms_at_1ghz": max_segment["estimated_lane_ms_at_1ghz"],
        },
    }


def generate_all(profile_name: str, out_root: Path) -> None:
    for permutation in permutation_names():
        plan = build_plan(permutation, profile_name)
        write_json(out_root / permutation / f"{profile_name}.json", plan)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("permutation", nargs="?", help="logical permutation name such as balanced__2160")
    parser.add_argument("--all", action="store_true", help="generate plans for every checked-in middle-core contract")
    parser.add_argument("--profile", choices=sorted(HARDWARE_PROFILES), default="am020_4x4_base")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.all:
        generate_all(args.profile, args.out_root)
        print(f"generated middle-core tiling plans for {args.profile} under {args.out_root}")
        return 0
    if not args.permutation:
        raise SystemExit("pass a permutation or --all")
    plan = build_plan(args.permutation, args.profile)
    output_path = args.out_root / args.permutation / f"{args.profile}.json"
    write_json(output_path, plan)
    print(f"generated {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
