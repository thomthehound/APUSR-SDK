#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Summarize RGP instruction-timing text exports.

RGP can export the pipeline ISA view as tabular text.  This helper consumes the
exported .txt directly, a directory, or a .zip containing one or more exported
instruction dumps.  It reports register headers, aggregate instruction-time
classes, and the largest per-instruction hotspots.
"""

from __future__ import annotations

import argparse
import collections
import csv
import dataclasses
import json
import pathlib
import re
import sys
import tempfile
import zipfile
from typing import Any, Iterable, Sequence

TEXT_SUFFIXES = (".txt", ".tsv")
HEADER_RE = re.compile(r"//\s*(?P<name>[^:]+):\s*(?P<value>.*)")
REG_RE = re.compile(r"(?P<used>\d+)\s*\((?P<allocated>\d+)\s+allocated\)")
CLK_RE = re.compile(r"(?P<cycles>[\d,]+)\s+clk")


@dataclasses.dataclass
class InstructionRow:
    file: str
    line_no: int
    opcode: str
    operands: str
    count: int | None = None
    percent: float | None = None
    cycles: int | None = None
    raw: str = ""


def _paths(inputs: Sequence[pathlib.Path], tmp: pathlib.Path) -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for item in inputs:
        if item.is_dir():
            out.extend(sorted(p for p in item.rglob("*") if p.suffix.lower() in TEXT_SUFFIXES))
        elif item.suffix.lower() == ".zip":
            dst = tmp / item.stem
            dst.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(item, "r") as zf:
                for member in zf.infolist():
                    if pathlib.PurePosixPath(member.filename).suffix.lower() in TEXT_SUFFIXES:
                        zf.extract(member, dst)
            out.extend(sorted(p for p in dst.rglob("*") if p.suffix.lower() in TEXT_SUFFIXES))
        elif item.suffix.lower() in TEXT_SUFFIXES:
            out.append(item)
        else:
            raise ValueError(f"unsupported input path: {item}")
    return out


def _parse_int(text: str) -> int | None:
    text = text.strip().replace(",", "")
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def _parse_float(text: str) -> float | None:
    text = text.strip().replace(",", "")
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def classify_opcode(opcode: str) -> str:
    op = opcode.strip()
    if not op:
        return "blank"
    if op.startswith("s_waitcnt"):
        return "waitcnt"
    if op.startswith("v_dot") or "dot4" in op:
        return "dot4"
    if op.startswith("buffer_load") or op.startswith("image_load") or op.startswith("global_load") or op.startswith("flat_load"):
        return "vmem_load"
    if op.startswith("buffer_store") or op.startswith("image_store") or op.startswith("global_store") or op.startswith("flat_store"):
        return "vmem_store"
    if op.startswith("ds_read") or op.startswith("ds_load"):
        return "lds_load"
    if op.startswith("ds_write") or op.startswith("ds_store"):
        return "lds_store"
    if op.startswith("s_"):
        return "salu"
    if op.startswith("v_"):
        if op.startswith(("v_log", "v_exp", "v_sin", "v_cos", "v_rcp", "v_rsq", "v_sqrt")):
            return "transcendental"
        if op.startswith(("v_fma", "v_mac", "v_mul", "v_add", "v_sub", "v_med", "v_max", "v_min", "v_cvt", "v_lsh", "v_or", "v_and", "v_cmp", "v_perm", "v_pack", "v_mad")):
            return "valu"
        return "valu_other"
    if op.endswith(":") or op.startswith("BB") or op.startswith("_"):
        return "label"
    return "other"


def parse_instruction_file(path: pathlib.Path) -> tuple[dict[str, Any], list[InstructionRow]]:
    headers: dict[str, Any] = {}
    rows: list[InstructionRow] = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            parts = [p.strip() for p in line.split("\t")]
            if len(parts) < 2:
                continue
            line_no = _parse_int(parts[0])
            if line_no is None:
                continue
            col1 = parts[1].strip()
            if col1.startswith("//"):
                m = HEADER_RE.search(col1)
                if m:
                    key = m.group("name").strip().lower().replace(" ", "_")
                    value = m.group("value").strip()
                    reg = REG_RE.search(value)
                    if reg and ("register" in key or "registers" in key):
                        headers[key + "_used"] = int(reg.group("used"))
                        headers[key + "_allocated"] = int(reg.group("allocated"))
                    else:
                        headers[key] = value
                continue
            opcode = col1
            # Label rows usually live in the opcode column and have no timing.
            operands = parts[2].strip() if len(parts) > 2 else ""
            count = _parse_int(parts[3]) if len(parts) > 3 else None
            percent = _parse_float(parts[4]) if len(parts) > 4 else None
            cycles = None
            if len(parts) > 5:
                m = CLK_RE.search(parts[5])
                if m:
                    cycles = _parse_int(m.group("cycles"))
            rows.append(InstructionRow(str(path), line_no, opcode, operands, count, percent, cycles, line))
    return headers, rows


def summarize_rows(rows: list[InstructionRow]) -> dict[str, Any]:
    by_class: dict[str, dict[str, float]] = collections.defaultdict(lambda: {"percent": 0.0, "cycles": 0.0, "count": 0.0, "rows": 0.0})
    by_opcode: dict[str, dict[str, float]] = collections.defaultdict(lambda: {"percent": 0.0, "cycles": 0.0, "count": 0.0, "rows": 0.0})
    for row in rows:
        cls = classify_opcode(row.opcode)
        for table, key in ((by_class, cls), (by_opcode, row.opcode)):
            table[key]["rows"] += 1
            if row.percent is not None:
                table[key]["percent"] += row.percent
            if row.cycles is not None:
                table[key]["cycles"] += row.cycles
            if row.count is not None:
                table[key]["count"] += row.count
    return {"classes": by_class, "opcodes": by_opcode}


def _rank_rows(rows: list[InstructionRow], only: str | None, limit: int) -> list[InstructionRow]:
    if only:
        rows = [r for r in rows if only.lower() in r.opcode.lower() or only.lower() in classify_opcode(r.opcode).lower()]
    return sorted(rows, key=lambda r: ((r.percent or 0.0), (r.cycles or 0)), reverse=True)[:limit]


def _print_table(title: str, table: dict[str, dict[str, float]], limit: int) -> None:
    print(title)
    items = sorted(table.items(), key=lambda kv: (kv[1].get("percent", 0.0), kv[1].get("cycles", 0.0)), reverse=True)
    print("  name,percent,cycles,count,rows")
    for name, v in items[:limit]:
        print(f"  {name},{v['percent']:.2f},{int(v['cycles'])},{int(v['count'])},{int(v['rows'])}")


def _csv_write(rows: list[InstructionRow], path: pathlib.Path | None) -> None:
    out = sys.stdout if path is None else path.open("w", newline="")
    try:
        writer = csv.DictWriter(out, fieldnames=["file", "line", "opcode", "class", "operands", "count", "percent", "cycles"])
        writer.writeheader()
        for r in rows:
            writer.writerow({
                "file": r.file,
                "line": r.line_no,
                "opcode": r.opcode,
                "class": classify_opcode(r.opcode),
                "operands": r.operands,
                "count": r.count,
                "percent": r.percent,
                "cycles": r.cycles,
            })
    finally:
        if path is not None:
            out.close()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Summarize RGP ISA instruction timing text exports.")
    ap.add_argument("input", type=pathlib.Path, nargs="+", help=".txt/.tsv instruction export, directory, or zip")
    ap.add_argument("--top", type=int, default=24, help="top rows/opcodes/classes to print")
    ap.add_argument("--only", help="filter hotspot rows by opcode substring or class name, e.g. waitcnt")
    ap.add_argument("--csv", nargs="?", const="-", help="write parsed instruction rows as CSV, optionally to a file")
    ap.add_argument("--json", action="store_true", help="emit JSON summary")
    ns = ap.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="apusr_rgp_inst_") as tmp_s:
        paths = _paths(ns.input, pathlib.Path(tmp_s))
        if not paths:
            raise SystemExit("no instruction text files found")
        report = []
        all_rows: list[InstructionRow] = []
        for p in paths:
            headers, rows = parse_instruction_file(p)
            all_rows.extend(rows)
            summary = summarize_rows(rows)
            report.append({
                "path": str(p),
                "headers": headers,
                "instruction_rows": len(rows),
                "classes": summary["classes"],
                "opcodes": summary["opcodes"],
                "hotspots": [dataclasses.asdict(r) | {"class": classify_opcode(r.opcode)} for r in _rank_rows(rows, ns.only, ns.top)],
            })

        if ns.csv is not None:
            _csv_write(all_rows, None if ns.csv == "-" else pathlib.Path(ns.csv))
            return 0
        if ns.json:
            print(json.dumps(report, indent=2, sort_keys=True))
            return 0

        for item in report:
            print(f"ISA: {item['path']}")
            print(f"  instruction_rows={item['instruction_rows']}")
            h = item["headers"]
            for key in sorted(h):
                if key.endswith(("_used", "_allocated")):
                    continue
                print(f"  {key}: {h[key]}")
            for reg in ("vector_registers", "scalar_registers", "lds_size_per_thread_group", "scratch_size_per_thread"):
                used = h.get(reg + "_used")
                alloc = h.get(reg + "_allocated")
                if used is not None:
                    print(f"  {reg}: {used} ({alloc} allocated)")
            _print_table("\n  by class:", item["classes"], ns.top)
            _print_table("\n  by opcode:", item["opcodes"], ns.top)
            print("\n  hotspots:")
            print("  line,opcode,class,percent,cycles,count,operands")
            for r in item["hotspots"]:
                print(
                    f"  {r['line_no']},{r['opcode']},{r['class']},{r.get('percent')},{r.get('cycles')},{r.get('count')},{r.get('operands')}"
                )
            print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
