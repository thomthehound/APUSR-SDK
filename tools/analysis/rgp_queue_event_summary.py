#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Decode the stable/useful fields of RGP QueueEvent RDF chunks.

The RGP QueueEvent schema is not public.  This tool names only fields that are
stable in the DX12 captures inspected for APUSR and keeps the raw words in the
CSV so future reverse-engineering does not have to start over.

Inputs may be raw .rgp files, directories, or zip bundles containing profiles.
The output is deliberately dispatch-agnostic: it reports every timed queue
entry, marker-like entry, duration, queue handle, command-buffer-ish id, submit
local event id, and timestamp fields.
"""
from __future__ import annotations

import argparse
import csv
import json
import pathlib
import struct
import sys
import tempfile
import zipfile
from typing import Any

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_rdf_summary as rdfmod  # noqa: E402

QUEUE_EVENT_STRUCT = struct.Struct('<18I')  # current v1 QueueEvent data size: 72 bytes

EVENT_TYPE_NAMES = {
    0: 'timed_event',     # draw/dispatch/copy/barrier-like GPU interval; has begin/end timestamp
    1: 'marker_like_1',   # instantaneous / region metadata in observed captures
    2: 'marker_like_2',
    3: 'sqtt_region',     # observed at SQTT-region boundaries in APUSR captures
}


def _capture_paths(inputs: list[pathlib.Path], temp_root: pathlib.Path) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for item in inputs:
        if item.is_dir():
            found.extend(sorted(p for p in item.rglob('*') if p.suffix.lower() == '.rgp'))
        elif item.suffix.lower() == '.zip':
            dst = temp_root / item.stem
            dst.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(item, 'r') as zf:
                for member in zf.infolist():
                    if pathlib.PurePosixPath(member.filename).suffix.lower() == '.rgp':
                        zf.extract(member, dst)
            found.extend(sorted(p for p in dst.rglob('*.rgp')))
        elif item.suffix.lower() == '.rgp':
            found.append(item)
        else:
            raise ValueError(f'unsupported input path: {item}')
    return found


def _hex64(v: int) -> str:
    return f'0x{v:016x}'


def _hex32(v: int) -> str:
    return f'0x{v:08x}'


def parse_queue_event(data: bytes) -> dict[str, Any]:
    if len(data) < QUEUE_EVENT_STRUCT.size:
        return {'decode_error': f'too small: {len(data)} bytes', 'data_size': len(data)}
    u = QUEUE_EVENT_STRUCT.unpack_from(data, 0)
    u64 = struct.unpack_from('<9Q', data, 0)

    event_type = int(u[4])
    begin_ts = int(u64[7])
    end_ts = int(u64[8])
    duration = end_ts - begin_ts if end_ts > begin_ts else None

    # Naming convention:
    # - exact/known-ish names are plain fields.
    # - guessed fields are prefixed with observed_/maybe_ and retain raw words.
    return {
        'process_id': int(u[1]),
        'queue_handle': _hex64(u64[1]),
        'event_type': event_type,
        'event_type_name': EVENT_TYPE_NAMES.get(event_type, f'unknown_{event_type}'),
        'maybe_api_event_id_or_draw_index': int(u[5]),
        'maybe_command_buffer_id': int(u[6]),
        'event_id_in_command_buffer': int(u[8]),
        'maybe_gpu_va_or_pso_ptr': _hex64(u64[5]),
        'cpu_timestamp': int(u64[6]),
        'gpu_begin_timestamp': begin_ts,
        'gpu_end_timestamp': end_ts if end_ts != 0 else None,
        'gpu_duration_ticks': duration,
        'u32_0': _hex32(u[0]), 'u32_1': _hex32(u[1]), 'u32_2': _hex32(u[2]), 'u32_3': _hex32(u[3]),
        'u32_4': _hex32(u[4]), 'u32_5': _hex32(u[5]), 'u32_6': _hex32(u[6]), 'u32_7': _hex32(u[7]),
        'u32_8': _hex32(u[8]), 'u32_9': _hex32(u[9]), 'u32_10': _hex32(u[10]), 'u32_11': _hex32(u[11]),
        'u32_12': _hex32(u[12]), 'u32_13': _hex32(u[13]), 'u32_14': _hex32(u[14]), 'u32_15': _hex32(u[15]),
        'u32_16': _hex32(u[16]), 'u32_17': _hex32(u[17]),
        'data_size': len(data),
    }


def decode_capture(capture: pathlib.Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    rdf = rdfmod.RdfFile(capture)
    rows: list[dict[str, Any]] = []
    for entry in rdf.entries:
        if entry.identifier != 'QueueEvent':
            continue
        row = parse_queue_event(rdf.chunk_data(entry))
        row.update({
            'capture': str(capture),
            'chunk_ordinal': entry.ordinal,
            'queue_event_index': entry.identifier_index,
            'chunk_version': entry.version,
        })
        rows.append(row)
    by_type: dict[str, int] = {}
    timed_durations = []
    for row in rows:
        by_type[row.get('event_type_name', 'unknown')] = by_type.get(row.get('event_type_name', 'unknown'), 0) + 1
        if row.get('gpu_duration_ticks') is not None:
            timed_durations.append(int(row['gpu_duration_ticks']))
    summary = {
        'capture': str(capture),
        'queue_events': len(rows),
        'event_type_counts': by_type,
        'timed_event_count': len(timed_durations),
        'timed_event_min_ticks': min(timed_durations) if timed_durations else None,
        'timed_event_max_ticks': max(timed_durations) if timed_durations else None,
        'timed_event_median_ticks': sorted(timed_durations)[len(timed_durations)//2] if timed_durations else None,
    }
    return rows, summary


def _write_csv(path: pathlib.Path, rows: list[dict[str, Any]]) -> None:
    fields = [
        'capture','queue_event_index','chunk_ordinal','event_type','event_type_name',
        'queue_handle','process_id','maybe_command_buffer_id','event_id_in_command_buffer',
        'maybe_api_event_id_or_draw_index','maybe_gpu_va_or_pso_ptr',
        'cpu_timestamp','gpu_begin_timestamp','gpu_end_timestamp','gpu_duration_ticks',
        'chunk_version','data_size',
    ] + [f'u32_{i}' for i in range(18)]
    with path.open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader(); w.writerows(rows)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description='Decode useful QueueEvent fields from RGP RDF captures.')
    ap.add_argument('capture', type=pathlib.Path, nargs='+', help='.rgp files, directories, or zip bundles')
    ap.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('rgp_queue_events'))
    ap.add_argument('--event-type', type=int, action='append', help='keep only these raw event_type values')
    ap.add_argument('--timed-only', action='store_true', help='keep only events with begin/end duration')
    ap.add_argument('--json', action='store_true', help='print JSON summary')
    ns = ap.parse_args(argv)
    ns.out_dir.mkdir(parents=True, exist_ok=True)
    all_rows: list[dict[str, Any]] = []
    summaries: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix='apusr_rgp_qe_') as tmp:
        captures = _capture_paths(ns.capture, pathlib.Path(tmp))
        if not captures:
            raise SystemExit('no .rgp files found')
        for cap in captures:
            rows, summary = decode_capture(cap)
            summaries.append(summary)
            if ns.event_type is not None:
                rows = [r for r in rows if int(r.get('event_type', -1)) in set(ns.event_type)]
            if ns.timed_only:
                rows = [r for r in rows if r.get('gpu_duration_ticks') is not None]
            all_rows.extend(rows)
    _write_csv(ns.out_dir / 'queue_events.csv', all_rows)
    (ns.out_dir / 'queue_event_summary.json').write_text(json.dumps(summaries, indent=2, sort_keys=True), encoding='utf-8')
    if ns.json:
        print(json.dumps({'summaries': summaries, 'rows': all_rows[:200]}, indent=2, sort_keys=True))
    else:
        for s in summaries:
            print(f"{s['capture']}: queue_events={s['queue_events']} types={s['event_type_counts']} timed={s['timed_event_count']} max_ticks={s['timed_event_max_ticks']}")
        print(f'wrote {ns.out_dir / "queue_events.csv"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
