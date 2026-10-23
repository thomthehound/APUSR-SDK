#!/usr/bin/env python3
"""Build APUSR's D3D12 Frame Generation shader permutation pack.

The normal path invokes DXC directly.  ``--import-reference`` is an explicit
migration/comparison path that understands FidelityFX-SC's generated-header
format under ``third_party/ffx_sdk``; the product build never consumes that
reference tree.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "src/framegen/algorithm/shaders"
INCLUDE_DIR = ROOT / "src/framegen/algorithm/include"
GPU_INCLUDE_DIR = INCLUDE_DIR / "gpu"
REFERENCE_DIR = ROOT / "third_party/ffx_sdk/framegeneration/generated/ffx_sc_output"
DEFAULT_HEADER = ROOT / "src/framegen/algorithm/generated/apusr_framegen_dxil.h"
DEFAULT_SOURCE = ROOT / "src/framegen/algorithm/generated/apusr_framegen_dxil.cpp"

EFFECT_FRAME_INTERPOLATION = 1
EFFECT_OPTICAL_FLOW = 2

FI_LOW_RES = 1 << 0
FI_JITTER = 1 << 1
FI_INVERTED = 1 << 2
FI_WAVE64 = 1 << 3
FI_FP16 = 1 << 4
OF_WAVE64 = 1 << 0
OF_FP16 = 1 << 1
OF_HDR = 1 << 2

FI_PASSES = [
    (0, "ffx_frameinterpolation_reconstruct_and_dilate_pass"),
    (1, "ffx_frameinterpolation_setup_pass"),
    (2, "ffx_frameinterpolation_reconstruct_previous_depth_pass"),
    (3, "ffx_frameinterpolation_game_motion_vector_field_pass"),
    (4, "ffx_frameinterpolation_optical_flow_vector_field_pass"),
    (5, "ffx_frameinterpolation_disocclusion_mask_pass"),
    (6, "ffx_frameinterpolation_pass"),
    (7, "ffx_frameinterpolation_compute_inpainting_pyramid_pass"),
    (8, "ffx_frameinterpolation_inpainting_pass"),
    (9, "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass"),
    (10, "ffx_frameinterpolation_debug_view_pass"),
]

OF_PASSES = [
    (0, "ffx_opticalflow_prepare_luma_pass"),
    (1, "ffx_opticalflow_compute_luminance_pyramid_pass"),
    (2, "ffx_opticalflow_generate_scd_histogram_pass"),
    (3, "ffx_opticalflow_compute_scd_divergence_pass"),
    (4, "ffx_opticalflow_compute_optical_flow_advanced_pass_v5"),
    (5, "ffx_opticalflow_filter_optical_flow_pass_v5"),
    (6, "ffx_opticalflow_scale_optical_flow_advanced_pass_v5"),
]


@dataclass(frozen=True)
class Key:
    effect: int
    pass_id: int
    options: int


@dataclass(frozen=True)
class Binding:
    name: str
    binding: int
    count: int = 1
    space: int = 0


@dataclass(frozen=True)
class Reflection:
    cbv: tuple[Binding, ...] = ()
    srv_texture: tuple[Binding, ...] = ()
    uav_texture: tuple[Binding, ...] = ()
    srv_buffer: tuple[Binding, ...] = ()
    uav_buffer: tuple[Binding, ...] = ()


@dataclass(frozen=True)
class CompiledShader:
    data: bytes
    reflection: Reflection


def resolve_dxc(arg: str | None) -> pathlib.Path:
    if arg:
        path = pathlib.Path(arg).resolve()
    elif os.name == "nt":
        path = ROOT / "tools/shader_compiler/windows/bin/dxc.exe"
    else:
        path = ROOT / "tools/shader_compiler/linux/bin/dxc"
    if not path.is_file():
        raise SystemExit(f"DXC not found: {path}")
    if os.name != "nt":
        path.chmod(path.stat().st_mode | 0o111)
    return path


def dxc_env(dxc: pathlib.Path) -> dict[str, str]:
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = str(dxc.parent) + os.pathsep + env.get("PATH", "")
    else:
        env["LD_LIBRARY_PATH"] = str(dxc.parent) + (
            os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
        )
    return env


def option_defines(key: Key) -> list[str]:
    if key.effect == EFFECT_FRAME_INTERPOLATION:
        # The old AMD DX12 build script spelled this define JITTER_MOTION_VECTORS,
        # but the maintained shader consumes JITTERED_MOTION_VECTORS. Keep the
        # consumed spelling here: this enables cancellation of camera jitter that
        # is already present in the game's motion-vector texture.
        return [
            f"FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={int(bool(key.options & FI_LOW_RES))}",
            f"FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS={int(bool(key.options & FI_JITTER))}",
            f"FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={int(bool(key.options & FI_INVERTED))}",
        ]
    return [f"FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={int(bool(key.options & OF_HDR))}"]


def compile_shader(dxc: pathlib.Path, key: Key, stem: str) -> CompiledShader:
    wave64 = bool(key.options & (FI_WAVE64 if key.effect == EFFECT_FRAME_INTERPOLATION else OF_WAVE64))
    fp16 = bool(key.options & (FI_FP16 if key.effect == EFFECT_FRAME_INTERPOLATION else OF_FP16))
    source = SHADER_DIR / f"{stem}.hlsl"
    if not source.is_file():
        raise SystemExit(f"Frame Generation shader not found: {source}")

    args = [
        str(dxc), "-Zs", "-E", "CS", "-Wno-for-redefinition", "-Wno-ambig-lit-shift",
        "-DFFX_HLSL=1", "-DFFX_GPU=1", "-DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0",
        "-I", str(INCLUDE_DIR), "-I", str(GPU_INCLUDE_DIR),
    ]
    if key.effect == EFFECT_FRAME_INTERPOLATION:
        args += [
            "-DFFX_FRAMEINTERPOLATION_EMBED_ROOTSIG=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
            "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
            "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        ]
    else:
        args += ["-DFFX_OPTICALFLOW_EMBED_ROOTSIG=0"]

    args += [f"-D{define}" for define in option_defines(key)]
    if fp16:
        args += ["-DFFX_HALF=1", "-enable-16bit-types"]
    else:
        args += ["-DFFX_HALF=0"]
    if wave64:
        args += ["-DFFX_PREFER_WAVE64=[WaveSize(64)]", "-DFFX_HLSL_SM=66", "-T", "cs_6_6"]
    else:
        args += ["-DFFX_HLSL_SM=62", "-T", "cs_6_2"]

    with tempfile.TemporaryDirectory(prefix="apusr_fg_dxil_") as td:
        out = pathlib.Path(td) / "shader.dxil"
        command = args + ["-Fo", str(out), str(source)]
        proc = subprocess.run(command, cwd=ROOT, env=dxc_env(dxc), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode:
            sys.stderr.write(proc.stdout.decode("utf-8", "replace"))
            sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
            raise SystemExit(f"DXC failed for {stem}, options=0x{key.options:x}")
        data = out.read_bytes()
        if len(data) < 4 or data[:4] != b"DXBC":
            raise SystemExit(f"DXC did not emit a valid DXIL container for {stem}")
        dump = subprocess.run(
            [str(dxc), "-dumpbin", str(out)], cwd=ROOT, env=dxc_env(dxc),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if dump.returncode:
            sys.stderr.write(dump.stderr.decode("utf-8", "replace"))
            raise SystemExit(f"DXC reflection dump failed for {stem}")
        reflection = parse_dxc_reflection(dump.stdout.decode("utf-8", "replace"))
    return CompiledShader(data, reflection)


def parse_dxc_reflection(text: str) -> Reflection:
    marker = "; Resource Bindings:"
    start = text.find(marker)
    if start < 0:
        return Reflection()
    lines = text[start:].splitlines()[1:]
    rows: list[str] = []
    in_table = False
    for raw in lines:
        line = raw[1:].strip() if raw.startswith(";") else raw.strip()
        if not in_table:
            if line.startswith("---"):
                in_table = True
            continue
        if not line:
            break
        if line.startswith("---") or line.startswith("Name"):
            continue
        rows.append(line)

    groups: dict[str, list[Binding]] = {k: [] for k in ("cbv", "srv_texture", "uav_texture", "srv_buffer", "uav_buffer")}
    bind_re = re.compile(r"^(cb|[tus])(\d+)(?:,space(\d+))?$")
    for row in rows:
        cols = row.split()
        if len(cols) < 7:
            continue
        name = cols[0]
        kind = cols[1]
        dim = cols[3].lower()
        bind_token = cols[-2]
        count_token = cols[-1]
        match = bind_re.match(bind_token)
        if not match:
            continue
        prefix, binding_s, space_s = match.groups()
        binding = int(binding_s)
        space = int(space_s or 0)
        count = 0 if count_token.lower() == "unbounded" else int(count_token)
        item = Binding(name, binding, count, space)
        if kind == "cbuffer" or prefix == "cb":
            groups["cbv"].append(item)
        elif kind == "sampler" or prefix == "s":
            continue
        elif prefix == "t":
            groups["srv_buffer" if dim in {"r/o", "r/w", "raw", "struct"} else "srv_texture"].append(item)
        elif prefix == "u":
            groups["uav_buffer" if dim in {"r/o", "r/w", "raw", "struct"} else "uav_texture"].append(item)
    return Reflection(*(tuple(groups[k]) for k in ("cbv", "srv_texture", "uav_texture", "srv_buffer", "uav_buffer")))


def all_compile_jobs() -> list[tuple[Key, str]]:
    jobs: list[tuple[Key, str]] = []
    for pass_id, stem in FI_PASSES:
        for core in range(8):
            for wave64 in (False, True):
                for fp16 in (False, True):
                    options = core | (FI_WAVE64 if wave64 else 0) | (FI_FP16 if fp16 else 0)
                    jobs.append((Key(EFFECT_FRAME_INTERPOLATION, pass_id, options), stem))
    for pass_id, stem in OF_PASSES:
        for hdr in (False, True):
            for wave64 in (False, True):
                for fp16 in (False, True):
                    options = (OF_HDR if hdr else 0) | (OF_WAVE64 if wave64 else 0) | (OF_FP16 if fp16 else 0)
                    jobs.append((Key(EFFECT_OPTICAL_FLOW, pass_id, options), stem))
    return jobs


def compile_all(dxc: pathlib.Path) -> dict[Key, CompiledShader]:
    result: dict[Key, CompiledShader] = {}
    jobs = all_compile_jobs()
    for index, (key, stem) in enumerate(jobs, 1):
        print(f"[{index:3}/{len(jobs)}] {stem} options=0x{key.options:02x}", file=sys.stderr)
        result[key] = compile_shader(dxc, key, stem)
    return result


def parse_int_array(text: str, suffix: str) -> tuple[int, ...]:
    match = re.search(rf"{re.escape(suffix)}\[\]\s*=\s*\{{([^}}]*)\}};", text, re.S)
    if not match:
        return ()
    return tuple(int(v) for v in re.findall(r"\b\d+\b", match.group(1)))


def parse_str_array(text: str, suffix: str) -> tuple[str, ...]:
    match = re.search(rf"{re.escape(suffix)}\[\]\s*=\s*\{{([^}}]*)\}};", text, re.S)
    if not match:
        return ()
    return tuple(re.findall(r'"([^"]+)"', match.group(1)))


def legacy_blob(path: pathlib.Path) -> CompiledShader:
    text = path.read_text(encoding="utf-8")
    data_match = re.search(r"_data\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not data_match:
        raise SystemExit(f"Reference blob data missing: {path}")
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", data_match.group(1)))

    def bindings(kind: str) -> tuple[Binding, ...]:
        names = parse_str_array(text, f"_{kind}ResourceNames")
        vals = parse_int_array(text, f"_{kind}ResourceBindings")
        counts = parse_int_array(text, f"_{kind}ResourceCounts")
        spaces = parse_int_array(text, f"_{kind}ResourceSpaces")
        if not names:
            return ()
        return tuple(Binding(n, vals[i], counts[i], spaces[i]) for i, n in enumerate(names))

    return CompiledShader(data, Reflection(
        cbv=bindings("CBV"),
        srv_texture=bindings("TextureSRV"),
        uav_texture=bindings("TextureUAV"),
        srv_buffer=bindings("BufferSRV"),
        uav_buffer=bindings("BufferUAV"),
    ))


def parse_indirection(text: str) -> tuple[int, ...]:
    match = re.search(r"_IndirectionTable\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        raise SystemExit("Reference permutation header has no indirection table")
    return tuple(int(v) for v in re.findall(r"\b\d+\b", match.group(1)))


def parse_info_blob_names(text: str) -> list[str]:
    block = re.search(r"_PermutationInfo\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not block:
        raise SystemExit("Reference permutation header has no permutation info array")
    return re.findall(r"\{\s*g_([A-Za-z0-9_]+)_size\s*,", block.group(1))


def import_reference() -> dict[Key, CompiledShader]:
    if not REFERENCE_DIR.is_dir():
        raise SystemExit(f"FidelityFX reference permutation directory not found: {REFERENCE_DIR}")
    blob_cache: dict[str, CompiledShader] = {}
    result: dict[Key, CompiledShader] = {}

    def import_family(effect: int, pass_id: int, stem: str, core_count: int, wave_bit: int, fp16_bit: int, core_to_options) -> None:
        for wave64 in (False, True):
            for fp16 in (False, True):
                suffix = ("_wave64" if wave64 else "") + ("_16bit" if fp16 else "")
                path = REFERENCE_DIR / f"{stem}{suffix}_permutations.h"
                text = path.read_text(encoding="utf-8")
                table = parse_indirection(text)
                info_names = parse_info_blob_names(text)
                if len(table) != core_count:
                    raise SystemExit(f"Unexpected permutation count in {path}: {len(table)} != {core_count}")
                for core, info_index in enumerate(table):
                    blob_name = info_names[info_index]
                    shader = blob_cache.get(blob_name)
                    if shader is None:
                        shader = legacy_blob(REFERENCE_DIR / f"{blob_name}.h")
                        blob_cache[blob_name] = shader
                    options = core_to_options(core) | (wave_bit if wave64 else 0) | (fp16_bit if fp16 else 0)
                    result[Key(effect, pass_id, options)] = shader

    for pass_id, stem in FI_PASSES:
        import_family(EFFECT_FRAME_INTERPOLATION, pass_id, stem, 8, FI_WAVE64, FI_FP16, lambda core: core)
    for pass_id, stem in OF_PASSES:
        import_family(EFFECT_OPTICAL_FLOW, pass_id, stem, 2, OF_WAVE64, OF_FP16, lambda core: OF_HDR if core else 0)
    print(f"Imported {len(result)} permutation keys from {len(blob_cache)} reference blobs.", file=sys.stderr)
    return result


def c_bytes(data: bytes) -> str:
    return "\n".join(
        "    " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ","
        for i in range(0, len(data), 16)
    )


def write_binding_arrays(f, prefix: str, values: tuple[Binding, ...]) -> tuple[str, str, str, str]:
    if not values:
        return ("nullptr", "nullptr", "nullptr", "nullptr")
    names = f"{prefix}Names"
    binds = f"{prefix}Bindings"
    counts = f"{prefix}Counts"
    spaces = f"{prefix}Spaces"
    f.write(f"const char* {names}[] = {{{', '.join(repr(v.name) for v in values)}}};\n".replace("'", '"'))
    f.write(f"constexpr std::uint32_t {binds}[] = {{{', '.join(str(v.binding) + 'u' for v in values)}}};\n")
    f.write(f"constexpr std::uint32_t {counts}[] = {{{', '.join(str(v.count) + 'u' for v in values)}}};\n")
    f.write(f"constexpr std::uint32_t {spaces}[] = {{{', '.join(str(v.space) + 'u' for v in values)}}};\n")
    return names, binds, counts, spaces


def write_pack(compiled: dict[Key, CompiledShader], header: pathlib.Path, source: pathlib.Path, provenance: str) -> None:
    ordered_keys = sorted(compiled, key=lambda k: (k.effect, k.pass_id, k.options))
    unique: dict[str, CompiledShader] = {}
    digest_for_key: dict[Key, str] = {}
    for key in ordered_keys:
        shader = compiled[key]
        digest = hashlib.sha256(shader.data).hexdigest()
        previous = unique.get(digest)
        if previous is not None and previous.reflection != shader.reflection:
            raise SystemExit(f"Identical DXIL has inconsistent reflection: {digest}")
        unique.setdefault(digest, shader)
        digest_for_key[key] = digest
    ordered_digests = sorted(unique)
    blob_index = {digest: i for i, digest in enumerate(ordered_digests)}

    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(
        "// SPDX-License-Identifier: Apache-2.0\n"
        "// Copyright (c) 2026 thomthehound.\n"
        "// Generated by tools/compile_framegen_dx12_shaders.py. Do not edit by hand.\n"
        "#pragma once\n\n"
        "#include \"apusr_framegen_backend_types.h\"\n\n"
        "namespace apusr::framegen::dxil {\n"
        "bool findShaderBlob(ApusrFramegenShaderEffect effect, std::uint8_t passId, std::uint32_t permutationOptions, ApusrFramegenShaderBlob* outBlob) noexcept;\n"
        "} // namespace apusr::framegen::dxil\n",
        encoding="utf-8",
    )

    with source.open("w", encoding="utf-8", newline="\n") as f:
        f.write("// SPDX-License-Identifier: MIT\n")
        f.write("// Copyright (C) 2025 Advanced Micro Devices, Inc.\n")
        f.write("// Copyright (c) 2026 thomthehound.\n")
        f.write("// APUSR-owned pack generated from AMD-derived MIT-licensed FI/OF shaders.\n")
        f.write("// Generated by tools/compile_framegen_dx12_shaders.py. Do not edit by hand.\n")
        f.write(f"// Payload provenance: {provenance}\n")
        f.write('#include "apusr_framegen_dxil.h"\n\n#include <cstring>\n\n')
        f.write("namespace apusr::framegen::dxil {\nnamespace {\n")
        f.write("struct BlobRecord {\n    const std::uint8_t* data;\n    std::uint32_t size;\n    std::uint32_t cbvCount, srvTextureCount, uavTextureCount, srvBufferCount, uavBufferCount;\n    const char** cbvNames; const std::uint32_t* cbvBindings; const std::uint32_t* cbvCounts; const std::uint32_t* cbvSpaces;\n    const char** srvTextureNames; const std::uint32_t* srvTextureBindings; const std::uint32_t* srvTextureCounts; const std::uint32_t* srvTextureSpaces;\n    const char** uavTextureNames; const std::uint32_t* uavTextureBindings; const std::uint32_t* uavTextureCounts; const std::uint32_t* uavTextureSpaces;\n    const char** srvBufferNames; const std::uint32_t* srvBufferBindings; const std::uint32_t* srvBufferCounts; const std::uint32_t* srvBufferSpaces;\n    const char** uavBufferNames; const std::uint32_t* uavBufferBindings; const std::uint32_t* uavBufferCounts; const std::uint32_t* uavBufferSpaces;\n};\n\n")

        records: list[str] = []
        for i, digest in enumerate(ordered_digests):
            shader = unique[digest]
            short = digest[:16]
            data_name = f"kDxil_{short}"
            f.write(f"// SHA256 {digest}\nconstexpr std::uint8_t {data_name}[] = {{\n{c_bytes(shader.data)}\n}};\n")
            arrays = []
            for tag, values in (
                ("Cbv", shader.reflection.cbv), ("SrvTex", shader.reflection.srv_texture),
                ("UavTex", shader.reflection.uav_texture), ("SrvBuf", shader.reflection.srv_buffer),
                ("UavBuf", shader.reflection.uav_buffer),
            ):
                arrays.append(write_binding_arrays(f, f"k{tag}_{short}", values))
            counts = [len(shader.reflection.cbv), len(shader.reflection.srv_texture), len(shader.reflection.uav_texture), len(shader.reflection.srv_buffer), len(shader.reflection.uav_buffer)]
            flat = [item for group in arrays for item in group]
            records.append(
                f"    {{{data_name}, static_cast<std::uint32_t>(sizeof({data_name})), "
                + ", ".join(f"{c}u" for c in counts) + ", " + ", ".join(flat) + "},"
            )
            f.write("\n")

        if len(ordered_digests) >= 0xffff:
            raise SystemExit("DXIL pack has too many unique blobs for the compact lookup table")
        f.write("constexpr BlobRecord kBlobs[] = {\n" + "\n".join(records) + "\n};\n\n")
        f.write(f"// Logical permutations: {len(ordered_keys)}; unique DXIL blobs: {len(ordered_digests)}\n")

        def write_index_table(name: str, effect: int, pass_count: int, option_count: int) -> None:
            f.write(f"constexpr std::uint16_t {name}[{pass_count}][{option_count}] = {{\n")
            for pass_id in range(pass_count):
                values = []
                for options in range(option_count):
                    key = Key(effect, pass_id, options)
                    digest = digest_for_key.get(key)
                    if digest is None:
                        values.append("0xffffu")
                    else:
                        values.append(f"{blob_index[digest]}u")
                f.write("    {" + ", ".join(values) + "},\n")
            f.write("};\n")

        write_index_table("kFiBlobIndex", EFFECT_FRAME_INTERPOLATION, len(FI_PASSES), 32)
        write_index_table("kOfBlobIndex", EFFECT_OPTICAL_FLOW, len(OF_PASSES), 8)
        f.write("} // namespace\n\n")
        f.write("bool findShaderBlob(ApusrFramegenShaderEffect effect, std::uint8_t passId, std::uint32_t permutationOptions, ApusrFramegenShaderBlob* outBlob) noexcept {\n")
        f.write("    if (!outBlob) return false;\n")
        f.write("    std::uint16_t blobIndex = 0xffffu;\n")
        f.write("    if (effect == APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION) {\n")
        f.write(f"        if (passId < {len(FI_PASSES)}u && permutationOptions < 32u) blobIndex = kFiBlobIndex[passId][permutationOptions];\n")
        f.write("    } else if (effect == APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW) {\n")
        f.write(f"        if (passId < {len(OF_PASSES)}u && permutationOptions < 8u) blobIndex = kOfBlobIndex[passId][permutationOptions];\n")
        f.write("    }\n")
        f.write("    if (blobIndex == 0xffffu) { std::memset(outBlob, 0, sizeof(*outBlob)); return false; }\n")
        f.write("    const auto& blob = kBlobs[blobIndex];\n")
        f.write("    *outBlob = {blob.data, blob.size, blob.cbvCount, blob.srvTextureCount, blob.uavTextureCount, blob.srvBufferCount, blob.uavBufferCount, blob.cbvNames, blob.cbvBindings, blob.cbvCounts, blob.cbvSpaces, blob.srvTextureNames, blob.srvTextureBindings, blob.srvTextureCounts, blob.srvTextureSpaces, blob.uavTextureNames, blob.uavTextureBindings, blob.uavTextureCounts, blob.uavTextureSpaces, blob.srvBufferNames, blob.srvBufferBindings, blob.srvBufferCounts, blob.srvBufferSpaces, blob.uavBufferNames, blob.uavBufferBindings, blob.uavBufferCounts, blob.uavBufferSpaces, effect, passId, 0u, permutationOptions};\n")
        f.write("    return true;\n}\n")
        f.write("} // namespace apusr::framegen::dxil\n")

    total_bytes = sum(len(unique[d].data) for d in ordered_digests)
    print(f"Wrote {len(ordered_keys)} keys -> {len(ordered_digests)} unique DXIL blobs ({total_bytes} bytes).", file=sys.stderr)


def smoke_compile(dxc: pathlib.Path) -> None:
    checks = [
        (Key(EFFECT_FRAME_INTERPOLATION, 1, FI_JITTER | FI_INVERTED | FI_WAVE64 | FI_FP16), "ffx_frameinterpolation_setup_pass"),
        (Key(EFFECT_OPTICAL_FLOW, 0, OF_HDR | OF_WAVE64 | OF_FP16), "ffx_opticalflow_prepare_luma_pass"),
    ]
    for key, stem in checks:
        shader = compile_shader(dxc, key, stem)
        if not shader.reflection.cbv:
            raise SystemExit(f"Reflection smoke check found no CBV for {stem}")
        print(f"smoke: {stem}: {len(shader.data)} bytes, {sum(len(v) for v in (shader.reflection.cbv, shader.reflection.srv_texture, shader.reflection.uav_texture, shader.reflection.srv_buffer, shader.reflection.uav_buffer))} reflected resources", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc")
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER)
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument("--import-reference", action="store_true", help="Import the quarantined FidelityFX-SC reference blobs instead of recompiling. Explicit migration/comparison path only.")
    parser.add_argument("--smoke", action="store_true", help="Compile one FI and one OF permutation and validate DXC reflection parsing without writing the pack.")
    args = parser.parse_args()

    if args.smoke:
        smoke_compile(resolve_dxc(args.dxc))
        return 0
    if args.import_reference:
        compiled = import_reference()
        provenance = "imported from quarantined FidelityFX-SC reference output; DXIL bytes preserved exactly"
    else:
        dxc = resolve_dxc(args.dxc)
        compiled = compile_all(dxc)
        provenance = "compiled directly by DXC from APUSR-maintained Frame Generation shader sources"
    write_pack(compiled, args.header.resolve(), args.source.resolve(), provenance)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
