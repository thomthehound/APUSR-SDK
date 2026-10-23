#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""One-command summary for APUSR profile bundles.

Input may be an RGP/RMV capture, an APUSR timestamp CSV, an RGP ISA text export,
a directory, or a zip bundle containing any combination of those files.  The
report intentionally focuses on the information we keep using while tuning:

* timestamp top dispatches by median GPU time;
* RGP/RDF compute-shader resource candidates by workgroup/register/LDS shape;
* instruction-export wait/opcode hotspots when RGP ISA text dumps are included.
"""

from __future__ import annotations

import argparse
import collections
import csv
import math
import pathlib
import statistics
import sys
import tempfile
import zipfile
from typing import Any, Iterable, Sequence

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_instruction_summary as inst
import rgp_rdf_summary as rdfmod


def _extract_or_find(inputs: Sequence[pathlib.Path], tmp: pathlib.Path) -> tuple[list[pathlib.Path], list[pathlib.Path], list[pathlib.Path]]:
    captures: list[pathlib.Path] = []
    timestamps: list[pathlib.Path] = []
    instruction_text: list[pathlib.Path] = []

    def collect(root: pathlib.Path) -> None:
        files = [root] if root.is_file() else list(root.rglob("*"))
        for p in files:
            if not p.is_file():
                continue
            suffix = p.suffix.lower()
            name = p.name.lower()
            if suffix in (".rgp", ".rmv"):
                captures.append(p)
            elif suffix == ".csv" and "timestamp" in name:
                timestamps.append(p)
            elif suffix in (".txt", ".tsv"):
                # Do not bake in pass names.  Keep any text export that the
                # instruction parser can validate later.
                instruction_text.append(p)

    for item in inputs:
        if item.is_dir() or item.suffix.lower() != ".zip":
            collect(item)
            continue
        dst = tmp / item.stem
        dst.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(item, "r") as zf:
            for member in zf.infolist():
                if member.is_dir():
                    continue
                suffix = pathlib.PurePosixPath(member.filename).suffix.lower()
                name = pathlib.PurePosixPath(member.filename).name.lower()
                if suffix in (".rgp", ".rmv", ".csv", ".txt", ".tsv") and (
                    suffix in (".rgp", ".rmv", ".txt", ".tsv") or "timestamp" in name
                ):
                    zf.extract(member, dst)
        collect(dst)
    return sorted(captures), sorted(timestamps), sorted(instruction_text)


def _parse_float(value: str | None) -> float:
    try:
        return float(value) if value not in (None, "") else 0.0
    except ValueError:
        return 0.0


def _parse_int(value: str | None) -> int:
    try:
        return int(value) if value not in (None, "") else 0
    except ValueError:
        return 0


def _pct(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    if len(values) == 1:
        return values[0]
    rank = (len(values) - 1) * q
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return values[int(lo)]
    t = rank - lo
    return values[int(lo)] * (1.0 - t) + values[int(hi)] * t


def _timestamp_summary(path: pathlib.Path, drop_warmup: int, drop_seconds: float, top: int, dispatch_filter: set[str]) -> list[dict[str, Any]]:
    rows: list[dict[str, str]] = []
    frame_order: list[int] = []
    with path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            name = row.get("dispatch_name", "")
            if not name or name == "__profiler_reset__":
                continue
            frame = _parse_int(row.get("model_frame_index"))
            if frame not in frame_order:
                frame_order.append(frame)
            rows.append(row)
    has_cpu_seconds = any("cpu_seconds_since_start" in r for r in rows)
    if drop_seconds > 0.0 and has_cpu_seconds:
        rows = [r for r in rows if _parse_float(r.get("cpu_seconds_since_start")) >= drop_seconds]
    else:
        keep = set(frame_order[drop_warmup:])
        if keep:
            rows = [r for r in rows if _parse_int(r.get("model_frame_index")) in keep]
    grouped: dict[str, list[dict[str, str]]] = collections.defaultdict(list)
    for row in rows:
        grouped[row.get("dispatch_name", "<unnamed>")].append(row)
    out: list[dict[str, Any]] = []
    for name, group in grouped.items():
        if dispatch_filter and name not in dispatch_filter:
            continue
        us = [_parse_float(r.get("microseconds")) for r in group]
        dispatch_us = [_parse_float(r.get("dispatch_microseconds")) for r in group]
        gx = statistics.median([_parse_int(r.get("groups_x")) for r in group]) if group else 0
        gy = statistics.median([_parse_int(r.get("groups_y")) for r in group]) if group else 0
        gz = statistics.median([_parse_int(r.get("groups_z")) for r in group]) if group else 0
        rank = statistics.median(dispatch_us) if any(v > 0.0 for v in dispatch_us) else statistics.median(us)
        out.append(
            {
                "dispatch": name,
                "count": len(group),
                "groups": [int(gx), int(gy), int(gz)],
                "median_us": statistics.median(us),
                "median_dispatch_us": statistics.median(dispatch_us) if dispatch_us else 0.0,
                "p95_us": _pct(us, 0.95),
                "rank": rank,
            }
        )
    out.sort(key=lambda r: r["rank"], reverse=True)
    return out[:top]


def _candidate_code_objects(path: pathlib.Path, workgroups: list[list[int]], top: int) -> list[dict[str, Any]]:
    rdf = rdfmod.RdfFile(path)
    pso = rdfmod.parse_pso_correlations(rdf)
    loads = rdfmod.parse_code_load_events(rdf)
    rows: list[dict[str, Any]] = []
    for entry in rdf.entries:
        if entry.identifier != "CodeObject":
            continue
        for row in rdfmod.summarize_code_object(entry, rdf.chunk_data(entry), rdf=rdf, pso=pso, loads=loads):
            if str(row.get("stage", "")).lower() != "cs":
                continue
            if workgroups and row.get("threadgroup_dimensions") not in workgroups:
                continue
            rows.append(row)
    rows.sort(key=lambda r: (int(r.get("lds_size") or 0), int(r.get("vgprs") or 0), int(r.get("sgprs") or 0)), reverse=True)
    return rows[:top]


def _parse_workgroups(items: list[str]) -> list[list[int]]:
    out: list[list[int]] = []
    for item in items:
        wg = rdfmod.parse_workgroup(item)
        if wg:
            out.append(wg)
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Summarize APUSR timestamp/RGP/instruction profile bundles.")
    ap.add_argument("input", type=pathlib.Path, nargs="+", help="zip, directory, .rgp/.rmv, timestamp csv, or instruction txt")
    ap.add_argument("--top", type=int, default=16)
    ap.add_argument("--drop-warmup", type=int, default=8)
    ap.add_argument("--drop-seconds", type=float, default=0.0)
    ap.add_argument("--dispatch", action="append", default=[], help="limit timestamp section to dispatch name; repeatable")
    ap.add_argument("--workgroup", action="append", default=[], help="candidate CS workgroup filter such as 8,8,1; repeatable. Defaults to all CS code objects")
    ap.add_argument("--all-cs", action="store_true", help="deprecated compatibility flag; all CS code objects are already shown unless --workgroup is supplied")
    args = ap.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="apusr_profile_bundle_") as tmp_s:
        captures, timestamps, instruction_text = _extract_or_find(args.input, pathlib.Path(tmp_s))
        print("APUSR profile bundle report")
        print(f"  RGP/RMV captures : {len(captures)}")
        print(f"  timestamp CSVs   : {len(timestamps)}")
        print(f"  instruction dumps: {len(instruction_text)}")

        dispatch_filter = set(args.dispatch)
        for ts in timestamps:
            print(f"\nTIMESTAMPS: {ts}")
            print("  rank,dispatch,count,groups,median_us,dispatch_us,p95_us")
            for i, row in enumerate(_timestamp_summary(ts, args.drop_warmup, args.drop_seconds, args.top, dispatch_filter), 1):
                groups = "x".join(str(x) for x in row["groups"])
                print(f"  {i},{row['dispatch']},{row['count']},{groups},{row['median_us']:.3f},{row['median_dispatch_us']:.3f},{row['p95_us']:.3f}")

        workgroups = [] if args.all_cs or not args.workgroup else _parse_workgroups(args.workgroup)
        for cap in captures:
            rdf = rdfmod.RdfFile(cap)
            counts = collections.Counter(e.identifier for e in rdf.entries)
            print(f"\nRGP/RDF: {cap}")
            print(f"  chunks={len(rdf.entries)} code_objects={counts.get('CodeObject', 0)} queue_events={counts.get('QueueEvent', 0)} spm={counts.get('SpmCounterData', 0)}")
            print("  candidate CS code objects sorted by LDS/VGPR/SGPR:")
            print("  chunk,co,tg,wave,sgpr,vgpr,lds,scratch,pso,hash")
            for row in _candidate_code_objects(cap, workgroups, args.top):
                tg = "x".join(str(x) for x in (row.get("threadgroup_dimensions") or []))
                print(
                    f"  {row['chunk_ordinal']},{row['code_object_index']},{tg},{row.get('wavefront_size')},"
                    f"{row.get('sgprs')},{row.get('vgprs')},{row.get('lds_size')},{row.get('scratch_memory_size')},"
                    f"{row.get('api_pso_hash_hex')},{row.get('internal_pipeline_hash_hex')}"
                )

        for txt in instruction_text:
            headers, rows = inst.parse_instruction_file(txt)
            summary = inst.summarize_rows(rows)
            print(f"\nISA: {txt}")
            if headers:
                for key in ("api_shader_hash", "api_pso_hash", "driver_internal_pipeline_hash"):
                    if key in headers:
                        print(f"  {key}: {headers[key]}")
                for reg in ("vector_registers", "scalar_registers"):
                    used = headers.get(reg + "_used")
                    alloc = headers.get(reg + "_allocated")
                    if used is not None:
                        print(f"  {reg}: {used} ({alloc} allocated)")
            print("  top classes: name,percent,cycles")
            for name, v in sorted(summary["classes"].items(), key=lambda kv: kv[1].get("percent", 0.0), reverse=True)[: min(args.top, 10)]:
                print(f"    {name},{v['percent']:.2f},{int(v['cycles'])}")
            print("  top hotspots: line,opcode,percent,cycles,operands")
            for row in sorted(rows, key=lambda r: ((r.percent or 0.0), (r.cycles or 0)), reverse=True)[: args.top]:
                print(f"    {row.line_no},{row.opcode},{row.percent},{row.cycles},{row.operands}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
