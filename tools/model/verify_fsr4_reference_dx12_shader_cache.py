# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Verify the packaged FSR4 INT8 DX12 reference shader-cache matrix.

This is a packaging-time guard, not a runtime fallback.  Product builds should
fail here when a declared preset/capacity/permutation/dispatch entry is missing
instead of discovering the hole after a game selects that mode.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path


PRESETS = ("native", "quality", "balanced", "performance", "drs", "ultraperf")
CAPACITIES = ("1080", "2160", "4320")
COLOR_SPACES = ("linear", "nonlinear", "srgb", "pq")
PRESET_VALUE = {"native": 0, "quality": 1, "balanced": 2, "performance": 3, "drs": 4, "ultraperf": 5}
CAPACITY_VALUE = {"1080": 0, "2160": 1, "4320": 2}
DISPATCHES = ("spd_auto_exposure", "fsr4_pre", *(f"fsr4_pass_{i}" for i in range(1, 13)), "fsr4_post", "rcas", "debug_view")
PRODUCT_DISPATCHES = tuple(d for d in DISPATCHES if d != "debug_view")


@dataclass(frozen=True)
class Permutation:
    preset: str
    capacity: str
    wmma: bool
    depth_inverted: bool
    low_res_motion_vectors: bool
    auto_exposure: bool
    jittered_motion_vectors: bool
    color_space: str
    debug_visualize: bool


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-root", type=Path, required=True)
    parser.add_argument("--preset", choices=("all", *PRESETS), default="all")
    parser.add_argument("--capacity", choices=("all", *CAPACITIES), default="all")
    parser.add_argument("--include-wmma", action="store_true", help="Also require WMMA-flavored shader entries. The DP4A INT8 reference flavor is the current product default.")
    parser.add_argument("--include-debug", action="store_true", help="Also require debug-visualize permutation variants for developer packages. This does not imply the optional debug_view dispatch unless --dispatch debug_view is passed explicitly.")
    parser.add_argument("--dispatch", action="append", choices=DISPATCHES, help="Restrict verification to one or more dispatches. Defaults to product dispatches, plus debug_view only with --include-debug.")
    parser.add_argument("--write-manifest", type=Path, default=None)
    return parser.parse_args()


def permutation_key(p: Permutation) -> int:
    key = 0
    if p.debug_visualize:
        key |= 1
    key <<= 1
    if p.color_space == "pq":
        key |= 1
    key <<= 1
    if p.color_space == "srgb":
        key |= 1
    key <<= 1
    if p.color_space != "linear":
        key |= 1
    key <<= 1
    if p.jittered_motion_vectors:
        key |= 1
    key <<= 1
    if p.auto_exposure:
        key |= 1
    key <<= 1
    if p.low_res_motion_vectors:
        key |= 1
    key <<= 1
    if p.depth_inverted:
        key |= 1
    key <<= 1
    if p.wmma:
        key |= 1
    key <<= 2
    key |= CAPACITY_VALUE[p.capacity]
    key <<= 3
    key |= PRESET_VALUE[p.preset]
    return key


def enumerate_permutations(args: argparse.Namespace) -> list[Permutation]:
    presets = PRESETS if args.preset == "all" else (args.preset,)
    capacities = CAPACITIES if args.capacity == "all" else (args.capacity,)
    backends = (False, True) if args.include_wmma else (False,)
    debug_values = (False, True) if args.include_debug else (False,)
    out: list[Permutation] = []
    for preset in presets:
        for capacity in capacities:
            for color_space in COLOR_SPACES:
                for wmma in backends:
                    for depth_inverted in (False, True):
                        for low_res_mv in (False, True):
                            for auto_exposure in (False, True):
                                for jittered_mv in (False, True):
                                    for debug in debug_values:
                                        out.append(Permutation(preset, capacity, wmma, depth_inverted, low_res_mv, auto_exposure, jittered_mv, color_space, debug))
    return out


def main() -> int:
    args = parse_args()
    dispatches = tuple(args.dispatch) if args.dispatch else PRODUCT_DISPATCHES
    missing: list[str] = []
    present_count = 0
    entries = []
    for permutation in enumerate_permutations(args):
        key = permutation_key(permutation)
        for dispatch in dispatches:
            if dispatch == "debug_view" and not permutation.debug_visualize:
                # The optional debug-view dispatch belongs only to debug-visualize developer variants.
                continue
            path = args.cache_root / permutation.preset / permutation.capacity / f"p{key}" / f"{dispatch}.dxil"
            exists = path.is_file() and path.stat().st_size > 0
            if exists:
                present_count += 1
            else:
                missing.append(path.as_posix())
            entries.append({
                "preset": permutation.preset,
                "capacity": permutation.capacity,
                "permutationKey": key,
                "dispatch": dispatch,
                "path": path.as_posix(),
                "present": exists,
            })
    manifest = {
        "schema": "apusr.fsr4_reference_dx12.shader_cache_manifest.v1",
        "cacheRoot": args.cache_root.as_posix(),
        "entryCount": len(entries),
        "presentCount": present_count,
        "missingCount": len(missing),
        "entries": entries,
    }
    if args.write_manifest:
        args.write_manifest.parent.mkdir(parents=True, exist_ok=True)
        args.write_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if missing:
        print(f"[fsr4-reference-cache] missing {len(missing)} / {len(entries)} shader cache entries")
        for path in missing[:25]:
            print(f"  missing: {path}")
        if len(missing) > 25:
            print(f"  ... {len(missing) - 25} more")
        return 1
    print(f"[fsr4-reference-cache] verified {present_count} shader cache entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
