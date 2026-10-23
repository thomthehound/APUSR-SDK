#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Check middle-core AIE resource budgets before running MLIR-AIE placement.

The check is intentionally conservative. It is not a substitute for the real
compiler, but it catches impossible graph shapes early: huge object-FIFO elements,
large elements with depth greater than one, compute tiles that reserve too much
local memory, cores that link too many serious kernels, and halo policies that
fall back into package-sized materialization.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


DEFAULT_LIMITS = {
    "compute_tile_data_memory_bytes": 64 * 1024,
    "compute_tile_program_memory_bytes": 16 * 1024,
    "memory_tile_data_memory_bytes": 512 * 1024,
    "max_hot_fifo_element_bytes": 16 * 1024,
    "preferred_hot_fifo_element_bytes": 4 * 1024,
    "large_fifo_element_bytes": 8 * 1024,
    "max_large_fifo_depth": 1,
    "max_compute_tile_reserved_bytes": 48 * 1024,
    "max_linked_kernel_count_per_compute_tile": 2,
    "tile_dma_s2mm_channels": 2,
    "tile_dma_mm2s_channels": 2,
    "memory_tile_dma_channels": 12,
    "memory_tile_neighbor_dma_channels": 8,
    "semaphore_locks_per_memory_module": 16,
}


class BudgetError(Exception):
    pass


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise BudgetError(f"{path}: JSON root must be an object")
    return payload


def _extract_budget(path: Path) -> dict[str, Any]:
    payload = _load_json(path)
    budget = payload.get("resource_budget")
    if not isinstance(budget, dict):
        raise BudgetError(f"{path}: missing resource_budget object")
    return budget


def _merged_limits(budget: dict[str, Any]) -> dict[str, int]:
    limits = dict(DEFAULT_LIMITS)
    supplied = budget.get("limits", {})
    if not isinstance(supplied, dict):
        raise BudgetError("resource_budget.limits must be an object when present")
    for key, value in supplied.items():
        if key not in limits:
            raise BudgetError(f"unknown resource limit: {key}")
        if not isinstance(value, int) or value <= 0:
            raise BudgetError(f"resource limit {key} must be a positive integer")
        limits[key] = value
    return limits


def _require_int(obj: dict[str, Any], key: str, label: str, minimum: int = 0) -> int:
    value = obj.get(key)
    if not isinstance(value, int) or value < minimum:
        raise BudgetError(f"{label}.{key} must be an integer >= {minimum}")
    return value


def _known_tile_names(budget: dict[str, Any]) -> tuple[set[str], set[str]]:
    compute_tiles = budget.get("compute_tiles", [])
    memory_tiles = budget.get("memory_tiles", [])
    compute_names = {tile.get("name") for tile in compute_tiles if isinstance(tile, dict) and isinstance(tile.get("name"), str)}
    memory_names = {tile.get("name") for tile in memory_tiles if isinstance(tile, dict) and isinstance(tile.get("name"), str)}
    return compute_names, memory_names


def _check_cell_shapes(budget: dict[str, Any], errors: list[str]) -> dict[str, int]:
    shapes = budget.get("cell_shapes", [])
    if not isinstance(shapes, list) or not shapes:
        errors.append("resource_budget.cell_shapes must be a non-empty list")
        return {}

    bytes_by_shape: dict[str, int] = {}
    for index, shape in enumerate(shapes):
        label = f"cell_shapes[{index}]"
        if not isinstance(shape, dict):
            errors.append(f"{label} must be an object")
            continue
        name = shape.get("name")
        if not isinstance(name, str) or not name:
            errors.append(f"{label}.name must be a non-empty string")
            continue
        try:
            width = _require_int(shape, "width", label, 1)
            rows = _require_int(shape, "rows", label, 1)
            channels = _require_int(shape, "channels", label, 1)
            bytes_per_sample = _require_int(shape, "bytes_per_sample", label, 1)
            element_bytes = _require_int(shape, "element_bytes", label, 1)
        except BudgetError as exc:
            errors.append(str(exc))
            continue
        expected = width * rows * channels * bytes_per_sample
        if element_bytes != expected:
            errors.append(f"{name}: element_bytes is {element_bytes}; expected {expected} from shape")
        if name in bytes_by_shape:
            errors.append(f"duplicate cell shape: {name}")
        bytes_by_shape[name] = element_bytes
    return bytes_by_shape


def _check_halo_policies(budget: dict[str, Any], errors: list[str]) -> set[str]:
    hardware = budget.get("hardware_features", {})
    if not isinstance(hardware, dict) or not hardware:
        errors.append("resource_budget.hardware_features must describe the AIE-ML features the profile relies on")
        hardware = {}

    required_features = [
        "tile_dma_4d_tensor_address_generation",
        "memory_tile_dma_5d_tensor_address_generation",
        "semaphore_locks_for_dma_and_neighbor_exchange",
    ]
    for feature in required_features:
        if hardware.get(feature) is not True:
            errors.append(f"resource_budget.hardware_features.{feature} must be true for the spatial halo contract")

    policies = budget.get("halo_policies", [])
    if not isinstance(policies, list) or not policies:
        errors.append("resource_budget.halo_policies must be a non-empty list")
        return set()

    allowed_materialization = {
        "consumer_side_view",
        "producer_emits_micro_halo_chunk",
        "line_ring_view",
        "explicit_edge_pad_only",
        "shim_or_memory_tile_tensor_access_view",
    }
    names: set[str] = set()
    for index, policy in enumerate(policies):
        label = f"halo_policies[{index}]"
        if not isinstance(policy, dict):
            errors.append(f"{label} must be an object")
            continue
        name = policy.get("name")
        materialization = policy.get("materialization")
        if not isinstance(name, str) or not name:
            errors.append(f"{label}.name must be a non-empty string")
            continue
        if name in names:
            errors.append(f"duplicate halo policy: {name}")
        names.add(name)
        if materialization not in allowed_materialization:
            errors.append(f"{name}: unsupported halo materialization policy {materialization!r}")
        if materialization == "materialized_full_package":
            errors.append(f"{name}: full-package halo materialization is forbidden")
        uses = policy.get("uses", [])
        if not isinstance(uses, list) or any(not isinstance(item, str) for item in uses):
            errors.append(f"{name}: uses must be a list of hardware feature names")
            continue
        for feature in uses:
            if hardware.get(feature) is not True:
                errors.append(f"{name}: uses unavailable hardware feature {feature}")
    return names




def _check_public_frame_slot(budget: dict[str, Any], cell_bytes: dict[str, int], errors: list[str]) -> None:
    slot = budget.get("public_frame_slot")
    if not isinstance(slot, dict):
        errors.append("resource_budget.public_frame_slot must describe the lane-major C16 cell transport")
        return
    try:
        cell_width = _require_int(slot, "cell_width", "public_frame_slot", 1)
        cell_rows = _require_int(slot, "cell_rows", "public_frame_slot", 1)
        cell_bytes_declared = _require_int(slot, "cell_bytes", "public_frame_slot", 1)
    except BudgetError as exc:
        errors.append(str(exc))
        return
    c16_cell_bytes = cell_bytes.get("c16_cell")
    if c16_cell_bytes is None:
        errors.append("resource_budget.cell_shapes must include c16_cell for the public frame slot")
        return
    if slot.get("transport_layout") != "row_major_c16_frame":
        errors.append("public_frame_slot.transport_layout must be row_major_c16_frame")
    if cell_width != 16 or cell_rows != 8:
        errors.append("public_frame_slot must use 16x8 C16 cells")
    if cell_bytes_declared != c16_cell_bytes:
        errors.append("public_frame_slot.cell_bytes must equal c16_cell.element_bytes")
    if slot.get("lane_order") != "placement_only_vertical_stripes":
        errors.append("public_frame_slot.lane_order must be placement_only_vertical_stripes")
    if slot.get("cell_order_within_lane") != "row_major_tensor_taps":
        errors.append("public_frame_slot.cell_order_within_lane must be row_major_tensor_taps")
    if slot.get("pixel_order") != "row_major_c16_channel_inner":
        errors.append("public_frame_slot.pixel_order must be row_major_c16_channel_inner")
    if slot.get("front_ingress_guard_pixels") != 1:
        errors.append("public_frame_slot.front_ingress_guard_pixels must be 1")


def _check_capacity_classes(budget: dict[str, Any], cell_bytes: dict[str, int], errors: list[str]) -> None:
    classes = budget.get("capacity_classes")
    if not isinstance(classes, list) or not classes:
        errors.append("resource_budget.capacity_classes must describe 1080/2160/4320 frame slots")
        return
    by_name = {item.get("name"): item for item in classes if isinstance(item, dict)}
    for required in ("1080", "2160", "4320"):
        if required not in by_name:
            errors.append(f"missing capacity class: {required}")
    c16_cell_bytes = cell_bytes.get("c16_cell", 0)
    for item in classes:
        if not isinstance(item, dict):
            errors.append("capacity_classes entries must be objects")
            continue
        name = item.get("name", "<unnamed>")
        label = f"capacity_classes[{name}]"
        try:
            seam_width = _require_int(item, "seam_width", label, 1)
            seam_rows = _require_int(item, "seam_rows", label, 1)
            cell_columns = _require_int(item, "cell_columns", label, 1)
            cell_rows = _require_int(item, "cell_rows", label, 1)
            frame_slot_bytes = _require_int(item, "frame_slot_bytes", label, 1)
        except BudgetError as exc:
            errors.append(str(exc))
            continue
        if seam_width != cell_columns * 16:
            errors.append(f"{label}: seam_width must equal cell_columns * 16")
        if seam_rows != cell_rows * 8:
            errors.append(f"{label}: seam_rows must equal cell_rows * 8")
        if frame_slot_bytes != seam_width * seam_rows * 16:
            errors.append(f"{label}: frame_slot_bytes must equal seam_width * seam_rows * 16 C16 bytes")
        front_ingress_slot_bytes = item.get("front_ingress_slot_bytes")
        if not isinstance(front_ingress_slot_bytes, int) or front_ingress_slot_bytes != (seam_width + 2) * (seam_rows + 2) * 16:
            errors.append(f"{label}: front_ingress_slot_bytes must equal zero-guarded ingress C16 frame bytes")



def _target_columns_for_device(device: str) -> int:
    if device == "npu1":
        return 4
    if device == "npu2":
        return 8
    raise BudgetError(f"unknown device in row-ring budget: {device!r}")


def _split_width(total: int, parts: int, index: int) -> int:
    base = total // parts
    extra = total % parts
    return base + (1 if index < extra else 0)


def _max_physical_column_width(cell_columns: int, macro_lane_columns: int, lane_groups: int) -> int:
    max_width = 0
    for lane_index in range(lane_groups):
        lane_width = _split_width(cell_columns, lane_groups, lane_index)
        for macro_column_index in range(macro_lane_columns):
            max_width = max(max_width, _split_width(lane_width, macro_lane_columns, macro_column_index))
    return max_width


def _check_line_ring_policies(budget: dict[str, Any], cell_bytes: dict[str, int], errors: list[str]) -> None:
    policies = budget.get("line_ring_policies")
    if not isinstance(policies, list) or not policies:
        errors.append("resource_budget.line_ring_policies must be a non-empty list")
        return
    for index, policy in enumerate(policies):
        label = f"line_ring_policies[{index}]"
        if not isinstance(policy, dict):
            errors.append(f"{label} must be an object")
            continue
        name = policy.get("name")
        shape = policy.get("cell_shape")
        if not isinstance(name, str) or not name:
            errors.append(f"{label}.name must be a non-empty string")
        if not isinstance(shape, str) or shape not in cell_bytes:
            errors.append(f"{label}.cell_shape must name a declared cell shape")
        try:
            ring_rows = _require_int(policy, "ring_rows", label, 0)
        except BudgetError as exc:
            errors.append(str(exc))
            continue
        if ring_rows > 3 and shape != "c16_cell":
            errors.append(f"{label}: non-C16 rings should not exceed three rows without an explicit budget update")



def _check_row_ring_budgets(
    budget: dict[str, Any],
    limits: dict[str, int],
    cell_bytes: dict[str, int],
    fifo_bytes_by_owner: dict[str, int],
    memory_static_bytes: dict[str, int],
    errors: list[str],
) -> None:
    patches = budget.get("row_ring_budgets")
    if not isinstance(patches, dict):
        errors.append("resource_budget.row_ring_budgets must describe the front C16 halo-patch carrier")
        return
    expected = {
        "schema_version": 2,
        "owner": "front_c16_halo_patch_engine",
        "storage": "dma_tensor_halo_patch_stream",
        "cell_shape": "c16_halo_cell",
        "ring_rows": 0,
    }
    for key, value in expected.items():
        if patches.get(key) != value:
            errors.append(f"row_ring_budgets.{key} must be {value!r}")

    c16_cell = cell_bytes.get("c16_cell")
    c16_halo = cell_bytes.get("c16_halo_cell")
    if c16_cell != 2048 or c16_halo != 2880:
        errors.append("row_ring_budgets requires declared c16_cell=2048 and c16_halo_cell=2880")
        return

    capacity_by_name = {item.get("name"): item for item in budget.get("capacity_classes", []) if isinstance(item, dict)}
    entries = patches.get("budgets")
    if not isinstance(entries, list) or not entries:
        errors.append("row_ring_budgets.budgets must be a non-empty list")
        return

    seen: set[tuple[str, str]] = set()
    for index, entry in enumerate(entries):
        label = f"row_ring_budgets.budgets[{index}]"
        if not isinstance(entry, dict):
            errors.append(f"{label} must be an object")
            continue
        device = entry.get("device")
        capacity = entry.get("capacity")
        if not isinstance(device, str) or device not in {"npu1", "npu2"}:
            errors.append(f"{label}.device must be npu1 or npu2")
            continue
        if not isinstance(capacity, str) or capacity not in capacity_by_name:
            errors.append(f"{label}.capacity must name a declared capacity class")
            continue
        key = (device, capacity)
        if key in seen:
            errors.append(f"duplicate halo-patch budget for {device}/{capacity}")
        seen.add(key)

        capacity_item = capacity_by_name[capacity]
        try:
            lane_groups = _require_int(entry, "lane_groups", label, 1)
            physical_columns = _require_int(entry, "physical_columns", label, 1)
            cell_rows = _require_int(entry, "cell_rows", label, 1)
            max_cell_columns = _require_int(entry, "max_physical_column_cell_columns", label, 1)
            halo_patch_bytes = _require_int(entry, "c16_halo_patch_bytes", label, 1)
            active_fifo_bytes = _require_int(entry, "front_c16_active_fifo_bytes", label, 1)
        except BudgetError as exc:
            errors.append(str(exc))
            continue

        target_columns = _target_columns_for_device(device)
        macro_lane_columns = budget.get("macro_lane_columns")
        expected_lane_groups = target_columns // macro_lane_columns if isinstance(macro_lane_columns, int) else 0
        expected_physical_columns = expected_lane_groups * macro_lane_columns if isinstance(macro_lane_columns, int) else 0
        expected_width = _max_physical_column_width(int(capacity_item["cell_columns"]), int(macro_lane_columns), expected_lane_groups)
        if lane_groups != expected_lane_groups:
            errors.append(f"{label}.lane_groups must be {expected_lane_groups}")
        if entry.get("worker_groups") != lane_groups:
            errors.append(f"{label}.worker_groups must match lane_groups")
        if entry.get("worker_columns") != macro_lane_columns:
            errors.append(f"{label}.worker_columns must match macro_lane_columns")
        if entry.get("physical_columns_used") != expected_physical_columns:
            errors.append(f"{label}.physical_columns_used must be {expected_physical_columns}")
        if physical_columns != expected_physical_columns:
            errors.append(f"{label}.physical_columns must be {expected_physical_columns}")
        if cell_rows != capacity_item.get("cell_rows"):
            errors.append(f"{label}.cell_rows must match capacity_classes[{capacity}].cell_rows")
        if max_cell_columns != expected_width:
            errors.append(f"{label}.max_physical_column_cell_columns must be {expected_width}")
        if halo_patch_bytes != c16_halo:
            errors.append(f"{label}.c16_halo_patch_bytes must be {c16_halo}")
        if active_fifo_bytes != c16_halo:
            errors.append(f"{label}.front_c16_active_fifo_bytes must be one c16_halo_cell")
        if entry.get("fits_one_compute_tile_fifo") is not True:
            errors.append(f"{label}.fits_one_compute_tile_fifo must be true")
        if active_fifo_bytes > limits["max_hot_fifo_element_bytes"]:
            errors.append(f"{label}: active halo-patch FIFO exceeds hot FIFO element limit")

    for device in ("npu1", "npu2"):
        for capacity in ("1080", "2160", "4320"):
            if (device, capacity) not in seen:
                errors.append(f"halo-patch budgets are missing {device}/{capacity}")


def _check_front_c16_row_engine(budget: dict[str, Any], errors: list[str]) -> None:
    engine = budget.get("front_c16_row_engine")
    if not isinstance(engine, dict):
        errors.append("resource_budget.front_c16_row_engine must describe the active front C16 halo-patch contract")
        return

    expected = {
        "schema_version": 2,
        "name": "front_c16_halo_patch_engine",
        "status": "active_spatial_core_contract",
        "cell_shape": "c16_halo_cell",
        "compile_carrier": "dma_tensor_halo_patch",
        "owned_cell_bytes": 2048,
        "halo_patch_bytes": 2880,
        "front_patch_source": "zero_guarded_row_major_c16_frame_tensor_tap",
        "front_patch_fifo_objects": 1,
        "hot_path_parameter_streams": 0,
    }
    for key, value in expected.items():
        if engine.get(key) != value:
            errors.append(f"front_c16_row_engine.{key} must be {value!r}")

    if engine.get("macro_lane_columns") != budget.get("macro_lane_columns"):
        errors.append("front_c16_row_engine.macro_lane_columns must match resource_budget.macro_lane_columns")
    frame_edge_policy = str(engine.get("frame_edge_policy", ""))
    if "zero-guarded" not in frame_edge_policy:
        errors.append("front_c16_row_engine.frame_edge_policy must state the zero-guarded ingress rule")
    interior_policy = str(engine.get("interior_edge_policy", ""))
    if "row-major frame tensor tap" not in interior_policy and "source frame" not in interior_policy:
        errors.append("front_c16_row_engine.interior_edge_policy must state the frame tensor-tap halo rule")
    if "zero-pad" in interior_policy and "no interior" not in interior_policy:
        errors.append("front_c16_row_engine.interior_edge_policy must reject interior zero-padding")

    seam_exchange = engine.get("seam_exchange")
    if not isinstance(seam_exchange, dict):
        errors.append("front_c16_row_engine.seam_exchange must state that the old seam path is removed")
    else:
        expected_seam = {
            "schema_version": 2,
            "status": "removed_from_active_spatial_core",
            "shape": "none",
            "seam_bytes": 0,
            "ring_rows": 0,
            "staging": "none",
            "consumer_input_dma_channels": 1,
            "shim_streams_added": 0,
            "full_row_fifos": 0,
            "per_cell_dma_tasks": 0,
        }
        for key, value in expected_seam.items():
            if seam_exchange.get(key) != value:
                errors.append(f"front_c16_row_engine.seam_exchange.{key} must be {value!r}")

    if engine.get("required_neighbor_paths") not in ([], None):
        errors.append("front_c16_row_engine.required_neighbor_paths must be empty for the halo-patch carrier")


def _check_front_c16_seam_exchange(budget: dict[str, Any], errors: list[str]) -> None:
    seam = budget.get("front_c16_seam_exchange")
    if not isinstance(seam, dict):
        errors.append("resource_budget.front_c16_seam_exchange must state that active seam exchange is removed")
        return

    expected = {
        "schema_version": 2,
        "status": "removed_from_active_spatial_core",
        "shape": "none",
        "seam_bytes": 0,
        "ring_rows": 0,
        "staging": "none",
        "consumer_input_dma_channels": 1,
        "shim_streams_added": 0,
        "full_row_fifos": 0,
        "per_cell_dma_tasks": 0,
    }
    for key, value in expected.items():
        if seam.get(key) != value:
            errors.append(f"front_c16_seam_exchange.{key} must be {value!r}")

    source = str(seam.get("source", ""))
    destination = str(seam.get("destination", ""))
    if "row-major C16 frame tensor tap" not in source or "18x10xC16 halo patch" not in destination:
        errors.append("front_c16_seam_exchange must redirect seam work to the row-major tensor-tap halo patch")


def _check_stream_constraints(budget: dict[str, Any], errors: list[str]) -> None:
    constraints = budget.get("stream_constraints")
    if not isinstance(constraints, dict):
        errors.append("resource_budget.stream_constraints must define product ingress/egress stream limits")
        return
    expected = {
        "normal_shim_ingress_streams_per_physical_column": 1,
        "normal_shim_egress_streams_per_physical_column": 1,
        "compute_stage_activation_streams_per_consumer": 2,
        "forbid_shifted_host_frame_taps_for_line_rings": True,
        "memory_tile_owns_row_halo_and_joined_pair_seam_staging": False,
        "tensor_dma_owns_front_halo_patch_gather": True,
    }
    for key, value in expected.items():
        if constraints.get(key) != value:
            errors.append(f"stream_constraints.{key} must be {value!r}")

def _check_data_movement_budget(budget: dict[str, Any], limits: dict[str, int], errors: list[str]) -> None:
    movement = budget.get("data_movement_budget")
    if not isinstance(movement, dict):
        errors.append("resource_budget.data_movement_budget must describe DMA channels and lock pressure")
        return

    supplied_limits = movement.get("limits", {})
    if not isinstance(supplied_limits, dict):
        errors.append("data_movement_budget.limits must be an object")
        supplied_limits = {}
    for key in (
        "tile_dma_s2mm_channels",
        "tile_dma_mm2s_channels",
        "memory_tile_dma_channels",
        "memory_tile_neighbor_dma_channels",
        "semaphore_locks_per_memory_module",
    ):
        if supplied_limits.get(key) != limits[key]:
            errors.append(f"data_movement_budget.limits.{key} must be {limits[key]}")

    _, memory_names = _known_tile_names(budget)
    memory_tiles = movement.get("memory_tiles")
    if not isinstance(memory_tiles, list) or not memory_tiles:
        errors.append("data_movement_budget.memory_tiles must be a non-empty list")
        return
    seen: set[str] = set()
    for index, tile in enumerate(memory_tiles):
        label = f"data_movement_budget.memory_tiles[{index}]"
        if not isinstance(tile, dict):
            errors.append(f"{label} must be an object")
            continue
        name = tile.get("name")
        if not isinstance(name, str) or name not in memory_names:
            errors.append(f"{label}.name must reference a declared memory tile")
            continue
        if name in seen:
            errors.append(f"duplicate data movement budget for memory tile {name}")
        seen.add(name)
        try:
            s2mm = _require_int(tile, "active_s2mm_channels", label)
            mm2s = _require_int(tile, "active_mm2s_channels", label)
            neighbor = _require_int(tile, "active_neighbor_dma_channels", label)
            locks = _require_int(tile, "locks_reserved", label)
        except BudgetError as exc:
            errors.append(str(exc))
            continue
        if s2mm > limits["tile_dma_s2mm_channels"]:
            errors.append(f"{name}: active S2MM channels {s2mm} exceed {limits['tile_dma_s2mm_channels']}")
        if mm2s > limits["tile_dma_mm2s_channels"]:
            errors.append(f"{name}: active MM2S channels {mm2s} exceed {limits['tile_dma_mm2s_channels']}")
        if s2mm + mm2s > limits["memory_tile_dma_channels"]:
            errors.append(f"{name}: active local DMA channels exceed {limits['memory_tile_dma_channels']}")
        if neighbor > limits["memory_tile_neighbor_dma_channels"]:
            errors.append(f"{name}: active neighbor DMA channels {neighbor} exceed {limits['memory_tile_neighbor_dma_channels']}")
        if locks > limits["semaphore_locks_per_memory_module"]:
            errors.append(f"{name}: reserved locks {locks} exceed {limits['semaphore_locks_per_memory_module']}")
    missing = memory_names - seen
    for name in sorted(missing):
        errors.append(f"data_movement_budget.memory_tiles is missing {name}")


def _check_implementation_phases(budget: dict[str, Any], errors: list[str]) -> None:
    phases = budget.get("implementation_phases")
    if not isinstance(phases, list) or not phases:
        errors.append("resource_budget.implementation_phases must list the current graph implementation phases")
        return
    names = {phase.get("name") for phase in phases if isinstance(phase, dict)}
    if "spatial_core_stage1_12_spine" not in names:
        errors.append("resource_budget.implementation_phases must include spatial_core_stage1_12_spine")
    if "cell_conveyor_envelope" not in names:
        errors.append("resource_budget.implementation_phases must include cell_conveyor_envelope")
    if "c16_halo_envelope" not in names:
        errors.append("resource_budget.implementation_phases must include c16_halo_envelope")
    if any("line_ring" in str(name) for name in names):
        errors.append("line-ring debug phases must not be retained in the middle-core graph contract")
    for index, phase in enumerate(phases):
        label = f"implementation_phases[{index}]"
        if not isinstance(phase, dict):
            errors.append(f"{label} must be an object")
            continue
        if not isinstance(phase.get("name"), str) or not phase.get("name"):
            errors.append(f"{label}.name must be a non-empty string")
        if phase.get("name") == "spatial_core_stage1_12_spine":
            if phase.get("product_complete") is not False:
                errors.append("spatial_core_stage1_12_spine must be marked product_complete=false until all stage math lands")
            if phase.get("emit_flag") != "--emit-graph-phase=spatial-core":
                errors.append("spatial_core_stage1_12_spine emit_flag must be --emit-graph-phase=spatial-core")
        if phase.get("name") == "cell_conveyor_envelope":
            if phase.get("product_complete") is not False:
                errors.append("cell_conveyor_envelope must be marked product_complete=false")
            if phase.get("emit_flag") != "--emit-graph-phase=cell-conveyor":
                errors.append("cell_conveyor_envelope emit_flag must be --emit-graph-phase=cell-conveyor")
        if phase.get("name") == "c16_halo_envelope":
            if phase.get("product_complete") is not False:
                errors.append("c16_halo_envelope must be marked product_complete=false")
            if phase.get("emit_flag") != "--emit-graph-phase=c16-halo":
                errors.append("c16_halo_envelope emit_flag must be --emit-graph-phase=c16-halo")

def _check_object_fifos(
    budget: dict[str, Any],
    limits: dict[str, int],
    cell_bytes: dict[str, int],
    halo_policy_names: set[str],
    errors: list[str],
) -> dict[str, int]:
    fifos = budget.get("object_fifos", [])
    if not isinstance(fifos, list) or not fifos:
        errors.append("resource_budget.object_fifos must be a non-empty list")
        return {}

    compute_names, memory_names = _known_tile_names(budget)
    bytes_by_owner: dict[str, int] = {}
    owner_types: dict[str, str] = {}
    for index, fifo in enumerate(fifos):
        label = f"object_fifos[{index}]"
        if not isinstance(fifo, dict):
            errors.append(f"{label} must be an object")
            continue
        try:
            element_bytes = _require_int(fifo, "element_bytes", label, 1)
            depth = _require_int(fifo, "depth", label, 1)
        except BudgetError as exc:
            errors.append(str(exc))
            continue

        name = fifo.get("name", f"fifo_{index}")
        hot = bool(fifo.get("hot", True))
        owner = fifo.get("owner", "<unowned>")
        owner_type = fifo.get("owner_type", "compute_tile")
        if not isinstance(owner, str) or not owner:
            errors.append(f"{label}.owner must be a non-empty string")
            owner = "<invalid>"
        if owner_type not in {"compute_tile", "memory_tile", "shim"}:
            errors.append(f"{label}.owner_type must be compute_tile, memory_tile, or shim")
            owner_type = "compute_tile"
        if owner_type == "compute_tile" and owner not in compute_names:
            errors.append(f"{name}: compute-tile owner {owner!r} is not declared in resource_budget.compute_tiles")
        if owner_type == "memory_tile" and owner not in memory_names:
            errors.append(f"{name}: memory-tile owner {owner!r} is not declared in resource_budget.memory_tiles")

        shape_name = fifo.get("cell_shape")
        if not isinstance(shape_name, str) or not shape_name:
            errors.append(f"{name}: object FIFO must name a cell_shape")
        elif shape_name not in cell_bytes:
            errors.append(f"{name}: unknown cell_shape {shape_name!r}")
        elif element_bytes != cell_bytes[shape_name]:
            errors.append(f"{name}: element_bytes is {element_bytes}; cell_shape {shape_name} is {cell_bytes[shape_name]} bytes")

        halo_policy = fifo.get("halo_policy")
        if halo_policy is not None and halo_policy not in halo_policy_names:
            errors.append(f"{name}: unknown halo_policy {halo_policy!r}")

        total_bytes = element_bytes * depth
        bytes_by_owner[owner] = bytes_by_owner.get(owner, 0) + total_bytes
        owner_types[owner] = owner_type

        if hot and element_bytes > limits["max_hot_fifo_element_bytes"]:
            errors.append(
                f"{name}: hot FIFO element is {element_bytes} bytes; limit is {limits['max_hot_fifo_element_bytes']} bytes"
            )
        if hot and element_bytes > limits["large_fifo_element_bytes"] and depth > limits["max_large_fifo_depth"]:
            errors.append(
                f"{name}: {element_bytes}-byte hot FIFO elements must use depth <= {limits['max_large_fifo_depth']}, got {depth}"
            )

    for owner, total_bytes in sorted(bytes_by_owner.items()):
        owner_type = owner_types.get(owner, "compute_tile")
        if owner_type == "compute_tile" and total_bytes > limits["max_compute_tile_reserved_bytes"]:
            errors.append(
                f"{owner}: object FIFO reservation is {total_bytes} bytes; conservative compute-tile budget is {limits['max_compute_tile_reserved_bytes']} bytes"
            )
        if owner_type == "memory_tile" and total_bytes > limits["memory_tile_data_memory_bytes"]:
            errors.append(
                f"{owner}: object FIFO reservation is {total_bytes} bytes; memory-tile capacity is {limits['memory_tile_data_memory_bytes']} bytes"
            )
    return bytes_by_owner


def _check_compute_tiles(budget: dict[str, Any], limits: dict[str, int], errors: list[str]) -> dict[str, int]:
    compute_tiles = budget.get("compute_tiles", [])
    expected_count = budget.get("compute_tiles_per_macro_lane")
    if not isinstance(compute_tiles, list) or not compute_tiles:
        errors.append("resource_budget.compute_tiles must be a non-empty list")
        return {}
    if isinstance(expected_count, int) and len(compute_tiles) != expected_count:
        errors.append(f"resource_budget declares {expected_count} compute tiles per macro-lane but lists {len(compute_tiles)}")

    static_by_name: dict[str, int] = {}
    for index, tile in enumerate(compute_tiles):
        label = f"compute_tiles[{index}]"
        if not isinstance(tile, dict):
            errors.append(f"{label} must be an object")
            continue
        name = tile.get("name", f"tile_{index}")
        try:
            static_reserved_bytes = _require_int(tile, "static_reserved_bytes", label)
            linked_kernel_count = _require_int(tile, "linked_kernel_count", label)
        except BudgetError as exc:
            errors.append(str(exc))
            continue
        if not isinstance(name, str) or not name:
            errors.append(f"{label}.name must be a non-empty string")
            name = f"tile_{index}"
        if name in static_by_name:
            errors.append(f"duplicate compute tile: {name}")
        static_by_name[name] = static_reserved_bytes
        if static_reserved_bytes > limits["max_compute_tile_reserved_bytes"]:
            errors.append(
                f"{name}: reserves {static_reserved_bytes} bytes; conservative compute-tile budget is {limits['max_compute_tile_reserved_bytes']} bytes"
            )
        if linked_kernel_count > limits["max_linked_kernel_count_per_compute_tile"]:
            errors.append(
                f"{name}: links {linked_kernel_count} serious kernels; limit is {limits['max_linked_kernel_count_per_compute_tile']} before compiler proof"
            )
    return static_by_name


def _check_memory_tiles(budget: dict[str, Any], limits: dict[str, int], errors: list[str]) -> dict[str, int]:
    memory_tiles = budget.get("memory_tiles", [])
    expected_count = budget.get("memory_tiles_per_macro_lane")
    if not isinstance(memory_tiles, list) or not memory_tiles:
        errors.append("resource_budget.memory_tiles must be a non-empty list")
        return {}
    if isinstance(expected_count, int) and len(memory_tiles) != expected_count:
        errors.append(f"resource_budget declares {expected_count} memory tiles per macro-lane but lists {len(memory_tiles)}")
    static_by_name: dict[str, int] = {}
    for index, tile in enumerate(memory_tiles):
        label = f"memory_tiles[{index}]"
        if not isinstance(tile, dict):
            errors.append(f"{label} must be an object")
            continue
        name = tile.get("name", f"memory_tile_{index}")
        try:
            static_reserved_bytes = _require_int(tile, "static_reserved_bytes", label)
        except BudgetError as exc:
            errors.append(str(exc))
            continue
        if not isinstance(name, str) or not name:
            errors.append(f"{label}.name must be a non-empty string")
            name = f"memory_tile_{index}"
        if name in static_by_name:
            errors.append(f"duplicate memory tile: {name}")
        static_by_name[name] = static_reserved_bytes
        if static_reserved_bytes > limits["memory_tile_data_memory_bytes"]:
            errors.append(f"{name}: reserves {static_reserved_bytes} bytes; memory-tile capacity is {limits['memory_tile_data_memory_bytes']} bytes")
    return static_by_name


def _check_physical_tile_reservations(
    limits: dict[str, int],
    fifo_bytes_by_owner: dict[str, int],
    compute_static_bytes: dict[str, int],
    memory_static_bytes: dict[str, int],
    errors: list[str],
) -> None:
    for name, static_bytes in sorted(compute_static_bytes.items()):
        total = static_bytes + fifo_bytes_by_owner.get(name, 0)
        if total > limits["compute_tile_data_memory_bytes"]:
            errors.append(f"{name}: static+FIFO reservation is {total} bytes; compute tile data memory is {limits['compute_tile_data_memory_bytes']} bytes")
    for name, static_bytes in sorted(memory_static_bytes.items()):
        total = static_bytes + fifo_bytes_by_owner.get(name, 0)
        if total > limits["memory_tile_data_memory_bytes"]:
            errors.append(f"{name}: static+FIFO reservation is {total} bytes; memory tile data memory is {limits['memory_tile_data_memory_bytes']} bytes")


def _check_resource_sharing(
    budget: dict[str, Any],
    limits: dict[str, int],
    fifo_bytes_by_owner: dict[str, int],
    compute_static_bytes: dict[str, int],
    memory_static_bytes: dict[str, int],
    errors: list[str],
) -> None:
    sharing = budget.get("resource_sharing")
    if not isinstance(sharing, dict):
        errors.append("resource_budget.resource_sharing must document neighbor-sharing assumptions")
        return

    required = {
        "memory_tile_capacity_model": "per_physical_tile_with_neighbor_access",
        "compute_tile_capacity_model": "per_physical_tile_with_neighbor_memory_access",
        "neighbor_access_does_not_increase_physical_tile_capacity": True,
        "memory_tile_arena_groups_are_not_per_column_capacity_multipliers": True,
        "compute_tile_neighbor_memory_is_a_data_movement_path": True,
    }
    for key, expected in required.items():
        if sharing.get(key) != expected:
            errors.append(f"resource_sharing.{key} must be {expected!r}")

    compute_names, memory_names = _known_tile_names(budget)
    links = sharing.get("memory_tile_neighbor_links")
    if not isinstance(links, list) or not links:
        errors.append("resource_sharing.memory_tile_neighbor_links must describe adjacent memory-tile access")
    else:
        linked_names: set[str] = set()
        for index, link in enumerate(links):
            label = f"resource_sharing.memory_tile_neighbor_links[{index}]"
            if not isinstance(link, dict):
                errors.append(f"{label} must be an object")
                continue
            name = link.get("name")
            if not isinstance(name, str) or name not in memory_names:
                errors.append(f"{label}.name must reference a declared memory tile")
                continue
            linked_names.add(name)
            neighbors = link.get("neighbors")
            if not isinstance(neighbors, list) or any(not isinstance(item, str) for item in neighbors):
                errors.append(f"{label}.neighbors must be a list of memory-tile names")
                continue
            for neighbor in neighbors:
                if neighbor not in memory_names:
                    errors.append(f"{name}: neighbor memory tile {neighbor!r} is not declared")
                if neighbor == name:
                    errors.append(f"{name}: memory tile cannot list itself as a neighbor")
            if link.get("path") != "east_west_memory_tile_dma_channels_0_3":
                errors.append(f"{name}: neighbor path must be east_west_memory_tile_dma_channels_0_3")
        for name in sorted(memory_names - linked_names):
            errors.append(f"resource_sharing.memory_tile_neighbor_links is missing {name}")

    arena_groups = sharing.get("memory_tile_arena_groups")
    if not isinstance(arena_groups, list) or not arena_groups:
        errors.append("resource_sharing.memory_tile_arena_groups must describe shared memory-tile planning groups")
    else:
        for index, group in enumerate(arena_groups):
            label = f"resource_sharing.memory_tile_arena_groups[{index}]"
            if not isinstance(group, dict):
                errors.append(f"{label} must be an object")
                continue
            tiles = group.get("tiles")
            if not isinstance(tiles, list) or not tiles or any(not isinstance(item, str) for item in tiles):
                errors.append(f"{label}.tiles must be a non-empty list of memory-tile names")
                continue
            for tile in tiles:
                if tile not in memory_names:
                    errors.append(f"{label}: unknown memory tile {tile!r}")
            expected_bytes = len(tiles) * limits["memory_tile_data_memory_bytes"]
            if group.get("max_aggregate_bytes") != expected_bytes:
                errors.append(f"{label}.max_aggregate_bytes must be {expected_bytes}")
            reserved = sum(memory_static_bytes.get(tile, 0) + fifo_bytes_by_owner.get(tile, 0) for tile in tiles)
            if reserved > expected_bytes:
                errors.append(f"{label}: aggregate reservation is {reserved} bytes; physical group capacity is {expected_bytes} bytes")

    compute_groups = sharing.get("compute_tile_neighbor_groups")
    if not isinstance(compute_groups, list) or not compute_groups:
        errors.append("resource_sharing.compute_tile_neighbor_groups must describe compute-tile neighbor sharing")
    else:
        for index, group in enumerate(compute_groups):
            label = f"resource_sharing.compute_tile_neighbor_groups[{index}]"
            if not isinstance(group, dict):
                errors.append(f"{label} must be an object")
                continue
            tiles = group.get("tiles")
            if not isinstance(tiles, list) or not tiles or any(not isinstance(item, str) for item in tiles):
                errors.append(f"{label}.tiles must be a non-empty list of compute-tile names")
                continue
            for tile in tiles:
                if tile not in compute_names:
                    errors.append(f"{label}: unknown compute tile {tile!r}")



def check_budget(path: Path) -> None:
    budget = _extract_budget(path)
    limits = _merged_limits(budget)
    errors: list[str] = []
    macro_lane_columns = budget.get("macro_lane_columns")
    if macro_lane_columns not in {2, 4}:
        errors.append("resource_budget.macro_lane_columns must be 2 or 4")
    cell_bytes = _check_cell_shapes(budget, errors)
    _check_public_frame_slot(budget, cell_bytes, errors)
    _check_capacity_classes(budget, cell_bytes, errors)
    _check_line_ring_policies(budget, cell_bytes, errors)
    _check_front_c16_seam_exchange(budget, errors)
    _check_stream_constraints(budget, errors)
    _check_data_movement_budget(budget, limits, errors)
    _check_implementation_phases(budget, errors)
    halo_policy_names = _check_halo_policies(budget, errors)
    fifo_bytes_by_owner = _check_object_fifos(budget, limits, cell_bytes, halo_policy_names, errors)
    compute_static_bytes = _check_compute_tiles(budget, limits, errors)
    memory_static_bytes = _check_memory_tiles(budget, limits, errors)
    _check_row_ring_budgets(budget, limits, cell_bytes, fifo_bytes_by_owner, memory_static_bytes, errors)
    _check_front_c16_row_engine(budget, errors)
    _check_physical_tile_reservations(limits, fifo_bytes_by_owner, compute_static_bytes, memory_static_bytes, errors)
    _check_resource_sharing(budget, limits, fifo_bytes_by_owner, compute_static_bytes, memory_static_bytes, errors)
    if errors:
        joined = "\n".join(f"  - {error}" for error in errors)
        raise BudgetError(f"{path}: middle-core resource budget failed\n{joined}")
    print(f"validated middle-core resource budget: {path}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="manifest/design-contract JSON files with resource_budget objects")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for path in args.paths:
        check_budget(path.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
