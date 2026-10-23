#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Compile imported FSR4 INT8 reference shaders into APUSR DX12 DXIL blobs.

This tool deliberately treats a checked-in blob set as a shader-cache slice, not
as the product feature matrix.  The preset/capacity/permutation fields generated
into the blob table are used by the runtime to reject mismatched blobs instead of
accidentally using one developer's test-bed permutation for every game mode.
"""

from __future__ import annotations

import argparse
import os
import platform
import signal
import stat
import shutil
import subprocess
import hashlib
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ShaderJob:
    name: str
    source: Path
    entry: str
    kind: str
    logical_stage: int
    extra_defines: tuple[str, ...] = ()
    use_model_defines: bool = True


@dataclass(frozen=True)
class PermutationOptions:
    wmma: bool
    depth_inverted: bool
    low_res_motion_vectors: bool
    auto_exposure: bool
    jittered_motion_vectors: bool
    color_space: str
    debug_visualize: bool


UTILITY_DEFINES = (
    "FFX_GPU=1",
    "FFX_HLSL=1",
    "FFX_HLSL_SM=66",
)

PRESET_ENUM = {
    "native": "NativeAA",
    "quality": "Quality",
    "balanced": "Balanced",
    "performance": "Performance",
    "drs": "DynamicResolution",
    "ultraperf": "UltraPerformance",
}
PRESET_VALUE = {
    "native": 0,
    "quality": 1,
    "balanced": 2,
    "performance": 3,
    "drs": 4,
    "ultraperf": 5,
}
CAPACITY_ENUM = {
    "1080": "R1080",
    "2160": "R2160",
    "4320": "R4320",
}
CAPACITY_VALUE = {
    "1080": 0,
    "2160": 1,
    "4320": 2,
}
PASS_KIND_ENUM = {
    "spd-auto-exposure": "SpdAutoExposure",
    "model-pre": "ModelPre",
    "model-middle": "ModelMiddle",
    "model-post": "ModelPost",
    "rcas": "Rcas",
    "debug-view": "DebugView",
}


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--preset", default="balanced", choices=("all", *tuple(PRESET_ENUM)))
    parser.add_argument("--capacity", default="2160", choices=("all", *tuple(CAPACITY_ENUM)))
    parser.add_argument("--wmma", action="store_true", help="Compile the WMMA/FP8 shader flavor instead of the DP4A INT8 reference flavor. DP4A INT8 remains the current default reference flavor.")
    parser.add_argument("--depth-inverted", dest="depth_inverted", action="store_true", default=True)
    parser.add_argument("--no-depth-inverted", dest="depth_inverted", action="store_false")
    parser.add_argument("--low-res-motion-vectors", dest="low_res_motion_vectors", action="store_true", default=True)
    parser.add_argument("--display-res-motion-vectors", dest="low_res_motion_vectors", action="store_false")
    parser.add_argument("--auto-exposure", dest="auto_exposure", action="store_true", default=True)
    parser.add_argument("--no-auto-exposure", dest="auto_exposure", action="store_false")
    parser.add_argument("--jittered-motion-vectors", action="store_true")
    parser.add_argument("--color-space", choices=("linear", "nonlinear", "srgb", "pq"), default="linear")
    parser.add_argument("--debug-visualize", action="store_true")
    parser.add_argument(
        "--header",
        type=Path,
        default=repo_root / "include/apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h",
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=repo_root / "src/generated/apusr_fsr4_reference_dx12_shader_blobs.cpp",
    )
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--product-matrix", action="store_true", help="Generate the declared DP4A INT8 reference product matrix for all selected presets/capacities. Requires --cache-root and --cache-only.")
    parser.add_argument("--include-debug", action="store_true", help="With --product-matrix, also generate debug-visualize variants for developer packages.")
    parser.add_argument("--cache-root", type=Path, default=None, help="Optional packaged DXIL cache root. Writes <preset>/<capacity>/p<permutationKey>/<dispatch>.dxil beside or instead of the embedded C++ cache.")
    parser.add_argument("--cache-only", action="store_true", help="Generate only packaged cache files and write an empty C++ blob table. Required for all/all product cache builds.")
    parser.add_argument("--shader-timeout-seconds", type=int, default=30)
    parser.add_argument("--jobs", type=int, default=0, help="Parallel DXC workers for product/cache generation. 0 uses the host CPU count.")
    parser.add_argument("--force", action="store_true", help="Recompile even when packaged-cache outputs already exist.")
    parser.add_argument("--dp4a-optimized-pass1-tiled-exact", action="store_true", help="Generate the DP4A-optimized product matrix by replacing fsr4_pass_1 with APUSR's dispatch-preserving tiled Stage1 shader. This is branch-local product behavior, not an env-gated experiment.")
    parser.add_argument("--dp4a-optimized-front-c16-tiled-exact", action="store_true", help="Generate the DP4A-optimized product matrix by replacing fsr4_pass_1 and fsr4_pass_2 with APUSR's dispatch-preserving tiled front-C16 shaders. This is branch-local product behavior, not an env-gated experiment.")
    return parser.parse_args()


def permutation_key(preset: str, capacity: str, options: PermutationOptions) -> int:
    nonlinear = options.color_space != "linear"
    srgb = options.color_space == "srgb"
    pq = options.color_space == "pq"
    key = 0
    if options.debug_visualize:
        key |= 1
    key <<= 1
    if pq:
        key |= 1
    key <<= 1
    if srgb:
        key |= 1
    key <<= 1
    if nonlinear:
        key |= 1
    key <<= 1
    if options.jittered_motion_vectors:
        key |= 1
    key <<= 1
    if options.auto_exposure:
        key |= 1
    key <<= 1
    if options.low_res_motion_vectors:
        key |= 1
    key <<= 1
    if options.depth_inverted:
        key |= 1
    key <<= 1
    if options.wmma:
        key |= 1
    key <<= 2
    key |= CAPACITY_VALUE[capacity]
    key <<= 3
    key |= PRESET_VALUE[preset]
    return key


def color_space_value(color_space: str) -> int:
    return {"linear": 0, "nonlinear": 1, "srgb": 2, "pq": 3}[color_space]


def model_defines(capacity: str, options: PermutationOptions) -> tuple[str, ...]:
    return (
        "FFX_GPU=1",
        "FFX_HLSL=1",
        "FFX_HALF=1",
        "FFX_HLSL_SM=66",
        f"FFX_MLSR_RESOLUTION={CAPACITY_VALUE[capacity]}",
        f"WMMA_ENABLED={1 if options.wmma else 0}",
        f"DOT4_ENABLED={0 if options.wmma else 1}",
        f"FFX_MLSR_DEPTH_INVERTED={1 if options.depth_inverted else 0}",
        f"FFX_MLSR_LOW_RES_MV={1 if options.low_res_motion_vectors else 0}",
        f"FFX_MLSR_JITTERED_MOTION_VECTORS={1 if options.jittered_motion_vectors else 0}",
        f"FFX_MLSR_AUTOEXPOSURE_ENABLED={1 if options.auto_exposure else 0}",
        f"FFX_MLSR_COLORSPACE={color_space_value(options.color_space)}",
        f"FFX_DEBUG_VISUALIZE={1 if options.debug_visualize else 0}",
    )


def ensure_linux_tool_executable(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"Linux shader compiler tool not found: {path}")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def compiler_paths(repo_root: Path) -> tuple[Path, Path]:
    system_name = platform.system().lower()
    if system_name == "windows":
        compiler_dir = repo_root / "tools/shader_compiler/windows/bin"
        dxc = compiler_dir / "dxc.exe"
    elif system_name == "linux":
        compiler_dir = repo_root / "tools/shader_compiler/linux/bin"
        dxc = compiler_dir / "dxc"
        ensure_linux_tool_executable(dxc)
    else:
        raise SystemExit(f"Unsupported shader compiler host platform: {platform.system()}")
    if not dxc.is_file():
        raise SystemExit(f"DXC compiler not found: {dxc}")
    return compiler_dir, dxc


def terminate_process_tree(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    if platform.system().lower() == "windows":
        process.kill()
        return
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return


def run_bounded_child(command: list[str], repo_root: Path, env: dict[str, str], timeout_seconds: int) -> None:
    creationflags = 0
    start_new_session = False
    if platform.system().lower() == "windows":
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        start_new_session = True

    process = subprocess.Popen(
        command,
        cwd=repo_root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        creationflags=creationflags,
        start_new_session=start_new_session,
    )
    start = time.monotonic()
    while True:
        return_code = process.poll()
        if return_code is not None:
            stdout, stderr = process.communicate()
            if stdout:
                print(stdout.decode(errors="replace"), end="")
            if stderr:
                print(stderr.decode(errors="replace"), end="")
            if return_code != 0:
                raise subprocess.CalledProcessError(return_code, command)
            return
        if time.monotonic() - start > timeout_seconds:
            terminate_process_tree(process)
            try:
                stdout, stderr = process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
            if stdout:
                print(stdout.decode(errors="replace"), end="")
            if stderr:
                print(stderr.decode(errors="replace"), end="")
            raise subprocess.TimeoutExpired(command, timeout_seconds)
        time.sleep(0.05)


def tiled_preset_define(preset: str) -> str:
    return {
        "native": "APUSR_FSR4_I8_PRESET_NATIVE=1",
        "quality": "APUSR_FSR4_I8_PRESET_QUALITY=1",
        "balanced": "APUSR_FSR4_I8_PRESET_BALANCED=1",
        "performance": "APUSR_FSR4_I8_PRESET_PERFORMANCE=1",
        "drs": "APUSR_FSR4_I8_PRESET_DRS=1",
        "ultraperf": "APUSR_FSR4_I8_PRESET_ULTRAPERF=1",
    }[preset]


def tiled_capacity_define(capacity: str) -> str:
    return {
        "1080": "APUSR_FSR4_I8_CAPACITY_1080=1",
        "2160": "APUSR_FSR4_I8_CAPACITY_2160=1",
        "4320": "APUSR_FSR4_I8_CAPACITY_4320=1",
    }[capacity]


def shader_jobs(
    preset: str,
    capacity: str,
    dp4a_optimized_pass1_tiled_exact: bool = False,
    dp4a_optimized_front_c16_tiled_exact: bool = False,
    debug_visualize: bool = False,
) -> list[ShaderJob]:
    root = Path("models/fsr4_int8/reference/fsr4_4_0_2_int8") / preset
    passes = root / f"passes_{capacity}.hlsl"
    common = Path("models/fsr4_int8/reference/fsr4_4_0_2_int8/common")
    pre_extra_defines = ("APUSR_FSR4_I8_PADDED_STAGE0_CARRIER=1",) if dp4a_optimized_front_c16_tiled_exact else ()
    jobs: list[ShaderJob] = [
        ShaderJob("spd_auto_exposure", common / "spd_auto_exposure.hlsl", "main", "spd-auto-exposure", 0, use_model_defines=False),
        ShaderJob("fsr4_pre", root / "pre.hlsl", "main", "model-pre", 0, pre_extra_defines),
    ]
    use_tiled_pass1 = dp4a_optimized_pass1_tiled_exact or dp4a_optimized_front_c16_tiled_exact
    use_tiled_pass2 = dp4a_optimized_front_c16_tiled_exact
    for stage in range(1, 13):
        middle_extra_defines: tuple[str, ...] = ()
        if dp4a_optimized_front_c16_tiled_exact and stage == 12:
            middle_extra_defines = (
                "APUSR_FSR4_I8_CNB_LDS_C16_HALO=1",
                "APUSR_FSR4_I8_CNB_STREAMED_HIDDEN_CONTRACT=1",
            )
        if stage == 1 and use_tiled_pass1:
            jobs.append(
                ShaderJob(
                    "fsr4_pass_1",
                    Path("shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl"),
                    "apusr_fsr4_i8_pass1_tiled_exact",
                    "model-middle",
                    1,
                    (tiled_preset_define(preset), tiled_capacity_define(capacity)),
                )
            )
            continue
        if stage == 2 and use_tiled_pass2:
            jobs.append(
                ShaderJob(
                    "fsr4_pass_2",
                    Path("shaders/dx12/optimized/apusr_fsr4_i8_pass2_tiled_exact.hlsl"),
                    "apusr_fsr4_i8_pass2_tiled_exact",
                    "model-middle",
                    2,
                    (tiled_preset_define(preset), tiled_capacity_define(capacity)),
                )
            )
            continue
        jobs.append(
            ShaderJob(
                f"fsr4_pass_{stage}",
                passes,
                f"fsr4_model_v07_i8_pass{stage}",
                "model-middle",
                stage,
                (f"MLSR_PASS_{stage}=1", *middle_extra_defines),
            )
        )
    post_extra_defines = (
        "APUSR_FSR4_I8_POST_LDS_CNB=1",
        "APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT=1",
        "APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT=1",
        "APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA=1",
    ) if dp4a_optimized_front_c16_tiled_exact else ()
    rcas_shader = (
        Path("shaders/dx12/optimized/apusr_fsr4_rcas_exposure_hoist.hlsl")
        if use_tiled_pass1
        else common / "rcas.hlsl"
    )
    jobs.extend(
        [
            ShaderJob("fsr4_post", root / "post.hlsl", "main", "model-post", 13, post_extra_defines),
            ShaderJob("rcas", rcas_shader, "main", "rcas", 14, use_model_defines=True),
        ]
    )
    if debug_visualize:
        jobs.append(ShaderJob("debug_view", common / "debug_view.hlsl", "main", "debug-view", 15))
    return jobs


def compile_shader(repo_root: Path, compiler_dir: Path, dxc: Path, job: ShaderJob, output_path: Path, timeout_seconds: int, base_model_defines: tuple[str, ...]) -> None:
    source_path = repo_root / job.source
    if not source_path.is_file():
        raise SystemExit(f"Shader source not found: {source_path}")

    env = os.environ.copy()
    if platform.system().lower() == "windows":
        env["PATH"] = str(compiler_dir) + (os.pathsep + env["PATH"] if env.get("PATH") else "")
    else:
        env["LD_LIBRARY_PATH"] = str(compiler_dir) + (os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")

    command = [
        str(dxc),
        "-nologo",
        "-O3",
        "-enable-16bit-types",
        "-HV",
        "2021",
        "-T",
        "cs_6_6",
        "-E",
        job.entry,
        "-I",
        str(repo_root / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include"),
        "-I",
        str(repo_root / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4"),
        "-Fo",
        str(output_path),
    ]
    base_defines = base_model_defines if job.use_model_defines else UTILITY_DEFINES
    for define in (*base_defines, *job.extra_defines):
        command.extend(["-D", define])
    command.append(str(source_path))

    print(f"[fsr4-reference-dxil] compiling {job.name} entry={job.entry}", flush=True)
    start = time.monotonic()
    try:
        run_bounded_child(command, repo_root, env, timeout_seconds)
        print(f"[fsr4-reference-dxil] compiled {job.name} in {time.monotonic() - start:.1f}s", flush=True)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"DXC timed out after {timeout_seconds}s while compiling {job.name}: {source_path}") from exc


def c_array_initializer(data: bytes) -> str:
    lines: list[str] = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    return "\n".join(lines)


def symbol_name(job: ShaderJob) -> str:
    return "k" + "".join(part.capitalize() for part in job.name.replace("-", "_").split("_")) + "Dxil"


def write_header(header: Path) -> None:
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(
        "\n".join(
            [
                "// SPDX-License-Identifier: Apache-2.0",
                "// Copyright (c) 2026 thomthehound.",
                "// Auto-generated by tools/model/compile_fsr4_reference_dx12_shaders.py.",
                "",
                "#pragma once",
                "",
                "#include <apusr/models/apusr_fsr4_int8_graph.h>",
                "",
                "#include <cstddef>",
                "#include <cstdint>",
                "",
                "namespace apusr::backends::fsr4_reference_dx12_shader_blobs {",
                "",
                "struct ShaderBlob {",
                "    const char* dispatchName;",
                "    models::fsr4_int8::Preset preset;",
                "    models::fsr4_int8::FrameCapacity frameCapacity;",
                "    std::uint32_t permutationKey;",
                "    models::fsr4_int8::PassKind kind;",
                "    std::uint32_t logicalStage;",
                "    const std::uint8_t* data;",
                "    std::size_t size;",
                "};",
                "",
                "[[nodiscard]] std::size_t shaderBlobCount() noexcept;",
                "[[nodiscard]] const ShaderBlob* shaderBlobAt(std::size_t index) noexcept;",
                "[[nodiscard]] const ShaderBlob* findShaderBlob(",
                "    models::fsr4_int8::Preset preset,",
                "    models::fsr4_int8::FrameCapacity frameCapacity,",
                "    std::uint32_t permutationKey,",
                "    const char* dispatchName) noexcept;",
                "",
                "} // namespace apusr::backends::fsr4_reference_dx12_shader_blobs",
                "",
            ]
        ),
        encoding="utf-8",
        newline="\n",
    )


def selected_presets(preset: str) -> tuple[str, ...]:
    return tuple(PRESET_ENUM) if preset == "all" else (preset,)


def selected_capacities(capacity: str) -> tuple[str, ...]:
    return tuple(CAPACITY_ENUM) if capacity == "all" else (capacity,)


def write_empty_source(source: Path) -> None:
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(
        "\n".join(
            [
                "// SPDX-License-Identifier: MIT",
                "// Copyright (C) 2025 Advanced Micro Devices, Inc.",
                "// Copyright (c) 2026 thomthehound.",
                "// Derived payload from the MIT-licensed FidelityFX SDK 2.0.0 / FSR 4.0.2 shader sources.",
                "// Auto-generated by tools/model/compile_fsr4_reference_dx12_shaders.py.",
                "",
                "#include <apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h>",
                "",
                "namespace apusr::backends::fsr4_reference_dx12_shader_blobs {",
                "",
                "std::size_t shaderBlobCount() noexcept",
                "{",
                "    return 0;",
                "}",
                "",
                "const ShaderBlob* shaderBlobAt(const std::size_t) noexcept",
                "{",
                "    return nullptr;",
                "}",
                "",
                "const ShaderBlob* findShaderBlob(",
                "    const models::fsr4_int8::Preset,",
                "    const models::fsr4_int8::FrameCapacity,",
                "    const std::uint32_t,",
                "    const char*) noexcept",
                "{",
                "    return nullptr;",
                "}",
                "",
                "} // namespace apusr::backends::fsr4_reference_dx12_shader_blobs",
                "",
            ]
        ),
        encoding="utf-8",
        newline="\n",
    )


def write_packaged_cache(cache_root: Path, preset: str, capacity: str, permutation: int, compiled: list[tuple[ShaderJob, bytes]]) -> None:
    output_dir = cache_root / preset / capacity / f"p{permutation}"
    output_dir.mkdir(parents=True, exist_ok=True)
    for job, data in compiled:
        (output_dir / f"{job.name}.dxil").write_bytes(data)
    (output_dir / "manifest.txt").write_text(
        "\n".join(job.name for job, _ in compiled) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"[fsr4-reference-dxil] wrote packaged cache {output_dir}")


def write_source(source: Path, preset: str, capacity: str, permutation: int, compiled: list[tuple[ShaderJob, bytes]]) -> None:
    source.parent.mkdir(parents=True, exist_ok=True)
    preset_enum = PRESET_ENUM[preset]
    capacity_enum = CAPACITY_ENUM[capacity]
    parts = [
        "// SPDX-License-Identifier: MIT",
        "// Copyright (C) 2025 Advanced Micro Devices, Inc.",
        "// Copyright (c) 2026 thomthehound.",
        "// Derived payload from the MIT-licensed FidelityFX SDK 2.0.0 / FSR 4.0.2 shader sources.",
        "// Auto-generated by tools/model/compile_fsr4_reference_dx12_shaders.py.",
        "",
        "#include <apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h>",
        "",
        "#include <cstring>",
        "",
        "namespace apusr::backends::fsr4_reference_dx12_shader_blobs {",
        "namespace {",
        "",
    ]
    for job, data in compiled:
        parts.append(f"alignas(16) const std::uint8_t {symbol_name(job)}[] = {{")
        parts.append(c_array_initializer(data))
        parts.append("};")
        parts.append("")

    parts.append("const ShaderBlob kShaderBlobs[] = {")
    for job, _data in compiled:
        kind_enum = PASS_KIND_ENUM[job.kind]
        parts.append(
            f"    ShaderBlob{{\"{job.name}\", models::fsr4_int8::Preset::{preset_enum}, "
            f"models::fsr4_int8::FrameCapacity::{capacity_enum}, {permutation}u, models::fsr4_int8::PassKind::{kind_enum}, "
            f"{job.logical_stage}u, {symbol_name(job)}, sizeof({symbol_name(job)})}},"
        )
    parts.extend(
        [
            "};",
            "",
            "} // namespace",
            "",
            "std::size_t shaderBlobCount() noexcept",
            "{",
            "    return sizeof(kShaderBlobs) / sizeof(kShaderBlobs[0]);",
            "}",
            "",
            "const ShaderBlob* shaderBlobAt(const std::size_t index) noexcept",
            "{",
            "    return index < shaderBlobCount() ? &kShaderBlobs[index] : nullptr;",
            "}",
            "",
            "const ShaderBlob* findShaderBlob(",
            "    const models::fsr4_int8::Preset preset,",
            "    const models::fsr4_int8::FrameCapacity frameCapacity,",
            "    const std::uint32_t permutationKey,",
            "    const char* dispatchName) noexcept",
            "{",
            "    if (dispatchName == nullptr) {",
            "        return nullptr;",
            "    }",
            "    for (const ShaderBlob& blob : kShaderBlobs) {",
            "        if (blob.preset == preset && blob.frameCapacity == frameCapacity && blob.permutationKey == permutationKey && std::strcmp(blob.dispatchName, dispatchName) == 0) {",
            "            return &blob;",
            "        }",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "} // namespace apusr::backends::fsr4_reference_dx12_shader_blobs",
            "",
        ]
    )
    source.write_text("\n".join(parts), encoding="utf-8", newline="\n")


def compile_slice(repo_root: Path, compiler_dir: Path, dxc: Path, preset: str, capacity: str, options: PermutationOptions, timeout_seconds: int, dp4a_optimized_pass1_tiled_exact: bool = False, dp4a_optimized_front_c16_tiled_exact: bool = False) -> tuple[int, list[tuple[ShaderJob, bytes]]]:
    jobs = shader_jobs(
        preset,
        capacity,
        dp4a_optimized_pass1_tiled_exact,
        dp4a_optimized_front_c16_tiled_exact,
        options.debug_visualize,
    )
    compiled: list[tuple[ShaderJob, bytes]] = []
    base_model_defines = model_defines(capacity, options)
    with tempfile.TemporaryDirectory(prefix="apusr_fsr4_reference_dxil_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        for job in jobs:
            dxil_path = temp_dir / f"{job.name}.dxil"
            compile_shader(repo_root, compiler_dir, dxc, job, dxil_path, timeout_seconds, base_model_defines)
            compiled.append((job, dxil_path.read_bytes()))
    return permutation_key(preset, capacity, options), compiled


def product_matrix_options(include_debug: bool) -> list[PermutationOptions]:
    out: list[PermutationOptions] = []
    for color_space in ("linear", "nonlinear", "srgb", "pq"):
        for depth_inverted in (False, True):
            for low_res_motion_vectors in (False, True):
                for auto_exposure in (False, True):
                    for jittered_motion_vectors in (False, True):
                        debug_values = (False, True) if include_debug else (False,)
                        for debug_visualize in debug_values:
                            out.append(PermutationOptions(
                                wmma=False,
                                depth_inverted=depth_inverted,
                                low_res_motion_vectors=low_res_motion_vectors,
                                auto_exposure=auto_exposure,
                                jittered_motion_vectors=jittered_motion_vectors,
                                color_space=color_space,
                                debug_visualize=debug_visualize,
                            ))
    return out



def effective_options_for_job(job: ShaderJob, options: PermutationOptions) -> PermutationOptions:
    """Collapse global runtime options to the subset that can affect this shader.

    FFX does not specialize every shader by every FSR4 runtime option.  In the
    DP4A INT8 reference path, the heavy middle passes are keyed only by model preset/capacity
    and pass number; recompiling them for depth/MV/exposure/colorspace variants
    is pure packaging waste.  This normalization keeps APUSR's runtime-facing
    global permutation key intact while avoiding redundant DXC work during
    packaged-cache generation.
    """
    if job.name == "spd_auto_exposure":
        return PermutationOptions(False, False, False, False, False, "linear", False)
    if job.name.startswith("fsr4_pass_"):
        return PermutationOptions(options.wmma, False, False, False, False, "linear", False)
    exposure_specialized = options.auto_exposure if options.color_space == "linear" else False
    if job.name == "rcas":
        return PermutationOptions(options.wmma, False, False, exposure_specialized, False, options.color_space, False)
    if job.name == "fsr4_post":
        return PermutationOptions(options.wmma, False, False, exposure_specialized, False, options.color_space, options.debug_visualize)
    if job.name == "fsr4_pre":
        return PermutationOptions(
            options.wmma,
            options.depth_inverted if options.low_res_motion_vectors else False,
            options.low_res_motion_vectors,
            exposure_specialized,
            options.jittered_motion_vectors,
            options.color_space,
            False,
        )
    return options


def generation_options(
    requested: PermutationOptions,
    product_matrix: bool,
    include_debug: bool,
) -> list[PermutationOptions]:
    """Return the exact requested slice unless the full product matrix is explicit."""
    return product_matrix_options(include_debug) if product_matrix else [requested]


def effective_capacity_for_job(job: ShaderJob, capacity: str) -> str:
    if job.name in ("spd_auto_exposure", "rcas"):
        return "1080"
    return capacity


def effective_preset_for_job(job: ShaderJob, preset: str) -> str:
    if job.name in ("spd_auto_exposure", "rcas"):
        return "balanced"
    return preset


@dataclass
class PackagedCompileTask:
    identity: tuple[object, ...]
    preset: str
    capacity: str
    options: PermutationOptions
    job: ShaderJob
    output_paths: list[Path]


def worker_count(requested: int) -> int:
    if requested and requested > 0:
        return requested
    return max(1, os.cpu_count() or 1)


def link_or_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        destination.unlink()
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def compile_task_to_temp(repo_root: Path, compiler_dir: Path, dxc: Path, task: PackagedCompileTask, timeout_seconds: int) -> bytes:
    with tempfile.TemporaryDirectory(prefix="apusr_fsr4_reference_dxil_task_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        dxil_path = temp_dir / f"{task.job.name}.dxil"
        compile_shader(repo_root, compiler_dir, dxc, task.job, dxil_path, timeout_seconds, model_defines(task.capacity, task.options))
        return dxil_path.read_bytes()


def existing_blob_for_task(task: PackagedCompileTask) -> Path | None:
    for path in task.output_paths:
        if path.is_file() and path.stat().st_size > 0:
            return path
    return None


def materialize_task_outputs(task: PackagedCompileTask, data: bytes) -> None:
    first = task.output_paths[0]
    first.parent.mkdir(parents=True, exist_ok=True)
    first.write_bytes(data)
    for path in task.output_paths[1:]:
        if path == first:
            continue
        link_or_copy(first, path)


def ensure_task_outputs_from_existing(task: PackagedCompileTask, existing: Path) -> None:
    for path in task.output_paths:
        if path.is_file() and path.stat().st_size > 0:
            continue
        link_or_copy(existing, path)


def build_packaged_product_tasks(presets: list[str], capacities: list[str], options_list: list[PermutationOptions], cache_root: Path, dp4a_optimized_pass1_tiled_exact: bool = False, dp4a_optimized_front_c16_tiled_exact: bool = False) -> tuple[list[PackagedCompileTask], dict[Path, list[str]], int, int]:
    tasks: dict[tuple[object, ...], PackagedCompileTask] = {}
    slice_manifests: dict[Path, list[str]] = {}
    full_slices = 0
    cache_entries = 0
    for selected_options in options_list:
        for selected_preset in presets:
            for selected_capacity in capacities:
                full_slices += 1
                full_key = permutation_key(selected_preset, selected_capacity, selected_options)
                output_dir = cache_root / selected_preset / selected_capacity / f"p{full_key}"
                jobs = shader_jobs(
                    selected_preset,
                    selected_capacity,
                    dp4a_optimized_pass1_tiled_exact,
                    dp4a_optimized_front_c16_tiled_exact,
                    selected_options.debug_visualize,
                )
                slice_manifests[output_dir] = [job.name for job in jobs]
                for job in jobs:
                    cache_entries += 1
                    effective_options = effective_options_for_job(job, selected_options)
                    effective_preset = effective_preset_for_job(job, selected_preset)
                    effective_capacity = effective_capacity_for_job(job, selected_capacity)
                    effective_job = job
                    if effective_preset != selected_preset:
                        # Rebuild utility/common jobs through a canonical preset object so the source path is stable.
                        effective_job = next(
                            j
                            for j in shader_jobs(
                                effective_preset,
                                effective_capacity,
                                dp4a_optimized_pass1_tiled_exact,
                                dp4a_optimized_front_c16_tiled_exact,
                                effective_options.debug_visualize,
                            )
                            if j.name == job.name
                        )
                    identity = (
                        effective_job.name,
                        effective_preset,
                        effective_capacity,
                        effective_options,
                        effective_job.source.as_posix(),
                        effective_job.entry,
                        effective_job.extra_defines,
                        effective_job.use_model_defines,
                    )
                    task = tasks.get(identity)
                    if task is None:
                        task = PackagedCompileTask(identity, effective_preset, effective_capacity, effective_options, effective_job, [])
                        tasks[identity] = task
                    task.output_paths.append(output_dir / f"{job.name}.dxil")
    return list(tasks.values()), slice_manifests, full_slices, cache_entries


def write_slice_manifests(slice_manifests: dict[Path, list[str]]) -> None:
    for output_dir, names in slice_manifests.items():
        output_dir.mkdir(parents=True, exist_ok=True)
        (output_dir / "manifest.txt").write_text("\n".join(names) + "\n", encoding="utf-8", newline="\n")



@dataclass
class EmbeddedLogicalEntry:
    dispatch_name: str
    preset: str
    capacity: str
    permutation: int
    kind: str
    logical_stage: int
    identity: tuple[object, ...]


def embedded_symbol_for_identity(identity: tuple[object, ...]) -> str:
    digest = hashlib.sha1(repr(identity).encode("utf-8")).hexdigest()[:16]
    job_name = str(identity[0]).replace("-", "_")
    return "kDxil_" + "".join(part.capitalize() for part in job_name.split("_")) + "_" + digest


def build_embedded_product_tasks(presets: list[str], capacities: list[str], options_list: list[PermutationOptions], dp4a_optimized_pass1_tiled_exact: bool = False, dp4a_optimized_front_c16_tiled_exact: bool = False) -> tuple[list[PackagedCompileTask], list[EmbeddedLogicalEntry]]:
    tasks: dict[tuple[object, ...], PackagedCompileTask] = {}
    entries: list[EmbeddedLogicalEntry] = []
    for selected_options in options_list:
        for selected_preset in presets:
            for selected_capacity in capacities:
                full_key = permutation_key(selected_preset, selected_capacity, selected_options)
                for job in shader_jobs(
                    selected_preset,
                    selected_capacity,
                    dp4a_optimized_pass1_tiled_exact,
                    dp4a_optimized_front_c16_tiled_exact,
                    selected_options.debug_visualize,
                ):
                    effective_options = effective_options_for_job(job, selected_options)
                    effective_preset = effective_preset_for_job(job, selected_preset)
                    effective_capacity = effective_capacity_for_job(job, selected_capacity)
                    effective_job = job
                    if effective_preset != selected_preset:
                        effective_job = next(
                            j
                            for j in shader_jobs(
                                effective_preset,
                                effective_capacity,
                                dp4a_optimized_pass1_tiled_exact,
                                dp4a_optimized_front_c16_tiled_exact,
                                effective_options.debug_visualize,
                            )
                            if j.name == job.name
                        )
                    identity = (
                        effective_job.name,
                        effective_preset,
                        effective_capacity,
                        effective_options,
                        effective_job.source.as_posix(),
                        effective_job.entry,
                        effective_job.extra_defines,
                        effective_job.use_model_defines,
                    )
                    task = tasks.get(identity)
                    if task is None:
                        task = PackagedCompileTask(identity, effective_preset, effective_capacity, effective_options, effective_job, [])
                        tasks[identity] = task
                    entries.append(EmbeddedLogicalEntry(
                        dispatch_name=job.name,
                        preset=selected_preset,
                        capacity=selected_capacity,
                        permutation=full_key,
                        kind=job.kind,
                        logical_stage=job.logical_stage,
                        identity=identity,
                    ))
    return list(tasks.values()), entries


def write_embedded_product_source(source: Path, tasks: list[PackagedCompileTask], entries: list[EmbeddedLogicalEntry], compiled_by_identity: dict[tuple[object, ...], bytes]) -> None:
    source.parent.mkdir(parents=True, exist_ok=True)
    parts = [
        "// SPDX-License-Identifier: MIT",
        "// Copyright (C) 2025 Advanced Micro Devices, Inc.",
        "// Copyright (c) 2026 thomthehound.",
        "// Derived payload from the MIT-licensed FidelityFX SDK 2.0.0 / FSR 4.0.2 shader sources.",
        "// Auto-generated by tools/model/compile_fsr4_reference_dx12_shaders.py.",
        "// Embedded full-product FSR4 INT8 DX12 reference shader cache.",
        "",
        "#include <apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h>",
        "",
        "#include <cstring>",
        "",
        "namespace apusr::backends::fsr4_reference_dx12_shader_blobs {",
        "namespace {",
        "",
    ]
    for task in tasks:
        data = compiled_by_identity[task.identity]
        parts.append(f"alignas(16) const std::uint8_t {embedded_symbol_for_identity(task.identity)}[] = {{")
        parts.append(c_array_initializer(data))
        parts.append("};")
        parts.append("")

    parts.append("const ShaderBlob kShaderBlobs[] = {")
    for entry in entries:
        symbol = embedded_symbol_for_identity(entry.identity)
        parts.append(
            f"    ShaderBlob{{\"{entry.dispatch_name}\", models::fsr4_int8::Preset::{PRESET_ENUM[entry.preset]}, "
            f"models::fsr4_int8::FrameCapacity::{CAPACITY_ENUM[entry.capacity]}, {entry.permutation}u, "
            f"models::fsr4_int8::PassKind::{PASS_KIND_ENUM[entry.kind]}, {entry.logical_stage}u, {symbol}, sizeof({symbol})}},"
        )
    parts.extend([
        "};",
        "",
        "} // namespace",
        "",
        "std::size_t shaderBlobCount() noexcept",
        "{",
        "    return sizeof(kShaderBlobs) / sizeof(kShaderBlobs[0]);",
        "}",
        "",
        "const ShaderBlob* shaderBlobAt(const std::size_t index) noexcept",
        "{",
        "    return index < shaderBlobCount() ? &kShaderBlobs[index] : nullptr;",
        "}",
        "",
        "const ShaderBlob* findShaderBlob(",
        "    const models::fsr4_int8::Preset preset,",
        "    const models::fsr4_int8::FrameCapacity frameCapacity,",
        "    const std::uint32_t permutationKey,",
        "    const char* dispatchName) noexcept",
        "{",
        "    if (dispatchName == nullptr) {",
        "        return nullptr;",
        "    }",
        "    for (const ShaderBlob& blob : kShaderBlobs) {",
        "        if (blob.preset == preset && blob.frameCapacity == frameCapacity && blob.permutationKey == permutationKey && std::strcmp(blob.dispatchName, dispatchName) == 0) {",
        "            return &blob;",
        "        }",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "} // namespace apusr::backends::fsr4_reference_dx12_shader_blobs",
        "",
    ])
    source.write_text("\n".join(parts), encoding="utf-8", newline="\n")


def generate_embedded_product_source(repo_root: Path, preset: str, capacity: str, timeout_seconds: int, source: Path, product_matrix: bool, include_debug: bool, options: PermutationOptions, jobs: int, force: bool, dp4a_optimized_pass1_tiled_exact: bool = False, dp4a_optimized_front_c16_tiled_exact: bool = False) -> None:
    del force  # Reserved for future intermediate-cache reuse; embedded generation always writes the requested source.
    presets = selected_presets(preset)
    capacities = selected_capacities(capacity)
    options_list = generation_options(options, product_matrix, include_debug)
    compiler_dir, dxc = compiler_paths(repo_root)
    tasks, entries = build_embedded_product_tasks(list(presets), list(capacities), options_list, dp4a_optimized_pass1_tiled_exact, dp4a_optimized_front_c16_tiled_exact)
    workers = worker_count(jobs)
    print(
        f"[fsr4-reference-dxil] embedded generation: logical_entries={len(entries)} unique_compile_jobs={len(tasks)} jobs={workers}",
        flush=True,
    )
    compiled_by_identity: dict[tuple[object, ...], bytes] = {}
    completed = 0
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        future_to_task = {pool.submit(compile_task_to_temp, repo_root, compiler_dir, dxc, task, timeout_seconds): task for task in tasks}
        for future in as_completed(future_to_task):
            task = future_to_task[future]
            try:
                compiled_by_identity[task.identity] = future.result()
            except Exception as exc:  # noqa: BLE001 - surface the task identity in generation logs.
                raise SystemExit(f"Failed compiling embedded shader job {task.identity}: {exc}") from exc
            completed += 1
            print(
                f"[fsr4-reference-dxil] [{completed}/{len(tasks)}] embedded {task.job.name} elapsed={time.monotonic() - started:.1f}s",
                flush=True,
            )
    write_embedded_product_source(source, tasks, entries, compiled_by_identity)
    print(f"[fsr4-reference-dxil] wrote embedded shader table {source}", flush=True)


def generate_product_packaged_cache(repo_root: Path, preset: str, capacity: str, timeout_seconds: int, cache_root: Path, product_matrix: bool, include_debug: bool, options: PermutationOptions, jobs: int, force: bool, dp4a_optimized_pass1_tiled_exact: bool = False, dp4a_optimized_front_c16_tiled_exact: bool = False) -> None:
    presets = selected_presets(preset)
    capacities = selected_capacities(capacity)
    options_list = generation_options(options, product_matrix, include_debug)
    compiler_dir, dxc = compiler_paths(repo_root)
    tasks, slice_manifests, full_slices, cache_entries = build_packaged_product_tasks(presets, capacities, options_list, cache_root, dp4a_optimized_pass1_tiled_exact, dp4a_optimized_front_c16_tiled_exact)
    write_slice_manifests(slice_manifests)
    workers = worker_count(jobs)
    print(
        f"[fsr4-reference-dxil] packaged generation: full_slices={full_slices} cache_entries={cache_entries} unique_compile_jobs={len(tasks)} jobs={workers} force={force}",
        flush=True,
    )

    skipped = 0
    pending: list[PackagedCompileTask] = []
    for task in tasks:
        existing = existing_blob_for_task(task)
        if existing is not None and not force:
            ensure_task_outputs_from_existing(task, existing)
            skipped += 1
        else:
            pending.append(task)
    if skipped:
        print(f"[fsr4-reference-dxil] reused {skipped} already-built unique shader job(s)", flush=True)
    if not pending:
        print(f"[fsr4-reference-dxil] packaged cache already complete: {cache_root}", flush=True)
        return

    completed = 0
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        future_to_task = {pool.submit(compile_task_to_temp, repo_root, compiler_dir, dxc, task, timeout_seconds): task for task in pending}
        for future in as_completed(future_to_task):
            task = future_to_task[future]
            try:
                data = future.result()
            except Exception as exc:  # noqa: BLE001 - surface the task identity in packaging logs.
                raise SystemExit(f"Failed compiling packaged shader job {task.identity}: {exc}") from exc
            materialize_task_outputs(task, data)
            completed += 1
            print(
                f"[fsr4-reference-dxil] [{completed}/{len(pending)}] materialized {task.job.name} outputs={len(task.output_paths)} elapsed={time.monotonic() - started:.1f}s",
                flush=True,
            )
    print(f"[fsr4-reference-dxil] completed packaged cache at {cache_root}", flush=True)


def generate(repo_root: Path, preset: str, capacity: str, options: PermutationOptions, header: Path, source: Path, timeout_seconds: int, cache_root: Path | None = None, cache_only: bool = False, product_matrix: bool = False, include_debug: bool = False, jobs: int = 0, force: bool = False, dp4a_optimized_pass1_tiled_exact: bool = False, dp4a_optimized_front_c16_tiled_exact: bool = False) -> None:
    presets = selected_presets(preset)
    capacities = selected_capacities(capacity)
    options_list = generation_options(options, product_matrix, include_debug)
    multi_slice = len(presets) != 1 or len(capacities) != 1 or len(options_list) != 1
    if product_matrix and options.wmma:
        raise SystemExit("--product-matrix currently declares the DP4A INT8 reference product path; WMMA needs a separate backend matrix")

    write_header(header)
    if cache_only:
        write_empty_source(source)

    if cache_only and cache_root is not None:
        generate_product_packaged_cache(repo_root, preset, capacity, timeout_seconds, cache_root, product_matrix, include_debug, options, jobs, force, dp4a_optimized_pass1_tiled_exact, dp4a_optimized_front_c16_tiled_exact)
        return

    if multi_slice:
        generate_embedded_product_source(repo_root, preset, capacity, timeout_seconds, source, product_matrix, include_debug, options, jobs, force, dp4a_optimized_pass1_tiled_exact, dp4a_optimized_front_c16_tiled_exact)
        return

    compiler_dir, dxc = compiler_paths(repo_root)

    total_blobs = 0
    total_slices = 0
    first_embedded_written = cache_only
    for selected_options in options_list:
        for selected_preset in presets:
            for selected_capacity in capacities:
                key, compiled = compile_slice(repo_root, compiler_dir, dxc, selected_preset, selected_capacity, selected_options, timeout_seconds, dp4a_optimized_pass1_tiled_exact, dp4a_optimized_front_c16_tiled_exact)
                if cache_root is not None:
                    write_packaged_cache(cache_root, selected_preset, selected_capacity, key, compiled)
                if not cache_only:
                    if first_embedded_written:
                        raise SystemExit("embedded C++ shader blob generation supports one preset/capacity/permutation slice; use --cache-only for multi-slice packaged caches")
                    write_source(source, selected_preset, selected_capacity, key, compiled)
                    first_embedded_written = True
                total_blobs += len(compiled)
                total_slices += 1
                print(f"[fsr4-reference-dxil] generated {len(compiled)} shader blob(s) for preset={selected_preset} capacity={selected_capacity} permutationKey={key}")
    print(f"[fsr4-reference-dxil] completed {total_slices} slice(s), {total_blobs} shader blob(s)")

def options_from_args(args: argparse.Namespace) -> PermutationOptions:
    return PermutationOptions(
        wmma=args.wmma,
        depth_inverted=args.depth_inverted,
        low_res_motion_vectors=args.low_res_motion_vectors,
        auto_exposure=args.auto_exposure,
        jittered_motion_vectors=args.jittered_motion_vectors,
        color_space=args.color_space,
        debug_visualize=args.debug_visualize,
    )


def main() -> None:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    header = args.header.resolve()
    source = args.source.resolve()
    options = options_from_args(args)
    if args.verify:
        with tempfile.TemporaryDirectory(prefix="apusr_fsr4_reference_blob_verify_") as temp_dir_name:
            temp_dir = Path(temp_dir_name)
            generated_header = temp_dir / header.name
            generated_source = temp_dir / source.name
            generate(repo_root, args.preset, args.capacity, options, generated_header, generated_source, args.shader_timeout_seconds, cache_only=args.cache_only, product_matrix=args.product_matrix, include_debug=args.include_debug, jobs=args.jobs, force=args.force, dp4a_optimized_pass1_tiled_exact=args.dp4a_optimized_pass1_tiled_exact, dp4a_optimized_front_c16_tiled_exact=args.dp4a_optimized_front_c16_tiled_exact)
            if header.read_text(encoding="utf-8") != generated_header.read_text(encoding="utf-8"):
                raise SystemExit("FSR4 reference DX12 shader blob header is stale. Regenerate with tools/model/compile_fsr4_reference_dx12_shaders.py.")
            if source.read_text(encoding="utf-8") != generated_source.read_text(encoding="utf-8"):
                raise SystemExit("FSR4 reference DX12 shader blob source is stale. Regenerate with tools/model/compile_fsr4_reference_dx12_shaders.py.")
        return
    generate(repo_root, args.preset, args.capacity, options, header, source, args.shader_timeout_seconds, args.cache_root.resolve() if args.cache_root is not None else None, args.cache_only, args.product_matrix, args.include_debug, args.jobs, args.force, args.dp4a_optimized_pass1_tiled_exact, args.dp4a_optimized_front_c16_tiled_exact)


if __name__ == "__main__":
    main()
