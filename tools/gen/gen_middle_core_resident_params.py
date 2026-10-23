#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the packed resident parameter arena used by the Stage1->12 middle-core.

The NPU parameter package manifest is the source of truth for logical ownership,
quantized tensor provenance, and best-guess integer epilogue records. This tool
keeps the resident arena ABI stable while the spatial middle-core rewrite changes
the internal graph shape.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any, Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.model.resident_arena_layout import analyze_residual_epilogues

DEFAULT_PACKAGE_ROOT = REPO_ROOT / "models/fsr4_int8/planning/npu_parameter_packages"
DEFAULT_OUTPUT_ROOT = REPO_ROOT / "models/fsr4_int8/resident_params"
DEFAULT_PARAMETER_PROFILE = "am020_4x4_base"
DEFAULT_ARENA_NAME = "stage1_12_full_arena.bin"
FULL_ARENA_BYTES = 122_880
FULL_ARENA_PAYLOAD_BYTES = 122_160
ALIGNMENT_BYTES = 4096

STAGE_SLICES = {
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


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def i32_pack(values: Iterable[int]) -> bytes:
    value_list = [int(value) for value in values]
    if not value_list:
        return b""
    return struct.pack(f"<{len(value_list)}i", *value_list)


def zero_i32(count: int) -> list[int]:
    return [0 for _ in range(count)]


def package_bundle_dir(package: dict[str, Any]) -> Path:
    logical_model = REPO_ROOT / str(package["source"]["logical_model"])
    return logical_model.parent


def page_map(package: dict[str, Any]) -> dict[tuple[int, str], dict[str, Any]]:
    return {(int(page["consumer_pass"]), str(page["parameter_name"])): page for page in package["pages"]}


def page_for(pages: dict[tuple[int, str], dict[str, Any]], pass_index: int, parameter_name: str) -> dict[str, Any]:
    key = (pass_index, parameter_name)
    if key not in pages:
        raise KeyError(f"parameter page is missing from NPU package: pass={pass_index} name={parameter_name}")
    return pages[key]


def read_weight(bundle_dir: Path, page: dict[str, Any], expected_bytes: int) -> bytes:
    if page.get("role") != "weight":
        raise ValueError(f"expected weight page, got {page.get('page_id')}")
    tensor_path = bundle_dir / str(page["source_logical_file"])
    payload = tensor_path.read_bytes()
    digest = sha256_bytes(payload)
    if digest != page.get("source_logical_sha256"):
        raise ValueError(f"source tensor digest mismatch for {tensor_path}: {digest}")
    if len(payload) != expected_bytes:
        raise ValueError(f"{page.get('page_id')} has {len(payload)} bytes; expected {expected_bytes}")
    return payload


def bias_values(page: dict[str, Any], expected_channels: int) -> list[int]:
    values = [int(value) for value in page["bias_epilogue_contract"]["best_guess"]["bias_i32_values"]]
    if len(values) != expected_channels:
        raise ValueError(f"{page.get('page_id')} has {len(values)} bias values; expected {expected_channels}")
    return values


def segment_values(page: dict[str, Any], key: str, expected_channels: int) -> tuple[list[int], list[int]]:
    values = [0 for _ in range(expected_channels)]
    shifts = [0 for _ in range(expected_channels)]
    covered = [False for _ in range(expected_channels)]
    for segment in page["bias_epilogue_contract"]["best_guess"]["segments"]:
        if key not in segment:
            raise ValueError(f"{page.get('page_id')} segment lacks {key}")
        start = int(segment["channel_start"])
        end = int(segment["channel_end"])
        if start < 0 or end > expected_channels or start >= end:
            raise ValueError(f"{page.get('page_id')} has invalid channel segment {start}:{end}")
        multiplier = int(segment[key]["multiplier_i32"])
        right_shift = int(segment[key]["right_shift"])
        for channel in range(start, end):
            values[channel] = multiplier
            shifts[channel] = right_shift
            covered[channel] = True
    if not all(covered):
        raise ValueError(f"{page.get('page_id')} does not cover all {expected_channels} channels for {key}")
    return values, shifts


def single_shift(shifts: Iterable[int], label: str) -> int:
    shift_set = {int(shift) for shift in shifts}
    if len(shift_set) != 1:
        raise ValueError(f"{label} requires one vector SRS shift, got {sorted(shift_set)}")
    return next(iter(shift_set))


def param_blob(parts: list[bytes], expected_bytes: int, label: str) -> bytes:
    payload = b"".join(parts)
    if len(payload) > expected_bytes:
        raise ValueError(f"{label} is {len(payload)} bytes; expected at most {expected_bytes}")
    return payload + bytes(expected_bytes - len(payload))


def convnext16(bundle_dir: Path, pages: dict[tuple[int, str], dict[str, Any]], pass_index: int) -> bytes:
    weights0 = read_weight(bundle_dir, page_for(pages, pass_index, "weights0"), 2304)
    weights1 = read_weight(bundle_dir, page_for(pages, pass_index, "weights1"), 512)
    weights2 = read_weight(bundle_dir, page_for(pages, pass_index, "weights2"), 512)
    bias0 = page_for(pages, pass_index, "bias0")
    bias1 = page_for(pages, pass_index, "bias1")
    bias2 = page_for(pages, pass_index, "bias2")
    mult0, shift0 = segment_values(bias0, "best_guess_multiplier", 16)
    mult1, shift1 = segment_values(bias1, "best_guess_multiplier", 32)
    mult2, shift2 = segment_values(bias2, "best_guess_multiplier", 16)
    residual_mult, residual_shift = segment_values(bias2, "best_guess_residual_multiplier", 16)
    header = i32_pack([
        single_shift(shift0, f"pass {pass_index} spatial"),
        single_shift(shift1, f"pass {pass_index} expand"),
        single_shift(shift2, f"pass {pass_index} contract"),
        0,
    ])
    return param_blob([
        header,
        weights0,
        i32_pack(bias_values(bias0, 16)),
        i32_pack(mult0),
        weights1,
        i32_pack(bias_values(bias1, 32)),
        i32_pack(mult1),
        weights2,
        i32_pack(bias_values(bias2, 16)),
        i32_pack(mult2),
        i32_pack(residual_mult),
        i32_pack(residual_shift),
        i32_pack(zero_i32(16)),
    ], 4048, f"pass {pass_index} ConvNext16")


def down_fnb(bundle_dir: Path, pages: dict[tuple[int, str], dict[str, Any]], down_pass: int, fnb_pass: int, channels: int) -> bytes:
    hidden = channels * 2
    spatial = channels // 2
    down_weight_bytes = 4 * (channels // 8) * (channels // 16) * 64
    spatial_weight_bytes = 9 * (spatial // 8) * 2 * 64
    expand_weight_bytes = (hidden // 8) * (channels // 8) * 64
    contract_weight_bytes = (channels // 8) * (hidden // 8) * 64
    down_bias = page_for(pages, down_pass, "bias")
    bias0 = page_for(pages, fnb_pass, "bias0")
    bias1 = page_for(pages, fnb_pass, "bias1")
    bias2 = page_for(pages, fnb_pass, "bias2")
    down_mult, down_shift = segment_values(down_bias, "best_guess_multiplier", channels)
    mult0, shift0 = segment_values(bias0, "best_guess_multiplier", spatial)
    mult1, shift1 = segment_values(bias1, "best_guess_multiplier", hidden)
    mult2, shift2 = segment_values(bias2, "best_guess_multiplier", channels)
    residual_mult, residual_shift = residual_pair_for_fnb(bias2, channels)
    header = i32_pack([
        single_shift(down_shift, f"pass {down_pass} downsample"),
        single_shift(shift0, f"pass {fnb_pass} spatial"),
        single_shift(shift1, f"pass {fnb_pass} expand"),
        single_shift(shift2, f"pass {fnb_pass} contract"),
    ])
    expected_bytes = 10000 if channels == 32 else 36880
    return param_blob([
        header,
        read_weight(bundle_dir, page_for(pages, down_pass, "weights"), down_weight_bytes),
        i32_pack(bias_values(down_bias, channels)),
        i32_pack(down_mult),
        read_weight(bundle_dir, page_for(pages, fnb_pass, "weights0"), spatial_weight_bytes),
        i32_pack(bias_values(bias0, spatial)),
        i32_pack(mult0),
        read_weight(bundle_dir, page_for(pages, fnb_pass, "weights1"), expand_weight_bytes),
        i32_pack(bias_values(bias1, hidden)),
        i32_pack(mult1),
        read_weight(bundle_dir, page_for(pages, fnb_pass, "weights2"), contract_weight_bytes),
        i32_pack(bias_values(bias2, channels)),
        i32_pack(mult2),
        i32_pack(residual_mult),
        i32_pack(residual_shift),
        i32_pack(zero_i32(channels)),
    ], expected_bytes, f"passes {down_pass}/{fnb_pass} down+FNB{channels}")


def residual_pair_for_fnb(bias_page: dict[str, Any], channels: int) -> tuple[list[int], list[int]]:
    segments = bias_page["bias_epilogue_contract"]["best_guess"]["segments"]
    if len(segments) != 1:
        return segment_values(bias_page, "best_guess_residual_multiplier", channels)
    segment = segments[0]
    if "best_guess_residual_multiplier" in segment:
        return segment_values(bias_page, "best_guess_residual_multiplier", channels)
    values = [0 for _ in range(channels)]
    shifts = [0 for _ in range(channels)]
    half = channels // 2
    first = segment["best_guess_residual_first_half_multiplier"]
    second = segment["best_guess_residual_second_half_multiplier"]
    for channel in range(0, half):
        values[channel] = int(first["multiplier_i32"])
        shifts[channel] = int(first["right_shift"])
    for channel in range(half, channels):
        values[channel] = int(second["multiplier_i32"])
        shifts[channel] = int(second["right_shift"])
    return values, shifts


def fnb(bundle_dir: Path, pages: dict[tuple[int, str], dict[str, Any]], pass_index: int, channels: int, expected_bytes: int) -> bytes:
    spatial = channels // 2
    hidden = channels * 2
    spatial_weight_bytes = 9 * (spatial // 8) * 2 * 64
    expand_weight_bytes = (hidden // 8) * (channels // 8) * 64
    contract_weight_bytes = (channels // 8) * (hidden // 8) * 64
    bias0 = page_for(pages, pass_index, "bias0")
    bias1 = page_for(pages, pass_index, "bias1")
    bias2 = page_for(pages, pass_index, "bias2")
    mult0, shift0 = segment_values(bias0, "best_guess_multiplier", spatial)
    mult1, shift1 = segment_values(bias1, "best_guess_multiplier", hidden)
    mult2, shift2 = segment_values(bias2, "best_guess_multiplier", channels)
    residual_mult, residual_shift = residual_pair_for_fnb(bias2, channels)
    header = i32_pack([
        single_shift(shift0, f"pass {pass_index} spatial"),
        single_shift(shift1, f"pass {pass_index} expand"),
        single_shift(shift2, f"pass {pass_index} contract"),
        0,
    ])
    return param_blob([
        header,
        read_weight(bundle_dir, page_for(pages, pass_index, "weights0"), spatial_weight_bytes),
        i32_pack(bias_values(bias0, spatial)),
        i32_pack(mult0),
        read_weight(bundle_dir, page_for(pages, pass_index, "weights1"), expand_weight_bytes),
        i32_pack(bias_values(bias1, hidden)),
        i32_pack(mult1),
        read_weight(bundle_dir, page_for(pages, pass_index, "weights2"), contract_weight_bytes),
        i32_pack(bias_values(bias2, channels)),
        i32_pack(mult2),
        i32_pack(residual_mult),
        i32_pack(residual_shift),
        i32_pack(zero_i32(channels)),
    ], expected_bytes, f"pass {pass_index} FNB{channels}")


def ct2d32_chunks(bundle_dir: Path, pages: dict[tuple[int, str], dict[str, Any]], pass_index: int) -> bytes:
    weights = read_weight(bundle_dir, page_for(pages, pass_index, "ct2d_weights"), 8192)
    bias_page = page_for(pages, pass_index, "ct2d_bias")
    bias = bias_values(bias_page, 32)
    mult, shift = segment_values(bias_page, "best_guess_multiplier", 32)
    skip_mult, skip_shift = segment_values(bias_page, "best_guess_residual_multiplier", 32)
    chunks: list[bytes] = []
    for chunk in range(4):
        start = chunk * 8
        end = start + 8
        header = i32_pack([single_shift(shift[start:end], f"pass {pass_index} ct2d chunk {chunk}"), 0, 0, 0])
        chunks.append(param_blob([
            header,
            weights[chunk * 2048 : (chunk + 1) * 2048],
            i32_pack(bias[start:end]),
            i32_pack(mult[start:end]),
            i32_pack(skip_mult[start:end]),
            i32_pack(skip_shift[start:end]),
            i32_pack(zero_i32(8)),
        ], 2224, f"pass {pass_index} CT2D32 chunk {chunk}"))
    return b"".join(chunks)


def fnb_ct2d16(bundle_dir: Path, pages: dict[tuple[int, str], dict[str, Any]], pass_index: int) -> bytes:
    fnb_part = fnb(bundle_dir, pages, pass_index, 32, 7696)
    ct2d_weight = read_weight(bundle_dir, page_for(pages, pass_index, "ct2d_weights"), 2048)
    ct2d_bias_page = page_for(pages, pass_index, "ct2d_bias")
    ct2d_mult, ct2d_shift = segment_values(ct2d_bias_page, "best_guess_multiplier", 16)
    skip_mult, skip_shift = segment_values(ct2d_bias_page, "best_guess_residual_multiplier", 16)
    header = bytearray(fnb_part[:16])
    struct.pack_into("<i", header, 12, single_shift(ct2d_shift, f"pass {pass_index} ct2d16"))
    return param_blob([
        bytes(header),
        fnb_part[16:],
        ct2d_weight,
        i32_pack(bias_values(ct2d_bias_page, 16)),
        i32_pack(ct2d_mult),
        i32_pack(skip_mult),
        i32_pack(skip_shift),
        i32_pack(zero_i32(16)),
    ], 10064, f"pass {pass_index} FNB32+CT2D16")


def build_arena(package: dict[str, Any]) -> tuple[bytes, list[dict[str, Any]]]:
    pages = page_map(package)
    bundle_dir = package_bundle_dir(package)
    arena = bytearray(FULL_ARENA_BYTES)
    stage_payloads = {
        "stage1_convnext16": convnext16(bundle_dir, pages, 1),
        "stage2_convnext16": convnext16(bundle_dir, pages, 2),
        "stage3_4_down_fnb32": down_fnb(bundle_dir, pages, 3, 4, 32),
        "stage5_fnb32_c8group": fnb(bundle_dir, pages, 5, 32, 7696),
        "stage6_7_down_fnb64": down_fnb(bundle_dir, pages, 6, 7, 64),
        "stage8_fnb64": fnb(bundle_dir, pages, 8, 64, 28176),
        "stage9_ct2d32_chunks": ct2d32_chunks(bundle_dir, pages, 9),
        "stage10_fnb32_c8group": fnb(bundle_dir, pages, 10, 32, 7696),
        "stage11_fnb32_ct2d16": fnb_ct2d16(bundle_dir, pages, 11),
        "stage12_convnext16": convnext16(bundle_dir, pages, 12),
    }
    records: list[dict[str, Any]] = []
    for name, payload in stage_payloads.items():
        offset, slice_bytes = STAGE_SLICES[name]
        if len(payload) > slice_bytes:
            raise ValueError(f"{name} payload {len(payload)} exceeds slice {slice_bytes}")
        arena[offset : offset + len(payload)] = payload
        records.append({
            "name": name,
            "offset_bytes": offset,
            "payload_bytes": len(payload),
            "slice_bytes": slice_bytes,
            "padding_bytes": slice_bytes - len(payload),
            "sha256": sha256_bytes(payload),
            "resident_epilogue_modes": analyze_residual_epilogues(name, payload),
        })
    if any(arena[FULL_ARENA_PAYLOAD_BYTES:]):
        raise ValueError("arena padding after payload boundary is not zero")
    return bytes(arena), records


def relpath(path: Path) -> str:
    return str(path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path)


def infer_model_permutation(package_path: Path, package_root: Path) -> str:
    try:
        return package_path.relative_to(package_root).parts[0]
    except (ValueError, IndexError):
        return package_path.parent.name


def infer_parameter_profile(package_path: Path) -> str:
    return package_path.stem


def default_output_path(package_path: Path, package_root: Path, output_root: Path) -> Path:
    permutation = infer_model_permutation(package_path, package_root)
    profile = infer_parameter_profile(package_path)
    return output_root / permutation / profile / DEFAULT_ARENA_NAME


def package_paths(package_root: Path, profile: str) -> list[Path]:
    return sorted(path for path in package_root.glob(f"*/{profile}.json") if path.is_file())


def write_arena(package_path: Path, output_path: Path, manifest_path: Path, package_root: Path) -> dict[str, Any]:
    package = load_json(package_path)
    arena, records = build_arena(package)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(arena)
    manifest = {
        "schema": "com.apusr.middle_core_resident_parameter_arena.v1",
        "source_package": relpath(package_path),
        "model_permutation": infer_model_permutation(package_path, package_root),
        "parameter_profile": infer_parameter_profile(package_path),
        "arena_bytes": len(arena),
        "payload_bytes": FULL_ARENA_PAYLOAD_BYTES,
        "alignment_bytes": ALIGNMENT_BYTES,
        "sha256": sha256_bytes(arena),
        "stage_slices": records,
        "numeric_status": "best_guess_epilogue_records_seeded_from_npu_parameter_package",
    }
    write_json(manifest_path, manifest)
    print(f"generated {output_path} ({len(arena)} bytes)")
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, help="single NPU parameter package to lower")
    parser.add_argument("--output", type=Path, help="single output arena path")
    parser.add_argument("--manifest", type=Path, help="single output manifest path")
    parser.add_argument("--all", action="store_true", help="lower every product package for the selected parameter profile")
    parser.add_argument("--package-root", type=Path, default=DEFAULT_PACKAGE_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--profile", default=DEFAULT_PARAMETER_PROFILE)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    package_root = args.package_root
    output_root = args.output_root

    if args.all or args.package is None:
        packages = package_paths(package_root, args.profile)
        if not packages:
            raise FileNotFoundError(f"no NPU parameter packages found under {package_root} for profile {args.profile}")
        manifests = []
        for package_path in packages:
            output_path = default_output_path(package_path, package_root, output_root)
            manifests.append(write_arena(package_path, output_path, output_path.with_suffix(".json"), package_root))
        summary_path = output_root / f"stage1_12_full_arena__{args.profile}.json"
        write_json(summary_path, {
            "schema": "com.apusr.middle_core_resident_parameter_arena_set.v1",
            "package_root": relpath(package_root),
            "output_root": relpath(output_root),
            "parameter_profile": args.profile,
            "arena_count": len(manifests),
            "arenas": [
                {
                    "model_permutation": manifest["model_permutation"],
                    "parameter_profile": manifest["parameter_profile"],
                    "source_package": manifest["source_package"],
                    "sha256": manifest["sha256"],
                    "arena_bytes": manifest["arena_bytes"],
                    "payload_bytes": manifest["payload_bytes"],
                }
                for manifest in manifests
            ],
        })
        print(f"generated {len(manifests)} resident parameter arenas")
        return 0

    output_path = args.output or default_output_path(args.package, package_root, output_root)
    manifest_path = args.manifest or output_path.with_suffix(".json")
    write_arena(args.package, output_path, manifest_path, package_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
