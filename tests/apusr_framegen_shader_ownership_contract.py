# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""High-signal contract for APUSR-owned Frame Generation shader infrastructure."""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    raise SystemExit(f"framegen shader ownership contract: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def file_names(path: pathlib.Path) -> set[str]:
    return {p.name for p in path.iterdir() if p.is_file()}


def main() -> int:
    require(
        file_names(ROOT / "tools/shader_compiler/linux/bin")
        == {"dxc", "libdxcompiler.so", "libdxil.so"},
        "Linux active compiler bin must be the matched DXC runtime trio only",
    )
    require(
        file_names(ROOT / "tools/shader_compiler/windows/bin")
        == {"dxc.exe", "dxcompiler.dll", "dxil.dll"},
        "Windows active compiler bin must be the matched DXC runtime trio only",
    )
    require(not (ROOT / "tools/shader_compiler/linux/dependencies").exists(),
            "Linux inactive compiler dependency tree returned")
    require(not (ROOT / "tools/shader_compiler/windows/dependencies").exists(),
            "Windows inactive compiler dependency tree returned")

    generated = ROOT / "src/framegen/algorithm/generated"
    require(
        file_names(generated) == {"apusr_framegen_dxil.h", "apusr_framegen_dxil.cpp"},
        "active DX12 FrameGen generated directory must contain only the APUSR pack",
    )

    pack = (generated / "apusr_framegen_dxil.cpp").read_text(encoding="utf-8")
    hashes = re.findall(r"^// SHA256 ([0-9a-f]{64})$", pack, re.MULTILINE)
    require(len(hashes) == 108 and len(set(hashes)) == 108,
            f"expected 108 unique DXIL blobs, found {len(hashes)}")
    require("Logical permutations: 408; unique DXIL blobs: 108" in pack,
            "generated pack cardinality marker changed")
    require("kFiBlobIndex[11][32]" in pack and "kOfBlobIndex[7][8]" in pack,
            "dense APUSR permutation lookup tables are missing")
    require("compiled directly by DXC from APUSR-maintained Frame Generation shader sources" in pack,
            "corrected direct-DXC pack provenance is missing")

    compile_dx12 = (ROOT / "tools/compile_dx12_shaders.py").read_text(encoding="utf-8")
    require("ffx_sc_cli" not in compile_dx12 and "FidelityFX_SC" not in compile_dx12,
            "bookend DX12 compiler regained a FidelityFX_SC dependency")

    compile_vk = (ROOT / "tools/compile_framegen_vulkan_shaders.py").read_text(encoding="utf-8")
    require("third_party/ffx_sdk" not in compile_vk,
            "Vulkan FrameGen compiler regained a third_party FidelityFX include dependency")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("third_party/ffx_sdk" not in cmake,
            "active CMake build regained a source/include dependency on the FidelityFX reference tree")

    direct = (ROOT / "tools/compile_framegen_dx12_shaders.py").read_text(encoding="utf-8")
    require("FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS" in direct,
            "native DX12 generator must target the jitter macro consumed by the maintained shader")
    require("--import-reference" in direct,
            "explicit reference-baseline import path disappeared")

    for name in ("ffx_core_hlsl.h", "ffx_core_gpu_common.h", "ffx_core_gpu_common_half.h", "ffx_core_portability.h"):
        path = ROOT / "src/framegen/algorithm/include/gpu" / name
        require(path.is_file(), f"maintained GPU support header missing: {name}")
        require("Advanced Micro Devices" in path.read_text(encoding="utf-8", errors="replace"),
                f"AMD provenance missing from maintained GPU support header: {name}")

    require((ROOT / "third_party/ffx_sdk/reference_tools/fidelityfx_sc/README.md").is_file(),
            "FidelityFX_SC historical reference was lost")
    print("FrameGen shader ownership contract: PASS (408 keys, 108 unique DXIL blobs, corrected jitter cancellation, DXC-only active toolchain)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
