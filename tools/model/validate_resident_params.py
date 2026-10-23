#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate APUSR resident FSR4 parameter arenas."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.model.resident_arena_layout import analyze_residual_epilogues, validate_residual_epilogues

DEFAULT_BOUNDARY = REPO_ROOT / "models" / "fsr4_int8" / "real_boundary_manifest_v0.json"
DEFAULT_INDEX = REPO_ROOT / "models" / "fsr4_int8" / "resident_params" / "stage1_12_full_arena__am020_4x4_base.json"
ARENA_BASENAME = "stage1_12_full_arena"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_permutations(boundary_path: Path) -> set[str]:
    boundary = load_json(boundary_path)
    return {str(contract.get("bundle_name")) for contract in boundary.get("contracts") or []}


def validate_stage_slices(arena_manifest_path: Path, arena_bytes: bytes, errors: list[str]) -> None:
    manifest = load_json(arena_manifest_path)
    payload_bytes = int(manifest.get("payload_bytes", -1))
    previous_end = 0
    for item in manifest.get("stage_slices") or []:
        name = str(item.get("name"))
        offset = int(item.get("offset_bytes", -1))
        payload = int(item.get("payload_bytes", -1))
        slice_bytes = int(item.get("slice_bytes", -1))
        padding = int(item.get("padding_bytes", -1))
        if offset < previous_end:
            errors.append(f"{arena_manifest_path}: overlapping stage slice {name}")
        if offset + payload > len(arena_bytes):
            errors.append(f"{arena_manifest_path}: stage slice {name} exceeds arena binary")
            continue
        if payload + padding != slice_bytes:
            errors.append(f"{arena_manifest_path}: stage slice {name} has inconsistent slice/padding sizes")
        slice_payload = arena_bytes[offset : offset + payload]
        digest = sha256_bytes(slice_payload)
        if digest != item.get("sha256"):
            errors.append(f"{arena_manifest_path}: stage slice {name} SHA256 mismatch")
        expected_epilogues = analyze_residual_epilogues(name, slice_payload)
        observed_epilogues = item.get("resident_epilogue_modes")
        if observed_epilogues != expected_epilogues:
            errors.append(f"{arena_manifest_path}: stage slice {name} resident epilogue metadata mismatch")
        for error in validate_residual_epilogues(name, slice_payload):
            errors.append(f"{arena_manifest_path}: {error}")
        previous_end = max(previous_end, offset + slice_bytes)
    if payload_bytes >= 0 and previous_end > payload_bytes:
        errors.append(f"{arena_manifest_path}: stage slices exceed declared payload bytes")


def validate(index_path: Path, boundary_path: Path, *, require_boundary_match: bool) -> list[str]:
    errors: list[str] = []
    index = load_json(index_path)
    root = index_path.parent
    arenas = index.get("arenas") or []
    seen: set[str] = set()

    if require_boundary_match:
        expected = expected_permutations(boundary_path)
        actual = {str(arena.get("model_permutation")) for arena in arenas}
        if actual != expected:
            errors.append(f"resident permutation set does not match boundary manifest: missing={sorted(expected - actual)} extra={sorted(actual - expected)}")

    for arena in arenas:
        permutation = str(arena.get("model_permutation"))
        profile = str(arena.get("parameter_profile"))
        key = f"{permutation}/{profile}"
        if key in seen:
            errors.append(f"duplicate resident arena entry: {key}")
            continue
        seen.add(key)

        arena_path = root / permutation / profile / f"{ARENA_BASENAME}.bin"
        arena_manifest_path = root / permutation / profile / f"{ARENA_BASENAME}.json"
        if not arena_path.is_file():
            errors.append(f"missing resident arena binary: {arena_path}")
            continue
        if not arena_manifest_path.is_file():
            errors.append(f"missing resident arena manifest: {arena_manifest_path}")
            continue

        arena_data = arena_path.read_bytes()
        digest = sha256_bytes(arena_data)
        if digest != arena.get("sha256"):
            errors.append(f"{arena_path}: SHA256 mismatch")
        if int(arena.get("arena_bytes", -1)) != len(arena_data):
            errors.append(f"{arena_path}: index arena_bytes does not match file size")

        arena_manifest = load_json(arena_manifest_path)
        if arena_manifest.get("sha256") != digest:
            errors.append(f"{arena_manifest_path}: manifest SHA256 mismatch")
        if arena_manifest.get("model_permutation") != permutation:
            errors.append(f"{arena_manifest_path}: model_permutation mismatch")
        if arena_manifest.get("parameter_profile") != profile:
            errors.append(f"{arena_manifest_path}: parameter_profile mismatch")
        if int(arena_manifest.get("arena_bytes", -1)) != len(arena_data):
            errors.append(f"{arena_manifest_path}: arena_bytes does not match file size")
        if int(arena_manifest.get("payload_bytes", -1)) != int(arena.get("payload_bytes", -2)):
            errors.append(f"{arena_manifest_path}: payload_bytes differs from index")
        validate_stage_slices(arena_manifest_path, arena_data, errors)

    if int(index.get("arena_count", -1)) != len(arenas):
        errors.append("resident index arena_count does not match arenas[] length")
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--boundary", type=Path, default=DEFAULT_BOUNDARY)
    parser.add_argument("--allow-boundary-mismatch", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors = validate(args.index, args.boundary, require_boundary_match=not args.allow_boundary_mismatch)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"validated resident parameter artifacts: {args.index}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
