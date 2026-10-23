#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate middle-core resident primitive record contracts."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.model.resident_arena_layout import (  # noqa: E402
    PAYLOAD_BYTES,
    STAGE_SLICES,
    analyze_residual_epilogues,
    resident_record_specs,
    validate_residual_epilogues,
)

DEFAULT_RECORDS = REPO_ROOT / "models/fsr4_middle_core_resident_records_v0.json"
DEFAULT_LEDGER = REPO_ROOT / "models/fsr4_middle_core_stage_ledger_v0.json"
DEFAULT_ARENA_SET = REPO_ROOT / "models/fsr4_int8/resident_params/stage1_12_full_arena__am020_4x4_base.json"
ARENA_NAME = "stage1_12_full_arena.bin"
STAGE1_ENTRY = "apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8"
STAGE2_ENTRY = "apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def repo_path(path_text: str) -> Path:
    path = Path(path_text)
    return path if path.is_absolute() else REPO_ROOT / path


def records_by_group(catalog: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for record in catalog.get("records") or []:
        result[str(record.get("resident_group", ""))] = record
    return result


def field_triplets(fields: list[dict[str, Any]]) -> list[tuple[str, int, int, str]]:
    return [
        (
            str(field.get("name")),
            int(field.get("offset_bytes", -1)),
            int(field.get("bytes", -1)),
            str(field.get("dtype", "")),
        )
        for field in fields
    ]


def spec_field_triplets(fields: list[Any]) -> list[tuple[str, int, int, str]]:
    return [(field.name, field.offset_bytes, field.bytes, field.dtype) for field in fields]


def common_record_errors(record: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    group = str(record.get("resident_group", "<missing>"))
    if record.get("schema") != "com.apusr.middle_core_resident_record.v1":
        errors.append(f"{group}: bad resident record schema: {record.get('schema')}")
    if not record.get("primitive_entry_point"):
        errors.append(f"{group}: resident record must name an AIE primitive entry point")
    if int(record.get("record_bytes", -1)) <= 0:
        errors.append(f"{group}: resident record must declare a positive record size")
    if int(record.get("hot_path_parameter_streams", -1)) != 0:
        errors.append(f"{group}: resident record must keep hot-path parameter streams at zero")
    if record.get("load_timing") != "model_epoch_load_params_sequence":
        errors.append(f"{group}: resident record must load through the model-epoch parameter path")
    context_policy = record.get("activation_context_policy") or {}
    if context_policy.get("reuse_available_context_before_generating_new_halo") is not True:
        errors.append(f"{group}: resident record must prefer available activation context before generating halos")
    if context_policy.get("interior_seam_halos_are_not_default") is not True:
        errors.append(f"{group}: resident record must not default to synthetic interior seam halos")
    if context_policy.get("frame_boundary_padding_source") != "front_guard_or_explicit_boundary_contract":
        errors.append(f"{group}: resident record must keep frame-boundary padding explicit")
    if int(record.get("performance_budget", {}).get("frame_path_parameter_dma_tasks", -1)) != 0:
        errors.append(f"{group}: resident budget must keep frame-path parameter DMA at zero")
    if record.get("performance_budget", {}).get("per_cell_dma_task_explosion_allowed") is not False:
        errors.append(f"{group}: resident budget must forbid per-cell DMA task explosion")
    fields = record.get("field_layout") or []
    cursor = 0
    for field in fields:
        field_name = str(field.get("name"))
        offset = int(field.get("offset_bytes", -1))
        size = int(field.get("bytes", -1))
        if offset != cursor:
            errors.append(f"{group}: field {field_name} starts at {offset}, expected {cursor}")
        if size <= 0:
            errors.append(f"{group}: field {field_name} must have positive size")
        cursor = offset + size
    if fields and cursor != int(record.get("record_bytes", -1)):
        errors.append(f"{group}: fields end at {cursor}, expected {record.get('record_bytes')}")
    policy = record.get("residual_epilogue_policy") or {}
    if policy.get("required_when_shift_differs") != "keep_shift_in_hot_record_or_prefold_at_model_epoch":
        errors.append(f"{group}: differing residual/skip shifts must be kept or prefolded during model-epoch load")
    return errors


def stage1_active_errors(record: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if record.get("primitive_entry_point") != STAGE1_ENTRY:
        errors.append("stage1_convnext16: active proof must expose the resident primitive entry point")
    if record.get("tile_local_hot_record_bytes") != 5776:
        errors.append("stage1_convnext16: active hot record must be 5776 expanded bytes")
    proof = record.get("active_compile_proof") or {}
    if proof.get("graph_phase") != "stage1-resident":
        errors.append("stage1_convnext16: active proof must bind the stage1-resident graph phase")
    if proof.get("uses_tile_local_seed_buffer") is not True:
        errors.append("stage1_convnext16: active proof must use the tile-local seed buffer until live load_params emission exists")
    hot_fields = record.get("tile_local_hot_field_layout") or []
    hot_cursor = 0
    for field in hot_fields:
        offset = int(field.get("offset_bytes", -1))
        size = int(field.get("bytes", -1))
        name = str(field.get("name"))
        if offset != hot_cursor:
            errors.append(f"stage1_convnext16: hot field {name} starts at {offset}, expected {hot_cursor}")
        hot_cursor = offset + size
    if hot_fields and hot_cursor != 5776:
        errors.append(f"stage1_convnext16: hot fields end at {hot_cursor}, expected 5776")
    return errors


def stage2_active_errors(record: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if record.get("primitive_entry_point") != STAGE2_ENTRY:
        errors.append("stage2_convnext16: proof must expose the Stage-2 halo-patch entry point")
    if record.get("implementation_status") != "stage2_resident_primitive_implemented_standalone_linked_and_plane_window_proofs":
        errors.append("stage2_convnext16: implementation status must describe the standalone, linked, and plane-window resident proofs")
    if record.get("tile_local_hot_record_bytes") != 5776:
        errors.append("stage2_convnext16: hot record must be 5776 expanded bytes")
    input_shape = (record.get("input_activation") or {}).get("shape")
    output_shape = (record.get("output_activation") or {}).get("shape")
    if input_shape != "c16_halo_cell" or output_shape != "c16_cell":
        errors.append("stage2_convnext16: compact proof must consume a C16 halo/plane patch and emit one C16 cell")
    alternates = record.get("additional_primitive_entry_points") or []
    if "apusr_middle_core_stage2_convnext16_3row_window_to_c16_cell_i8" in alternates:
        errors.append("stage2_convnext16: rejected three-input row-window primitive must not be cataloged as a product proof")
    context_policy = record.get("activation_context_policy") or {}
    if context_policy.get("preferred_source") != "stage1_c16_row_major_activation_ring":
        errors.append("stage2_convnext16: product path must prefer the Stage-1 row-major activation ring")
    if "do_not_synthesize_interior_stage2_halos" not in str(context_policy.get("synthetic_halo_policy")):
        errors.append("stage2_convnext16: product path must reject synthetic interior Stage2 halos")
    product_context = record.get("product_context_contract") or {}
    if product_context.get("graph_phase") != "stage1_2-linked-resident":
        errors.append("stage2_convnext16: product context must bind the stage1_2-linked-resident graph phase")
    if product_context.get("standalone_graph_phase") != "stage2-resident":
        errors.append("stage2_convnext16: product context must retain the standalone stage2-resident proof phase")
    if product_context.get("plane_window_graph_phase") != "stage2-plane-window-resident":
        errors.append("stage2_convnext16: product context must retain the compact plane-window stage2 proof phase")
    if product_context.get("context_window_shape") != "single compact 18x10xC16 producer-plane window" or int(product_context.get("context_window_bytes", -1)) != 2880:
        errors.append("stage2_convnext16: product context must declare one compact 18x10xC16 producer-plane window")
    if product_context.get("logical_halo_shape") != "18x10xC16" or int(product_context.get("logical_halo_bytes", -1)) != 2880:
        errors.append("stage2_convnext16: product context must retain the logical 18x10 C16 halo view")
    if product_context.get("source") != "worker-local Stage-1 C16 row-major activation ring":
        errors.append("stage2_convnext16: product context source must be the worker-local Stage-1 C16 row-major activation ring")
    if product_context.get("cross_worker_frame_hot_dependencies") is not False:
        errors.append("stage2_convnext16: product context must forbid cross-worker frame-hot dependencies")
    if product_context.get("cross_worker_guard_exchange") is not False:
        errors.append("stage2_convnext16: product context must forbid cross-worker guard exchange")
    if product_context.get("worker_boundary_guard_source") != "owned_or_redundantly_computed_inside_each_worker":
        errors.append("stage2_convnext16: stripe-boundary guards must be local/redundant inside each worker")
    if "three_separate_top_center_bottom_row_window_inputs" not in tuple(product_context.get("rejected_compile_carriers") or ()):
        errors.append("stage2_convnext16: rejected three-input row-window carrier must be recorded")
    hot_fields = record.get("tile_local_hot_field_layout") or []
    hot_cursor = 0
    for field in hot_fields:
        offset = int(field.get("offset_bytes", -1))
        size = int(field.get("bytes", -1))
        name = str(field.get("name"))
        if offset != hot_cursor:
            errors.append(f"stage2_convnext16: hot field {name} starts at {offset}, expected {hot_cursor}")
        hot_cursor = offset + size
    if hot_fields and hot_cursor != 5776:
        errors.append(f"stage2_convnext16: hot fields end at {hot_cursor}, expected 5776")
    return errors



def stage3_transition_errors(record: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if record.get("primitive_entry_point") != "apusr_middle_core_stage3_downsample_window_to_c32_cell_i8":
        errors.append("stage3_4_down_fnb32: transition proof must expose the Stage-3 downsample entry point")
    if record.get("implementation_status") != "active_transition_compile_proofs_downsample_and_fused_pressure":
        errors.append("stage3_4_down_fnb32: implementation status must describe the active transition proofs")
    additional = record.get("additional_primitive_entry_points") or []
    if "apusr_middle_core_stage3_4_fused_window20x40_to_c32_cell_i8" not in additional:
        errors.append("stage3_4_down_fnb32: fused pressure proof entry point must stay cataloged")
    input_shape = (record.get("input_activation") or {}).get("shape")
    output_shape = (record.get("output_activation") or {}).get("shape")
    if input_shape != "c16_cell_2x2_window" or output_shape != "c32_cell":
        errors.append("stage3_4_down_fnb32: transition proof must consume a 2x2 C16 cell window and emit one C32 cell")
    context_policy = record.get("activation_context_policy") or {}
    if context_policy.get("preferred_source") != "upstream_c16_producer_overlap_row_ring":
        errors.append("stage3_4_down_fnb32: product path must prefer the upstream C16 producer-overlap row ring")
    if "do_not_synthesize_interior_stage3_4_halos" not in str(context_policy.get("synthetic_halo_policy")):
        errors.append("stage3_4_down_fnb32: product path must reject synthetic interior Stage3/4 halos")
    product_context = record.get("product_context_contract") or {}
    if product_context.get("graph_phase") != "stage3_4-product-resident":
        errors.append("stage3_4_down_fnb32: product context must bind the stage3_4-product-resident graph phase")
    if product_context.get("context_window_shape") != "20x40xC16" or int(product_context.get("context_window_bytes", -1)) != 12800:
        errors.append("stage3_4_down_fnb32: product context must declare the 20x40xC16 fused window")
    if product_context.get("source") != "worker-local Stage-2 C16 producer-overlap row ring":
        errors.append("stage3_4_down_fnb32: product context source must be the worker-local Stage-2 C16 producer-overlap row ring")
    if product_context.get("cross_worker_frame_hot_dependencies") is not False:
        errors.append("stage3_4_down_fnb32: product context must forbid cross-worker frame-hot dependencies")
    if product_context.get("per_cell_dma_task_explosion_allowed") is not False:
        errors.append("stage3_4_down_fnb32: product context must forbid per-cell DMA task explosion")
    if record.get("tile_local_hot_record_bytes") != 13712:
        errors.append("stage3_4_down_fnb32: hot record must be 13712 expanded bytes")
    hot_fields = record.get("tile_local_hot_field_layout") or []
    hot_cursor = 0
    for field in hot_fields:
        offset = int(field.get("offset_bytes", -1))
        size = int(field.get("bytes", -1))
        name = str(field.get("name"))
        if offset != hot_cursor:
            errors.append(f"stage3_4_down_fnb32: hot field {name} starts at {offset}, expected {hot_cursor}")
        hot_cursor = offset + size
    if hot_fields and hot_cursor != 13712:
        errors.append(f"stage3_4_down_fnb32: hot fields end at {hot_cursor}, expected 13712")
    return errors


def catalog_errors(catalog: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if catalog.get("schema") != "com.apusr.middle_core_resident_records.v0":
        errors.append(f"bad resident record catalog schema: {catalog.get('schema')}")
    policy = catalog.get("policy") or {}
    for key in (
        "kernel_owns_physical_record_format",
        "source_model_owns_tensor_meaning_only",
        "records_are_model_epoch_loaded",
        "stage_specific_docs_are_not_required",
        "residual_epilogues_are_validated_per_resident_stage",
        "hot_record_omits_shift_only_when_shift_matches_output_srs",
        "reuse_available_activation_context_before_synthesizing_halos",
        "synthetic_interior_seam_halos_are_last_resort",
        "frame_boundary_padding_requires_explicit_contract",
        "performance_budgets_are_checked_by_contract_tools",
        "stage2_product_context_uses_stage1_c16_overlap",
        "stage3_4_product_context_uses_upstream_c16_overlap",
    ):
        if policy.get(key) is not True:
            errors.append(f"resident record catalog policy must be true: {key}")
    for key in ("hot_path_parameter_streams", "frame_hot_parameter_dma_tasks"):
        if int(policy.get(key, -1)) != 0:
            errors.append(f"resident record catalog must keep {key} at zero")
    if policy.get("per_cell_dma_task_explosion_allowed") is not False:
        errors.append("resident record catalog must forbid per-cell DMA task explosion")

    records = records_by_group(catalog)
    specs = resident_record_specs()
    missing = sorted(set(specs) - set(records))
    extra = sorted(set(records) - set(specs))
    if missing:
        errors.append(f"resident record catalog is missing resident groups: {missing}")
    if extra:
        errors.append(f"resident record catalog has unknown resident groups: {extra}")
    for group, spec in specs.items():
        record = records.get(group)
        if not record:
            continue
        errors.extend(common_record_errors(record))
        if record.get("logical_name") != spec.name:
            errors.append(f"{group}: logical_name drifted: {record.get('logical_name')}")
        if tuple(int(stage) for stage in record.get("logical_stages", [])) != spec.logical_stages:
            errors.append(f"{group}: logical stage binding drifted: {record.get('logical_stages')}")
        for key, value in (
            ("record_bytes", spec.record_bytes),
            ("source_record_bytes", spec.record_bytes),
            ("resident_arena_offset_bytes", spec.arena_offset_bytes),
            ("resident_arena_slice_bytes", spec.arena_slice_bytes),
        ):
            if int(record.get(key, -1)) != value:
                errors.append(f"{group}: {key}={record.get(key)} expected {value}")
        if field_triplets(record.get("field_layout") or []) != spec_field_triplets(list(spec.field_layout)):
            errors.append(f"{group}: source field layout drifted")
        if group == "stage1_convnext16":
            errors.extend(stage1_active_errors(record))
        if group == "stage2_convnext16":
            errors.extend(stage2_active_errors(record))
        if group == "stage3_4_down_fnb32":
            errors.extend(stage3_transition_errors(record))
    return errors


def ledger_errors(ledger: dict[str, Any], catalog: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    records = records_by_group(catalog)
    stages = {int(stage["stage"]): stage for stage in ledger.get("logical_stages") or []}
    if sorted(stages) != list(range(1, 13)):
        errors.append(f"ledger must describe logical stages 1..12, got {sorted(stages)}")
    for stage_num, stage in stages.items():
        group = str(stage.get("resident_parameter_group", ""))
        if group not in records:
            errors.append(f"ledger stage {stage_num} uses resident group not present in catalog: {group}")
        if int(stage.get("hot_path_parameter_streams", -1)) != 0:
            errors.append(f"ledger stage {stage_num} must keep hot-path parameter streams at zero")
        if stage.get("parameter_load_timing") != "model_epoch":
            errors.append(f"ledger stage {stage_num} must use model-epoch parameter loading")
    catalog_stage_coverage = sorted({stage for record in records.values() for stage in record.get("logical_stages", [])})
    if catalog_stage_coverage != list(range(1, 13)):
        errors.append(f"resident catalog must cover logical stages 1..12, got {catalog_stage_coverage}")
    artifacts = ledger.get("implementation_artifacts") or {}
    if artifacts.get("resident_record_catalog") != "models/fsr4_middle_core_resident_records_v0.json":
        errors.append("ledger must point to the generic resident record catalog")
    if "stage1_resident_record" in artifacts:
        errors.append("ledger must not point at a one-off Stage-1 record file")
    return errors


def header_shift_errors(stage_name: str, payload: bytes, manifest_path: Path) -> list[str]:
    errors: list[str] = []
    if len(payload) < 16:
        return [f"{manifest_path}: {stage_name} payload is too small for header"]
    header = struct.unpack_from("<4i", payload, 0)
    for lane, shift in enumerate(header):
        if shift < 0 or shift > 31:
            errors.append(f"{manifest_path}: {stage_name} header lane {lane} must fit 0..31, got {shift}")
    return errors


def arena_errors(arena_set: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    output_root = repo_path(str(arena_set.get("output_root", "models/fsr4_int8/resident_params")))
    expected_slices = resident_record_specs()
    for arena in arena_set.get("arenas") or []:
        permutation = str(arena.get("model_permutation"))
        profile = str(arena.get("parameter_profile"))
        arena_path = output_root / permutation / profile / ARENA_NAME
        manifest_path = arena_path.with_suffix(".json")
        if not arena_path.is_file() or not manifest_path.is_file():
            errors.append(f"missing resident arena or manifest for {permutation}/{profile}")
            continue
        payload = arena_path.read_bytes()
        manifest = load_json(manifest_path)
        slices = {str(slice_info.get("name")): slice_info for slice_info in manifest.get("stage_slices") or []}
        if sorted(slices) != sorted(expected_slices):
            errors.append(f"{manifest_path}: stage slice names drifted: {sorted(slices)}")
            continue
        for name, spec in expected_slices.items():
            slice_info = slices[name]
            expected_offset, expected_slice_bytes = STAGE_SLICES[name]
            expected_payload_bytes = PAYLOAD_BYTES[name]
            offset = int(slice_info.get("offset_bytes", -1))
            payload_bytes = int(slice_info.get("payload_bytes", -1))
            slice_bytes = int(slice_info.get("slice_bytes", -1))
            if (offset, payload_bytes, slice_bytes) != (expected_offset, expected_payload_bytes, expected_slice_bytes):
                errors.append(
                    f"{manifest_path}: {name} slice is offset/payload/slice "
                    f"{(offset, payload_bytes, slice_bytes)}, expected {(expected_offset, expected_payload_bytes, expected_slice_bytes)}"
                )
                continue
            stage_bytes = payload[offset : offset + payload_bytes]
            if len(stage_bytes) != payload_bytes:
                errors.append(f"{manifest_path}: {name} payload is truncated")
                continue
            if sha256_bytes(stage_bytes) != slice_info.get("sha256"):
                errors.append(f"{manifest_path}: {name} slice sha256 mismatch")
            if slice_bytes > payload_bytes:
                padding = payload[offset + payload_bytes : offset + slice_bytes]
                if any(padding):
                    errors.append(f"{manifest_path}: {name} slice padding must remain zero")
            errors.extend(header_shift_errors(name, stage_bytes, manifest_path))
            expected_epilogues = analyze_residual_epilogues(name, stage_bytes)
            if slice_info.get("resident_epilogue_modes") != expected_epilogues:
                errors.append(f"{manifest_path}: {name} resident epilogue metadata mismatch")
            for error in validate_residual_epilogues(name, stage_bytes):
                errors.append(f"{manifest_path}: {error}")
            for field in spec.field_layout:
                if "weights" in field.name:
                    block = stage_bytes[field.offset_bytes : field.offset_bytes + field.bytes]
                    if not any(value != 0 for value in block):
                        errors.append(f"{manifest_path}: {name} weight block {field.name} is all zero")
    return errors


def source_errors() -> list[str]:
    errors: list[str] = []
    graph = (REPO_ROOT / "tools/analysis/middle_core_spatial_graph.py").read_text(encoding="utf-8")
    required_graph = [
        "build_stage1_resident_program",
        "build_stage2_resident_program",
        "build_stage2_plane_window_resident_program",
        "build_stage1_2_linked_resident_program",
        "build_stage1_2_worker_ring_resident_program",
        "build_stage1_2_to_stage3_4_worker_ring_resident_program",
        "build_stage1_2_to_stage3_paired_macroblock_resident_program",
        "build_stage3_down_resident_program",
        "build_stage3_4_fused_resident_program",
        "stage2-resident",
        "stage2-plane-window-resident",
        "stage1_2-linked-resident",
        "stage1_2-worker-ring-resident",
        "stage1_4-worker-ring-resident",
        "stage1_2-to-stage3_4-worker-ring-resident",
        "stage1_2-to-stage3-paired-macroblock-resident",
        "stage3-down-resident",
        "stage3_4-fused-resident",
        "stage3_4-product-resident",
        "build_stage3_4_product_resident_program",
        "_stage3_c16_input_window_row_block_tap",
        "stage1_params = Buffer(stage1_param_ty",
        "STAGE1_HOT_RECORD_BYTES",
        "initial_value=_stage1_identity_params()",
        STAGE1_ENTRY,
        STAGE2_ENTRY,
        "STAGE2_C16_CONTEXT_BYTES",
        "apusr_middle_core_c16_cell_to_halo_i8",
        "binds the arena only",
        "placeholder",
    ]
    for needle in required_graph:
        if needle not in graph:
            errors.append(f"resident proof graph contract missing {needle!r}")
    primitive_paths = [
        REPO_ROOT / "kernels/apusr_middle_core_stage1_12_spatial2col/src/aie/apusr_middle_core_vector_primitives.cc",
        REPO_ROOT / "kernels/apusr_middle_core_stage1_12_spatial4col/src/aie/apusr_middle_core_vector_primitives.cc",
    ]
    texts = [path.read_text(encoding="utf-8") for path in primitive_paths]
    if texts[0] != texts[1]:
        errors.append("2col and 4col vector primitive sources must stay aligned")
    required_primitive = [
        "kStage1HotContractWeightsOffset",
        "kStage1HotResidualScaleBlocksOffset",
        "load_c16_m4x8_halo_patch_tap_i32",
        "aie::unpack(residual_i8)",
        "kFrontStage1CompactRecordBytes",
        "kStage1HotRecordBytes",
        "c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl",
        STAGE1_ENTRY,
        "load_c16_m4x8_halo_patch_tap",
        "store_c16_m4x8(output_c16",
        "spatial3x3_c16_halo_patch_m4x8_with_epilogue_blocks",
        "kStage1HotExpandBiasBlocksOffset",
        "kStage1HotContractBiasBlocksOffset",
        "kStage1HotResidualScaleBlocksOffset",
        "kStage34HotRecordBytes",
        "load_c16_downsample_m4x8_tap",
        "apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8",
        "c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl",
        "load_c16_m4x8_i32",
        "apusr_middle_core_stage3_downsample_window_to_c32_cell_i8",
        "apusr_middle_core_stage3_4_fused_window20x40_to_c32_cell_i8",
        "load_stage3_4_c32_halo_patch_m4x8",
    ]
    for needle in required_primitive:
        if needle not in texts[0]:
            errors.append(f"resident primitive source contract missing {needle!r}")
    if "gather_c16_m4x8_halo_patch_tap_i32" in texts[0] or "residual_block" in texts[0]:
        errors.append("resident residual path must not use scalar i8-to-i32 gather scratch")
    return errors


def validate(records_path: Path, ledger_path: Path, arena_set_path: Path) -> list[str]:
    catalog = load_json(records_path)
    ledger = load_json(ledger_path)
    arena_set = load_json(arena_set_path)
    errors: list[str] = []
    errors.extend(catalog_errors(catalog))
    errors.extend(ledger_errors(ledger, catalog))
    errors.extend(arena_errors(arena_set))
    errors.extend(source_errors())
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("records", nargs="?", type=Path, default=DEFAULT_RECORDS)
    parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    parser.add_argument("--arena-set", type=Path, default=DEFAULT_ARENA_SET)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors = validate(args.records, args.ledger, args.arena_set)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"validated middle-core resident primitive records: {args.records}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
