#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Extract AMDGPU code-object ELFs from RGP/RDF captures.

RGP stores loaded shader code objects as ELF blobs inside ``CodeObject`` RDF
chunks.  This tool lets us pull those ELFs out of a raw ``.rgp`` file, a
profile directory, or a zip bundle and optionally run an AMDGPU-capable
``llvm-objdump`` over the result.

Important boundary: this extracts static shader code objects.  RGP's per-line
Instruction Timing table is derived from SQTT/ISA correlation in the GUI and is
not currently decoded here.  Use this to avoid manual shader-code-object export;
manual RGP instruction-timing text export is still needed until QueueEvent/SQTT
correlation is implemented.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Sequence

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_rdf_summary as rdfmod  # noqa: E402


def _safe_label(text: str) -> str:
    out = []
    for ch in text:
        if ch.isalnum() or ch in ("-", "_", "."):
            out.append(ch)
        else:
            out.append("_")
    value = "".join(out).strip("._")
    return value or "code_object"


def _row_matches(row: dict[str, Any], args: argparse.Namespace) -> bool:
    stage = args.stage.lower().lstrip(".") if args.stage else None
    if stage and str(row.get("stage", "")).lower() != stage:
        return False
    if args.workgroup and row.get("threadgroup_dimensions") != args.workgroup:
        return False
    if args.chunk is not None and int(row.get("chunk_ordinal", -1)) != args.chunk:
        return False
    if args.code_object_index is not None and int(row.get("code_object_index", -1)) != args.code_object_index:
        return False
    for opt, field, cmp in (
        (args.min_lds, "lds_size", "min"),
        (args.max_lds, "lds_size", "max"),
        (args.min_vgprs, "vgprs", "min"),
        (args.max_vgprs, "vgprs", "max"),
        (args.min_sgprs, "sgprs", "min"),
        (args.max_sgprs, "sgprs", "max"),
    ):
        if opt is None:
            continue
        value = row.get(field)
        if value is None:
            return False
        if cmp == "min" and int(value) < opt:
            return False
        if cmp == "max" and int(value) > opt:
            return False
    if args.pipeline_hash:
        needle = args.pipeline_hash.lower().replace("-", ":")
        if needle not in str(row.get("internal_pipeline_hash_hex") or "").lower():
            return False
    if args.api_pso_hash:
        needle = args.api_pso_hash.lower()
        if needle not in str(row.get("api_pso_hash_hex") or "").lower():
            return False
    return True


def _sort_key(row: dict[str, Any], key: str) -> Any:
    value = row.get(key)
    if value is None:
        return -1
    if isinstance(value, list):
        return tuple(value)
    return value


def _objdump_supports_amdgpu(path: str) -> bool:
    try:
        proc = subprocess.run([path, "--version"], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=8)
    except Exception:
        return False
    text = proc.stdout.lower()
    # Upstream LLVM/ROCm objdump lists the AMDGPU backend as amdgcn.  Some
    # distro or Swift LLVM builds omit it; those can read the ELF header but
    # cannot disassemble the code object.
    return "amdgcn" in text or "amdgpu" in text


def _find_objdump(user_path: str | None) -> str | None:
    if user_path:
        p = pathlib.Path(user_path)
        return str(p) if p.exists() else user_path
    for name in (
        "llvm-objdump",
        "llvm-objdump.exe",
        "amdgpu-objdump",
        "roc-objdump",
    ):
        found = shutil.which(name)
        if found and _objdump_supports_amdgpu(found):
            return found
    return None


def _disassemble(objdump: str, elf_path: pathlib.Path, out_path: pathlib.Path, mcpu: str) -> tuple[bool, str]:
    cmd = [objdump, "-d", f"--mcpu={mcpu}", str(elf_path)]
    try:
        proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        return False, f"failed to execute {objdump}: {exc}"
    out_path.write_text(proc.stdout + ("\n" if proc.stdout and proc.stderr else "") + proc.stderr, encoding="utf-8")
    if proc.returncode != 0:
        return False, f"objdump failed with exit {proc.returncode}: {' '.join(cmd)}"
    return True, "ok"


def _write_text_section(data: bytes, path: pathlib.Path) -> int:
    total = 0
    sections = rdfmod._parse_elf_sections(data)  # intentionally reuse local ELF parser
    for sec in sections:
        if sec.get("name") == ".text":
            off = int(sec["offset"])
            size = int(sec["size"])
            path.write_bytes(data[off : off + size])
            total = size
            break
    return total


def _metadata_for_write(row: dict[str, Any], capture: pathlib.Path, elf_path: pathlib.Path, text_path: pathlib.Path | None, disasm_path: pathlib.Path | None, disasm_status: str | None) -> dict[str, Any]:
    out = dict(row)
    out["capture"] = str(capture)
    out["elf_path"] = str(elf_path)
    out["text_path"] = str(text_path) if text_path else ""
    out["disassembly_path"] = str(disasm_path) if disasm_path else ""
    out["disassembly_status"] = disasm_status or ""
    if isinstance(out.get("threadgroup_dimensions"), list):
        out["threadgroup_dimensions"] = "x".join(str(x) for x in out["threadgroup_dimensions"])
    if isinstance(out.get("internal_pipeline_hash"), list):
        out["internal_pipeline_hash"] = ":".join(f"0x{int(x):016x}" for x in out["internal_pipeline_hash"])
    return out


def _fieldnames(rows: list[dict[str, Any]]) -> list[str]:
    preferred = [
        "capture",
        "chunk_ordinal",
        "code_object_index",
        "stage",
        "threadgroup_dimensions",
        "wavefront_size",
        "sgprs",
        "vgprs",
        "lds_size",
        "scratch_memory_size",
        "user_sgprs",
        "code_object_bytes",
        "elf_text_bytes",
        "entry",
        "api_pso_hash_hex",
        "internal_pipeline_hash_hex",
        "elf_path",
        "text_path",
        "disassembly_path",
        "disassembly_status",
    ]
    extra = sorted(set().union(*(r.keys() for r in rows)) - set(preferred)) if rows else []
    return preferred + extra


def extract_from_capture(capture: pathlib.Path, out_dir: pathlib.Path, args: argparse.Namespace, objdump: str | None) -> list[dict[str, Any]]:
    rdf = rdfmod.RdfFile(capture)
    pso = rdfmod.parse_pso_correlations(rdf)
    loads = rdfmod.parse_code_load_events(rdf)
    candidates: list[tuple[dict[str, Any], bytes]] = []
    for entry in rdf.entries:
        if entry.identifier != "CodeObject":
            continue
        data = rdf.chunk_data(entry)
        for row in rdfmod.summarize_code_object(entry, data, rdf=rdf, pso=pso, loads=loads):
            if _row_matches(row, args):
                candidates.append((row, data))
    candidates.sort(key=lambda item: _sort_key(item[0], args.sort_by), reverse=args.descending)
    if args.limit is not None:
        candidates = candidates[: args.limit]

    rows: list[dict[str, Any]] = []
    cap_label = _safe_label(capture.stem)
    cap_dir = out_dir / cap_label
    cap_dir.mkdir(parents=True, exist_ok=True)
    for row, data in candidates:
        tg = row.get("threadgroup_dimensions") or []
        tg_label = "x".join(str(x) for x in tg) if isinstance(tg, list) else str(tg)
        base = _safe_label(
            f"chunk{int(row['chunk_ordinal']):05d}_co{int(row['code_object_index']):05d}_"
            f"{row.get('stage','stage')}_tg{tg_label}_vgpr{row.get('vgprs')}_lds{row.get('lds_size')}"
        )
        elf_path = cap_dir / f"{base}.elf"
        elf_path.write_bytes(data)
        text_path: pathlib.Path | None = None
        if args.extract_text:
            text_path = cap_dir / f"{base}.text.bin"
            text_size = _write_text_section(data, text_path)
            if text_size == 0 and text_path.exists():
                text_path.unlink()
                text_path = None
        disasm_path: pathlib.Path | None = None
        disasm_status = ""
        if args.disassemble:
            disasm_path = cap_dir / f"{base}.isa.txt"
            if not objdump:
                disasm_status = "no llvm-objdump/amdgpu-objdump found on PATH; ELF extracted only"
                disasm_path.write_text(disasm_status + "\n", encoding="utf-8")
            else:
                ok, msg = _disassemble(objdump, elf_path, disasm_path, args.mcpu)
                disasm_status = "ok" if ok else msg
        rows.append(_metadata_for_write(row, capture, elf_path, text_path, disasm_path, disasm_status))
    return rows


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Extract shader CodeObject ELFs from RGP/RDF captures.")
    ap.add_argument("capture", type=pathlib.Path, nargs="+", help=".rgp/.rmv, directory, or zip bundle")
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("rgp_code_objects"))
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
    ap.add_argument("--limit", type=int, default=20, help="maximum code objects to extract after filtering; pass 0 for none")
    ap.add_argument("--extract-text", action="store_true", help="also write raw ELF .text bytes")
    ap.add_argument("--disassemble", action="store_true", help="try to run an AMDGPU-capable llvm-objdump over each extracted ELF")
    ap.add_argument("--llvm-objdump", help="path to AMDGPU-capable llvm-objdump/amdgpu-objdump")
    ap.add_argument("--mcpu", default="gfx1151", help="GPU target for objdump disassembly, e.g. gfx1103/gfx1151")
    ap.add_argument("--json", action="store_true", help="also write manifest.json")
    args = ap.parse_args(argv)

    if args.limit == 0:
        raise SystemExit("--limit 0 would extract nothing")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    objdump = _find_objdump(args.llvm_objdump) if args.disassemble else None

    rows: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="apusr_rgp_extract_") as tmp_s:
        captures = rdfmod._capture_paths(args.capture, pathlib.Path(tmp_s))
        if not captures:
            raise SystemExit("no .rgp/.rmv files found")
        for cap in captures:
            rows.extend(extract_from_capture(cap, args.out_dir, args, objdump))

    manifest_csv = args.out_dir / "manifest.csv"
    with manifest_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=_fieldnames(rows), extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    if args.json:
        (args.out_dir / "manifest.json").write_text(json.dumps(rows, indent=2, sort_keys=True), encoding="utf-8")

    print(f"extracted {len(rows)} code object(s) to {args.out_dir}")
    print(f"manifest: {manifest_csv}")
    if args.disassemble and objdump:
        print(f"disassembler: {objdump}")
    elif args.disassemble:
        print("disassembler: not found; extracted ELF only")
    if rows:
        print("first rows:")
        for row in rows[: min(8, len(rows))]:
            print(
                f"  chunk={row.get('chunk_ordinal')} co={row.get('code_object_index')} "
                f"stage={row.get('stage')} tg={row.get('threadgroup_dimensions')} "
                f"vgpr={row.get('vgprs')} sgpr={row.get('sgprs')} lds={row.get('lds_size')} "
                f"elf={row.get('elf_path')} disasm={row.get('disassembly_status')}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
