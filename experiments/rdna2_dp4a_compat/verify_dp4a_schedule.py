# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Compile/audit quarantined RDNA2 DP4A scheduling experiments.

This script is intentionally outside APUSR's product build and test graph. It
reconstructs the quarantined DX12 scheduling experiments from APUSR-owned include
shadows without adding compatibility switches to the shipping shader compiler.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools/model"))
sys.path.insert(0, str(REPO_ROOT / "tools/analysis"))

import compile_fsr4_reference_dx12_shaders as shaders  # noqa: E402
import dxil_dp4a_schedule as audit  # noqa: E402

QUARANTINE_INCLUDE = Path("experiments/rdna2_dp4a_compat/shaders")

EXPECTED_PASS36 = {
    ("fsr4_pass_3", "serial"): (512, 16),
    ("fsr4_pass_3", "depth4"): (512, 4),
    ("fsr4_pass_3", "dechained"): (512, 1),
    ("fsr4_pass_6", "serial"): (2048, 32),
    ("fsr4_pass_6", "depth4"): (2048, 4),
    ("fsr4_pass_6", "dechained"): (2048, 1),
}

EXPECTED_PASS911 = {
    ("fsr4_pass_9", "serial"): (5760, 32),
    ("fsr4_pass_9", "dechained"): (5760, 1),
    ("fsr4_pass_11", "serial"): (2112, 16),
    ("fsr4_pass_11", "dechained"): (2112, 1),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--full-matrix",
        action="store_true",
        help="Audit all six presets and all three capacities instead of balanced/2160 only.",
    )
    parser.add_argument("--shader-timeout-seconds", type=int, default=60)
    return parser.parse_args()


def verify_pass911_source_contract() -> None:
    reference = REPO_ROOT / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/ml2code_runtime/operators/int8_NHWC/Fused/FNB_CT2D_ADD.hlsli"
    override = REPO_ROOT / QUARANTINE_INCLUDE / "ml2code_runtime/operators/int8_NHWC/Fused/FNB_CT2D_ADD.hlsli"
    override_text = override.read_text(encoding="utf-8")

    serial_pattern = re.compile(
        r"^(\s*)([A-Za-z_][A-Za-z0-9_\[\]\.]*) = dot4add_i8packed\((.*), \2\);$"
    )
    transformed = 0
    for line in reference.read_text(encoding="utf-8").splitlines():
        match = serial_pattern.match(line)
        if not match:
            continue
        indent, accumulator, operands = match.groups()
        expected = f"{indent}{accumulator} += dot4add_i8packed({operands}, 0);"
        assert expected in override_text, (line, expected)
        transformed += 1

    assert transformed == 53, transformed
    assert override_text.count("+= dot4add_i8packed") == 53


def compile_job(
    compiler_dir: Path,
    dxc: Path,
    job: shaders.ShaderJob,
    output: Path,
    timeout_seconds: int,
    base_defines: tuple[str, ...],
    include_quarantine: bool,
    schedule_defines: tuple[str, ...] = (),
) -> None:
    source_path = REPO_ROOT / job.source
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
    ]
    if include_quarantine:
        command.extend(["-I", str(REPO_ROOT / QUARANTINE_INCLUDE)])
    command.extend(
        [
            "-I",
            str(REPO_ROOT / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include"),
            "-I",
            str(REPO_ROOT / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4"),
            "-Fo",
            str(output),
        ]
    )
    for define in (*base_defines, *job.extra_defines, *schedule_defines):
        command.extend(["-D", define])
    command.append(str(source_path))
    shaders.run_bounded_child(command, REPO_ROOT, env, timeout_seconds)


def audit_one(
    compiler_dir: Path,
    dxc: Path,
    dump_env: dict[str, str],
    preset: str,
    capacity: str,
    pass_name: str,
    schedule: str,
    temp: Path,
    timeout_seconds: int,
) -> None:
    jobs = {job.name: job for job in shaders.shader_jobs(preset, capacity, debug_visualize=False)}
    job = jobs[pass_name]
    options = shaders.PermutationOptions(False, True, True, True, False, "linear", False)
    base_defines = shaders.model_defines(capacity, options)

    include_quarantine = schedule != "serial"
    schedule_defines: tuple[str, ...] = ()
    if pass_name in {"fsr4_pass_3", "fsr4_pass_6"}:
        if schedule == "depth4":
            schedule_defines = ("APUSR_FSR4_I8_DP4A_SCHEDULE_DEPTH4=1",)
        elif schedule == "dechained":
            schedule_defines = ("APUSR_FSR4_I8_DP4A_SCHEDULE_DECHAINED=1",)
        expected = EXPECTED_PASS36[(pass_name, schedule)]
    else:
        expected = EXPECTED_PASS911[(pass_name, schedule)]

    output = temp / f"{preset}_{capacity}_{pass_name}_{schedule}.dxil"
    compile_job(
        compiler_dir,
        dxc,
        job,
        output,
        timeout_seconds,
        base_defines,
        include_quarantine,
        schedule_defines,
    )
    result = audit.audit_file(dxc, output, dump_env)
    expected_calls, expected_depth = expected
    assert result.signed_dp4a_calls == expected_calls, (
        preset,
        capacity,
        pass_name,
        schedule,
        result.signed_dp4a_calls,
        expected_calls,
    )
    assert result.max_direct_dp4a_chain == expected_depth, (
        preset,
        capacity,
        pass_name,
        schedule,
        result.max_direct_dp4a_chain,
        expected_depth,
    )


def main() -> None:
    args = parse_args()
    verify_pass911_source_contract()
    compiler_dir, dxc = shaders.compiler_paths(REPO_ROOT)
    dump_env = audit.prepare_dxc(dxc)

    presets = tuple(shaders.PRESET_ENUM) if args.full_matrix else ("balanced",)
    capacities = tuple(shaders.CAPACITY_ENUM) if args.full_matrix else ("2160",)

    with tempfile.TemporaryDirectory(prefix="apusr_rdna2_dp4a_quarantine_") as temp_name:
        temp = Path(temp_name)
        for preset in presets:
            for capacity in capacities:
                for schedule in ("serial", "depth4", "dechained"):
                    for pass_name in ("fsr4_pass_3", "fsr4_pass_6"):
                        audit_one(
                            compiler_dir,
                            dxc,
                            dump_env,
                            preset,
                            capacity,
                            pass_name,
                            schedule,
                            temp,
                            args.shader_timeout_seconds,
                        )
                for schedule in ("serial", "dechained"):
                    for pass_name in ("fsr4_pass_9", "fsr4_pass_11"):
                        audit_one(
                            compiler_dir,
                            dxc,
                            dump_env,
                            preset,
                            capacity,
                            pass_name,
                            schedule,
                            temp,
                            args.shader_timeout_seconds,
                        )

    scope = "full six-preset x three-capacity matrix" if args.full_matrix else "balanced/2160 representative slice"
    print(f"RDNA2 DP4A quarantine verified: {scope}; no product build integration is enabled")


if __name__ == "__main__":
    main()
