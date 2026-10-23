#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Probe RGP SqttData prefixes/windows with the ROCprof decoder aggregate tool.

This is a reverse-engineering helper.  RGP's instruction-timing table is built
from SQTT records correlated with queue events and code objects.  Full raw
SqttData chunks can be huge and may stall decoders, so this tool extracts the
chunks, cuts bounded byte prefixes or windows, runs rocprof_trace_aggregate with
a timeout, and records what decoded successfully.

The normal first use is prefix probing:

  python tools/analysis/rgp_sqtt_window_probe.py capture.rgp \
    --aggregate-tool build/rocprof_trace_decoder/bin/rocprof_trace_aggregate \
    --chunk-index 0 --prefix 64K --prefix 256K --prefix 1M --timeout-sec 20

For arbitrary windows, use --window-size/--stride/--start/--stop.  Arbitrary
RGP windows are expected to fail unless they happen to start at a decoder-valid
boundary; that is useful information too.
"""
from __future__ import annotations

import argparse
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

SIZE_RE = re.compile(r'^(\d+)([KkMmGg])?$')


def parse_size(text: str) -> int:
    m = SIZE_RE.match(text.strip())
    if not m:
        raise argparse.ArgumentTypeError(f'bad size: {text}')
    value = int(m.group(1))
    suffix = (m.group(2) or '').lower()
    if suffix == 'k': value *= 1024
    elif suffix == 'm': value *= 1024 * 1024
    elif suffix == 'g': value *= 1024 * 1024 * 1024
    return value


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


def _write_file_slice(src: bytes, dst: pathlib.Path, start: int, size: int) -> int:
    start = max(0, min(start, len(src)))
    end = max(start, min(start + size, len(src)))
    dst.write_bytes(src[start:end])
    return end - start


def _run_aggregate(tool: pathlib.Path, input_path: pathlib.Path, out_prefix: pathlib.Path, timeout_sec: float,
                   *, raw: bool, waves: bool, all_records: bool) -> dict[str, Any]:
    latency_csv = out_prefix.with_suffix('.latency.csv')
    summary_json = out_prefix.with_suffix('.summary.json')
    cmd = [str(tool), str(input_path), str(latency_csv), '--summary-json', str(summary_json)]
    if all_records:
        cmd += ['--all-record-csv', str(out_prefix.with_suffix(''))]
    else:
        if waves:
            cmd += ['--wave-csv', str(out_prefix.with_suffix('.waves.csv'))]
        if raw:
            cmd += ['--raw-instruction-csv', str(out_prefix.with_suffix('.instructions.csv'))]
    row: dict[str, Any] = {'latency_csv': str(latency_csv), 'summary_json': str(summary_json)}
    try:
        proc = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout_sec if timeout_sec > 0 else None)
        row['returncode'] = proc.returncode
        row['stderr'] = proc.stderr.strip()[-2000:]
        row['stdout'] = proc.stdout.strip()[-2000:]
        if summary_json.exists():
            try:
                summary = json.loads(summary_json.read_text(encoding='utf-8'))
                row.update({f'decode_{k}': v for k, v in summary.items() if k != 'record_counts'})
                for k, v in summary.get('record_counts', {}).items():
                    row[f'record_{k}'] = v
            except Exception as exc:
                row['summary_parse_error'] = repr(exc)
        row['status'] = 'ok' if proc.returncode == 0 else 'decode_failed'
    except subprocess.TimeoutExpired as exc:
        row['status'] = 'timeout'
        row['returncode'] = None
        row['stderr'] = (exc.stderr or '')[-2000:] if isinstance(exc.stderr, str) else ''
        row['stdout'] = (exc.stdout or '')[-2000:] if isinstance(exc.stdout, str) else ''
    return row


def _window_specs(data_size: int, ns: argparse.Namespace) -> list[tuple[str, int, int]]:
    specs: list[tuple[str, int, int]] = []
    for p in ns.prefix:
        specs.append((f'prefix_{p}', 0, min(p, data_size)))
    if ns.window_size:
        start = ns.start or 0
        stop = ns.stop if ns.stop is not None else data_size
        stride = ns.stride or ns.window_size
        pos = start
        while pos < min(stop, data_size):
            specs.append((f'window_{pos}_{min(pos + ns.window_size, data_size)}', pos, min(ns.window_size, data_size - pos)))
            pos += stride
    # Preserve order, remove exact duplicates.
    seen = set()
    out = []
    for label, start, size in specs:
        key = (start, size)
        if key in seen: continue
        seen.add(key); out.append((label, start, size))
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description='Probe RGP SQTT byte prefixes/windows with rocprof_trace_aggregate.')
    ap.add_argument('capture', type=pathlib.Path, nargs='+', help='.rgp files, directories, or zip bundles')
    ap.add_argument('--aggregate-tool', type=pathlib.Path, required=True, help='path to built rocprof_trace_aggregate')
    ap.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('rgp_sqtt_window_probe'))
    ap.add_argument('--chunk-index', type=int, action='append', help='only probe these SqttData indices')
    ap.add_argument('--prefix', type=parse_size, action='append', default=[], help='probe a prefix size, e.g. 64K, 1M; may be repeated')
    ap.add_argument('--window-size', type=parse_size, help='probe fixed-size windows')
    ap.add_argument('--stride', type=parse_size, help='window stride; default window-size')
    ap.add_argument('--start', type=parse_size, help='window start offset')
    ap.add_argument('--stop', type=parse_size, help='window stop offset')
    ap.add_argument('--timeout-sec', type=float, default=20.0)
    ap.add_argument('--keep-slices', action='store_true', help='keep temporary slice .bin files')
    ap.add_argument('--raw-instructions', action='store_true', help='write decoded raw instruction CSV for successful slices; can be large')
    ap.add_argument('--waves', action='store_true', help='write decoded wave CSV for successful slices')
    ap.add_argument('--all-records', action='store_true', help='write all extra decoder CSVs via rocprof_trace_aggregate --all-record-csv')
    ns = ap.parse_args(argv)
    if not ns.prefix and not ns.window_size:
        ns.prefix = [64*1024, 256*1024, 1024*1024]
    ns.out_dir.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []

    with tempfile.TemporaryDirectory(prefix='apusr_sqtt_probe_') as tmp:
        captures = _capture_paths(ns.capture, pathlib.Path(tmp))
        if not captures:
            raise SystemExit('no .rgp files found')
        for cap in captures:
            cap_dir = ns.out_dir / cap.stem
            cap_dir.mkdir(parents=True, exist_ok=True)
            for ordinal, idx, data in _sqtt_chunks(cap):
                if ns.chunk_index is not None and idx not in set(ns.chunk_index):
                    continue
                specs = _window_specs(len(data), ns)
                for label, start, size in specs:
                    slice_path = cap_dir / f'sqtt{idx:03d}_{label}.bin'
                    actual = _write_file_slice(data, slice_path, start, size)
                    out_prefix = cap_dir / f'sqtt{idx:03d}_{label}'
                    row: dict[str, Any] = {
                        'capture': str(cap),
                        'sqtt_index': idx,
                        'chunk_ordinal': ordinal,
                        'full_bytes': len(data),
                        'label': label,
                        'start': start,
                        'bytes': actual,
                        'slice_path': str(slice_path),
                    }
                    row.update(_run_aggregate(ns.aggregate_tool, slice_path, out_prefix, ns.timeout_sec,
                                              raw=ns.raw_instructions, waves=ns.waves, all_records=ns.all_records))
                    if not ns.keep_slices:
                        try: slice_path.unlink()
                        except OSError: pass
                    rows.append(row)
                    print(f"sqtt[{idx}] {label}: {row.get('status')} waves={row.get('decode_waves')} inst={row.get('decode_instructions_seen')} pcs={row.get('decode_unique_pcs')}")

    fields = sorted({k for r in rows for k in r.keys()})
    with (ns.out_dir / 'sqtt_window_probe.csv').open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader(); w.writerows(rows)
    (ns.out_dir / 'sqtt_window_probe.json').write_text(json.dumps(rows, indent=2, sort_keys=True), encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
