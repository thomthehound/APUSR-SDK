#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Summarize APUSR DX12 GPU timestamp CSV captures.

The runtime writes apusr_dx12_gpu_timestamps.csv when APUSR_DX12_GPU_TIMESTAMPS=1.
This script groups per-dispatch rows and ranks likely bottlenecks by median GPU time.
Newer captures include CPU wall-clock seconds and bound-resource footprint columns so
startup/menu frames can be dropped by elapsed time and bandwidth/dataflow suspects can
be reviewed next to compute-heavy dispatches.
"""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean, median


DATAFLOW_COLUMNS = (
    "srv_unique_bytes",
    "uav_unique_bytes",
    "srv_external_bytes",
    "uav_external_bytes",
    "srv_temporal_bytes",
    "uav_temporal_bytes",
    "scratch_srv_bytes",
    "scratch_uav_bytes",
)


def parse_float(value: str | None) -> float:
    try:
        return float(value) if value not in (None, "") else 0.0
    except (TypeError, ValueError):
        return 0.0


def parse_int(value: str | None) -> int:
    try:
        return int(value) if value not in (None, "") else 0
    except (TypeError, ValueError):
        return 0


def pct(values: list[float], percentile: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    if len(values) == 1:
        return values[0]
    rank = (len(values) - 1) * percentile
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return values[lo]
    t = rank - lo
    return values[lo] * (1.0 - t) + values[hi] * t


def mib(value: float) -> float:
    return value / (1024.0 * 1024.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="Path to apusr_dx12_gpu_timestamps.csv")
    parser.add_argument("--top", type=int, default=24, help="Rows to print")
    parser.add_argument("--drop-warmup", type=int, default=8, help="Drop first N captured model frames when --drop-seconds is unavailable or zero")
    parser.add_argument("--drop-seconds", type=float, default=0.0, help="Drop rows captured before this CPU elapsed time. Requires structured profiler CSVs")
    parser.add_argument("--after-model-frame", type=int, default=None, help="Drop rows before this model_frame_index")
    parser.add_argument("--dataflow", action="store_true", help="Print bound-resource footprint columns when present in structured profiler CSVs")
    parser.add_argument("--rank-by", choices=("block", "dispatch"), default="block", help="Rank by whole recorded block or pure Dispatch() body time when dispatch-body timing is present")
    args = parser.parse_args()

    if not args.csv.is_file():
        raise SystemExit(f"missing timestamp CSV: {args.csv}")

    frame_order: list[int] = []
    rows: list[dict[str, str]] = []
    with args.csv.open("r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            name = row.get("dispatch_name", "")
            if not name or name == "__profiler_reset__":
                continue
            frame = parse_int(row.get("model_frame_index"))
            if frame not in frame_order:
                frame_order.append(frame)
            rows.append(row)

    has_cpu_seconds = any("cpu_seconds_since_start" in r for r in rows)
    if args.drop_seconds > 0.0 and has_cpu_seconds:
        rows = [r for r in rows if parse_float(r.get("cpu_seconds_since_start")) >= args.drop_seconds]
    elif args.after_model_frame is not None:
        rows = [r for r in rows if parse_int(r.get("model_frame_index")) >= args.after_model_frame]
    else:
        kept_frames = set(frame_order[args.drop_warmup:])
        if kept_frames:
            rows = [r for r in rows if parse_int(r.get("model_frame_index")) in kept_frames]

    if args.after_model_frame is not None and not (args.drop_seconds <= 0.0 or not has_cpu_seconds):
        rows = [r for r in rows if parse_int(r.get("model_frame_index")) >= args.after_model_frame]

    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[row.get("dispatch_name", "<unnamed>")].append(row)

    summary = []
    for name, group in grouped.items():
        us = [parse_float(r.get("microseconds")) for r in group]
        dispatch_us = [parse_float(r.get("dispatch_microseconds")) for r in group]
        setup_us = [parse_float(r.get("setup_microseconds")) for r in group]
        ticks = [parse_float(r.get("ticks")) for r in group]
        dispatch_ticks = [parse_float(r.get("dispatch_ticks")) for r in group]
        pct_frame = [parse_float(r.get("frame_percent")) for r in group]
        dispatch_pct_frame = [parse_float(r.get("dispatch_frame_percent")) for r in group]
        srv_mib = mib(median([parse_float(r.get("srv_unique_bytes")) for r in group]))
        uav_mib = mib(median([parse_float(r.get("uav_unique_bytes")) for r in group]))
        external_mib = mib(median([parse_float(r.get("srv_external_bytes")) + parse_float(r.get("uav_external_bytes")) for r in group]))
        temporal_mib = mib(median([parse_float(r.get("srv_temporal_bytes")) + parse_float(r.get("uav_temporal_bytes")) for r in group]))
        scratch_mib = mib(median([parse_float(r.get("scratch_srv_bytes")) + parse_float(r.get("scratch_uav_bytes")) for r in group]))
        median_us = median(us)
        median_dispatch_us = median(dispatch_us) if any(v > 0.0 for v in dispatch_us) else 0.0
        median_setup_us = median(setup_us) if any(v > 0.0 for v in setup_us) else 0.0
        rank_us_values = dispatch_us if args.rank_by == "dispatch" and any(v > 0.0 for v in dispatch_us) else us
        rank_tick_values = dispatch_ticks if args.rank_by == "dispatch" and any(v > 0.0 for v in dispatch_ticks) else ticks
        rank_median_us = median(rank_us_values)
        footprint_mib = srv_mib + uav_mib
        footprint_gib_s = (footprint_mib * 1000.0 / rank_median_us) if rank_median_us > 0.0 else 0.0
        # If no queue timestamp frequency was available, microseconds are zero;
        # rank by ticks in that case.
        rank_value = rank_median_us if any(v > 0.0 for v in rank_us_values) else median(rank_tick_values)
        summary.append({
            "name": name,
            "count": len(group),
            "median_us": median_us,
            "mean_us": mean(us),
            "p95_us": pct(us, 0.95),
            "median_dispatch_us": median_dispatch_us,
            "median_setup_us": median_setup_us,
            "median_ticks": median(ticks),
            "median_dispatch_ticks": median(dispatch_ticks) if dispatch_ticks else 0.0,
            "median_frame_pct": median(pct_frame),
            "median_dispatch_frame_pct": median(dispatch_pct_frame) if dispatch_pct_frame else 0.0,
            "srv_mib": srv_mib,
            "uav_mib": uav_mib,
            "footprint_mib": footprint_mib,
            "footprint_gib_s": footprint_gib_s,
            "external_mib": external_mib,
            "temporal_mib": temporal_mib,
            "scratch_mib": scratch_mib,
            "rank": rank_value,
        })

    summary.sort(key=lambda r: r["rank"], reverse=True)
    frames = sorted({parse_int(r.get("model_frame_index")) for r in rows})
    if rows:
        seconds = [parse_float(r.get("cpu_seconds_since_start")) for r in rows if "cpu_seconds_since_start" in r]
        seconds_summary = f" cpu_seconds=[{min(seconds):.3f},{max(seconds):.3f}]" if seconds else ""
    else:
        seconds_summary = ""
    print(f"captured_rows={len(rows)} dispatches={len(summary)} frames={len(frames)}{seconds_summary}")
    if args.dataflow:
        print("rank,dispatch,count,median_us,dispatch_us,setup_us,mean_us,p95_us,median_ticks,dispatch_ticks,median_frame_pct,dispatch_frame_pct,srv_mib,uav_mib,footprint_mib,footprint_gib_s,external_mib,temporal_mib,scratch_mib")
    else:
        print("rank,dispatch,count,median_us,dispatch_us,setup_us,mean_us,p95_us,median_ticks,dispatch_ticks,median_frame_pct,dispatch_frame_pct")
    for i, row in enumerate(summary[: max(0, args.top)], 1):
        base = (
            f"{i},{row['name']},{row['count']},"
            f"{row['median_us']:.3f},{row['median_dispatch_us']:.3f},{row['median_setup_us']:.3f},"
            f"{row['mean_us']:.3f},{row['p95_us']:.3f},"
            f"{row['median_ticks']:.0f},{row['median_dispatch_ticks']:.0f},"
            f"{row['median_frame_pct']:.3f},{row['median_dispatch_frame_pct']:.3f}"
        )
        if args.dataflow:
            base += (
                f",{row['srv_mib']:.3f},{row['uav_mib']:.3f},{row['footprint_mib']:.3f},"
                f"{row['footprint_gib_s']:.3f},{row['external_mib']:.3f},{row['temporal_mib']:.3f},{row['scratch_mib']:.3f}"
            )
        print(base)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
