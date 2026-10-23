#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate the checked-in universal resident parameter arena set."""

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

DEFAULT_MANIFEST = REPO_ROOT / "models/fsr4_int8/resident_params/stage1_12_full_arena__am020_4x4_base.json"
EXPECTED_PERMUTATIONS = {
    f"{preset}__{height}"
    for preset in ["balanced", "drs", "native", "performance", "quality", "ultraperf"]
    for height in ["1080", "2160", "4320"]
}
EXPECTED_SCHEMA = "com.apusr.middle_core_resident_parameter_arena_set.v1"
EXPECTED_ARENA_BYTES = 122_880
EXPECTED_PAYLOAD_BYTES = 122_160
ARENA_NAME = "stage1_12_full_arena.bin"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def repo_path(path_text: str) -> Path:
    path = Path(path_text)
    return path if path.is_absolute() else REPO_ROOT / path


def validate_manifest(manifest_path: Path) -> list[str]:
    errors: list[str] = []
    manifest = load_json(manifest_path)
    if manifest.get("schema") != EXPECTED_SCHEMA:
        errors.append(f"bad resident parameter set schema: {manifest.get('schema')}")
    if int(manifest.get("arena_count", -1)) != len(EXPECTED_PERMUTATIONS):
        errors.append(f"resident parameter set must contain {len(EXPECTED_PERMUTATIONS)} arenas")

    output_root = repo_path(str(manifest.get("output_root", "models/fsr4_int8/resident_params")))
    arenas = manifest.get("arenas") or []
    seen: set[str] = set()
    for arena in arenas:
        permutation = str(arena.get("model_permutation"))
        profile = str(arena.get("parameter_profile"))
        key = f"{permutation}/{profile}"
        if key in seen:
            errors.append(f"duplicate resident arena entry: {key}")
            continue
        seen.add(key)
        if permutation not in EXPECTED_PERMUTATIONS:
            errors.append(f"unexpected resident arena permutation: {permutation}")
        if profile != manifest.get("parameter_profile"):
            errors.append(f"resident arena {permutation} has wrong profile {profile}")
        arena_path = output_root / permutation / profile / ARENA_NAME
        arena_manifest_path = arena_path.with_suffix(".json")
        if not arena_path.is_file():
            errors.append(f"missing resident arena binary: {arena_path}")
            continue
        arena_bytes = arena_path.stat().st_size
        if arena_bytes != EXPECTED_ARENA_BYTES:
            errors.append(f"{arena_path}: expected {EXPECTED_ARENA_BYTES} bytes, got {arena_bytes}")
        digest = sha256_file(arena_path)
        if digest != arena.get("sha256"):
            errors.append(f"{arena_path}: sha256 mismatch {digest} != {arena.get('sha256')}")
        if int(arena.get("arena_bytes", -1)) != EXPECTED_ARENA_BYTES:
            errors.append(f"{key}: manifest arena_bytes changed")
        if int(arena.get("payload_bytes", -1)) != EXPECTED_PAYLOAD_BYTES:
            errors.append(f"{key}: manifest payload_bytes changed")
        if arena_manifest_path.is_file():
            arena_manifest = load_json(arena_manifest_path)
            if arena_manifest.get("sha256") != digest:
                errors.append(f"{arena_manifest_path}: per-arena sha256 mismatch")
            if arena_manifest.get("model_permutation") != permutation:
                errors.append(f"{arena_manifest_path}: model permutation mismatch")
            if arena_manifest.get("parameter_profile") != profile:
                errors.append(f"{arena_manifest_path}: parameter profile mismatch")
            arena_data = arena_path.read_bytes()
            for item in arena_manifest.get("stage_slices") or []:
                name = str(item.get("name"))
                offset = int(item.get("offset_bytes", -1))
                payload_bytes = int(item.get("payload_bytes", -1))
                if offset < 0 or payload_bytes < 0 or offset + payload_bytes > len(arena_data):
                    errors.append(f"{arena_manifest_path}: invalid stage slice bounds for {name}")
                    continue
                arena_payload = arena_data[offset : offset + payload_bytes]
                expected_epilogues = analyze_residual_epilogues(name, arena_payload)
                if item.get("resident_epilogue_modes") != expected_epilogues:
                    errors.append(f"{arena_manifest_path}: resident epilogue metadata mismatch for {name}")
                for error in validate_residual_epilogues(name, arena_payload):
                    errors.append(f"{arena_manifest_path}: {error}")
        else:
            errors.append(f"missing resident arena manifest: {arena_manifest_path}")

    missing = EXPECTED_PERMUTATIONS - {str(arena.get("model_permutation")) for arena in arenas}
    if missing:
        errors.append(f"missing resident arena permutations: {sorted(missing)}")
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", nargs="?", type=Path, default=DEFAULT_MANIFEST)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    errors = validate_manifest(args.manifest)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"validated resident parameter arena set: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
