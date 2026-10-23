#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Extract RGP CodeObject ELFs and run Radeon GPU Analyzer binary analysis.

This is a static-analysis companion to ``rgp_code_object_extract.py``.  It can
consume raw .rgp files, directories, or profile zip bundles, extract matching
AMDGPU CodeObject ELFs, and pass each ELF to AMD's command-line Radeon GPU
Analyzer (RGA) binary-analysis mode.

RGA gives us useful static ISA/liveness/resource outputs directly from RGP's
embedded code objects:

* disassembled ISA;
* resource-usage CSV;
* VGPR and SGPR liveness tables;
* optional basic-block CFG.

This still does not reproduce RGP's per-line Instruction Timing percentages or
cycles.  Those are sampled SQTT timing correlations produced by RGP, not RGA.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Any

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_code_object_extract as coextract  # noqa: E402
import rgp_rdf_summary as rdfmod  # noqa: E402

RN_LINE_RE = re.compile(r"^\s*(?P<line>\d+)\s*\|\s*(?P<rn>\d+)\s*\|.*?\|\s*(?P<inst>.*)$")


def _safe_label(text: str) -> str:
    out = []
    for ch in text:
        if ch.isalnum() or ch in ("-", "_", "."):
            out.append(ch)
        else:
            out.append("_")
    return "".join(out).strip("._") or "item"


def _extract_rdts_archive(archive: pathlib.Path, tmp_root: pathlib.Path) -> pathlib.Path:
    """Extract only the pieces needed for command-line RGA."""
    root_name: str | None = None
    with tarfile.open(archive, "r:*") as tf:
        members = tf.getmembers()
        for m in members:
            parts = pathlib.PurePosixPath(m.name).parts
            if parts:
                root_name = parts[0]
                break
        if not root_name:
            raise RuntimeError(f"unable to find root directory in {archive}")
        # Keep archive extraction small.  Binary-analysis mode only needs the
        # rga launcher/binary plus AMDGPU disassembly and SHAE liveness helpers.
        # Extracting the whole suite is hundreds of MB and slow in WSL/CI.
        wanted_exact = {
            f"{root_name}/rga",
            f"{root_name}/rga-bin",
            f"{root_name}/rga-bin-internal",
            f"{root_name}/RGA_README.md",
            f"{root_name}/LICENSE.txt",
            f"{root_name}/utils/shae",
            f"{root_name}/utils/lc/disassembler/amdgpu-dis",
        }
        selected = [m for m in members if m.name in wanted_exact]
        try:
            tf.extractall(tmp_root, members=selected, filter="data")
        except TypeError:  # Python < 3.12
            tf.extractall(tmp_root, members=selected)
    return tmp_root / root_name


def _find_rga(args: argparse.Namespace, tmp_root: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path | None]:
    if args.rga:
        p = pathlib.Path(args.rga)
        return p, None
    root: pathlib.Path | None = pathlib.Path(args.rdts_root) if args.rdts_root else None
    if args.rdts_archive:
        root = _extract_rdts_archive(pathlib.Path(args.rdts_archive), tmp_root)
    if root:
        for name in ("rga", "rga.exe", "RadeonGPUAnalyzerCLI.exe"):
            p = root / name
            if p.exists():
                return p, root
        # The Linux suite has rga plus rga-bin; Windows layouts may differ.
        for p in root.rglob("rga*"):
            if p.is_file() and os.access(p, os.X_OK):
                return p, root
        raise RuntimeError(f"could not find rga under {root}")
    for name in ("rga", "rga.exe"):
        found = shutil.which(name)
        if found:
            return pathlib.Path(found), None
    raise RuntimeError("RGA not found. Pass --rdts-root, --rdts-archive, or --rga.")


def _env_for_rga(rga: pathlib.Path, root: pathlib.Path | None) -> dict[str, str]:
    env = os.environ.copy()
    candidates: list[pathlib.Path] = []
    if root:
        candidates.append(root / "lib")
        candidates.append(root / "utils" / "dx12" / "dxc")
    candidates.append(rga.parent / "lib")
    lib_paths = [str(p) for p in candidates if p.exists()]
    if lib_paths:
        old = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = os.pathsep.join(lib_paths + ([old] if old else []))
    return env


def _read_analysis_csv(path: pathlib.Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    with path.open(newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        try:
            row = next(reader)
        except StopIteration:
            return {}
    out: dict[str, Any] = {}
    for key, value in row.items():
        if value is None:
            continue
        value = value.strip()
        if not value:
            out[key.lower()] = value
            continue
        try:
            out[key.lower()] = int(value)
        except ValueError:
            out[key.lower()] = value
    return out


def _scan_livereg(path: pathlib.Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "max_live": "",
        "max_live_line": "",
        "max_live_instruction": "",
    }
    if not path.exists():
        return result
    max_rn = -1
    max_line = ""
    max_inst = ""
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RN_LINE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            rn = int(m.group("rn"))
            if rn > max_rn:
                max_rn = rn
                max_line = m.group("line")
                max_inst = m.group("inst").strip()
    if max_rn >= 0:
        result.update({"max_live": max_rn, "max_live_line": max_line, "max_live_instruction": max_inst})
    return result


def _find_one(directory: pathlib.Path, pattern: str) -> pathlib.Path | None:
    matches = sorted(directory.glob(pattern))
    return matches[0] if matches else None


def _run_rga_one(rga: pathlib.Path, rdts_root: pathlib.Path | None, elf_path: pathlib.Path, out_dir: pathlib.Path, with_cfg: bool, timeout_s: int) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    # RGA writes per-device files in the output directory using the requested
    # filenames as a hint.  Use a private directory per code object to avoid
    # collisions and keep all derived files together.
    cmd = [
        str(rga),
        "-s",
        "bin",
        "--co",
        str(elf_path),
        "--isa",
        str(out_dir / "isa.txt"),
        "-a",
        str(out_dir / "analysis.csv"),
        "--livereg",
        str(out_dir / "livereg.txt"),
        "--livereg-sgpr",
        str(out_dir / "livereg_sgpr.txt"),
    ]
    if with_cfg:
        cmd.extend(["--cfg", str(out_dir / "cfg.dot")])
    proc = subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=_env_for_rga(rga, rdts_root),
        check=False,
        timeout=timeout_s,
    )
    log_path = out_dir / "rga.log"
    log_path.write_text(proc.stdout, encoding="utf-8", errors="replace")
    analysis_path = _find_one(out_dir, "*_analysis_*.csv") or _find_one(out_dir, "analysis*.csv")
    isa_path = _find_one(out_dir, "*_isa_*.txt") or _find_one(out_dir, "isa*.txt")
    livereg_path = _find_one(out_dir, "*_livereg_*.txt") or _find_one(out_dir, "livereg*.txt")
    livereg_sgpr_path = _find_one(out_dir, "*_livereg_sgpr_*.txt") or _find_one(out_dir, "livereg_sgpr*.txt")
    cfg_path = _find_one(out_dir, "*_cfg_*.dot") or _find_one(out_dir, "cfg*.dot")

    analysis = _read_analysis_csv(analysis_path) if analysis_path else {}
    vgpr_live = _scan_livereg(livereg_path) if livereg_path else {}
    sgpr_live = _scan_livereg(livereg_sgpr_path) if livereg_sgpr_path else {}

    status = "ok" if proc.returncode == 0 else f"rga_exit_{proc.returncode}"
    return {
        "rga_status": status,
        "rga_log_path": str(log_path),
        "rga_analysis_path": str(analysis_path or ""),
        "rga_isa_path": str(isa_path or ""),
        "rga_livereg_path": str(livereg_path or ""),
        "rga_livereg_sgpr_path": str(livereg_sgpr_path or ""),
        "rga_cfg_path": str(cfg_path or ""),
        "rga_device": analysis.get("device", ""),
        "rga_used_vgprs": analysis.get("used_vgprs", ""),
        "rga_used_sgprs": analysis.get("used_sgprs", ""),
        "rga_used_lds_bytes": analysis.get("used_lds_bytes", ""),
        "rga_scratch_mem": analysis.get("scratch_mem", ""),
        "rga_vgpr_spills": analysis.get("vgpr_spills", ""),
        "rga_sgpr_spills": analysis.get("sgpr_spills", ""),
        "rga_isa_size": analysis.get("isa_size", ""),
        "rga_max_live_vgprs": vgpr_live.get("max_live", ""),
        "rga_max_live_vgprs_line": vgpr_live.get("max_live_line", ""),
        "rga_max_live_vgprs_instruction": vgpr_live.get("max_live_instruction", ""),
        "rga_max_live_sgprs": sgpr_live.get("max_live", ""),
        "rga_max_live_sgprs_line": sgpr_live.get("max_live_line", ""),
        "rga_max_live_sgprs_instruction": sgpr_live.get("max_live_instruction", ""),
    }


def _extract_rows(captures: list[pathlib.Path], out_dir: pathlib.Path, args: argparse.Namespace) -> list[dict[str, Any]]:
    extract_args = argparse.Namespace(
        stage=args.stage,
        workgroup=args.workgroup,
        chunk=args.chunk,
        code_object_index=args.code_object_index,
        min_lds=args.min_lds,
        max_lds=args.max_lds,
        min_vgprs=args.min_vgprs,
        max_vgprs=args.max_vgprs,
        min_sgprs=args.min_sgprs,
        max_sgprs=args.max_sgprs,
        pipeline_hash=args.pipeline_hash,
        api_pso_hash=args.api_pso_hash,
        sort_by=args.sort_by,
        descending=args.descending,
        limit=args.limit,
        extract_text=False,
        disassemble=False,
        mcpu="gfx1151",
    )
    rows: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="apusr_rgp_rga_extract_") as tmp_s:
        caps = rdfmod._capture_paths(captures, pathlib.Path(tmp_s))
        if not caps:
            raise SystemExit("no .rgp/.rmv files found")
        co_dir = out_dir / "code_objects"
        for cap in caps:
            rows.extend(coextract.extract_from_capture(cap, co_dir, extract_args, objdump=None))
    return rows


def _fieldnames(rows: list[dict[str, Any]]) -> list[str]:
    preferred = [
        "capture",
        "chunk_ordinal",
        "code_object_index",
        "stage",
        "threadgroup_dimensions",
        "wavefront_size",
        "vgprs",
        "sgprs",
        "lds_size",
        "scratch_memory_size",
        "api_pso_hash_hex",
        "internal_pipeline_hash_hex",
        "elf_path",
        "rga_status",
        "rga_device",
        "rga_used_vgprs",
        "rga_used_sgprs",
        "rga_used_lds_bytes",
        "rga_scratch_mem",
        "rga_vgpr_spills",
        "rga_sgpr_spills",
        "rga_isa_size",
        "rga_max_live_vgprs",
        "rga_max_live_vgprs_line",
        "rga_max_live_vgprs_instruction",
        "rga_max_live_sgprs",
        "rga_max_live_sgprs_line",
        "rga_max_live_sgprs_instruction",
        "rga_isa_path",
        "rga_livereg_path",
        "rga_livereg_sgpr_path",
        "rga_cfg_path",
        "rga_log_path",
    ]
    extra = sorted(set().union(*(r.keys() for r in rows)) - set(preferred)) if rows else []
    return preferred + extra


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Extract RGP code objects and run RGA binary analysis/liveness.")
    ap.add_argument("capture", type=pathlib.Path, nargs="+", help=".rgp/.rmv, directory, or zip bundle")
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("rgp_rga_report"))
    ap.add_argument("--rdts-root", help="RadeonDeveloperToolSuite root containing rga/rga-bin/utils/lib")
    ap.add_argument("--rdts-archive", help="RadeonDeveloperToolSuite .tgz/.tar.gz; needed RGA pieces are extracted temporarily")
    ap.add_argument("--rga", help="explicit path to rga/rga.exe")
    ap.add_argument("--stage", help="filter by hardware stage, e.g. CS")
    ap.add_argument("--workgroup", type=rdfmod.parse_workgroup, help="filter compute code objects by threadgroup dimensions X,Y,Z")
    ap.add_argument("--chunk", type=int, help="extract one RDF chunk ordinal")
    ap.add_argument("--code-object-index", type=int, help="extract one CodeObject index")
    ap.add_argument("--min-lds", type=int)
    ap.add_argument("--max-lds", type=int)
    ap.add_argument("--min-vgprs", type=int)
    ap.add_argument("--max-vgprs", type=int)
    ap.add_argument("--min-sgprs", type=int)
    ap.add_argument("--max-sgprs", type=int)
    ap.add_argument("--pipeline-hash", help="filter by internal pipeline hash substring")
    ap.add_argument("--api-pso-hash", help="filter by API PSO hash substring")
    ap.add_argument("--sort-by", default="chunk_ordinal", choices=("chunk_ordinal", "code_object_index", "lds_size", "vgprs", "sgprs", "code_object_bytes", "elf_text_bytes"))
    ap.add_argument("--descending", action="store_true")
    ap.add_argument("--limit", type=int, default=8, help="maximum matching code objects to analyze")
    ap.add_argument("--with-cfg", action="store_true", help="also ask RGA/SHAE for basic-block CFG dot files")
    ap.add_argument("--timeout", type=int, default=90, help="seconds per code object for RGA")
    ap.add_argument("--json", action="store_true", help="also write rga_code_object_report.json")
    args = ap.parse_args(argv)

    if args.limit == 0:
        raise SystemExit("--limit 0 would analyze nothing")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="apusr_rdts_") as tmp_s:
        rga, rdts_root = _find_rga(args, pathlib.Path(tmp_s))
        rows = _extract_rows(args.capture, args.out_dir, args)
        report_rows: list[dict[str, Any]] = []
        for i, row in enumerate(rows):
            elf_path = pathlib.Path(str(row.get("elf_path", "")))
            label = _safe_label(
                f"{i:03d}_chunk{row.get('chunk_ordinal')}_co{row.get('code_object_index')}_"
                f"{row.get('stage')}_tg{row.get('threadgroup_dimensions')}_vgpr{row.get('vgprs')}_lds{row.get('lds_size')}"
            )
            rga_out = args.out_dir / "rga" / label
            analysis = _run_rga_one(rga, rdts_root, elf_path, rga_out, args.with_cfg, args.timeout)
            merged = dict(row)
            merged.update(analysis)
            report_rows.append(merged)

    csv_path = args.out_dir / "rga_code_object_report.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=_fieldnames(report_rows), extrasaction="ignore")
        writer.writeheader()
        writer.writerows(report_rows)
    if args.json:
        (args.out_dir / "rga_code_object_report.json").write_text(json.dumps(report_rows, indent=2, sort_keys=True), encoding="utf-8")

    print(f"analyzed {len(report_rows)} code object(s)")
    print(f"report: {csv_path}")
    print(f"rga: {rga}")
    if report_rows:
        print("first rows:")
        for row in report_rows[: min(8, len(report_rows))]:
            print(
                f"  chunk={row.get('chunk_ordinal')} co={row.get('code_object_index')} "
                f"stage={row.get('stage')} tg={row.get('threadgroup_dimensions')} "
                f"vgpr={row.get('vgprs')} sgpr={row.get('sgprs')} lds={row.get('lds_size')} "
                f"rga_vgpr={row.get('rga_used_vgprs')} max_live_vgpr={row.get('rga_max_live_vgprs')} "
                f"status={row.get('rga_status')}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
