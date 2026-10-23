#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Correlate decoded RGP SQTT slices with RGP QueueEvent GPU timestamps.

This is an experimental bridge between RGP's RDF QueueEvent chunks and AMD's
rocprof-trace-decoder output.  The decoder emits realtime records that map
shader_clock -> realtime_clock; in APUSR DX12 RGP captures, QueueEvent GPU
begin/end timestamps appear to use the same realtime clock domain.  This tool
uses that mapping to translate QueueEvent intervals into decoder shader-clock
windows, then aggregates decoded raw instruction rows inside those windows.

The normal workflow is intentionally bounded so we do not try to decode a huge
SqttData blob at once:

  python tools/analysis/rgp_sqtt_queue_correlate.py rgp_profiles_v526_2.zip \
    --aggregate-tool build/rocprof_trace_decoder/bin/rocprof_trace_aggregate \
    --sqtt-index 0 --prefix 1M --top-events 20 --out-dir sqtt_qe_corr

Outputs include:
  * queue_event_correlation.csv: every timed QueueEvent mapped to shader clocks
    when covered by the decoded realtime records.
  * selected_events.csv: chosen events for per-dispatch aggregation.
  * event_<idx>_latency_by_pc.csv: instruction timing inside each selected
    queue-event interval, based on decoded SQTT raw instruction rows.

This still is not a perfect clone of RGP's Instruction Timing pane.  It is a
practical correlation probe: if the selected event is covered by the decoded
slice and the raw instruction stream contains matching waves, it gives us a
per-PC latency table without manual GUI export.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from typing import Any

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_rdf_summary as rdfmod  # noqa: E402
import rgp_queue_event_summary as qemod  # noqa: E402
from rgp_sqtt_window_probe import parse_size  # noqa: E402

DEFAULT_WRAP_PERIOD = 1 << 36  # RGP timestamps observed near 68.7B; adjust with --timestamp-wrap.


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


def _sqtt_chunks(capture: pathlib.Path) -> list[tuple[int, int, bytes]]:
    rdf = rdfmod.RdfFile(capture)
    return [(e.ordinal, e.identifier_index, rdf.chunk_data(e)) for e in rdf.entries if e.identifier == 'SqttData']


def _write_slice(data: bytes, dst: pathlib.Path, start: int, size: int) -> int:
    start = max(0, min(start, len(data)))
    end = max(start, min(start + size, len(data)))
    dst.write_bytes(data[start:end])
    return end - start


def _run_aggregate(tool: pathlib.Path, input_bin: pathlib.Path, out_dir: pathlib.Path, timeout_sec: float) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    latency_csv = out_dir / 'slice_latency_by_pc.csv'
    summary_json = out_dir / 'slice_summary.json'
    stem = out_dir / 'slice_records'
    cmd = [
        str(tool), str(input_bin), str(latency_csv),
        '--summary-json', str(summary_json),
        '--all-record-csv', str(stem),
    ]
    try:
        proc = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout_sec if timeout_sec > 0 else None)
        summary: dict[str, Any] = {}
        if summary_json.exists():
            try:
                summary = json.loads(summary_json.read_text(encoding='utf-8'))
            except Exception as exc:
                summary = {'summary_parse_error': repr(exc)}
        summary.update({
            'returncode': proc.returncode,
            'status': 'ok' if proc.returncode == 0 else 'decode_failed',
            'stdout_tail': proc.stdout[-2000:],
            'stderr_tail': proc.stderr[-2000:],
            'latency_csv': str(latency_csv),
            'summary_json': str(summary_json),
            'raw_instruction_csv': str(stem) + '.instructions.csv',
            'wave_csv': str(stem) + '.waves.csv',
            'realtime_csv': str(stem) + '.realtime.csv',
            'event_csv': str(stem) + '.events.csv',
            'occupancy_csv': str(stem) + '.occupancy.csv',
            'shaderdata_csv': str(stem) + '.shaderdata.csv',
            'other_simd_csv': str(stem) + '.other_simd.csv',
        })
        return summary
    except subprocess.TimeoutExpired as exc:
        return {
            'status': 'timeout',
            'returncode': None,
            'stdout_tail': (exc.stdout or '')[-2000:] if isinstance(exc.stdout, str) else '',
            'stderr_tail': (exc.stderr or '')[-2000:] if isinstance(exc.stderr, str) else '',
        }


def _read_csv_dicts(path: pathlib.Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open('r', newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))


def _to_int(v: Any, default: int | None = None) -> int | None:
    if v is None or v == '':
        return default
    try:
        return int(str(v), 0)
    except Exception:
        return default


def _unwrap_sequence(values: list[int], period: int) -> list[int]:
    if not values:
        return []
    out = [values[0]]
    offset = 0
    prev = values[0]
    # A large backward jump usually indicates wrap.  Small disorder can happen
    # in decoded event batches, so only treat jumps bigger than half-period as
    # true wrap.
    for v in values[1:]:
        cand = v + offset
        if cand + period // 2 < prev:
            offset += period
            cand = v + offset
        elif cand - period // 2 > prev:
            offset -= period
            cand = v + offset
        out.append(cand)
        prev = cand
    return out


def _load_realtime(path: pathlib.Path, period: int) -> list[dict[str, int]]:
    rows = _read_csv_dicts(path)
    parsed = []
    for r in rows:
        sh = _to_int(r.get('shader_clock'))
        rt = _to_int(r.get('realtime_clock'))
        if sh is None or rt is None:
            continue
        parsed.append({'shader_clock': sh, 'realtime_clock_raw': rt})
    parsed.sort(key=lambda r: r['shader_clock'])
    unwrapped = _unwrap_sequence([r['realtime_clock_raw'] for r in parsed], period)
    for r, u in zip(parsed, unwrapped):
        r['realtime_clock_unwrapped'] = u
    # Keep monotonic pairs only; corrupted partial slices can contain local
    # disorder after an incomplete decode boundary.
    clean: list[dict[str, int]] = []
    last_sh = None
    last_rt = None
    for r in parsed:
        sh = r['shader_clock']; rt = r['realtime_clock_unwrapped']
        if last_sh is not None and (sh <= last_sh or rt < last_rt):
            continue
        clean.append(r); last_sh = sh; last_rt = rt
    return clean


def _unwrap_timestamp_near(ts: int, lo: int, hi: int, period: int) -> int:
    best = ts
    best_dist = None
    center = (lo + hi) // 2
    for k in range(-4, 8):
        cand = ts + k * period
        if lo <= cand <= hi:
            return cand
        dist = abs(cand - center)
        if best_dist is None or dist < best_dist:
            best = cand; best_dist = dist
    return best


def _interp_shader_clock(realtime_rows: list[dict[str, int]], rt: int) -> float | None:
    if len(realtime_rows) < 2:
        return None
    xs = [r['realtime_clock_unwrapped'] for r in realtime_rows]
    ys = [r['shader_clock'] for r in realtime_rows]
    if rt < xs[0] or rt > xs[-1]:
        return None
    idx = bisect.bisect_left(xs, rt)
    if idx < len(xs) and xs[idx] == rt:
        return float(ys[idx])
    if idx == 0 or idx >= len(xs):
        return None
    x0, x1 = xs[idx - 1], xs[idx]
    y0, y1 = ys[idx - 1], ys[idx]
    if x1 == x0:
        return float(y0)
    t = (rt - x0) / (x1 - x0)
    return y0 + t * (y1 - y0)


def _aggregate_raw_for_window(raw_rows: list[dict[str, str]], begin_sh: float, end_sh: float) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    agg: dict[tuple[int, int], dict[str, Any]] = {}
    kept = 0
    min_t = None
    max_t = None
    for r in raw_rows:
        t = _to_int(r.get('time'))
        if t is None or t < begin_sh or t > end_sh:
            continue
        kept += 1
        min_t = t if min_t is None else min(min_t, t)
        end = t + (_to_int(r.get('duration'), 0) or 0)
        max_t = end if max_t is None else max(max_t, end)
        co = _to_int(r.get('code_object_id'), 0) or 0
        pc = _to_int(r.get('pc_address'), 0) or 0
        key = (co, pc)
        row = agg.setdefault(key, {
            'code_object_id': co,
            'pc_address': pc,
            'hitcount': 0,
            'latency': 0,
            'stall': 0,
            'active': 0,
            'idle': 0,
            'category': _to_int(r.get('category'), 0) or 0,
        })
        row['hitcount'] += 1
        row['latency'] += _to_int(r.get('duration'), 0) or 0
        row['stall'] += _to_int(r.get('stall'), 0) or 0
        row['active'] += _to_int(r.get('active'), 0) or 0
        row['idle'] += _to_int(r.get('idle'), 0) or 0
    rows = sorted(agg.values(), key=lambda r: r['latency'], reverse=True)
    summary = {'decoded_instruction_rows': kept, 'decoded_min_time': min_t, 'decoded_max_time': max_t, 'unique_pcs': len(rows)}
    return rows, summary


def _write_csv(path: pathlib.Path, rows: list[dict[str, Any]], field_order: list[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if field_order is None:
        fields = sorted({k for r in rows for k in r.keys()}) if rows else []
    else:
        extra = sorted(set().union(*(r.keys() for r in rows)) - set(field_order)) if rows else []
        fields = field_order + extra
    with path.open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader(); w.writerows(rows)


def _event_matches(row: dict[str, Any], ns: argparse.Namespace) -> bool:
    if ns.event_type_name and str(row.get('event_type_name')) != ns.event_type_name:
        return False
    dur = _to_int(row.get('gpu_duration_ticks'))
    if ns.min_duration_ticks is not None and (dur is None or dur < ns.min_duration_ticks):
        return False
    if ns.max_duration_ticks is not None and (dur is None or dur > ns.max_duration_ticks):
        return False
    if ns.queue_event_index is not None and int(row.get('queue_event_index', -1)) not in set(ns.queue_event_index):
        return False
    return True


def process_capture(capture: pathlib.Path, ns: argparse.Namespace) -> dict[str, Any]:
    cap_dir = ns.out_dir / capture.stem
    cap_dir.mkdir(parents=True, exist_ok=True)
    sqtt_chunks = _sqtt_chunks(capture)
    chosen = None
    for ordinal, idx, data in sqtt_chunks:
        if idx == ns.sqtt_index:
            chosen = (ordinal, idx, data)
            break
    if chosen is None:
        raise RuntimeError(f'SqttData index {ns.sqtt_index} not found in {capture}')
    ordinal, sqtt_idx, sqtt_data = chosen
    size = ns.prefix if ns.prefix is not None else ns.window_size
    if size is None:
        size = 1024 * 1024
    start = ns.start or 0
    slice_path = cap_dir / f'sqtt{sqtt_idx:03d}_{start}_{start + size}.bin'
    actual = _write_slice(sqtt_data, slice_path, start, size)
    decode_dir = cap_dir / 'decoded'
    decode = _run_aggregate(ns.aggregate_tool, slice_path, decode_dir, ns.timeout_sec)
    if not ns.keep_slice:
        try: slice_path.unlink()
        except OSError: pass

    qe_rows, qe_summary = qemod.decode_capture(capture)
    rt_rows = _load_realtime(pathlib.Path(decode.get('realtime_csv', '')), ns.timestamp_wrap)
    raw_rows = _read_csv_dicts(pathlib.Path(decode.get('raw_instruction_csv', '')))
    rt_lo = rt_rows[0]['realtime_clock_unwrapped'] if rt_rows else None
    rt_hi = rt_rows[-1]['realtime_clock_unwrapped'] if rt_rows else None

    corr_rows: list[dict[str, Any]] = []
    selected_candidates: list[dict[str, Any]] = []
    for r in qe_rows:
        if not _event_matches(r, ns):
            continue
        begin = _to_int(r.get('gpu_begin_timestamp'))
        end = _to_int(r.get('gpu_end_timestamp'))
        out = dict(r)
        out['covered_by_realtime'] = 0
        out['mapped_shader_begin'] = ''
        out['mapped_shader_end'] = ''
        out['decoded_instruction_rows'] = 0
        out['decoded_unique_pcs'] = 0
        if begin is not None and end is not None and rt_lo is not None and rt_hi is not None:
            begin_u = _unwrap_timestamp_near(begin, rt_lo, rt_hi, ns.timestamp_wrap)
            end_u = _unwrap_timestamp_near(end, rt_lo, rt_hi, ns.timestamp_wrap)
            out['gpu_begin_unwrapped'] = begin_u
            out['gpu_end_unwrapped'] = end_u
            sh0 = _interp_shader_clock(rt_rows, begin_u)
            sh1 = _interp_shader_clock(rt_rows, end_u)
            if sh0 is not None and sh1 is not None:
                if sh1 < sh0:
                    sh0, sh1 = sh1, sh0
                out['covered_by_realtime'] = 1
                out['mapped_shader_begin'] = f'{sh0:.3f}'
                out['mapped_shader_end'] = f'{sh1:.3f}'
                _, summ = _aggregate_raw_for_window(raw_rows, sh0, sh1)
                out['decoded_instruction_rows'] = summ['decoded_instruction_rows']
                out['decoded_unique_pcs'] = summ['unique_pcs']
                out['decoded_min_time'] = summ['decoded_min_time']
                out['decoded_max_time'] = summ['decoded_max_time']
                selected_candidates.append(out)
        corr_rows.append(out)

    corr_fields = [
        'queue_event_index','event_type_name','gpu_duration_ticks','covered_by_realtime',
        'gpu_begin_timestamp','gpu_end_timestamp','gpu_begin_unwrapped','gpu_end_unwrapped',
        'mapped_shader_begin','mapped_shader_end','decoded_instruction_rows','decoded_unique_pcs',
        'maybe_command_buffer_id','event_id_in_command_buffer','maybe_api_event_id_or_draw_index','maybe_gpu_va_or_pso_ptr',
    ]
    _write_csv(cap_dir / 'queue_event_correlation.csv', corr_rows, corr_fields)

    selected_candidates.sort(key=lambda r: (int(r.get('decoded_instruction_rows') or 0), int(r.get('gpu_duration_ticks') or 0)), reverse=True)
    if ns.queue_event_index is not None:
        selected = selected_candidates
    else:
        selected = selected_candidates[: ns.top_events]
    _write_csv(cap_dir / 'selected_events.csv', selected, corr_fields)

    for ev in selected:
        sh0 = float(ev['mapped_shader_begin']); sh1 = float(ev['mapped_shader_end'])
        rows, summ = _aggregate_raw_for_window(raw_rows, sh0, sh1)
        out_path = cap_dir / f"event_{int(ev['queue_event_index']):05d}_latency_by_pc.csv"
        _write_csv(out_path, rows, ['code_object_id','pc_address','hitcount','latency','stall','active','idle','category'])
        ev['latency_csv'] = str(out_path)
        ev.update({f'event_{k}': v for k, v in summ.items()})

    _write_csv(cap_dir / 'selected_events_with_outputs.csv', selected, corr_fields + ['latency_csv','event_decoded_instruction_rows','event_decoded_min_time','event_decoded_max_time','event_unique_pcs'])
    summary = {
        'capture': str(capture),
        'sqtt_index': sqtt_idx,
        'sqtt_chunk_ordinal': ordinal,
        'sqtt_full_bytes': len(sqtt_data),
        'slice_start': start,
        'slice_bytes': actual,
        'decode': decode,
        'queue_event_summary': qe_summary,
        'realtime_records_clean': len(rt_rows),
        'realtime_unwrapped_min': rt_lo,
        'realtime_unwrapped_max': rt_hi,
        'raw_instruction_rows': len(raw_rows),
        'correlated_events': sum(1 for r in corr_rows if int(r.get('covered_by_realtime') or 0) != 0),
        'selected_events': len(selected),
    }
    (cap_dir / 'summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True), encoding='utf-8')
    return summary


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description='Correlate decoded RGP SQTT slices with QueueEvent dispatch windows.')
    ap.add_argument('capture', type=pathlib.Path, nargs='+', help='.rgp files, directories, or zip bundles')
    ap.add_argument('--aggregate-tool', type=pathlib.Path, required=True, help='built rocprof_trace_aggregate')
    ap.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('rgp_sqtt_queue_correlate'))
    ap.add_argument('--sqtt-index', type=int, default=0)
    group = ap.add_mutually_exclusive_group()
    group.add_argument('--prefix', type=parse_size, help='decode prefix bytes, e.g. 1M')
    group.add_argument('--window-size', type=parse_size, help='decode byte window size; use with --start')
    ap.add_argument('--start', type=parse_size, default=0, help='byte start offset for the SQTT slice')
    ap.add_argument('--timestamp-wrap', type=int, default=DEFAULT_WRAP_PERIOD, help='realtime timestamp wrap period')
    ap.add_argument('--timeout-sec', type=float, default=30.0)
    ap.add_argument('--keep-slice', action='store_true')
    ap.add_argument('--event-type-name', default='timed_event')
    ap.add_argument('--queue-event-index', type=int, action='append', help='select specific QueueEvent indices; may be repeated')
    ap.add_argument('--min-duration-ticks', type=int)
    ap.add_argument('--max-duration-ticks', type=int)
    ap.add_argument('--top-events', type=int, default=20, help='auto-select top correlated events by decoded instruction count')
    ns = ap.parse_args(argv)
    ns.out_dir.mkdir(parents=True, exist_ok=True)

    summaries = []
    with tempfile.TemporaryDirectory(prefix='apusr_sqtt_qe_corr_') as tmp:
        captures = _capture_paths(ns.capture, pathlib.Path(tmp))
        if not captures:
            raise SystemExit('no .rgp files found')
        for cap in captures:
            print(f'processing {cap}')
            summary = process_capture(cap, ns)
            summaries.append(summary)
            print(f"  decode={summary['decode'].get('status')} realtime={summary['realtime_records_clean']} raw_inst={summary['raw_instruction_rows']} correlated_events={summary['correlated_events']} selected={summary['selected_events']}")
    (ns.out_dir / 'summary.json').write_text(json.dumps(summaries, indent=2, sort_keys=True), encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
