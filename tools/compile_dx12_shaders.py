#!/usr/bin/env python3
"""Compile APUSR DX12 bookend shaders into the checked-in DXIL blob header."""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import signal
import stat
import subprocess
import time
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ShaderJob:
    name: str
    source: Path
    symbol: str
    defines: tuple[str, ...] = ()


SHADER_TARGET = "cs_6_6"
SHADER_ENTRY = "main"
SHADER_RECIPE_VERSION = "apusr-dx12-embedded-v1"
INCLUDE_RE = re.compile(rb'^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]', re.MULTILINE)
SHADERS = (
    ShaderJob(
        name="apusr_fsr4_auto_exposure_reduce",
        source=Path("shaders/dx12/apusr_fsr4_auto_exposure_reduce.hlsl"),
        symbol="Fsr4AutoExposureReduce",
    ),
    ShaderJob(
        name="apusr_fsr4_auto_exposure_resolve",
        source=Path("shaders/dx12/apusr_fsr4_auto_exposure_resolve.hlsl"),
        symbol="Fsr4AutoExposureResolve",
    ),
    ShaderJob(
        name="apusr_fsr4_capture_model_input_packed",
        source=Path("shaders/dx12/apusr_fsr4_capture_model_input.hlsl"),
        symbol="Fsr4CaptureModelInputPacked",
        defines=("APUSR_DEBUG_HALF4_SIDECAR=0",),
    ),
    ShaderJob(
        name="apusr_fsr4_capture_model_input_half4",
        source=Path("shaders/dx12/apusr_fsr4_capture_model_input.hlsl"),
        symbol="Fsr4CaptureModelInputHalf4",
        defines=("APUSR_DEBUG_HALF4_SIDECAR=1",),
    ),
    ShaderJob(
        name="apusr_fsr4_export_tokens_packed",
        source=Path("shaders/dx12/apusr_fsr4_export_tokens.hlsl"),
        symbol="Fsr4ExportTokensPacked",
        defines=("APUSR_DEBUG_HALF4_SIDECAR=0",),
    ),
    ShaderJob(
        name="apusr_fsr4_export_tokens_half4",
        source=Path("shaders/dx12/apusr_fsr4_export_tokens.hlsl"),
        symbol="Fsr4ExportTokensHalf4",
        defines=("APUSR_DEBUG_HALF4_SIDECAR=1",),
    ),
    ShaderJob(
        name="apusr_fsr4_import_tokens_packed",
        source=Path("shaders/dx12/apusr_fsr4_import_tokens.hlsl"),
        symbol="Fsr4ImportTokensPacked",
        defines=("APUSR_DEBUG_HALF4_SIDECAR=0",),
    ),
    ShaderJob(
        name="apusr_fsr4_import_tokens_half4",
        source=Path("shaders/dx12/apusr_fsr4_import_tokens.hlsl"),
        symbol="Fsr4ImportTokensHalf4",
        defines=("APUSR_DEBUG_HALF4_SIDECAR=1",),
    ),
    ShaderJob(
        name="apusr_roundtrip_copy_verify",
        source=Path("shaders/dx12/apusr_roundtrip_copy_verify.hlsl"),
        symbol="RoundTripCopyVerify",
    ),
    ShaderJob(
        name="apusr_fsr4_rcas",
        source=Path("shaders/dx12/apusr_fsr4_rcas.hlsl"),
        symbol="Fsr4Rcas",
    ),
)


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="APUSR repository root. Defaults to the parent of tools/.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repo_root / "src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_shader_blobs.h",
        help="Header to write or verify.",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Compile to a temporary header and fail if it differs from --output.",
    )
    parser.add_argument(
        "--shader-timeout-seconds",
        type=int,
        default=20,
        help="Maximum wall time allowed for each individual DXC invocation.",
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
        creationflags=creationflags,
        start_new_session=start_new_session,
    )
    start = time.monotonic()
    while True:
        return_code = process.poll()
        if return_code is not None:
            if return_code != 0:
                raise subprocess.CalledProcessError(return_code, command)
            return
        if time.monotonic() - start > timeout_seconds:
            terminate_process_tree(process)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
            raise subprocess.TimeoutExpired(command, timeout_seconds)
        time.sleep(0.05)

def compile_shader(
    repo_root: Path,
    compiler_dir: Path,
    dxc: Path,
    job: ShaderJob,
    output_path: Path,
    timeout_seconds: int,
) -> None:
    source_path = repo_root / job.source
    if not source_path.is_file():
        raise SystemExit(f"Shader source not found: {source_path}")

    env = os.environ.copy()
    if platform.system().lower() == "windows":
        path_value = str(compiler_dir)
        if env.get("PATH"):
            path_value = path_value + os.pathsep + env["PATH"]
        env["PATH"] = path_value
    else:
        library_path = str(compiler_dir)
        if env.get("LD_LIBRARY_PATH"):
            library_path = library_path + os.pathsep + env["LD_LIBRARY_PATH"]
        env["LD_LIBRARY_PATH"] = library_path

    command = [
        str(dxc),
        "-T",
        SHADER_TARGET,
        "-E",
        SHADER_ENTRY,
        "-I",
        str(repo_root / "shaders"),
    ]
    for define in job.defines:
        command.extend(["-D", define])
    command.extend([
        "-Fo",
        str(output_path),
        str(source_path),
    ])
    print(f"[shader] compiling {job.name} ({SHADER_TARGET})", flush=True)
    start = time.monotonic()
    try:
        run_bounded_child(command, repo_root, env, timeout_seconds)
        elapsed = time.monotonic() - start
        print(f"[shader] compiled {job.name} in {elapsed:.1f}s", flush=True)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(
            f"DXC timed out after {timeout_seconds}s while compiling {job.name}: {source_path}"
        ) from exc


def shader_source_closure(repo_root: Path, job: ShaderJob) -> list[Path]:
    """Return the deterministic transitive include closure for one embedded shader."""
    pending = [repo_root / job.source]
    visited: set[Path] = set()
    ordered: list[Path] = []
    while pending:
        path = pending.pop()
        path = path.resolve()
        if path in visited:
            continue
        if not path.is_file():
            raise SystemExit(f"Shader dependency not found: {path}")
        visited.add(path)
        ordered.append(path)
        data = path.read_bytes()
        includes: list[Path] = []
        for match in INCLUDE_RE.finditer(data):
            include_name = match.group(1).decode("utf-8")
            local = (path.parent / include_name).resolve()
            shared = (repo_root / "shaders" / include_name).resolve()
            if local.is_file():
                includes.append(local)
            elif shared.is_file():
                includes.append(shared)
            else:
                raise SystemExit(f"Shader include not found: {include_name} (from {path})")
        pending.extend(reversed(includes))
    return sorted(ordered, key=lambda path: path.relative_to(repo_root).as_posix())


def shader_source_fingerprint(repo_root: Path, job: ShaderJob) -> str:
    """Hash source, transitive includes, and the compile recipe without host-DXC bytes."""
    digest = hashlib.sha256()
    recipe = "\n".join((
        SHADER_RECIPE_VERSION,
        f"target={SHADER_TARGET}",
        f"entry={SHADER_ENTRY}",
        *(f"define={define}" for define in job.defines),
    ))
    digest.update(recipe.encode("utf-8"))
    digest.update(b"\0")
    for path in shader_source_closure(repo_root, job):
        relative = path.relative_to(repo_root).as_posix().encode("utf-8")
        digest.update(relative)
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def embedded_blob_bytes(header: str, job: ShaderJob) -> bytes | None:
    array_name = f"k{job.symbol}Dxil"
    match = re.search(
        rf"inline constexpr std::uint8_t {re.escape(array_name)}\[\] = \{{(?P<body>.*?)\n\}};",
        header,
        flags=re.DOTALL,
    )
    if match is None:
        return None
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group("body")))


def metadata_hash(header: str, kind: str, job: ShaderJob) -> str | None:
    match = re.search(
        rf"^// APUSR-{re.escape(kind)}-SHA256 {re.escape(job.symbol)} ([0-9a-f]{{64}})$",
        header,
        flags=re.MULTILINE,
    )
    return match.group(1) if match is not None else None


def verify_header(repo_root: Path, output: Path, shader_timeout_seconds: int) -> None:
    """Compile-smoke current sources and verify host-independent checked-in freshness metadata."""
    compiler_dir, dxc = compiler_paths(repo_root)
    print(
        f"[shader] verifying {len(SHADERS)} shader variant(s); "
        f"per-child timeout={shader_timeout_seconds}s",
        flush=True,
    )
    with tempfile.TemporaryDirectory(prefix="apusr_dxil_verify_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        for job in SHADERS:
            dxil_path = temp_dir / f"{job.name}.dxil"
            compile_shader(repo_root, compiler_dir, dxc, job, dxil_path, shader_timeout_seconds)
            compiled = dxil_path.read_bytes()
            if len(compiled) < 4 or compiled[:4] != b"DXBC":
                raise SystemExit(f"DXC produced an invalid DXIL container for {job.name}")

    header = output.read_text(encoding="utf-8")
    stale: list[str] = []
    corrupt: list[str] = []
    for job in SHADERS:
        recorded_source = metadata_hash(header, "SOURCE", job)
        current_source = shader_source_fingerprint(repo_root, job)
        if recorded_source != current_source:
            stale.append(job.name)

        blob = embedded_blob_bytes(header, job)
        recorded_blob = metadata_hash(header, "BLOB", job)
        if blob is None or recorded_blob is None or hashlib.sha256(blob).hexdigest() != recorded_blob:
            corrupt.append(job.name)

    if stale:
        raise SystemExit(
            "DX12 shader blobs are stale for: " + ", ".join(stale) +
            ". Run tools/compile_dx12_shaders.py on a supported host and commit the updated header."
        )
    if corrupt:
        raise SystemExit(
            "DX12 embedded shader header integrity check failed for: " + ", ".join(corrupt) +
            ". Regenerate tools/compile_dx12_shaders.py output."
        )

def c_array_initializer(data: bytes) -> str:
    lines: list[str] = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    return "\n".join(lines)


def write_header(repo_root: Path, output: Path, shader_timeout_seconds: int) -> None:
    compiler_dir, dxc = compiler_paths(repo_root)
    compiled: list[tuple[ShaderJob, bytes]] = []
    print(
        f"[shader] verifying {len(SHADERS)} shader variant(s); "
        f"per-child timeout={shader_timeout_seconds}s",
        flush=True,
    )
    with tempfile.TemporaryDirectory(prefix="apusr_dxil_") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        for job in SHADERS:
            dxil_path = temp_dir / f"{job.name}.dxil"
            compile_shader(repo_root, compiler_dir, dxc, job, dxil_path, shader_timeout_seconds)
            compiled.append((job, dxil_path.read_bytes()))

    parts = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// Copyright (c) 2026 thomthehound.",
        "// Auto-generated by tools/compile_dx12_shaders.py.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace apusr::frontends::shader_blobs {",
        "",
        "struct ShaderBlob {",
        "    const std::uint8_t* data;",
        "    std::size_t size;",
        "};",
        "",
    ]

    for job, data in compiled:
        array_name = f"k{job.symbol}Dxil"
        parts.append(f"// APUSR-SOURCE-SHA256 {job.symbol} {shader_source_fingerprint(repo_root, job)}")
        parts.append(f"// APUSR-BLOB-SHA256 {job.symbol} {hashlib.sha256(data).hexdigest()}")
        parts.append(f"inline constexpr std::uint8_t {array_name}[] = {{")
        parts.append(c_array_initializer(data))
        parts.append("};")
        parts.append("")
        parts.append(
            f"inline constexpr ShaderBlob {job.symbol[0].lower() + job.symbol[1:]}() noexcept {{"
        )
        parts.append(f"    return ShaderBlob{{{array_name}, sizeof({array_name})}};")
        parts.append("}")
        parts.append("")

    parts.append("} // namespace apusr::frontends::shader_blobs")
    parts.append("")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(parts), encoding="utf-8", newline="\n")


def main() -> None:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    output = args.output.resolve()

    if args.verify:
        verify_header(repo_root, output, args.shader_timeout_seconds)
        return

    write_header(repo_root, output, args.shader_timeout_seconds)


if __name__ == "__main__":
    main()
