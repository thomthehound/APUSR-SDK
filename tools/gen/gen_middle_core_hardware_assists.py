#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate hardware-assist policy plans for the FSR4 INT8 middle core.

This plan is separate from the tiling plan because hardware assists are not all
placement decisions. SRS/requant epilogues, memory-tile zero padding, 4D DMA,
raw intrinsics, and future sparse compression change how kernels should be
written and how budgets should be interpreted. They do not change the 256-byte
middle-core token ABI.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
MODEL_ROOT = REPO_ROOT / "models" / "fsr4_int8"
LOGICAL_ROOT = MODEL_ROOT / "logical"
TILING_ROOT = MODEL_ROOT / "planning" / "middle_core_tiling"
DEFAULT_OUT_ROOT = MODEL_ROOT / "planning" / "middle_core_hardware_assists"
SCHEMA = "com.apusr.fsr4_middle_core_hardware_assist_plan.v1"
TOKEN_BYTES = 256
CHANNEL_VECTOR_GRANULARITY = 16
VECTOR_LOAD_STORE_ALIGNMENT_BYTES = 32
AIE_ML_COMPRESSION_CHUNK_BYTES = 32

# UG1603 exposes these limits in words for memory-tile zero padding.
MEMTILE_ZERO_PAD_LIMIT_WORDS = {
    "dimension0": 64,
    "dimension1": 32,
    "dimension2": 16,
}

RELU_CARRIER_CANDIDATE_OPS = {
    "ConvNextBlock",
    "FasterNetBlock",
    "FNB_CT2D_ADD",
}

SPATIAL_OPS = {
    "ConvNextBlock",
    "FasterNetBlock",
    "FusedConv2D_k2s2b_QuantizedOutput",
    "FNB_CT2D_ADD",
}


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def permutation_names() -> list[str]:
    return sorted({path.parent.name for path in TILING_ROOT.glob("*/*.json")})


def weight_dimensions(parameter: dict[str, Any]) -> dict[str, int]:
    names = list(parameter.get("descriptor_dim_names") or [])
    dims = list(parameter.get("logical_size") or [])
    dim_by_name = {str(name): int(value) for name, value in zip(names, dims, strict=True)}
    kernel_width = dim_by_name.get("KW", dim_by_name.get("W", 1))
    kernel_height = dim_by_name.get("KH", dim_by_name.get("H", 1))
    input_channels = dim_by_name.get("IC", dim_by_name.get("C", 1))
    output_channels = dim_by_name.get("OC", dim_by_name.get("N", input_channels))
    return {
        "kernel_width": int(kernel_width),
        "kernel_height": int(kernel_height),
        "input_channels": int(input_channels),
        "output_channels": int(output_channels),
    }


def implementation_candidates(dims: dict[str, int]) -> list[dict[str, str]]:
    kernel_width = dims["kernel_width"]
    kernel_height = dims["kernel_height"]
    input_channels = dims["input_channels"]
    output_channels = dims["output_channels"]
    if kernel_width == 1 and kernel_height == 1:
        return [
            {
                "candidate": "aie::mmul",
                "reason": "Pointwise CxN work maps naturally to blocked matrix multiplication when C/N packing matches supported AIE-ML shapes.",
            },
            {
                "candidate": "raw mac_4x8_8x8 / addmac_4x8_8x8 intrinsics",
                "reason": "Use raw intrinsics for small, fused, or awkward pointwise blocks where mmul packing costs more than it saves.",
            },
        ]
    if input_channels == output_channels and input_channels <= 128:
        return [
            {
                "candidate": "aie::sliding_mul_ch / aie::sliding_mac_ch",
                "reason": "Multi-channel sliding multiplication is the first candidate for channel-first spatial windows.",
            },
            {
                "candidate": "raw mac_4x8_8x8 with explicit window staging",
                "reason": "Keep this route when sliding_mul_ch cannot express the fused layout, stride, or epilogue cleanly.",
            },
        ]
    return [
        {
            "candidate": "staged window + aie::mmul or raw mac_4x8_8x8",
            "reason": "Spatial convolution with channel expansion/contraction probably needs explicit staging before a blocked MAC core.",
        },
        {
            "candidate": "aie::sliding_mul_ch measurement candidate",
            "reason": "Still worth benchmarking if the channel-first register layout can be reached without expensive shuffles.",
        },
    ]


def stage_weight_summary(stage: dict[str, Any]) -> list[dict[str, Any]]:
    weights: list[dict[str, Any]] = []
    for name, parameter in sorted((stage.get("parameters") or {}).items()):
        if parameter.get("role") != "weight":
            continue
        dims = weight_dimensions(parameter)
        weights.append(
            {
                "name": name,
                **dims,
                "spatial_kernel": bool(dims["kernel_width"] > 1 or dims["kernel_height"] > 1),
                "input_channels_multiple_of_16": dims["input_channels"] % CHANNEL_VECTOR_GRANULARITY == 0,
                "output_channels_multiple_of_16": dims["output_channels"] % CHANNEL_VECTOR_GRANULARITY == 0,
                "implementation_candidates": implementation_candidates(dims),
            }
        )
    return weights


def quantization_scalar_names(stage: dict[str, Any]) -> list[str]:
    names: list[str] = []
    for name in sorted((stage.get("scalar_args") or {}).keys()):
        lowered = str(name).lower()
        if "scale" in lowered or "quant" in lowered or "rcpr" in lowered:
            names.append(str(name))
    return names


def build_stage_assists(stage: dict[str, Any]) -> dict[str, Any]:
    pass_index = int(stage["pass_index"])
    op = str(stage["op"])
    weights = stage_weight_summary(stage)
    has_spatial_weight = any(bool(item["spatial_kernel"]) for item in weights)
    channel_padding_required = any(
        not bool(item["input_channels_multiple_of_16"] and item["output_channels_multiple_of_16"])
        for item in weights
    )
    quant_scalars = quantization_scalar_names(stage)
    return {
        "pass": pass_index,
        "op": op,
        "weights": weights,
        "quantization_scalar_args": quant_scalars,
        "primitive_coding_candidates": {
            "prefer_aie_api_first": True,
            "raw_intrinsics_are_allowed_when_api_shape_costs_more": True,
            "native_u8xs8_int8_mac_available": True,
            "native_accumulator_type_for_8x8": "v32acc32",
            "vector_load_store_alignment_bytes": VECTOR_LOAD_STORE_ALIGNMENT_BYTES,
        },
        "srs_requant_epilogue": {
            "required_for_int8_product_kernel": bool(quant_scalars),
            "policy": "Use AIE SRS/requant/saturation in the primitive epilogue; do not spill a widened intermediate just to quantize it later.",
            "unsigned_srs_candidate_for_relu": op in RELU_CARRIER_CANDIDATE_OPS,
            "unsafe_negative_shift_policy": "Do not generate SRS shifts -4..-2 unless the primitive explicitly proves truncation or saturation-against-zero semantics.",
            "u8_relu_carrier_candidate": op in RELU_CARRIER_CANDIDATE_OPS,
            "u8_relu_policy": "Allowed only on explicitly marked internal ReLU handoffs whose consumer uses the u8 activation x s8 weight contract; signed/parity cases remain available.",
        },
        "zero_padding": {
            "algorithmic_padding_candidate": op in SPATIAL_OPS and has_spatial_weight,
            "channel_granularity_padding_required_now": channel_padding_required,
            "memory_tile_zero_padding_limits_words": MEMTILE_ZERO_PAD_LIMIT_WORDS,
            "one_c16_token_is_64_32bit_words": True,
            "one_token_edge_pad_fits_dimension0_limit": True,
            "policy": "Use memory-tile MM2S zero-padding for documented zero-pad edges or channel-granularity padding. Do not use it for real lane-neighbor halos or nonzero edge semantics.",
            "interior_lane_halos_are_real_data": bool(has_spatial_weight),
        },
        "data_movement_assists": {
            "use_4d_dma_for_regular_rows_and_skip_rings": True,
            "memory_tile_repetition_candidate": True,
            "primary_same_lane_handoff": "nearest-neighbor shared memory and locks before routed streams; tile DMA is not the primary adjacent-row path",
            "kernel_pointer_addressing_candidates": ["add_2d", "add_3d", "aie::begin_vector", "aie::load_v"],
        },
        "compression": {
            "base_plan_depends_on_compression": False,
            "future_candidate": bool(op in RELU_CARRIER_CANDIDATE_OPS),
            "aie_ml_compression_chunk_bytes": AIE_ML_COMPRESSION_CHUNK_BYTES,
            "sparse_mmul_requires_exact_50_percent_sparsity": True,
            "policy": "Do not include sparse-compression savings in base sizing until MLIR-AIE exposes the feature and measurements show a win.",
        },
    }


def build_feature_catalog() -> dict[str, Any]:
    return {
        "programming_model": {
            "preferred_surface": "AI Engine API through aie_api/aie.hpp",
            "reason": "The API is portable across AI Engine variants and lowers to efficient low-level intrinsics. Raw intrinsics stay available for measured kernels.",
        },
        "integer_mac": {
            "native_u8xs8_mac": True,
            "raw_intrinsics": ["mul_4x8_8x8", "mac_4x8_8x8", "addmac_4x8_8x8"],
            "accumulator": "v32acc32",
            "policy": "The measured u8 activation x s8 weight carrier is a native AIE-ML path, not a software emulation trick.",
        },
        "matrix_and_window_apis": {
            "pointwise_candidate": "aie::mmul",
            "spatial_candidate": "aie::sliding_mul_ch / aie::sliding_mac_ch",
            "policy": "Benchmark API and raw-intrinsic forms for each primitive before promoting the product kernel body.",
        },
        "srs": {
            "ssrs_i32_to_i8": True,
            "ussrs_i32_to_u8": True,
            "shift_range": "-4..59",
            "unsafe_negative_shifts": [-4, -3, -2],
            "policy": "Use SRS for the epilogue; avoid unsafe negative shifts unless the primitive explicitly proves the documented safe cases.",
        },
        "loads_and_addressing": {
            "vector_load_store_alignment_bytes": VECTOR_LOAD_STORE_ALIGNMENT_BYTES,
            "plain_pointer_vector_load_store_supported": True,
            "kernel_2d_3d_address_intrinsics": ["add_2d_ptr", "add_2d_byte", "add_3d_ptr", "add_3d_byte"],
        },
        "memory_tile_dma": {
            "zero_padding_supported": True,
            "zero_padding_supported_dimensions": [0, 1, 2],
            "zero_padding_limit_words_before_or_after": MEMTILE_ZERO_PAD_LIMIT_WORDS,
            "repetition_candidate": True,
            "dma_dimensions": 4,
        },
        "sparsity_and_compression": {
            "activation_compression_chunk_bytes": AIE_ML_COMPRESSION_CHUNK_BYTES,
            "sparse_mmul_candidate": True,
            "sparse_mmul_requires_exact_50_percent_sparsity": True,
            "base_plan_credit": 0,
        },
    }


def build_plan(permutation: str, profile: str) -> dict[str, Any]:
    tiling_path = TILING_ROOT / permutation / f"{profile}.json"
    logical_path = LOGICAL_ROOT / permutation / "logical_model.json"
    tiling = load_json(tiling_path)
    logical = load_json(logical_path)
    stage_assists = [
        build_stage_assists(stage)
        for stage in logical.get("stages", [])
        if stage.get("kind") == "operator_call" and stage.get("pass_index") is not None and 1 <= int(stage["pass_index"]) <= 12
    ]

    max_line_buffer_bytes = max(int(lane["line_buffer_bytes"]) for lane in tiling["lanes"])
    max_staging_token_cols = max(int(lane["staging_token_cols"]) for lane in tiling["lanes"])
    any_channel_padding = any(
        bool(stage["zero_padding"]["channel_granularity_padding_required_now"])
        for stage in stage_assists
    )
    return {
        "schema": SCHEMA,
        "permutation": permutation,
        "hardware_profile": tiling["hardware_profile"],
        "feature_catalog": build_feature_catalog(),
        "contract": {
            "middle_core_token_bytes": TOKEN_BYTES,
            "frame_bridge_chunks_are_not_compute_tokens": True,
            "compute_lane_split": tiling["contract"]["tile_split"],
        },
        "calculation_policy": {
            "srs_requant": "Count requant/SRS as an epilogue operation in primitive timing; do not budget an extra stream, frame spill, or widened activation buffer for it.",
            "u8_relu_carrier": "The default product path may use full-range u8 ReLU carriers on explicit internal ReLU handoffs, matching the measured APUSR/FSR4 experiment. Debug/parity paths must be able to use the signed reference carrier.",
            "memory_tile_zero_padding": "Global zero-pad and channel-granularity padding are generated by memory-tile DMA where the operator contract permits it. Worst-case local staging still counts interior lane halos because they are real neighboring tokens.",
            "compression": "Sparse compression is a later optimization. The base plan carries zero compression credit.",
            "api_vs_raw_intrinsics": "Start with AI Engine API candidates, then keep raw intrinsics when the API shape forces extra shuffles, spills, or unfused epilogues.",
        },
        "local_staging_budget_interpretation": {
            "max_staging_token_cols_including_real_lane_halos": max_staging_token_cols,
            "max_line_buffer_bytes_including_real_lane_halos": max_line_buffer_bytes,
            "image_edge_zero_padding_storage_credit_bytes": 0,
            "reason": "The product budget is the steady-state interior lane case. Hardware zero padding removes edge-padding materialization and branches, but it does not shrink the interior line-buffer bound.",
        },
        "channel_granularity_padding": {
            "vector_granularity_channels": CHANNEL_VECTOR_GRANULARITY,
            "required_by_current_model": any_channel_padding,
            "current_model_padding_credit_bytes": 0,
            "reason": "Current middle-core channel counts are already multiples of 16. Keep the policy because future models may not be.",
        },
        "stage_assists": stage_assists,
    }


def generate_all(profile: str, out_root: Path) -> None:
    for permutation in permutation_names():
        if not (TILING_ROOT / permutation / f"{profile}.json").exists():
            continue
        write_json(out_root / permutation / f"{profile}.json", build_plan(permutation, profile))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("permutation", nargs="?", help="logical permutation name such as balanced__2160")
    parser.add_argument("--all", action="store_true", help="generate hardware-assist plans for every checked-in tiling plan")
    parser.add_argument("--profile", default="am020_4x4_base", help="tiling profile name")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.all:
        generate_all(args.profile, args.out_root)
        print(f"generated middle-core hardware-assist plans for {args.profile} under {args.out_root}")
        return 0
    if not args.permutation:
        raise SystemExit("pass a permutation or --all")
    output_path = args.out_root / args.permutation / f"{args.profile}.json"
    write_json(output_path, build_plan(args.permutation, args.profile))
    print(f"generated {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
