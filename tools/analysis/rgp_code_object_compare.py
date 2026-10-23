#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Compare RGP CodeObject shader-resource metadata across captures.

This is useful when small HLSL changes appear to produce random occupancy/register
changes.  The comparison is keyed by API PSO hash when RGP exposes it, otherwise
by internal pipeline hash, and it reports SGPR/VGPR/LDS/scratch deltas.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
from typing import Any, Sequence

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_rdf_summary as rdfmod


def _collect(paths: Sequence[pathlib.Path], tmp: pathlib.Path) -> list[pathlib.Path]:
    return rdfmod._capture_paths(paths, tmp)  # type: ignore[attr-defined]


def _rows(path: pathlib.Path, ns: argparse.Namespace) -> list[dict[str, Any]]:
    rdf = rdfmod.RdfFile(path)
    pso = rdfmod.parse_pso_correlations(rdf)
    loads = rdfmod.parse_code_load_events(rdf)
    out: list[dict[str, Any]] = []
    for entry in rdf.entries:
        if entry.identifier != "CodeObject":
            continue
        for row in rdfmod.summarize_code_object(entry, rdf.chunk_data(entry), rdf=rdf, pso=pso, loads=loads):
            row["capture"] = str(path)
            if not rdfmod._row_matches(row, ns):  # type: ignore[attr-defined]
                continue
            out.append(row)
    return out


def _key(row: dict[str, Any], mode: str) -> str:
    if mode == "api-pso":
        return str(row.get("api_pso_hash_hex") or row.get("internal_pipeline_hash_hex"))
    if mode == "internal":
        return str(row.get("internal_pipeline_hash_hex"))
    if mode == "shape":
        tg = row.get("threadgroup_dimensions") or []
        return f"{row.get('stage')}:{'x'.join(str(x) for x in tg)}:{row.get('wavefront_size')}:{row.get('lds_size')}"
    return str(row.get("api_pso_hash_hex") or row.get("internal_pipeline_hash_hex"))


def _num(v: Any) -> int:
    return int(v) if v is not None else 0


def _fmt_delta(value: int) -> str:
    if value > 0:
        return f"+{value}"
    return str(value)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Compare RGP CodeObject SGPR/VGPR/LDS metadata across captures.")
    ap.add_argument("capture", type=pathlib.Path, nargs="+", help=".rgp/.rmv, directory, or zip bundle; at least two captures are useful")
    ap.add_argument("--stage", default="CS")
    ap.add_argument("--workgroup", type=rdfmod.parse_workgroup)
    ap.add_argument("--min-lds", type=int)
    ap.add_argument("--max-lds", type=int)
    ap.add_argument("--min-vgprs", type=int)
    ap.add_argument("--max-vgprs", type=int)
    ap.add_argument("--min-sgprs", type=int)
    ap.add_argument("--max-sgprs", type=int)
    ap.add_argument("--pipeline-hash")
    ap.add_argument("--api-pso-hash")
    ap.add_argument("--key", choices=("api-pso", "internal", "shape"), default="api-pso")
    ap.add_argument("--limit", type=int, default=80)
    ns = ap.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="apusr_rgp_compare_") as tmp_s:
        caps = [p for p in _collect(ns.capture, pathlib.Path(tmp_s)) if p.suffix.lower() == ".rgp"]
        if len(caps) < 1:
            raise SystemExit("no .rgp captures found")
        per_capture = [(cap, _rows(cap, ns)) for cap in caps]
        # Keep the highest-LDS/highest-VGPR row per key per capture to avoid spam from
        # duplicate loaded variants of the same PSO.
        tables: list[tuple[pathlib.Path, dict[str, dict[str, Any]]]] = []
        for cap, rows in per_capture:
            table: dict[str, dict[str, Any]] = {}
            for row in rows:
                key = _key(row, ns.key)
                old = table.get(key)
                if old is None or (_num(row.get("lds_size")), _num(row.get("vgprs")), _num(row.get("sgprs"))) > (_num(old.get("lds_size")), _num(old.get("vgprs")), _num(old.get("sgprs"))):
                    table[key] = row
            tables.append((cap, table))

        keys = sorted(set().union(*(set(t.keys()) for _, t in tables)))
        baseline = tables[0][1]
        print("RGP CodeObject comparison")
        for i, (cap, table) in enumerate(tables):
            print(f"  c{i}: {cap} rows={len(table)}")
        print("key,stage,tg," + ",".join(f"c{i}_sgpr,c{i}_vgpr,c{i}_lds,c{i}_scratch,c{i}_chunk" for i in range(len(tables))) + (",d_sgpr,d_vgpr,d_lds" if len(tables) >= 2 else ""))
        emitted = 0
        for key in keys:
            rows = [table.get(key) for _, table in tables]
            if not any(rows):
                continue
            first = next(r for r in rows if r is not None)
            tg = "x".join(str(x) for x in (first.get("threadgroup_dimensions") or []))
            fields = [key, str(first.get("stage")), tg]
            for row in rows:
                if row is None:
                    fields.extend(["", "", "", "", ""])
                else:
                    fields.extend([str(row.get("sgprs")), str(row.get("vgprs")), str(row.get("lds_size")), str(row.get("scratch_memory_size")), str(row.get("chunk_ordinal"))])
            if len(tables) >= 2:
                r0, r1 = rows[0], rows[1]
                if r0 is not None and r1 is not None:
                    fields.extend([
                        _fmt_delta(_num(r1.get("sgprs")) - _num(r0.get("sgprs"))),
                        _fmt_delta(_num(r1.get("vgprs")) - _num(r0.get("vgprs"))),
                        _fmt_delta(_num(r1.get("lds_size")) - _num(r0.get("lds_size"))),
                    ])
                else:
                    fields.extend(["", "", ""])
            print(",".join(fields))
            emitted += 1
            if emitted >= ns.limit:
                break
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
