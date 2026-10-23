#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate HLSL constants for APUSR's FSR4 GPU bookend shaders.

APUSR owns generated FSR4 product artifacts; raw/logical extraction data lives in the external tooling workspace.
The data was originally derived from the FFX INT8 SDK through the earlier model-tooling flow, but the
repo-local copy is now the source of truth for bookend shader generation.
"""

from __future__ import annotations

import argparse
import json
import pathlib
from dataclasses import dataclass
from typing import Any, Iterable


MODE_ORDER = (
    ("native", "NATIVE", "APUSR_FSR4_PRESET_NATIVEAA"),
    ("quality", "QUALITY", "APUSR_FSR4_PRESET_QUALITY"),
    ("balanced", "BALANCED", "APUSR_FSR4_PRESET_BALANCED"),
    ("performance", "PERFORMANCE", "APUSR_FSR4_PRESET_PERFORMANCE"),
    ("drs", "DRS", "APUSR_FSR4_PRESET_DRS"),
    ("ultraperf", "ULTRAPERF", "APUSR_FSR4_PRESET_ULTRAPERF"),
)


@dataclass(frozen=True)
class ModeBookendData:
    mode: str
    symbol: str
    preset_macro: str
    stage0_weights: list[int]
    stage0_bias: list[int]
    stage0_scale: float
    stage13_input_scale: float
    stage13_output_tensor: str
    stage13_weights0: list[int]
    stage13_bias0: list[int]
    stage13_weights1: list[int]
    stage13_bias1: list[int]
    stage13_weights2: list[int]
    stage13_bias2: list[int]
    stage13_ct2d_weights: list[int]
    stage13_ct2d_bias: list[int]
    stage13_weight0_scale: float
    stage13_weight1_scale: float
    stage13_weight2_scale: float
    stage13_ct2d_weight_scale: float
    stage13_cnb_output_scale: float
    stage13_input_scale1: float
    stage13_input_scale2: float
    stage13_rcpr_output_scale0: float
    stage13_rcpr_output_scale1: float


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[1]


def read_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def stage_by_pass(stages: list[dict[str, Any]], pass_index: int) -> dict[str, Any]:
    for stage in stages:
        if stage.get("pass_index") == pass_index:
            return stage
    raise ValueError(f"logical model is missing pass_index {pass_index}")


def tensor_file(bundle_dir: pathlib.Path, tensor: dict[str, Any]) -> pathlib.Path:
    path = bundle_dir / tensor["logical_file"]
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def read_u16_le(path: pathlib.Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 2 != 0:
        raise ValueError(f"{path} byte size is not divisible by 2")
    return [data[i] | (data[i + 1] << 8) for i in range(0, len(data), 2)]


def read_i8(path: pathlib.Path) -> list[int]:
    return [value - 256 if value >= 128 else value for value in path.read_bytes()]


def pack_u16_pair(lo: int, hi: int) -> int:
    return (lo & 0xffff) | ((hi & 0xffff) << 16)


def pack_i8_quad(values: Iterable[int]) -> int:
    packed = 0
    for index, value in enumerate(values):
        packed |= (value & 0xff) << (index * 8)
    return packed


def raw_index(dims: list[int], indices: Iterable[int]) -> int:
    index = 0
    stride = 1
    for dim, coord in zip(reversed(dims), reversed(list(indices))):
        index += coord * stride
        stride *= dim
    return index


def pack_stage0_weights(raw_u16: list[int], logical_size: list[int]) -> list[int]:
    # the logical metadata stores this fp16 tensor as [KW, KH, IC, OC] without the
    # padded 8th input channel.  The APUSR shader constants are [OC][quad][IC/2]
    # and include the padded zero channel because the bookend consumes two
    # half4 feature vectors.
    kw, kh, input_channels, output_channels = logical_size
    if (kw, kh, input_channels, output_channels) != (2, 2, 7, 16):
        raise ValueError(f"unexpected stage0 weight shape: {logical_size}")
    if len(raw_u16) != kw * kh * input_channels * output_channels:
        raise ValueError("stage0 weight element count mismatch")

    packed: list[int] = []
    for output in range(output_channels):
        for ky in range(kh):
            for kx in range(kw):
                for channel in range(0, 8, 2):
                    lo = raw_u16[raw_index(logical_size, (kx, ky, channel, output))] if channel < input_channels else 0
                    hi_channel = channel + 1
                    hi = raw_u16[raw_index(logical_size, (kx, ky, hi_channel, output))] if hi_channel < input_channels else 0
                    packed.append(pack_u16_pair(lo, hi))
    if len(packed) != 256:
        raise AssertionError(len(packed))
    return packed


def pack_bias_h2(raw_u16: list[int], logical_size: list[int], expected_channels: int, name: str) -> list[int]:
    if logical_size != [expected_channels]:
        raise ValueError(f"unexpected {name} bias shape: {logical_size}")
    if len(raw_u16) != expected_channels:
        raise ValueError(f"{name} bias element count mismatch: {len(raw_u16)}")
    return [pack_u16_pair(raw_u16[i], raw_u16[i + 1]) for i in range(0, expected_channels, 2)]


def pack_nhwc_i8_by_output(raw_i8: list[int], logical_size: list[int], expected_shape: tuple[int, int, int, int], name: str) -> list[int]:
    # Produces [KY][KX][OC][IC/4] dwords.  The shader uses this shape directly
    # for DP4A-style work while still treating the model tensor as NHWC.
    if tuple(logical_size) != expected_shape:
        raise ValueError(f"unexpected {name} shape: {logical_size}")
    kw, kh, input_channels, output_channels = logical_size
    if len(raw_i8) != kw * kh * input_channels * output_channels:
        raise ValueError(f"{name} element count mismatch")
    if input_channels % 4 != 0:
        raise ValueError(f"{name} input channel count must be divisible by 4")

    packed: list[int] = []
    for ky in range(kh):
        for kx in range(kw):
            for output in range(output_channels):
                for channel in range(0, input_channels, 4):
                    packed.append(pack_i8_quad(
                        raw_i8[raw_index(logical_size, (kx, ky, channel + lane, output))]
                        for lane in range(4)
                    ))
    return packed


def pack_hwcn_i8_by_output(raw_i8: list[int], logical_size: list[int], expected_shape: tuple[int, int, int, int], name: str) -> list[int]:
    # Produces [PY][PX][OC][IC/4] dwords for the final 2x2 CT2D projection.
    # the logical metadata labels this tensor as HWCN: [KH, KW, output_channel, input_channel].
    if tuple(logical_size) != expected_shape:
        raise ValueError(f"unexpected {name} shape: {logical_size}")
    kh, kw, output_channels, input_channels = logical_size
    if len(raw_i8) != kh * kw * output_channels * input_channels:
        raise ValueError(f"{name} element count mismatch")
    if input_channels % 4 != 0:
        raise ValueError(f"{name} input channel count must be divisible by 4")

    packed: list[int] = []
    for py in range(kh):
        for px in range(kw):
            for output in range(output_channels):
                for channel in range(0, input_channels, 4):
                    packed.append(pack_i8_quad(
                        raw_i8[raw_index(logical_size, (py, px, output, channel + lane))]
                        for lane in range(4)
                    ))
    return packed


def mode_bundle(logical_root: pathlib.Path, mode: str) -> pathlib.Path:
    # Bookend weights are resolution-independent in the extracted FSR4 INT8
    # bundles.  Use the 2160 bundle as the canonical source and verify the other
    # resolutions agree.
    return logical_root / f"{mode}__2160"


def verify_resolution_independent(logical_root: pathlib.Path, mode: str, canonical: dict[str, Any]) -> None:
    canonical_stage0 = stage_by_pass(canonical["stages"], 0)
    canonical_stage13 = stage_by_pass(canonical["stages"], 13)
    tensor_names = (
        (canonical_stage0, "weights"),
        (canonical_stage0, "bias"),
        (canonical_stage13, "weights0"),
        (canonical_stage13, "bias0"),
        (canonical_stage13, "weights1"),
        (canonical_stage13, "bias1"),
        (canonical_stage13, "weights2"),
        (canonical_stage13, "bias2"),
        (canonical_stage13, "ct2d_weights"),
        (canonical_stage13, "ct2d_bias"),
    )
    canonical_shas = {name: stage["parameters"][name]["logical_sha256"] for stage, name in tensor_names}
    canonical_scalars = {
        "stage0_scale": canonical_stage0["activations"]["output"]["quantization_scale"],
        "stage13_input_scale": canonical_stage13["activations"]["input"]["quantization_scale"],
        **canonical_stage13["scalar_args"],
    }
    for resolution in (1080, 2160, 4320):
        path = logical_root / f"{mode}__{resolution}" / "logical_model.json"
        model = read_json(path)
        stage0 = stage_by_pass(model["stages"], 0)
        stage13 = stage_by_pass(model["stages"], 13)
        stages_by_name = {"weights": stage0, "bias": stage0}
        stages_by_name.update({name: stage13 for name in canonical_shas if name not in stages_by_name})
        for name, sha in canonical_shas.items():
            if stages_by_name[name]["parameters"][name]["logical_sha256"] != sha:
                raise ValueError(f"{mode} {name} differs across resolutions")
        scalars = {
            "stage0_scale": stage0["activations"]["output"]["quantization_scale"],
            "stage13_input_scale": stage13["activations"]["input"]["quantization_scale"],
            **stage13["scalar_args"],
        }
        for name, value in canonical_scalars.items():
            if scalars[name] != value:
                raise ValueError(f"{mode} {name} differs across resolutions")


def load_mode_data(logical_root: pathlib.Path, mode: str, symbol: str, preset_macro: str) -> ModeBookendData:
    bundle_dir = mode_bundle(logical_root, mode)
    model = read_json(bundle_dir / "logical_model.json")
    verify_resolution_independent(logical_root, mode, model)
    stage0 = stage_by_pass(model["stages"], 0)
    stage13 = stage_by_pass(model["stages"], 13)
    params = stage13["parameters"]
    scalars = stage13["scalar_args"]

    raw_stage0_weights = read_u16_le(tensor_file(bundle_dir, stage0["parameters"]["weights"]))
    raw_stage0_bias = read_u16_le(tensor_file(bundle_dir, stage0["parameters"]["bias"]))
    return ModeBookendData(
        mode=mode,
        symbol=symbol,
        preset_macro=preset_macro,
        stage0_weights=pack_stage0_weights(raw_stage0_weights, stage0["parameters"]["weights"]["logical_size"]),
        stage0_bias=pack_bias_h2(raw_stage0_bias, stage0["parameters"]["bias"]["logical_size"], 16, "stage0"),
        stage0_scale=float(stage0["activations"]["output"]["quantization_scale"]),
        stage13_input_scale=float(stage13["activations"]["input"]["quantization_scale"]),
        stage13_output_tensor=stage13["boundaries"]["output"]["tensor_id"],
        stage13_weights0=pack_nhwc_i8_by_output(
            read_i8(tensor_file(bundle_dir, params["weights0"])), params["weights0"]["logical_size"], (3, 3, 16, 16), "stage13 weights0"),
        stage13_bias0=pack_bias_h2(read_u16_le(tensor_file(bundle_dir, params["bias0"])), params["bias0"]["logical_size"], 16, "stage13 bias0"),
        stage13_weights1=pack_nhwc_i8_by_output(
            read_i8(tensor_file(bundle_dir, params["weights1"])), params["weights1"]["logical_size"], (1, 1, 16, 32), "stage13 weights1"),
        stage13_bias1=pack_bias_h2(read_u16_le(tensor_file(bundle_dir, params["bias1"])), params["bias1"]["logical_size"], 32, "stage13 bias1"),
        stage13_weights2=pack_nhwc_i8_by_output(
            read_i8(tensor_file(bundle_dir, params["weights2"])), params["weights2"]["logical_size"], (1, 1, 32, 16), "stage13 weights2"),
        stage13_bias2=pack_bias_h2(read_u16_le(tensor_file(bundle_dir, params["bias2"])), params["bias2"]["logical_size"], 16, "stage13 bias2"),
        stage13_ct2d_weights=pack_hwcn_i8_by_output(
            read_i8(tensor_file(bundle_dir, params["ct2d_weights"])), params["ct2d_weights"]["logical_size"], (2, 2, 8, 16), "stage13 ct2d weights"),
        stage13_ct2d_bias=pack_bias_h2(read_u16_le(tensor_file(bundle_dir, params["ct2d_bias"])), params["ct2d_bias"]["logical_size"], 8, "stage13 ct2d bias"),
        stage13_weight0_scale=float(params["weights0"]["quantization_scale"]),
        stage13_weight1_scale=float(params["weights1"]["quantization_scale"]),
        stage13_weight2_scale=float(params["weights2"]["quantization_scale"]),
        stage13_ct2d_weight_scale=float(params["ct2d_weights"]["quantization_scale"]),
        stage13_cnb_output_scale=float(scalars["cnbOutputScale"]),
        stage13_input_scale1=float(scalars["inputScale1"]),
        stage13_input_scale2=float(scalars["inputScale2"]),
        stage13_rcpr_output_scale0=float(scalars["rcprOutputScale0"]),
        stage13_rcpr_output_scale1=float(scalars["rcprOutputScale1"]),
    )


def format_uint_array(name: str, values: Iterable[int], columns: int = 8) -> str:
    values = list(values)
    lines = [f"static const uint {name}[{len(values)}] = {{"]
    for offset in range(0, len(values), columns):
        chunk = values[offset : offset + columns]
        lines.append("    " + ", ".join(f"0x{value & 0xffffffff:x}" for value in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def append_switch(parts: list[str], return_type: str, function_name: str, suffix: str, default_symbol: str, cast: str = "") -> None:
    parts.append(f"{return_type} {function_name}(uint preset{', uint index' if return_type == 'uint' else ''})")
    parts.append("{")
    parts.append("    switch (preset) {")
    for _mode, symbol, preset_macro in MODE_ORDER:
        expr = f"Apusr{suffix}_{symbol}"
        if return_type == "uint":
            expr += "[index]"
        if cast:
            expr = f"{cast}({expr})"
        parts.append(f"    case {preset_macro}: return {expr};")
    default_expr = f"Apusr{suffix}_{default_symbol}"
    if return_type == "uint":
        default_expr += "[index]"
    if cast:
        default_expr = f"{cast}({default_expr})"
    parts.append("    default:")
    parts.append(f"        return {default_expr};")
    parts.append("    }")
    parts.append("}")
    parts.append("")


def generate_header(entries: list[ModeBookendData]) -> str:
    parts = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// Copyright (c) 2026 thomthehound.",
        "// Auto-generated by tools/generate_fsr4_bookend_weights.py from FSR4 INT8 metadata.",
        "",
        "#ifndef APUSR_FSR4_BOOKEND_WEIGHTS_HLSLI",
        "#define APUSR_FSR4_BOOKEND_WEIGHTS_HLSLI",
        "",
    ]
    for entry in entries:
        parts.append(format_uint_array(f"ApusrStage0Weights_{entry.symbol}", entry.stage0_weights))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage0Bias_{entry.symbol}", entry.stage0_bias))
        parts.append("")
        parts.append(f"static const float ApusrStage0Scale_{entry.symbol} = {entry.stage0_scale:.17g};")
        parts.append(f"static const float ApusrStage13InputScale_{entry.symbol} = {entry.stage13_input_scale:.17g};")
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Weights0_{entry.symbol}", entry.stage13_weights0))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Bias0_{entry.symbol}", entry.stage13_bias0))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Weights1_{entry.symbol}", entry.stage13_weights1))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Bias1_{entry.symbol}", entry.stage13_bias1))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Weights2_{entry.symbol}", entry.stage13_weights2))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Bias2_{entry.symbol}", entry.stage13_bias2))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Ct2dWeights_{entry.symbol}", entry.stage13_ct2d_weights))
        parts.append("")
        parts.append(format_uint_array(f"ApusrStage13Ct2dBias_{entry.symbol}", entry.stage13_ct2d_bias))
        parts.append("")
        parts.append(f"static const float ApusrStage13Weight0Scale_{entry.symbol} = {entry.stage13_weight0_scale:.17g};")
        parts.append(f"static const float ApusrStage13Weight1Scale_{entry.symbol} = {entry.stage13_weight1_scale:.17g};")
        parts.append(f"static const float ApusrStage13Weight2Scale_{entry.symbol} = {entry.stage13_weight2_scale:.17g};")
        parts.append(f"static const float ApusrStage13Ct2dWeightScale_{entry.symbol} = {entry.stage13_ct2d_weight_scale:.17g};")
        parts.append(f"static const float ApusrStage13CnbOutputScale_{entry.symbol} = {entry.stage13_cnb_output_scale:.17g};")
        parts.append(f"static const float ApusrStage13InputScale1_{entry.symbol} = {entry.stage13_input_scale1:.17g};")
        parts.append(f"static const float ApusrStage13InputScale2_{entry.symbol} = {entry.stage13_input_scale2:.17g};")
        parts.append(f"static const float ApusrStage13RcprOutputScale0_{entry.symbol} = {entry.stage13_rcpr_output_scale0:.17g};")
        parts.append(f"static const float ApusrStage13RcprOutputScale1_{entry.symbol} = {entry.stage13_rcpr_output_scale1:.17g};")
        parts.append("")

    append_switch(parts, "uint", "ApusrStage0Weight", "Stage0Weights", "BALANCED")
    append_switch(parts, "uint", "ApusrStage0Bias", "Stage0Bias", "BALANCED")
    append_switch(parts, "float", "ApusrStage0Scale", "Stage0Scale", "BALANCED")
    append_switch(parts, "float", "ApusrStage13InputScale", "Stage13InputScale", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Weight0", "Stage13Weights0", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Bias0", "Stage13Bias0", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Weight1", "Stage13Weights1", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Bias1", "Stage13Bias1", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Weight2", "Stage13Weights2", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Bias2", "Stage13Bias2", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Ct2dWeight", "Stage13Ct2dWeights", "BALANCED")
    append_switch(parts, "uint", "ApusrStage13Ct2dBias", "Stage13Ct2dBias", "BALANCED")
    append_switch(parts, "float", "ApusrStage13Weight0Scale", "Stage13Weight0Scale", "BALANCED")
    append_switch(parts, "float", "ApusrStage13Weight1Scale", "Stage13Weight1Scale", "BALANCED")
    append_switch(parts, "float", "ApusrStage13Weight2Scale", "Stage13Weight2Scale", "BALANCED")
    append_switch(parts, "float", "ApusrStage13Ct2dWeightScale", "Stage13Ct2dWeightScale", "BALANCED")
    append_switch(parts, "float", "ApusrStage13CnbOutputScale", "Stage13CnbOutputScale", "BALANCED")
    append_switch(parts, "float", "ApusrStage13InputScale1", "Stage13InputScale1", "BALANCED")
    append_switch(parts, "float", "ApusrStage13InputScale2", "Stage13InputScale2", "BALANCED")
    append_switch(parts, "float", "ApusrStage13RcprOutputScale0", "Stage13RcprOutputScale0", "BALANCED")
    append_switch(parts, "float", "ApusrStage13RcprOutputScale1", "Stage13RcprOutputScale1", "BALANCED")

    parts.append("#endif // APUSR_FSR4_BOOKEND_WEIGHTS_HLSLI")
    parts.append("")
    return "\n".join(parts)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model-data-dir",
        type=pathlib.Path,
        default=None,
        help="FSR4 INT8 model data root. Defaults to models/fsr4_int8 for legacy local checkouts.",
    )
    parser.add_argument("--output", type=pathlib.Path, default=repo_root() / "shaders/generated/apusr_fsr4_bookend_weights.hlsli")
    parser.add_argument("--verify", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    data_root = args.model_data_dir if args.model_data_dir is not None else (repo_root() / "models/fsr4_int8")
    logical_root = data_root / "int8" / "logical" if (data_root / "int8" / "logical").is_dir() else data_root / "logical"
    if not logical_root.is_dir():
        raise SystemExit(f"FSR4 logical metadata not found: {logical_root}")
    entries = [load_mode_data(logical_root, mode, symbol, preset_macro) for mode, symbol, preset_macro in MODE_ORDER]
    generated = generate_header(entries)
    if args.verify:
        existing = args.output.read_text(encoding="utf-8")
        if existing != generated:
            raise SystemExit("FSR4 bookend weight header is stale. Regenerate it with tools/generate_fsr4_bookend_weights.py.")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
