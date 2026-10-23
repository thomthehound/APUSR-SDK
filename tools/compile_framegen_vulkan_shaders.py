#!/usr/bin/env python3
"""Compile APUSR Frame Generation FI/OF HLSL permutations to an embedded Vulkan SPIR-V pack."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "src/framegen/algorithm/shaders"
INCLUDE_DIR = ROOT / "src/framegen/algorithm/include"
GPU_INCLUDE_DIR = INCLUDE_DIR / "gpu"

EFFECT_FRAME_INTERPOLATION = 1
EFFECT_OPTICAL_FLOW = 2

FI_LOW_RES = 1 << 0
FI_JITTER = 1 << 1
FI_INVERTED = 1 << 2
FI_ALLOW_FP16 = 1 << 4
OF_ALLOW_FP16 = 1 << 1
OF_HDR = 1 << 2

FI_PASSES = [
    (0, "ffx_frameinterpolation_reconstruct_and_dilate_pass.hlsl"),
    (1, "ffx_frameinterpolation_setup_pass.hlsl"),
    (2, "ffx_frameinterpolation_reconstruct_previous_depth_pass.hlsl"),
    (3, "ffx_frameinterpolation_game_motion_vector_field_pass.hlsl"),
    (4, "ffx_frameinterpolation_optical_flow_vector_field_pass.hlsl"),
    (5, "ffx_frameinterpolation_disocclusion_mask_pass.hlsl"),
    (6, "ffx_frameinterpolation_pass.hlsl"),
    (7, "ffx_frameinterpolation_compute_inpainting_pyramid_pass.hlsl"),
    (8, "ffx_frameinterpolation_inpainting_pass.hlsl"),
    (9, "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass.hlsl"),
    (10, "ffx_frameinterpolation_debug_view_pass.hlsl"),
]

OF_PASSES = [
    (0, "ffx_opticalflow_prepare_luma_pass.hlsl"),
    (1, "ffx_opticalflow_compute_luminance_pyramid_pass.hlsl"),
    (2, "ffx_opticalflow_generate_scd_histogram_pass.hlsl"),
    (3, "ffx_opticalflow_compute_scd_divergence_pass.hlsl"),
    (4, "ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl"),
    (5, "ffx_opticalflow_filter_optical_flow_pass_v5.hlsl"),
    (6, "ffx_opticalflow_scale_optical_flow_advanced_pass_v5.hlsl"),
]

@dataclass(frozen=True)
class Key:
    effect: int
    pass_id: int
    options: int


def resolve_dxc(arg: str | None) -> pathlib.Path:
    if arg:
        path = pathlib.Path(arg).resolve()
    elif os.name == "nt":
        path = ROOT / "tools/shader_compiler/windows/bin/dxc.exe"
    else:
        path = ROOT / "tools/shader_compiler/linux/bin/dxc"
    if not path.is_file():
        raise SystemExit(f"DXC not found: {path}")
    return path


def compile_one(dxc: pathlib.Path, source: pathlib.Path, defines: list[str], fp16: bool) -> bytes:
    cmd = [
        str(dxc), "-spirv", "-T", "cs_6_6", "-E", "CS",
        "-fspv-target-env=vulkan1.2",
        "-fvk-b-shift", "0", "0",
        "-fvk-t-shift", "32", "0",
        "-fvk-u-shift", "64", "0",
        "-fvk-s-shift", "96", "0",
        "-I", str(INCLUDE_DIR),
        "-I", str(GPU_INCLUDE_DIR),
        "-DFFX_GPU=1", "-DFFX_HLSL=1",
    ]
    if fp16:
        cmd += ["-DFFX_HALF=1"]
    else:
        cmd += ["-DFFX_HALF=0"]
    cmd += [f"-D{define}" for define in defines]

    env = os.environ.copy()
    if os.name != "nt":
        libdir = str(dxc.parent)
        env["LD_LIBRARY_PATH"] = libdir + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")

    with tempfile.NamedTemporaryFile(suffix=".spv", delete=False) as tmp:
        out_path = pathlib.Path(tmp.name)
    try:
        proc = subprocess.run(cmd + [str(source), "-Fo", str(out_path)], cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode:
            sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
            raise SystemExit(f"DXC failed for {source.name} defines={defines}")
        if proc.stderr:
            # Treat compiler diagnostics as failures. The selected Vulkan permutation set is expected warning-clean.
            sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
            raise SystemExit(f"DXC emitted diagnostics for {source.name} defines={defines}")
        data = out_path.read_bytes()
    finally:
        out_path.unlink(missing_ok=True)
    if len(data) < 20 or data[:4] != struct.pack("<I", 0x07230203):
        raise SystemExit(f"DXC did not emit valid SPIR-V for {source.name}")
    return data


def permutations():
    for pass_id, filename in FI_PASSES:
        for core in range(8):
            for fp16 in (False, True):
                options = core | (FI_ALLOW_FP16 if fp16 else 0)
                defines = [
                    f"FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={1 if core & FI_LOW_RES else 0}",
                    f"FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS={1 if core & FI_JITTER else 0}",
                    f"FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={1 if core & FI_INVERTED else 0}",
                ]
                yield Key(EFFECT_FRAME_INTERPOLATION, pass_id, options), SHADER_DIR / filename, defines, fp16

    for pass_id, filename in OF_PASSES:
        for hdr in (False, True):
            for fp16 in (False, True):
                options = (OF_HDR if hdr else 0) | (OF_ALLOW_FP16 if fp16 else 0)
                defines = [
                    f"FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={1 if hdr else 0}",
                    "APUSR_OPTICALFLOW_VULKAN_PACKED_SAD=1",
                ]
                yield Key(EFFECT_OPTICAL_FLOW, pass_id, options), SHADER_DIR / filename, defines, fp16


def write_pack(dxc: pathlib.Path, out_h: pathlib.Path, out_cpp: pathlib.Path) -> None:
    compiled: dict[Key, bytes] = {}
    unique: dict[str, bytes] = {}
    key_hash: dict[Key, str] = {}
    compile_cache: dict[tuple[pathlib.Path, tuple[str, ...], bool], bytes] = {}

    all_perms = list(permutations())
    compile_count = 0
    for key, source, defines, fp16 in all_perms:
        compile_key = (source, tuple(defines), fp16)
        blob = compile_cache.get(compile_key)
        if blob is None:
            blob = compile_one(dxc, source, defines, fp16)
            compile_cache[compile_key] = blob
            compile_count += 1
            print(
                f"[{compile_count:3}] {source.name} effect={key.effect} pass={key.pass_id} "
                f"defines={','.join(defines)} fp16={int(fp16)} {len(blob)} B",
                file=sys.stderr,
            )
        digest = hashlib.sha256(blob).hexdigest()
        compiled[key] = blob
        unique.setdefault(digest, blob)
        key_hash[key] = digest

    ordered_hashes = sorted(unique)
    offsets: dict[str, tuple[int, int]] = {}
    words: list[int] = []
    byte_offset = 0
    for digest in ordered_hashes:
        blob = unique[digest]
        if len(blob) % 4:
            raise SystemExit("SPIR-V blob was not word aligned")
        offsets[digest] = (byte_offset, len(blob))
        words.extend(struct.unpack(f"<{len(blob) // 4}I", blob))
        byte_offset += len(blob)

    entries = sorted(compiled, key=lambda k: (k.effect, k.pass_id, k.options))
    out_h.parent.mkdir(parents=True, exist_ok=True)
    out_h.write_text("""// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
// Generated by tools/compile_framegen_vulkan_shaders.py. Do not edit by hand.
#pragma once

#include <cstddef>
#include <cstdint>

namespace apusr::framegen::vk {

struct SpirvPermutation {
    std::uint8_t effect;
    std::uint8_t passId;
    std::uint16_t reserved;
    std::uint32_t options;
    std::uint32_t offset;
    std::uint32_t size;
};

const std::uint32_t* framegenSpirvWords() noexcept;
std::size_t framegenSpirvWordCount() noexcept;
const SpirvPermutation* framegenSpirvPermutations() noexcept;
std::size_t framegenSpirvPermutationCount() noexcept;
const std::uint8_t* findFramegenSpirv(std::uint8_t effect, std::uint8_t passId, std::uint32_t options, std::uint32_t* size) noexcept;

} // namespace apusr::framegen::vk
""", encoding="utf-8")

    with out_cpp.open("w", encoding="utf-8") as f:
        f.write("// SPDX-License-Identifier: MIT\n")
        f.write("// Copyright (C) 2025 Advanced Micro Devices, Inc.\n")
        f.write("// Copyright (c) 2026 thomthehound.\n")
        f.write("// APUSR-owned pack generated from AMD-derived MIT-licensed FI/OF shaders.\n")
        f.write("// Generated by tools/compile_framegen_vulkan_shaders.py. Do not edit by hand.\n")
        f.write('#include "apusr_framegen_vk_spirv.h"\n\n')
        f.write("namespace apusr::framegen::vk {\nnamespace {\n")
        f.write("alignas(4) constexpr std::uint32_t kSpirv[] = {\n")
        for i in range(0, len(words), 8):
            f.write("    " + ", ".join(f"0x{w:08x}u" for w in words[i:i+8]) + ",\n")
        f.write("};\n\nconstexpr SpirvPermutation kPermutations[] = {\n")
        for key in entries:
            off, size = offsets[key_hash[key]]
            f.write(f"    {{{key.effect}, {key.pass_id}, 0, 0x{key.options:08x}u, {off}u, {size}u}},\n")
        f.write("};\n} // namespace\n\n")
        f.write("const std::uint32_t* framegenSpirvWords() noexcept { return kSpirv; }\n")
        f.write("std::size_t framegenSpirvWordCount() noexcept { return (sizeof(kSpirv) / sizeof(kSpirv[0])); }\n")
        f.write("const SpirvPermutation* framegenSpirvPermutations() noexcept { return kPermutations; }\n")
        f.write("std::size_t framegenSpirvPermutationCount() noexcept { return (sizeof(kPermutations) / sizeof(kPermutations[0])); }\n")
        f.write("const std::uint8_t* findFramegenSpirv(std::uint8_t effect, std::uint8_t passId, std::uint32_t options, std::uint32_t* size) noexcept {\n")
        f.write("    for (const auto& p : kPermutations) {\n")
        f.write("        if (p.effect == effect && p.passId == passId && p.options == options) {\n")
        f.write("            if (size) *size = p.size;\n")
        f.write("            return reinterpret_cast<const std::uint8_t*>(kSpirv) + p.offset;\n")
        f.write("        }\n    }\n")
        f.write("    if (size) *size = 0;\n    return nullptr;\n}\n")
        f.write("} // namespace apusr::framegen::vk\n")

    print(
        f"compiled {compile_count} shader variants for {len(entries)} permutation keys -> "
        f"{len(unique)} unique blobs, {byte_offset} bytes",
        file=sys.stderr,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc")
    parser.add_argument("--header", default=str(ROOT / "src/framegen/ffx_vk/generated/apusr_framegen_vk_spirv.h"))
    parser.add_argument("--source", default=str(ROOT / "src/framegen/ffx_vk/generated/apusr_framegen_vk_spirv.cpp"))
    args = parser.parse_args()
    write_pack(resolve_dxc(args.dxc), pathlib.Path(args.header), pathlib.Path(args.source))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
