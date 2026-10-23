#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the NPU-shaped math and cycle ledger for the FSR4 middle core.

This freezes what the AIE kernels must compute, primitive by primitive. It also
records deterministic lower-bound cycle estimates for compute, local movement,
stream movement, and skip-delay rings. The counts are not a scheduler promise;
they are the contract used to decide where the first real MLIR-AIE kernels must
land and what must be measured once those kernels exist.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "reference"))
from middle_core_primitives import (  # noqa: E402
    AIE_DIRECT_STREAM_BYTES_PER_CYCLE,
    AIE_I8_MACS_PER_TILE_CYCLE,
    AIE_VECTOR_LOAD_BYTES_PER_CYCLE,
    AIE_VECTOR_STORE_BYTES_PER_CYCLE,
    TOKEN_BYTES,
    build_stage_primitives,
    lane_stage_cycles,
    token_io_cycles,
)

MODEL_ROOT = REPO_ROOT / "models" / "fsr4_int8"
CONTRACT_ROOT = MODEL_ROOT / "planning" / "middle_core_contracts"
LOGICAL_ROOT = MODEL_ROOT / "logical"
TILING_ROOT = MODEL_ROOT / "planning" / "middle_core_tiling"
PARAM_ROOT = MODEL_ROOT / "planning" / "npu_parameter_packages"
DEFAULT_OUT_ROOT = MODEL_ROOT / "planning" / "middle_core_math_contracts"
SCHEMA = "com.apusr.fsr4_middle_core_math_contract.v0"
MEMORY_TILE_BYTES = 512 * 1024
REFERENCE_CLOCK_HZ = 1_000_000_000


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def permutation_names() -> list[str]:
    return sorted(path.parent.name for path in CONTRACT_ROOT.glob("*/middle_core_contract.json"))


def profile_names(permutation: str) -> list[str]:
    root = TILING_ROOT / permutation
    return sorted(path.stem for path in root.glob("*.json"))


def logical_stage_map(logical_model: dict[str, Any]) -> dict[int, dict[str, Any]]:
    return {
        int(stage["pass_index"]): stage
        for stage in logical_model.get("stages") or []
        if stage.get("pass_index") is not None
    }


def lane_cycle_ledgers(contract: dict[str, Any], logical_model: dict[str, Any], tiling: dict[str, Any]) -> list[dict[str, Any]]:
    logical_by_pass = logical_stage_map(logical_model)
    ledgers: list[dict[str, Any]] = []
    for lane in tiling["lanes"]:
        lane_token_cols = int(lane["token_cols"])
        stages: list[dict[str, Any]] = []
        for stage in contract["stages"]:
            stage_pass = int(stage["pass"])
            cycle_info = lane_stage_cycles(stage, logical_by_pass[stage_pass], lane_token_cols)
            stages.append(
                {
                    "pass": stage_pass,
                    "op": stage["op_fn"],
                    "lane_output_tokens": cycle_info["lane_output_tokens"],
                    "compute_cycles_floor": cycle_info["compute_cycles_floor"],
                    "output_store_cycles_floor": cycle_info["token_io_cycles"]["local_vector_store_cycles"],
                    "direct_stream_cycles_per_direction_floor": cycle_info["token_io_cycles"]["direct_stream_cycles_per_direction"],
                    "primitives": cycle_info["primitives"],
                }
            )
        ledgers.append(
            {
                "lane": int(lane["lane"]),
                "token_col_range": [int(lane["start_token_col"]), int(lane["end_token_col"])],
                "token_cols": lane_token_cols,
                "staging_token_cols_with_halo": int(lane["staging_token_cols"]),
                "line_buffer_bytes": int(lane["line_buffer_bytes"]),
                "stages": stages,
            }
        )
    return ledgers


def stage_contracts(contract: dict[str, Any], logical_model: dict[str, Any]) -> list[dict[str, Any]]:
    logical_by_pass = logical_stage_map(logical_model)
    stages: list[dict[str, Any]] = []
    for stage in contract["stages"]:
        stage_pass = int(stage["pass"])
        primitives = build_stage_primitives(stage, logical_by_pass[stage_pass])
        weighted_macs = sum(int(primitive.get("macs", 0)) for primitive in primitives)
        stages.append(
            {
                "pass": stage_pass,
                "op": stage["op_fn"],
                "kind": stage["kind"],
                "primary": stage["primary"],
                "skip": stage.get("skip"),
                "output": stage["output"],
                "token_cadence": stage["token_cadence"],
                "weighted_macs": weighted_macs,
                "full_frame_compute_cycles_floor_at_256_i8_macs_per_cycle": (weighted_macs + AIE_I8_MACS_PER_TILE_CYCLE - 1)
                // AIE_I8_MACS_PER_TILE_CYCLE,
                "primitives": primitives,
            }
        )
    return stages


def segment_summary(tiling: dict[str, Any], lane_ledgers: list[dict[str, Any]]) -> list[dict[str, Any]]:
    passes_by_segment = {tuple(item["passes"]): item["segment"] for item in tiling["pipeline_segments"]}
    summaries: list[dict[str, Any]] = []
    for segment in tiling["pipeline_segments"]:
        passes = [int(value) for value in segment["passes"]]
        max_lane_cycles = 0
        for lane in lane_ledgers:
            lane_cycles = sum(int(stage["compute_cycles_floor"]) for stage in lane["stages"] if int(stage["pass"]) in passes)
            max_lane_cycles = max(max_lane_cycles, lane_cycles)
        summaries.append(
            {
                "segment": passes_by_segment[tuple(passes)],
                "passes": passes,
                "max_lane_compute_cycles_floor": max_lane_cycles,
                "max_lane_compute_ms_floor_at_1ghz": max_lane_cycles / (REFERENCE_CLOCK_HZ / 1000),
            }
        )
    return summaries


def resident_parameter_package_bytes(param_package: dict[str, Any]) -> int:
    storage = param_package.get("storage_policy") or {}
    return int(storage.get("per_macro_lane_package_allocation_bytes", storage.get("per_lane_package_allocation_bytes", 0)))


def resident_memory_strip_bytes(param_package: dict[str, Any]) -> int:
    storage = param_package.get("storage_policy") or {}
    return int(storage.get("memory_tile_bytes_per_macro_lane", MEMORY_TILE_BYTES))


def skip_delay_ledgers(contract: dict[str, Any], tiling: dict[str, Any], param_package: dict[str, Any]) -> list[dict[str, Any]]:
    per_lane_package = resident_parameter_package_bytes(param_package)
    tiling_skip_by_name = {skip["name"]: skip for skip in tiling["skip_rings"]}
    ledgers: list[dict[str, Any]] = []
    for skip in contract["skip_fifos"]:
        tiling_skip = tiling_skip_by_name[skip["name"]]
        per_lane_tokens = int(tiling_skip["per_lane_recommended_tokens"])
        per_lane_ring_bytes = int(tiling_skip["per_lane_ring_bytes"])
        io = token_io_cycles(per_lane_tokens)
        ledgers.append(
            {
                "name": skip["name"],
                "src_pass": int(skip["src_pass"]),
                "dst_pass": int(skip["dst_pass"]),
                "delay_model": skip["delay_model"],
                "initial_delay_rows": int(skip["initial_delay_rows"]),
                "initial_delay_tokens_full_frame": int(skip["initial_delay_tokens"]),
                "per_lane_recommended_tokens": per_lane_tokens,
                "per_lane_ring_bytes": per_lane_ring_bytes,
                "per_lane_write_cycles_floor": io["direct_stream_cycles_per_direction"],
                "per_lane_read_cycles_floor": io["direct_stream_cycles_per_direction"],
                "memory_tile_bytes": MEMORY_TILE_BYTES,
                "macro_lane_memory_strip_bytes": resident_memory_strip_bytes(param_package),
                "fits_one_memory_tile_per_lane_before_parameters": per_lane_ring_bytes <= MEMORY_TILE_BYTES,
                "fits_one_memory_tile_per_lane_with_replicated_parameter_package": per_lane_ring_bytes + per_lane_package <= MEMORY_TILE_BYTES,
                "fits_macro_lane_memory_strip_with_resident_parameter_package": per_lane_ring_bytes + per_lane_package <= resident_memory_strip_bytes(param_package),
            }
        )
    return ledgers


def delay_ring_summary(skip_ledgers: list[dict[str, Any]], param_package: dict[str, Any]) -> dict[str, Any]:
    per_lane_package = resident_parameter_package_bytes(param_package)
    single_buffer = sum(int(skip["per_lane_ring_bytes"]) for skip in skip_ledgers)
    double_buffer = single_buffer * 2
    return {
        "per_lane_parameter_package_bytes": per_lane_package,
        "per_macro_lane_parameter_package_bytes": per_lane_package,
        "per_lane_skip_ring_single_buffer_bytes": single_buffer,
        "per_lane_skip_ring_double_buffer_bytes": double_buffer,
        "per_lane_total_with_single_buffer_bytes": per_lane_package + single_buffer,
        "per_lane_total_with_double_buffer_bytes": per_lane_package + double_buffer,
        "per_macro_lane_total_with_single_buffer_bytes": per_lane_package + single_buffer,
        "per_macro_lane_total_with_double_buffer_bytes": per_lane_package + double_buffer,
        "memory_tile_bytes": MEMORY_TILE_BYTES,
        "macro_lane_memory_strip_bytes": resident_memory_strip_bytes(param_package),
        "single_buffer_fits_with_parameters": per_lane_package + single_buffer <= MEMORY_TILE_BYTES,
        "double_buffer_fits_with_parameters": per_lane_package + double_buffer <= MEMORY_TILE_BYTES,
        "single_buffer_fits_macro_lane_strip_with_parameters": per_lane_package + single_buffer <= resident_memory_strip_bytes(param_package),
        "double_buffer_fits_macro_lane_strip_with_parameters": per_lane_package + double_buffer <= resident_memory_strip_bytes(param_package),
        "policy": "The product mapping stores parameters as macro-lane resident pages in the two-memory-tile strip. Skip rings stay on-chip and use the paired memory tiles rather than per-column private SRAM.",
    }


def datapath_trace(contract: dict[str, Any], tiling: dict[str, Any]) -> list[dict[str, Any]]:
    lanes = tiling["lanes"]
    lane_count = len(lanes)
    return [
        {
            "step": 0,
            "node": "frame_conveyor_ingress",
            "operation": "GPU-exported C16 frame slot becomes NPU ingress payload",
            "tile_scope": "DMA/interface row",
        },
        {
            "step": 1,
            "node": "lane_distributor",
            "operation": f"split each row into {lane_count} contiguous spatial token lanes plus horizontal halos",
            "tile_scope": "DMA row to same-column compute lanes",
        },
        {
            "step": 2,
            "node": "compute_row_2",
            "operation": "passes 1, 2, 3 consume C16 tokens, tap skipA after pass 2, emit C32 downsample tokens",
            "tile_scope": "one tile per lane",
        },
        {
            "step": 3,
            "node": "compute_row_3",
            "operation": "passes 4, 5, 6 consume C32 tokens, tap skipB after pass 5, emit C64 downsample tokens",
            "tile_scope": "same-lane adjacent compute tile",
        },
        {
            "step": 4,
            "node": "compute_row_4",
            "operation": "passes 7, 8, 9 consume C64 tokens, read aligned skipB, emit C32 upsample tokens",
            "tile_scope": "same-lane adjacent compute tile plus per-lane memory-tile ring read",
        },
        {
            "step": 5,
            "node": "compute_row_5",
            "operation": "passes 10, 11, 12 consume C32 tokens, read aligned skipA, emit C16 egress tokens",
            "tile_scope": "same-lane adjacent compute tile plus per-lane memory-tile ring read",
        },
        {
            "step": 6,
            "node": "frame_conveyor_egress",
            "operation": "stage12 C16 model egress frame slot becomes GPU stage13/post input",
            "tile_scope": "DMA/interface row",
        },
    ]


def build_contract(permutation: str, profile: str) -> dict[str, Any]:
    contract = load_json(CONTRACT_ROOT / permutation / "middle_core_contract.json")
    logical_model = load_json(LOGICAL_ROOT / permutation / "logical_model.json")
    tiling = load_json(TILING_ROOT / permutation / f"{profile}.json")
    param_package = load_json(PARAM_ROOT / permutation / f"{profile}.json")
    stages = stage_contracts(contract, logical_model)
    lanes = lane_cycle_ledgers(contract, logical_model, tiling)
    segments = segment_summary(tiling, lanes)
    skip_ledgers = skip_delay_ledgers(contract, tiling, param_package)
    return {
        "schema": SCHEMA,
        "permutation": permutation,
        "hardware_profile": tiling["hardware_profile"],
        "reference_clock_hz": REFERENCE_CLOCK_HZ,
        "hardware_cycle_constants": {
            "token_bytes": TOKEN_BYTES,
            "i8_macs_per_tile_cycle": AIE_I8_MACS_PER_TILE_CYCLE,
            "direct_stream_bytes_per_cycle": AIE_DIRECT_STREAM_BYTES_PER_CYCLE,
            "vector_load_bytes_per_cycle": AIE_VECTOR_LOAD_BYTES_PER_CYCLE,
            "vector_store_bytes_per_cycle": AIE_VECTOR_STORE_BYTES_PER_CYCLE,
            "mac_accounting": "dense extracted INT8 tensors; do not collapse 3x3 weights to depthwise/grouped MACs unless a later pack format explicitly proves that sparsity/structure",
        },
        "bookend_split": contract["bookends"],
        "ingress": contract["ingress"],
        "egress": contract["egress"],
        "stages": stages,
        "lane_cycle_ledgers": lanes,
        "pipeline_segment_cycle_summary": segments,
        "skip_delay_rings": skip_ledgers,
        "delay_ring_memory_summary": delay_ring_summary(skip_ledgers, param_package),
        "datapath_trace": datapath_trace(contract, tiling),
        "promotion_policy": {
            "kernel_body_must_match_primitive_math": True,
            "stage_local_pass_is_not_product_proof": True,
            "full_product_proof_requires_stage12_model_egress": True,
            "cycle_estimates_are_lower_bounds_until_mlir_aie_object_code_is_measured": True,
        },
    }


def generate(permutations: list[str], profiles: list[str] | None, out_root: Path) -> list[Path]:
    outputs: list[Path] = []
    for permutation in permutations:
        selected_profiles = profiles or profile_names(permutation)
        for profile in selected_profiles:
            payload = build_contract(permutation, profile)
            path = out_root / permutation / f"{profile}.json"
            write_json(path, payload)
            outputs.append(path)
    return outputs


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("permutation", nargs="?", help="logical permutation name such as balanced__2160")
    parser.add_argument("--all", action="store_true", help="generate math contracts for every middle-core permutation")
    parser.add_argument("--profile", action="append", help="hardware profile. Repeat or omit to use all profiles for each permutation")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.all:
        permutations = permutation_names()
    elif args.permutation:
        permutations = [args.permutation]
    else:
        raise SystemExit("pass a permutation or --all")
    outputs = generate(permutations, args.profile, args.out_root)
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
