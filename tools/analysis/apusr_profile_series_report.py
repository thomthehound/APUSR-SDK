#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Collate APUSR timestamp CSVs and RGP instruction dumps across experiments.

This is deliberately dispatch-agnostic.  It accepts zip bundles, directories, or
raw files containing any mix of:

* apusr_dx12_gpu_timestamps*.csv
* RGP copied instruction dumps as .txt/.tsv

Outputs are generic tables for every dispatch seen in the input series.  Optional
--focus-dispatch/--focus-regex arguments can add a narrow focus table, but no
specific APUSR passes are baked into the report.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import io
import pathlib
import re
import sys
import zipfile
from typing import Iterable, Sequence

import pandas as pd


@dataclasses.dataclass(frozen=True)
class InputBlob:
    source: pathlib.Path
    logical_name: str
    payload: bytes


def _version_from_name(name: str) -> str:
    stem = pathlib.Path(name).stem
    m = re.search(r"v(\d+)(?:_(\d+))?", stem)
    if not m:
        return stem
    return "v" + m.group(1) + (("_" + m.group(2)) if m.group(2) else "")


def _sort_run_key(run: str) -> tuple[int, int, str]:
    m = re.fullmatch(r"v(\d+)(?:_(\d+))?", run)
    if not m:
        return (999999, 0, run)
    return (int(m.group(1)), int(m.group(2) or 0), run)


def _iter_inputs(paths: Sequence[pathlib.Path]) -> Iterable[InputBlob]:
    for path in paths:
        if path.is_dir():
            yield from _iter_inputs(sorted(p for p in path.rglob("*") if p.is_file()))
        elif path.suffix.lower() == ".zip":
            with zipfile.ZipFile(path, "r") as zf:
                for info in zf.infolist():
                    lower = info.filename.lower()
                    if lower.endswith((".csv", ".txt", ".tsv")):
                        yield InputBlob(path, info.filename, zf.read(info))
        elif path.suffix.lower() in (".csv", ".txt", ".tsv"):
            yield InputBlob(path, path.name, path.read_bytes())


def _read_timestamp_csv(blob: InputBlob) -> pd.DataFrame | None:
    if not blob.logical_name.lower().endswith(".csv"):
        return None
    try:
        df = pd.read_csv(io.BytesIO(blob.payload))
    except Exception:
        return None
    required = {"cpu_frame_sequence", "dispatch_name", "microseconds"}
    if not required.issubset(df.columns):
        return None
    return df


def _parse_instruction_dump(blob: InputBlob) -> tuple[list[dict[str, object]], dict[str, object]] | None:
    if not blob.logical_name.lower().endswith((".txt", ".tsv")):
        return None
    text = blob.payload.decode("utf-8", errors="replace")
    if "Vector registers" not in text and "Scalar registers" not in text and "_amdgpu_cs_main" not in text:
        return None

    headers: dict[str, object] = {}
    m = re.search(r"Vector registers:\s*(\d+)\s*\((\d+) allocated\)", text)
    if m:
        headers["vgprs_used"] = int(m.group(1))
        headers["vgprs_allocated"] = int(m.group(2))
    m = re.search(r"Scalar registers:\s*(\d+)\s*\((\d+) allocated\)", text)
    if m:
        headers["sgprs_used"] = int(m.group(1))
        headers["sgprs_allocated"] = int(m.group(2))
    for key, pat in (
        ("api_shader_hash", r"API shader hash:\s*(0x[0-9A-Fa-f]+)"),
        ("api_pso_hash", r"API PSO hash:\s*(0x[0-9A-Fa-f]+)"),
        ("internal_pipeline_hash", r"Driver internal pipeline hash:\s*(0x[0-9A-Fa-f]+)"),
        ("internal_pipeline_hash", r"Driver internal pipeline hash:\s*([0-9A-Fa-f]+)"),
    ):
        m = re.search(pat, text)
        if m:
            headers[key] = m.group(1)

    rows: list[dict[str, object]] = []
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        idx = parts[0].strip()
        op = parts[1].strip()
        args = parts[2].strip()
        count = parts[3].strip().replace(",", "")
        pct = parts[4].strip()
        clk = parts[5].strip().split()[0].replace(",", "") if parts[5].strip() else ""
        if not (idx.isdigit() and op and count.isdigit() and pct):
            continue
        try:
            rows.append(
                {
                    "line": int(idx),
                    "opcode": op,
                    "args": args,
                    "count": int(count),
                    "pct": float(pct),
                    "clk": int(clk or 0),
                }
            )
        except Exception:
            continue
    if not rows and not headers:
        return None
    return rows, headers


def _classify_opcode(op: str) -> str:
    if op == "s_waitcnt":
        return "waitcnt"
    if op.startswith(("buffer_load", "image_load", "global_load", "flat_load")):
        return "vmem_load"
    if op.startswith(("buffer_store", "image_store", "global_store", "flat_store")):
        return "vmem_store"
    if op.startswith(("ds_read", "ds_load")):
        return "lds_load"
    if op.startswith(("ds_write", "ds_store")):
        return "lds_store"
    if "dot4" in op or op.startswith("v_dot"):
        return "dot4"
    if op.startswith(("v_log", "v_exp", "v_rcp", "v_rsq", "v_sqrt", "v_sin", "v_cos")):
        return "transcendental"
    if op.startswith("v_"):
        return "valu_other"
    if op.startswith("s_"):
        return "salu_other"
    return "other"


def _write_csv(df: pd.DataFrame, path: pathlib.Path) -> None:
    if df is None or df.empty:
        path.write_text("", encoding="utf-8")
    else:
        df.to_csv(path, index=False)


def _pick_baseline(runs: Sequence[str], requested: str | None) -> str | None:
    if not runs:
        return None
    sorted_runs = sorted(set(runs), key=_sort_run_key)
    if requested and requested in set(sorted_runs):
        return requested
    if requested:
        print(f"warning: requested baseline '{requested}' not found; using {sorted_runs[0]}", file=sys.stderr)
    return sorted_runs[0]


def _focus_dispatches(all_dispatches: Sequence[str], exact: Sequence[str], regexes: Sequence[str]) -> list[str]:
    selected: set[str] = set(d for d in exact if d in set(all_dispatches))
    compiled = [re.compile(r) for r in regexes]
    for dispatch in all_dispatches:
        if any(r.search(dispatch) for r in compiled):
            selected.add(dispatch)
    return sorted(selected)


def _dispatch_delta_table(timing_stats: pd.DataFrame, baseline_run: str | None) -> pd.DataFrame:
    if timing_stats.empty or not baseline_run:
        return pd.DataFrame()
    base = timing_stats[timing_stats["run"] == baseline_run][["dispatch", "p50_us", "p90_us", "p99_us", "mean_us"]].rename(
        columns={"p50_us": "baseline_p50_us", "p90_us": "baseline_p90_us", "p99_us": "baseline_p99_us", "mean_us": "baseline_mean_us"}
    )
    merged = timing_stats.merge(base, on="dispatch", how="left")
    for stat in ("p50", "p90", "p99", "mean"):
        merged[f"delta_{stat}_us"] = merged[f"{stat}_us"] - merged[f"baseline_{stat}_us"]
        merged[f"delta_{stat}_pct"] = (merged[f"delta_{stat}_us"] / merged[f"baseline_{stat}_us"] * 100.0).where(merged[f"baseline_{stat}_us"] != 0)
    return merged.sort_values(["run", "delta_p50_us"], ascending=[True, False], key=lambda s: s.map(_sort_run_key) if s.name == "run" else s)


def _top_dispatch_matrix(timing_stats: pd.DataFrame, baseline_run: str | None, limit: int, rank_by: str) -> pd.DataFrame:
    if timing_stats.empty:
        return pd.DataFrame()
    if baseline_run and baseline_run in set(timing_stats["run"]):
        ranked_dispatches = (
            timing_stats[timing_stats["run"] == baseline_run]
            .sort_values(rank_by, ascending=False)["dispatch"]
            .head(limit)
            .tolist()
        )
    else:
        ranked_dispatches = (
            timing_stats.groupby("dispatch", as_index=False)[rank_by]
            .median()
            .sort_values(rank_by, ascending=False)["dispatch"]
            .head(limit)
            .tolist()
        )
    matrix = timing_stats[timing_stats["dispatch"].isin(ranked_dispatches)].pivot_table(
        index="run", columns="dispatch", values="p50_us", aggfunc="median"
    )
    matrix = matrix.reindex(columns=ranked_dispatches).reset_index()
    return matrix.sort_values("run", key=lambda s: s.map(_sort_run_key))


def _write_markdown(
    out_path: pathlib.Path,
    *,
    timing_stats: pd.DataFrame,
    all_medians: pd.DataFrame,
    top_medians: pd.DataFrame,
    focus_medians: pd.DataFrame,
    deltas: pd.DataFrame,
    instruction_summary: pd.DataFrame,
    top_lines: pd.DataFrame,
    baseline_run: str | None,
    top_dispatches: int,
) -> None:
    md: list[str] = []
    md.append("# APUSR generic profile series report\n")
    md.append("This report is dispatch-agnostic. It ranks every dispatch present in the timestamp inputs; named focus tables only appear when requested with `--focus-dispatch` or `--focus-regex`.\n")
    if baseline_run:
        md.append(f"Baseline run: `{baseline_run}`.\n")

    if not timing_stats.empty:
        md.append("## Runs and dispatch coverage\n")
        coverage = timing_stats.groupby("run", as_index=False).agg(dispatches=("dispatch", "nunique"), rows=("samples", "sum"))
        coverage = coverage.sort_values("run", key=lambda s: s.map(_sort_run_key))
        md.append(coverage.to_markdown(index=False))
        md.append("")

    if not top_medians.empty:
        md.append(f"## Top {top_dispatches} dispatch medians\n")
        md.append(top_medians.to_markdown(index=False, floatfmt=".2f"))
        md.append("")

    if not focus_medians.empty:
        md.append("## Requested focus dispatch medians\n")
        md.append(focus_medians.to_markdown(index=False, floatfmt=".2f"))
        md.append("")

    if not deltas.empty and baseline_run:
        md.append(f"## Largest median-time changes vs `{baseline_run}`\n")
        filtered = deltas[deltas["run"] != baseline_run].copy()
        if not filtered.empty:
            regress = filtered[filtered["delta_p50_us"] > 0].sort_values("delta_p50_us", ascending=False).head(20)
            improve = filtered[filtered["delta_p50_us"] < 0].sort_values("delta_p50_us", ascending=True).head(20)
            md.append("### Regressions\n")
            if regress.empty:
                md.append("No positive median-time regressions versus baseline.\n")
            else:
                md.append(regress[["run", "dispatch", "p50_us", "baseline_p50_us", "delta_p50_us", "delta_p50_pct", "samples"]].to_markdown(index=False, floatfmt=".2f"))
            md.append("\n### Improvements\n")
            if improve.empty:
                md.append("No negative median-time deltas versus baseline.\n")
            else:
                md.append(improve[["run", "dispatch", "p50_us", "baseline_p50_us", "delta_p50_us", "delta_p50_pct", "samples"]].to_markdown(index=False, floatfmt=".2f"))
            md.append("")

    if not instruction_summary.empty:
        md.append("## Instruction dump summaries\n")
        core_cols = [
            "run", "dump", "timed_rows", "pct_sum", "vgprs_used", "vgprs_allocated", "sgprs_used", "sgprs_allocated",
            "waitcnt_pct", "vmem_load_pct", "dot4_pct", "lds_load_pct", "lds_store_pct", "transcendental_pct",
        ]
        md.append(instruction_summary[[c for c in core_cols if c in instruction_summary.columns]].to_markdown(index=False, floatfmt=".2f"))
        md.append("")

    if not top_lines.empty:
        waits = top_lines[top_lines["opcode"] == "s_waitcnt"].sort_values(["dump", "pct"], ascending=[True, False]).groupby("dump").head(8)
        if not waits.empty:
            md.append("## Top `s_waitcnt` lines by dump\n")
            md.append(waits[["run", "dump", "line", "args", "count", "pct", "clk"]].to_markdown(index=False, floatfmt=".2f"))
            md.append("")
        hot = top_lines.sort_values(["dump", "pct"], ascending=[True, False]).groupby("dump").head(8)
        if not hot.empty:
            md.append("## Top hot instruction lines by dump\n")
            md.append(hot[["run", "dump", "line", "opcode", "args", "count", "pct", "clk"]].to_markdown(index=False, floatfmt=".2f"))
            md.append("")

    out_path.write_text("\n".join(md), encoding="utf-8")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Collate APUSR timestamp CSVs and RGP instruction dumps across experiments without baking in pass names.")
    ap.add_argument("inputs", type=pathlib.Path, nargs="+", help="zip/raw/directory inputs")
    ap.add_argument("--out-dir", type=pathlib.Path, required=True, help="directory for CSV and Markdown outputs")
    ap.add_argument("--warmup-frames", type=int, default=120, help="discard frames before this cpu_frame_sequence")
    ap.add_argument("--baseline-run", default=None, help="run label used for deltas; defaults to earliest run found")
    ap.add_argument("--top-dispatches", type=int, default=24, help="number of baseline/top-ranked dispatches to include in the compact matrix")
    ap.add_argument("--rank-by", default="p50_us", choices=("p50_us", "p90_us", "p99_us", "mean_us", "max_us"), help="stat used to choose compact top dispatches")
    ap.add_argument("--focus-dispatch", action="append", default=[], help="exact dispatch name to include in an optional focus matrix; repeatable")
    ap.add_argument("--focus-regex", action="append", default=[], help="regex selecting dispatch names for an optional focus matrix; repeatable")
    ns = ap.parse_args(argv)

    ns.out_dir.mkdir(parents=True, exist_ok=True)
    timing_rows: list[dict[str, object]] = []
    instruction_rows: list[dict[str, object]] = []
    top_line_rows: list[dict[str, object]] = []

    seen_timestamps: set[tuple[str, str]] = set()
    for blob in _iter_inputs(ns.inputs):
        run = _version_from_name(blob.source.name)
        df = _read_timestamp_csv(blob)
        if df is not None:
            key = (run, blob.logical_name)
            if key in seen_timestamps:
                continue
            seen_timestamps.add(key)
            df = df[df["cpu_frame_sequence"] >= ns.warmup_frames]
            for name, group in df.groupby("dispatch_name"):
                us = group["microseconds"].dropna()
                if us.empty:
                    continue
                row = {
                    "run": run,
                    "source": blob.source.name,
                    "csv": blob.logical_name,
                    "dispatch": name,
                    "samples": int(len(us)),
                    "p50_us": float(us.median()),
                    "p90_us": float(us.quantile(0.90)),
                    "p99_us": float(us.quantile(0.99)),
                    "max_us": float(us.max()),
                    "mean_us": float(us.mean()),
                }
                for col in ("groups_x", "groups_y", "groups_z", "dispatch_microseconds", "setup_microseconds", "srv_unique_bytes", "uav_unique_bytes", "srv_external_bytes", "uav_external_bytes", "srv_temporal_bytes", "uav_temporal_bytes", "scratch_srv_bytes", "scratch_uav_bytes"):
                    if col in group.columns:
                        vals = pd.to_numeric(group[col], errors="coerce").dropna()
                        if not vals.empty:
                            row[f"median_{col}"] = float(vals.median())
                timing_rows.append(row)
            continue

        parsed = _parse_instruction_dump(blob)
        if parsed is None:
            continue
        rows, headers = parsed
        pct_by_class: dict[str, float] = collections.defaultdict(float)
        pct_by_op: dict[str, float] = collections.defaultdict(float)
        for row in rows:
            op = str(row["opcode"])
            pct_by_class[_classify_opcode(op)] += float(row["pct"])
            pct_by_op[op] += float(row["pct"])
        summary: dict[str, object] = {
            "run": run,
            "source": blob.source.name,
            "dump": pathlib.PurePosixPath(blob.logical_name).name,
            "timed_rows": len(rows),
            "pct_sum": sum(float(r["pct"]) for r in rows),
            **headers,
        }
        for cls in ("waitcnt", "dot4", "vmem_load", "vmem_store", "lds_load", "lds_store", "transcendental", "valu_other", "salu_other", "other"):
            summary[f"{cls}_pct"] = pct_by_class[cls]
        for op, pct in sorted(pct_by_op.items(), key=lambda kv: kv[1], reverse=True)[:40]:
            safe_op = re.sub(r"[^0-9A-Za-z_]+", "_", op).strip("_")
            summary[f"op_{safe_op}_pct"] = pct
        instruction_rows.append(summary)
        for row in sorted(rows, key=lambda r: float(r["pct"]), reverse=True)[:80]:
            top_line_rows.append({"run": run, "source": blob.source.name, "dump": summary["dump"], **row, "class": _classify_opcode(str(row["opcode"]))})

    timing_stats = pd.DataFrame(timing_rows)
    if not timing_stats.empty:
        timing_stats = timing_stats.sort_values(["run", "dispatch"], key=lambda s: s.map(_sort_run_key) if s.name == "run" else s)
    _write_csv(timing_stats, ns.out_dir / "dispatch_timing_stats.csv")

    baseline_run = _pick_baseline(timing_stats["run"].tolist() if not timing_stats.empty else [], ns.baseline_run)

    if not timing_stats.empty:
        all_medians = timing_stats.pivot_table(index="run", columns="dispatch", values="p50_us", aggfunc="median").reset_index()
        all_medians = all_medians.sort_values("run", key=lambda s: s.map(_sort_run_key))
    else:
        all_medians = pd.DataFrame()
    _write_csv(all_medians, ns.out_dir / "dispatch_median_matrix.csv")

    top_medians = _top_dispatch_matrix(timing_stats, baseline_run, ns.top_dispatches, ns.rank_by)
    _write_csv(top_medians, ns.out_dir / "top_dispatch_medians.csv")

    all_dispatches = sorted(timing_stats["dispatch"].unique().tolist()) if not timing_stats.empty else []
    focus = _focus_dispatches(all_dispatches, ns.focus_dispatch, ns.focus_regex)
    if focus and not timing_stats.empty:
        focus_medians = timing_stats[timing_stats["dispatch"].isin(focus)].pivot_table(index="run", columns="dispatch", values="p50_us", aggfunc="median").reset_index()
        focus_medians = focus_medians.sort_values("run", key=lambda s: s.map(_sort_run_key))
    else:
        focus_medians = pd.DataFrame()
    _write_csv(focus_medians, ns.out_dir / "focus_dispatch_medians.csv")

    deltas = _dispatch_delta_table(timing_stats, baseline_run)
    _write_csv(deltas, ns.out_dir / "dispatch_delta_vs_baseline.csv")

    instr = pd.DataFrame(instruction_rows)
    if not instr.empty:
        instr = instr.sort_values(["source", "dump"])
    _write_csv(instr, ns.out_dir / "instruction_dump_summary.csv")

    top = pd.DataFrame(top_line_rows)
    if not top.empty:
        top = top.sort_values(["source", "dump", "pct"], ascending=[True, True, False])
    _write_csv(top, ns.out_dir / "instruction_top_lines.csv")

    _write_markdown(
        ns.out_dir / "apusr_profile_series_report.md",
        timing_stats=timing_stats,
        all_medians=all_medians,
        top_medians=top_medians,
        focus_medians=focus_medians,
        deltas=deltas,
        instruction_summary=instr,
        top_lines=top,
        baseline_run=baseline_run,
        top_dispatches=ns.top_dispatches,
    )
    print(ns.out_dir / "apusr_profile_series_report.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
