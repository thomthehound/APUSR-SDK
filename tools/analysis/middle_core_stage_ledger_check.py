#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate the canonical middle-core stage ledger."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LEDGER = REPO_ROOT / "models/fsr4_middle_core_stage_ledger_v0.json"
DEFAULT_ARENA_SET = REPO_ROOT / "models/fsr4_int8/resident_params/stage1_12_full_arena__am020_4x4_base.json"
EXPECTED_SCHEMA = "com.apusr.middle_core_stage_ledger.v0"
EXPECTED_STAGES = list(range(1, 13))
EXPECTED_OPERATOR_FAMILIES = {
    1: "ConvNextBlock",
    2: "ConvNextBlock",
    3: "FusedConv2D_k2s2b_QuantizedOutput",
    4: "FasterNetBlock",
    5: "FasterNetBlock",
    6: "FusedConv2D_k2s2b_QuantizedOutput",
    7: "FasterNetBlock",
    8: "FasterNetBlock",
    9: "FNB_CT2D_ADD",
    10: "FasterNetBlock",
    11: "FNB_CT2D_ADD",
    12: "ConvNextBlock",
}
EXPECTED_SPATIAL_EFFECTS = {
    1: "same_scale",
    2: "same_scale",
    3: "downsample_k2s2",
    4: "same_scale",
    5: "same_scale",
    6: "downsample_k2s2",
    7: "same_scale",
    8: "same_scale",
    9: "upsample_ct2d_add",
    10: "same_scale",
    11: "upsample_ct2d_add",
    12: "same_scale",
}
EXPECTED_SKIP_ROLES = {
    2: "produces_skipA_for_stage11",
    5: "produces_skipB_for_stage9",
    9: "consumes_skipB_from_stage5",
    11: "consumes_skipA_from_stage2",
}


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def arena_manifest_path(arena_set: dict[str, Any]) -> Path:
    arenas = arena_set.get("arenas") or []
    if not arenas:
        raise ValueError("resident arena set has no arenas")
    first = arenas[0]
    output_root = REPO_ROOT / str(arena_set.get("output_root", "models/fsr4_int8/resident_params"))
    return output_root / str(first["model_permutation"]) / str(first["parameter_profile"]) / "stage1_12_full_arena.json"


def resident_slice_map(arena_set_path: Path) -> dict[str, dict[str, Any]]:
    arena_set = load_json(arena_set_path)
    per_arena_path = arena_manifest_path(arena_set)
    per_arena = load_json(per_arena_path)
    return {str(slice_info["name"]): slice_info for slice_info in per_arena.get("stage_slices") or []}


def add_stage_errors(errors: list[str], ledger: dict[str, Any]) -> None:
    stages = ledger.get("logical_stages") or []
    stage_numbers = [int(stage.get("stage", -1)) for stage in stages]
    if stage_numbers != EXPECTED_STAGES:
        errors.append(f"ledger must list logical stages 1..12 in order, got {stage_numbers}")

    physical_roles = {str(role.get("name")) for role in ledger.get("physical_roles") or []}
    shapes = set((ledger.get("activation_cell_shapes") or {}).keys())
    parameter_groups = {str(group.get("name")) for group in ledger.get("current_resident_packing_groups") or []}

    seen_names: set[str] = set()
    for stage in stages:
        index = int(stage.get("stage", -1))
        name = str(stage.get("name", ""))
        if not name or name in seen_names:
            errors.append(f"stage {index} has missing or duplicate name: {name}")
        seen_names.add(name)
        if stage.get("operator_family") != EXPECTED_OPERATOR_FAMILIES.get(index):
            errors.append(f"stage {index} operator mismatch: {stage.get('operator_family')}")
        if stage.get("spatial_effect") != EXPECTED_SPATIAL_EFFECTS.get(index):
            errors.append(f"stage {index} spatial effect mismatch: {stage.get('spatial_effect')}")
        if str(stage.get("input_cell_shape")) not in shapes:
            errors.append(f"stage {index} uses unknown input shape {stage.get('input_cell_shape')}")
        if str(stage.get("output_cell_shape")) not in shapes:
            errors.append(f"stage {index} uses unknown output shape {stage.get('output_cell_shape')}")
        if str(stage.get("physical_role")) not in physical_roles:
            errors.append(f"stage {index} uses unknown physical role {stage.get('physical_role')}")
        if str(stage.get("resident_parameter_group")) not in parameter_groups:
            errors.append(f"stage {index} uses unknown resident group {stage.get('resident_parameter_group')}")
        if stage.get("parameter_load_timing") != "model_epoch":
            errors.append(f"stage {index} must load parameters at model epoch")
        if int(stage.get("hot_path_parameter_streams", -1)) != 0:
            errors.append(f"stage {index} must not require hot-path parameter streams")
        if not stage.get("primitive_sequence"):
            errors.append(f"stage {index} lacks primitive sequence")
        if not stage.get("parameter_records"):
            errors.append(f"stage {index} lacks parameter record list")
        expected_skip = EXPECTED_SKIP_ROLES.get(index, "none")
        if stage.get("skip_role") != expected_skip:
            errors.append(f"stage {index} skip role mismatch: {stage.get('skip_role')} != {expected_skip}")


def add_group_errors(errors: list[str], ledger: dict[str, Any], resident_slices: dict[str, dict[str, Any]]) -> None:
    groups = ledger.get("current_resident_packing_groups") or []
    covered: list[int] = []
    seen_groups: set[str] = set()
    for group in groups:
        name = str(group.get("name", ""))
        if not name or name in seen_groups:
            errors.append(f"resident packing group has missing or duplicate name: {name}")
            continue
        seen_groups.add(name)
        logical_stages = [int(stage) for stage in group.get("logical_stages") or []]
        covered.extend(logical_stages)
        if name not in resident_slices:
            errors.append(f"ledger resident group is absent from checked-in arena manifests: {name}")
            continue
        resident = resident_slices[name]
        for key in ("offset_bytes", "payload_bytes"):
            if int(group.get(key, -1)) != int(resident.get(key, -2)):
                errors.append(f"resident group {name} {key} mismatch: ledger={group.get(key)} arena={resident.get(key)}")
        if len(logical_stages) > 1 and not str(group.get("packing_status", "")).startswith("provisional_"):
            errors.append(f"multi-stage resident group {name} must be explicitly provisional")
        if len(logical_stages) > 1 and group.get("split_allowed") is not True:
            errors.append(f"multi-stage resident group {name} must allow a future physical split")
    if sorted(covered) != EXPECTED_STAGES:
        errors.append(f"resident packing groups must cover stages 1..12 exactly, got {sorted(covered)}")


def add_policy_errors(errors: list[str], ledger: dict[str, Any]) -> None:
    if ledger.get("schema") != EXPECTED_SCHEMA:
        errors.append(f"bad ledger schema: {ledger.get('schema')}")
    if ledger.get("npu_owned_logical_stage_range") != [1, 12]:
        errors.append("ledger must describe NPU-owned logical stages 1..12")
    rules = ledger.get("ledger_rules") or {}
    required_true = [
        "kernel_owns_physical_parameter_format",
        "source_model_owns_tensor_meaning_only",
        "resident_parameters_load_at_model_epoch",
        "current_resident_slices_are_seed_contracts_not_sacred_layouts",
        "logical_transition_stages_are_first_class_work",
        "forbid_resident_mutation_while_old_work_can_read",
    ]
    for key in required_true:
        if rules.get(key) is not True:
            errors.append(f"ledger rule must be true: {key}")
    if int(rules.get("hot_path_parameter_streams", -1)) != 0:
        errors.append("ledger must forbid hot-path parameter streams")


def validate(ledger_path: Path, arena_set_path: Path) -> list[str]:
    errors: list[str] = []
    ledger = load_json(ledger_path)
    add_policy_errors(errors, ledger)
    add_stage_errors(errors, ledger)
    add_group_errors(errors, ledger, resident_slice_map(arena_set_path))
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ledger", nargs="?", type=Path, default=DEFAULT_LEDGER)
    parser.add_argument("--arena-set", type=Path, default=DEFAULT_ARENA_SET)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors = validate(args.ledger, args.arena_set)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"validated middle-core stage ledger: {args.ledger}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
