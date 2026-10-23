#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

"""Import the real FFX FSR4 v07 INT8 shader artifacts into APUSR.

This script is deliberately boring: it copies the preset-specific pre, middle,
post, initializer, and include artifacts from an ffxint8 checkout and rewrites a
small hash manifest.  It does not compile shaders and it does not translate the
model.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

PRESETS = ["quality", "balanced", "performance", "ultraperf", "native", "drs"]
PRESET_FILES = [
    "pre.hlsl",
    "passes_1080.hlsl",
    "passes_2160.hlsl",
    "passes_4320.hlsl",
    "post.hlsl",
    "initializers.bin",
]
COMMON_FILES = ["debug_view.hlsl", "rcas.hlsl", "spd_auto_exposure.hlsl"]
AMD_EXT_HLSL_FILES = [
    "AmdExtD3DShaderIntrinsics.hlsl",
    "AmdExtD3DShaderIntrinsicsAtomicOps.hlsl",
    "AmdExtD3DShaderIntrinsicsMatrixOps.hlsl",
    "AmdExtD3DShaderIntrinsicsMatrixOpsHelper.hlsl",
    "AmdExtD3DShaderIntrinsicsWaveOps.hlsl",
]
GPU_SHARED_HEADERS = [
    "ffx_core.h",
    "ffx_common_types.h",
    "ffx_core_hlsl.h",
    "ffx_core_gpu_common.h",
    "ffx_core_gpu_common_half.h",
    "ffx_core_portability.h",
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_entry(path: Path) -> dict[str, object]:
    return {"bytes": path.stat().st_size, "sha256": sha256_file(path)}


def copy_checked(src: Path, dst: Path) -> dict[str, object]:
    if not src.exists():
        raise FileNotFoundError(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    return file_entry(src)


def copy_tree(src: Path, dst: Path) -> dict[str, object]:
    if not src.exists():
        raise FileNotFoundError(src)
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    entries: dict[str, object] = {}
    for path in sorted(p for p in dst.rglob("*") if p.is_file()):
        rel = path.relative_to(dst).as_posix()
        entries[rel] = file_entry(path)
    return entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffxint8-root", required=True, type=Path)
    parser.add_argument("--repo-root", default=Path.cwd(), type=Path)
    args = parser.parse_args()

    ffx_root = args.ffxint8_root / "Kits/FidelityFX"
    shader_root = ffx_root / "upscalers/fsr4/internal/shaders"
    dst_root = args.repo_root / "models/fsr4_int8/reference/fsr4_4_0_2_int8"
    if not shader_root.exists():
        raise FileNotFoundError(shader_root)

    presets = []
    for preset in PRESETS:
        src_dir = shader_root / f"fsr4_model_v07_i8_{preset}"
        dst_dir = dst_root / preset
        entry = {"preset": preset, "model": f"fsr4_model_v07_i8_{preset}", "files": {}}
        for name in PRESET_FILES:
            entry["files"][name] = copy_checked(src_dir / name, dst_dir / name)
        presets.append(entry)

    common = {}
    common_dir = dst_root / "common"
    for name in COMMON_FILES:
        common[name] = copy_checked(shader_root / name, common_dir / name)

    include_root = dst_root / "include"
    include = {
        "fsr4": copy_tree(ffx_root / "upscalers/fsr4/include/gpu/fsr4", include_root / "fsr4"),
        "fsr1": copy_tree(ffx_root / "upscalers/fsr4/include/gpu/fsr1", include_root / "fsr1"),
        "spd": copy_tree(ffx_root / "upscalers/fsr4/include/gpu/spd", include_root / "spd"),
        "ml2code_runtime": copy_tree(ffx_root / "upscalers/fsr4/dx12/ml2code_runtime", include_root / "ml2code_runtime"),
        "AmdExtD3D": {},
        "gpu_shared": {},
    }
    for name in AMD_EXT_HLSL_FILES:
        include["AmdExtD3D"][name] = copy_checked(
            ffx_root / "api/internal/dx12/AmdExtD3D" / name,
            include_root / "AmdExtD3D" / name,
        )
    for name in GPU_SHARED_HEADERS:
        include["gpu_shared"][name] = copy_checked(
            ffx_root / "api/internal/gpu" / name,
            include_root / name,
        )

    manifest = {
        "schema": "apusr.fsr4_int8_reference_artifacts.v1",
        "source": "ffxint8/Kits/FidelityFX/upscalers/fsr4/internal/shaders",
        "include_source": "ffxint8/Kits/FidelityFX/upscalers/fsr4/include/gpu/fsr4 + dx12/ml2code_runtime + selected HLSL GPU shared headers",
        "model_family": "fsr4_model_v07_i8",
        "presets": presets,
        "common": common,
        "include": include,
    }
    (dst_root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Imported FSR4 INT8 reference artifacts into {dst_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
