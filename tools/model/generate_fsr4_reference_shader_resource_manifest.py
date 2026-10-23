# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Generate a shader-resource manifest for the FSR4 INT8 reference shaders.

The manifest schema is intentionally backend-neutral.  The current generator
uses DXC assembly output for DX12/DXIL, but entries record shader-visible HLSL
register usage rather than Direct3D root-signature details.  A future Vulkan
path should emit the same schema with targetApi="vulkan" and the SPIR-V-facing
binary metadata filled in.

This tool is packaging/startup-validation infrastructure.  The generated JSON is
loaded once by the backend validator and must never become per-frame work.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

from compile_fsr4_reference_dx12_shaders import (  # type: ignore
    CAPACITY_ENUM,
    PRESET_ENUM,
    UTILITY_DEFINES,
    PermutationOptions,
    compiler_paths,
    effective_capacity_for_job,
    effective_options_for_job,
    effective_preset_for_job,
    model_defines,
    options_from_args,
    permutation_key,
    product_matrix_options,
    run_bounded_child,
    selected_capacities,
    selected_presets,
    shader_jobs,
    worker_count,
)

_BIND_RE = re.compile(r"\b(?P<kind>cb|t|u|s)(?P<index>\d+)\b")
_TYPE_TO_SPACE = {
    "cbuffer": "cbv",
    "sampler": "sampler",
    "texture": "srv",
    "UAV": "uav",
}
_KIND_TO_SPACE = {
    "cb": "cbv",
    "s": "sampler",
    "t": "srv",
    "u": "uav",
}


@dataclass
class ManifestCompileTask:
    identity: tuple[object, ...]
    preset: str
    capacity: str
    options: PermutationOptions
    job: object
    entries: list[dict[str, object]]
    resource_set_id: str = ""


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--target-api", choices=("dx12",), default="dx12", help="Backend target described by this manifest. Vulkan should add a SPIR-V generator that emits the same schema.")
    parser.add_argument("--preset", default="balanced", choices=("all", *tuple(PRESET_ENUM)))
    parser.add_argument("--capacity", default="2160", choices=("all", *tuple(CAPACITY_ENUM)))
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
    parser.add_argument("--include-debug", action="store_true", help="With --product-matrix, also generate debug-visualize variants for developer packages.")
    parser.add_argument("--product-matrix", action="store_true", help="Generate the declared DP4A INT8 reference product matrix for all selected presets/capacities. This is packaging work, not a short preflight.")
    parser.add_argument("--quick", action="store_true", help="Compile only pre, pass1, and post per selected slice. Explicit developer preflight only; invalid with --product-matrix.")
    parser.add_argument("--include-utility", action="store_true", help="Also include SPD auto-exposure and RCAS shaders.")
    parser.add_argument("--pretty", action="store_true", help="Write pretty JSON. Product targets use compact JSON to keep generated artifacts small and fast to write.")
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def dispatch_name(job_name: str) -> str:
    if job_name == "fsr4_pre":
        return "fsr4_pre"
    if job_name == "fsr4_post":
        return "fsr4_post"
    if job_name == "spd_auto_exposure":
        return "spd_auto_exposure"
    if job_name == "rcas":
        return "rcas"
    if job_name.startswith("fsr4_pass_"):
        return job_name
    if job_name == "debug_view":
        return "debug_view"
    return job_name


def pass_kind(job_name: str) -> str:
    if job_name == "fsr4_pre":
        return "model-pre"
    if job_name == "fsr4_post":
        return "model-post"
    if job_name.startswith("fsr4_pass_"):
        return "model-middle"
    if job_name == "spd_auto_exposure":
        return "spd-auto-exposure"
    if job_name == "rcas":
        return "rcas"
    if job_name == "debug_view":
        return "debug-view"
    return "unknown"


def logical_stage(job_name: str) -> int:
    if job_name.startswith("fsr4_pass_"):
        try:
            return int(job_name.rsplit("_", 1)[1])
        except ValueError:
            return 0
    if job_name == "fsr4_post":
        return 13
    return 0


def compile_for_assembly(repo_root: Path, compiler_dir: Path, dxc: Path, job, dxil_path: Path, asm_path: Path, timeout_seconds: int, base_model_defines: tuple[str, ...]) -> None:
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
        str(dxil_path),
        "-Fc",
        str(asm_path),
    ]
    base_defines = base_model_defines if job.use_model_defines else UTILITY_DEFINES
    for define in (*base_defines, *job.extra_defines):
        command.extend(["-D", define])
    command.append(str(source_path))

    print(f"[fsr4-resource-manifest] compiling {dispatch_name(job.name)} entry={job.entry}", flush=True)
    start = time.monotonic()
    try:
        run_bounded_child(command, repo_root, env, timeout_seconds)
        print(f"[fsr4-resource-manifest] compiled {dispatch_name(job.name)} in {time.monotonic() - start:.1f}s", flush=True)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"DXC timed out after {timeout_seconds}s while generating resource manifest for {job.name}: {source_path}") from exc


def parse_resource_bindings(asm_path: Path) -> list[dict[str, object]]:
    lines = asm_path.read_text(encoding="utf-8", errors="replace").splitlines()
    in_table = False
    out: list[dict[str, object]] = []
    for raw in lines:
        line = raw.strip()
        if line == "; Resource Bindings:":
            in_table = True
            continue
        if not in_table:
            continue
        if line.startswith("target datalayout") or line.startswith("target triple"):
            break
        if not line.startswith(";"):
            continue
        row = line[1:].strip()
        if not row or row.startswith("Name") or row.startswith("---"):
            continue
        fields = row.split()
        if len(fields) < 7:
            continue
        bind = fields[-2]
        match = _BIND_RE.fullmatch(bind)
        if not match:
            continue
        kind = match.group("kind")
        bind_space = _KIND_TO_SPACE[kind]
        type_space = _TYPE_TO_SPACE.get(fields[1])
        if type_space and type_space != bind_space:
            raise SystemExit(f"Unexpected binding type mismatch in {asm_path}: {row}")
        out.append({
            "name": fields[0],
            "space": bind_space,
            "register": int(match.group("index")),
            "registerSpace": 0,
            "type": fields[1],
            "format": fields[2],
            "dimension": fields[3],
            "compilerId": fields[4],
            "count": int(fields[-1]) if fields[-1].isdigit() else fields[-1],
        })
    return out


def selected_options(args: argparse.Namespace) -> list[PermutationOptions]:
    if args.product_matrix:
        if args.quick:
            raise SystemExit("--product-matrix is a packaging/product validation mode and cannot be combined with --quick")
        if args.wmma:
            raise SystemExit("--product-matrix currently declares the DP4A INT8 reference product path; WMMA needs a separate backend matrix")
        return product_matrix_options(args.include_debug)
    return [options_from_args(args)]


def jobs_for_manifest(preset: str, capacity: str, quick: bool, include_utility: bool):
    jobs = shader_jobs(preset, capacity)
    if quick:
        keep = {"fsr4_pre", "fsr4_pass_1", "fsr4_post"}
        if include_utility:
            keep.update({"spd_auto_exposure", "rcas"})
        return [job for job in jobs if job.name in keep]
    if include_utility:
        return jobs
    return [job for job in jobs if job.name not in {"spd_auto_exposure", "rcas"}]


def build_manifest_tasks(args: argparse.Namespace) -> tuple[list[ManifestCompileTask], int]:
    presets = selected_presets(args.preset)
    capacities = selected_capacities(args.capacity)
    options_list = selected_options(args)
    tasks: dict[tuple[object, ...], ManifestCompileTask] = {}
    entry_count = 0
    for options in options_list:
        for preset in presets:
            for capacity in capacities:
                full_key = permutation_key(preset, capacity, options)
                for job in jobs_for_manifest(preset, capacity, args.quick, args.include_utility):
                    entry_count += 1
                    effective_options = effective_options_for_job(job, options)
                    effective_preset = effective_preset_for_job(job, preset)
                    effective_capacity = effective_capacity_for_job(job, capacity)
                    effective_job = job
                    if effective_preset != preset:
                        effective_job = next(j for j in shader_jobs(effective_preset, effective_capacity) if j.name == job.name)
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
                        task = ManifestCompileTask(identity, effective_preset, effective_capacity, effective_options, effective_job, [])
                        tasks[identity] = task
                    task.entries.append({
                        "preset": preset,
                        "capacity": capacity,
                        "permutationKey": full_key,
                        "targetApi": args.target_api,
                        "binaryFormat": "dxil",
                        "bindingModel": "hlsl-registers",
                        "dispatch": dispatch_name(job.name),
                        "passKind": pass_kind(job.name),
                        "logicalStage": logical_stage(job.name),
                        "entryPoint": job.entry,
                        "source": job.source.as_posix(),
                    })
    return list(tasks.values()), entry_count


def compile_manifest_task(repo_root: Path, compiler_dir: Path, dxc: Path, task: ManifestCompileTask, timeout_seconds: int) -> tuple[dict[str, object], list[dict[str, object]]]:
    with tempfile.TemporaryDirectory(prefix="apusr_fsr4_resource_manifest_task_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        dxil_path = temp_dir / f"{task.job.name}.dxil"
        asm_path = temp_dir / f"{task.job.name}.asm"
        compile_for_assembly(repo_root, compiler_dir, dxc, task.job, dxil_path, asm_path, timeout_seconds, model_defines(task.capacity, task.options))
        bindings = parse_resource_bindings(asm_path)

    resource_set = {
        "id": task.resource_set_id,
        "dispatch": dispatch_name(task.job.name),
        "effectivePreset": task.preset,
        "effectiveCapacity": task.capacity,
        "entryPoint": task.job.entry,
        "source": task.job.source.as_posix(),
        "resourceCount": len(bindings),
        "resources": bindings,
    }
    out: list[dict[str, object]] = []
    for base in task.entries:
        entry = dict(base)
        entry["resourceSetId"] = task.resource_set_id
        entry["resourceCount"] = len(bindings)
        out.append(entry)
    return resource_set, out


def write_manifest_atomic(output: Path, manifest: dict[str, object], pretty: bool) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temp_output = output.with_name(output.name + ".tmp")
    print(f"[fsr4-resource-manifest] writing manifest to {output}", flush=True)
    with temp_output.open("w", encoding="utf-8", newline="\n") as stream:
        if pretty:
            json.dump(manifest, stream, indent=2)
        else:
            json.dump(manifest, stream, separators=(",", ":"))
        stream.write("\n")
    temp_output.replace(output)


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    compiler_dir, dxc = compiler_paths(repo_root)
    tasks, logical_entries = build_manifest_tasks(args)
    workers = worker_count(args.jobs)
    print(
        f"[fsr4-resource-manifest] generation: logical_entries={logical_entries} unique_compile_jobs={len(tasks)} jobs={workers} product_matrix={args.product_matrix} quick={args.quick}",
        flush=True,
    )
    tasks.sort(key=lambda task: tuple(str(part) for part in task.identity))
    for index, task in enumerate(tasks):
        task.resource_set_id = f"rs{index:05d}"

    entries: list[dict[str, object]] = []
    resource_sets: list[dict[str, object]] = []
    started = time.monotonic()
    completed = 0
    with ThreadPoolExecutor(max_workers=workers) as pool:
        future_to_task = {pool.submit(compile_manifest_task, repo_root, compiler_dir, dxc, task, args.timeout_seconds): task for task in tasks}
        for future in as_completed(future_to_task):
            task = future_to_task[future]
            try:
                resource_set, task_entries = future.result()
                resource_sets.append(resource_set)
                entries.extend(task_entries)
            except Exception as exc:  # noqa: BLE001
                raise SystemExit(f"Failed generating resource manifest for shader job {task.identity}: {exc}") from exc
            completed += 1
            print(f"[fsr4-resource-manifest] [{completed}/{len(tasks)}] reflected {task.job.name} entries={len(task.entries)} elapsed={time.monotonic() - started:.1f}s", flush=True)
    entries.sort(key=lambda item: (str(item["preset"]), str(item["capacity"]), int(item["permutationKey"]), str(item["dispatch"])))
    resource_sets.sort(key=lambda item: str(item["id"]))
    single_key = None
    selected_opts = selected_options(args)
    presets = selected_presets(args.preset)
    capacities = selected_capacities(args.capacity)
    if len(selected_opts) == 1 and len(presets) == 1 and len(capacities) == 1:
        single_key = permutation_key(presets[0], capacities[0], selected_opts[0])
    manifest = {
        "schema": "apusr.fsr4_reference.shader_resource_manifest.v2",
        "generator": Path(__file__).name,
        "targetApi": args.target_api,
        "compiler": "dxc",
        "binaryFormat": "dxil",
        "sourceLanguage": "hlsl",
        "targetProfile": "cs_6_6",
        "hlslVersion": "2021",
        "presetSelection": args.preset,
        "capacitySelection": args.capacity,
        "productMatrix": args.product_matrix,
        "permutationKey": single_key,
        "quick": args.quick,
        "includeUtility": args.include_utility,
        "deduplicatedCompileJobs": len(tasks),
        "resourceSetCount": len(resource_sets),
        "logicalEntryCount": len(entries),
        "resourceSets": resource_sets,
        "entries": entries,
    }
    write_manifest_atomic(args.output, manifest, args.pretty)
    total_resources = sum(int(resource_set["resourceCount"]) for resource_set in resource_sets)
    print(f"[fsr4-resource-manifest] wrote {len(entries)} logical shader entries using {len(resource_sets)} resource sets / {total_resources} unique resources to {args.output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
