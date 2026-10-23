#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate the middle-core stage placement and fusion/fission plan."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PLACEMENT = REPO_ROOT / "models/fsr4_middle_core_stage_placement_v0.json"
DEFAULT_LEDGER = REPO_ROOT / "models/fsr4_middle_core_stage_ledger_v0.json"
SPATIAL_MANIFESTS = {
    "spatial2col": REPO_ROOT / "kernels/apusr_middle_core_stage1_12_spatial2col/design_contract.json",
    "spatial4col": REPO_ROOT / "kernels/apusr_middle_core_stage1_12_spatial4col/design_contract.json",
}
EXPECTED_SCHEMA = "com.apusr.middle_core_stage_placement.v0"
EXPECTED_STAGES = list(range(1, 13))
EXPECTED_PROFILES = {
    "spatial2col": {
        "macro_lane_columns": 2,
        "compute_tiles_per_macro_lane": 8,
        "memory_tiles_per_macro_lane": 2,
        "allowed_verdict": "diagnostic_only_not_product_candidate",
    },
    "spatial4col": {
        "macro_lane_columns": 4,
        "compute_tiles_per_macro_lane": 16,
        "memory_tiles_per_macro_lane": 4,
        "allowed_verdict": "canonical_product_profile",
    },
}
EXPECTED_HARD_SPLITS = {
    (2, 3),
    (5, 6),
    (8, 9),
    (10, 11),
}
EXPECTED_SPLITTABLE_GROUPS = {
    "stage3_4_down_fnb32",
    "stage6_7_down_fnb64",
}


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def add_policy_errors(errors: list[str], placement: dict[str, Any]) -> None:
    if placement.get("schema") != EXPECTED_SCHEMA:
        errors.append(f"bad placement schema: {placement.get('schema')}")
    policy = placement.get("policy") or {}
    required_true = [
        "stage_ledger_is_source_of_truth",
        "same_physical_role_does_not_imply_monolithic_kernel_fusion",
        "consumer_halo_requires_expanded_footprint_or_internal_boundary",
        "transition_stages_are_first_class_split_candidates",
        "spatial4col_is_canonical_product_profile",
        "spatial2col_is_diagnostic_only",
        "producer_rings_require_physical_memory_tile_placement",
        "worker_groups_are_independent_product_units",
        "cross_worker_frame_hot_dependencies_forbidden",
        "npu1_four_column_worker_is_product_floor",
    ]
    for key in required_true:
        if policy.get(key) is not True:
            errors.append(f"placement policy must be true: {key}")
    if int(policy.get("hot_path_parameter_streams", -1)) != 0:
        errors.append("placement plan must preserve zero hot-path parameter streams")


def add_hardware_errors(errors: list[str], placement: dict[str, Any], spatial_contracts: dict[str, dict[str, Any]]) -> None:
    assumptions = placement.get("am020_connectivity_assumptions") or {}
    reference_limits = spatial_contracts["spatial2col"].get("resource_budget", {}).get("limits", {})
    movement_limits = spatial_contracts["spatial2col"].get("resource_budget", {}).get("data_movement_budget", {}).get("limits", {})
    expected = {
        "compute_tile_data_memory_bytes": reference_limits.get("compute_tile_data_memory_bytes"),
        "compute_tile_program_memory_bytes": reference_limits.get("compute_tile_program_memory_bytes"),
        "compute_tile_dma_s2mm_channels": movement_limits.get("tile_dma_s2mm_channels"),
        "compute_tile_dma_mm2s_channels": movement_limits.get("tile_dma_mm2s_channels"),
        "memory_tile_data_memory_bytes": reference_limits.get("memory_tile_data_memory_bytes"),
        "memory_tile_dma_channels": movement_limits.get("memory_tile_dma_channels"),
        "memory_tile_neighbor_dma_channels": movement_limits.get("memory_tile_neighbor_dma_channels"),
        "semaphore_locks_per_memory_module": movement_limits.get("semaphore_locks_per_memory_module"),
    }
    for key, expected_value in expected.items():
        if expected_value is None:
            errors.append(f"spatial design contract lacks expected hardware limit {key}")
            continue
        if int(assumptions.get(key, -1)) != int(expected_value):
            errors.append(f"placement assumption {key}={assumptions.get(key)} does not match design contract {expected_value}")
    if assumptions.get("compute_tile_neighbor_memory_visibility") is not True:
        errors.append("placement assumptions must include compute-tile neighbor memory visibility")
    if assumptions.get("memory_tile_neighbor_access") != "east_west":
        errors.append("placement assumptions must keep memory-tile neighbor access east_west")



def add_worker_group_contract_errors(errors: list[str], placement: dict[str, Any]) -> None:
    contract = placement.get("worker_group_contract") or {}
    expected = {
        "profile": "spatial4col",
        "worker_is_atomic_product_unit": True,
        "worker_columns": 4,
        "npu1_auto_worker_groups": 1,
        "npu2_auto_worker_groups": 2,
        "cross_worker_frame_hot_dependencies": False,
        "cross_worker_guard_exchange": False,
    }
    for key, value in expected.items():
        if contract.get(key) != value:
            errors.append(f"worker_group_contract.{key} must be {value!r}, got {contract.get(key)!r}")
    if "independent" not in str(contract.get("auto_semantics", "")):
        errors.append("worker_group_contract.auto_semantics must state independent workers")
    if "redundantly computes" not in str(contract.get("guard_policy", "")):
        errors.append("worker_group_contract.guard_policy must state local/redundant guard production")

def add_profile_errors(errors: list[str], placement: dict[str, Any], spatial_contracts: dict[str, dict[str, Any]]) -> None:
    profiles = placement.get("profile_feasibility") or {}
    for profile, expected in EXPECTED_PROFILES.items():
        info = profiles.get(profile)
        if not isinstance(info, dict):
            errors.append(f"placement plan must include profile feasibility for {profile}")
            continue
        if info.get("verdict") != expected["allowed_verdict"]:
            errors.append(f"{profile} feasibility verdict drifted: {info.get('verdict')}")
        budget = spatial_contracts[profile].get("resource_budget", {})
        for key in ("macro_lane_columns", "compute_tiles_per_macro_lane", "memory_tiles_per_macro_lane"):
            if int(info.get(key, -1)) != int(expected[key]):
                errors.append(f"{profile} placement {key} must be {expected[key]}, got {info.get(key)}")
            if int(budget.get(key, -2)) != int(expected[key]):
                errors.append(f"{profile} design contract {key} must be {expected[key]}, got {budget.get(key)}")
        memory_tiles = {tile.get("name") for tile in budget.get("memory_tiles") or []}
        pressure_tiles = {entry.get("tile") for entry in info.get("memory_tile_pressure") or []}
        if memory_tiles != pressure_tiles:
            errors.append(f"{profile} memory-tile pressure list must match design contract tiles: {pressure_tiles} != {memory_tiles}")
    two_col_verdict = profiles.get("spatial2col", {}).get("verdict")
    if two_col_verdict != "diagnostic_only_not_product_candidate":
        errors.append("2col must remain diagnostic-only after the producer-ring placement decision")
    four_col_verdict = profiles.get("spatial4col", {}).get("verdict")
    if four_col_verdict != "canonical_product_profile":
        errors.append("4col must be the canonical product profile after the producer-ring placement decision")


def add_role_group_errors(errors: list[str], placement: dict[str, Any], ledger: dict[str, Any]) -> None:
    ledger_roles = {role.get("name"): role for role in ledger.get("physical_roles") or []}
    stage_to_role = {int(stage["stage"]): stage["physical_role"] for stage in ledger.get("logical_stages") or []}
    covered: list[int] = []
    for group in placement.get("role_fusion_groups") or []:
        role = str(group.get("role", ""))
        if role not in ledger_roles:
            errors.append(f"role fusion group uses unknown role {role}")
            continue
        stages = [int(stage) for stage in group.get("logical_stages") or []]
        covered.extend(stages)
        for stage in stages:
            if stage not in stage_to_role:
                errors.append(f"role fusion group {role} uses unknown stage {stage}")
            elif stage_to_role[stage] != role:
                errors.append(f"role fusion group {role} includes stage {stage}, but ledger role is {stage_to_role[stage]}")
        if not group.get("fusion_status"):
            errors.append(f"role fusion group {role} lacks fusion_status")
        if not group.get("parameter_implication"):
            errors.append(f"role fusion group {role} lacks parameter_implication")
    if sorted(covered) != EXPECTED_STAGES:
        errors.append(f"role fusion groups must cover logical stages 1..12 exactly, got {sorted(covered)}")


def add_boundary_errors(errors: list[str], placement: dict[str, Any], ledger: dict[str, Any]) -> None:
    stage_numbers = {int(stage["stage"]) for stage in ledger.get("logical_stages") or []}
    boundaries = placement.get("boundary_decisions") or []
    numeric_pairs: list[tuple[int, int]] = []
    has_egress_boundary = False
    for boundary in boundaries:
        producer = boundary.get("producer_stage")
        consumer = boundary.get("consumer_stage")
        if not isinstance(producer, int) or producer not in stage_numbers:
            errors.append(f"boundary has invalid producer stage: {producer}")
            continue
        if consumer == "egress_pack":
            if producer != 12:
                errors.append("only stage 12 may feed egress_pack")
            has_egress_boundary = True
        elif isinstance(consumer, int) and consumer in stage_numbers:
            numeric_pairs.append((producer, consumer))
        else:
            errors.append(f"boundary {producer} has invalid consumer stage: {consumer}")
        for key in ("default_decision", "connection_class", "fusion_option", "defer_reason", "parameter_implication"):
            if not boundary.get(key):
                errors.append(f"boundary {producer}->{consumer} lacks {key}")
    expected_pairs = [(stage, stage + 1) for stage in range(1, 12)]
    if numeric_pairs != expected_pairs:
        errors.append(f"boundary decisions must list adjacent stage pairs in order, got {numeric_pairs}")
    if not has_egress_boundary:
        errors.append("boundary decisions must include stage 12 -> egress_pack")
    hard_pairs = {(int(boundary["producer_stage"]), int(boundary["consumer_stage"])) for boundary in boundaries if isinstance(boundary.get("consumer_stage"), int) and boundary.get("hard_split") is True}
    if not EXPECTED_HARD_SPLITS.issubset(hard_pairs):
        errors.append(f"hard split set must include {sorted(EXPECTED_HARD_SPLITS)}, got {sorted(hard_pairs)}")
    for boundary in boundaries:
        consumer = boundary.get("consumer_stage")
        if isinstance(consumer, int) and boundary.get("producer_stage") in {3, 6}:
            if "splittable" not in str(boundary.get("parameter_implication", "")):
                errors.append(f"boundary {boundary.get('producer_stage')}->{consumer} must keep grouped transition records splittable")


def add_resident_group_errors(errors: list[str], placement: dict[str, Any], ledger: dict[str, Any]) -> None:
    ledger_groups = {str(group.get("name")): group for group in ledger.get("current_resident_packing_groups") or []}
    for group_name in EXPECTED_SPLITTABLE_GROUPS:
        group = ledger_groups.get(group_name)
        if not group:
            errors.append(f"ledger must keep provisional split candidate {group_name}")
            continue
        if group.get("split_allowed") is not True:
            errors.append(f"ledger group {group_name} must remain split_allowed")
        if not str(group.get("packing_status", "")).startswith("provisional_"):
            errors.append(f"ledger group {group_name} must remain provisional")
    text = json.dumps(placement)
    for group_name in EXPECTED_SPLITTABLE_GROUPS:
        if group_name not in text:
            errors.append(f"placement plan must mention splittable resident group {group_name}")


def validate(placement_path: Path, ledger_path: Path) -> list[str]:
    placement = load_json(placement_path)
    ledger = load_json(ledger_path)
    spatial_contracts = {profile: load_json(path) for profile, path in SPATIAL_MANIFESTS.items()}
    errors: list[str] = []
    add_policy_errors(errors, placement)
    add_worker_group_contract_errors(errors, placement)
    add_hardware_errors(errors, placement, spatial_contracts)
    add_profile_errors(errors, placement, spatial_contracts)
    add_role_group_errors(errors, placement, ledger)
    add_boundary_errors(errors, placement, ledger)
    add_resident_group_errors(errors, placement, ledger)
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("placement", nargs="?", type=Path, default=DEFAULT_PLACEMENT)
    parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors = validate(args.placement, args.ledger)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"validated middle-core stage placement plan: {args.placement}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
