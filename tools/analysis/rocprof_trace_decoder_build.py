#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Build AMD ROCprof Trace Decoder from a rocm-systems checkout or archive.

The decoder source currently lives at:
    rocm-systems/projects/rocprof-trace-decoder

This helper does not vendor the full ROCm monorepo into APUSR.  It consumes an
external checkout/archive and writes the build products into a local output
directory.  The primary output is librocprof-trace-decoder, which can be used by
``rgp_sqtt_decode_experimental.py``.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from typing import Any


def _extract_archive(archive: pathlib.Path, dst: pathlib.Path) -> pathlib.Path:
    dst.mkdir(parents=True, exist_ok=True)
    if archive.suffix.lower() == ".zip":
        with zipfile.ZipFile(archive, "r") as zf:
            zf.extractall(dst)
    elif archive.suffix.lower() in {".tgz", ".gz"} or archive.name.endswith(".tar.gz"):
        with tarfile.open(archive, "r:*") as tf:
            tf.extractall(dst)
    else:
        raise ValueError(f"unsupported archive type: {archive}")
    return dst


def _find_decoder_root(root: pathlib.Path) -> pathlib.Path:
    candidates = [
        root,
        root / "projects" / "rocprof-trace-decoder",
        root / "rocm-systems-develop" / "projects" / "rocprof-trace-decoder",
        root / "rocm-systems" / "projects" / "rocprof-trace-decoder",
    ]
    for c in candidates:
        if (c / "CMakeLists.txt").is_file() and (c / "include" / "rocprof_trace_decoder" / "rocprof_trace_decoder.h").is_file():
            return c.resolve()
    for c in root.rglob("rocprof-trace-decoder"):
        if (c / "CMakeLists.txt").is_file() and (c / "include" / "rocprof_trace_decoder" / "rocprof_trace_decoder.h").is_file():
            return c.resolve()
    raise FileNotFoundError(f"could not find projects/rocprof-trace-decoder under {root}")


def _run(cmd: list[str], *, cwd: pathlib.Path | None = None) -> None:
    print("+", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)


def _find_lib(build_dir: pathlib.Path) -> pathlib.Path:
    names = ["librocprof-trace-decoder.so", "librocprof-trace-decoder.dylib", "rocprof-trace-decoder.dll"]
    for name in names:
        hits = sorted(build_dir.rglob(name))
        if hits:
            return hits[0].resolve()
    hits = sorted(build_dir.rglob("*rocprof*trace*decoder*"))
    for h in hits:
        if h.is_file() and h.suffix in {".so", ".dylib", ".dll"}:
            return h.resolve()
    raise FileNotFoundError(f"built library not found under {build_dir}")


def _build_tool(decoder_root: pathlib.Path, build_dir: pathlib.Path, lib_path: pathlib.Path, tool_out: pathlib.Path) -> pathlib.Path:
    src = pathlib.Path(__file__).resolve().with_name("rocprof_trace_aggregate.cpp")
    if not src.is_file():
        raise FileNotFoundError(src)
    compiler = os.environ.get("CXX", "c++")
    include = decoder_root / "include"
    libdir = lib_path.parent
    exe = tool_out / ("rocprof_trace_aggregate.exe" if os.name == "nt" else "rocprof_trace_aggregate")
    tool_out.mkdir(parents=True, exist_ok=True)
    cmd = [
        compiler,
        "-std=c++20",
        "-O3",
        f"-I{include}",
        str(src),
        f"-L{libdir}",
        "-lrocprof-trace-decoder",
        f"-Wl,-rpath,{libdir}",
        "-o",
        str(exe),
    ]
    _run(cmd)
    return exe.resolve()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Build rocprof-trace-decoder from rocm-systems source.")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--rocm-systems-root", type=pathlib.Path, help="rocm-systems checkout root or projects/rocprof-trace-decoder directory")
    src.add_argument("--rocm-systems-archive", type=pathlib.Path, help="zip/tar archive containing rocm-systems")
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("build/rocprof_trace_decoder"))
    ap.add_argument("--build-type", default="Release")
    ap.add_argument("--generator", default="Ninja")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--build-aggregate-tool", action="store_true", help="also build APUSR's fast PC-latency aggregation CLI")
    ns = ap.parse_args(argv)

    ns.out_dir.mkdir(parents=True, exist_ok=True)
    temp_ctx: tempfile.TemporaryDirectory[str] | None = None
    try:
        if ns.rocm_systems_archive:
            extract_root = ns.out_dir / "src_extract"
            if extract_root.exists():
                shutil.rmtree(extract_root)
            _extract_archive(ns.rocm_systems_archive.resolve(), extract_root)
            root = extract_root
        else:
            root = ns.rocm_systems_root.resolve()
        decoder_root = _find_decoder_root(root)
        build_dir = ns.out_dir / "decoder_build"
        cmake_cmd = [
            "cmake",
            "-S",
            str(decoder_root),
            "-B",
            str(build_dir),
            "-G",
            ns.generator,
            f"-DCMAKE_BUILD_TYPE={ns.build_type}",
            "-DBUILD_TESTS=OFF",
            "-DDISABLE_COMGR=ON",
        ]
        _run(cmake_cmd)
        _run(["cmake", "--build", str(build_dir), f"-j{ns.jobs}"])
        lib_path = _find_lib(build_dir)
        report: dict[str, Any] = {
            "decoder_root": str(decoder_root),
            "build_dir": str(build_dir.resolve()),
            "library": str(lib_path),
        }
        if ns.build_aggregate_tool:
            report["aggregate_tool"] = str(_build_tool(decoder_root, build_dir, lib_path, ns.out_dir / "bin"))
        (ns.out_dir / "rocprof_trace_decoder_build.json").write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
        print(json.dumps(report, indent=2, sort_keys=True))
    finally:
        if temp_ctx is not None:
            temp_ctx.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
