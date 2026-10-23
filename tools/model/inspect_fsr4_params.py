#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Inspect APUSR's checked-in FSR4 product parameter artifacts."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BOUNDARY = REPO_ROOT / "models" / "fsr4_int8" / "real_boundary_manifest_v0.json"
DEFAULT_RESIDENT_INDEX = REPO_ROOT / "models" / "fsr4_int8" / "resident_params" / "stage1_12_full_arena__am020_4x4_base.json"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def boundary_summary(boundary_manifest: dict[str, Any]) -> dict[str, Any]:
    contracts = boundary_manifest.get("contracts") or []
    by_mode: dict[str, list[str]] = defaultdict(list)
    ingress_scales: set[str] = set()
    egress_scales: set[str] = set()
    token_bytes = (boundary_manifest.get("boundary") or {}).get("token_bytes")
    for contract in contracts:
        mode = str(contract.get("mode"))
        resolution = str(contract.get("resolution"))
        by_mode[mode].append(resolution)
        ingress_scales.add(str(((contract.get("ingress") or {}).get("consumer") or {}).get("scale_domain")))
        egress_scales.add(str(((contract.get("egress") or {}).get("producer") or {}).get("scale_domain")))
    return {
        "contract_count": len(contracts),
        "token_bytes": token_bytes,
        "modes": {mode: sorted(values, key=int) for mode, values in sorted(by_mode.items())},
        "ingress_scale_domains": sorted(ingress_scales),
        "egress_scale_domains": sorted(egress_scales),
    }


def resident_summary(resident_index: dict[str, Any]) -> dict[str, Any]:
    arenas = resident_index.get("arenas") or []
    by_sha: dict[str, list[str]] = defaultdict(list)
    permutations: list[str] = []
    arena_bytes = resident_index.get("arena_bytes")
    payload_bytes = resident_index.get("payload_bytes")
    if arenas:
        arena_bytes = arena_bytes if arena_bytes is not None else arenas[0].get("arena_bytes")
        payload_bytes = payload_bytes if payload_bytes is not None else arenas[0].get("payload_bytes")
    for arena in arenas:
        permutation = str(arena.get("model_permutation"))
        permutations.append(permutation)
        by_sha[str(arena.get("sha256"))].append(permutation)
    return {
        "schema": resident_index.get("schema"),
        "parameter_profile": resident_index.get("parameter_profile"),
        "arena_count": len(arenas),
        "arena_bytes": arena_bytes,
        "payload_bytes": payload_bytes,
        "permutations": sorted(permutations),
        "unique_arena_payloads": [
            {"sha256": digest, "permutations": sorted(names)} for digest, names in sorted(by_sha.items())
        ],
    }


def stage_slice_summary(index_path: Path, resident_index: dict[str, Any]) -> list[dict[str, Any]]:
    arenas = resident_index.get("arenas") or []
    if not arenas:
        return []
    first = arenas[0]
    arena_manifest_path = index_path.parent / str(first["model_permutation"]) / str(first["parameter_profile"]) / "stage1_12_full_arena.json"
    arena_manifest = load_json(arena_manifest_path)
    return [
        {
            "name": item.get("name"),
            "offset_bytes": item.get("offset_bytes"),
            "payload_bytes": item.get("payload_bytes"),
            "slice_bytes": item.get("slice_bytes"),
            "padding_bytes": item.get("padding_bytes"),
        }
        for item in arena_manifest.get("stage_slices") or []
    ]


def display_path(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def build_report(boundary_path: Path, resident_index_path: Path, *, include_slices: bool) -> dict[str, Any]:
    boundary_manifest = load_json(boundary_path)
    resident_index = load_json(resident_index_path)
    report = {
        "boundary_manifest": display_path(boundary_path),
        "resident_index": display_path(resident_index_path),
        "boundary": boundary_summary(boundary_manifest),
        "resident": resident_summary(resident_index),
    }
    if include_slices:
        report["resident"]["stage_slices"] = stage_slice_summary(resident_index_path, resident_index)
    return report


def print_text_report(report: dict[str, Any]) -> None:
    boundary = report["boundary"]
    resident = report["resident"]
    print("FSR4 product parameter artifacts")
    print(f"  boundary manifest: {report['boundary_manifest']}")
    print(f"  resident index:    {report['resident_index']}")
    print()
    print("Boundary seam")
    print(f"  contracts:      {boundary['contract_count']}")
    print(f"  token bytes:    {boundary['token_bytes']}")
    print(f"  modes:          {', '.join(boundary['modes'].keys())}")
    print(f"  ingress scales: {', '.join(boundary['ingress_scale_domains'])}")
    print(f"  egress scales:  {', '.join(boundary['egress_scale_domains'])}")
    print()
    print("Resident arenas")
    print(f"  schema:         {resident['schema']}")
    print(f"  profile:        {resident['parameter_profile']}")
    print(f"  arenas:         {resident['arena_count']}")
    print(f"  payload bytes:  {resident['payload_bytes']}")
    print(f"  arena bytes:    {resident['arena_bytes']}")
    print(f"  unique payloads:{len(resident['unique_arena_payloads']):>2}")
    for payload in resident["unique_arena_payloads"]:
        digest = str(payload["sha256"])
        names = ", ".join(payload["permutations"])
        print(f"    {digest[:12]}...  {names}")
    if resident.get("stage_slices"):
        print()
        print("Stage slices")
        for item in resident["stage_slices"]:
            print(
                f"  {item['name']:<28} "
                f"offset={item['offset_bytes']:>6} "
                f"payload={item['payload_bytes']:>6} "
                f"slice={item['slice_bytes']:>6} "
                f"pad={item['padding_bytes']:>4}"
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boundary", type=Path, default=DEFAULT_BOUNDARY)
    parser.add_argument("--resident-index", type=Path, default=DEFAULT_RESIDENT_INDEX)
    parser.add_argument("--details", action="store_true", help="include resident stage-slice offsets")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of text")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = build_report(args.boundary, args.resident_index, include_slices=args.details)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_text_report(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
