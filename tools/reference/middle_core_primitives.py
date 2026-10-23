#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""NPU-shaped primitive decomposition for the FSR4 INT8 middle core.

This module is shared by the math-contract and tiling generators so stage MAC
counts cannot drift between the routing plan and the kernel bring-up plan.
"""

from __future__ import annotations

import math
from typing import Any

TOKEN_BYTES = 256
AIE_I8_MACS_PER_TILE_CYCLE = 256
AIE_DIRECT_STREAM_BYTES_PER_CYCLE = 4
AIE_VECTOR_LOAD_BYTES_PER_CYCLE = 32
AIE_VECTOR_STORE_BYTES_PER_CYCLE = 32


def ceil_div(num: int, den: int) -> int:
    if den <= 0:
        raise ValueError("division denominator must be positive")
    return (int(num) + int(den) - 1) // int(den)


def tensor_shape(tensor: dict[str, Any]) -> dict[str, int]:
    if "W" in tensor:
        width = int(tensor["W"])
        height = int(tensor["H"])
        channels = int(tensor.get("effective_C", tensor.get("C")))
        token_width_px = int(tensor["token_width_px"])
        tokens_per_row = int(tensor["tokens_per_row"])
        total_tokens = int(tensor["total_tokens"])
    else:
        logical_size = list(tensor["logical_size"])
        width = int(logical_size[0])
        height = int(logical_size[1])
        channels = int(logical_size[2])
        token_width_px = TOKEN_BYTES // channels
        tokens_per_row = ceil_div(width, token_width_px)
        total_tokens = tokens_per_row * height
    return {
        "width": width,
        "height": height,
        "channels": channels,
        "token_width_px": token_width_px,
        "tokens_per_row": tokens_per_row,
        "total_tokens": total_tokens,
        "pixels": width * height,
    }


def weight_dimensions(parameter: dict[str, Any]) -> dict[str, int]:
    names = [str(name) for name in parameter.get("descriptor_dim_names") or []]
    dims = [int(value) for value in parameter.get("logical_size") or []]
    by_name = dict(zip(names, dims, strict=True))
    return {
        "kernel_width": int(by_name.get("KW", by_name.get("W", 1))),
        "kernel_height": int(by_name.get("KH", by_name.get("H", 1))),
        "input_channels": int(by_name.get("IC", by_name.get("C", 1))),
        "output_channels": int(by_name.get("OC", by_name.get("N", by_name.get("C", 1)))),
    }


def weighted_macs(pixel_count: int, parameter: dict[str, Any]) -> int:
    dims = weight_dimensions(parameter)
    return (
        int(pixel_count)
        * dims["kernel_width"]
        * dims["kernel_height"]
        * dims["input_channels"]
        * dims["output_channels"]
    )


def dense_weight_primitive(
    *,
    name: str,
    stage_pass: int,
    parameter_name: str,
    parameter: dict[str, Any],
    pixel_count: int,
    output_tokens: int,
    math: str,
    consumes: str,
    produces: str,
    activation_dtype: str = "i8",
    weight_dtype: str = "s8",
) -> dict[str, Any]:
    dims = weight_dimensions(parameter)
    macs = weighted_macs(pixel_count, parameter)
    return {
        "name": name,
        "pass": int(stage_pass),
        "kind": "weighted_dense_i8_i8_acc32",
        "activation_dtype": activation_dtype,
        "weight_dtype": weight_dtype,
        "math": math,
        "parameter": parameter_name,
        "kernel_width": dims["kernel_width"],
        "kernel_height": dims["kernel_height"],
        "input_channels": dims["input_channels"],
        "output_channels": dims["output_channels"],
        "output_pixels": int(pixel_count),
        "output_tokens": int(output_tokens),
        "macs": macs,
        "compute_cycles_floor_at_256_i8_macs_per_cycle": ceil_div(macs, AIE_I8_MACS_PER_TILE_CYCLE),
        "consumes": consumes,
        "produces": produces,
    }


def epilogue_primitive(*, name: str, stage_pass: int, elements: int, math: str, consumes: str, produces: str) -> dict[str, Any]:
    return {
        "name": name,
        "pass": int(stage_pass),
        "kind": "epilogue_i32_to_i8",
        "math": math,
        "elements": int(elements),
        "vector_groups_32b": ceil_div(elements, 32),
        "compute_cycles_floor": ceil_div(elements, 32),
        "consumes": consumes,
        "produces": produces,
    }


def token_io_cycles(token_count: int) -> dict[str, int]:
    token_count = int(token_count)
    return {
        "tokens": token_count,
        "direct_stream_cycles_per_direction": token_count * ceil_div(TOKEN_BYTES, AIE_DIRECT_STREAM_BYTES_PER_CYCLE),
        "local_vector_load_cycles": token_count * ceil_div(TOKEN_BYTES, AIE_VECTOR_LOAD_BYTES_PER_CYCLE),
        "local_vector_store_cycles": token_count * ceil_div(TOKEN_BYTES, AIE_VECTOR_STORE_BYTES_PER_CYCLE),
    }


def build_stage_primitives(contract_stage: dict[str, Any], logical_stage: dict[str, Any]) -> list[dict[str, Any]]:
    stage_pass = int(contract_stage["pass"])
    op = str(contract_stage["op_fn"])
    primary = tensor_shape(contract_stage["primary"])
    output = tensor_shape(contract_stage["output"])
    params = logical_stage.get("parameters") or {}

    if op == "ConvNextBlock":
        return [
            dense_weight_primitive(
                name="spatial3x3_c16_to_c16",
                stage_pass=stage_pass,
                parameter_name="weights0",
                parameter=params["weights0"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="dense 3x3 spatial convolution over the C16 token carrier",
                consumes="primary_i8",
                produces="spatial_acc32",
            ),
            epilogue_primitive(
                name="spatial_bias_srs",
                stage_pass=stage_pass,
                elements=output["pixels"] * 16,
                math="bias0 + SRS/requant to signed i8 intermediate",
                consumes="spatial_acc32",
                produces="t0_i8",
            ),
            dense_weight_primitive(
                name="pointwise_c16_to_c32",
                stage_pass=stage_pass,
                parameter_name="weights1",
                parameter=params["weights1"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="1x1 expansion convolution",
                consumes="t0_i8",
                produces="expand_acc32",
            ),
            epilogue_primitive(
                name="relu_srs_c32",
                stage_pass=stage_pass,
                elements=output["pixels"] * 32,
                math="bias1 + SRS/requant + ReLU carrier",
                consumes="expand_acc32",
                produces="t1_relu_u8",
            ),
            dense_weight_primitive(
                name="pointwise_c32_to_c16",
                stage_pass=stage_pass,
                parameter_name="weights2",
                parameter=params["weights2"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="1x1 contraction convolution over the unsigned ReLU carrier and signed weights",
                consumes="t1_relu_u8",
                produces="contract_acc32",
                activation_dtype="u8",
            ),
            epilogue_primitive(
                name="residual_srs_c16",
                stage_pass=stage_pass,
                elements=output["pixels"] * 16,
                math="bias2 + accumulator-domain scaled residual add + SRS/requant to signed i8 output",
                consumes="contract_acc32 + primary_i8",
                produces="output_i8",
            ),
        ]

    if op == "FasterNetBlock":
        channels = output["channels"]
        spatial_dims = weight_dimensions(params["weights0"])
        return [
            dense_weight_primitive(
                name=f"partial_spatial3x3_c{spatial_dims['input_channels']}_to_c{spatial_dims['output_channels']}",
                stage_pass=stage_pass,
                parameter_name="weights0",
                parameter=params["weights0"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="dense grouped 3x3 spatial branch over the extracted FasterNet branch channels",
                consumes="primary_i8_spatial_branch",
                produces="spatial_branch_acc32",
            ),
            epilogue_primitive(
                name="partial_spatial_bias_srs",
                stage_pass=stage_pass,
                elements=output["pixels"] * spatial_dims["output_channels"],
                math="bias0 + SRS/requant for the spatial branch; bypass channels remain local",
                consumes="spatial_branch_acc32",
                produces="mixed_i8",
            ),
            dense_weight_primitive(
                name=f"pointwise_c{channels}_to_c{channels * 2}",
                stage_pass=stage_pass,
                parameter_name="weights1",
                parameter=params["weights1"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="1x1 expansion convolution",
                consumes="mixed_i8",
                produces="expand_acc32",
            ),
            epilogue_primitive(
                name=f"relu_srs_c{channels * 2}",
                stage_pass=stage_pass,
                elements=output["pixels"] * channels * 2,
                math="bias1 + SRS/requant + ReLU carrier",
                consumes="expand_acc32",
                produces="relu_u8",
            ),
            dense_weight_primitive(
                name=f"pointwise_c{channels * 2}_to_c{channels}",
                stage_pass=stage_pass,
                parameter_name="weights2",
                parameter=params["weights2"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="1x1 contraction convolution over the unsigned ReLU carrier and signed weights",
                consumes="relu_u8",
                produces="contract_acc32",
                activation_dtype="u8",
            ),
            epilogue_primitive(
                name=f"residual_srs_c{channels}",
                stage_pass=stage_pass,
                elements=output["pixels"] * channels,
                math="bias2 + accumulator-domain scaled residual add + SRS/requant to signed i8 output",
                consumes="contract_acc32 + primary_i8",
                produces="output_i8",
            ),
        ]

    if op == "FusedConv2D_k2s2b_QuantizedOutput":
        return [
            dense_weight_primitive(
                name=f"down2x2_c{primary['channels']}_to_c{output['channels']}",
                stage_pass=stage_pass,
                parameter_name="weights",
                parameter=params["weights"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="2x2 stride-2 dense convolution; channel expansion preserves row-token count",
                consumes="primary_i8_row_pair",
                produces="downsample_acc32",
            ),
            epilogue_primitive(
                name=f"downsample_srs_c{output['channels']}",
                stage_pass=stage_pass,
                elements=output["pixels"] * output["channels"],
                math="bias + split-scale SRS/requant to signed i8 output",
                consumes="downsample_acc32",
                produces="output_i8",
            ),
        ]

    if op == "FNB_CT2D_ADD":
        primary_channels = primary["channels"]
        output_channels = output["channels"]
        spatial_dims = weight_dimensions(params["weights0"])
        return [
            dense_weight_primitive(
                name=f"partial_spatial3x3_c{spatial_dims['input_channels']}_to_c{spatial_dims['output_channels']}",
                stage_pass=stage_pass,
                parameter_name="weights0",
                parameter=params["weights0"],
                pixel_count=primary["pixels"],
                output_tokens=primary["total_tokens"],
                math="low-scale grouped FasterNet spatial branch before CT2D",
                consumes="primary_i8_spatial_branch",
                produces="spatial_branch_acc32",
            ),
            epilogue_primitive(
                name="partial_spatial_bias_srs",
                stage_pass=stage_pass,
                elements=primary["pixels"] * spatial_dims["output_channels"],
                math="bias0 + SRS/requant for the spatial branch; bypass channels remain local",
                consumes="spatial_branch_acc32",
                produces="mixed_i8",
            ),
            dense_weight_primitive(
                name=f"pointwise_c{primary_channels}_to_c{primary_channels * 2}",
                stage_pass=stage_pass,
                parameter_name="weights1",
                parameter=params["weights1"],
                pixel_count=primary["pixels"],
                output_tokens=primary["total_tokens"],
                math="low-scale 1x1 expansion convolution",
                consumes="mixed_i8",
                produces="expand_acc32",
            ),
            epilogue_primitive(
                name=f"relu_srs_c{primary_channels * 2}",
                stage_pass=stage_pass,
                elements=primary["pixels"] * primary_channels * 2,
                math="bias1 + SRS/requant + ReLU carrier",
                consumes="expand_acc32",
                produces="relu_u8",
            ),
            dense_weight_primitive(
                name=f"pointwise_c{primary_channels * 2}_to_c{primary_channels}",
                stage_pass=stage_pass,
                parameter_name="weights2",
                parameter=params["weights2"],
                pixel_count=primary["pixels"],
                output_tokens=primary["total_tokens"],
                math="low-scale 1x1 contraction convolution over the unsigned ReLU carrier and signed weights",
                consumes="relu_u8",
                produces="fnb_acc32",
                activation_dtype="u8",
            ),
            epilogue_primitive(
                name=f"fnb_residual_srs_c{primary_channels}",
                stage_pass=stage_pass,
                elements=primary["pixels"] * primary_channels,
                math="bias2 + accumulator-domain scaled low-scale residual add + SRS/requant before CT2D",
                consumes="fnb_acc32 + primary_i8",
                produces="fnb_i8",
            ),
            dense_weight_primitive(
                name=f"ct2d2x2_c{primary_channels}_to_c{output_channels}",
                stage_pass=stage_pass,
                parameter_name="ct2d_weights",
                parameter=params["ct2d_weights"],
                pixel_count=output["pixels"],
                output_tokens=output["total_tokens"],
                math="2x2 transposed-convolution/upscale to the skip domain",
                consumes="fnb_i8",
                produces="ct2d_acc32",
            ),
            epilogue_primitive(
                name=f"skip_add_srs_c{output_channels}",
                stage_pass=stage_pass,
                elements=output["pixels"] * output_channels,
                math="ct2d_bias + accumulator-domain scaled skip add + SRS/requant to signed i8 output",
                consumes="ct2d_acc32 + skip_i8",
                produces="output_i8",
            ),
        ]

    raise ValueError(f"unsupported middle-core op: {op}")


def stage_macs(contract_stage: dict[str, Any], logical_stage: dict[str, Any]) -> dict[str, Any]:
    primitives = build_stage_primitives(contract_stage, logical_stage)
    weighted = [primitive for primitive in primitives if primitive["kind"] == "weighted_dense_i8_i8_acc32"]
    primitive_macs = {str(primitive["parameter"]): int(primitive["macs"]) for primitive in weighted}
    return {"primitive_macs": primitive_macs, "total_macs": sum(primitive_macs.values())}


def lane_stage_cycles(contract_stage: dict[str, Any], logical_stage: dict[str, Any], lane_token_cols: int) -> dict[str, Any]:
    output = tensor_shape(contract_stage["output"])
    full_tokens_per_row = output["tokens_per_row"]
    if full_tokens_per_row <= 0:
        raise ValueError("stage output tokens_per_row must be positive")
    lane_fraction_num = int(lane_token_cols)
    lane_fraction_den = int(full_tokens_per_row)
    primitives = build_stage_primitives(contract_stage, logical_stage)
    lane_primitives: list[dict[str, Any]] = []
    total_compute_cycles = 0
    for primitive in primitives:
        item = dict(primitive)
        if primitive["kind"] == "weighted_dense_i8_i8_acc32":
            lane_macs = ceil_div(int(primitive["macs"]) * lane_fraction_num, lane_fraction_den)
            item["lane_macs"] = lane_macs
            item["lane_compute_cycles_floor_at_256_i8_macs_per_cycle"] = ceil_div(lane_macs, AIE_I8_MACS_PER_TILE_CYCLE)
            total_compute_cycles += int(item["lane_compute_cycles_floor_at_256_i8_macs_per_cycle"])
        else:
            lane_elements = ceil_div(int(primitive["elements"]) * lane_fraction_num, lane_fraction_den)
            item["lane_elements"] = lane_elements
            item["lane_compute_cycles_floor"] = ceil_div(lane_elements, 32)
        lane_primitives.append(item)
    lane_output_tokens = int(lane_token_cols) * output["height"]
    io = token_io_cycles(lane_output_tokens)
    return {
        "lane_token_cols": int(lane_token_cols),
        "lane_output_tokens": lane_output_tokens,
        "compute_cycles_floor": total_compute_cycles,
        "token_io_cycles": io,
        "primitives": lane_primitives,
    }
