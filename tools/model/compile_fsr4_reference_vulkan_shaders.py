# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Compile APUSR's FSR4 INT8 graph to Vulkan 1.2 / SPIR-V 1.5.

Product generation emits one self-indexed shader pack. Logical runtime keys are
mapped to deduplicated physical SPIR-V blobs inside the pack; filesystem paths
are deliberately not part of the runtime ABI. --output-root remains available
for explicit developer slice inspection only.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import platform
import struct
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

import compile_fsr4_reference_dx12_shaders as dx12
import spirv_dot_accumulate as dot_accumulate


SRV_BINDING_BASE = 0
UAV_BINDING_BASE = 32
CBV_BINDING_BASE = 64
SAMPLER_BINDING_BASE = 80
DESCRIPTOR_SET = 0
VULKAN_TARGET = "1.2"
SPIRV_TARGET = "1.5"
SHADER_MODEL = "cs_6_6"
HLSL_VERSION = "2021"
OPTIMIZATION = "O3"
DOT_ACCUMULATE_LOWERINGS = ("bounded-fused", "split")
VECTOR_LOAD_STAGES = (3, 6, 7, 8, 9)
VECTOR_STORE_STAGES = (3, 6, 9)
VECTOR_INITIALIZER_SRV_REGISTER = 31
VECTOR_SCRATCH_UAV_REGISTER = 31

PIPELINE_VARIANTS = {
    "strict-reference": (0, False, False),
    "cpu-dp4a-tuned": (1, False, False),
    "dp4a-optimized": (2, False, True),
}
CAPACITY_DISK_VALUE = {"1080": 1080, "2160": 2160, "4320": 4320}
PASS_KIND_VALUE = {
    "spd-auto-exposure": 0,
    "model-pre": 1,
    "model-middle": 2,
    "model-post": 3,
    "rcas": 4,
    "debug-view": 5,
}

PACK_MAGIC = b"APUSRVK\0"
PACK_HEADER = struct.Struct("<8s7I")
PACK_ENTRY = struct.Struct("<7I")
PACK_BLOB = struct.Struct("<2I")

# Every SPIR-V capability emitted by the current package is classified here.
# A new capability must be reviewed before a product pack can be generated.
CAPABILITY_REQUIREMENTS: dict[int, tuple[str, str] | None] = {
    1: None,  # Shader
    9: ("vulkan12Features", "shaderFloat16"),
    22: ("coreFeatures", "shaderInt16"),
    39: ("vulkan12Features", "shaderInt8"),
    49: ("coreFeatures", "shaderStorageImageExtendedFormats"),
    50: None,  # ImageQuery
    56: ("coreFeatures", "shaderStorageImageWriteWithoutFormat"),
    68: ("subgroupOperations", "quad"),
    # Current DXC establishes SM 6.6 compute-quad topology with
    # DerivativeGroupLinearKHR for 1-D threadgroups using QuadRead* intrinsics.
    5350: ("extensionFeatures", "computeDerivativeGroupLinear"),
    6018: ("extensionFeatures", "shaderIntegerDotProduct"),
    6019: ("extensionFeatures", "shaderIntegerDotProduct"),
}
EXTENSION_REQUIREMENTS: dict[str, tuple[str, str] | None] = {
    "SPV_KHR_integer_dot_product": ("deviceExtensionsBelowVulkan13", "VK_KHR_shader_integer_dot_product"),
    "SPV_KHR_compute_shader_derivatives": ("deviceExtensions", "VK_KHR_compute_shader_derivatives"),
}


@dataclass(frozen=True)
class CompileTask:
    identity: tuple[object, ...]
    preset: str
    capacity: str
    options: dx12.PermutationOptions
    job: dx12.ShaderJob


@dataclass(frozen=True)
class LogicalEntry:
    pipeline_variant: int
    preset: int
    capacity: int
    permutation: int
    pass_kind: int
    logical_stage: int
    identity: tuple[object, ...]

    def key(self) -> tuple[int, int, int, int, int, int]:
        return (
            self.pipeline_variant,
            self.preset,
            self.capacity,
            self.permutation,
            self.pass_kind,
            self.logical_stage,
        )


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--preset", default="balanced", choices=("all", *tuple(dx12.PRESET_ENUM)))
    parser.add_argument("--capacity", default="2160", choices=("all", *tuple(dx12.CAPACITY_ENUM)))
    parser.add_argument("--depth-inverted", dest="depth_inverted", action="store_true", default=True)
    parser.add_argument("--no-depth-inverted", dest="depth_inverted", action="store_false")
    parser.add_argument("--low-res-motion-vectors", dest="low_res_motion_vectors", action="store_true", default=True)
    parser.add_argument("--display-res-motion-vectors", dest="low_res_motion_vectors", action="store_false")
    parser.add_argument("--auto-exposure", dest="auto_exposure", action="store_true", default=True)
    parser.add_argument("--no-auto-exposure", dest="auto_exposure", action="store_false")
    parser.add_argument("--jittered-motion-vectors", action="store_true")
    parser.add_argument("--color-space", choices=("linear", "nonlinear", "srgb", "pq"), default="linear")
    parser.add_argument("--debug-visualize", action="store_true")
    parser.add_argument("--pipeline-variant", choices=tuple(PIPELINE_VARIANTS), default="dp4a-optimized")
    parser.add_argument("--product-matrix", action="store_true")
    parser.add_argument("--include-debug", action="store_true", help="Include developer debug-visualization permutations in a product pack.")
    parser.add_argument("--output-root", type=Path, default=None, help="Developer-only loose SPIR-V slice output.")
    parser.add_argument("--output-pack", type=Path, default=None, help="Self-indexed runtime shader-pack output.")
    parser.add_argument("--manifest", type=Path, default=None, help="Build-time pack manifest; not required at runtime.")
    parser.add_argument("--shader-timeout-seconds", type=int, default=30)
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument(
        "--dot-accumulate-lowering",
        choices=DOT_ACCUMULATE_LOWERINGS,
        default="bounded-fused",
        help=(
            "bounded-fused replaces only no-overflow-proven OpSDot+OpIAdd SSA chains with "
            "OpSDotAccSat; split preserves DXC output as an A/B baseline"
        ),
    )
    return parser.parse_args()


def _spv_string(words: list[int]) -> str:
    raw = b"".join(struct.pack("<I", word) for word in words)
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def spirv_contract(path: Path) -> tuple[list[dict[str, int | str]], list[int], list[str]]:
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4 != 0:
        raise RuntimeError(f"invalid SPIR-V byte length: {path}")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != 0x07230203 or words[1] != 0x00010500:
        raise RuntimeError(f"expected SPIR-V 1.5 module: {path}")

    names: dict[int, str] = {}
    decorations: dict[int, dict[int, int]] = {}
    capabilities: set[int] = set()
    extensions: set[str] = set()
    cursor = 5
    while cursor < len(words):
        first = words[cursor]
        word_count = first >> 16
        opcode = first & 0xFFFF
        if word_count == 0 or cursor + word_count > len(words):
            raise RuntimeError(f"malformed SPIR-V instruction stream: {path}")
        inst = words[cursor : cursor + word_count]
        if opcode == 5 and word_count >= 3:  # OpName
            names[inst[1]] = _spv_string(inst[2:])
        elif opcode == 10 and word_count >= 2:  # OpExtension
            extensions.add(_spv_string(inst[1:]))
        elif opcode == 17 and word_count == 2:  # OpCapability
            capabilities.add(inst[1])
        elif opcode == 71 and word_count >= 4:  # OpDecorate
            decoration = inst[2]
            if decoration in (33, 34):  # Binding, DescriptorSet
                decorations.setdefault(inst[1], {})[decoration] = inst[3]
        cursor += word_count

    bindings: list[dict[str, int | str]] = []
    occupied: dict[tuple[int, int], str] = {}
    for target, decoration in decorations.items():
        if 33 not in decoration:
            continue
        descriptor_set = decoration.get(34, 0)
        binding = decoration[33]
        name = names.get(target, f"id_{target}")
        slot = (descriptor_set, binding)
        previous = occupied.get(slot)
        if previous is not None:
            raise RuntimeError(
                f"SPIR-V descriptor collision in {path.name}: set={descriptor_set} binding={binding} "
                f"is used by both {previous} and {name}"
            )
        occupied[slot] = name
        bindings.append({"name": name, "set": descriptor_set, "binding": binding})
    bindings.sort(key=lambda item: (int(item["set"]), int(item["binding"]), str(item["name"])))
    return bindings, sorted(capabilities), sorted(extensions)


def spirv_has_opcode(path: Path, required_opcode: int) -> bool:
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4 != 0:
        raise RuntimeError(f"invalid SPIR-V byte length: {path}")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203 or words[1] != 0x00010500:
        raise RuntimeError(f"expected SPIR-V 1.5 module: {path}")

    cursor = 5
    while cursor < len(words):
        first = words[cursor]
        word_count = first >> 16
        opcode = first & 0xFFFF
        if word_count == 0 or cursor + word_count > len(words):
            raise RuntimeError(f"malformed SPIR-V instruction stream: {path}")
        if opcode == required_opcode:
            return True
        cursor += word_count
    return False


def spirv_uint4_load_count(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4 != 0:
        raise RuntimeError(f"invalid SPIR-V byte length: {path}")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203 or words[1] != 0x00010500:
        raise RuntimeError(f"expected SPIR-V 1.5 module: {path}")

    uint32_types: set[int] = set()
    uint4_types: set[int] = set()
    instructions: list[tuple[int, tuple[int, ...]]] = []
    cursor = 5
    while cursor < len(words):
        word_count = words[cursor] >> 16
        opcode = words[cursor] & 0xFFFF
        if word_count == 0 or cursor + word_count > len(words):
            raise RuntimeError(f"malformed SPIR-V instruction stream: {path}")
        inst = tuple(words[cursor : cursor + word_count])
        instructions.append((opcode, inst))
        if opcode == 21 and word_count == 4 and inst[2] == 32 and inst[3] == 0:  # OpTypeInt uint32
            uint32_types.add(inst[1])
        cursor += word_count

    for opcode, inst in instructions:
        if opcode == 23 and len(inst) == 4 and inst[2] in uint32_types and inst[3] == 4:  # OpTypeVector uint4
            uint4_types.add(inst[1])
    return sum(1 for opcode, inst in instructions if opcode == 61 and len(inst) >= 4 and inst[1] in uint4_types)


def spirv_uint4_store_count(path: Path) -> int:
    data = path.read_bytes()
    if len(data) % 4 != 0:
        raise RuntimeError(f"malformed SPIR-V byte length: {path}")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    uint32_types: set[int] = set()
    uint4_types: set[int] = set()
    pointer_types: dict[int, int] = {}
    value_types: dict[int, int] = {}
    instructions: list[tuple[int, tuple[int, ...]]] = []
    cursor = 5
    while cursor < len(words):
        word_count = words[cursor] >> 16
        opcode = words[cursor] & 0xFFFF
        if word_count == 0 or cursor + word_count > len(words):
            raise RuntimeError(f"malformed SPIR-V instruction stream: {path}")
        inst = tuple(words[cursor : cursor + word_count])
        instructions.append((opcode, inst))
        if opcode == 21 and word_count == 4 and inst[2] == 32 and inst[3] == 0:  # OpTypeInt uint32
            uint32_types.add(inst[1])
        elif opcode == 32 and word_count == 4:  # OpTypePointer
            pointer_types[inst[1]] = inst[3]
        cursor += word_count

    for opcode, inst in instructions:
        if opcode == 23 and len(inst) == 4 and inst[2] in uint32_types and inst[3] == 4:  # OpTypeVector uint4
            uint4_types.add(inst[1])

    # Record result-id -> result-type for instructions with the normal result-type/result-id prefix.
    # We only need pointer-producing access chains here.
    for opcode, inst in instructions:
        if opcode in (65, 66, 67, 68) and len(inst) >= 4:  # access-chain family
            value_types[inst[2]] = inst[1]

    count = 0
    for opcode, inst in instructions:
        if opcode != 62 or len(inst) < 3:  # OpStore pointer object ...
            continue
        pointer_id = inst[1]
        pointer_type = value_types.get(pointer_id)
        if pointer_type is not None and pointer_types.get(pointer_type) in uint4_types:
            count += 1
    return count


def validate_vector_load_contract(path: Path, job: dx12.ShaderJob) -> None:
    if job.kind != "model-middle" or job.logical_stage not in VECTOR_LOAD_STAGES:
        return
    bindings, _capabilities, _extensions = spirv_contract(path)
    binding_by_name = {str(item["name"]): int(item["binding"]) for item in bindings}
    expected = {
        "ApusrMl2cScratchVector": UAV_BINDING_BASE + VECTOR_SCRATCH_UAV_REGISTER,
    }
    if job.logical_stage != 3:
        expected["ApusrMl2cInitializerVector"] = SRV_BINDING_BASE + VECTOR_INITIALIZER_SRV_REGISTER
    if any(binding_by_name.get(name) != binding for name, binding in expected.items()):
        raise RuntimeError(
            f"Vulkan stage {job.logical_stage} lost its aligned vector-load descriptor aliases: {path}"
        )
    if spirv_uint4_load_count(path) == 0:
        raise RuntimeError(f"Vulkan stage {job.logical_stage} lost its uint4 storage-buffer loads: {path}")
    if job.logical_stage in VECTOR_STORE_STAGES and spirv_uint4_store_count(path) == 0:
        raise RuntimeError(f"Vulkan stage {job.logical_stage} lost its uint4 storage-buffer stores: {path}")


def derive_vulkan_requirements(capabilities: set[int], extensions: set[str]) -> dict[str, object]:
    unknown_capabilities = capabilities.difference(CAPABILITY_REQUIREMENTS)
    if unknown_capabilities:
        raise RuntimeError(
            "unclassified SPIR-V capabilities require Vulkan contract review: "
            + ", ".join(str(value) for value in sorted(unknown_capabilities))
        )
    unknown_extensions = extensions.difference(EXTENSION_REQUIREMENTS)
    if unknown_extensions:
        raise RuntimeError(
            "unclassified SPIR-V extensions require Vulkan contract review: "
            + ", ".join(sorted(unknown_extensions))
        )

    requirements: dict[str, set[str]] = {
        "coreFeatures": set(),
        "vulkan12Features": set(),
        "extensionFeatures": set(),
        "deviceExtensions": set(),
        "deviceExtensionsBelowVulkan13": set(),
        "subgroupOperations": set(),
    }
    for capability in capabilities:
        requirement = CAPABILITY_REQUIREMENTS[capability]
        if requirement is not None:
            requirements[requirement[0]].add(requirement[1])
    for extension in extensions:
        requirement = EXTENSION_REQUIREMENTS[extension]
        if requirement is not None:
            requirements[requirement[0]].add(requirement[1])

    return {
        "minimumApiVersion": VULKAN_TARGET,
        "spirvCapabilities": sorted(capabilities),
        "spirvExtensions": sorted(extensions),
        **{name: sorted(values) for name, values in requirements.items()},
    }


def compile_shader(
    repo_root: Path,
    compiler_dir: Path,
    dxc: Path,
    job: dx12.ShaderJob,
    output_path: Path,
    timeout_seconds: int,
    base_model_defines: tuple[str, ...],
    dot_accumulate_lowering: str,
) -> None:
    source_path = repo_root / job.source
    output_path.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if platform.system().lower() == "windows":
        env["PATH"] = str(compiler_dir) + (os.pathsep + env["PATH"] if env.get("PATH") else "")
    else:
        env["LD_LIBRARY_PATH"] = str(compiler_dir) + (os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")

    command = [
        str(dxc),
        "-nologo",
        f"-{OPTIMIZATION}",
        "-enable-16bit-types",
        "-HV", HLSL_VERSION,
        "-T", SHADER_MODEL,
        "-E", job.entry,
        "-spirv",
        f"-fspv-target-env=vulkan{VULKAN_TARGET}",
        "-fvk-use-dx-layout",
        "-fvk-t-shift", str(SRV_BINDING_BASE), str(DESCRIPTOR_SET),
        "-fvk-u-shift", str(UAV_BINDING_BASE), str(DESCRIPTOR_SET),
        "-fvk-b-shift", str(CBV_BINDING_BASE), str(DESCRIPTOR_SET),
        "-fvk-s-shift", str(SAMPLER_BINDING_BASE), str(DESCRIPTOR_SET),
        "-I", str(repo_root / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include"),
        "-I", str(repo_root / "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4"),
        "-Fo", str(output_path),
        "-D", "APUSR_FSR4_TEXTURE_DATA_TYPE3=float3",
        "-D", "APUSR_FSR4_TEXTURE_DATA_TYPE_VECTOR=float4",
        "-D", "APUSR_VULKAN_SPIRV=1",
    ]
    base_defines = base_model_defines if job.use_model_defines else dx12.UTILITY_DEFINES
    for define in (*base_defines, *job.extra_defines):
        command.extend(["-D", define])
    if job.kind == "model-middle" and job.logical_stage in VECTOR_LOAD_STAGES:
        command.extend(["-D", "APUSR_VULKAN_VECTOR_LOADS=1"])
    if job.kind == "model-pre":
        command.extend(["-D", "APUSR_VULKAN_PRE_INPUT_CACHE=1"])
    if job.kind == "model-post":
        command.extend(["-D", "APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE=1"])
    command.append(str(source_path))
    dx12.run_bounded_child(command, repo_root, env, timeout_seconds)
    if dot_accumulate_lowering == "bounded-fused":
        dot_accumulate.lower_file_in_place(output_path)
    elif dot_accumulate_lowering != "split":
        raise ValueError(f"unknown Vulkan DOT4 accumulate lowering: {dot_accumulate_lowering}")
    validate_vector_load_contract(output_path, job)


def selected_presets(value: str) -> list[str]:
    return list(dx12.PRESET_ENUM) if value == "all" else [value]


def selected_capacities(value: str) -> list[str]:
    return list(dx12.CAPACITY_ENUM) if value == "all" else [value]


def compile_identity(
    job: dx12.ShaderJob,
    preset: str,
    capacity: str,
    options: dx12.PermutationOptions,
) -> tuple[object, ...]:
    return (
        job.name,
        preset,
        capacity,
        options,
        job.source.as_posix(),
        job.entry,
        job.extra_defines,
        job.use_model_defines,
    )


def canonical_job(
    job: dx12.ShaderJob,
    preset: str,
    capacity: str,
    options: dx12.PermutationOptions,
    optimized_pass1: bool,
    optimized_front: bool,
) -> tuple[dx12.ShaderJob, str, str, dx12.PermutationOptions]:
    effective_options = dx12.effective_options_for_job(job, options)
    effective_preset = dx12.effective_preset_for_job(job, preset)
    effective_capacity = dx12.effective_capacity_for_job(job, capacity)
    effective_job = job
    if effective_preset != preset:
        effective_job = next(
            candidate
            for candidate in dx12.shader_jobs(
                effective_preset,
                effective_capacity,
                optimized_pass1,
                optimized_front,
                effective_options.debug_visualize,
            )
            if candidate.name == job.name
        )
    return effective_job, effective_preset, effective_capacity, effective_options


def build_pack_plan(
    pipeline_variant: str,
    presets: list[str],
    capacities: list[str],
    options_list: list[dx12.PermutationOptions],
) -> tuple[list[CompileTask], list[LogicalEntry]]:
    pipeline_value, optimized_pass1, optimized_front = PIPELINE_VARIANTS[pipeline_variant]
    tasks: dict[tuple[object, ...], CompileTask] = {}
    entries: list[LogicalEntry] = []
    for options in options_list:
        for preset in presets:
            for capacity in capacities:
                permutation = dx12.permutation_key(preset, capacity, options)
                for job in dx12.shader_jobs(preset, capacity, optimized_pass1, optimized_front, options.debug_visualize):
                    effective_job, effective_preset, effective_capacity, effective_options = canonical_job(
                        job, preset, capacity, options, optimized_pass1, optimized_front
                    )
                    identity = compile_identity(effective_job, effective_preset, effective_capacity, effective_options)
                    tasks.setdefault(
                        identity,
                        CompileTask(identity, effective_preset, effective_capacity, effective_options, effective_job),
                    )
                    entries.append(LogicalEntry(
                        pipeline_variant=pipeline_value,
                        preset=dx12.PRESET_VALUE[preset],
                        capacity=CAPACITY_DISK_VALUE[capacity],
                        permutation=permutation,
                        pass_kind=PASS_KIND_VALUE[job.kind],
                        logical_stage=job.logical_stage,
                        identity=identity,
                    ))
    entries.sort(key=LogicalEntry.key)
    for previous, current in zip(entries, entries[1:]):
        if previous.key() == current.key():
            raise RuntimeError(f"duplicate logical Vulkan shader key: {current.key()}")
    return sorted(tasks.values(), key=lambda task: repr(task.identity)), entries


def compile_task_to_path(
    repo_root: Path,
    compiler_dir: Path,
    dxc: Path,
    task: CompileTask,
    output_path: Path,
    timeout_seconds: int,
    dot_accumulate_lowering: str,
) -> Path:
    compile_shader(
        repo_root,
        compiler_dir,
        dxc,
        task.job,
        output_path,
        timeout_seconds,
        dx12.model_defines(task.capacity, task.options),
        dot_accumulate_lowering,
    )
    return output_path


def physical_memory_bytes() -> int | None:
    if platform.system().lower() == "windows":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong),
                ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]
        status = MemoryStatus()
        status.dwLength = ctypes.sizeof(MemoryStatus)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int(status.ullTotalPhys)
        return None
    try:
        meminfo = Path("/proc/meminfo").read_text()
        for line in meminfo.splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError):
        pass
    try:
        return int(os.sysconf("SC_PHYS_PAGES")) * int(os.sysconf("SC_PAGE_SIZE"))
    except (AttributeError, OSError, ValueError):
        return None


def auto_heavy_worker_count(cpu_workers: int) -> tuple[int, int | None]:
    total_memory = physical_memory_bytes()
    if total_memory is None:
        return max(1, min(cpu_workers, 4)), None
    gib = 1024**3
    # Passes 7-9 peak near 2.6 GiB per DXC process in the current SPIR-V
    # compiler. Size this stable machine-level limit from installed RAM rather
    # than a transient free-memory snapshot taken while Ninja is busy elsewhere.
    memory_workers = max(1, (total_memory - 4 * gib) // (3 * gib))
    return max(1, min(cpu_workers, memory_workers)), total_memory


def compile_tasks(
    repo_root: Path,
    tasks: list[CompileTask],
    temp_root: Path,
    timeout_seconds: int,
    requested_jobs: int,
    dot_accumulate_lowering: str,
) -> dict[tuple[object, ...], Path]:
    compiler_dir, dxc = dx12.compiler_paths(repo_root)
    workers = dx12.worker_count(requested_jobs)
    heavy_names = {"fsr4_pass_7", "fsr4_pass_8", "fsr4_pass_9"}
    light_tasks = [task for task in tasks if task.job.name not in heavy_names]
    heavy_tasks = [task for task in tasks if task.job.name in heavy_names]
    if requested_jobs > 0:
        heavy_workers = workers
        total_memory = physical_memory_bytes()
    else:
        heavy_workers, total_memory = auto_heavy_worker_count(workers)
    memory_text = "unknown" if total_memory is None else f"{total_memory / (1024**3):.1f} GiB"
    print(
        f"[fsr4-vulkan-pack] unique compile jobs={len(tasks)} workers={workers} "
        f"heavy_workers={heavy_workers} heavy_jobs={len(heavy_tasks)} physical_memory={memory_text}",
        flush=True,
    )
    outputs: dict[tuple[object, ...], Path] = {}
    started = time.monotonic()
    completed = 0

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {}
        for index, task in enumerate(light_tasks):
            path = temp_root / f"light_{index:04d}.spv"
            futures[pool.submit(
                compile_task_to_path,
                repo_root,
                compiler_dir,
                dxc,
                task,
                path,
                timeout_seconds,
                dot_accumulate_lowering,
            )] = task
        for future in as_completed(futures):
            task = futures[future]
            try:
                outputs[task.identity] = future.result()
            except Exception as exc:  # noqa: BLE001
                raise SystemExit(f"failed compiling Vulkan shader {task.identity}: {exc}") from exc
            completed += 1
            if completed == len(light_tasks) or completed % 25 == 0:
                print(f"[fsr4-vulkan-pack] compiled {completed}/{len(tasks)} elapsed={time.monotonic() - started:.1f}s", flush=True)

    if heavy_tasks:
        print(
            f"[fsr4-vulkan-pack] light phase complete: {len(light_tasks)} shaders; "
            f"heavy phase: {len(heavy_tasks)} shaders with {heavy_workers} worker(s)",
            flush=True,
        )

    with ThreadPoolExecutor(max_workers=heavy_workers) as pool:
        futures = {}
        for index, task in enumerate(heavy_tasks):
            path = temp_root / f"heavy_{index:04d}.spv"
            futures[pool.submit(
                compile_task_to_path,
                repo_root,
                compiler_dir,
                dxc,
                task,
                path,
                timeout_seconds,
                dot_accumulate_lowering,
            )] = task
        for future in as_completed(futures):
            task = futures[future]
            try:
                outputs[task.identity] = future.result()
            except Exception as exc:  # noqa: BLE001
                raise SystemExit(f"failed compiling Vulkan shader {task.identity}: {exc}") from exc
            completed += 1
            if completed == len(tasks) or completed % 10 == 0:
                print(f"[fsr4-vulkan-pack] compiled {completed}/{len(tasks)} elapsed={time.monotonic() - started:.1f}s", flush=True)
    return outputs

def align4(value: int) -> int:
    return (value + 3) & ~3


def write_shader_pack(
    output_pack: Path,
    manifest_path: Path,
    pipeline_variant: str,
    logical_slices: int,
    tasks: list[CompileTask],
    entries: list[LogicalEntry],
    compiled: dict[tuple[object, ...], Path],
    dot_accumulate_lowering: str,
) -> None:
    identity_hash: dict[tuple[object, ...], str] = {}
    blobs_by_hash: dict[str, bytes] = {}
    package_capabilities: set[int] = set()
    package_extensions: set[str] = set()
    package_has_sdot = False
    package_has_sdot_acc_sat = False
    dot_audits: list[dot_accumulate.ModuleAudit] = []
    for task in tasks:
        path = compiled[task.identity]
        _bindings, capabilities, extensions = spirv_contract(path)
        package_capabilities.update(capabilities)
        package_extensions.update(extensions)
        package_has_sdot |= spirv_has_opcode(path, 4450)  # OpSDot / OpSDotKHR
        package_has_sdot_acc_sat |= spirv_has_opcode(path, 4453)  # OpSDotAccSat / KHR
        dot_audits.append(dot_accumulate.audit_file(path))
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        identity_hash[task.identity] = digest
        blobs_by_hash.setdefault(digest, data)

    vulkan_requirements = derive_vulkan_requirements(package_capabilities, package_extensions)
    if "shaderIntegerDotProduct" not in vulkan_requirements["extensionFeatures"] or \
            "SPV_KHR_integer_dot_product" not in package_extensions or not package_has_sdot:
        raise RuntimeError(
            "Vulkan DP4A product pack lost its native packed integer-dot contract; "
            "dot4add_i8packed must compile to SPV_KHR_integer_dot_product/OpSDot"
        )
    if dot_accumulate_lowering == "bounded-fused" and not package_has_sdot_acc_sat:
        raise RuntimeError(
            "bounded-fused Vulkan product pack lost its explicit accumulating DOT4 contract; "
            "expected at least one no-overflow-proven OpSDotAccSat"
        )
    if "computeDerivativeGroupLinear" not in vulkan_requirements["extensionFeatures"] or \
            "SPV_KHR_compute_shader_derivatives" not in package_extensions or \
            "VK_KHR_compute_shader_derivatives" not in vulkan_requirements["deviceExtensions"]:
        raise RuntimeError(
            "Vulkan product pack lost the SM 6.6 compute-quad topology contract; "
            "QuadRead* lowering requires SPV_KHR_compute_shader_derivatives/DerivativeGroupLinearKHR"
        )

    ordered_hashes = sorted(blobs_by_hash)
    blob_index = {digest: index for index, digest in enumerate(ordered_hashes)}
    entries_offset = PACK_HEADER.size
    blobs_offset = entries_offset + len(entries) * PACK_ENTRY.size
    data_offset = align4(blobs_offset + len(ordered_hashes) * PACK_BLOB.size)

    blob_records: list[tuple[int, int]] = []
    cursor = data_offset
    for digest in ordered_hashes:
        cursor = align4(cursor)
        data = blobs_by_hash[digest]
        blob_records.append((cursor, len(data)))
        cursor += len(data)
    total_bytes = cursor

    output_pack.parent.mkdir(parents=True, exist_ok=True)
    with output_pack.open("wb") as out:
        out.write(PACK_HEADER.pack(
            PACK_MAGIC,
            PACK_HEADER.size,
            len(entries),
            len(ordered_hashes),
            entries_offset,
            blobs_offset,
            data_offset,
            total_bytes,
        ))
        for entry in entries:
            out.write(PACK_ENTRY.pack(*entry.key(), blob_index[identity_hash[entry.identity]]))
        for offset, size in blob_records:
            out.write(PACK_BLOB.pack(offset, size))
        if out.tell() < data_offset:
            out.write(b"\0" * (data_offset - out.tell()))
        for digest, (offset, _size) in zip(ordered_hashes, blob_records):
            if out.tell() < offset:
                out.write(b"\0" * (offset - out.tell()))
            out.write(blobs_by_hash[digest])
        if out.tell() != total_bytes:
            raise RuntimeError("internal shader-pack size mismatch")

    manifest = {
        "format": "apusr-fsr4-vulkan-shader-pack",
        "pipelineVariant": pipeline_variant,
        "logicalSlices": logical_slices,
        "logicalEntries": len(entries),
        "uniqueCompileJobs": len(tasks),
        "physicalBlobs": len(ordered_hashes),
        "packBytes": total_bytes,
        "packSha256": hashlib.sha256(output_pack.read_bytes()).hexdigest(),
        "compilerProfile": {
            "vulkanTarget": VULKAN_TARGET,
            "spirvTarget": SPIRV_TARGET,
            "shaderModel": SHADER_MODEL,
            "hlslVersion": HLSL_VERSION,
            "optimization": OPTIMIZATION,
            "enable16BitTypes": True,
            "dxLayout": True,
            "descriptorSet": DESCRIPTOR_SET,
            "srvBindingBase": SRV_BINDING_BASE,
            "uavBindingBase": UAV_BINDING_BASE,
            "cbvBindingBase": CBV_BINDING_BASE,
            "samplerBindingBase": SAMPLER_BINDING_BASE,
            "dotAccumulateLowering": dot_accumulate_lowering,
            "vectorLoadStages": list(VECTOR_LOAD_STAGES),
            "vectorInitializerSrvRegister": VECTOR_INITIALIZER_SRV_REGISTER,
            "vectorScratchUavRegister": VECTOR_SCRATCH_UAV_REGISTER,
            "preInputCache": True,
            "preInputCacheMaxExtent": 17,
            "postAdaptiveInputCache": True,
            "postInputCacheMaxExtent": 18,
        },
        "dotAccumulate": {
            "signedDotInstructions": sum(item.signed_dot_instructions for item in dot_audits),
            "signedDotAccumulateInstructions": sum(item.signed_dot_accumulate_instructions for item in dot_audits),
            "remainingSplitDotAddPairs": sum(item.split_dot_add_pairs for item in dot_audits),
            "maxProvenChainDepth": max((item.max_proven_chain_depth for item in dot_audits), default=0),
            "signedDot4AbsoluteBound": dot_accumulate.SIGNED_DOT4_ABS_BOUND,
            "maximumExactChainDepth": dot_accumulate.MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH,
        },
        "vulkanRequirements": vulkan_requirements,
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"[fsr4-vulkan-pack] wrote {output_pack}: logical={len(entries)} compile_jobs={len(tasks)} "
        f"physical={len(ordered_hashes)} bytes={total_bytes}",
        flush=True,
    )


def generate_loose_slice(args: argparse.Namespace, repo_root: Path) -> None:
    if args.preset == "all" or args.capacity == "all":
        raise SystemExit("--output-root is a developer single-slice mode; choose one preset and capacity")
    options = dx12.PermutationOptions(
        wmma=False,
        depth_inverted=args.depth_inverted,
        low_res_motion_vectors=args.low_res_motion_vectors,
        auto_exposure=args.auto_exposure,
        jittered_motion_vectors=args.jittered_motion_vectors,
        color_space=args.color_space,
        debug_visualize=args.debug_visualize,
    )
    _pipeline_value, optimized_pass1, optimized_front = PIPELINE_VARIANTS[args.pipeline_variant]
    compiler_dir, dxc = dx12.compiler_paths(repo_root)
    permutation = dx12.permutation_key(args.preset, args.capacity, options)
    output_dir = args.output_root.resolve() / args.preset / args.capacity / f"p{permutation}"
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_shaders = []
    capabilities: set[int] = set()
    extensions: set[str] = set()
    for job in dx12.shader_jobs(args.preset, args.capacity, optimized_pass1, optimized_front, options.debug_visualize):
        path = output_dir / f"{job.name}.spv"
        compile_shader(
            repo_root,
            compiler_dir,
            dxc,
            job,
            path,
            args.shader_timeout_seconds,
            dx12.model_defines(args.capacity, options),
            args.dot_accumulate_lowering,
        )
        bindings, shader_capabilities, shader_extensions = spirv_contract(path)
        dot_audit = dot_accumulate.audit_file(path)
        capabilities.update(shader_capabilities)
        extensions.update(shader_extensions)
        manifest_shaders.append({
            "name": job.name,
            "entry": job.entry,
            "passKind": job.kind,
            "logicalStage": job.logical_stage,
            "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "bindings": bindings,
            "dotAccumulate": dot_audit.manifest(),
        })
    (output_dir / "manifest.json").write_text(json.dumps({
        "format": "apusr-fsr4-vulkan-developer-slice",
        "pipelineVariant": args.pipeline_variant,
        "preset": args.preset,
        "capacity": args.capacity,
        "permutationKey": permutation,
        "dotAccumulateLowering": args.dot_accumulate_lowering,
        "vectorLoadStages": list(VECTOR_LOAD_STAGES),
        "vectorInitializerSrvRegister": VECTOR_INITIALIZER_SRV_REGISTER,
        "vectorScratchUavRegister": VECTOR_SCRATCH_UAV_REGISTER,
        "preInputCache": True,
        "preInputCacheMaxExtent": 17,
        "postAdaptiveInputCache": True,
        "postInputCacheMaxExtent": 18,
        "vulkanRequirements": derive_vulkan_requirements(capabilities, extensions),
        "shaders": manifest_shaders,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"[fsr4-reference-spirv] wrote developer slice {output_dir}")


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    if args.output_root is None and args.output_pack is None:
        raise SystemExit("choose --output-root for a developer slice or --output-pack for a runtime shader pack")
    if args.output_root is not None and args.output_pack is not None:
        raise SystemExit("--output-root and --output-pack are separate modes")
    if args.output_root is not None:
        if args.product_matrix:
            raise SystemExit("--product-matrix emits a shader pack, not a loose shader directory")
        generate_loose_slice(args, repo_root)
        return 0

    presets = selected_presets(args.preset)
    capacities = selected_capacities(args.capacity)
    if args.product_matrix:
        options_list = dx12.product_matrix_options(args.include_debug)
    else:
        options_list = [dx12.PermutationOptions(
            wmma=False,
            depth_inverted=args.depth_inverted,
            low_res_motion_vectors=args.low_res_motion_vectors,
            auto_exposure=args.auto_exposure,
            jittered_motion_vectors=args.jittered_motion_vectors,
            color_space=args.color_space,
            debug_visualize=args.debug_visualize,
        )]
    tasks, entries = build_pack_plan(args.pipeline_variant, presets, capacities, options_list)
    logical_slices = len(presets) * len(capacities) * len(options_list)
    output_pack = args.output_pack.resolve()
    manifest = args.manifest.resolve() if args.manifest is not None else output_pack.with_suffix(output_pack.suffix + ".json")
    with tempfile.TemporaryDirectory(prefix="apusr_fsr4_vulkan_pack_") as temp_dir:
        compiled = compile_tasks(
            repo_root,
            tasks,
            Path(temp_dir),
            args.shader_timeout_seconds,
            args.jobs,
            args.dot_accumulate_lowering,
        )
        write_shader_pack(
            output_pack,
            manifest,
            args.pipeline_variant,
            logical_slices,
            tasks,
            entries,
            compiled,
            args.dot_accumulate_lowering,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
