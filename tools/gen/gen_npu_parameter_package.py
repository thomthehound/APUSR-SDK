#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the NPU-owned FSR4 middle-core parameter closure.

The package produced here is intentionally narrower than the full FSR4 logical
model. It includes only constants consumed by the NPU-owned middle-core pass
range declared in the middle-core contract. GPU bookend constants stay outside
this package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
MODEL_ROOT = REPO_ROOT / "models" / "fsr4_int8"
LOGICAL_ROOT = MODEL_ROOT / "logical"
CONTRACT_ROOT = MODEL_ROOT / "planning" / "middle_core_contracts"
TILING_ROOT = MODEL_ROOT / "planning" / "middle_core_tiling"
ASSIST_ROOT = MODEL_ROOT / "planning" / "middle_core_hardware_assists"
DEFAULT_OUT_ROOT = MODEL_ROOT / "planning" / "npu_parameter_packages"
SCHEMA = "com.apusr.fsr4_npu_parameter_package.v2"
MACRO_LANE_WIDTH_COLUMNS = 2
PAGE_ALIGNMENT_BYTES = 32
PACKAGE_ALIGNMENT_BYTES = 4096
EPILOGUE_SCALAR_RECORD_BYTES = 16
BIAS_ACCUMULATOR_BYTES = 4
AIE_VECTOR_GRANULARITY_CHANNELS = 16

sys.path.insert(0, str(REPO_ROOT / "tools" / "reference"))
from operator_schemas import get_schema  # noqa: E402


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def align_up(value: int, alignment: int) -> int:
    return ((int(value) + int(alignment) - 1) // int(alignment)) * int(alignment)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stable_sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def round_half_away_from_zero(value: float) -> int:
    """Match the reference-shader intent more closely than Python's bankers round."""
    if not math.isfinite(value):
        raise ValueError(f"non-finite value in epilogue fit: {value}")
    magnitude = math.floor(abs(value) + 0.5)
    return int(math.copysign(magnitude, value))


def clamp_i32(value: int) -> int:
    return max(-(2**31), min((2**31) - 1, int(value)))


def read_float16_values(bundle_dir: Path, tensor: dict[str, Any]) -> list[float]:
    logical_file = tensor.get("logical_file")
    if not logical_file:
        raise ValueError(f"tensor has no logical_file: {tensor.get('tensor_id') or tensor.get('name')}")
    path = bundle_dir / str(logical_file)
    data = path.read_bytes()
    if len(data) % 2:
        raise ValueError(f"float16 tensor has odd byte count: {path}")
    return [float(value) for value in struct.unpack(f"<{len(data) // 2}e", data)]


def pack_i32_values(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}i", *values)


def fixed_point_multiplier(real_multiplier: float) -> dict[str, Any]:
    """Return a generic signed fixed-point multiply plus right-shift record."""
    if not math.isfinite(real_multiplier):
        raise ValueError(f"non-finite multiplier: {real_multiplier}")
    if real_multiplier == 0.0:
        return {
            "format": "signed_integer_multiplier_then_srs_shift",
            "multiplier_i32": 0,
            "right_shift": 0,
            "real_multiplier": 0.0,
            "reconstructed_multiplier": 0.0,
            "relative_error": 0.0,
        }
    for right_shift in range(31, -1, -1):
        scaled = round_half_away_from_zero(real_multiplier * float(1 << right_shift))
        if -(2**31) <= scaled <= (2**31) - 1:
            reconstructed = scaled / float(1 << right_shift)
            rel_error = abs(reconstructed - real_multiplier) / max(abs(real_multiplier), 1.0e-30)
            return {
                "format": "signed_integer_multiplier_then_srs_shift",
                "multiplier_i32": int(scaled),
                "right_shift": int(right_shift),
                "real_multiplier": real_multiplier,
                "reconstructed_multiplier": reconstructed,
                "relative_error": rel_error,
            }
    raise ValueError(f"could not fit multiplier into int32: {real_multiplier}")


def scalar_float(stage: dict[str, Any], name: str) -> float:
    scalars = stage.get("scalar_args") or {}
    if name not in scalars:
        raise KeyError(f"stage {stage.get('pass_index')} lacks scalar {name}")
    return float(scalars[name])


def optional_scalar_float(stage: dict[str, Any], name: str, default: float) -> float:
    scalars = stage.get("scalar_args") or {}
    return float(scalars.get(name, default))


def activation_scale(stage: dict[str, Any], name: str) -> float:
    activation = (stage.get("activations") or {}).get(name)
    if not activation:
        raise KeyError(f"stage {stage.get('pass_index')} lacks activation {name}")
    scale = activation.get("quantization_scale")
    if scale is None:
        raise ValueError(f"stage {stage.get('pass_index')} activation {name} has no quantization_scale")
    return float(scale)


def optional_activation_scale(stage: dict[str, Any], name: str, default: float) -> float:
    activation = (stage.get("activations") or {}).get(name)
    if not activation or activation.get("quantization_scale") is None:
        return float(default)
    return float(activation["quantization_scale"])


def parameter_scale(stage: dict[str, Any], name: str) -> float:
    parameter = (stage.get("parameters") or {}).get(name)
    if not parameter:
        raise KeyError(f"stage {stage.get('pass_index')} lacks parameter {name}")
    scale = parameter.get("quantization_scale")
    if scale is None:
        raise ValueError(f"parameter {name} has no quantization_scale")
    return float(scale)


def paired_weight_name(bias_name: str) -> str:
    if bias_name == "bias":
        return "weights"
    if bias_name == "ct2d_bias":
        return "ct2d_weights"
    if bias_name.startswith("bias"):
        return "weights" + bias_name[len("bias") :]
    raise ValueError(f"cannot infer paired weight for {bias_name}")


def segmented_output_scales(stage: dict[str, Any], bias_name: str, channel_count: int) -> list[dict[str, Any]]:
    op = str(stage.get("op"))
    output_qscale = optional_activation_scale(stage, "output", 1.0)
    if op == "ConvNextBlock":
        if bias_name == "bias0":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": 1.0 / scalar_float(stage, "rcprOutputScale0"), "clamp": "signed_i8"}]
        if bias_name == "bias1":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": 1.0 / scalar_float(stage, "rcprOutputScale1"), "clamp": "relu_u8_full_range_best_guess"}]
        if bias_name == "bias2":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": output_qscale, "clamp": "signed_i8", "residual_scale": activation_scale(stage, "input")}]
    if op == "FusedConv2D_k2s2b_QuantizedOutput":
        split = min(16, channel_count)
        return [
            {"channel_start": 0, "channel_end": split, "output_scale": scalar_float(stage, "quantFactor0"), "clamp": "signed_i8"},
            {"channel_start": split, "channel_end": channel_count, "output_scale": scalar_float(stage, "quantFactor1"), "clamp": "signed_i8"},
        ]
    if op == "FasterNetBlock":
        if bias_name == "bias0":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": scalar_float(stage, "quantScale1"), "clamp": "signed_i8"}]
        if bias_name == "bias1":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": scalar_float(stage, "activationScaleFactor"), "clamp": "relu_u8_full_range_best_guess"}]
        if bias_name == "bias2":
            first = optional_scalar_float(stage, "quantOutputScale0", output_qscale)
            second = optional_scalar_float(stage, "quantOutputScale1", output_qscale)
            split = channel_count // 2
            return [
                {"channel_start": 0, "channel_end": split, "output_scale": first, "clamp": "signed_i8", "residual_scale": scalar_float(stage, "quantScale0")},
                {"channel_start": split, "channel_end": channel_count, "output_scale": second, "clamp": "signed_i8", "residual_scale": scalar_float(stage, "quantScale1")},
            ]
    if op == "FNB_CT2D_ADD":
        if bias_name == "bias0":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": scalar_float(stage, "quantScale1"), "clamp": "signed_i8"}]
        if bias_name == "bias1":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": scalar_float(stage, "activationScaleFactor"), "clamp": "relu_u8_full_range_best_guess"}]
        if bias_name == "bias2":
            return [{"channel_start": 0, "channel_end": channel_count, "output_scale": scalar_float(stage, "fnbOutputScale"), "clamp": "signed_i8", "residual_scale_first_half": scalar_float(stage, "quantScale0"), "residual_scale_second_half": scalar_float(stage, "quantScale1")}]
        if bias_name == "ct2d_bias":
            first = optional_scalar_float(stage, "quantOutputScale0", output_qscale)
            second = optional_scalar_float(stage, "quantOutputScale1", output_qscale)
            split = channel_count // 2
            return [
                {"channel_start": 0, "channel_end": split, "output_scale": first, "clamp": "signed_i8", "residual_scale": activation_scale(stage, "inputAdd")},
                {"channel_start": split, "channel_end": channel_count, "output_scale": second, "clamp": "signed_i8", "residual_scale": activation_scale(stage, "inputAdd")},
            ]
    raise ValueError(f"no best-guess epilogue mapping for {op}.{bias_name}")


def accumulator_scale_for_bias(stage: dict[str, Any], bias_name: str) -> float:
    op = str(stage.get("op"))
    weight_scale = parameter_scale(stage, paired_weight_name(bias_name))
    if op == "ConvNextBlock":
        if bias_name == "bias0":
            return activation_scale(stage, "input") * weight_scale
        if bias_name == "bias1":
            return scalar_float(stage, "inputScale1") * weight_scale
        if bias_name == "bias2":
            return scalar_float(stage, "inputScale2") * weight_scale
    if op == "FusedConv2D_k2s2b_QuantizedOutput" and bias_name == "bias":
        return activation_scale(stage, "input") * weight_scale
    if op in {"FasterNetBlock", "FNB_CT2D_ADD"}:
        if bias_name == "bias0":
            return scalar_float(stage, "quantScale0") * weight_scale
        if bias_name == "bias1":
            return scalar_float(stage, "quantScale1") * weight_scale
        if bias_name == "bias2":
            return scalar_float(stage, "activationScaleFactor") * weight_scale
        if bias_name == "ct2d_bias":
            return scalar_float(stage, "fnbOutputScale") * weight_scale
    raise ValueError(f"no accumulator-scale mapping for {op}.{bias_name}")


def best_guess_bias_epilogue(bundle_dir: Path, stage: dict[str, Any], bias_name: str, parameter: dict[str, Any]) -> dict[str, Any]:
    source_values = read_float16_values(bundle_dir, parameter)
    accumulator_scale = accumulator_scale_for_bias(stage, bias_name)
    if accumulator_scale == 0.0:
        raise ValueError(f"zero accumulator scale for {stage.get('op')}.{bias_name}")
    i32_values = [clamp_i32(round_half_away_from_zero(value / accumulator_scale)) for value in source_values]
    packed = pack_i32_values(i32_values)
    saturated = sum(1 for value in i32_values if value in {-(2**31), (2**31) - 1})
    segments = []
    for segment in segmented_output_scales(stage, bias_name, len(source_values)):
        output_scale = float(segment["output_scale"])
        item = dict(segment)
        item["accumulator_scale"] = accumulator_scale
        item["best_guess_multiplier"] = fixed_point_multiplier(accumulator_scale / output_scale)
        if "residual_scale" in item:
            item["best_guess_residual_multiplier"] = fixed_point_multiplier(float(item["residual_scale"]) / output_scale)
        if "residual_scale_first_half" in item:
            item["best_guess_residual_first_half_multiplier"] = fixed_point_multiplier(float(item["residual_scale_first_half"]) / output_scale)
        if "residual_scale_second_half" in item:
            item["best_guess_residual_second_half_multiplier"] = fixed_point_multiplier(float(item["residual_scale_second_half"]) / output_scale)
        segments.append(item)
    return {
        "status": "best_guess_needs_golden_vector_investigation",
        "source_formula": "q = round((dot_i32 * accumulator_scale + bias_fp16 + residual_terms) / output_scale)",
        "runtime_formula": "q ~= srs((dot_i32 + bias_acc_i32) * multiplier_i32, right_shift) plus any residual multiplier terms",
        "rounding_policy": "host_best_guess_round_half_away_from_zero; compare against the promoted AIE SRS mode",
        "accumulator_scale": accumulator_scale,
        "bias_i32_count": len(i32_values),
        "bias_i32_min": min(i32_values) if i32_values else 0,
        "bias_i32_max": max(i32_values) if i32_values else 0,
        "bias_i32_saturated_count": saturated,
        "bias_i32_sha256": stable_sha256_bytes(packed),
        "bias_i32_values": i32_values,
        "segments": segments,
        "investigation_needed": [
            "Compare host best-guess rounding against the actual AIE rounding/saturation mode.",
            "Fit primitive-level multiplier/shift records against synthetic accumulator sweeps.",
            "Re-check residual/skip add ordering once the primitive body is promoted.",
            "Replace this best-guess record with measured records before product arithmetic is declared final.",
        ],
    }



def tensor_file_size(bundle_dir: Path, tensor: dict[str, Any]) -> int:
    logical_file = tensor.get("logical_file")
    if not logical_file:
        raise ValueError(f"tensor has no logical_file: {tensor.get('tensor_id') or tensor.get('name')}")
    path = bundle_dir / str(logical_file)
    if not path.exists():
        raise FileNotFoundError(f"missing parameter file: {path}")
    expected_sha = tensor.get("logical_sha256")
    actual_sha = sha256_file(path)
    if expected_sha and actual_sha != expected_sha:
        raise ValueError(f"sha256 mismatch for {path}: expected {expected_sha}, got {actual_sha}")
    return path.stat().st_size


def parameter_dimensions(parameter: dict[str, Any]) -> dict[str, int]:
    names = list(parameter.get("descriptor_dim_names") or [])
    dims = [int(value) for value in (parameter.get("logical_size") or [])]
    dim_by_name = {str(name): int(value) for name, value in zip(names, dims, strict=True)}
    return {
        "kernel_width": int(dim_by_name.get("KW", dim_by_name.get("W", 1))),
        "kernel_height": int(dim_by_name.get("KH", dim_by_name.get("H", 1))),
        "input_channels": int(dim_by_name.get("IC", dim_by_name.get("C", 1))),
        "output_channels": int(dim_by_name.get("OC", dim_by_name.get("N", dim_by_name.get("C", 1)))),
    }


def pack_family(stage_op: str, parameter_name: str, parameter: dict[str, Any]) -> str:
    role = str(parameter.get("role"))
    if role == "bias":
        return "epilogue_bias_i32_accum_pending_fit_v1"
    if role != "weight":
        return "unsupported"
    dims = parameter_dimensions(parameter)
    kw = dims["kernel_width"]
    kh = dims["kernel_height"]
    ic = dims["input_channels"]
    oc = dims["output_channels"]
    if parameter_name == "ct2d_weights":
        return "ct2d_k2s2_hwcn_aie_page_v0"
    if kw == 1 and kh == 1:
        return "pointwise_mmul_16x16_page_v0"
    if kw == 2 and kh == 2:
        return "spatial_k2s2_window_page_v0"
    if kw == 3 and kh == 3 and ic == oc:
        return "depthwise_or_full_k3_window_page_v0"
    if kw == 3 and kh == 3:
        return "partial_k3_channel_window_page_v0"
    return f"spatial_{kw}x{kh}_window_page_v0"


def pack_layout_note(pack: str) -> str:
    if pack == "pointwise_mmul_16x16_page_v0":
        return "Pack as 16-channel input/output tiles for aie::mmul or AM020 mac_4x8_8x8 microkernels; final byte order is locked by the promoted primitive kernel."
    if pack == "ct2d_k2s2_hwcn_aie_page_v0":
        return "Keep CT2D weights as a dedicated 2x2 transposed-convolution page; do not fold them into the skip-add epilogue."
    if pack.endswith("window_page_v0"):
        return "Pack for channel-window consumption from line buffers, not for HLSL texture/load order; exact lane micro-order is finalized by the primitive kernel."
    if pack == "epilogue_bias_i32_accum_pending_fit_v1":
        return "Host-side epilogue fitting converts the FP16 source bias to int32 accumulator-domain records before any AIE hot-path use; FP16 remains provenance only."
    return "No product packing rule yet."


def discover_permutations() -> list[str]:
    return sorted(path.parent.name for path in LOGICAL_ROOT.glob("*/logical_model.json"))


def selected_stage_range(contract: dict[str, Any]) -> tuple[int, int]:
    pass_range = contract.get("npu_owned_pass_range") or [1, 12]
    if len(pass_range) != 2:
        raise ValueError(f"bad npu_owned_pass_range: {pass_range}")
    return int(pass_range[0]), int(pass_range[1])


def scalar_record(stage: dict[str, Any], name: str, value: Any) -> dict[str, Any]:
    lowered = name.lower()
    numeric_value = float(value)
    if "rcpr" in lowered:
        semantic = "reciprocal_output_scale"
    elif "output" in lowered and "scale" in lowered:
        semantic = "output_scale"
    elif "input" in lowered and "scale" in lowered:
        semantic = "input_scale"
    elif "quant" in lowered:
        semantic = "quantization_factor"
    elif "activation" in lowered:
        semantic = "activation_scale"
    elif "fnb" in lowered:
        semantic = "fused_block_output_scale"
    else:
        semantic = "scalar"
    return {
        "name": name,
        "source_value": str(value),
        "source_dtype": "float32_string",
        "runtime_dtype": "fixed_point_epilogue_record",
        "semantic": semantic,
        "record_family": "epilogue_multiplier_shift_srs_best_guess_v0",
        "record_bytes": EPILOGUE_SCALAR_RECORD_BYTES,
        "hot_path_fp16_allowed": False,
        "product_status": "best_guess_needs_golden_vector_investigation",
        "consumer_pass": int(stage["pass_index"]),
        "best_guess_fixed_point": fixed_point_multiplier(numeric_value),
        "investigation_note": "This scalar is kept as an integer fixed-point seed. Primitive-specific records on bias pages carry the more useful combined multiplier/output-scale terms.",
    }


def bias_runtime_bytes(parameter: dict[str, Any]) -> int:
    dims = [int(value) for value in parameter.get("logical_size") or []]
    if len(dims) != 1:
        raise ValueError(f"expected one-dimensional bias tensor, got {dims}")
    return dims[0] * BIAS_ACCUMULATOR_BYTES


def build_stage_page(
    bundle_dir: Path,
    stage: dict[str, Any],
    parameter_name: str,
    parameter: dict[str, Any],
    offset: int,
) -> tuple[dict[str, Any], int]:
    pass_index = int(stage["pass_index"])
    stage_op = str(stage["op"])
    role = str(parameter.get("role"))
    source_dtype = str(parameter.get("dtype"))
    source_bytes = tensor_file_size(bundle_dir, parameter)
    runtime_dtype = "int32" if role == "bias" else source_dtype
    runtime_bytes = bias_runtime_bytes(parameter) if role == "bias" else source_bytes
    packed_bytes = align_up(runtime_bytes, PAGE_ALIGNMENT_BYTES)
    family = pack_family(stage_op, parameter_name, parameter)
    page = {
        "page_id": f"p{pass_index:02d}_{parameter_name}",
        "consumer_pass": pass_index,
        "consumer_op": stage_op,
        "parameter_name": parameter_name,
        "role": role,
        "dtype": runtime_dtype,
        "source_dtype": source_dtype,
        "runtime_dtype": runtime_dtype,
        "logical_size": parameter.get("logical_size"),
        "descriptor_dim_names": parameter.get("descriptor_dim_names"),
        "source_logical_file": parameter.get("logical_file"),
        "source_logical_sha256": parameter.get("logical_sha256"),
        "source_quantization_scale": parameter.get("quantization_scale"),
        "source_bytes": source_bytes,
        "runtime_bytes": runtime_bytes,
        "page_offset_bytes": offset,
        "page_alignment_bytes": PAGE_ALIGNMENT_BYTES,
        "page_bytes": packed_bytes,
        "page_padding_bytes": packed_bytes - runtime_bytes,
        "pack_family": family,
        "pack_layout_note": pack_layout_note(family),
    }
    if role == "weight":
        dims = parameter_dimensions(parameter)
        page["kernel"] = dims
        page["channel_granularity_ok"] = bool(
            dims["input_channels"] % AIE_VECTOR_GRANULARITY_CHANNELS == 0
            and dims["output_channels"] % AIE_VECTOR_GRANULARITY_CHANNELS == 0
        )
    elif role == "bias":
        page["bias_epilogue_contract"] = {
            "source_dtype": source_dtype,
            "runtime_dtype": "int32",
            "conversion_owner": "host_package_epilogue_best_guess",
            "accumulator_domain_status": "best_guess_seeded_needs_golden_vector_investigation",
            "hot_path_fp16_allowed": False,
            "source_fp16_kept_as_provenance_only": True,
            "best_guess": best_guess_bias_epilogue(bundle_dir, stage, parameter_name, parameter),
        }
    return page, offset + packed_bytes


def build_package(permutation: str, profile: str) -> dict[str, Any]:
    logical_path = LOGICAL_ROOT / permutation / "logical_model.json"
    contract_path = CONTRACT_ROOT / permutation / "middle_core_contract.json"
    tiling_path = TILING_ROOT / permutation / f"{profile}.json"
    assist_path = ASSIST_ROOT / permutation / f"{profile}.json"
    for required in [logical_path, contract_path, tiling_path, assist_path]:
        if not required.exists():
            raise FileNotFoundError(required)

    logical = load_json(logical_path)
    contract = load_json(contract_path)
    tiling = load_json(tiling_path)
    assist = load_json(assist_path)
    start_pass, end_pass = selected_stage_range(contract)
    bundle_dir = logical_path.parent
    lane_count = int(tiling["hardware_profile"]["lane_count"])
    if lane_count % MACRO_LANE_WIDTH_COLUMNS != 0:
        raise ValueError(f"lane_count {lane_count} is not divisible by macro-lane width {MACRO_LANE_WIDTH_COLUMNS}")
    macro_lane_count = lane_count // MACRO_LANE_WIDTH_COLUMNS
    memory_tile_bytes = int(tiling["hardware_facts_used"]["memory_tile_bytes"])
    memory_tile_bytes_per_macro_lane = memory_tile_bytes * MACRO_LANE_WIDTH_COLUMNS

    offset = 0
    pages: list[dict[str, Any]] = []
    stages: list[dict[str, Any]] = []
    scalar_records: list[dict[str, Any]] = []
    seen_page_ids: set[str] = set()

    for stage in logical.get("stages") or []:
        if stage.get("kind") != "operator_call" or stage.get("pass_index") is None:
            continue
        pass_index = int(stage["pass_index"])
        if not start_pass <= pass_index <= end_pass:
            continue
        schema = get_schema(str(stage["op"]))
        if schema is None:
            raise ValueError(f"no schema for NPU-owned op {stage.get('op')} in {logical_path}")
        stage_parameters = stage.get("parameters") or {}
        missing_params = sorted(set(schema.parameter_args) - set(stage_parameters))
        extra_params = sorted(set(stage_parameters) - set(schema.parameter_args))
        if missing_params or extra_params:
            raise ValueError(
                f"stage {pass_index} parameter schema mismatch: missing={missing_params} extra={extra_params}"
            )
        stage_scalar_names = set(stage.get("scalar_args") or {})
        missing_required_scalars = sorted(set(schema.required_scalar_args) - stage_scalar_names)
        unknown_scalars = sorted(stage_scalar_names - set(schema.allowed_scalar_args()))
        if missing_required_scalars or unknown_scalars:
            raise ValueError(
                f"stage {pass_index} scalar schema mismatch: missing={missing_required_scalars} unknown={unknown_scalars}"
            )

        stage_page_ids: list[str] = []
        role_counts: dict[str, int] = {}
        for parameter_name, parameter in sorted(stage_parameters.items()):
            page, offset = build_stage_page(bundle_dir, stage, parameter_name, parameter, offset)
            if page["page_id"] in seen_page_ids:
                raise ValueError(f"duplicate page id {page['page_id']}")
            seen_page_ids.add(str(page["page_id"]))
            pages.append(page)
            stage_page_ids.append(str(page["page_id"]))
            role_counts[str(page["role"])] = role_counts.get(str(page["role"]), 0) + 1

        stage_scalars = [scalar_record(stage, name, value) for name, value in sorted((stage.get("scalar_args") or {}).items())]
        scalar_records.extend(stage_scalars)
        stages.append(
            {
                "pass": pass_index,
                "op": stage.get("op"),
                "primitive_sequence": list(schema.primitive_sequence),
                "page_ids": stage_page_ids,
                "parameter_role_counts": role_counts,
                "scalar_arg_names": [record["name"] for record in stage_scalars],
                "epilogue_status": "best_guess_fixed_point_records_need_investigation" if stage_scalars else "no_named_scale_args",
            }
        )

    if len(stages) != end_pass - start_pass + 1:
        raise ValueError(f"expected {end_pass - start_pass + 1} NPU-owned stages, got {len(stages)}")

    page_bytes = sum(int(page["page_bytes"]) for page in pages)
    scalar_bytes = len(scalar_records) * EPILOGUE_SCALAR_RECORD_BYTES
    per_lane_payload_bytes = page_bytes + scalar_bytes
    per_lane_allocation_bytes = align_up(per_lane_payload_bytes, PACKAGE_ALIGNMENT_BYTES)
    excluded = summarize_excluded_parameters(logical, bundle_dir, start_pass, end_pass)

    return {
        "schema": SCHEMA,
        "permutation": permutation,
        "hardware_profile": tiling["hardware_profile"],
        "source": {
            "logical_model": str(logical_path.relative_to(REPO_ROOT)).replace("\\", "/"),
            "middle_core_contract": str(contract_path.relative_to(REPO_ROOT)).replace("\\", "/"),
            "tiling_plan": str(tiling_path.relative_to(REPO_ROOT)).replace("\\", "/"),
            "hardware_assist_plan": str(assist_path.relative_to(REPO_ROOT)).replace("\\", "/"),
            "model_format": logical.get("format"),
            "initializer_sha256": (logical.get("source") or {}).get("initializer_sha256"),
        },
        "closure_policy": {
            "npu_owned_pass_range": [start_pass, end_pass],
            "gpu_bookends_excluded": True,
            "inactive_permutations_excluded": True,
            "include_only_named_stage_parameter_closure": True,
            "per_frame_parameter_traffic_allowed": False,
            "runtime_parameters_policy": "RTP-style scalar controls are allowed later; bulk weights/biases are immutable macro-lane resident pages uploaded at context creation/reconfiguration.",
        },
        "numeric_policy": {
            "hot_path_fp16_allowed": False,
            "fp16_source_provenance_allowed": True,
            "bias_runtime_dtype": "int32",
            "bias_conversion_owner": "host_package_epilogue_best_guess",
            "scale_runtime_dtype": "fixed_point_epilogue_record",
            "srs_owner": "primitive_epilogue",
            "product_status": "best_guess_seeded_needs_golden_vector_investigation",
        },
        "storage_policy": {
            "residency_granularity": "macro_lane",
            "macro_lane_replicated": True,
            "macro_lane_width_columns": MACRO_LANE_WIDTH_COLUMNS,
            "macro_lane_count": macro_lane_count,
            "memory_tiles_per_macro_lane": MACRO_LANE_WIDTH_COLUMNS,
            "memory_tile_bytes_per_memory_tile": memory_tile_bytes,
            "memory_tile_bytes_per_macro_lane": memory_tile_bytes_per_macro_lane,
            "page_alignment_bytes": PAGE_ALIGNMENT_BYTES,
            "package_allocation_alignment_bytes": PACKAGE_ALIGNMENT_BYTES,
            "per_macro_lane_source_page_bytes": sum(int(page["source_bytes"]) for page in pages),
            "per_macro_lane_runtime_page_bytes": sum(int(page["runtime_bytes"]) for page in pages),
            "per_macro_lane_packed_page_bytes": page_bytes,
            "per_macro_lane_scalar_record_bytes": scalar_bytes,
            "per_macro_lane_package_payload_bytes": per_lane_payload_bytes,
            "per_macro_lane_package_allocation_bytes": per_lane_allocation_bytes,
            "per_macro_lane_memory_strip_fits": per_lane_allocation_bytes <= memory_tile_bytes_per_macro_lane,
            "all_macro_lanes_package_allocation_bytes": per_lane_allocation_bytes * macro_lane_count,
            "lane_replicated": False,
            "per_lane_source_page_bytes": sum(int(page["source_bytes"]) for page in pages),
            "per_lane_runtime_page_bytes": sum(int(page["runtime_bytes"]) for page in pages),
            "per_lane_packed_page_bytes": page_bytes,
            "per_lane_scalar_record_bytes": scalar_bytes,
            "per_lane_package_payload_bytes": per_lane_payload_bytes,
            "per_lane_package_allocation_bytes": per_lane_allocation_bytes,
            "memory_tile_bytes_per_lane": memory_tile_bytes,
            "per_lane_memory_tile_fits": per_lane_allocation_bytes <= memory_tile_bytes,
            "all_lanes_package_allocation_bytes": per_lane_allocation_bytes * lane_count,
            "legacy_lane_fields_note": "per_lane_* fields are retained for old planning scripts. The product residency unit is the selected spatial macro-lane memory strip.",
        },
        "stages": sorted(stages, key=lambda item: int(item["pass"])),
        "pages": pages,
        "epilogue_scalar_records": scalar_records,
        "excluded_logical_parameters": excluded,
        "validation_invariants": [
            "Every page has exactly one NPU middle-core consumer pass.",
            "Every NPU-owned stage parameter appears in exactly one package page.",
            "No GPU bookend parameter is included in this package.",
            "No per-frame parameter upload is allowed in the product path.",
            "No NPU hot-path page or scalar record may expose float16 as its runtime dtype.",
            "Best-guess fixed-point epilogue records are starting seeds only and must be replaced with measured primitive records before product arithmetic is declared final.",
        ],
    }


def summarize_excluded_parameters(logical: dict[str, Any], bundle_dir: Path, start_pass: int, end_pass: int) -> dict[str, Any]:
    excluded_count = 0
    excluded_bytes = 0
    excluded_passes: dict[str, int] = {}
    for stage in logical.get("stages") or []:
        if stage.get("kind") != "operator_call" or stage.get("pass_index") is None:
            continue
        pass_index = int(stage["pass_index"])
        if start_pass <= pass_index <= end_pass:
            continue
        for _name, parameter in (stage.get("parameters") or {}).items():
            if parameter.get("role") not in {"weight", "bias"}:
                continue
            excluded_count += 1
            excluded_passes[str(pass_index)] = excluded_passes.get(str(pass_index), 0) + 1
            try:
                excluded_bytes += tensor_file_size(bundle_dir, parameter)
            except FileNotFoundError:
                pass
    return {
        "count": excluded_count,
        "source_bytes": excluded_bytes,
        "by_pass": excluded_passes,
        "reason": "These parameters belong to GPU bookends or non-NPU stages for the current product split.",
    }


def generate_all(profile: str, out_root: Path) -> None:
    for permutation in discover_permutations():
        if not (TILING_ROOT / permutation / f"{profile}.json").exists():
            continue
        write_json(out_root / permutation / f"{profile}.json", build_package(permutation, profile))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("permutation", nargs="?", help="logical permutation name such as balanced__2160")
    parser.add_argument("--all", action="store_true", help="generate package manifests for every checked-in permutation")
    parser.add_argument("--profile", default="am020_4x4_base", help="tiling profile name")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.all:
        generate_all(args.profile, args.out_root)
        print(f"generated NPU parameter package manifests for {args.profile} under {args.out_root}")
        return 0
    if not args.permutation:
        raise SystemExit("pass a permutation or --all")
    out = args.out_root / args.permutation / f"{args.profile}.json"
    write_json(out, build_package(args.permutation, args.profile))
    print(f"generated {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
