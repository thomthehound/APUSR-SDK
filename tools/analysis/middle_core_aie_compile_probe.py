#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Compile salvageable middle-core AIE arithmetic sources with Peano.

This is a toolchain/arithmetic probe, not a product graph build. The old
full-arena graph is quarantined, but its C++ stage kernels are still useful
compile probes while the spatial rewrite grows new graph wiring.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import pathlib
import platform
import json
import re
import shutil
import subprocess
import sys
import sysconfig
from collections.abc import Iterable, Sequence

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
KERNEL_ROOT = REPO_ROOT / "kernels" / "quarantine" / "apusr_middle_core_stage1_12_full_arena_2col_failed_v0" / "src" / "aie"
SOURCE_FILES = [
    KERNEL_ROOT / "apusr_middle_core_convnext16_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_down2x2_fasternet32_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_fasternet32_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_fnb32_ct2d16_add_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_fnb32_c8group_ct2d16_add_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_down2x2_fasternet64_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_fasternet64_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_fnb64_ct2d32_add_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_ct2d32_ochunk_add_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_ct2d32_ochunk_halo4_add_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_fasternet32_c8group_tile.cc",
    KERNEL_ROOT / "apusr_middle_core_queue_bridge_tiles.cc",
]
TARGETS = ["aie2", "aie2p"]


def _exe(name: str) -> str:
    return f"{name}.exe" if platform.system().lower() == "windows" else name


def _dedupe_existing_paths(paths: Iterable[pathlib.Path]) -> list[pathlib.Path]:
    seen: set[pathlib.Path] = set()
    existing: list[pathlib.Path] = []
    for path in paths:
        try:
            resolved = path.expanduser().resolve()
        except OSError:
            continue
        if resolved in seen or not resolved.exists():
            continue
        seen.add(resolved)
        existing.append(resolved)
    return existing


def _sysconfig_path(name: str) -> pathlib.Path | None:
    value = sysconfig.get_path(name)
    return pathlib.Path(value) if value else None


def _candidate_script_dirs() -> list[pathlib.Path]:
    candidates: list[pathlib.Path] = []
    for key in ("scripts", "platlib", "purelib"):
        value = _sysconfig_path(key)
        if value:
            candidates.append(value)
    candidates.extend([
        pathlib.Path(sys.prefix) / "Scripts",
        pathlib.Path(sys.prefix) / "bin",
        pathlib.Path(sys.exec_prefix) / "Scripts",
        pathlib.Path(sys.exec_prefix) / "bin",
    ])
    return _dedupe_existing_paths(candidates)


def _probe_clang_target(clang: pathlib.Path, target: str) -> bool:
    command = [str(clang), f"--target={_target_triple(target)}", "-dM", "-E", "-"]
    try:
        process = subprocess.run(
            command,
            cwd=REPO_ROOT,
            input="",
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return process.returncode == 0


def _resolve_peano_tool(tool_name: str, explicit: str | None) -> pathlib.Path:
    if explicit:
        candidates = [pathlib.Path(explicit)]
    else:
        candidates = []
        for env_name in ("PEANO_INSTALL_DIR", "LLVM_AIE_INSTALL_DIR"):
            if os.environ.get(env_name):
                candidates.append(pathlib.Path(os.environ[env_name]) / "bin" / _exe(tool_name))
        candidates.extend(script_dir / _exe(tool_name) for script_dir in _candidate_script_dirs())
        which = shutil.which(_exe(tool_name)) or shutil.which(tool_name)
        if which:
            candidates.append(pathlib.Path(which))

    existing = _dedupe_existing_paths(candidates)
    if tool_name == "clang++":
        for tool in existing:
            if _probe_clang_target(tool, "aie2"):
                return tool
        searched = "\n  ".join(str(path) for path in existing) or "<none>"
        raise FileNotFoundError(f"could not find a Peano clang++ with AIE2 target support. Searched:\n  {searched}")
    if existing:
        return existing[0]
    searched = "\n  ".join(str(path) for path in candidates) or "<none>"
    raise FileNotFoundError(f"missing {tool_name}. Searched:\n  {searched}")


def _candidate_include_roots() -> list[pathlib.Path]:
    candidates: list[pathlib.Path] = []

    if os.environ.get("MLIR_AIE_INCLUDE_DIR"):
        candidates.append(pathlib.Path(os.environ["MLIR_AIE_INCLUDE_DIR"]))

    spec = importlib.util.find_spec("mlir_aie")
    if spec and spec.origin:
        package_root = pathlib.Path(spec.origin).resolve().parent
        candidates.append(package_root / "include")

    for key in ("purelib", "platlib"):
        value = _sysconfig_path(key)
        if value:
            candidates.append(value / "mlir_aie" / "include")

    for root_env in ("MLIR_AIE_INSTALL_DIR", "CONDA_PREFIX", "VIRTUAL_ENV"):
        root = os.environ.get(root_env)
        if not root:
            continue
        root_path = pathlib.Path(root)
        candidates.extend([
            root_path / "include",
            root_path / "Lib" / "site-packages" / "mlir_aie" / "include",
            root_path / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}" / "site-packages" / "mlir_aie" / "include",
        ])

    return candidates


def _resolve_include_root(explicit: str | None) -> pathlib.Path:
    candidates = [pathlib.Path(explicit)] if explicit else _candidate_include_roots()
    for include_root in _dedupe_existing_paths(candidates):
        if (include_root / "aie_api" / "aie.hpp").exists():
            return include_root
    searched = "\n  ".join(str(path) for path in candidates) or "<none>"
    raise FileNotFoundError(f"could not find MLIR-AIE include root containing aie_api/aie.hpp. Searched:\n  {searched}")


def _run(log_path: pathlib.Path, command: Sequence[str]) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write("+ " + " ".join(command) + "\n")
        log.flush()
        process = subprocess.run(command, cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        log.write(process.stdout)
    return process.returncode



def _analyze_objdump(log_path: pathlib.Path) -> dict[str, int]:
    text = log_path.read_text(encoding="utf-8", errors="replace").lower()
    return {
        "instruction_lines": len(re.findall(r"^\s*[0-9a-f]+:", text, flags=re.MULTILINE)),
        "mac_like": len(re.findall(r"\b(?:mac|msc|mmul|vmac|vmsc)\b", text)),
        "scalar_mul": len(re.findall(r"\bmul\b", text)),
        "srs_like": len(re.findall(r"\b(?:srs|ups|pack|unpack|vst\.srs)\b", text)),
        "vector_load_store_like": len(re.findall(r"\b(?:vld|vst|lda\.s8|st\.s8)\b", text)),
    }

def _write_empty_source(out_root: pathlib.Path) -> pathlib.Path:
    empty = out_root / "empty.cpp"
    empty.write_text("", encoding="utf-8")
    return empty


def _target_triple(target: str) -> str:
    if target not in TARGETS:
        raise ValueError(f"unsupported target: {target}")
    return f"{target}-none-unknown-elf"


def _compile_command(
    clang: pathlib.Path,
    include_root: pathlib.Path,
    target: str,
    source: pathlib.Path,
    output: pathlib.Path,
    extra_args: Sequence[str],
) -> list[str]:
    return [
        str(clang),
        f"--target={_target_triple(target)}",
        "-std=c++20",
        "-O2",
        f"-I{include_root}",
        f"-I{KERNEL_ROOT}",
        *extra_args,
        "-c",
        str(source),
        "-o",
        str(output),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-root", default=str(REPO_ROOT / "out" / "middle_core_aie_compile_probe"))
    parser.add_argument("--clang", help="Peano clang++ path. Defaults to PEANO_INSTALL_DIR/bin/clang++.")
    parser.add_argument("--objdump", help="Peano llvm-objdump path. Defaults to PEANO_INSTALL_DIR/bin/llvm-objdump.")
    parser.add_argument("--include-root", help="MLIR-AIE include root containing aie_api/aie.hpp.")
    parser.add_argument("--target", action="append", choices=TARGETS, help="Target to compile. May be repeated. Defaults to both.")
    parser.add_argument("--extra-clang-arg", action="append", default=[], help="Extra argument forwarded to clang++.")
    parser.add_argument("--keep-going", action="store_true", help="Compile every source/target before returning failure.")
    args = parser.parse_args()

    out_root = pathlib.Path(args.out_root).resolve()
    if out_root.exists():
        shutil.rmtree(out_root)
    out_root.mkdir(parents=True)
    log_root = out_root / "logs"
    object_root = out_root / "objects"

    clang = _resolve_peano_tool("clang++", args.clang)
    objdump = _resolve_peano_tool("llvm-objdump", args.objdump)
    include_root = _resolve_include_root(args.include_root)
    targets = args.target or TARGETS
    empty_source = _write_empty_source(out_root)

    disassembly_analysis: dict[str, dict[str, dict[str, int]]] = {}

    summary: list[str] = [
        f"repo_root={REPO_ROOT}",
        f"clang={clang}",
        f"objdump={objdump}",
        f"include_root={include_root}",
        f"targets={','.join(targets)}",
    ]

    failures = 0
    for target in targets:
        macro_log = log_root / f"{target}_macros.log"
        macro_rc = _run(
            macro_log,
            [str(clang), f"--target={_target_triple(target)}", "-dM", "-E", str(empty_source)],
        )
        summary.append(f"{target}.macros.rc={macro_rc}")
        if macro_rc != 0:
            failures += 1
            if not args.keep_going:
                break

        for source in SOURCE_FILES:
            object_path = object_root / target / f"{source.stem}.o"
            object_path.parent.mkdir(parents=True, exist_ok=True)
            compile_log = log_root / f"{target}_{source.stem}_compile.log"
            compile_rc = _run(
                compile_log,
                _compile_command(clang, include_root, target, source, object_path, args.extra_clang_arg),
            )
            summary.append(f"{target}.{source.stem}.compile.rc={compile_rc}")
            if compile_rc != 0:
                failures += 1
                if not args.keep_going:
                    break
                continue

            objdump_log = log_root / f"{target}_{source.stem}_objdump.log"
            objdump_rc = _run(objdump_log, [str(objdump), "-d", str(object_path)])
            summary.append(f"{target}.{source.stem}.objdump.rc={objdump_rc}")
            if objdump_rc == 0:
                disassembly_analysis.setdefault(target, {})[source.stem] = _analyze_objdump(objdump_log)
            if objdump_rc != 0:
                failures += 1
                if not args.keep_going:
                    break
        if failures and not args.keep_going:
            break

    analysis_path = out_root / "disassembly_analysis.json"
    analysis_path.write_text(json.dumps(disassembly_analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    summary_path = out_root / "summary.txt"
    summary.append(f"disassembly_analysis={analysis_path}")
    summary_path.write_text("\n".join(summary) + "\n", encoding="utf-8")
    print(f"middle-core AIE compile probe wrote: {out_root}")
    print(summary_path.read_text(encoding="utf-8"), end="")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
