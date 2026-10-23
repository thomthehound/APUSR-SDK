#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate APUSR kernel manifests."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any


REQUIRED_TOP_LEVEL = {
    "schema_version",
    "name",
    "kernel_name",
    "artifact",
    "target",
    "operator",
    "xrt_abi",
    "sources",
    "mlir_source",
}

REQUIRED_OPERATOR = {
    "type",
    "filter_width",
    "filter_height",
    "input_halo_rows",
    "input_halo_columns",
    "line_buffer_rows",
    "default_token_band_rows",
    "default_token_ring_depth",
    "default_microtile_output_width",
    "default_microtile_output_rows",
    "token_shape",
    "live_state_policy",
    "output_cadence",
}

REQUIRED_XRT_ABI = {
    "opcode_arg_index",
    "instruction_buffer_arg_index",
    "instruction_count_arg_index",
    "control_ring_buffer_arg_index",
    "input_token_buffer_arg_index",
    "output_token_buffer_arg_index",
    "band_index_arg_index",
    "band_count_arg_index",
    "input_first_row_arg_index",
    "input_row_count_arg_index",
    "output_first_row_arg_index",
    "output_row_count_arg_index",
    "render_width_arg_index",
    "render_height_arg_index",
    "first_band_flag_arg_index",
    "last_band_flag_arg_index",
}

DPU_CONTROL_XRT_ABI = {
    "opcode_arg_index",
    "instruction_buffer_arg_index",
    "instruction_count_arg_index",
}

BUFFER_XRT_ABI = {
    "control_ring_buffer_arg_index",
    "input_token_buffer_arg_index",
    "output_token_buffer_arg_index",
}

OPTIONAL_SCALAR_XRT_ABI = REQUIRED_XRT_ABI.difference(DPU_CONTROL_XRT_ABI).difference(BUFFER_XRT_ABI)


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def require_keys(obj: dict[str, Any], keys: set[str], label: str, errors: list[str]) -> None:
    missing = sorted(keys.difference(obj))
    for key in missing:
        errors.append(f"{label} is missing required key: {key}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    kernel_root = manifest_path.parent
    errors: list[str] = []

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        print(f"invalid JSON: {error}", file=sys.stderr)
        return 1

    if not isinstance(manifest, dict):
        print("manifest root must be a JSON object", file=sys.stderr)
        return 1

    require_keys(manifest, REQUIRED_TOP_LEVEL, "manifest", errors)
    require(manifest.get("schema_version") == 1, "schema_version must be 1", errors)

    name = manifest.get("name")
    kernel_name = manifest.get("kernel_name")
    artifact = manifest.get("artifact")
    sources = manifest.get("sources")
    mlir_source = manifest.get("mlir_source")

    require(isinstance(name, str) and name.startswith("apusr_"), "name must be an apusr_* string", errors)
    require(
        isinstance(kernel_name, str) and (kernel_name.startswith("apusr_") or kernel_name == "MLIR_AIE"),
        "kernel_name must be an apusr_* logical name or the mlir-aie DPU kernel name MLIR_AIE",
        errors,
    )
    require(isinstance(artifact, str) and artifact.endswith(".xclbin"), "artifact must name an .xclbin", errors)
    require(isinstance(sources, list) and bool(sources), "sources must be a non-empty list", errors)
    require(
        isinstance(mlir_source, str) and (mlir_source.endswith(".mlir") or mlir_source.endswith(".py")),
        "mlir_source must name a repo-owned .mlir source or Python MLIR generator",
        errors,
    )

    operator = manifest.get("operator", {})
    if isinstance(operator, dict):
        require_keys(operator, REQUIRED_OPERATOR, "operator", errors)
        operator_type = operator.get("type")
        require(operator.get("default_token_band_rows", 0) > 0, "default_token_band_rows must be positive", errors)
        require(operator.get("default_token_ring_depth", 0) >= 2, "default_token_ring_depth must be at least 2", errors)
        require(operator.get("default_microtile_output_width", 0) > 0, "default_microtile_output_width must be positive", errors)
        require(operator.get("default_microtile_output_rows", 0) > 0, "default_microtile_output_rows must be positive", errors)

        if operator_type in {"roundtrip_nop_c16_identity_copy", "roundtrip_nop_c16_full_seam_identity_copy"}:
            require(operator.get("filter_width") == 1, "round-trip NOP must declare filter_width=1", errors)
            require(operator.get("filter_height") == 1, "round-trip NOP must declare filter_height=1", errors)
            require(operator.get("input_halo_rows") == 0, "round-trip NOP must not require input halo rows", errors)
            require(operator.get("input_halo_columns") == 0, "round-trip NOP must not require input halo columns", errors)
            require(operator.get("line_buffer_rows") == 1, "round-trip NOP must declare one live row-band stream", errors)
            require(
                operator.get("token_shape") == "model_native_c16_int8_nhwc_row_band",
                "round-trip NOP must use model-native full row bands",
                errors,
            )
            require(
                operator.get("live_state_policy") in {
                    "chunked_stream_copy_touches_every_payload_byte",
                    "full_seam_chunked_stream_copy_touches_every_payload_byte",
                    "full_seam_bounded_runlist_stream_copy_touches_every_payload_byte",
                    "full_seam_conservative_bounded_runlist_stream_copy_touches_every_payload_byte",
                },
                "round-trip NOP live state must be a chunked byte-touching stream copy",
                errors,
            )
            configured_bytes = operator.get("default_configured_frame_bytes", operator.get("default_configured_band_bytes", 0))
            chunk_bytes = operator.get("chunk_bytes", 0)
            require(isinstance(configured_bytes, int) and configured_bytes > 0, "round-trip NOP default configured bytes must be positive", errors)
            require(isinstance(chunk_bytes, int) and chunk_bytes > 0, "round-trip NOP chunk_bytes must be positive", errors)
            if isinstance(configured_bytes, int) and isinstance(chunk_bytes, int) and chunk_bytes > 0:
                require(configured_bytes % chunk_bytes == 0, "round-trip NOP default configured bytes must be divisible by chunk_bytes", errors)
        elif operator_type == "middle_core_down2x2_fasternet32_two_column_tile":
            require(operator.get("filter_width") == 3, "down/FNB32 fused kernel must declare filter_width=3", errors)
            require(operator.get("filter_height") == 3, "down/FNB32 fused kernel must declare filter_height=3", errors)
            require(operator.get("input_halo_rows") == 2, "down/FNB32 fused kernel must declare the stride-2 source halo rows", errors)
            require(operator.get("input_halo_columns") == 2, "down/FNB32 fused kernel must declare the stride-2 source halo columns", errors)
            require(operator.get("line_buffer_rows") == 4, "down/FNB32 fused kernel must declare the fused downsample/FNB live rows", errors)
            require(
                operator.get("token_shape") == "model_native_c16_to_c32_int8_nhwc_microtiles",
                "down/FNB32 fused kernel must use the C16-to-C32 microtile token shape",
                errors,
            )
            require(
                operator.get("live_state_policy") == "fuse_downsample_halo_and_first_fasternet_block_with_three_row_c32_ring",
                "down/FNB32 fused kernel must keep only a three-row C32 downsample ring local to the worker tile",
                errors,
            )
            require(
                operator.get("downsample_scratch_policy") == "three_row_c32_ring_instead_of_full_20x10_c32_halo",
                "down/FNB32 fused kernel must declare the three-row C32 downsample scratch policy",
                errors,
            )
            require(
                operator.get("downsample_ring_bytes_per_worker") == 20 * 3 * 32,
                "down/FNB32 fused kernel must declare the C32 downsample ring byte count",
                errors,
            )
        elif operator_type == "middle_core_down2x2_fasternet64_two_column_tile":
            require(operator.get("filter_width") == 3, "down/FNB64 fused kernel must declare filter_width=3", errors)
            require(operator.get("filter_height") == 3, "down/FNB64 fused kernel must declare filter_height=3", errors)
            require(operator.get("input_halo_rows") == 2, "down/FNB64 fused kernel must declare the stride-2 source halo rows", errors)
            require(operator.get("input_halo_columns") == 2, "down/FNB64 fused kernel must declare the stride-2 source halo columns", errors)
            require(operator.get("line_buffer_rows") == 4, "down/FNB64 fused kernel must declare the fused downsample/FNB live rows", errors)
            require(
                operator.get("token_shape") == "model_native_c32_to_c64_int8_nhwc_microtiles",
                "down/FNB64 fused kernel must use the C32-to-C64 microtile token shape",
                errors,
            )
            require(
                operator.get("live_state_policy") == "fuse_downsample_halo_and_first_fasternet64_block_with_three_row_c64_ring",
                "down/FNB64 fused kernel must keep only a three-row C64 downsample ring local to the worker tile",
                errors,
            )
        elif operator_type == "middle_core_fasternet32_two_column_tile":
            require(operator.get("filter_width") == 3, "FNB32 kernel must declare filter_width=3", errors)
            require(operator.get("filter_height") == 3, "FNB32 kernel must declare filter_height=3", errors)
            require(operator.get("input_halo_rows") == 1, "FNB32 kernel must declare one halo row", errors)
            require(operator.get("input_halo_columns") == 1, "FNB32 kernel must declare one halo column", errors)
            require(operator.get("line_buffer_rows") == 3, "FNB32 kernel must declare the three-row spatial window", errors)
            require(
                operator.get("token_shape") == "model_native_c32_int8_nhwc_microtiles",
                "FNB32 kernel must use the C32 microtile token shape",
                errors,
            )
            require(
                operator.get("live_state_policy") == "standalone_c32_fasternet_block_keeps_partial_spatial_mix_expand_carrier_contract_local_to_tile",
                "FNB32 kernel must keep partial spatial, expand carrier, and contraction state local to the worker tile",
                errors,
            )
        elif operator_type == "middle_core_fasternet64_two_column_tile":
            require(operator.get("filter_width") == 3, "FNB64 kernel must declare filter_width=3", errors)
            require(operator.get("filter_height") == 3, "FNB64 kernel must declare filter_height=3", errors)
            require(operator.get("input_halo_rows") == 1, "FNB64 kernel must declare one halo row", errors)
            require(operator.get("input_halo_columns") == 1, "FNB64 kernel must declare one halo column", errors)
            require(operator.get("line_buffer_rows") == 3, "FNB64 kernel must declare the three-row spatial window", errors)
            require(
                operator.get("token_shape") == "model_native_c64_int8_nhwc_microtiles",
                "FNB64 kernel must use the C64 microtile token shape",
                errors,
            )
            require(
                operator.get("live_state_policy") == "standalone_c64_fasternet_block_uses_single_buffer_package_and_keeps_spatial_expand_contract_local_to_tile",
                "FNB64 kernel must use single-buffer package FIFOs and keep spatial, expand carrier, and contraction state local",
                errors,
            )
        elif operator_type == "middle_core_fasternet32_ct2d16_skip_add_two_column_tile":
            require(operator.get("filter_width") == 3, "FNB32/CT2D16 kernel must declare filter_width=3", errors)
            require(operator.get("filter_height") == 3, "FNB32/CT2D16 kernel must declare filter_height=3", errors)
            require(operator.get("input_halo_rows") == 1, "FNB32/CT2D16 kernel must declare one primary halo row", errors)
            require(operator.get("input_halo_columns") == 1, "FNB32/CT2D16 kernel must declare one primary halo column", errors)
            require(operator.get("line_buffer_rows") == 3, "FNB32/CT2D16 kernel must declare the three-row low-scale spatial window", errors)
            require(
                operator.get("token_shape") == "model_native_c32_to_c16_ct2d_add_int8_nhwc_microtiles",
                "FNB32/CT2D16 kernel must use the C32-to-C16 skip-join microtile token shape",
                errors,
            )
            require(
                operator.get("live_state_policy") == "fuse_stage11_fasternet32_and_ct2d16_skip_join_without_materializing_low_scale_c32_output",
                "FNB32/CT2D16 kernel must keep the low-scale C32 FasterNet result local to the worker tile",
                errors,
            )
        elif operator_type == "frame_conveyor_benchmark_c16_dma_forward":
            require(operator.get("filter_width") == 1, "frame-conveyor benchmark must declare filter_width=1", errors)
            require(operator.get("filter_height") == 1, "frame-conveyor benchmark must declare filter_height=1", errors)
            require(operator.get("input_halo_rows") == 0, "frame-conveyor benchmark must not require input halo rows", errors)
            require(operator.get("input_halo_columns") == 0, "frame-conveyor benchmark must not require input halo columns", errors)
            require(operator.get("line_buffer_rows") == 1, "frame-conveyor benchmark must declare one live frame-slot stream", errors)
            require(
                operator.get("token_shape") == "c16_int8_frame_slot_transport_benchmark",
                "frame-conveyor benchmark must use complete C16 frame slots",
                errors,
            )
            require(
                operator.get("live_state_policy") == "parallel_4096_byte_chunk_dma_forward_for_frame_conveyor_regression_telemetry",
                "frame-conveyor benchmark live state must be the parallel 4096-byte DMA-forward path",
                errors,
            )
            configured_bytes = operator.get("default_configured_frame_bytes", 0)
            chunk_bytes = operator.get("chunk_bytes", 0)
            columns = operator.get("column_count_default", 0)
            channels = operator.get("dma_channels_per_column_default", 0)
            require(isinstance(configured_bytes, int) and configured_bytes > 0, "frame-conveyor benchmark default configured bytes must be positive", errors)
            require(isinstance(chunk_bytes, int) and chunk_bytes > 0, "frame-conveyor benchmark chunk_bytes must be positive", errors)
            require(isinstance(columns, int) and columns > 0, "frame-conveyor benchmark column_count_default must be positive", errors)
            require(isinstance(channels, int) and channels in {1, 2}, "frame-conveyor benchmark dma_channels_per_column_default must be 1 or 2", errors)
            require(
                operator.get("product_wiring_policy") == "standalone_cli_benchmark_only_not_a_game_frontend_or_product_kernel_profile",
                "frame-conveyor benchmark must remain outside product/game frontend wiring",
                errors,
            )
            if isinstance(chunk_bytes, int) and chunk_bytes > 0:
                require(chunk_bytes % 4 == 0, "frame-conveyor benchmark chunk_bytes must divide into uint32 words", errors)
            if all(isinstance(value, int) and value > 0 for value in (configured_bytes, chunk_bytes, columns, channels)):
                transport_bytes = ((configured_bytes + chunk_bytes - 1) // chunk_bytes) * chunk_bytes
                transport_chunks = transport_bytes // chunk_bytes
                require(
                    transport_chunks >= columns * channels,
                    "frame-conveyor benchmark transport chunk count must be at least columns * channels",
                    errors,
                )

        elif operator_type == "frame_bridge_c16_full_seam_dma_forward":
            require(operator.get("filter_width") == 1, "frame bridge must declare filter_width=1", errors)
            require(operator.get("filter_height") == 1, "frame bridge must declare filter_height=1", errors)
            require(operator.get("input_halo_rows") == 0, "frame bridge must not require input halo rows", errors)
            require(operator.get("input_halo_columns") == 0, "frame bridge must not require input halo columns", errors)
            require(operator.get("line_buffer_rows") == 1, "frame bridge must declare one live frame-slot stream", errors)
            require(
                operator.get("token_shape") == "model_native_c16_int8_nhwc_frame_slot",
                "frame bridge must use full C16 frame slots",
                errors,
            )
            require(
                operator.get("live_state_policy") == "full_seam_parallel_dma_forward_bounded_by_4096b_chunks",
                "frame bridge live state must be the parallel 4096-byte chunk DMA forward path",
                errors,
            )
            configured_bytes = operator.get("default_configured_frame_bytes", 0)
            chunk_bytes = operator.get("chunk_bytes", 0)
            columns = operator.get("column_count_default", 0)
            channels = operator.get("dma_channels_per_column_default", 0)
            require(isinstance(configured_bytes, int) and configured_bytes > 0, "frame bridge default configured bytes must be positive", errors)
            require(isinstance(chunk_bytes, int) and chunk_bytes > 0, "frame bridge chunk_bytes must be positive", errors)
            require(isinstance(columns, int) and columns > 0, "frame bridge column_count_default must be positive", errors)
            require(isinstance(channels, int) and channels in {1, 2}, "frame bridge dma_channels_per_column_default must be 1 or 2", errors)
            if isinstance(chunk_bytes, int) and chunk_bytes > 0:
                require(chunk_bytes % 4 == 0, "frame bridge chunk_bytes must divide into uint32 words", errors)
            if all(isinstance(value, int) and value > 0 for value in (configured_bytes, chunk_bytes, columns, channels)):
                transport_bytes = ((configured_bytes + chunk_bytes - 1) // chunk_bytes) * chunk_bytes
                transport_chunks = transport_bytes // chunk_bytes
                require(
                    transport_chunks >= columns * channels,
                    "frame bridge transport chunk count must be at least columns * channels",
                    errors,
                )
        else:
            require(operator.get("filter_width") == 3, "first spatial kernel must declare filter_width=3", errors)
            require(operator.get("filter_height") == 3, "first spatial kernel must declare filter_height=3", errors)
            require(operator.get("input_halo_rows") == 1, "3x3 spatial kernel must declare one halo row", errors)
            require(operator.get("input_halo_columns") == 1, "3x3 spatial kernel must declare one halo column", errors)
            require(operator.get("line_buffer_rows") == 3, "3x3 spatial kernel must declare three line-buffer rows", errors)
            require(
                operator.get("token_shape") in {"band_of_npu_local_microtiles", "model_native_c16_int8_nhwc_microtiles"},
                "first spatial kernel must use model/NPU-local microtiles, not full-width row bands",
                errors,
            )
            require(
                operator.get("live_state_policy") == "bounded_by_microtile_not_full_row_band",
                "first spatial kernel live state must be bounded by microtile size",
                errors,
            )
    else:
        errors.append("operator must be an object")

    xrt_abi = manifest.get("xrt_abi", {})
    if isinstance(xrt_abi, dict):
        require_keys(xrt_abi, REQUIRED_XRT_ABI, "xrt_abi", errors)
        dpu_control_indices = [xrt_abi.get(key) for key in sorted(DPU_CONTROL_XRT_ABI)]
        buffer_indices = [xrt_abi.get(key) for key in sorted(BUFFER_XRT_ABI)]
        scalar_indices = [xrt_abi.get(key) for key in sorted(OPTIONAL_SCALAR_XRT_ABI)]
        require(all(isinstance(index, int) and index >= 0 for index in dpu_control_indices), "DPU control xrt_abi indices must be non-negative integers", errors)
        require(all(isinstance(index, int) and index >= -1 for index in buffer_indices), "buffer xrt_abi indices must be integers >= -1", errors)
        require(xrt_abi.get("input_token_buffer_arg_index", -1) >= 0, "input token buffer arg index must be enabled", errors)
        require(xrt_abi.get("output_token_buffer_arg_index", -1) >= 0, "output token buffer arg index must be enabled", errors)
        require(all(isinstance(index, int) and index >= -1 for index in scalar_indices), "scalar xrt_abi indices must be integers >= -1", errors)
        enabled_indices = dpu_control_indices + [index for index in buffer_indices + scalar_indices if isinstance(index, int) and index >= 0]
        require(len(set(enabled_indices)) == len(enabled_indices), "enabled xrt_abi indices must be unique", errors)
    else:
        errors.append("xrt_abi must be an object")


    if isinstance(mlir_source, str):
        mlir_source_path = (kernel_root / mlir_source).resolve()
        try:
            mlir_source_path.relative_to(kernel_root)
        except ValueError:
            errors.append(f"mlir_source escapes kernel root: {mlir_source}")
        else:
            if not mlir_source_path.exists():
                errors.append(f"mlir_source does not exist: {mlir_source}")

    if isinstance(sources, list):
        for source in sources:
            if not isinstance(source, str):
                errors.append("sources entries must be strings")
                continue
            source_path = (kernel_root / source).resolve()
            try:
                source_path.relative_to(kernel_root)
            except ValueError:
                errors.append(f"source escapes kernel root: {source}")
                continue
            if not source_path.exists():
                errors.append(f"source does not exist: {source}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"validated kernel manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
