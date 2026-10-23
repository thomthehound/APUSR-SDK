# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Inspect APUSR's imported FSR4 INT8 scratch layout and tiling contract.

This tool is intentionally independent of the runtime so scratch-carrier changes
can be checked before they are promoted into C++ resource planning or shader
replacement.  It parses the generated ML2Code HLSL tensor descriptors, compares
those extents to APUSR's declared scratch capacities, and can emit a machine-
readable scratch ABI contract for the current dense Stage0 layout and proposed
padded Stage0 layouts.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path

PRESETS = ("native", "quality", "balanced", "performance", "drs", "ultraperf")
CAPACITY_DISPLAY = {
    "1080": (1920, 1080),
    "2160": (3840, 2160),
    "4320": (7680, 4320),
}
DECLARED_SCRATCH_BYTES = {
    "1080": 20_880_256,
    "2160": 83_232_256,
    "4320": 332_352_256,
}
COMMON_WEIRD_DISPLAY_SUITE = (
    (1280, 1024),   # 5:4 legacy LCD
    (1600, 1200),   # 4:3/UXGA
    (1920, 1200),   # 16:10
    (2560, 1080),   # 21:9
    (3440, 1440),   # 21:9, current common ultrawide class
    (3840, 1600),   # 24:10 ultrawide
    (5120, 1440),   # 32:9 super-ultrawide
    (5760, 1080),   # 3x1080p surround
    (7680, 1440),   # 3x1440p surround / very wide desktop
    (1080, 1920),   # portrait 1080p
    (1440, 2560),   # portrait 1440p
    (2160, 3840),   # portrait 4K
    (1921, 1079),   # odd edge/projection case
    (3839, 2159),   # just under 2160 capacity
    (7680, 4320),   # max imported capacity
)
PASS1_TILE_W = 64
PASS1_TILE_H = 2
PASS1_HALO = 1
C16_CHANNELS = 16
C16_BYTES = C16_CHANNELS
C16_DWORDS = 4
PASS1_LOAD_W = PASS1_TILE_W + 2 * PASS1_HALO
PASS1_LOAD_H = PASS1_TILE_H + 2 * PASS1_HALO
PASS1_LDS_BYTES = PASS1_LOAD_W * PASS1_LOAD_H * C16_BYTES
SCRATCH_LAYOUT_VARIANTS = ("dense-current", "padded-stage0", "all")


SLICE_RE = re.compile(
    r"const uint3 storageSize_slice_(?P<slice>\d+) = uint3\((?P<sx>\d+), (?P<sy>\d+), (?P<sc>\d+)\);\s*"
    r"const uint3 tensorByteStrides_slice_\1 = uint3\((?P<bx>\d+), (?P<by>\d+), (?P<bc>\d+)\);.*?"
    r"threadGroupByteOffsetInTensor_slice_\1 \+ (?P<offset>\d+)",
    re.S,
)


@dataclass(frozen=True)
class TensorSlice:
    slice_index: int
    offset: int
    storage_size: tuple[int, int, int]
    byte_strides: tuple[int, int, int]
    end_exclusive: int


@dataclass(frozen=True)
class CapacityReport:
    capacity: str
    capacity_display: tuple[int, int]
    c16_size: tuple[int, int]
    c16_frame_bytes: int
    generated_max_scratch_end: int
    declared_scratch_bytes: int
    declared_reserve_bytes: int
    padded_stage0_size: tuple[int, int]
    padded_stage0_bytes: int
    padded_stage0_extra_bytes: int
    padded_pass1_output_offset: int
    padded_max_scratch_end_if_stage1_shifted: int
    padded_reserve_bytes_if_stage1_shifted: int
    pass1_capacity_groups_x: int
    pass1_capacity_groups_y: int
    pass1_capacity_groups_x_is_even: bool
    pass1_capacity_width_is_integral: bool
    pass1_lds_bytes: int
    pass1_load4_per_group: int
    pass1_dp4a_per_output_estimate: int
    pass1_dp4a_per_group_estimate: int


@dataclass(frozen=True)
class DisplayReport:
    display: tuple[int, int]
    aligned_display: tuple[int, int]
    capacity: str
    c16_size: tuple[int, int]
    groups_x: int
    groups_y: int
    last_group_valid_pixels: int
    last_group_padding_pixels: int
    width_is_integral_64_tile: bool
    groups_x_is_even: bool


@dataclass(frozen=True)
class MiddleStageReport:
    display: tuple[int, int]
    aligned_display: tuple[int, int]
    capacity: str
    logical_stage: int
    tensor_size: tuple[int, int]
    downscale_shift: int
    tile_size: tuple[int, int]
    groups: tuple[int, int]
    last_group_valid: tuple[int, int]
    last_group_padding: tuple[int, int]
    width_is_integral_tile: bool
    height_is_integral_tile: bool
    groups_x_is_even: bool


@dataclass(frozen=True)
class ScratchRegionReport:
    role: str
    owner: str
    name: str
    offset_bytes: int
    byte_size: int
    end_bytes: int
    alignment_bytes: int
    producer_writes: bool
    consumer_reads: bool
    may_alias_after_barrier: bool


@dataclass(frozen=True)
class ScratchContractReport:
    variant: str
    capacity: str
    capacity_display: tuple[int, int]
    stage0_storage_size: tuple[int, int]
    stage0_channels: int
    stage0_logical_origin: tuple[int, int]
    stage0_row_stride_bytes: int
    stage1_output_offset_bytes: int
    generated_max_scratch_end_bytes: int
    max_used_scratch_bytes: int
    declared_scratch_bytes: int
    reserve_bytes: int
    extra_bytes_vs_dense_current: int
    fits_declared_scratch: bool
    regions_are_contiguous: bool
    regions_are_non_overlapping: bool
    stage0_origin_has_one_pixel_halo: bool
    regions: list[ScratchRegionReport]


def round_up_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def align_up(value: int, alignment: int) -> int:
    return round_up_div(value, alignment) * alignment


def capacity_for_display(width: int, height: int) -> str:
    for capacity, (cw, ch) in CAPACITY_DISPLAY.items():
        if width <= cw and height <= ch:
            return capacity
    raise SystemExit(f"display {width}x{height} exceeds imported FSR4 4320 capacity")


def parse_slices(path: Path) -> list[TensorSlice]:
    text = path.read_text(encoding="utf-8", errors="replace")
    slices: list[TensorSlice] = []
    for match in SLICE_RE.finditer(text):
        sx, sy, sc = int(match["sx"]), int(match["sy"]), int(match["sc"])
        bx, by, bc = int(match["bx"]), int(match["by"]), int(match["bc"])
        offset = int(match["offset"])
        end_exclusive = offset + (sx - 1) * bx + (sy - 1) * by + (sc - 1) * bc + 1
        slices.append(TensorSlice(
            slice_index=int(match["slice"]),
            offset=offset,
            storage_size=(sx, sy, sc),
            byte_strides=(bx, by, bc),
            end_exclusive=end_exclusive,
        ))
    return slices


def capacity_report(repo_root: Path, preset: str, capacity: str) -> CapacityReport:
    width, height = CAPACITY_DISPLAY[capacity]
    c16_width = width // 2
    c16_height = height // 2
    c16_frame_bytes = c16_width * c16_height * C16_BYTES
    path = repo_root / "models/fsr4_int8/reference/fsr4_4_0_2_int8" / preset / f"passes_{capacity}.hlsl"
    slices = parse_slices(path)
    if not slices:
        raise SystemExit(f"no generated scratch tensor slices parsed from {path}")
    generated_max = max(s.end_exclusive for s in slices)
    declared = DECLARED_SCRATCH_BYTES[capacity]
    padded_width = c16_width + 2
    padded_height = c16_height + 2
    padded_stage0_bytes = padded_width * padded_height * C16_BYTES
    padded_extra = padded_stage0_bytes - c16_frame_bytes
    shifted_max = generated_max + padded_extra
    capacity_groups_x = c16_width // PASS1_TILE_W
    return CapacityReport(
        capacity=capacity,
        capacity_display=(width, height),
        c16_size=(c16_width, c16_height),
        c16_frame_bytes=c16_frame_bytes,
        generated_max_scratch_end=generated_max,
        declared_scratch_bytes=declared,
        declared_reserve_bytes=declared - generated_max,
        padded_stage0_size=(padded_width, padded_height),
        padded_stage0_bytes=padded_stage0_bytes,
        padded_stage0_extra_bytes=padded_extra,
        padded_pass1_output_offset=padded_stage0_bytes,
        padded_max_scratch_end_if_stage1_shifted=shifted_max,
        padded_reserve_bytes_if_stage1_shifted=declared - shifted_max,
        pass1_capacity_groups_x=capacity_groups_x,
        pass1_capacity_groups_y=round_up_div(c16_height, PASS1_TILE_H),
        pass1_capacity_groups_x_is_even=(capacity_groups_x % 2) == 0,
        pass1_capacity_width_is_integral=(c16_width % PASS1_TILE_W) == 0,
        pass1_lds_bytes=PASS1_LDS_BYTES,
        pass1_load4_per_group=PASS1_LOAD_W * PASS1_LOAD_H,
        pass1_dp4a_per_output_estimate=832,
        pass1_dp4a_per_group_estimate=832 * PASS1_TILE_W * PASS1_TILE_H,
    )


def _dense_stage0_bytes(capacity: str) -> int:
    width, height = CAPACITY_DISPLAY[capacity]
    return (width // 2) * (height // 2) * C16_BYTES


def _padded_stage0_bytes(capacity: str) -> int:
    width, height = CAPACITY_DISPLAY[capacity]
    return ((width // 2) + 2) * ((height // 2) + 2) * C16_BYTES


def scratch_contract_report(repo_root: Path, preset: str, capacity: str, variant: str) -> ScratchContractReport:
    if variant not in ("dense-current", "padded-stage0"):
        raise ValueError(f"unsupported scratch layout variant: {variant}")
    cap = capacity_report(repo_root, preset, capacity)
    dense_bytes = _dense_stage0_bytes(capacity)
    padded = variant == "padded-stage0"
    stage0_bytes = _padded_stage0_bytes(capacity) if padded else dense_bytes
    stage0_storage_size = cap.padded_stage0_size if padded else cap.c16_size
    origin = (1, 1) if padded else (0, 0)
    stage1_offset = stage0_bytes
    extra = stage0_bytes - dense_bytes
    max_used = cap.generated_max_scratch_end + extra
    reserve = cap.declared_scratch_bytes - max_used
    generated_tail_size = max(0, max_used - stage1_offset)
    regions = [
        ScratchRegionReport(
            role="stage0-c16-carrier",
            owner="shared-uma-candidate" if padded else "gpu-only",
            name="stage0_c16_padded_carrier" if padded else "stage0_c16_dense_current",
            offset_bytes=0,
            byte_size=stage0_bytes,
            end_bytes=stage0_bytes,
            alignment_bytes=C16_BYTES,
            producer_writes=True,
            consumer_reads=True,
            may_alias_after_barrier=False,
        ),
        ScratchRegionReport(
            role="generated-stage1-and-tail",
            owner="gpu-only",
            name="generated_stage1_to_post_shifted" if padded else "generated_stage1_to_post_current",
            offset_bytes=stage1_offset,
            byte_size=generated_tail_size,
            end_bytes=stage1_offset + generated_tail_size,
            alignment_bytes=C16_BYTES,
            producer_writes=True,
            consumer_reads=True,
            may_alias_after_barrier=True,
        ),
        ScratchRegionReport(
            role="declared-reserve",
            owner="shared-uma-candidate" if padded else "gpu-only",
            name="declared_reserve_after_padded_shift" if padded else "declared_reserve_current",
            offset_bytes=max_used,
            byte_size=max(0, reserve),
            end_bytes=cap.declared_scratch_bytes if reserve >= 0 else max_used,
            alignment_bytes=C16_BYTES,
            producer_writes=False,
            consumer_reads=False,
            may_alias_after_barrier=False,
        ),
    ]
    contiguous = all(r.offset_bytes == (0 if i == 0 else regions[i - 1].end_bytes) for i, r in enumerate(regions))
    non_overlapping = all(r.offset_bytes >= (0 if i == 0 else regions[i - 1].end_bytes) for i, r in enumerate(regions))
    return ScratchContractReport(
        variant=variant,
        capacity=capacity,
        capacity_display=cap.capacity_display,
        stage0_storage_size=stage0_storage_size,
        stage0_channels=C16_CHANNELS,
        stage0_logical_origin=origin,
        stage0_row_stride_bytes=stage0_storage_size[0] * C16_BYTES,
        stage1_output_offset_bytes=stage1_offset,
        generated_max_scratch_end_bytes=cap.generated_max_scratch_end,
        max_used_scratch_bytes=max_used,
        declared_scratch_bytes=cap.declared_scratch_bytes,
        reserve_bytes=max(0, reserve),
        extra_bytes_vs_dense_current=extra,
        fits_declared_scratch=reserve >= 0,
        regions_are_contiguous=contiguous,
        regions_are_non_overlapping=non_overlapping,
        stage0_origin_has_one_pixel_halo=padded,
        regions=regions,
    )


def display_report(width: int, height: int) -> DisplayReport:
    aw, ah = align_up(width, 8), align_up(height, 8)
    capacity = capacity_for_display(aw, ah)
    c16_width, c16_height = aw // 2, ah // 2
    groups_x = round_up_div(c16_width, PASS1_TILE_W)
    last_valid = c16_width - (groups_x - 1) * PASS1_TILE_W if groups_x else 0
    padding = 0 if last_valid in (0, PASS1_TILE_W) else PASS1_TILE_W - last_valid
    return DisplayReport(
        display=(width, height),
        aligned_display=(aw, ah),
        capacity=capacity,
        c16_size=(c16_width, c16_height),
        groups_x=groups_x,
        groups_y=round_up_div(c16_height, PASS1_TILE_H),
        last_group_valid_pixels=last_valid,
        last_group_padding_pixels=padding,
        width_is_integral_64_tile=(padding == 0),
        groups_x_is_even=(groups_x % 2) == 0,
    )


def middle_stage_shape(logical_stage: int) -> tuple[int, int, int]:
    if logical_stage in (1, 2):
        return 1, 64, 2
    if logical_stage == 12:
        return 1, 64, 1
    if logical_stage in (3, 4, 5, 10, 11):
        return 2, 64, 1
    if logical_stage in (6, 7, 8):
        return 3, 64, 1
    if logical_stage == 9:
        return 3, 8, 8
    raise ValueError(f"logical stage {logical_stage} is not a generated middle pass")


def middle_stage_report(width: int, height: int, logical_stage: int) -> MiddleStageReport:
    aw, ah = align_up(width, 8), align_up(height, 8)
    capacity = capacity_for_display(aw, ah)
    shift, tile_w, tile_h = middle_stage_shape(logical_stage)
    tensor_w, tensor_h = aw >> shift, ah >> shift
    groups_x = round_up_div(tensor_w, tile_w)
    groups_y = round_up_div(tensor_h, tile_h)
    last_valid_x = tensor_w - (groups_x - 1) * tile_w if groups_x else 0
    last_valid_y = tensor_h - (groups_y - 1) * tile_h if groups_y else 0
    pad_x = 0 if last_valid_x in (0, tile_w) else tile_w - last_valid_x
    pad_y = 0 if last_valid_y in (0, tile_h) else tile_h - last_valid_y
    return MiddleStageReport(
        display=(width, height),
        aligned_display=(aw, ah),
        capacity=capacity,
        logical_stage=logical_stage,
        tensor_size=(tensor_w, tensor_h),
        downscale_shift=shift,
        tile_size=(tile_w, tile_h),
        groups=(groups_x, groups_y),
        last_group_valid=(last_valid_x, last_valid_y),
        last_group_padding=(pad_x, pad_y),
        width_is_integral_tile=(pad_x == 0),
        height_is_integral_tile=(pad_y == 0),
        groups_x_is_even=(groups_x % 2) == 0,
    )


def parse_display(value: str) -> tuple[int, int]:
    match = re.fullmatch(r"(\d+)x(\d+)", value.strip().lower())
    if not match:
        raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT, for example 3440x1440")
    return int(match.group(1)), int(match.group(2))


def report_dict(repo_root: Path, preset: str, displays: list[tuple[int, int]], include_middle_stages: bool, layout_variant: str) -> dict[str, object]:
    capacities = [capacity_report(repo_root, preset, c) for c in CAPACITY_DISPLAY]
    display_reports = [display_report(w, h) for w, h in displays]
    middle_stage_reports: list[MiddleStageReport] = []
    if include_middle_stages:
        for w, h in displays:
            for stage in range(1, 13):
                middle_stage_reports.append(middle_stage_report(w, h, stage))
    contract_variants = ("dense-current", "padded-stage0") if layout_variant == "all" else (layout_variant,)
    scratch_contracts = [scratch_contract_report(repo_root, preset, c, v) for c in CAPACITY_DISPLAY for v in contract_variants]
    return {
        "preset_used_for_generated_parse": preset,
        "capacity_reports": [asdict(r) for r in capacities],
        "display_reports": [asdict(r) for r in display_reports],
        "middle_stage_reports": [asdict(r) for r in middle_stage_reports],
        "scratch_layout_contracts": [asdict(r) for r in scratch_contracts],
        "notes": [
            "Generated middle-pass scratch layout is preset-invariant for a given capacity; the preset only supplies the parsed source file.",
            "A padded Stage0 C16 carrier can fit inside the existing declared scratch sizes if Stage1 output is shifted by the padded Stage0 overhead.",
            "Capacity-sized tensors are cleanly tiled for the front C16 64x2 passes, but arbitrary display widths can still produce a partial final strip.",
            "Odd, portrait, super-ultrawide, and surround displays are handled by explicit aligned-display dispatch and per-stage final-tile padding; do not assume group counts are even.",
            "Scratch layout variants are ABI proposals: every producer, consumer, descriptor, and future NPU island must agree on the same region offsets and ownership rules before enabling a variant.",
        ],
    }


def print_markdown(data: dict[str, object]) -> None:
    print("# FSR4 INT8 scratch/front-C16 layout report\n")
    print("| capacity | C16 plane | dense bytes | generated max | declared scratch | reserve | padded Stage0 bytes | extra | reserve after shift | pass1 groups X | X even |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in data["capacity_reports"]:  # type: ignore[index]
        print(
            f"| {r['capacity']} | {r['c16_size'][0]}x{r['c16_size'][1]} | "
            f"{r['c16_frame_bytes']} | {r['generated_max_scratch_end']} | "
            f"{r['declared_scratch_bytes']} | {r['declared_reserve_bytes']} | "
            f"{r['padded_stage0_bytes']} | {r['padded_stage0_extra_bytes']} | "
            f"{r['padded_reserve_bytes_if_stage1_shifted']} | {r['pass1_capacity_groups_x']} | "
            f"{str(r['pass1_capacity_groups_x_is_even']).lower()} |"
        )
    if data["display_reports"]:  # type: ignore[index]
        print("\n| display | aligned | capacity | C16 plane | pass1 groups | last group valid | pad pixels | integral 64-wide | groups X even |")
        print("|---|---:|---|---:|---:|---:|---:|---:|---:|")
        for r in data["display_reports"]:  # type: ignore[index]
            print(
                f"| {r['display'][0]}x{r['display'][1]} | {r['aligned_display'][0]}x{r['aligned_display'][1]} | "
                f"{r['capacity']} | {r['c16_size'][0]}x{r['c16_size'][1]} | "
                f"{r['groups_x']}x{r['groups_y']} | {r['last_group_valid_pixels']} | "
                f"{r['last_group_padding_pixels']} | {str(r['width_is_integral_64_tile']).lower()} | "
                f"{str(r['groups_x_is_even']).lower()} |"
            )
    if data.get("middle_stage_reports"):
        print("\n| display | stage | tensor | tile | groups | last valid | pad | whole X | whole Y | groups X even |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in data["middle_stage_reports"]:  # type: ignore[index]
            print(
                f"| {r['display'][0]}x{r['display'][1]} | {r['logical_stage']} | "
                f"{r['tensor_size'][0]}x{r['tensor_size'][1]} | {r['tile_size'][0]}x{r['tile_size'][1]} | "
                f"{r['groups'][0]}x{r['groups'][1]} | {r['last_group_valid'][0]}x{r['last_group_valid'][1]} | "
                f"{r['last_group_padding'][0]}x{r['last_group_padding'][1]} | "
                f"{str(r['width_is_integral_tile']).lower()} | {str(r['height_is_integral_tile']).lower()} | "
                f"{str(r['groups_x_is_even']).lower()} |"
            )
    if data.get("scratch_layout_contracts"):
        print("\n| layout | capacity | stage0 storage | origin | row stride | stage1 offset | max used | declared | reserve | fits | contiguous | non-overlap |")
        print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in data["scratch_layout_contracts"]:  # type: ignore[index]
            print(
                f"| {r['variant']} | {r['capacity']} | {r['stage0_storage_size'][0]}x{r['stage0_storage_size'][1]} | "
                f"{r['stage0_logical_origin'][0]},{r['stage0_logical_origin'][1]} | "
                f"{r['stage0_row_stride_bytes']} | {r['stage1_output_offset_bytes']} | "
                f"{r['max_used_scratch_bytes']} | {r['declared_scratch_bytes']} | {r['reserve_bytes']} | "
                f"{str(r['fits_declared_scratch']).lower()} | {str(r['regions_are_contiguous']).lower()} | "
                f"{str(r['regions_are_non_overlapping']).lower()} |"
            )

    print("\nFront-C16 pass1/pass2 tiled shader constants:")
    print(f"- output tile: {PASS1_TILE_W}x{PASS1_TILE_H}x16")
    print(f"- halo preload: {PASS1_LOAD_W}x{PASS1_LOAD_H}x16 = {PASS1_LDS_BYTES} bytes LDS")
    print(f"- global preload: {PASS1_LOAD_W * PASS1_LOAD_H} Load4 C16-pixel reads per group")
    print("- DP4A estimate: 832 dot4 operations per output pixel, 106,496 per 64x2 group")


def run_checks(data: dict[str, object]) -> None:
    errors: list[str] = []
    for r in data["capacity_reports"]:  # type: ignore[index]
        if r["generated_max_scratch_end"] > r["declared_scratch_bytes"]:
            errors.append(f"{r['capacity']}: generated scratch exceeds declared scratch")
        if r["padded_max_scratch_end_if_stage1_shifted"] > r["declared_scratch_bytes"]:
            errors.append(f"{r['capacity']}: padded Stage0 + shifted Stage1 does not fit declared scratch")
        if not r["pass1_capacity_width_is_integral"]:
            errors.append(f"{r['capacity']}: capacity C16 width is not an integral 64-wide pass1 strip")
        if r["pass1_lds_bytes"] != 4224:
            errors.append(f"{r['capacity']}: unexpected pass1/pass2 LDS footprint")
    for r in data.get("scratch_layout_contracts", []):
        if not r["fits_declared_scratch"]:
            errors.append(f"{r['capacity']} {r['variant']}: scratch layout does not fit declared scratch")
        if not r["regions_are_contiguous"]:
            errors.append(f"{r['capacity']} {r['variant']}: scratch regions are not contiguous")
        if not r["regions_are_non_overlapping"]:
            errors.append(f"{r['capacity']} {r['variant']}: scratch regions overlap")
        previous_end = 0
        for region in r["regions"]:
            if region["offset_bytes"] % region["alignment_bytes"] != 0:
                errors.append(f"{r['capacity']} {r['variant']} {region['name']}: offset is not aligned")
            if region["byte_size"] % region["alignment_bytes"] != 0:
                errors.append(f"{r['capacity']} {r['variant']} {region['name']}: size is not aligned")
            if region["offset_bytes"] != previous_end:
                errors.append(f"{r['capacity']} {r['variant']} {region['name']}: region gap before offset")
            previous_end = region["end_bytes"]
        if previous_end != r["declared_scratch_bytes"]:
            errors.append(f"{r['capacity']} {r['variant']}: regions do not cover declared scratch")
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        raise SystemExit(1)


def main() -> None:
    default_repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=default_repo)
    parser.add_argument("--preset", choices=PRESETS, default="balanced")
    parser.add_argument("--display", action="append", type=parse_display, default=[], help="Optional actual display size to audit, e.g. 3440x1440. Can be repeated.")
    parser.add_argument("--common-weird-displays", action="store_true", help="Audit a built-in suite of common unusual aspect ratios: ultrawide, surround, portrait, and odd sizes.")
    parser.add_argument("--middle-stages", action="store_true", help="Include per-stage middle-pass tensor/tile dispatch reports for each display.")
    parser.add_argument("--layout-variant", choices=SCRATCH_LAYOUT_VARIANTS, default="padded-stage0", help="Scratch ABI variant to include in the report. Use 'all' to compare the current dense layout with the padded Stage0 candidate.")
    parser.add_argument("--write-contract", type=Path, help="Write the JSON report to this path in addition to stdout/markdown. Intended for checked-in or CI scratch ABI snapshots.")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of the default markdown table.")
    parser.add_argument("--check", action="store_true", help="Fail if generated or candidate padded Stage0 layout exceeds declared scratch or capacity tiles are not integral.")
    args = parser.parse_args()

    displays = list(args.display)
    if args.common_weird_displays:
        for item in COMMON_WEIRD_DISPLAY_SUITE:
            if item not in displays:
                displays.append(item)

    data = report_dict(args.repo_root.resolve(), args.preset, displays, args.middle_stages, args.layout_variant)
    if args.check:
        run_checks(data)
    if args.write_contract:
        args.write_contract.parent.mkdir(parents=True, exist_ok=True)
        args.write_contract.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        print_markdown(data)


if __name__ == "__main__":
    main()
