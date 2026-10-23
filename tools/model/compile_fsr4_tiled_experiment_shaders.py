# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Compile APUSR FSR4 INT8 tiled-DP4A Stage1 shader into a developer DXIL cache.

The pass1 tiled-exact shader deliberately preserves the generated pass1
runtime dispatch ABI.  The shader source is specialized by preset/capacity, but
pass1 does not depend on color-space, depth, motion-vector, auto-exposure, or
jitter feature permutations.  To avoid a narrow runtime trap, this tool compiles
one DXIL blob per preset/capacity pair and materializes that blob under every
non-debug DP4A product permutation key for that preset/capacity.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import signal
import stat
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory


UTILITY_DEFINES = (
    "FFX_GPU=1",
    "FFX_HLSL=1",
    "FFX_HALF=1",
    "FFX_HLSL_SM=66",
    "FFX_MLSR_RESOLUTION=1",
    "WMMA_ENABLED=0",
    "DOT4_ENABLED=1",
    "FFX_DEBUG_VISUALIZE=0",
)

PRESETS = (
    ("native", "NATIVE", 0),
    ("quality", "QUALITY", 1),
    ("balanced", "BALANCED", 2),
    ("performance", "PERFORMANCE", 3),
    ("drs", "DRS", 4),
    ("ultraperf", "ULTRAPERF", 5),
)

CAPACITIES = (
    ("1080", 0),
    ("2160", 1),
    ("4320", 2),
)

COLOR_SPACES = (
    "linear",
    "nonlinear",
    "srgb",
    "pq",
)


@dataclass(frozen=True)
class TiledShaderJob:
    source: Path
    entry: str
    cache_preset: str
    preset_define: str
    preset_value: int
    cache_capacity: str
    capacity_value: int
    cache_dispatch: str


def permutation_key(*,
                    preset_value: int,
                    capacity_value: int,
                    color_space: str,
                    jittered_motion_vectors: bool,
                    auto_exposure: bool,
                    low_res_motion_vectors: bool,
                    depth_inverted: bool,
                    wmma: bool = False,
                    debug_visualize: bool = False) -> int:
    # Keep bit packing identical to src/models/fsr4_int8/apusr_fsr4_int8_graph.cpp.
    key = 0
    if debug_visualize:
        key |= 1
    key <<= 1
    if color_space == "pq":
        key |= 1
    key <<= 1
    if color_space == "srgb":
        key |= 1
    key <<= 1
    if color_space != "linear":
        key |= 1
    key <<= 1
    if jittered_motion_vectors:
        key |= 1
    key <<= 1
    if auto_exposure:
        key |= 1
    key <<= 1
    if low_res_motion_vectors:
        key |= 1
    key <<= 1
    if depth_inverted:
        key |= 1
    key <<= 1
    if wmma:
        key |= 1
    key <<= 2
    key |= capacity_value
    key <<= 3
    key |= preset_value
    return key


def product_permutation_keys_for_job(job: TiledShaderJob) -> list[int]:
    keys: set[int] = set()
    bools = (False, True)
    for color_space in COLOR_SPACES:
        for jittered in bools:
            for auto_exposure in bools:
                for low_res_mv in bools:
                    for depth_inverted in bools:
                        keys.add(permutation_key(
                            preset_value=job.preset_value,
                            capacity_value=job.capacity_value,
                            color_space=color_space,
                            jittered_motion_vectors=jittered,
                            auto_exposure=auto_exposure,
                            low_res_motion_vectors=low_res_mv,
                            depth_inverted=depth_inverted,
                        ))
    return sorted(keys)


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--cache-root",
        type=Path,
        default=repo_root / "build/experiments/fsr4_pass1_tiled_exact_shader_cache",
    )
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Compile every preset/capacity specialization to a temporary output and do not update the cache.",
    )
    parser.add_argument(
        "--materialize-product-permutations",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Copy each preset/capacity blob under every non-debug DP4A product permutation key for that pair.",
    )
    return parser.parse_args()


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
            stdout, stderr = process.communicate()
            if stdout:
                print(stdout.decode(errors="replace"), end="")
            if stderr:
                print(stderr.decode(errors="replace"), end="")
            raise subprocess.TimeoutExpired(command, timeout_seconds)
        time.sleep(0.05)


def pass1_tiled_exact_jobs() -> list[TiledShaderJob]:
    source = Path("shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl")
    entry = "apusr_fsr4_i8_pass1_tiled_exact"
    jobs: list[TiledShaderJob] = []
    for preset_name, preset_define, preset_value in PRESETS:
        for capacity_name, capacity_value in CAPACITIES:
            jobs.append(TiledShaderJob(
                source=source,
                entry=entry,
                cache_preset=preset_name,
                preset_define=preset_define,
                preset_value=preset_value,
                cache_capacity=capacity_name,
                capacity_value=capacity_value,
                cache_dispatch="fsr4_pass_1",
            ))
    return jobs


def pass2_tiled_exact_jobs() -> list[TiledShaderJob]:
    source = Path("shaders/dx12/optimized/apusr_fsr4_i8_pass2_tiled_exact.hlsl")
    entry = "apusr_fsr4_i8_pass2_tiled_exact"
    jobs: list[TiledShaderJob] = []
    for preset_name, preset_define, preset_value in PRESETS:
        for capacity_name, capacity_value in CAPACITIES:
            jobs.append(TiledShaderJob(
                source=source,
                entry=entry,
                cache_preset=preset_name,
                preset_define=preset_define,
                preset_value=preset_value,
                cache_capacity=capacity_name,
                capacity_value=capacity_value,
                cache_dispatch="fsr4_pass_2",
            ))
    return jobs


def front_c16_tiled_exact_jobs() -> list[TiledShaderJob]:
    return pass1_tiled_exact_jobs() + pass2_tiled_exact_jobs()


def compile_job(repo_root: Path,
                compiler_dir: Path,
                dxc: Path,
                job: TiledShaderJob,
                output_path: Path,
                timeout_seconds: int) -> None:
    source_path = repo_root / job.source
    if not source_path.is_file():
        raise SystemExit(f"Tiled shader source not found: {source_path}")

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
    for define in UTILITY_DEFINES:
        command.extend(["-D", define])
    command.extend(["-D", f"APUSR_FSR4_I8_PRESET_{job.preset_define}=1"])
    command.extend(["-D", f"APUSR_FSR4_I8_CAPACITY_{job.cache_capacity}=1"])
    command.append(str(source_path))

    print(
        f"[fsr4-tiled-dxil] compiling {job.cache_dispatch} "
        f"preset={job.cache_preset} capacity={job.cache_capacity} from {job.source}",
        flush=True,
    )
    try:
        run_bounded_child(command, repo_root, env, timeout_seconds)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"DXC timed out after {timeout_seconds}s while compiling {job.source}") from exc


def materialize_job_cache(cache_root: Path, job: TiledShaderJob, source_blob: Path) -> int:
    keys = product_permutation_keys_for_job(job)
    for key in keys:
        output_path = cache_root / job.cache_preset / job.cache_capacity / f"p{key}" / f"{job.cache_dispatch}.dxil"
        output_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_blob, output_path)
        manifest = output_path.parent / "manifest.txt"
        manifest.write_text(
            "# APUSR developer packaged shader override cache\n"
            f"{job.cache_dispatch}=front_c16_tiled_exact\n",
            encoding="utf-8",
            newline="\n",
        )
    return len(keys)


def main() -> None:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    compiler_dir, dxc = compiler_paths(repo_root)
    jobs = front_c16_tiled_exact_jobs()

    if args.verify:
        verify_root = repo_root / "build" / "verify" / "fsr4_front_c16_tiled_exact"
        verify_root.mkdir(parents=True, exist_ok=True)
        for job in jobs:
            output_path = verify_root / job.cache_preset / job.cache_capacity / f"{job.cache_dispatch}.dxil"
            output_path.parent.mkdir(parents=True, exist_ok=True)
            compile_job(repo_root, compiler_dir, dxc, job, output_path, args.timeout_seconds)
            print(f"[fsr4-tiled-dxil] verified {output_path}")
        return

    total_entries = 0
    with TemporaryDirectory(prefix="apusr_front_c16_tiled_exact_") as tmp_dir_str:
        tmp_dir = Path(tmp_dir_str)
        for job in jobs:
            blob_path = tmp_dir / job.cache_preset / job.cache_capacity / f"{job.cache_dispatch}.dxil"
            blob_path.parent.mkdir(parents=True, exist_ok=True)
            compile_job(repo_root, compiler_dir, dxc, job, blob_path, args.timeout_seconds)
            if args.materialize_product_permutations:
                total_entries += materialize_job_cache(args.cache_root, job, blob_path)
            else:
                key = permutation_key(
                    preset_value=job.preset_value,
                    capacity_value=job.capacity_value,
                    color_space="linear",
                    jittered_motion_vectors=False,
                    auto_exposure=True,
                    low_res_motion_vectors=True,
                    depth_inverted=True,
                )
                output_path = args.cache_root / job.cache_preset / job.cache_capacity / f"p{key}" / f"{job.cache_dispatch}.dxil"
                output_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(blob_path, output_path)
                total_entries += 1

    print(f"[fsr4-tiled-dxil] materialized {total_entries} packaged override entries under {args.cache_root}")


if __name__ == "__main__":
    main()
