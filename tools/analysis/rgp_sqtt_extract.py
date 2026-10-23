#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Extract raw SQTT chunks from AMD_RDF/RGP captures.

This is intentionally low-level.  RGP's Instruction Timing pane is derived
from SQTT/thread-trace data, but the timing table is not stored as a plain CSV
inside .rgp.  This helper extracts the raw SqttData chunks and enough context
(CodeObject metadata, QueueEvent inventory) to feed later decoders.

Inputs may be raw .rgp files, directories, or zip bundles containing profiles.
"""
from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys
import tempfile
import zipfile
from typing import Any

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_rdf_summary as rdfmod  # noqa: E402


def _capture_paths(inputs: list[pathlib.Path], temp_root: pathlib.Path) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for item in inputs:
        if item.is_dir():
            found.extend(sorted(p for p in item.rglob("*") if p.suffix.lower() == ".rgp"))
        elif item.suffix.lower() == ".zip":
            dst = temp_root / item.stem
            dst.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(item, "r") as zf:
                for member in zf.infolist():
                    if pathlib.PurePosixPath(member.filename).suffix.lower() == ".rgp":
                        zf.extract(member, dst)
            found.extend(sorted(p for p in dst.rglob("*.rgp")))
        elif item.suffix.lower() == ".rgp":
            found.append(item)
        else:
            raise ValueError(f"unsupported input path: {item}")
    return found


def _hex(data: bytes) -> str:
    return data.hex()


def _summarize_code_objects(rdf: rdfmod.RdfFile) -> list[dict[str, Any]]:
    pso = rdfmod.parse_pso_correlations(rdf)
    loads = rdfmod.parse_code_load_events(rdf)
    rows: list[dict[str, Any]] = []
    for entry in rdf.entries:
        if entry.identifier != "CodeObject":
            continue
        try:
            for row in rdfmod.summarize_code_object(entry, rdf.chunk_data(entry), rdf=rdf, pso=pso, loads=loads):
                rows.append(row)
        except Exception as exc:
            rows.append({"chunk_ordinal": entry.ordinal, "code_object_index": entry.identifier_index, "error": str(exc)})
    return rows


def extract_one(capture: pathlib.Path, out_dir: pathlib.Path, *, write_code_objects: bool, summarize_code_objects: bool) -> dict[str, Any]:
    rdf = rdfmod.RdfFile(capture)
    safe_cap = capture.stem.replace(" ", "_")
    cap_dir = out_dir / safe_cap
    sqtt_dir = cap_dir / "sqtt"
    co_dir = cap_dir / "code_objects"
    sqtt_dir.mkdir(parents=True, exist_ok=True)
    if write_code_objects:
        co_dir.mkdir(parents=True, exist_ok=True)

    chunk_counts: dict[str, int] = {}
    for e in rdf.entries:
        chunk_counts[e.identifier] = chunk_counts.get(e.identifier, 0) + 1

    sqtt_rows: list[dict[str, Any]] = []
    for e in rdf.entries:
        if e.identifier != "SqttData":
            continue
        header = rdf.chunk_header(e)
        data = rdf.chunk_data(e)
        filename = f"sqtt_{e.identifier_index:03d}_chunk_{e.ordinal:05d}.bin"
        path = sqtt_dir / filename
        path.write_bytes(data)
        sqtt_rows.append(
            {
                "capture": str(capture),
                "chunk_ordinal": e.ordinal,
                "sqtt_index": e.identifier_index,
                "chunk_version": e.version,
                "header_size": len(header),
                "data_size": len(data),
                "header_hex": _hex(header),
                "path": str(path),
            }
        )

    queue_rows: list[dict[str, Any]] = []
    for e in rdf.entries:
        if e.identifier != "QueueEvent":
            continue
        data = rdf.chunk_data(e)
        # QueueEvent is still only partially decoded in this project.  Preserve
        # the raw leading fields and full blob for future correlation work.
        queue_rows.append(
            {
                "capture": str(capture),
                "chunk_ordinal": e.ordinal,
                "event_index": e.identifier_index,
                "chunk_version": e.version,
                "data_size": len(data),
                "data_hex": _hex(data),
            }
        )

    code_rows = _summarize_code_objects(rdf) if summarize_code_objects else []
    if write_code_objects:
        for e in rdf.entries:
            if e.identifier != "CodeObject":
                continue
            (co_dir / f"code_object_{e.identifier_index:05d}_chunk_{e.ordinal:05d}.elf").write_bytes(rdf.chunk_data(e))

    summary = {
        "capture": str(capture),
        "rdf_version": rdf.version,
        "chunk_count": len(rdf.entries),
        "chunk_counts": chunk_counts,
        "sqtt_chunks": sqtt_rows,
        "queue_events": queue_rows,
        "code_objects": code_rows,
    }

    with (cap_dir / "sqtt_manifest.csv").open("w", newline="", encoding="utf-8") as f:
        fields = ["capture", "chunk_ordinal", "sqtt_index", "chunk_version", "header_size", "data_size", "header_hex", "path"]
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader(); w.writerows(sqtt_rows)
    with (cap_dir / "queue_event_raw.csv").open("w", newline="", encoding="utf-8") as f:
        fields = ["capture", "chunk_ordinal", "event_index", "chunk_version", "data_size", "data_hex"]
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader(); w.writerows(queue_rows)
    with (cap_dir / "code_object_summary.csv").open("w", newline="", encoding="utf-8") as f:
        fields = sorted({k for row in code_rows for k in row.keys()})
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader(); w.writerows(code_rows)
    (cap_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    return summary


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Extract raw SQTT chunks and context from RGP captures.")
    ap.add_argument("capture", type=pathlib.Path, nargs="+", help=".rgp files, directories, or zip bundles")
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("rgp_sqtt_extract"))
    ap.add_argument("--write-code-objects", action="store_true", help="also extract raw CodeObject ELFs")
    ap.add_argument("--summarize-code-objects", action="store_true", help="parse and write CodeObject metadata summary; slower on captures with thousands of shaders")
    ns = ap.parse_args(argv)
    ns.out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="apusr_sqtt_extract_") as tmp:
        captures = _capture_paths(ns.capture, pathlib.Path(tmp))
        if not captures:
            raise SystemExit("no .rgp files found")
        summaries = [extract_one(c, ns.out_dir, write_code_objects=ns.write_code_objects, summarize_code_objects=ns.summarize_code_objects) for c in captures]
    brief = []
    for s in summaries:
        brief.append({
            "capture": s["capture"],
            "sqtt_chunks": len(s["sqtt_chunks"]),
            "sqtt_bytes": sum(int(x["data_size"]) for x in s["sqtt_chunks"]),
            "queue_events": len(s["queue_events"]),
            "code_object_stage_records": len(s["code_objects"]),
        })
    (ns.out_dir / "summary.json").write_text(json.dumps(brief, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(brief, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
