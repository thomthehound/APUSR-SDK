#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Resident arena layout helpers shared by generators and validators."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class FieldLayout:
    name: str
    offset_bytes: int
    bytes: int
    dtype: str
    meaning: str | None = None

    def to_json(self) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "name": self.name,
            "offset_bytes": self.offset_bytes,
            "bytes": self.bytes,
            "dtype": self.dtype,
        }
        if self.meaning:
            payload["meaning"] = self.meaning
        return payload


@dataclass(frozen=True)
class ResidualEpilogueSpec:
    name: str
    scale_offset_bytes: int
    shift_offset_bytes: int
    channels: int
    output_shift_header_lane: int


@dataclass(frozen=True)
class ResidentRecordSpec:
    name: str
    resident_group: str
    logical_stages: tuple[int, ...]
    operator_family: str
    primitive_family: str
    record_bytes: int
    arena_offset_bytes: int
    arena_slice_bytes: int
    field_layout: tuple[FieldLayout, ...]
    input_shape: str
    output_shape: str
    implementation_status: str
    primitive_entry_point: str
    hot_record_strategy: str
    load_time_transforms: tuple[str, ...]
    tile_local_hot_record_bytes: int | None = None
    tile_local_hot_field_layout: tuple[FieldLayout, ...] = ()
    active_compile_proof: bool = False
    additional_primitive_entry_points: tuple[str, ...] = ()
    activation_context_strategy: str = "reuse_producer_overlap_or_tile_local_row_ring_when_available"
    synthetic_halo_policy: str = "avoid_for_interior_cells_boundary_only_or_explicitly_proven_local_case"
    product_context_contract: dict[str, Any] | None = None

    def to_json(self) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "schema": "com.apusr.middle_core_resident_record.v1",
            "logical_name": self.name,
            "logical_stages": list(self.logical_stages),
            "primary_logical_stage": self.logical_stages[0],
            "resident_group": self.resident_group,
            "operator_family": self.operator_family,
            "primitive_family": self.primitive_family,
            "resident_arena_offset_bytes": self.arena_offset_bytes,
            "resident_arena_slice_bytes": self.arena_slice_bytes,
            "record_bytes": self.record_bytes,
            "source_record_bytes": self.record_bytes,
            "field_layout": [field.to_json() for field in self.field_layout],
            "input_activation": {"shape": self.input_shape},
            "output_activation": {"shape": self.output_shape},
            "primitive_entry_point": self.primitive_entry_point,
            "load_timing": "model_epoch_load_params_sequence",
            "hot_path_parameter_streams": 0,
            "hot_record_strategy": self.hot_record_strategy,
            "load_time_transforms": list(self.load_time_transforms),
            "implementation_status": self.implementation_status,
            "activation_context_policy": {
                "preferred_source": self.activation_context_strategy,
                "synthetic_halo_policy": self.synthetic_halo_policy,
                "reuse_available_context_before_generating_new_halo": True,
                "frame_boundary_padding_source": "front_guard_or_explicit_boundary_contract",
                "interior_seam_halos_are_not_default": True,
            },
            "performance_budget": {
                "hot_path_parameter_streams": 0,
                "frame_path_parameter_dma_tasks": 0,
                "full_row_fifos": 0,
                "per_cell_dma_task_explosion_allowed": False,
                "watched_scalar_pack_ops": 0,
                "watch_vector_data_motion_growth": True,
            },
            "residual_epilogue_policy": {
                "activation_widening": "vector_load_unpack_i8_to_i16_to_i32",
                "hot_record_rule": "shift_may_be_omitted_only_when_residual_or_skip_shift_matches_output_srs",
                "required_when_shift_differs": "keep_shift_in_hot_record_or_prefold_at_model_epoch",
            },
        }
        if self.product_context_contract is not None:
            payload["product_context_contract"] = self.product_context_contract
        if self.additional_primitive_entry_points:
            payload["additional_primitive_entry_points"] = list(self.additional_primitive_entry_points)
        if self.tile_local_hot_record_bytes is not None:
            payload["tile_local_hot_record_bytes"] = self.tile_local_hot_record_bytes
            payload["tile_local_hot_field_layout"] = [field.to_json() for field in self.tile_local_hot_field_layout]
        if self.active_compile_proof:
            payload["active_compile_proof"] = {
                "graph_phase": "stage1-resident",
                "uses_tile_local_seed_buffer": True,
                "live_load_params_sequence_pending": True,
            }
        return payload


STAGE_SLICES: dict[str, tuple[int, int]] = {
    "stage1_convnext16": (0, 4048),
    "stage2_convnext16": (4064, 4048),
    "stage3_4_down_fnb32": (8128, 10000),
    "stage5_fnb32_c8group": (18144, 8192),
    "stage6_7_down_fnb64": (26336, 36880),
    "stage8_fnb64": (63232, 28176),
    "stage9_ct2d32_chunks": (91424, 8896),
    "stage10_fnb32_c8group": (100320, 7696),
    "stage11_fnb32_ct2d16": (108032, 10064),
    "stage12_convnext16": (118112, 4048),
}

PAYLOAD_BYTES: dict[str, int] = {
    "stage1_convnext16": 4048,
    "stage2_convnext16": 4048,
    "stage3_4_down_fnb32": 10000,
    "stage5_fnb32_c8group": 7696,
    "stage6_7_down_fnb64": 36880,
    "stage8_fnb64": 28176,
    "stage9_ct2d32_chunks": 8896,
    "stage10_fnb32_c8group": 7696,
    "stage11_fnb32_ct2d16": 10064,
    "stage12_convnext16": 4048,
}

LOGICAL_STAGE_GROUPS: dict[str, tuple[int, ...]] = {
    "stage1_convnext16": (1,),
    "stage2_convnext16": (2,),
    "stage3_4_down_fnb32": (3, 4),
    "stage5_fnb32_c8group": (5,),
    "stage6_7_down_fnb64": (6, 7),
    "stage8_fnb64": (8,),
    "stage9_ct2d32_chunks": (9,),
    "stage10_fnb32_c8group": (10,),
    "stage11_fnb32_ct2d16": (11,),
    "stage12_convnext16": (12,),
}


def i32_values(payload: bytes, offset_bytes: int, count: int) -> tuple[int, ...]:
    return struct.unpack_from(f"<{count}i", payload, offset_bytes)


def _append(fields: list[FieldLayout], name: str, size: int, dtype: str, meaning: str | None = None) -> None:
    offset = fields[-1].offset_bytes + fields[-1].bytes if fields else 0
    fields.append(FieldLayout(name, offset, size, dtype, meaning))


def _convnext16_fields() -> tuple[FieldLayout, ...]:
    fields: list[FieldLayout] = []
    _append(fields, "header_i32", 16, "i32[4]", "spatial_shift, expand_shift, contract_shift, reserved")
    _append(fields, "spatial3x3.weights0_s8_m4x8", 2304, "s8[9][2][2][8x8]")
    _append(fields, "spatial3x3.bias0_i32", 64, "i32[16]")
    _append(fields, "spatial3x3.multiplier_i32", 64, "i32[16]")
    _append(fields, "pointwise_expand.weights1_s8_m4x8", 512, "s8[4][2][8x8]")
    _append(fields, "pointwise_expand.bias1_i32", 128, "i32[32]")
    _append(fields, "pointwise_expand.multiplier_i32", 128, "i32[32]")
    _append(fields, "pointwise_contract.weights2_s8_m4x8", 512, "s8[2][4][8x8]")
    _append(fields, "pointwise_contract.bias2_i32", 64, "i32[16]")
    _append(fields, "pointwise_contract.multiplier_i32", 64, "i32[16]")
    _append(fields, "residual.multiplier_i32", 64, "i32[16]")
    _append(fields, "residual.shift_i32", 64, "i32[16]")
    _append(fields, "reserved_i32", 64, "i32[16]")
    return tuple(fields)


def _convnext16_hot_fields() -> tuple[FieldLayout, ...]:
    fields: list[FieldLayout] = []
    _append(fields, "header_i32", 16, "i32[4]")
    _append(fields, "spatial3x3.weights0_s8_m4x8", 2304, "s8[9][2][2][8x8]")
    _append(fields, "spatial3x3.bias0_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "spatial3x3.multiplier_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "pointwise_expand.weights1_s8_m4x8", 512, "s8[4][2][8x8]")
    _append(fields, "pointwise_expand.bias1_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "pointwise_expand.multiplier_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "pointwise_contract.weights2_s8_m4x8", 512, "s8[2][4][8x8]")
    _append(fields, "pointwise_contract.bias2_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "pointwise_contract.multiplier_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "residual.multiplier_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "reserved", 128, "u8[128]")
    return tuple(fields)



def _stage3_4_down_fnb32_hot_fields() -> tuple[FieldLayout, ...]:
    fields: list[FieldLayout] = []
    _append(fields, "header_i32", 16, "i32[4]", "downsample_shift, spatial_shift, expand_shift, contract_shift")
    _append(fields, "downsample.weights_s8_k2s2_m4x8", 2048, "s8[4][4][2][8x8]")
    _append(fields, "downsample.bias_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "downsample.multiplier_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "fnb.partial_spatial3x3.weights0_s8_m4x8", 2304, "s8[9][2][2][8x8]")
    _append(fields, "fnb.partial_spatial3x3.bias0_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "fnb.partial_spatial3x3.multiplier_i32_m4x8_blocks", 256, "i32[2][32]")
    _append(fields, "fnb.pointwise_expand.weights1_s8_m4x8", 2048, "s8[8][4][8x8]")
    _append(fields, "fnb.pointwise_expand.bias1_i32_m4x8_blocks", 1024, "i32[8][32]")
    _append(fields, "fnb.pointwise_expand.multiplier_i32_m4x8_blocks", 1024, "i32[8][32]")
    _append(fields, "fnb.pointwise_contract.weights2_s8_m4x8", 2048, "s8[4][8][8x8]")
    _append(fields, "fnb.pointwise_contract.bias2_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "fnb.pointwise_contract.multiplier_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "fnb.residual.multiplier_i32_m4x8_blocks", 512, "i32[4][32]")
    _append(fields, "reserved", 128, "u8[128]")
    return tuple(fields)


def _fnb_fields(channels: int, *, prefix: str = "") -> tuple[FieldLayout, ...]:
    spatial = channels // 2
    hidden = channels * 2
    spatial_weight_bytes = 9 * (spatial // 8) * 2 * 64
    expand_weight_bytes = (hidden // 8) * (channels // 8) * 64
    contract_weight_bytes = (channels // 8) * (hidden // 8) * 64
    fields: list[FieldLayout] = []
    if not prefix:
        _append(fields, "header_i32", 16, "i32[4]", "spatial_shift, expand_shift, contract_shift, reserved")
    _append(fields, f"{prefix}partial_spatial3x3.weights0_s8_m4x8", spatial_weight_bytes, f"s8[9][{spatial // 8}][2][8x8]")
    _append(fields, f"{prefix}partial_spatial3x3.bias0_i32", spatial * 4, f"i32[{spatial}]")
    _append(fields, f"{prefix}partial_spatial3x3.multiplier_i32", spatial * 4, f"i32[{spatial}]")
    _append(fields, f"{prefix}pointwise_expand.weights1_s8_m4x8", expand_weight_bytes, f"s8[{hidden // 8}][{channels // 8}][8x8]")
    _append(fields, f"{prefix}pointwise_expand.bias1_i32", hidden * 4, f"i32[{hidden}]")
    _append(fields, f"{prefix}pointwise_expand.multiplier_i32", hidden * 4, f"i32[{hidden}]")
    _append(fields, f"{prefix}pointwise_contract.weights2_s8_m4x8", contract_weight_bytes, f"s8[{channels // 8}][{hidden // 8}][8x8]")
    _append(fields, f"{prefix}pointwise_contract.bias2_i32", channels * 4, f"i32[{channels}]")
    _append(fields, f"{prefix}pointwise_contract.multiplier_i32", channels * 4, f"i32[{channels}]")
    _append(fields, f"{prefix}residual.multiplier_i32", channels * 4, f"i32[{channels}]")
    _append(fields, f"{prefix}residual.shift_i32", channels * 4, f"i32[{channels}]")
    _append(fields, f"{prefix}reserved_i32", channels * 4, f"i32[{channels}]")
    return tuple(fields)


def _down_fnb_fields(channels: int) -> tuple[FieldLayout, ...]:
    down_weight_bytes = 4 * (channels // 8) * (channels // 16) * 64
    fields: list[FieldLayout] = []
    _append(fields, "header_i32", 16, "i32[4]", "downsample_shift, spatial_shift, expand_shift, contract_shift")
    _append(fields, "downsample.weights_s8_k2s2_m4x8", down_weight_bytes, f"s8[4][{channels // 8}][{channels // 16}][8x8]")
    _append(fields, "downsample.bias_i32", channels * 4, f"i32[{channels}]")
    _append(fields, "downsample.multiplier_i32", channels * 4, f"i32[{channels}]")
    base = fields[-1].offset_bytes + fields[-1].bytes
    for field in _fnb_fields(channels, prefix="fnb."):
        if field.name == "header_i32":
            continue
        fields.append(FieldLayout(field.name, base + field.offset_bytes, field.bytes, field.dtype, field.meaning))
    return tuple(fields)


def _ct2d32_chunk_fields() -> tuple[FieldLayout, ...]:
    fields: list[FieldLayout] = []
    for chunk in range(4):
        p = f"chunk{chunk}."
        _append(fields, f"{p}header_i32", 16, "i32[4]", "ct2d_shift, reserved, reserved, reserved")
        _append(fields, f"{p}ct2d32.weights_s8_k2s2_m4x8", 2048, "s8[2][2][8][8x8]")
        _append(fields, f"{p}ct2d32.bias_i32", 32, "i32[8]")
        _append(fields, f"{p}ct2d32.multiplier_i32", 32, "i32[8]")
        _append(fields, f"{p}skip.multiplier_i32", 32, "i32[8]")
        _append(fields, f"{p}skip.shift_i32", 32, "i32[8]")
        _append(fields, f"{p}reserved_i32", 32, "i32[8]")
    return tuple(fields)


def _fnb_ct2d16_fields() -> tuple[FieldLayout, ...]:
    fields = list(_fnb_fields(32))
    base = fields[-1].offset_bytes + fields[-1].bytes
    tail = [
        FieldLayout("ct2d16.weights_s8_k2s2_m4x8", base, 2048, "s8[2][2][4][8x8]"),
        FieldLayout("ct2d16.bias_i32", base + 2048, 64, "i32[16]"),
        FieldLayout("ct2d16.multiplier_i32", base + 2112, 64, "i32[16]"),
        FieldLayout("ct2d16.skip.multiplier_i32", base + 2176, 64, "i32[16]"),
        FieldLayout("ct2d16.skip.shift_i32", base + 2240, 64, "i32[16]"),
        FieldLayout("ct2d16.reserved_i32", base + 2304, 64, "i32[16]"),
    ]
    fields.extend(tail)
    return tuple(fields)


def _with_payload_padding(stage_name: str, fields: tuple[FieldLayout, ...]) -> tuple[FieldLayout, ...]:
    expected = PAYLOAD_BYTES[stage_name]
    cursor = fields[-1].offset_bytes + fields[-1].bytes
    if cursor == expected:
        return fields
    if cursor > expected:
        raise ValueError(f"{stage_name} fields end at {cursor}, beyond payload {expected}")
    return (*fields, FieldLayout("payload_padding", cursor, expected - cursor, f"u8[{expected - cursor}]"))


def field_layout_for_stage(stage_name: str) -> tuple[FieldLayout, ...]:
    if stage_name in {"stage1_convnext16", "stage2_convnext16", "stage12_convnext16"}:
        return _convnext16_fields()
    if stage_name == "stage3_4_down_fnb32":
        return _down_fnb_fields(32)
    if stage_name in {"stage5_fnb32_c8group", "stage10_fnb32_c8group"}:
        return _fnb_fields(32)
    if stage_name == "stage6_7_down_fnb64":
        return _with_payload_padding(stage_name, _down_fnb_fields(64))
    if stage_name == "stage8_fnb64":
        return _with_payload_padding(stage_name, _fnb_fields(64))
    if stage_name == "stage9_ct2d32_chunks":
        return _ct2d32_chunk_fields()
    if stage_name == "stage11_fnb32_ct2d16":
        return _fnb_ct2d16_fields()
    raise KeyError(f"unknown resident stage slice: {stage_name}")


def _record_bytes(stage_name: str) -> int:
    return PAYLOAD_BYTES[stage_name]


def _convnext16_residual_specs() -> list[ResidualEpilogueSpec]:
    return [ResidualEpilogueSpec("convnext16.residual", 3856, 3920, 16, 2)]


def _fnb_residual_spec(name: str, channels: int, output_shift_header_lane: int, base_offset: int = 0) -> ResidualEpilogueSpec:
    spatial = channels // 2
    hidden = channels * 2
    spatial_weight_bytes = 9 * (spatial // 8) * 2 * 64
    expand_weight_bytes = (hidden // 8) * (channels // 8) * 64
    contract_weight_bytes = (channels // 8) * (hidden // 8) * 64
    scale_offset = base_offset + (
        16
        + spatial_weight_bytes
        + spatial * 4
        + spatial * 4
        + expand_weight_bytes
        + hidden * 4
        + hidden * 4
        + contract_weight_bytes
        + channels * 4
        + channels * 4
    )
    shift_offset = scale_offset + channels * 4
    return ResidualEpilogueSpec(name, scale_offset, shift_offset, channels, output_shift_header_lane)


def _down_fnb_residual_spec(name: str, channels: int) -> ResidualEpilogueSpec:
    down_weight_bytes = 4 * (channels // 8) * (channels // 16) * 64
    down_prefix_bytes = 16 + down_weight_bytes + channels * 4 + channels * 4
    fnb = _fnb_residual_spec(name, channels, 3, base_offset=down_prefix_bytes - 16)
    return fnb


def _ct2d32_chunk_specs() -> list[ResidualEpilogueSpec]:
    specs: list[ResidualEpilogueSpec] = []
    chunk_bytes = 2224
    for chunk in range(4):
        base = chunk * chunk_bytes
        specs.append(ResidualEpilogueSpec(f"ct2d32.chunk{chunk}.skip", base + 2128, base + 2160, 8, 0))
    return specs


def residual_epilogue_specs(stage_name: str) -> list[ResidualEpilogueSpec]:
    if stage_name in {"stage1_convnext16", "stage2_convnext16", "stage12_convnext16"}:
        return _convnext16_residual_specs()
    if stage_name == "stage3_4_down_fnb32":
        return [_down_fnb_residual_spec("fnb32.residual", 32)]
    if stage_name in {"stage5_fnb32_c8group", "stage10_fnb32_c8group"}:
        return [_fnb_residual_spec("fnb32.residual", 32, 2)]
    if stage_name == "stage6_7_down_fnb64":
        return [_down_fnb_residual_spec("fnb64.residual", 64)]
    if stage_name == "stage8_fnb64":
        return [_fnb_residual_spec("fnb64.residual", 64, 2)]
    if stage_name == "stage9_ct2d32_chunks":
        return _ct2d32_chunk_specs()
    if stage_name == "stage11_fnb32_ct2d16":
        return [
            _fnb_residual_spec("fnb32.residual", 32, 2),
            ResidualEpilogueSpec("ct2d16.skip", 9872, 9936, 16, 3),
        ]
    return []


def _value_mode(values: tuple[int, ...]) -> str:
    unique = set(values)
    if len(unique) == 1:
        return "uniform"
    if len(unique) == 2:
        midpoint = len(values) // 2
        if len(set(values[:midpoint])) == 1 and len(set(values[midpoint:])) == 1:
            return "split_half"
        return "two_segment"
    return "multi_segment"


def analyze_residual_epilogues(stage_name: str, payload: bytes) -> list[dict[str, Any]]:
    header = i32_values(payload, 0, 4)
    result: list[dict[str, Any]] = []
    for spec in residual_epilogue_specs(stage_name):
        multipliers = i32_values(payload, spec.scale_offset_bytes, spec.channels)
        shifts = i32_values(payload, spec.shift_offset_bytes, spec.channels)
        output_shift = header[spec.output_shift_header_lane]
        shift_values = sorted(set(shifts))
        shift_mode = "same_as_output_srs" if shift_values == [output_shift] else "requires_residual_shift_handling"
        result.append(
            {
                "name": spec.name,
                "channels": spec.channels,
                "scale_offset_bytes": spec.scale_offset_bytes,
                "shift_offset_bytes": spec.shift_offset_bytes,
                "output_shift_header_lane": spec.output_shift_header_lane,
                "output_shift": output_shift,
                "multiplier_mode": _value_mode(multipliers),
                "multiplier_value_count": len(set(multipliers)),
                "shift_mode": shift_mode,
                "shift_values": shift_values,
                "hot_record_rule": "shift_may_be_omitted" if shift_mode == "same_as_output_srs" else "shift_must_be_kept_or_prefolded",
                "activation_widening": "vector_load_unpack_i8_to_i16_to_i32",
            }
        )
    return result


def validate_residual_epilogues(stage_name: str, payload: bytes) -> list[str]:
    errors: list[str] = []
    for epilogue in analyze_residual_epilogues(stage_name, payload):
        shift_values = epilogue["shift_values"]
        if any(int(value) < 0 or int(value) > 31 for value in shift_values):
            errors.append(f"{stage_name} {epilogue['name']}: residual shifts must fit 0..31, got {shift_values}")
        if int(epilogue["multiplier_value_count"]) <= 0:
            errors.append(f"{stage_name} {epilogue['name']}: missing residual multiplier values")
    return errors


def resident_record_specs() -> dict[str, ResidentRecordSpec]:
    common_shift_transform = (
        "expand_bias_and_multiplier_vectors_to_the_primitive_hot_shape",
        "if_residual_or_skip_shift_differs_from_output_srs_keep_shift_in_hot_record_or_prefold_at_model_epoch",
        "do_not_emit_frame_hot_parameter_dma_or_scalar_unpack_work",
    )
    records: dict[str, ResidentRecordSpec] = {}
    for name in STAGE_SLICES:
        offset, slice_bytes = STAGE_SLICES[name]
        fields = field_layout_for_stage(name)
        record_bytes = _record_bytes(name)
        if name == "stage1_convnext16":
            records[name] = ResidentRecordSpec(
                name=name,
                resident_group=name,
                logical_stages=LOGICAL_STAGE_GROUPS[name],
                operator_family="ConvNextBlock",
                primitive_family="convnext16_halo_to_c16_cell",
                record_bytes=record_bytes,
                arena_offset_bytes=offset,
                arena_slice_bytes=slice_bytes,
                field_layout=fields,
                input_shape="c16_halo_cell",
                output_shape="c16_cell",
                implementation_status="active_compile_proof",
                primitive_entry_point="apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8",
                hot_record_strategy="model_epoch_expanded_tile_local_hot_record",
                load_time_transforms=common_shift_transform,
                tile_local_hot_record_bytes=5776,
                tile_local_hot_field_layout=_convnext16_hot_fields(),
                active_compile_proof=True,
            )
        elif name == "stage2_convnext16":
            records[name] = ResidentRecordSpec(
                name=name,
                resident_group=name,
                logical_stages=LOGICAL_STAGE_GROUPS[name],
                operator_family="ConvNextBlock",
                primitive_family="convnext16_producer_plane_window_to_c16_cell",
                record_bytes=record_bytes,
                arena_offset_bytes=offset,
                arena_slice_bytes=slice_bytes,
                field_layout=fields,
                input_shape="c16_halo_cell",
                output_shape="c16_cell",
                implementation_status="stage2_resident_primitive_implemented_standalone_linked_and_plane_window_proofs",
                primitive_entry_point="apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8",
                hot_record_strategy="model_epoch_expanded_tile_local_hot_record",
                load_time_transforms=common_shift_transform,
                tile_local_hot_record_bytes=5776,
                tile_local_hot_field_layout=_convnext16_hot_fields(),
                activation_context_strategy="stage1_c16_row_major_activation_ring",
                synthetic_halo_policy="do_not_synthesize_interior_stage2_halos; consume_a_single_compact_18x10_view_from_the_stage1_activation_ring_or_explicit_boundary_contract",
                product_context_contract={
                    "graph_phase": "stage1_2-linked-resident",
                    "standalone_graph_phase": "stage2-resident",
                    "plane_window_graph_phase": "stage2-plane-window-resident",
                    "producer_stage": 1,
                    "consumer_stage": 2,
                    "source": "worker-local Stage-1 C16 row-major activation ring",
                    "worker_scope": "one spatial4col worker; npu1 uses one worker, npu2 auto uses independent copies",
                    "cross_worker_frame_hot_dependencies": False,
                    "cross_worker_guard_exchange": False,
                    "worker_boundary_guard_source": "owned_or_redundantly_computed_inside_each_worker",
                    "context_window_shape": "single compact 18x10xC16 producer-plane window",
                    "context_window_bytes": 2880,
                    "logical_halo_shape": "18x10xC16",
                    "logical_halo_bytes": 2880,
                    "left_right_halo_source": "adjacent pixels in the same worker-local Stage-1 row-major activation band; stripe-boundary guards are local/redundant, not exchanged",
                    "top_bottom_halo_source": "adjacent rows in the same worker-local Stage-1 row-major activation band",
                    "purpose": "Run the second C16 ConvNext block from Stage-1 output context and produce the C16 overlap source required by fused Stage 3/4.",
                    "standalone_compile_carrier": "compact_explicit_halo_patch_row_block",
                    "plane_window_compile_carrier": "compact_explicit_halo_patch_row_block_until_the_stage1_row_major_ring_is_wired",
                    "linked_compile_carrier": "Stage1 C16 cell FIFO -> tile-local Stage2 carrier bridge until producer row-major ring is wired",
                    "rejected_compile_carriers": ("three_separate_top_center_bottom_row_window_inputs",),
                    "hot_parameter_streams": 0,
                    "per_cell_dma_task_explosion_allowed": False,
                },
            )
        else:
            family: str
            input_shape: str
            output_shape: str
            entry = f"planned_{name}_resident_primitive"
            status = "cataloged_pending_aie_primitive"
            hot_strategy = "model_epoch_hot_record_shape_pending_primitive_lowering"
            if name in {"stage12_convnext16"}:
                family, input_shape, output_shape = "convnext16_halo_to_c16_cell", "c16_halo_cell", "c16_cell"
            elif name == "stage3_4_down_fnb32":
                family, input_shape, output_shape = "downsample_c16_to_c32_then_fnb32", "c16_cell_2x2_window", "c32_cell"
                status = "active_transition_compile_proofs_downsample_and_fused_pressure"
                entry = "apusr_middle_core_stage3_downsample_window_to_c32_cell_i8"
                hot_strategy = "model_epoch_expanded_tile_local_hot_record"
            elif name == "stage5_fnb32_c8group":
                family, input_shape, output_shape = "fnb32_same_scale", "c32_halo_cell", "c32_cell"
            elif name == "stage6_7_down_fnb64":
                family, input_shape, output_shape = "downsample_c32_to_c64_then_fnb64", "c32_cell", "c64_cell"
            elif name == "stage8_fnb64":
                family, input_shape, output_shape = "fnb64_same_scale", "c64_halo_cell", "c64_cell"
            elif name == "stage9_ct2d32_chunks":
                family, input_shape, output_shape = "ct2d32_c8_chunks_add_skipb", "c64_cell", "c32_cell"
            elif name == "stage10_fnb32_c8group":
                family, input_shape, output_shape = "fnb32_same_scale", "c32_halo_cell", "c32_cell"
            elif name == "stage11_fnb32_ct2d16":
                family, input_shape, output_shape = "fnb32_then_ct2d16_add_skipa", "c32_halo_cell", "c16_cell"
            else:
                raise AssertionError(name)
            tile_hot_bytes = 13712 if name == "stage3_4_down_fnb32" else None
            tile_hot_fields = _stage3_4_down_fnb32_hot_fields() if name == "stage3_4_down_fnb32" else ()
            records[name] = ResidentRecordSpec(
                name=name,
                resident_group=name,
                logical_stages=LOGICAL_STAGE_GROUPS[name],
                operator_family=family,
                primitive_family=family,
                record_bytes=record_bytes,
                arena_offset_bytes=offset,
                arena_slice_bytes=slice_bytes,
                field_layout=fields,
                input_shape=input_shape,
                output_shape=output_shape,
                implementation_status=status,
                primitive_entry_point=entry,
                hot_record_strategy=hot_strategy,
                load_time_transforms=common_shift_transform,
                tile_local_hot_record_bytes=tile_hot_bytes,
                tile_local_hot_field_layout=tile_hot_fields,
                additional_primitive_entry_points=("apusr_middle_core_stage3_4_fused_window20x40_to_c32_cell_i8",) if name == "stage3_4_down_fnb32" else (),
                activation_context_strategy="upstream_c16_producer_overlap_row_ring" if name == "stage3_4_down_fnb32" else "reuse_producer_overlap_or_tile_local_row_ring_when_available",
                synthetic_halo_policy="do_not_synthesize_interior_stage3_4_halos; use producer_overlap_or_explicit_boundary_contract" if name == "stage3_4_down_fnb32" else "avoid_for_interior_cells_boundary_only_or_explicitly_proven_local_case",
                product_context_contract=(
                    {
                        "graph_phase": "stage3_4-product-resident",
                        "standalone_pressure_graph_phase": "stage3_4-fused-resident",
                        "producer_stage": 2,
                        "consumer_stages": [3, 4],
                        "source": "worker-local Stage-2 C16 producer-overlap row ring",
                        "worker_scope": "one spatial4col worker; no frame-hot dependency on peer workers",
                        "cross_worker_frame_hot_dependencies": False,
                        "context_window_shape": "20x40xC16",
                        "context_window_bytes": 12800,
                        "purpose": "Provide the one-pixel C32 Stage-4 spatial context before Stage-3 downsample without synthesizing interior seam halos.",
                        "standalone_compile_carrier": "row_major_c16_frame_tap_until_stage2_is_linked",
                        "hot_parameter_streams": 0,
                        "per_cell_dma_task_explosion_allowed": False,
                    }
                    if name == "stage3_4_down_fnb32"
                    else None
                ),
            )
    return records


def resident_record_catalog_payload() -> dict[str, Any]:
    return {
        "schema": "com.apusr.middle_core_resident_records.v0",
        "purpose": "Machine-readable physical resident primitive records. Add primitive facts here instead of creating one document per stage.",
        "policy": {
            "kernel_owns_physical_record_format": True,
            "source_model_owns_tensor_meaning_only": True,
            "records_are_model_epoch_loaded": True,
            "stage_specific_docs_are_not_required": True,
            "residual_epilogues_are_validated_per_resident_stage": True,
            "hot_record_omits_shift_only_when_shift_matches_output_srs": True,
            "reuse_available_activation_context_before_synthesizing_halos": True,
            "synthetic_interior_seam_halos_are_last_resort": True,
            "frame_boundary_padding_requires_explicit_contract": True,
            "hot_path_parameter_streams": 0,
            "frame_hot_parameter_dma_tasks": 0,
            "per_cell_dma_task_explosion_allowed": False,
            "performance_budgets_are_checked_by_contract_tools": True,
            "stage2_product_context_uses_stage1_c16_overlap": True,
            "stage3_4_product_context_uses_upstream_c16_overlap": True,
            "worker_groups_are_independent_product_units": True,
            "cross_worker_frame_hot_dependencies_forbidden": True,
        },
        "records": [spec.to_json() for spec in resident_record_specs().values()],
    }
