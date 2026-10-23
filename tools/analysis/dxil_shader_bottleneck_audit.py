# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Compile selected FSR4 INT8 shaders with dxc and summarize static DXIL hot-path signals.

This is a static audit helper, not a GPU timing profiler.  It is meant to make
machine-written HLSL easier to compare while deciding which dispatches deserve
RGP/PIX or in-engine timestamp profiling.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class ShaderJob:
    name: str
    source: pathlib.Path
    entry: str
    defines: tuple[str, ...]
    logical_stage: int | None = None


PRESET_CAPACITY_DEFINE = {
    "native": "APUSR_FSR4_I8_PRESET_NATIVE",
    "quality": "APUSR_FSR4_I8_PRESET_QUALITY",
    "balanced": "APUSR_FSR4_I8_PRESET_BALANCED",
    "performance": "APUSR_FSR4_I8_PRESET_PERFORMANCE",
    "ultraperf": "APUSR_FSR4_I8_PRESET_ULTRAPERF",
    "drs": "APUSR_FSR4_I8_PRESET_DRS",
}

CAPACITY_DEFINE = {
    "1080": "APUSR_FSR4_I8_CAPACITY_1080",
    "2160": "APUSR_FSR4_I8_CAPACITY_2160",
    "4320": "APUSR_FSR4_I8_CAPACITY_4320",
}

UNARY_OPS = {
    6: "Round_ne",
    13: "FMax",
    14: "FMin",
    21: "Exp",
    22: "Frc",
    23: "Log",
    24: "Sqrt",
    25: "Rsqrt",
    26: "Sin",
    27: "Cos",
    28: "Tanh",
}


def repo_default_dxc(repo: pathlib.Path) -> pathlib.Path:
    return repo / "tools" / "shader_compiler" / "linux" / "bin" / "dxc"


def common_defines() -> list[str]:
    return [
        "FFX_GPU=1",
        "FFX_HLSL=1",
        "FFX_HALF=1",
        "FFX_HLSL_SM=66",
        "FFX_MLSR_RESOLUTION=1",
        "WMMA_ENABLED=0",
        "DOT4_ENABLED=1",
        "FFX_MLSR_DEPTH_INVERTED=1",
        "FFX_MLSR_LOW_RES_MV=1",
        "FFX_MLSR_JITTERED_MOTION_VECTORS=0",
        "FFX_MLSR_AUTOEXPOSURE_ENABLED=1",
        "FFX_MLSR_COLORSPACE=0",
        "FFX_DEBUG_VISUALIZE=0",
    ]


def selected_jobs(repo: pathlib.Path, preset: str, capacity: str, full_middle: bool) -> list[ShaderJob]:
    model = repo / "models" / "fsr4_int8" / "reference" / "fsr4_4_0_2_int8"
    passes = model / preset / f"passes_{capacity}.hlsl"
    post = model / preset / "post.hlsl"
    optimized = repo / "shaders" / "dx12" / "optimized"
    preset_define = PRESET_CAPACITY_DEFINE[preset]
    capacity_define = CAPACITY_DEFINE[capacity]

    jobs: list[ShaderJob] = []
    middle_range = range(1, 13) if full_middle else range(1, 3)
    for stage in middle_range:
        jobs.append(
            ShaderJob(
                name=f"fsr4_pass_{stage}_ref",
                source=passes,
                entry=f"fsr4_model_v07_i8_pass{stage}",
                defines=(f"MLSR_PASS_{stage}=1",),
                logical_stage=stage,
            )
        )

    jobs.extend([
        ShaderJob(
            name="fsr4_pass_12_lds_cnb_streamed",
            source=passes,
            entry="fsr4_model_v07_i8_pass12",
            defines=(
                "MLSR_PASS_12=1",
                "APUSR_FSR4_I8_CNB_LDS_C16_HALO=1",
                "APUSR_FSR4_I8_CNB_STREAMED_HIDDEN_CONTRACT=1",
            ),
            logical_stage=12,
        ),
        ShaderJob(
            name="fsr4_pass_1_tiled_exact",
            source=optimized / "apusr_fsr4_i8_pass1_tiled_exact.hlsl",
            entry="apusr_fsr4_i8_pass1_tiled_exact",
            defines=(preset_define + "=1", capacity_define + "=1"),
            logical_stage=1,
        ),
        ShaderJob(
            name="fsr4_pass_2_tiled_exact",
            source=optimized / "apusr_fsr4_i8_pass2_tiled_exact.hlsl",
            entry="apusr_fsr4_i8_pass2_tiled_exact",
            defines=(preset_define + "=1", capacity_define + "=1"),
            logical_stage=2,
        ),
        ShaderJob(
            name="fsr4_post_ref",
            source=post,
            entry="main",
            defines=(),
            logical_stage=13,
        ),
        ShaderJob(
            name="fsr4_post_lds_cnb_streamed",
            source=post,
            entry="main",
            defines=("APUSR_FSR4_I8_POST_LDS_CNB=1", "APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT=1"),
            logical_stage=13,
        ),
        ShaderJob(
            name="fsr4_post_tail_cache",
            source=post,
            entry="main",
            defines=(
                "APUSR_FSR4_I8_POST_LDS_CNB=1",
                "APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT=1",
                "APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT=1",
                "APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA=1",
            ),
            logical_stage=13,
        ),
        ShaderJob(
            name="fsr4_post_tail_cache_paired_ct2d",
            source=post,
            entry="main",
            defines=(
                "APUSR_FSR4_I8_POST_LDS_CNB=1",
                "APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT=1",
                "APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT=1",
                "APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA=1",
                "APUSR_FSR4_I8_POST_PAIRED_CT2D=1",
            ),
            logical_stage=13,
        ),
    ])
    return jobs


def round_up_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor if divisor else 0


def dispatch_thread_estimate(stage: int | None, width: int, height: int, optimized: bool) -> int:
    if stage is None or width <= 0 or height <= 0:
        return 0
    if stage in (1, 2):
        ew = width >> 1
        eh = height >> 1
        if optimized:
            return round_up_div(ew, 64) * round_up_div(eh, 2) * 64 * 2
        return round_up_div(ew, 64) * eh * 64
    if stage == 12:
        ew = width >> 1
        eh = height >> 1
        return round_up_div(ew, 64) * eh * 64
    if stage in (3, 4, 5, 10, 11):
        ew = width >> 2
        eh = height >> 2
        return round_up_div(ew, 64) * eh * 64
    if stage in (6, 7, 8):
        ew = width >> 3
        eh = height >> 3
        return round_up_div(ew, 64) * eh * 64
    if stage == 9:
        ew = width >> 3
        eh = height >> 3
        return round_up_div(ew, 8) * round_up_div(eh, 8) * 64
    if stage == 13:
        return round_up_div(width, 16) * round_up_div(height, 16) * 64
    return 0

def run_dxc(repo: pathlib.Path, dxc: pathlib.Path, out_dir: pathlib.Path, job: ShaderJob, preset: str) -> pathlib.Path:
    model = repo / "models" / "fsr4_int8" / "reference" / "fsr4_4_0_2_int8"
    asm_path = out_dir / f"{job.name}.asm"
    dxil_path = out_dir / f"{job.name}.dxil"
    cmd = [
        str(dxc),
        "-T",
        "cs_6_6",
        "-HV",
        "2021",
        "-enable-16bit-types",
        "-O3",
        "-I",
        str(model / preset),
        "-I",
        str(model / "common"),
        "-I",
        str(model / "include"),
        "-I",
        str(model / "include" / "fsr4"),
    ]
    for define in common_defines():
        cmd.extend(["-D", define])
    for define in job.defines:
        cmd.extend(["-D", define])
    cmd.extend(["-E", job.entry, str(job.source), "-Fo", str(dxil_path), "-Fc", str(asm_path)])

    env = os.environ.copy()
    dxc_dir = str(dxc.parent)
    env["LD_LIBRARY_PATH"] = dxc_dir + (os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    subprocess.run(cmd, check=True, cwd=repo, env=env)
    return asm_path


def count_pattern(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text))


def analyze_asm(path: pathlib.Path, job: ShaderJob, width: int, height: int) -> dict[str, int | str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    dispatch_threads = dispatch_thread_estimate(job.logical_stage, width, height, "tiled_exact" in job.name)
    result: dict[str, int | str] = {
        "shader": path.stem,
        "logical_stage": job.logical_stage if job.logical_stage is not None else "",
        "dispatch_thread_estimate": dispatch_threads,
        "asm_lines": text.count("\n") + 1,
        "dp4a_static_lines": count_pattern(text, r"Dot4AddI8Packed"),
        "raw_buffer_load_lines": count_pattern(text, r"rawBufferLoad"),
        "raw_buffer_store_lines": count_pattern(text, r"rawBufferStore"),
        "buffer_load_lines": count_pattern(text, r"bufferLoad"),
        "buffer_store_lines": count_pattern(text, r"bufferStore"),
        "texture_load_lines": count_pattern(text, r"textureLoad"),
        "texture_store_lines": count_pattern(text, r"textureStore"),
        "unary_f32_lines": count_pattern(text, r"unary\.f32"),
    }
    for opcode, name in UNARY_OPS.items():
        result[f"unary_{name.lower()}_lines"] = count_pattern(text, rf"unary\.f32\(i32 {opcode},")
    result["dp4a_dynamic_estimate"] = int(result["dp4a_static_lines"]) * dispatch_threads
    return result


def write_csv(path: pathlib.Path, rows: Iterable[dict[str, int | str]]) -> None:
    rows = list(rows)
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--preset", choices=sorted(PRESET_CAPACITY_DEFINE), default="balanced")
    parser.add_argument("--capacity", choices=sorted(CAPACITY_DEFINE), default="2160")
    parser.add_argument("--dxc", type=pathlib.Path, default=None)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build") / "shader_audit")
    parser.add_argument("--csv", type=pathlib.Path, default=None)
    parser.add_argument("--full-middle", action="store_true", help="also compile generated middle passes 2-12")
    parser.add_argument("--display-width", type=int, default=3840, help="display width for dynamic dispatch-thread estimates")
    parser.add_argument("--display-height", type=int, default=2160, help="display height for dynamic dispatch-thread estimates")
    args = parser.parse_args(argv)

    repo = args.repo.resolve()
    dxc = (args.dxc or repo_default_dxc(repo)).resolve()
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    if not dxc.is_file():
        raise SystemExit(f"dxc not found: {dxc}")

    rows = []
    for job in selected_jobs(repo, args.preset, args.capacity, args.full_middle):
        asm = run_dxc(repo, dxc, out_dir, job, args.preset)
        rows.append(analyze_asm(asm, job, args.display_width, args.display_height))

    csv_path = (args.csv or (out_dir / "shader_audit.csv")).resolve()
    write_csv(csv_path, rows)
    print(f"wrote {csv_path}")
    for row in rows:
        print(
            f"{row['shader']}: dp4a={row['dp4a_static_lines']} unary={row['unary_f32_lines']} "
            f"threads~{row['dispatch_thread_estimate']} dynDp4a~{int(row['dp4a_dynamic_estimate'])/1e9:.3f}B "
            f"rawLoad={row['raw_buffer_load_lines']} texLoad={row['texture_load_lines']} texStore={row['texture_store_lines']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
