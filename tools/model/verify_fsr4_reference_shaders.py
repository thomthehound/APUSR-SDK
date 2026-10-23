# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Compile selected slices of the imported FSR4 INT8 reference shader matrix.

This is a preflight tool for the APUSR DX12 reference backend.  It can compile a
single preset/capacity/permutation slice for fast iteration or a quick matrix
across all presets/capacities to make sure the reference runner is not quietly
becoming a Balanced/R2160-only fixture.
"""

from __future__ import annotations

import argparse
import os
import platform
import signal
import stat
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path


PRESETS = ("quality", "balanced", "performance", "ultraperf", "native", "drs")
CAPACITIES = ("1080", "2160", "4320")
CAPACITY_VALUE = {"1080": 0, "2160": 1, "4320": 2}


@dataclass(frozen=True)
class ShaderJob:
    name: str
    source: Path
    entry: str
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


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--preset", default="balanced", choices=("all", *PRESETS))
    parser.add_argument("--capacity", default="2160", choices=("all", *CAPACITIES))
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument("--jobs", type=int, default=0, help="Parallel DXC workers. 0 uses the host CPU count.")
    parser.add_argument("--wmma", action="store_true")
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
        "--quick",
        action="store_true",
        help="Compile only pre, pass1, and post per preset/capacity slice. Suitable for short developer preflights.",
    )
    parser.add_argument(
        "--include-utility",
        action="store_true",
        help="Also compile the reference SPD auto-exposure and RCAS shaders for each slice.",
    )
    return parser.parse_args()


def color_space_value(color_space: str) -> int:
    # Mirrors ffxint8 GetColorspace(): linear=0, nonlinear=1, sRGB=2, PQ=3.
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


def shader_jobs(preset: str, capacity: str, quick: bool, include_utility: bool) -> list[ShaderJob]:
    root = Path("models/fsr4_int8/reference/fsr4_4_0_2_int8") / preset
    passes = root / f"passes_{capacity}.hlsl"
    jobs = [ShaderJob("pre", root / "pre.hlsl", "main")]
    stages = (1,) if quick else tuple(range(1, 13))
    for stage in stages:
        jobs.append(
            ShaderJob(
                f"pass{stage}",
                passes,
                f"fsr4_model_v07_i8_pass{stage}",
                (f"MLSR_PASS_{stage}=1",),
            )
        )
    jobs.append(ShaderJob("post", root / "post.hlsl", "main"))
    if include_utility:
        common = Path("models/fsr4_int8/reference/fsr4_4_0_2_int8/common")
        jobs.append(ShaderJob("spd-auto-exposure", common / "spd_auto_exposure.hlsl", "main", use_model_defines=False))
        jobs.append(ShaderJob("rcas", common / "rcas.hlsl", "main", use_model_defines=True))
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
        "-no-warnings",
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

    print(f"[fsr4-reference] compiling preset-slice {job.name} entry={job.entry}", flush=True)
    start = time.monotonic()
    try:
        run_bounded_child(command, repo_root, env, timeout_seconds)
        print(f"[fsr4-reference] compiled {job.name} in {time.monotonic() - start:.1f}s", flush=True)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"DXC timed out after {timeout_seconds}s while compiling {job.name}: {source_path}") from exc


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



def worker_count(requested: int) -> int:
    if requested and requested > 0:
        return requested
    return max(1, os.cpu_count() or 1)

def main() -> None:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    compiler_dir, dxc = compiler_paths(repo_root)
    presets = PRESETS if args.preset == "all" else (args.preset,)
    capacities = CAPACITIES if args.capacity == "all" else (args.capacity,)
    options = options_from_args(args)
    workers = worker_count(args.jobs)
    scheduled: list[tuple[str, str, ShaderJob, tuple[str, ...]]] = []
    for preset in presets:
        for capacity in capacities:
            jobs = shader_jobs(preset, capacity, args.quick, args.include_utility)
            print(f"[fsr4-reference] slice preset={preset} capacity={capacity} jobs={len(jobs)}", flush=True)
            base_model_defines = model_defines(capacity, options)
            for job in jobs:
                scheduled.append((preset, capacity, job, base_model_defines))
    print(f"[fsr4-reference] compiling {len(scheduled)} shader job(s) with jobs={workers}", flush=True)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="apusr_fsr4_reference_dxil_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            future_to_job = {
                pool.submit(
                    compile_shader,
                    repo_root,
                    compiler_dir,
                    dxc,
                    job,
                    temp_dir / f"{preset}_{capacity}_{job.name}.dxil",
                    args.timeout_seconds,
                    base_defines,
                ): (preset, capacity, job)
                for preset, capacity, job, base_defines in scheduled
            }
            completed = 0
            for future in as_completed(future_to_job):
                preset, capacity, job = future_to_job[future]
                try:
                    future.result()
                except Exception as exc:  # noqa: BLE001
                    raise SystemExit(f"Failed shader preflight for preset={preset} capacity={capacity} job={job.name}: {exc}") from exc
                completed += 1
                print(f"[fsr4-reference] [{completed}/{len(scheduled)}] finished preset={preset} capacity={capacity} job={job.name} elapsed={time.monotonic() - started:.1f}s", flush=True)
    print(f"[fsr4-reference] compiled {len(scheduled)} shader job(s) for preset={args.preset} capacity={args.capacity}")


if __name__ == "__main__":
    main()
