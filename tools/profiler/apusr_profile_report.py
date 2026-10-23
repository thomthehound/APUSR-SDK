# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Decode APUSR .aprof ring traces and emit useful tuning reports.

The writer is intentionally fixed-record and memory-mapped so the game thread
never performs per-event file I/O. This tool turns those raw shards into CSV,
summary text, and an optional Chrome/Perfetto-compatible trace JSON.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import os
import statistics
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

MAGIC = 0x3152465041535552
HEADER_BYTES = 4096
RECORD_BYTES = 256
HEADER_PREFIX = struct.Struct("<QIIIIQQQQQQIIII128s128s")
# commit + 4 qwords + 8 primary values + 4 extra values + 8 u32 + 48 + 40
RECORD = struct.Struct("<QQQQQ12Q8I48s40s")

TYPE_STREAM = 1
TYPE_CPU_INTERVAL = 2
TYPE_FRAME = 3
TYPE_GPU_INTERVAL = 4
TYPE_CALIBRATION = 5
TYPE_COUNTER = 6
TYPE_INSTANT = 7
TYPE_ERROR = 8
TYPE_TEXT = 9
TYPE_QUEUE_EVENT = 10
TYPE_DECISION = 11
TYPE_STATE_TRANSITION = 12
TYPE_RESOURCE_EVENT = 13
TYPE_FLOW_EVENT = 14
TYPE_THREAD_INFO = 15
TYPE_WAIT_INTERVAL = 16
TYPE_METADATA = 17
TYPE_GPU_WORKLOAD_INFO = 18

FRAME_CONFIGURATION_CHANGED = 1 << 0
FRAME_FAILED = 1 << 1
FRAME_BACKEND_SWITCHED = 1 << 2

BACKENDS = {0: "none", 1: "dx12", 2: "vulkan", 3: "xrt"}
COMPONENTS = {0: "core", 1: "upscaler", 2: "framegen", 3: "graphics_interop", 4: "npu", 5: "frontend"}
QUEUE_OPERATIONS = {0: "submit", 1: "wait", 2: "signal", 3: "present", 4: "acquire"}
DECISION_OUTCOMES = {
    0: "accepted", 1: "presented", 2: "source_only", 3: "deferred", 4: "warmup",
    5: "skipped", 6: "dropped", 7: "fallback", 8: "retried", 9: "failed",
}
RESOURCE_OPERATIONS = {
    0: "create", 1: "destroy", 2: "allocate", 3: "free", 4: "import",
    5: "export", 6: "acquire", 7: "release", 8: "bind", 9: "unbind",
}
FLOW_PHASES = {0: "begin", 1: "step", 2: "end", 3: "cancel"}
WAIT_KINDS = {
    0: "unknown", 1: "cpu_primitive", 2: "gpu_fence", 3: "timeline_semaphore",
    4: "queue_idle", 5: "device_idle", 6: "present", 7: "acquire",
    8: "timer", 9: "worker_wake", 10: "external",
}
WAIT_RESULTS = {
    0: "unknown", 1: "satisfied", 2: "timeout", 3: "interrupted",
    4: "cancelled", 5: "failed",
}
RECORD_TYPES = {
    TYPE_STREAM: "stream", TYPE_CPU_INTERVAL: "cpu_interval", TYPE_FRAME: "frame",
    TYPE_GPU_INTERVAL: "gpu_interval", TYPE_CALIBRATION: "calibration", TYPE_COUNTER: "counter",
    TYPE_INSTANT: "instant", TYPE_ERROR: "error", TYPE_TEXT: "text",
    TYPE_QUEUE_EVENT: "queue_event", TYPE_DECISION: "decision",
    TYPE_STATE_TRANSITION: "state_transition", TYPE_RESOURCE_EVENT: "resource_event",
    TYPE_FLOW_EVENT: "flow_event", TYPE_THREAD_INFO: "thread_info",
    TYPE_WAIT_INTERVAL: "wait_interval", TYPE_METADATA: "metadata",
    TYPE_GPU_WORKLOAD_INFO: "gpu_workload_info",
}

# Stream records may advertise the semantic classes a subsystem knows how to
# emit. A zero mask is intentionally "undeclared/legacy", not "supports
# nothing", so older compatible traces remain meaningful.
STREAM_CAPABILITIES = {
    1 << 0: ("cpu_intervals", "cpu_interval"),
    1 << 1: ("frames", "frame"),
    1 << 2: ("gpu_intervals", "gpu_interval"),
    1 << 3: ("calibration", "calibration"),
    1 << 4: ("counters", "counter"),
    1 << 5: ("instants", "instant"),
    1 << 6: ("errors", "error"),
    1 << 7: ("queue_events", "queue_event"),
    1 << 8: ("decisions", "decision"),
    1 << 9: ("state_transitions", "state_transition"),
    1 << 10: ("resource_events", "resource_event"),
    1 << 11: ("flows", "flow_event"),
    1 << 12: ("thread_info", "thread_info"),
    1 << 13: ("wait_intervals", "wait_interval"),
    1 << 14: ("metadata", "metadata"),
    1 << 15: ("gpu_workload_info", "gpu_workload_info"),
}
STREAM_CAPABILITY_BY_RECORD = {record_type: (bit, name) for bit, (name, record_type) in STREAM_CAPABILITIES.items()}


def capability_names(mask: int) -> list[str]:
    return [name for bit, (name, _) in STREAM_CAPABILITIES.items() if mask & bit]


def mode_name(mode: int) -> str:
    return {0: "off", 1: "sampled", 2: "full", 3: "errors"}.get(mode, str(mode))


def cstr(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * p
    low = math.floor(pos)
    high = math.ceil(pos)
    if low == high:
        return ordered[low]
    return ordered[low] * (high - pos) + ordered[high] * (pos - low)


@dataclass
class Shard:
    path: Path
    cpu_frequency: int
    start_tick: int
    module_nonce: int
    process_id: int
    module: str
    session: str
    mode: int
    ring_mib: int
    sample_interval_ms: int
    burst_frames: int
    write_sequence: int
    capacity: int
    records: list[dict]


def read_shard(path: Path) -> Shard:
    data = path.read_bytes()
    if len(data) < HEADER_BYTES:
        raise ValueError(f"{path}: too small")
    fields = HEADER_PREFIX.unpack_from(data, 0)
    (magic, version, header_bytes, record_bytes, mode, capacity, write_sequence,
     stream_sequence, cpu_frequency, start_tick, module_nonce, process_id,
     ring_mib, interval_ms, burst_frames, module_raw, session_raw) = fields
    if magic != MAGIC or version != 1 or header_bytes != HEADER_BYTES or record_bytes != RECORD_BYTES:
        raise ValueError(f"{path}: unsupported trace header")

    first_sequence = max(0, write_sequence - capacity)
    records: list[dict] = []
    text_chunks: dict[tuple[int, int], list[tuple[int, str]]] = {}
    base = HEADER_BYTES
    for sequence in range(first_sequence, write_sequence):
        slot = sequence % capacity
        offset = base + slot * RECORD_BYTES
        if offset + RECORD_BYTES > len(data):
            continue
        unpacked = RECORD.unpack_from(data, offset)
        commit = unpacked[0]
        if commit != sequence + 1:
            continue
        cpu_tick, stream_id, frame_id, correlation_id = unpacked[1:5]
        values = list(unpacked[5:13])
        extra_values = list(unpacked[13:17])
        type_, backend, component, thread_id, flags, aux0, aux1, aux2 = unpacked[17:25]
        name, detail = cstr(unpacked[25]), cstr(unpacked[26])
        record = {
            "sequence": sequence,
            "cpu_tick": cpu_tick,
            "stream_id": stream_id,
            "frame_id": frame_id,
            "correlation_id": correlation_id,
            "values": values,
            "extra_values": extra_values,
            "type": type_,
            "backend": BACKENDS.get(backend, str(backend)),
            "component": COMPONENTS.get(component, str(component)),
            "thread_id": thread_id,
            "flags": flags,
            "aux0": aux0,
            "aux1": aux1,
            "aux2": aux2,
            "name": name,
            "detail": detail,
            "source": str(path),
        }
        if type_ == TYPE_TEXT:
            text_chunks.setdefault((stream_id, correlation_id), []).append((aux0, name + detail))
        else:
            records.append(record)

    for record in records:
        key = (record["stream_id"], record["correlation_id"])
        chunks = text_chunks.get(key)
        if chunks:
            chunks.sort()
            record["detail"] += "".join(text for _, text in chunks)

    return Shard(
        path=path,
        cpu_frequency=cpu_frequency,
        start_tick=start_tick,
        module_nonce=module_nonce,
        process_id=process_id,
        module=cstr(module_raw),
        session=cstr(session_raw),
        mode=mode,
        ring_mib=ring_mib,
        sample_interval_ms=interval_ms,
        burst_frames=burst_frames,
        write_sequence=write_sequence,
        capacity=capacity,
        records=records,
    )


def discover(inputs: Iterable[str]) -> list[Path]:
    out: list[Path] = []
    for item in inputs:
        path = Path(item)
        if path.is_dir():
            out.extend(sorted(path.rglob("*.aprof")))
        elif path.is_file():
            out.append(path)
    return sorted(set(out))


def unpack_pair(value: int) -> tuple[int, int]:
    return ((value >> 32) & 0xFFFFFFFF, value & 0xFFFFFFFF)


def native_queue_override(record: dict) -> int:
    """Native GPU queue override packed into aux1/aux2 for v1 GPU records."""
    return (record.get("aux1", 0) & 0xFFFFFFFF) | ((record.get("aux2", 0) & 0xFFFFFFFF) << 32)


def gpu_duration_status(record: dict) -> tuple[bool, str]:
    """Return whether a recorded GPU duration is numerically meaningful.

    Backends may provide normalized durations without raw ticks. Those remain
    valid. Raw begin/end ticks, however, cannot be converted to time without a
    non-zero GPU timestamp frequency.
    """
    if record["type"] == TYPE_FRAME:
        begin, end, frequency = record["extra_values"][0], record["extra_values"][1], record["extra_values"][2]
    elif record["type"] == TYPE_GPU_INTERVAL:
        begin, end, frequency = record["extra_values"][0], record["extra_values"][2], record["extra_values"][3]
    else:
        return True, "not_gpu_timing"
    if (begin != 0 or end != 0) and frequency == 0:
        return False, "raw_ticks_without_frequency"
    if begin != 0 or end != 0:
        return True, "raw_ticks_with_frequency"
    return True, "normalized_duration_only"


def signed_u64(value: int) -> int:
    return value - (1 << 64) if value & (1 << 63) else value


def merge_tick_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Merge overlapping/touching half-open timestamp ranges."""
    valid = sorted((start, end) for start, end in ranges if end > start)
    if not valid:
        return []
    merged = [valid[0]]
    for start, end in valid[1:]:
        prior_start, prior_end = merged[-1]
        if start <= prior_end:
            merged[-1] = (prior_start, max(prior_end, end))
        else:
            merged.append((start, end))
    return merged


def distribution_stats(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {
            "samples": 0, "mean": 0.0, "median": 0.0, "p90": 0.0,
            "p95": 0.0, "p99": 0.0, "min": 0.0, "max": 0.0, "stddev": 0.0,
        }
    return {
        "samples": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "p90": percentile(values, .90),
        "p95": percentile(values, .95),
        "p99": percentile(values, .99),
        "min": min(values),
        "max": max(values),
        "stddev": statistics.pstdev(values) if len(values) > 1 else 0.0,
    }


def reconstruct_queue_dependencies(shards: list[Shard], records: list[dict]) -> list[dict]:
    """Pair synchronization waits with the signal enqueue that can satisfy them.

    These are CPU enqueue timestamps.  The resulting gap is intentionally not
    a GPU stall duration; it is causal context for queue-order analysis.
    """
    shard_by_source = {str(s.path): s for s in shards}
    signals: dict[tuple[int, int, int], list[dict]] = {}
    binary_signals: dict[tuple[int, int], list[dict]] = {}
    waits: list[dict] = []
    for r in records:
        if r["type"] != TYPE_QUEUE_EVENT:
            continue
        op = QUEUE_OPERATIONS.get(r["aux0"], str(r["aux0"]))
        sync_object = r["values"][1]
        if sync_object == 0:
            continue
        pid = r.get("process_id", 0)
        value = r["values"][2]
        if op == "signal":
            if value != 0:
                signals.setdefault((pid, sync_object, value), []).append(r)
            else:
                binary_signals.setdefault((pid, sync_object), []).append(r)
        elif op == "wait":
            waits.append(r)

    edges: list[dict] = []
    for wait in waits:
        pid = wait.get("process_id", 0)
        sync_object = wait["values"][1]
        value = wait["values"][2]
        candidates = signals.get((pid, sync_object, value), []) if value != 0 \
            else binary_signals.get((pid, sync_object), [])
        signal = None
        for candidate in reversed(candidates):
            if candidate["cpu_tick"] <= wait["cpu_tick"]:
                signal = candidate
                break

        wait_shard = shard_by_source.get(wait["source"])
        frequency = wait_shard.cpu_frequency if wait_shard else 0
        enqueue_gap_us = 0.0
        if signal and frequency and wait["cpu_tick"] >= signal["cpu_tick"]:
            enqueue_gap_us = (wait["cpu_tick"] - signal["cpu_tick"]) * 1e6 / frequency
        edges.append({
            "wait": wait,
            "signal": signal,
            "enqueue_gap_us": enqueue_gap_us,
            "cross_queue": bool(signal and signal["values"][0] != wait["values"][0]),
        })
    return edges



def calibration_index(records: list[dict]) -> dict[tuple[str, int, int, int, int], dict]:
    return {
        (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue_override(r)): r
        for r in records if r["type"] == TYPE_CALIBRATION
    }


def calibrated_gpu_cpu_tick(
    record: dict,
    raw_gpu_tick: int,
    gpu_frequency: int,
    calibrations: dict[tuple[str, int, int, int, int], dict],
) -> float | None:
    """Map a native GPU timestamp into the profiler CPU/QPC tick domain.

    Calibration is queue-specific when the backend supplied a native queue.
    Older v1 records may use a zero queue tag, which is accepted only as the
    compatibility fallback for the same stream/frame/sample.
    """
    native_queue = native_queue_override(record)
    key = (record["source"], record["stream_id"], record["frame_id"], record["correlation_id"], native_queue)
    calibration = calibrations.get(key)
    if not calibration and native_queue:
        calibration = calibrations.get(
            (record["source"], record["stream_id"], record["frame_id"], record["correlation_id"], 0))
    if not calibration or raw_gpu_tick == 0 or gpu_frequency == 0:
        return None
    cpu_tick0 = calibration["values"][0]
    gpu_tick0 = calibration["values"][1]
    calibration_gpu_frequency = calibration["values"][2]
    cpu_frequency = calibration["values"][3]
    if cpu_frequency == 0 or calibration_gpu_frequency == 0:
        return None
    valid_bits = calibration["aux0"] or 64
    if 0 < valid_bits < 64:
        modulus = 1 << valid_bits
        mask = modulus - 1
        delta = ((raw_gpu_tick & mask) - (gpu_tick0 & mask)) & mask
        if delta >= modulus // 2:
            delta -= modulus
    else:
        delta = raw_gpu_tick - gpu_tick0
    return cpu_tick0 + delta * cpu_frequency / calibration_gpu_frequency


def observed_gpu_queue_gaps(shards: list[Shard], records: list[dict]) -> list[dict]:
    """Find uncovered gaps between APUSR-profiled intervals on one native queue.

    A gap is *not* asserted to be a hardware idle or blocked duration: APUSR may
    simply not instrument work inside it. Native wait events are attached as
    causal context when they match the next profiled frame or are enqueued
    within the observed time neighborhood.
    """
    shard_by_source = {str(s.path): s for s in shards}
    streams = {(r["source"], r["stream_id"]): r for r in records if r["type"] == TYPE_STREAM}
    calibrations = calibration_index(records)
    dependency_by_wait_sequence = {
        edge["wait"]["sequence"]: edge for edge in reconstruct_queue_dependencies(shards, records)
    }
    waits_by_queue: dict[tuple[str, int], list[dict]] = {}
    for r in records:
        if r["type"] == TYPE_QUEUE_EVENT and r["aux0"] == 1 and r["values"][0] != 0:
            waits_by_queue.setdefault((r["source"], r["values"][0]), []).append(r)
    for waits in waits_by_queue.values():
        waits.sort(key=lambda r: r["cpu_tick"])

    grouped: dict[tuple[str, int, int], list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_GPU_INTERVAL:
            continue
        native_queue = native_queue_override(r)
        if native_queue == 0:
            stream = streams.get((r["source"], r["stream_id"]))
            native_queue = stream["values"][1] if stream else 0
        if native_queue == 0:
            continue
        begin = calibrated_gpu_cpu_tick(r, r["extra_values"][0], r["extra_values"][3], calibrations)
        end = calibrated_gpu_cpu_tick(r, r["extra_values"][2], r["extra_values"][3], calibrations)
        if begin is None or end is None or end < begin:
            continue
        item = {"record": r, "begin_cpu_tick": begin, "end_cpu_tick": end, "native_queue": native_queue}
        grouped.setdefault((r["source"], r["stream_id"], native_queue), []).append(item)

    rows: list[dict] = []
    for (source, stream_id, native_queue), intervals in grouped.items():
        intervals.sort(key=lambda item: (item["begin_cpu_tick"], item["end_cpu_tick"]))
        shard = shard_by_source.get(source)
        if not shard or shard.cpu_frequency == 0:
            continue
        queue_waits = waits_by_queue.get((source, native_queue), [])
        for previous, following in zip(intervals, intervals[1:]):
            gap_start = previous["end_cpu_tick"]
            gap_end = following["begin_cpu_tick"]
            if gap_end <= gap_start:
                continue
            prev_r = previous["record"]
            next_r = following["record"]
            matching_waits = [
                wait for wait in queue_waits
                if wait["frame_id"] == next_r["frame_id"] or gap_start <= wait["cpu_tick"] <= gap_end
            ]
            wait = matching_waits[-1] if matching_waits else None
            dependency = dependency_by_wait_sequence.get(wait["sequence"]) if wait else None
            signal = dependency["signal"] if dependency else None
            rows.append({
                "source": source,
                "process_id": next_r.get("process_id", 0),
                "session": next_r.get("session", ""),
                "module": next_r.get("module", ""),
                "backend": next_r["backend"],
                "component": next_r["component"],
                "stream_id": stream_id,
                "native_queue": native_queue,
                "previous_frame_id": prev_r["frame_id"],
                "previous_name": prev_r["name"],
                "next_frame_id": next_r["frame_id"],
                "next_name": next_r["name"],
                "gap_start_cpu_tick": gap_start,
                "gap_end_cpu_tick": gap_end,
                "gap_us": (gap_end - gap_start) * 1e6 / shard.cpu_frequency,
                "wait_associated": wait is not None,
                "wait_frame_id": wait["frame_id"] if wait else 0,
                "wait_name": wait["name"] if wait else "",
                "sync_object": wait["values"][1] if wait else 0,
                "sync_value": wait["values"][2] if wait else 0,
                "signal_queue": signal["values"][0] if signal else 0,
                "cross_queue_dependency": bool(dependency and dependency["cross_queue"]),
                "matched_signal": signal is not None,
            })
    return rows


def write_csvs(shards: list[Shard], records: list[dict], output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)

    # Current writers mix a per-module nonce into stream IDs, but merged traces
    # should not depend on hash uniqueness (and older/synthetic shards may use
    # local IDs). Scope every stream lookup by source shard as well.
    streams = {(r["source"], r["stream_id"]): r for r in records if r["type"] == TYPE_STREAM}
    shard_by_source = {str(shard.path): shard for shard in shards}

    def stream_record(r: dict) -> dict | None:
        return streams.get((r["source"], r["stream_id"]))

    def stream_name(r: dict) -> str:
        stream = stream_record(r)
        return stream["name"] if stream else ""

    def common(r: dict) -> dict:
        return {
            "process_id": r.get("process_id", 0), "session": r.get("session", ""),
            "module": r.get("module", ""), "backend": r["backend"], "component": r["component"],
            "stream_id": r["stream_id"], "stream_name": stream_name(r),
        }

    stream_rows = []
    for r in records:
        if r["type"] != TYPE_STREAM:
            continue
        api_version, driver_version = unpack_pair(r["values"][2])
        vendor_id, device_id = unpack_pair(r["values"][3])
        row = common(r)
        row.update({
            "name": r["name"], "native_device": f"0x{r['values'][0]:x}",
            "native_queue": f"0x{r['values'][1]:x}", "api_version": api_version,
            "driver_version": driver_version, "vendor_id": f"0x{vendor_id:04x}",
            "device_id": f"0x{device_id:04x}", "queue_family": r["aux0"],
            "capabilities": f"0x{r['values'][4]:016x}",
            "capability_names": ";".join(capability_names(r["values"][4])),
            "flags": r["flags"], "source": r["source"],
        })
        stream_rows.append(row)

    cpu_rows = []
    for r in records:
        if r["type"] != TYPE_CPU_INTERVAL:
            continue
        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"], "name": r["name"],
            "thread_id": r["thread_id"], "start_cpu_tick": r["values"][0],
            "end_cpu_tick": r["values"][1], "duration_ms": r["values"][2] / 1e6,
            "flags": r["flags"], "source": r["source"],
        })
        cpu_rows.append(row)

    frame_rows = []
    for r in records:
        if r["type"] != TYPE_FRAME:
            continue
        render_w, render_h = unpack_pair(r["values"][2])
        display_w, display_h = unpack_pair(r["values"][3])
        duration_valid, timing_status = gpu_duration_status(r)
        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"],
            "configuration_hash": f"0x{r['values'][0]:016x}", "burst_sequence": r["values"][1],
            "burst_frame": r["aux0"], "render_width": render_w, "render_height": render_h,
            "display_width": display_w, "display_height": display_h,
            "gpu_ms": r["values"][4] / 1e6, "cpu_total_ms": r["values"][5] / 1e6,
            "cpu_record_ms": r["values"][6] / 1e6, "workgroups": r["values"][7],
            "gpu_begin_tick": r["extra_values"][0], "gpu_end_tick": r["extra_values"][1],
            "gpu_frequency": r["extra_values"][2],
            "gpu_duration_valid": duration_valid, "gpu_timing_status": timing_status,
            "native_queue": f"0x{native_queue_override(r):x}" if native_queue_override(r) else "",
            "flags": r["flags"],
            "configuration_changed": bool(r["flags"] & FRAME_CONFIGURATION_CHANGED),
            "failed": bool(r["flags"] & FRAME_FAILED),
            "backend_switched": bool(r["flags"] & FRAME_BACKEND_SWITCHED),
            "source": r["source"],
        })
        frame_rows.append(row)

    gpu_rows = []
    for r in records:
        if r["type"] != TYPE_GPU_INTERVAL:
            continue
        gx, gy = unpack_pair(r["values"][5])
        gz, level = unpack_pair(r["values"][6])
        barriers, descriptors = unpack_pair(r["values"][7])
        duration_valid, timing_status = gpu_duration_status(r)
        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"], "name": r["name"],
            "gpu_ms": r["values"][0] / 1e6, "setup_ms": r["values"][1] / 1e6,
            "execution_ms": r["values"][2] / 1e6, "read_bytes": r["values"][3], "write_bytes": r["values"][4],
            "groups_x": gx, "groups_y": gy, "groups_z": gz, "profile_level": level,
            "barriers": barriers, "descriptors": descriptors,
            "gpu_begin_tick": r["extra_values"][0], "gpu_execution_begin_tick": r["extra_values"][1],
            "gpu_end_tick": r["extra_values"][2], "gpu_frequency": r["extra_values"][3],
            "gpu_duration_valid": duration_valid, "gpu_timing_status": timing_status,
            "native_queue": f"0x{native_queue_override(r):x}" if native_queue_override(r) else "",
            "flags": r["flags"], "source": r["source"],
        })
        gpu_rows.append(row)

    calibration_rows = []
    for r in records:
        if r["type"] != TYPE_CALIBRATION:
            continue
        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"], "cpu_tick": r["values"][0],
            "gpu_tick": r["values"][1], "gpu_frequency": r["values"][2],
            "cpu_frequency": r["values"][3], "max_deviation_ns": r["values"][4],
            "native_queue": f"0x{native_queue_override(r):x}" if native_queue_override(r) else "",
            "gpu_valid_bits": r["aux0"] or 64, "flags": r["flags"], "source": r["source"],
        })
        calibration_rows.append(row)

    error_rows = []
    counter_rows = []
    instant_rows = []
    queue_rows = []
    decision_rows = []
    state_rows = []
    resource_rows = []
    flow_rows = []
    thread_rows = []
    wait_interval_rows = []
    metadata_rows = []
    gpu_workload_rows = []
    for r in records:
        if r["type"] == TYPE_ERROR:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "stage": r["name"], "status_code": r["values"][0],
                "native_code": r["values"][1], "native_code_signed": signed_u64(r["values"][1]),
                "native_code_hex": f"0x{r['values'][1]:016x}" if r["values"][1] else "",
                "context_value": r["values"][2],
                "message": r["detail"], "source": r["source"],
            })
            error_rows.append(row)
        elif r["type"] == TYPE_COUNTER:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "cpu_tick": r["cpu_tick"], "name": r["name"],
                "value": r["values"][0], "value2": r["values"][1], "flags": r["flags"], "source": r["source"],
            })
            counter_rows.append(row)
        elif r["type"] == TYPE_INSTANT:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "name": r["name"], "detail": r["detail"], "flags": r["flags"], "source": r["source"],
            })
            instant_rows.append(row)
        elif r["type"] == TYPE_QUEUE_EVENT:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"],
                "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "name": r["name"], "operation": QUEUE_OPERATIONS.get(r["aux0"], str(r["aux0"])),
                "native_queue": f"0x{r['values'][0]:x}", "sync_object": f"0x{r['values'][1]:x}",
                "sync_value": r["values"][2], "related_queue": f"0x{r['values'][3]:x}",
                "submit_sequence": r["values"][4], "stage_mask": f"0x{r['aux1']:08x}",
                "object_count": r["aux2"], "flags": r["flags"], "source": r["source"],
            })
            queue_rows.append(row)
        elif r["type"] == TYPE_DECISION:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "correlation_id": r["correlation_id"],
                "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "name": r["name"], "outcome": DECISION_OUTCOMES.get(r["aux0"], str(r["aux0"])),
                "reason": r["detail"],
                "value0": r["values"][0], "value1": r["values"][1],
                "value2": r["values"][2], "value3": r["values"][3],
                "flags": r["flags"], "triggered_burst": bool(r["flags"] & 1),
                "source": r["source"],
            })
            decision_rows.append(row)
        elif r["type"] == TYPE_STATE_TRANSITION:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "name": r["name"], "previous_value": r["values"][0], "next_value": r["values"][1],
                "subject": r["values"][2], "epoch": r["values"][3], "reason": r["detail"],
                "flags": r["flags"], "source": r["source"],
            })
            state_rows.append(row)
        elif r["type"] == TYPE_RESOURCE_EVENT:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "name": r["name"], "operation": RESOURCE_OPERATIONS.get(r["aux0"], str(r["aux0"])),
                "resource_id": r["values"][0],
                "native_handle": f"0x{r['values'][1]:x}" if r["values"][1] else "",
                "bytes": r["values"][2], "alignment": r["values"][3],
                "heap_or_memory_type": r["aux1"], "detail": r["detail"],
                "flags": r["flags"], "source": r["source"],
            })
            resource_rows.append(row)
        elif r["type"] == TYPE_FLOW_EVENT:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "occurrence_id": r["values"][3],
                "flow_id": r["correlation_id"],
                "parent_flow_id": r["values"][0], "cpu_tick": r["cpu_tick"],
                "thread_id": r["thread_id"], "name": r["name"],
                "phase": FLOW_PHASES.get(r["aux0"], str(r["aux0"])), "detail": r["detail"],
                "value0": r["values"][1], "value1": r["values"][2],
                "flags": r["flags"], "source": r["source"],
            })
            flow_rows.append(row)
        elif r["type"] == TYPE_THREAD_INFO:
            row = common(r)
            raw_priority = signed_u64(r["values"][2])
            row.update({
                "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "native_thread_id": r["values"][0] or r["thread_id"],
                "name": r["name"], "role": r["detail"], "priority": raw_priority,
                "affinity_mask": f"0x{r['values'][1]:x}" if r["values"][1] else "",
                "flags": r["flags"], "source": r["source"],
            })
            thread_rows.append(row)
        elif r["type"] == TYPE_WAIT_INTERVAL:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "correlation_id": r["correlation_id"],
                "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"], "name": r["name"],
                "kind": WAIT_KINDS.get(r["aux0"], str(r["aux0"])),
                "result": WAIT_RESULTS.get(r["aux1"], str(r["aux1"])),
                "start_cpu_tick": r["values"][0], "end_cpu_tick": r["values"][1],
                "duration_ns": r["values"][2], "duration_ms": r["values"][2] / 1e6,
                "object": f"0x{r['values'][3]:x}" if r["values"][3] else "",
                "object_raw": r["values"][3], "value": r["values"][4],
                "timeout_ns": r["values"][5], "timeout_ms": r["values"][5] / 1e6,
                "observed_value": r["values"][6], "detail": r["detail"],
                "flags": r["flags"], "source": r["source"],
            })
            wait_interval_rows.append(row)
        elif r["type"] == TYPE_METADATA:
            row = common(r)
            row.update({
                "frame_id": r["frame_id"], "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "name": r["name"], "detail": r["detail"],
                "value0": r["values"][0], "value1": r["values"][1],
                "flags": r["flags"], "source": r["source"],
            })
            metadata_rows.append(row)
        elif r["type"] == TYPE_GPU_WORKLOAD_INFO:
            tx, ty = unpack_pair(r["values"][4])
            tz, subgroup = unpack_pair(r["values"][5])
            row = common(r)
            row.update({
                "cpu_tick": r["cpu_tick"], "thread_id": r["thread_id"],
                "workload_id": r["correlation_id"], "name": r["name"], "detail": r["detail"],
                "native_pipeline": f"0x{r['values'][0]:x}" if r["values"][0] else "",
                "code_hash": f"0x{r['values'][1]:016x}" if r["values"][1] else "",
                "code_bytes": r["values"][2], "shared_memory_bytes": r["values"][3],
                "threads_x": tx, "threads_y": ty, "threads_z": tz, "subgroup_size": subgroup,
                "flags": r["flags"], "source": r["source"],
            })
            gpu_workload_rows.append(row)

    def save(name: str, rows: list[dict]):
        path = output / name
        if not rows:
            path.write_text("", encoding="utf-8")
            return

        # Derived tables can legitimately contain rows with different optional
        # fields (for example DXGI versus Vulkan display-timing observations).
        # Build a stable union schema instead of assuming the first row happens
        # to contain every field used by later rows.
        fieldnames: list[str] = []
        seen_fields: set[str] = set()
        for row in rows:
            for field in row:
                if field in seen_fields:
                    continue
                seen_fields.add(field)
                fieldnames.append(field)

        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)

    save("streams.csv", stream_rows)
    save("cpu_intervals.csv", cpu_rows)
    save("frames.csv", frame_rows)
    save("gpu_intervals.csv", gpu_rows)
    save("calibrations.csv", calibration_rows)
    save("errors.csv", error_rows)
    save("counters.csv", counter_rows)
    save("decisions.csv", decision_rows)
    save("state_transitions.csv", state_rows)
    save("resource_events.csv", resource_rows)
    save("flows.csv", flow_rows)
    save("threads.csv", thread_rows)
    save("wait_intervals.csv", wait_interval_rows)
    save("metadata.csv", metadata_rows)
    save("gpu_workloads.csv", gpu_workload_rows)

    # Attribute Vulkan shader/pipeline/cache creation to the process module that
    # issued each call. The Windows host-capture layer uses frame_id as a cold-
    # path event ID so counters and CPU intervals for one driver-facing call can
    # be joined without adding another profiler record type.
    pipeline_callsite_modules: dict[tuple, str] = {}
    for row in metadata_rows:
        if row.get("name") != "vulkan.pipeline.callsite_module":
            continue
        key = (row.get("process_id", 0), row.get("session", ""), int(row.get("value0", 0) or 0))
        pipeline_callsite_modules[key] = row.get("detail", "") or "unknown"

    pipeline_events: dict[tuple, dict] = {}
    def pipeline_event(row: dict) -> dict | None:
        if row.get("stream_name") != "vulkan.pipeline_activity":
            return None
        key = (row.get("process_id", 0), row.get("session", ""), row.get("module", ""),
               row.get("source", ""), row.get("stream_id", 0), row.get("frame_id", 0))
        event = pipeline_events.setdefault(key, {
            "process_id": row.get("process_id", 0), "session": row.get("session", ""),
            "module": row.get("module", ""), "backend": row.get("backend", ""),
            "component": row.get("component", ""), "stream_id": row.get("stream_id", 0),
            "stream_name": row.get("stream_name", ""), "event_id": row.get("frame_id", 0),
            "operation": "", "duration_ms": "", "caller_module": "", "caller_handle": "",
            "result": "", "object_handle": "", "pipeline_cache": "", "create_count": "",
            "bytes": "", "code_hash": "", "flags": "", "source": row.get("source", ""),
        })
        return event

    for row in cpu_rows:
        event = pipeline_event(row)
        if event is None or not row.get("name", "").startswith("vk."):
            continue
        event["operation"] = row.get("name", "")
        event["duration_ms"] = row.get("duration_ms", "")

    for row in counter_rows:
        event = pipeline_event(row)
        if event is None:
            continue
        name = row.get("name", "")
        value = int(row.get("value", 0) or 0)
        value2 = int(row.get("value2", 0) or 0)
        if name == "vulkan.pipeline.callsite":
            event["caller_handle"] = f"0x{value:x}" if value else ""
            event["caller_module"] = pipeline_callsite_modules.get(
                (row.get("process_id", 0), row.get("session", ""), value), "unknown" if value else "")
        elif name == "vulkan.shader_module.create":
            event["bytes"] = value
            event["code_hash"] = f"0x{value2:016x}" if value2 else ""
            event["flags"] = row.get("flags", 0)
        elif name == "vulkan.shader_module.result":
            event["result"] = signed_u64(value)
            event["object_handle"] = f"0x{value2:x}" if value2 else ""
        elif name in ("vulkan.pipeline.compute.create", "vulkan.pipeline.graphics.create"):
            event["create_count"] = value
            event["pipeline_cache"] = f"0x{value2:x}" if value2 else ""
            event["flags"] = row.get("flags", 0)
        elif name in ("vulkan.pipeline.compute.result", "vulkan.pipeline.graphics.result"):
            event["result"] = signed_u64(value)
            event["object_handle"] = f"0x{value2:x}" if value2 else ""
        elif name == "vulkan.pipeline_cache.create":
            event["bytes"] = value
            event["object_handle"] = f"0x{value2:x}" if value2 else ""
            event["flags"] = row.get("flags", 0)
        elif name == "vulkan.pipeline_cache.get_data":
            event["bytes"] = value2
            event["flags"] = row.get("flags", 0)
        elif name == "vulkan.pipeline_cache.merge":
            event["create_count"] = value
            event["pipeline_cache"] = f"0x{value2:x}" if value2 else ""
        elif name == "vulkan.pipeline_cache.result":
            event["result"] = signed_u64(value)
            if value2:
                event["pipeline_cache"] = f"0x{value2:x}"
        elif name in ("vulkan.shader_module.destroy", "vulkan.pipeline.destroy", "vulkan.pipeline_cache.destroy"):
            event["object_handle"] = f"0x{value:x}" if value else ""

    vulkan_pipeline_activity = sorted(
        (event for event in pipeline_events.values() if event.get("operation")),
        key=lambda row: (int(row.get("process_id", 0) or 0), row.get("session", ""), int(row.get("event_id", 0) or 0)))
    save("vulkan_pipeline_activity.csv", vulkan_pipeline_activity)

    memory_budget_rows = []
    for row in counter_rows:
        name = row.get("name", "")
        if not name.startswith("memory.budget."):
            continue
        usage = int(row.get("value", 0) or 0)
        budget = int(row.get("value2", 0) or 0)
        flags = int(row.get("flags", 0) or 0)
        segment = name.removeprefix("memory.budget.")
        heap_index = ""
        device_local = ""
        if segment == "heap":
            heap_index = flags & 0xff
            device_local = bool(flags & 0x100)
        memory_budget_rows.append({
            "process_id": row.get("process_id", 0),
            "session": row.get("session", ""),
            "module": row.get("module", ""),
            "backend": row.get("backend", ""),
            "component": row.get("component", ""),
            "stream_name": row.get("stream_name", ""),
            "stream_id": row.get("stream_id", 0),
            "frame_id": row.get("frame_id", 0),
            "cpu_tick": row.get("cpu_tick", 0),
            "segment": segment,
            "heap_index": heap_index,
            "device_local": device_local,
            "usage_bytes": usage,
            "budget_bytes": budget,
            "headroom_bytes": budget - usage,
            "usage_mib": usage / (1024.0 * 1024.0),
            "budget_mib": budget / (1024.0 * 1024.0),
            "headroom_mib": (budget - usage) / (1024.0 * 1024.0),
            "pressure_pct": (100.0 * usage / budget) if budget else 0.0,
            "over_budget": bool(budget and usage > budget),
            "flags": flags,
            "source": row.get("source", ""),
        })
    save("memory_budget.csv", memory_budget_rows)

    memory_reservation_rows = []
    for row in counter_rows:
        name = row.get("name", "")
        if not name.startswith("memory.reservation."):
            continue
        reservation = int(row.get("value", 0) or 0)
        available = int(row.get("value2", 0) or 0)
        memory_reservation_rows.append({
            "process_id": row.get("process_id", 0),
            "session": row.get("session", ""),
            "module": row.get("module", ""),
            "backend": row.get("backend", ""),
            "component": row.get("component", ""),
            "stream_name": row.get("stream_name", ""),
            "stream_id": row.get("stream_id", 0),
            "frame_id": row.get("frame_id", 0),
            "cpu_tick": row.get("cpu_tick", 0),
            "segment": name.removeprefix("memory.reservation."),
            "reservation_bytes": reservation,
            "available_for_reservation_bytes": available,
            "reservation_mib": reservation / (1024.0 * 1024.0),
            "available_for_reservation_mib": available / (1024.0 * 1024.0),
            "flags": row.get("flags", 0),
            "source": row.get("source", ""),
        })
    save("memory_reservation.csv", memory_reservation_rows)

    display_groups: dict[tuple, dict] = {}
    for row in counter_rows:
        name = row.get("name", "")
        if not name.startswith("display."):
            continue
        key = (
            row.get("source", ""), row.get("stream_id", 0), row.get("frame_id", 0),
            int(row.get("flags", 0) or 0))
        group = display_groups.setdefault(key, {
            "process_id": row.get("process_id", 0),
            "session": row.get("session", ""),
            "module": row.get("module", ""),
            "backend": row.get("backend", ""),
            "component": row.get("component", ""),
            "stream_name": row.get("stream_name", ""),
            "stream_id": row.get("stream_id", 0),
            "frame_id": row.get("frame_id", 0),
            "generated": bool(int(row.get("flags", 0) or 0) & 1),
            "source": row.get("source", ""),
        })
        if name == "display.present_to_sync_ns":
            group["present_begin_to_sync_ms"] = int(row.get("value", 0) or 0) / 1e6
            group["present_return_to_sync_ms"] = int(row.get("value2", 0) or 0) / 1e6
        elif name == "display.dxgi_sync_qpc":
            group["actual_sync_qpc"] = int(row.get("value", 0) or 0)
            group["present_id"] = int(row.get("value2", 0) or 0)
        elif name == "display.dxgi_refresh_counts":
            group["present_refresh_count"] = int(row.get("value", 0) or 0)
            group["sync_refresh_count"] = int(row.get("value2", 0) or 0)
        elif name == "display.dxgi_sync_gpu_time":
            group["sync_gpu_time"] = int(row.get("value", 0) or 0)
        elif name == "display.vk_actual_present_ns":
            group["present_id"] = int(row.get("frame_id", 0) or 0)
            group["actual_present_ns"] = int(row.get("value", 0) or 0)
            group["earliest_present_ns"] = int(row.get("value2", 0) or 0)
            actual = group["actual_present_ns"]
            earliest = group["earliest_present_ns"]
            group["actual_minus_earliest_ms"] = (actual - earliest) / 1e6 if actual and earliest else 0.0
        elif name == "display.vk_present_margin_ns":
            group["present_id"] = int(row.get("frame_id", 0) or 0)
            group["present_margin_ms"] = int(row.get("value", 0) or 0) / 1e6
            group["desired_present_ns"] = int(row.get("value2", 0) or 0)
    display_timing_rows = list(display_groups.values())
    vk_display_groups: dict[tuple[str, int], list[dict]] = {}
    for row in display_timing_rows:
        if row.get("backend") == "vulkan" and int(row.get("actual_present_ns", 0) or 0) != 0:
            vk_display_groups.setdefault((row.get("source", ""), int(row.get("stream_id", 0) or 0)), []).append(row)
    for rows in vk_display_groups.values():
        rows.sort(key=lambda r: int(r.get("actual_present_ns", 0) or 0))
        previous_actual = 0
        for row in rows:
            actual = int(row.get("actual_present_ns", 0) or 0)
            row["actual_present_interval_ms"] = (actual - previous_actual) / 1e6 if previous_actual and actual >= previous_actual else 0.0
            previous_actual = actual
    display_timing_rows.sort(key=lambda r: (r.get("process_id", 0), r.get("stream_id", 0), r.get("frame_id", 0), r.get("generated", False)))
    save("display_timing.csv", display_timing_rows)

    save("instants.csv", instant_rows)
    save("queue_events.csv", queue_rows)

    # Reconstruct queue dependency edges from native synchronization identity.
    # The gap below is the interval between CPU enqueue points. It is useful for
    # causality/order analysis but is deliberately not labelled GPU wait time.
    queue_dependencies = []
    for edge in reconstruct_queue_dependencies(shards, records):
        r = edge["wait"]
        signal = edge["signal"]
        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"],
            "wait_name": r["name"], "sync_object": f"0x{r['values'][1]:x}", "sync_value": r["values"][2],
            "wait_queue": f"0x{r['values'][0]:x}", "signal_queue": f"0x{signal['values'][0]:x}" if signal else "",
            "wait_cpu_tick": r["cpu_tick"], "signal_cpu_tick": signal["cpu_tick"] if signal else 0,
            "enqueue_gap_us": edge["enqueue_gap_us"],
            "matched_signal": signal is not None,
            "signal_frame_id": signal["frame_id"] if signal else 0,
            "signal_name": signal["name"] if signal else "",
            "cross_queue": edge["cross_queue"],
            "source": r["source"],
        })
        queue_dependencies.append(row)
    save("queue_dependencies.csv", queue_dependencies)

    # Non-perturbative queue-gap analysis. These are uncovered spans between
    # calibrated APUSR GPU intervals on one queue, not asserted hardware idle
    # time: uninstrumented game/driver work may exist inside them. Native wait
    # events are attached when available to make synchronization-shaped gaps
    # easy to find without inserting extra marker submissions.
    gpu_queue_gap_rows = []
    for gap in observed_gpu_queue_gaps(shards, records):
        native_queue = int(gap["native_queue"] or 0)
        gpu_queue_gap_rows.append({
            "process_id": gap["process_id"], "session": gap["session"], "module": gap["module"],
            "backend": gap["backend"], "component": gap["component"],
            "stream_id": gap["stream_id"],
            "stream_name": next((
                stream["name"] for (source, sid), stream in streams.items()
                if source == gap["source"] and sid == gap["stream_id"]), ""),
            "native_queue": f"0x{native_queue:x}",
            "previous_frame_id": gap["previous_frame_id"], "previous_name": gap["previous_name"],
            "next_frame_id": gap["next_frame_id"], "next_name": gap["next_name"],
            "gap_start_cpu_tick": gap["gap_start_cpu_tick"],
            "gap_end_cpu_tick": gap["gap_end_cpu_tick"],
            "gap_us": gap["gap_us"],
            "wait_associated": gap["wait_associated"], "wait_frame_id": gap["wait_frame_id"],
            "wait_name": gap["wait_name"],
            "sync_object": f"0x{int(gap['sync_object'] or 0):x}" if gap["sync_object"] else "",
            "sync_value": gap["sync_value"],
            "signal_queue": f"0x{int(gap['signal_queue'] or 0):x}" if gap["signal_queue"] else "",
            "matched_signal": gap["matched_signal"],
            "cross_queue_dependency": gap["cross_queue_dependency"],
            "source": gap["source"],
        })
    save("gpu_queue_gaps.csv", gpu_queue_gap_rows)

    gpu_gap_stats_groups: dict[tuple, list[dict]] = {}
    for row in gpu_queue_gap_rows:
        key = (row["process_id"], row["session"], row["backend"], row["component"],
               row["stream_name"], row["native_queue"], row["wait_associated"])
        gpu_gap_stats_groups.setdefault(key, []).append(row)
    gpu_gap_stats = []
    for key, rows in sorted(gpu_gap_stats_groups.items()):
        process_id, session, backend, component, stream, queue, wait_associated = key
        stats = distribution_stats([float(row["gap_us"]) for row in rows])
        gpu_gap_stats.append({
            "process_id": process_id, "session": session, "backend": backend, "component": component,
            "stream_name": stream, "native_queue": queue, "wait_associated": wait_associated,
            "samples": stats["samples"], "mean_gap_us": stats["mean"], "p95_gap_us": stats["p95"],
            "p99_gap_us": stats["p99"], "max_gap_us": stats["max"],
            "cross_queue_dependencies": sum(bool(row["cross_queue_dependency"]) for row in rows),
        })
    save("gpu_queue_gap_stats.csv", gpu_gap_stats)

    queue_stats_groups: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_QUEUE_EVENT:
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r["backend"], r["component"],
               stream_name(r), r["values"][0], QUEUE_OPERATIONS.get(r["aux0"], str(r["aux0"])), r["name"])
        queue_stats_groups.setdefault(key, []).append(r)
    queue_stats = []
    for key, rows in sorted(queue_stats_groups.items()):
        process_id, session, backend, component, stream, queue, operation, name = key
        queue_stats.append({
            "process_id": process_id, "session": session, "backend": backend, "component": component,
            "stream_name": stream, "native_queue": f"0x{queue:x}", "operation": operation, "name": name,
            "events": len(rows),
            "unique_sync_objects": len({r["values"][1] for r in rows if r["values"][1] != 0}),
            "max_sync_value": max((r["values"][2] for r in rows), default=0),
            "object_count_mean": statistics.fmean(r["aux2"] for r in rows) if rows else 0.0,
        })
    save("queue_event_stats.csv", queue_stats)

    counter_stats_groups: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_COUNTER:
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], stream_name(r), r["name"])
        counter_stats_groups.setdefault(key, []).append(r)
    counter_stats = []
    for key, rows in sorted(counter_stats_groups.items()):
        process_id, session, module, backend, component, stream, name = key
        value_stats = distribution_stats([float(r["values"][0]) for r in rows])
        value2_stats = distribution_stats([float(r["values"][1]) for r in rows])
        counter_stats.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_name": stream, "name": name,
            "samples": len(rows),
            **{f"value_{k}": v for k, v in value_stats.items() if k != "samples"},
            **{f"value2_{k}": v for k, v in value2_stats.items() if k != "samples"},
        })
    save("counter_stats.csv", counter_stats)

    decision_groups: dict[tuple, list[dict]] = {}
    decision_subject_totals: dict[tuple, int] = {}
    for row in decision_rows:
        subject_key = (
            row["process_id"], row["session"], row["module"], row["backend"],
            row["component"], row["stream_name"], row["name"])
        decision_subject_totals[subject_key] = decision_subject_totals.get(subject_key, 0) + 1
        key = subject_key + (row["outcome"], row["reason"])
        decision_groups.setdefault(key, []).append(row)
    decision_stats = []
    for key, rows in sorted(decision_groups.items()):
        process_id, session, module, backend, component, stream, name, outcome, reason = key
        subject_key = key[:-2]
        total = decision_subject_totals.get(subject_key, len(rows))
        decision_stats.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_name": stream,
            "name": name, "outcome": outcome, "reason": reason,
            "count": len(rows), "subject_total": total,
            "percent": (100.0 * len(rows) / total) if total else 0.0,
            "burst_triggers": sum(bool(row["triggered_burst"]) for row in rows),
            "first_frame_id": min((int(row["frame_id"]) for row in rows), default=0),
            "last_frame_id": max((int(row["frame_id"]) for row in rows), default=0),
        })
    save("decision_stats.csv", decision_stats)

    # Build one compact per-stream/per-frame structured-event index.  This is
    # intentionally generic: frame-generation, upscaling, frontend, and future
    # NPU paths can all answer "what happened to frame N?" without a custom
    # report query.  The raw CSVs remain authoritative; this table only joins
    # the event sequence and terminal disposition for fast triage.
    frame_event_groups: dict[tuple, dict] = {}

    def frame_group(row: dict) -> dict:
        key = (row["source"], row["stream_id"], int(row.get("frame_id", 0) or 0))
        group = frame_event_groups.get(key)
        if group is None:
            group = {
                "process_id": row.get("process_id", 0), "session": row.get("session", ""),
                "module": row.get("module", ""), "backend": row.get("backend", ""),
                "component": row.get("component", ""), "stream_name": row.get("stream_name", ""),
                "stream_id": row.get("stream_id", 0), "frame_id": int(row.get("frame_id", 0) or 0),
                "source": row.get("source", ""), "decisions": [], "errors": [], "waits": [],
                "flows": [], "first_cpu_tick": None, "last_cpu_tick": None,
            }
            frame_event_groups[key] = group
        tick = int(row.get("cpu_tick", 0) or 0)
        if tick:
            group["first_cpu_tick"] = tick if group["first_cpu_tick"] is None else min(group["first_cpu_tick"], tick)
            group["last_cpu_tick"] = tick if group["last_cpu_tick"] is None else max(group["last_cpu_tick"], tick)
        return group

    for row in decision_rows:
        if int(row.get("frame_id", 0) or 0) == 0:
            continue
        frame_group(row)["decisions"].append(row)
    for row in error_rows:
        if int(row.get("frame_id", 0) or 0) == 0:
            continue
        frame_group(row)["errors"].append(row)
    for row in wait_interval_rows:
        if int(row.get("frame_id", 0) or 0) == 0:
            continue
        frame_group(row)["waits"].append(row)
    for row in flow_rows:
        if int(row.get("frame_id", 0) or 0) == 0:
            continue
        frame_group(row)["flows"].append(row)

    frame_outcomes = []
    terminal_outcomes = {"presented", "source-only", "dropped", "fallback", "failed", "skipped", "warmup"}
    for _, group in sorted(frame_event_groups.items(), key=lambda item: (item[1]["process_id"], item[1]["stream_id"], item[1]["frame_id"])):
        decisions_for_frame = sorted(group["decisions"], key=lambda row: int(row.get("cpu_tick", 0) or 0))
        errors_for_frame = sorted(group["errors"], key=lambda row: int(row.get("cpu_tick", 0) or 0))
        waits_for_frame = sorted(group["waits"], key=lambda row: int(row.get("start_cpu_tick", 0) or 0))
        flows_for_frame = sorted(group["flows"], key=lambda row: int(row.get("cpu_tick", 0) or 0))
        terminal = next((row for row in reversed(decisions_for_frame) if row.get("outcome") in terminal_outcomes),
                        decisions_for_frame[-1] if decisions_for_frame else None)
        decision_path = ";".join(
            f"{row['name']}={row['outcome']}:{row['reason']}" if row.get("reason")
            else f"{row['name']}={row['outcome']}"
            for row in decisions_for_frame)
        error_path = ";".join(
            f"{row['stage']}:{row['message']}" if row.get("message") else row["stage"]
            for row in errors_for_frame)
        flow_terminals = [row for row in flows_for_frame if row.get("phase") in ("end", "cancel")]
        frame_outcomes.append({
            "process_id": group["process_id"], "session": group["session"], "module": group["module"],
            "backend": group["backend"], "component": group["component"],
            "stream_name": group["stream_name"], "stream_id": group["stream_id"], "frame_id": group["frame_id"],
            "first_cpu_tick": group["first_cpu_tick"] or 0, "last_cpu_tick": group["last_cpu_tick"] or 0,
            "decision_count": len(decisions_for_frame),
            "terminal_decision": terminal["name"] if terminal else "",
            "terminal_outcome": terminal["outcome"] if terminal else "",
            "terminal_reason": terminal["reason"] if terminal else "",
            "decision_path": decision_path,
            "error_count": len(errors_for_frame), "error_path": error_path,
            "explicit_wait_count": len(waits_for_frame),
            "explicit_wait_total_ms": sum(float(row.get("duration_ms", 0.0) or 0.0) for row in waits_for_frame),
            "explicit_wait_max_ms": max((float(row.get("duration_ms", 0.0) or 0.0) for row in waits_for_frame), default=0.0),
            "flow_count": len({(row.get("name", ""), row.get("flow_id", 0)) for row in flows_for_frame}),
            "flow_terminal_count": len(flow_terminals),
            "flow_cancel_count": sum(row.get("phase") == "cancel" for row in flow_terminals),
            "source": group["source"],
        })
    save("frame_outcomes.csv", frame_outcomes)

    # Reconstruct logical-state residency from explicit transitions.  The last
    # state in a trace is deliberately marked open-ended rather than assuming
    # it ended at process exit; the observed duration simply stops at the last
    # retained record in that shard.
    latest_tick_by_source: dict[str, int] = {}
    for record in records:
        source = record["source"]
        latest_tick_by_source[source] = max(
            latest_tick_by_source.get(source, 0), int(record["cpu_tick"]))
    state_groups: dict[tuple, list[dict]] = {}
    for row in state_rows:
        key = (row["source"], row["stream_id"], row["name"], row["subject"])
        state_groups.setdefault(key, []).append(row)
    state_residency = []
    for key, rows in sorted(state_groups.items()):
        source, stream_id, name, subject = key
        rows.sort(key=lambda row: int(row["cpu_tick"]))
        shard = shard_by_source.get(source)
        frequency = shard.cpu_frequency if shard else 0
        for index, row in enumerate(rows):
            start = int(row["cpu_tick"])
            if index + 1 < len(rows):
                end = int(rows[index + 1]["cpu_tick"])
                closed = True
            else:
                end = int(latest_tick_by_source.get(source, start))
                closed = False
            duration_ms = ((end - start) * 1000.0 / frequency) if frequency and end >= start else 0.0
            state_residency.append({
                "process_id": row["process_id"], "session": row["session"], "module": row["module"],
                "backend": row["backend"], "component": row["component"],
                "stream_name": row["stream_name"], "stream_id": stream_id,
                "name": name, "subject": subject, "state_value": row["next_value"],
                "entered_frame_id": row["frame_id"], "entered_cpu_tick": start,
                "observed_end_cpu_tick": end, "observed_duration_ms": duration_ms,
                "closed_by_next_transition": closed, "reason": row["reason"], "source": source,
            })
    save("state_residency.csv", state_residency)

    # Condition ordinary performance observations on durable logical states.
    # This is intentionally generic: a frontend enable bit, backend mode, model
    # epoch, quality mode, or future NPU state can all be compared without a
    # feature-specific report. State membership uses the host observation tick;
    # asynchronous GPU work that crosses a transition boundary should still be
    # inspected in timeline.json before making a boundary-sensitive claim.
    state_series: dict[tuple, list[dict]] = {}
    for row in state_rows:
        key = (row["process_id"], row["session"], row["name"], row["subject"])
        state_series.setdefault(key, []).append(row)
    for rows in state_series.values():
        rows.sort(key=lambda row: int(row["cpu_tick"]))

    performance_observations: dict[tuple[int, str], list[tuple[int, dict, str, str, float]]] = {}
    def add_state_observation(record: dict, tick: int, family: str, metric: str, value: float) -> None:
        if tick <= 0 or not math.isfinite(value):
            return
        performance_observations.setdefault((record.get("process_id", 0), record.get("session", "")), []).append(
            (tick, record, family, metric, value))

    for r in records:
        if r["type"] == TYPE_FRAME:
            if gpu_duration_status(r)[0]:
                add_state_observation(r, int(r["cpu_tick"]), "frame", "gpu_ms", r["values"][4] / 1e6)
            add_state_observation(r, int(r["cpu_tick"]), "frame", "cpu_total_ms", r["values"][5] / 1e6)
            add_state_observation(r, int(r["cpu_tick"]), "frame", "cpu_record_ms", r["values"][6] / 1e6)
        elif r["type"] == TYPE_GPU_INTERVAL and gpu_duration_status(r)[0]:
            add_state_observation(r, int(r["cpu_tick"]), "gpu", "gpu_ms", r["values"][0] / 1e6)
            add_state_observation(r, int(r["cpu_tick"]), "gpu", "execution_ms", r["values"][2] / 1e6)
            add_state_observation(r, int(r["cpu_tick"]), "gpu", "setup_ms", r["values"][1] / 1e6)
        elif r["type"] == TYPE_CPU_INTERVAL:
            add_state_observation(r, int(r["values"][0]), "cpu", "duration_ms", r["values"][2] / 1e6)
        elif r["type"] == TYPE_WAIT_INTERVAL:
            add_state_observation(r, int(r["values"][0]), "wait", "duration_ms", r["values"][2] / 1e6)

    conditioned_groups: dict[tuple, list[float]] = {}
    for (process_id, session, state_name, subject), transitions in sorted(state_series.items()):
        observations = performance_observations.get((process_id, session), [])
        transition_ticks = [int(row["cpu_tick"]) for row in transitions]
        for tick, obs, family, metric, value in observations:
            state_index = bisect.bisect_right(transition_ticks, tick) - 1
            if state_index < 0:
                continue
            state_value = transitions[state_index]["next_value"]
            obs_name = obs["name"] if family in ("gpu", "cpu", "wait") else "frame"
            key = (process_id, session, state_name, subject, state_value,
                   obs.get("module", ""), obs["backend"], obs["component"], stream_name(obs),
                   family, obs_name, metric)
            conditioned_groups.setdefault(key, []).append(value)
    state_conditioned_stats = []
    for key, values in sorted(conditioned_groups.items()):
        (process_id, session, state_name, subject, state_value, module, backend, component, stream,
         family, name, metric) = key
        stats = distribution_stats(values)
        state_conditioned_stats.append({
            "process_id": process_id, "session": session, "state_name": state_name,
            "state_subject": subject, "state_value": state_value, "module": module,
            "backend": backend, "component": component, "stream_name": stream,
            "family": family, "name": name, "metric": metric,
            **stats, "total": sum(values),
        })
    save("state_conditioned_stats.csv", state_conditioned_stats)

    resource_groups: dict[tuple, list[dict]] = {}
    for row in resource_rows:
        # Resource IDs are only meaningful inside the stream that emitted them.
        # Distinct logical contexts are allowed to expose the same stream name
        # and reuse small local IDs, so stream_id must be part of the ledger key.
        key = (row["process_id"], row["session"], row["module"], row["backend"],
               row["component"], row["stream_id"], row["stream_name"], row["name"], row["source"])
        resource_groups.setdefault(key, []).append(row)
    resource_balance = []
    for key, rows in sorted(resource_groups.items()):
        process_id, session, module, backend, component, stream_id, stream, name, source = key
        rows.sort(key=lambda row: int(row["cpu_tick"]))
        active: dict[int, int] = {}
        current_bytes = 0
        peak_bytes = 0
        peak_count = 0
        unmatched_close = 0
        indeterminate_close = 0
        shard = shard_by_source.get(source)
        retained_window_truncated = bool(shard and shard.write_sequence > shard.capacity)
        opens = 0
        closes = 0
        for row in rows:
            operation = row["operation"]
            resource_id = int(row["resource_id"] or 0)
            size = int(row["bytes"] or 0)
            if operation in ("create", "allocate", "import", "acquire"):
                opens += 1
                if resource_id:
                    old = active.get(resource_id)
                    if old is not None:
                        current_bytes -= old
                    active[resource_id] = size
                    current_bytes += size
                    peak_bytes = max(peak_bytes, current_bytes)
                    peak_count = max(peak_count, len(active))
            elif operation in ("destroy", "free", "release"):
                closes += 1
                if resource_id and resource_id in active:
                    current_bytes -= active.pop(resource_id)
                else:
                    if retained_window_truncated:
                        indeterminate_close += 1
                    else:
                        unmatched_close += 1
        resource_balance.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_id": stream_id, "stream_name": stream,
            "name": name, "events": len(rows), "opens": opens, "closes": closes,
            "peak_active_count": peak_count, "peak_active_bytes": peak_bytes,
            "outstanding_count": len(active), "outstanding_bytes": sum(active.values()),
            "unmatched_close_count": unmatched_close,
            "indeterminate_close_count": indeterminate_close,
            "retained_window_truncated": retained_window_truncated,
            "outstanding_resource_ids": ";".join(str(value) for value in sorted(active)),
            "source": source,
        })
    save("resource_balance.csv", resource_balance)

    # Individual resource lifetimes make leaks/reuse visible without manually
    # matching create/free rows in a giant event table. Re-opened IDs and
    # unmatched closes are retained as explicit malformed lifetimes rather than
    # silently repaired.
    resource_lifetimes = []
    for key, rows in sorted(resource_groups.items()):
        process_id, session, module, backend, component, stream_id, stream, name, source = key
        rows = sorted(rows, key=lambda row: int(row["cpu_tick"]))
        shard = shard_by_source.get(source)
        frequency = shard.cpu_frequency if shard else 0
        active_rows: dict[int, dict] = {}
        for row in rows:
            operation = row["operation"]
            resource_id = int(row["resource_id"] or 0)
            if resource_id == 0:
                continue
            if operation in ("create", "allocate", "import", "acquire"):
                previous = active_rows.get(resource_id)
                if previous is not None:
                    resource_lifetimes.append({
                        "process_id": process_id, "session": session, "module": module,
                        "backend": backend, "component": component, "stream_id": stream_id, "stream_name": stream,
                        "name": name, "resource_id": resource_id,
                        "native_handle": previous.get("native_handle", ""),
                        "open_operation": previous["operation"], "close_operation": "",
                        "open_frame_id": previous["frame_id"], "close_frame_id": "",
                        "open_cpu_tick": previous["cpu_tick"], "close_cpu_tick": "",
                        "lifetime_ms": 0.0, "bytes": previous["bytes"],
                        "alignment": previous["alignment"],
                        "heap_or_memory_type": previous["heap_or_memory_type"],
                        "detail": previous["detail"], "closed": False,
                        "problem": "reopened_without_close", "source": source,
                    })
                active_rows[resource_id] = row
            elif operation in ("destroy", "free", "release"):
                opened = active_rows.pop(resource_id, None)
                if opened is None:
                    resource_lifetimes.append({
                        "process_id": process_id, "session": session, "module": module,
                        "backend": backend, "component": component, "stream_id": stream_id, "stream_name": stream,
                        "name": name, "resource_id": resource_id,
                        "native_handle": row.get("native_handle", ""),
                        "open_operation": "", "close_operation": operation,
                        "open_frame_id": "", "close_frame_id": row["frame_id"],
                        "open_cpu_tick": "", "close_cpu_tick": row["cpu_tick"],
                        "lifetime_ms": 0.0, "bytes": row["bytes"],
                        "alignment": row["alignment"],
                        "heap_or_memory_type": row["heap_or_memory_type"],
                        "detail": row["detail"], "closed": False,
                        "problem": ("close_without_open_retained_window"
                                    if shard and shard.write_sequence > shard.capacity else "close_without_open"),
                        "source": source,
                    })
                    continue
                duration_ms = 0.0
                if frequency and int(row["cpu_tick"]) >= int(opened["cpu_tick"]):
                    duration_ms = (int(row["cpu_tick"]) - int(opened["cpu_tick"])) * 1000.0 / frequency
                resource_lifetimes.append({
                    "process_id": process_id, "session": session, "module": module,
                    "backend": backend, "component": component, "stream_id": stream_id, "stream_name": stream,
                    "name": name, "resource_id": resource_id,
                    "native_handle": opened.get("native_handle", ""),
                    "open_operation": opened["operation"], "close_operation": operation,
                    "open_frame_id": opened["frame_id"], "close_frame_id": row["frame_id"],
                    "open_cpu_tick": opened["cpu_tick"], "close_cpu_tick": row["cpu_tick"],
                    "lifetime_ms": duration_ms, "bytes": opened["bytes"],
                    "alignment": opened["alignment"],
                    "heap_or_memory_type": opened["heap_or_memory_type"],
                    "detail": opened["detail"], "closed": True,
                    "problem": "", "source": source,
                })
        observed_end = latest_tick_by_source.get(source, 0)
        for resource_id, opened in sorted(active_rows.items()):
            duration_ms = 0.0
            if frequency and observed_end >= int(opened["cpu_tick"]):
                duration_ms = (observed_end - int(opened["cpu_tick"])) * 1000.0 / frequency
            resource_lifetimes.append({
                "process_id": process_id, "session": session, "module": module,
                "backend": backend, "component": component, "stream_id": stream_id, "stream_name": stream,
                "name": name, "resource_id": resource_id,
                "native_handle": opened.get("native_handle", ""),
                "open_operation": opened["operation"], "close_operation": "",
                "open_frame_id": opened["frame_id"], "close_frame_id": "",
                "open_cpu_tick": opened["cpu_tick"], "close_cpu_tick": "",
                "lifetime_ms": duration_ms, "bytes": opened["bytes"],
                "alignment": opened["alignment"],
                "heap_or_memory_type": opened["heap_or_memory_type"],
                "detail": opened["detail"], "closed": False,
                "problem": "outstanding_at_trace_end", "source": source,
            })
    save("resource_lifetimes.csv", resource_lifetimes)

    flow_groups: dict[tuple, list[dict]] = {}
    for row in flow_rows:
        # A logical flow may intentionally cross APUSR streams/modules.  Flow IDs
        # are therefore scoped to process + session + semantic flow name rather
        # than to the stream that happened to emit each step.
        key = (row["process_id"], row["session"], row["flow_id"], row["name"])
        flow_groups.setdefault(key, []).append(row)
    flow_stats = []
    for key, rows in sorted(flow_groups.items()):
        process_id, session, flow_id, name = key
        rows.sort(key=lambda row: int(row["cpu_tick"]))
        begin = next((row for row in rows if row["phase"] == "begin"), None)
        terminal = next((row for row in reversed(rows) if row["phase"] in ("end", "cancel")), None)
        exemplar = begin or rows[0]
        shard = shard_by_source.get(exemplar["source"])
        frequency = shard.cpu_frequency if shard else 0
        duration_ms = 0.0
        if begin and terminal and frequency and int(terminal["cpu_tick"]) >= int(begin["cpu_tick"]):
            duration_ms = (int(terminal["cpu_tick"]) - int(begin["cpu_tick"])) * 1000.0 / frequency
        modules = sorted({row["module"] for row in rows})
        backends = sorted({row["backend"] for row in rows})
        components = sorted({row["component"] for row in rows})
        stream_names = sorted({row["stream_name"] for row in rows})
        stream_ids = sorted({str(row["stream_id"]) for row in rows})
        sources = sorted({row["source"] for row in rows})
        flow_stats.append({
            "process_id": process_id, "session": session,
            "module": exemplar["module"], "backend": exemplar["backend"],
            "component": exemplar["component"], "stream_name": exemplar["stream_name"],
            "stream_id": exemplar["stream_id"], "flow_id": flow_id,
            "parent_flow_id": exemplar["parent_flow_id"],
            "name": name, "frame_id": exemplar["frame_id"],
            "occurrence_id": exemplar.get("occurrence_id", 0), "events": len(rows),
            "steps": sum(row["phase"] == "step" for row in rows),
            "terminal_phase": terminal["phase"] if terminal else "",
            "terminal_detail": terminal["detail"] if terminal else "",
            "complete": bool(begin and terminal), "duration_ms": duration_ms,
            "participating_modules": ";".join(modules),
            "participating_backends": ";".join(backends),
            "participating_components": ";".join(components),
            "participating_stream_names": ";".join(stream_names),
            "participating_stream_ids": ";".join(stream_ids),
            "source": exemplar["source"], "participating_sources": ";".join(sources),
        })
    save("flow_stats.csv", flow_stats)

    flow_summary_groups: dict[tuple, list[dict]] = {}
    for row in flow_stats:
        key = (row["process_id"], row["session"], row["module"], row["backend"],
               row["component"], row["stream_name"], row["name"], row["terminal_phase"], row["terminal_detail"])
        flow_summary_groups.setdefault(key, []).append(row)
    flow_summary = []
    for key, rows in sorted(flow_summary_groups.items()):
        process_id, session, module, backend, component, stream, name, terminal_phase, terminal_detail = key
        durations = [float(row["duration_ms"]) for row in rows if row["complete"]]
        stats = distribution_stats(durations)
        flow_summary.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_name": stream,
            "name": name, "terminal_phase": terminal_phase, "terminal_detail": terminal_detail,
            "flows": len(rows), "complete_flows": sum(bool(row["complete"]) for row in rows),
            "mean_ms": stats["mean"], "median_ms": stats["median"], "p95_ms": stats["p95"],
            "p99_ms": stats["p99"], "max_ms": stats["max"],
        })
    save("flow_summary.csv", flow_summary)

    wait_groups: dict[tuple, list[dict]] = {}
    for row in wait_interval_rows:
        key = (row["process_id"], row["session"], row["module"], row["backend"],
               row["component"], row["stream_name"], row["name"], row["kind"],
               row["result"], row["detail"])
        wait_groups.setdefault(key, []).append(row)
    wait_reason_stats = []
    for key, rows in sorted(wait_groups.items()):
        process_id, session, module, backend, component, stream, name, kind, result, detail = key
        durations = [float(row["duration_ms"]) for row in rows]
        values = distribution_stats(durations)
        requested = [float(row["timeout_ms"]) for row in rows if float(row["timeout_ms"]) > 0.0]
        requested_stats = distribution_stats(requested)
        wait_reason_stats.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_name": stream,
            "name": name, "kind": kind, "result": result, "detail": detail,
            "samples": len(rows), "total_ms": sum(durations),
            "mean_ms": values["mean"], "median_ms": values["median"],
            "p95_ms": values["p95"], "p99_ms": values["p99"], "max_ms": values["max"],
            "timeout_requested_median_ms": requested_stats["median"],
            "timeout_requested_p95_ms": requested_stats["p95"],
        })
    wait_reason_stats.sort(key=lambda row: float(row["total_ms"]), reverse=True)
    save("wait_reason_stats.csv", wait_reason_stats)

    # Join explicit host waits to queue-signal enqueue records when both sides
    # expose the same native synchronization object/value. This remains an
    # enqueue relationship, not a claim about when the GPU signal executed.
    signal_map: dict[tuple[int, int, int], list[dict]] = {}
    binary_signal_map: dict[tuple[int, int], list[dict]] = {}
    for record in records:
        if record["type"] != TYPE_QUEUE_EVENT or record["aux0"] != 2:
            continue
        shard = shard_by_source.get(record["source"])
        pid = shard.process_id if shard else 0
        obj = int(record["values"][1])
        value = int(record["values"][2])
        if not obj:
            continue
        if value:
            signal_map.setdefault((pid, obj, value), []).append(record)
        else:
            binary_signal_map.setdefault((pid, obj), []).append(record)
    for items in list(signal_map.values()) + list(binary_signal_map.values()):
        items.sort(key=lambda row: int(row["cpu_tick"]))

    wait_dependencies = []
    for row in wait_interval_rows:
        obj = int(row.get("object_raw", 0) or 0)
        if not obj:
            continue
        pid = int(row.get("process_id", 0) or 0)
        value = int(row.get("value", 0) or 0)
        candidates = signal_map.get((pid, obj, value), []) if value else binary_signal_map.get((pid, obj), [])
        end_tick = int(row["end_cpu_tick"])
        signal = None
        for candidate in reversed(candidates):
            if int(candidate["cpu_tick"]) <= end_tick:
                signal = candidate
                break
        source_shard = shard_by_source.get(row["source"])
        frequency = source_shard.cpu_frequency if source_shard else 0
        delta_ms = 0.0
        if signal is not None and frequency:
            delta_ms = (int(signal["cpu_tick"]) - int(row["start_cpu_tick"])) * 1000.0 / frequency
        wait_dependencies.append({
            "process_id": pid, "session": row["session"], "module": row["module"],
            "backend": row["backend"], "component": row["component"],
            "stream_name": row["stream_name"], "frame_id": row["frame_id"],
            "name": row["name"], "kind": row["kind"], "result": row["result"],
            "object": row["object"], "value": value, "wait_duration_ms": row["duration_ms"],
            "signal_found": signal is not None,
            "signal_name": signal["name"] if signal else "",
            "signal_queue": f"0x{signal['values'][0]:x}" if signal and signal["values"][0] else "",
            "signal_enqueue_delta_from_wait_start_ms": delta_ms if signal else "",
            "cpu_enqueue_only": True,
            "source": row["source"],
        })
    save("wait_dependencies.csv", wait_dependencies)

    def union_ticks(intervals: list[tuple[int, int]]) -> int:
        if not intervals:
            return 0
        ordered = sorted((a, b) for a, b in intervals if b >= a)
        if not ordered:
            return 0
        total = 0
        start, end = ordered[0]
        for next_start, next_end in ordered[1:]:
            if next_start <= end:
                end = max(end, next_end)
            else:
                total += end - start
                start, end = next_start, next_end
        return total + end - start

    # Per-thread wall-time accounting uses interval unions so nested CPU scopes
    # do not double-count. It is intentionally "observed" utilization within
    # captured intervals, not process-wide scheduler utilization.
    thread_interval_groups: dict[tuple, dict[str, list[tuple[int, int]]]] = {}
    for row in cpu_rows:
        key = (row["source"], int(row["thread_id"]))
        thread_interval_groups.setdefault(key, {"cpu": [], "wait": []})["cpu"].append(
            (int(row["start_cpu_tick"]), int(row["end_cpu_tick"])))
    for row in wait_interval_rows:
        key = (row["source"], int(row["thread_id"]))
        thread_interval_groups.setdefault(key, {"cpu": [], "wait": []})["wait"].append(
            (int(row["start_cpu_tick"]), int(row["end_cpu_tick"])))
    thread_names: dict[tuple[str, int], dict] = {}
    for row in thread_rows:
        thread_names[(row["source"], int(row["thread_id"]))] = row
    thread_activity = []
    for key, groups in sorted(thread_interval_groups.items()):
        source, thread_id = key
        shard = shard_by_source.get(source)
        frequency = shard.cpu_frequency if shard else 0
        all_intervals = groups["cpu"] + groups["wait"]
        if not all_intervals or not frequency:
            continue
        first = min(a for a, _ in all_intervals)
        last = max(b for _, b in all_intervals)
        span_ticks = max(0, last - first)
        cpu_ticks = union_ticks(groups["cpu"])
        wait_ticks = union_ticks(groups["wait"])
        meta = thread_names.get(key, {})
        thread_activity.append({
            "process_id": shard.process_id if shard else 0,
            "session": shard.session if shard else "", "module": shard.module if shard else "",
            "thread_id": thread_id, "thread_name": meta.get("name", ""),
            "thread_role": meta.get("role", ""), "priority": meta.get("priority", ""),
            "observed_span_ms": span_ticks * 1000.0 / frequency,
            "cpu_scope_union_ms": cpu_ticks * 1000.0 / frequency,
            "explicit_wait_union_ms": wait_ticks * 1000.0 / frequency,
            "cpu_scope_fraction_pct": (100.0 * cpu_ticks / span_ticks) if span_ticks else 0.0,
            "explicit_wait_fraction_pct": (100.0 * wait_ticks / span_ticks) if span_ticks else 0.0,
            "source": source,
        })
    save("thread_activity.csv", thread_activity)

    # Application-facing WSI timing deserves its own compact view. The
    # generic CPU/counter reports keep every raw sample, while this table makes
    # acquire blocking, present-call cost, and host cadence immediately visible
    # for each captured swapchain stream.
    wsi_groups: dict[tuple, dict[str, list]] = {}
    for r in records:
        if stream_name(r) != "vulkan.wsi":
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], r["source"], r["stream_id"])
        group = wsi_groups.setdefault(key, {
            "acquire_ms": [], "present_ms": [], "acquire_interval_ms": [], "present_interval_ms": [],
            "acquire_results": [], "present_results": [], "metadata": {},
        })
        if r["type"] == TYPE_CPU_INTERVAL:
            if r["name"] == "wsi.acquire_call":
                group["acquire_ms"].append(r["values"][2] / 1e6)
            elif r["name"] == "wsi.present_call":
                group["present_ms"].append(r["values"][2] / 1e6)
        elif r["type"] == TYPE_COUNTER:
            if r["name"] == "wsi.acquire_interval_ns":
                group["acquire_interval_ms"].append(r["values"][0] / 1e6)
            elif r["name"] == "wsi.present_interval_ns":
                group["present_interval_ms"].append(r["values"][0] / 1e6)
            elif r["name"] == "wsi.acquire_result":
                group["acquire_results"].append(signed_u64(r["values"][0]))
            elif r["name"] == "wsi.present_result":
                group["present_results"].append(signed_u64(r["values"][0]))
            elif r["name"].startswith("wsi.swapchain_"):
                group["metadata"][r["name"]] = r

    wsi_stats = []
    for key, group in sorted(wsi_groups.items()):
        process_id, session, module, backend, component, source, stream_id = key
        acquire = distribution_stats(group["acquire_ms"])
        present = distribution_stats(group["present_ms"])
        acquire_interval = distribution_stats(group["acquire_interval_ms"])
        present_interval = distribution_stats(group["present_interval_ms"])
        extent_record = group["metadata"].get("wsi.swapchain_extent")
        contract_record = group["metadata"].get("wsi.swapchain_contract")
        handle_record = group["metadata"].get("wsi.swapchain_handle")
        extent_w, extent_h = unpack_pair(extent_record["values"][0]) if extent_record else (0, 0)
        fmt, present_mode = unpack_pair(contract_record["values"][0]) if contract_record else (0, 0)
        wsi_stats.append({
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_id": stream_id, "stream_name": "vulkan.wsi",
            "swapchain": f"0x{handle_record['values'][0]:x}" if handle_record else "",
            "width": extent_w, "height": extent_h,
            "image_count": extent_record["values"][1] if extent_record else 0,
            "format": fmt, "present_mode": present_mode,
            "image_usage": contract_record["values"][1] if contract_record else 0,
            "virtualized": bool(contract_record and (contract_record["flags"] & 1)),
            "acquire_samples": len(group["acquire_ms"]),
            "acquire_mean_ms": acquire["mean"], "acquire_p95_ms": acquire["p95"],
            "acquire_p99_ms": acquire["p99"], "acquire_max_ms": acquire["max"],
            "present_samples": len(group["present_ms"]),
            "present_mean_ms": present["mean"], "present_p95_ms": present["p95"],
            "present_p99_ms": present["p99"], "present_max_ms": present["max"],
            "acquire_interval_mean_ms": acquire_interval["mean"],
            "acquire_interval_p95_ms": acquire_interval["p95"],
            "present_interval_mean_ms": present_interval["mean"],
            "present_interval_p95_ms": present_interval["p95"],
            "acquire_error_samples": sum(value < 0 for value in group["acquire_results"]),
            "present_error_samples": sum(value < 0 for value in group["present_results"]),
            "source": source,
        })
    save("wsi_stats.csv", wsi_stats)

    pacing_groups: dict[tuple, list[float]] = {}
    pacing_value2: dict[tuple, list[float]] = {}
    pacing_units: dict[tuple, str] = {}

    def add_pacing_metric(r: dict, metric: str, unit: str, value: float, value2: float = 0.0) -> None:
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], stream_name(r), metric)
        pacing_groups.setdefault(key, []).append(value)
        pacing_value2.setdefault(key, []).append(value2)
        pacing_units[key] = unit

    pacing_cpu_prefixes = ("presentation.", "pacing.", "interpolation.")
    for r in records:
        if r["type"] == TYPE_CPU_INTERVAL and r["name"].startswith(pacing_cpu_prefixes):
            add_pacing_metric(r, r["name"], "ms", r["values"][2] / 1e6)
        elif r["type"] != TYPE_COUNTER:
            continue
        name = r["name"]
        if name in ("presentation.source_interval_ns", "presentation.source_interval_hint_ns",
                    "presentation.generated_visible_ns", "presentation.paced_present_interval_ns",
                    "presentation.generated_target_late_ns", "presentation.source_target_late_ns"):
            add_pacing_metric(r, name, "ms", r["values"][0] / 1e6, r["values"][1] / 1e6)
        elif name in ("interpolation.present_delta_ticks", "interpolation.observed_delta_ticks"):
            frequency = r["values"][1]
            value_ms = (r["values"][0] * 1000.0 / frequency) if frequency else 0.0
            add_pacing_metric(r, name, "ms", value_ms, float(frequency))
        elif name == "presentation.queue_depth":
            add_pacing_metric(r, name, "count", float(r["values"][0]), float(r["values"][1]))
        elif name in ("interpolation.blocking_fence_wait", "presentation.generation_not_ready",
                    "presentation.generation_watchdog_timeout", "presentation.generation_deadline_miss"):
            add_pacing_metric(r, name, "boolean", float(r["values"][0]), float(r["values"][1]))
        elif name == "presentation.generated_attempted":
            add_pacing_metric(r, "presentation.generated_attempted", "boolean", float(r["values"][0]))
            add_pacing_metric(r, "presentation.generated_presented", "boolean", float(r["values"][1]))

    pacing_stats = []
    for key, values in sorted(pacing_groups.items()):
        process_id, session, module, backend, component, stream, metric = key
        stats = distribution_stats(values)
        secondary = pacing_value2.get(key, [])
        unit = pacing_units.get(key, "")
        pacing_stats.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_name": stream,
            "metric": metric, "unit": unit, "samples": len(values),
            "mean": stats["mean"], "median": stats["median"], "p95": stats["p95"],
            "p99": stats["p99"], "max": stats["max"],
            "true_percent": (100.0 * sum(value != 0.0 for value in values) / len(values))
                if unit == "boolean" and values else 0.0,
            "value2_mean": statistics.fmean(secondary) if secondary else 0.0,
        })
    save("pacing_stats.csv", pacing_stats)

    health_rows = []
    records_by_source: dict[str, list[dict]] = {}
    for r in records:
        records_by_source.setdefault(r["source"], []).append(r)
    for shard in shards:
        retained = min(shard.write_sequence, shard.capacity)
        overwritten = max(0, shard.write_sequence - shard.capacity)
        source_records = records_by_source.get(str(shard.path), [])
        latest_tick = max((r["cpu_tick"] for r in source_records), default=shard.start_tick)
        capture_span_seconds = ((latest_tick - shard.start_tick) / shard.cpu_frequency) \
            if shard.cpu_frequency and latest_tick >= shard.start_tick else 0.0
        records_per_second = shard.write_sequence / capture_span_seconds if capture_span_seconds > 0.0 else 0.0
        estimated_ring_seconds = shard.capacity / records_per_second if records_per_second > 0.0 else 0.0
        health_rows.append({
            "process_id": shard.process_id, "session": shard.session, "module": shard.module,
            "mode": mode_name(shard.mode),
            "sample_interval_ms": shard.sample_interval_ms, "burst_frames": shard.burst_frames,
            "source": str(shard.path), "capacity_records": shard.capacity,
            "ring_bytes": shard.capacity * RECORD_BYTES, "records_written": shard.write_sequence,
            "records_retained": retained, "records_overwritten": overwritten,
            "wraps": shard.write_sequence // shard.capacity if shard.capacity else 0,
            "retained_percent_of_written": (100.0 * retained / shard.write_sequence) if shard.write_sequence else 100.0,
            "capture_span_seconds": capture_span_seconds,
            "records_per_second": records_per_second,
            "estimated_ring_seconds_at_observed_rate": estimated_ring_seconds,
        })
    save("trace_health.csv", health_rows)

    record_count_rows = []
    for shard in shards:
        source = str(shard.path)
        counts: dict[str, int] = {}
        for r in records_by_source.get(source, []):
            record_type = RECORD_TYPES.get(r["type"], f"type_{r['type']}")
            counts[record_type] = counts.get(record_type, 0) + 1
        for record_type, count in sorted(counts.items()):
            record_count_rows.append({
                "process_id": shard.process_id, "session": shard.session, "module": shard.module,
                "mode": mode_name(shard.mode), "record_type": record_type, "retained_records": count,
                "source": source,
            })
    save("record_counts.csv", record_count_rows)

    # Per-stream coverage makes instrumentation gaps visible. Capability
    # declarations distinguish "supported but not observed in this capture"
    # from "coverage unknown". Seed from Stream records so a stream with zero
    # subsequent observations still appears in the report.
    stream_coverage_groups: dict[tuple, dict[str, int]] = {}
    for r in records:
        if r["type"] != TYPE_STREAM:
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], r["name"], r["stream_id"], r["source"])
        stream_coverage_groups.setdefault(key, {})
    for r in records:
        if r["type"] == TYPE_STREAM:
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], stream_name(r), r["stream_id"], r["source"])
        record_type = RECORD_TYPES.get(r["type"], f"type_{r['type']}")
        counts = stream_coverage_groups.setdefault(key, {})
        counts[record_type] = counts.get(record_type, 0) + 1
    coverage_types = [
        "cpu_interval", "frame", "gpu_interval", "calibration", "counter", "instant",
        "error", "queue_event", "decision", "state_transition", "resource_event",
        "flow_event", "thread_info", "wait_interval", "metadata", "gpu_workload_info",
    ]
    stream_coverage = []
    capability_coverage = []
    for key, counts in sorted(stream_coverage_groups.items()):
        process_id, session, module, backend, component, stream, stream_id, source = key
        stream_meta = streams.get((source, stream_id))
        capability_mask = int(stream_meta["values"][4]) if stream_meta else 0
        declared_names = capability_names(capability_mask)
        row = {
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "stream_id": stream_id,
            "retained_records": sum(counts.values()),
            "capabilities": f"0x{capability_mask:016x}",
            "capability_names": ";".join(declared_names),
            "capability_manifest_present": bool(capability_mask),
            "source": source,
        }
        row.update({f"{record_type}_records": counts.get(record_type, 0) for record_type in coverage_types})
        stream_coverage.append(row)

        relevant_records = {record_type for record_type in counts if record_type in STREAM_CAPABILITY_BY_RECORD}
        declared_records = {record_type for bit, (_, record_type) in STREAM_CAPABILITIES.items() if capability_mask & bit}
        for record_type in sorted(declared_records | relevant_records):
            bit, capability_name = STREAM_CAPABILITY_BY_RECORD[record_type]
            declared = bool(capability_mask & bit)
            observed = counts.get(record_type, 0)
            if not capability_mask:
                status = "manifest_unknown"
            elif declared and observed:
                status = "observed"
            elif declared:
                status = "declared_not_observed"
            else:
                status = "observed_undeclared"
            capability_coverage.append({
                "process_id": process_id, "session": session, "module": module, "backend": backend,
                "component": component, "stream_name": stream, "stream_id": stream_id,
                "capability": capability_name, "record_type": record_type,
                "declared": declared, "observed_records": observed, "status": status, "source": source,
            })
    save("stream_coverage.csv", stream_coverage)
    save("capability_coverage.csv", capability_coverage)

    anomalies = []
    for row in health_rows:
        if int(row["records_overwritten"]) > 0:
            anomalies.append({
                "severity": "warning", "category": "trace", "process_id": row["process_id"],
                "session": row["session"], "module": row["module"], "backend": "", "component": "",
                "stream_name": "", "frame_id": "", "name": "ring_overwrite",
                "detail": f"{row['records_overwritten']} records overwritten; retained {row['retained_percent_of_written']:.1f}%",
                "source": row["source"],
            })
    for row in error_rows:
        anomalies.append({
            "severity": "error", "category": "runtime", "process_id": row["process_id"],
            "session": row["session"], "module": row["module"], "backend": row["backend"],
            "component": row["component"], "stream_name": row["stream_name"],
            "frame_id": row["frame_id"], "name": row["stage"], "detail": row["message"],
            "source": row["source"],
        })
    for row in capability_coverage:
        if row["status"] != "observed_undeclared":
            continue
        anomalies.append({
            "severity": "warning", "category": "profiler_coverage", "process_id": row["process_id"],
            "session": row["session"], "module": row["module"], "backend": row["backend"],
            "component": row["component"], "stream_name": row["stream_name"],
            "frame_id": "", "name": "capability_manifest_mismatch",
            "detail": f"observed {row['record_type']} records without declared {row['capability']} capability",
            "source": row["source"],
        })
    for row in resource_lifetimes:
        if not row["problem"] or row["problem"] == "close_without_open_retained_window":
            continue
        anomalies.append({
            "severity": "warning", "category": "resource", "process_id": row["process_id"],
            "session": row["session"], "module": row["module"], "backend": row["backend"],
            "component": row["component"], "stream_name": row["stream_name"],
            "frame_id": row["open_frame_id"] or row["close_frame_id"], "name": row["name"],
            "detail": f"{row['problem']} resource_id={row['resource_id']} bytes={row['bytes']}",
            "source": row["source"],
        })
    for row in flow_stats:
        if row["complete"]:
            continue
        anomalies.append({
            "severity": "warning", "category": "flow", "process_id": row["process_id"],
            "session": row["session"], "module": row["module"], "backend": row["backend"],
            "component": row["component"], "stream_name": row["stream_name"],
            "frame_id": row["frame_id"], "name": row["name"],
            "detail": f"incomplete flow id={row['flow_id']} terminal={row['terminal_phase']}",
            "source": row["source"],
        })
    for row in wait_interval_rows:
        if row["result"] not in ("timeout", "interrupted", "cancelled", "failed"):
            continue
        anomalies.append({
            "severity": "error" if row["result"] == "failed" else "warning",
            "category": "wait", "process_id": row["process_id"], "session": row["session"],
            "module": row["module"], "backend": row["backend"], "component": row["component"],
            "stream_name": row["stream_name"], "frame_id": row["frame_id"], "name": row["name"],
            "detail": f"{row['kind']} {row['result']} after {float(row['duration_ms']):.3f} ms object={row['object']} value={row['value']}",
            "source": row["source"],
        })
    for row in decision_rows:
        if row["outcome"] not in ("dropped", "failed", "fallback"):
            continue
        anomalies.append({
            "severity": "error" if row["outcome"] == "failed" else "warning",
            "category": "decision", "process_id": row["process_id"], "session": row["session"],
            "module": row["module"], "backend": row["backend"], "component": row["component"],
            "stream_name": row["stream_name"], "frame_id": row["frame_id"], "name": row["name"],
            "detail": f"{row['outcome']} reason={row['reason']}", "source": row["source"],
        })
    save("anomalies.csv", anomalies)

    # Capture-quality facts are deliberately conservative. A missing optional
    # capability is reported as information; only destroyed evidence (ring
    # overwrite) or explicit profiler/runtime anomalies are warnings/errors.
    capture_quality = []
    for row in health_rows:
        overwritten = int(row["records_overwritten"])
        capture_quality.append({
            "process_id": row["process_id"], "session": row["session"], "module": row["module"],
            "backend": "", "component": "", "stream_name": "", "stream_id": "",
            "check": "ring_retention", "status": "warning" if overwritten else "pass",
            "value": overwritten,
            "detail": (f"{overwritten} retained-window records were overwritten" if overwritten
                       else "no ring overwrite observed"),
            "source": row["source"],
        })
    coverage_by_stream = {
        (row["process_id"], row["session"], row["module"], row["backend"], row["component"], row["stream_name"], row["stream_id"], row["source"]): row
        for row in stream_coverage
    }
    calibration_streams = {
        (row["process_id"], row["session"], row["module"], row["backend"], row["component"], row["stream_name"], row["stream_id"], row["source"])
        for row in calibration_rows
    }
    invalid_gpu_timing_by_stream: dict[tuple, int] = {}
    for record in records:
        if record["type"] not in (TYPE_FRAME, TYPE_GPU_INTERVAL) or gpu_duration_status(record)[0]:
            continue
        timing_key = (
            record.get("process_id", 0), record.get("session", ""), record.get("module", ""),
            record["backend"], record["component"], stream_name(record), record["stream_id"], record["source"],
        )
        invalid_gpu_timing_by_stream[timing_key] = invalid_gpu_timing_by_stream.get(timing_key, 0) + 1
    for key, row in sorted(coverage_by_stream.items()):
        process_id, session, module, backend, component, stream, stream_id, source = key
        gpu_records = int(row.get("gpu_interval_records", 0))
        frame_records = int(row.get("frame_records", 0))
        if gpu_records:
            calibrated = key in calibration_streams
            invalid_timing = invalid_gpu_timing_by_stream.get(key, 0)
            if invalid_timing:
                capture_quality.append({
                    "process_id": process_id, "session": session, "module": module,
                    "backend": backend, "component": component, "stream_name": stream, "stream_id": stream_id,
                    "check": "gpu_clock_calibration", "status": "warning",
                    "value": invalid_timing,
                    "detail": (f"{invalid_timing} GPU timing records contain raw ticks but no timestamp frequency; "
                               "their durations are unavailable and excluded from timing statistics"),
                    "source": source,
                })
            else:
                capture_quality.append({
                    "process_id": process_id, "session": session, "module": module,
                    "backend": backend, "component": component, "stream_name": stream, "stream_id": stream_id,
                    "check": "gpu_clock_calibration", "status": "pass" if calibrated else "info",
                    "value": int(calibrated),
                    "detail": ("absolute GPU/CPU timeline calibration observed" if calibrated else
                               "GPU durations are valid, but absolute timeline placement is not calibrated for this stream"),
                    "source": source,
                })
        samples = frame_records if frame_records else gpu_records
        if samples:
            capture_quality.append({
                "process_id": process_id, "session": session, "module": module,
                "backend": backend, "component": component, "stream_name": stream, "stream_id": stream_id,
                "check": "sample_volume", "status": "info", "value": samples,
                "detail": "retained frame samples" if frame_records else "retained GPU interval records (no frame record on this stream)",
                "source": source,
            })

        capability_mask = int(str(row.get("capabilities", "0")), 16) if str(row.get("capabilities", "")).startswith("0x") else 0
        observed_undeclared = [
            item["capability"] for item in capability_coverage
            if item["process_id"] == process_id and item["session"] == session and
               item["module"] == module and item["backend"] == backend and
               item["component"] == component and item["stream_name"] == stream and
               item["stream_id"] == stream_id and item["source"] == source and item["status"] == "observed_undeclared"
        ]
        if capability_mask == 0:
            capture_quality.append({
                "process_id": process_id, "session": session, "module": module,
                "backend": backend, "component": component, "stream_name": stream, "stream_id": stream_id,
                "check": "capability_manifest", "status": "info", "value": 0,
                "detail": "semantic capability manifest unavailable (compatible legacy or unannotated stream)",
                "source": source,
            })
        else:
            declared_count = len(capability_names(capability_mask))
            capture_quality.append({
                "process_id": process_id, "session": session, "module": module,
                "backend": backend, "component": component, "stream_name": stream, "stream_id": stream_id,
                "check": "capability_manifest", "status": "warning" if observed_undeclared else "pass",
                "value": declared_count,
                "detail": ("observed undeclared semantic classes: " + ", ".join(observed_undeclared)
                           if observed_undeclared else f"declares {declared_count} semantic record classes"),
                "source": source,
            })

        if gpu_records:
            workload_records = int(row.get("gpu_workload_info_records", 0))
            declares_workload_info = bool(capability_mask & (1 << 15))
            capture_quality.append({
                "process_id": process_id, "session": session, "module": module,
                "backend": backend, "component": component, "stream_name": stream, "stream_id": stream_id,
                "check": "gpu_workload_metadata",
                "status": "pass" if workload_records else "info",
                "value": workload_records,
                "detail": ("static pipeline/code identity observed" if workload_records else
                           ("workload metadata is supported but no pipeline identity was retained in this capture"
                            if declares_workload_info else "GPU timing is present; static pipeline/code metadata is not declared for this stream")),
                "source": source,
            })
    severity_counts = {"error": 0, "warning": 0}
    for row in anomalies:
        severity = row.get("severity", "")
        if severity in severity_counts:
            severity_counts[severity] += 1
    capture_quality.append({
        "process_id": "", "session": "", "module": "", "backend": "", "component": "",
        "stream_name": "", "stream_id": "", "check": "explicit_anomalies",
        "status": "error" if severity_counts["error"] else ("warning" if severity_counts["warning"] else "pass"),
        "value": severity_counts["error"] + severity_counts["warning"],
        "detail": f"errors={severity_counts['error']} warnings={severity_counts['warning']}", "source": "",
    })
    save("capture_quality.csv", capture_quality)

    frame_cfg = {
        (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]): r["values"][0]
        for r in records if r["type"] == TYPE_FRAME
    }
    frame_cfg_by_frame = {
        (r["source"], r["stream_id"], r["frame_id"]): r["values"][0]
        for r in records if r["type"] == TYPE_FRAME
    }

    def configuration_for(r: dict) -> int:
        return frame_cfg.get(
            (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]),
            frame_cfg_by_frame.get((r["source"], r["stream_id"], r["frame_id"]), 0))
    calibration_keys = {
        (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue_override(r))
        for r in records if r["type"] == TYPE_CALIBRATION
    }

    # Distribution reports preserve each run/configuration instead of burying
    # tail latency behind a single grand mean.
    frame_groups: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_FRAME:
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], stream_name(r), r["values"][0])
        frame_groups.setdefault(key, []).append(r)
    frame_stats = []
    for key, rows in sorted(frame_groups.items()):
        process_id, session, module, backend, component, stream, cfg = key
        gpu = distribution_stats([r["values"][4] / 1e6 for r in rows if r["values"][4] > 0 and gpu_duration_status(r)[0]])
        cpu_total = distribution_stats([r["values"][5] / 1e6 for r in rows if r["values"][5] > 0])
        cpu_record = distribution_stats([r["values"][6] / 1e6 for r in rows if r["values"][6] > 0])
        render_w, render_h = unpack_pair(rows[0]["values"][2])
        display_w, display_h = unpack_pair(rows[0]["values"][3])
        calibrated = sum(
            (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue_override(r)) in calibration_keys
            or (native_queue_override(r) != 0 and
                (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], 0) in calibration_keys)
            for r in rows
        )
        frame_stats.append({
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
            "samples": len(rows),
            "configuration_change_samples": sum(bool(r["flags"] & FRAME_CONFIGURATION_CHANGED) for r in rows),
            "failed_samples": sum(bool(r["flags"] & FRAME_FAILED) for r in rows),
            "backend_switch_samples": sum(bool(r["flags"] & FRAME_BACKEND_SWITCHED) for r in rows),
            "render_width": render_w, "render_height": render_h,
            "display_width": display_w, "display_height": display_h,
            "gpu_clock_calibrated_samples": calibrated,
            **{f"gpu_{k}_ms": v for k, v in gpu.items() if k != "samples"},
            **{f"cpu_total_{k}_ms": v for k, v in cpu_total.items() if k != "samples"},
            **{f"cpu_record_{k}_ms": v for k, v in cpu_record.items() if k != "samples"},
            "workgroups_mean": statistics.fmean(r["values"][7] for r in rows),
        })
    save("frame_stats.csv", frame_stats)

    cpu_interval_groups: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_CPU_INTERVAL:
            continue
        cfg = configuration_for(r)
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], stream_name(r), cfg, r["name"])
        cpu_interval_groups.setdefault(key, []).append(r)
    cpu_interval_stats = []
    for key, rows in sorted(cpu_interval_groups.items()):
        process_id, session, module, backend, component, stream, cfg, name = key
        stats = distribution_stats([r["values"][2] / 1e6 for r in rows])
        cpu_interval_stats.append({
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
            "name": name, "samples": stats["samples"],
            **{f"cpu_{k}_ms": v for k, v in stats.items() if k != "samples"},
        })
    save("cpu_interval_stats.csv", cpu_interval_stats)

    # Synchronization is important enough to deserve its own compact report.
    # Keep the classification intentionally mechanical so the report never
    # invents a causal stall that the instrumentation did not actually name.
    wait_stats = []
    for row in cpu_interval_stats:
        lower = row["name"].lower()
        if "wait" not in lower and "sync" not in lower and "acquire" not in lower:
            continue
        wait_stats.append(dict(row))
    wait_stats.sort(key=lambda row: float(row.get("cpu_p95_ms", 0.0)), reverse=True)
    save("wait_stats.csv", wait_stats)

    # Derive exclusive/self time from nested CPU scopes.  The writer only
    # records interval boundaries; doing this offline keeps the hot path cheap.
    # Subtract the union of strictly-contained child ranges so grandchildren
    # are never double-counted. Overlapping-but-not-contained scopes remain
    # independent because inferring ownership would be dishonest.
    cpu_nesting_groups: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_CPU_INTERVAL:
            continue
        key = (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], r["thread_id"])
        cpu_nesting_groups.setdefault(key, []).append(r)

    cpu_exclusive_rows = []
    cpu_exclusive_groups: dict[tuple, list[tuple[float, float]]] = {}
    for rows in cpu_nesting_groups.values():
        for parent in rows:
            start = parent["values"][0]
            end = parent["values"][1]
            total_ticks = max(0, end - start)
            child_ranges = []
            for child in rows:
                if child is parent:
                    continue
                child_start = child["values"][0]
                child_end = child["values"][1]
                strictly_contained = (
                    child_start >= start and child_end <= end and
                    (child_start > start or child_end < end)
                )
                if strictly_contained and child_end > child_start:
                    child_ranges.append((child_start, child_end))

            covered_ticks = 0
            if child_ranges:
                child_ranges.sort()
                merged_start, merged_end = child_ranges[0]
                for child_start, child_end in child_ranges[1:]:
                    if child_start <= merged_end:
                        merged_end = max(merged_end, child_end)
                    else:
                        covered_ticks += merged_end - merged_start
                        merged_start, merged_end = child_start, child_end
                covered_ticks += merged_end - merged_start
            covered_ticks = min(covered_ticks, total_ticks)
            inclusive_ms = parent["values"][2] / 1e6
            exclusive_ms = inclusive_ms * (total_ticks - covered_ticks) / total_ticks if total_ticks else inclusive_ms
            nested_ms = max(0.0, inclusive_ms - exclusive_ms)
            cfg = configuration_for(parent)
            row = common(parent)
            row.update({
                "frame_id": parent["frame_id"], "sample_sequence": parent["correlation_id"],
                "thread_id": parent["thread_id"], "configuration_hash": f"0x{cfg:016x}",
                "name": parent["name"], "inclusive_ms": inclusive_ms,
                "exclusive_ms": exclusive_ms, "nested_ms": nested_ms,
                "exclusive_share_percent": (100.0 * exclusive_ms / inclusive_ms) if inclusive_ms else 0.0,
                "source": parent["source"],
            })
            cpu_exclusive_rows.append(row)
            agg_key = (parent.get("process_id", 0), parent.get("session", ""), parent.get("module", ""),
                       parent["backend"], parent["component"], stream_name(parent), cfg, parent["name"])
            cpu_exclusive_groups.setdefault(agg_key, []).append((inclusive_ms, exclusive_ms))
    save("cpu_exclusive.csv", cpu_exclusive_rows)

    cpu_exclusive_stats = []
    for key, values in sorted(cpu_exclusive_groups.items()):
        process_id, session, module, backend, component, stream, cfg, name = key
        inclusive = distribution_stats([value[0] for value in values])
        exclusive = distribution_stats([value[1] for value in values])
        inclusive_mean = float(inclusive["mean"])
        exclusive_mean = float(exclusive["mean"])
        cpu_exclusive_stats.append({
            "process_id": process_id, "session": session, "module": module,
            "backend": backend, "component": component, "stream_name": stream,
            "configuration_hash": f"0x{cfg:016x}", "name": name, "samples": len(values),
            **{f"inclusive_{k}_ms": v for k, v in inclusive.items() if k != "samples"},
            **{f"exclusive_{k}_ms": v for k, v in exclusive.items() if k != "samples"},
            "exclusive_share_percent_mean": (100.0 * exclusive_mean / inclusive_mean) if inclusive_mean else 0.0,
        })
    cpu_exclusive_stats.sort(key=lambda row: float(row.get("exclusive_mean_ms", 0.0)), reverse=True)
    save("cpu_exclusive_stats.csv", cpu_exclusive_stats)

    # Thread occupancy/concurrency is also derived offline from the interval
    # boundaries. Nested scopes are unioned per thread so instrumentation depth
    # cannot manufacture CPU work. These numbers describe captured APUSR scopes,
    # not whole-process CPU utilization.
    thread_frame_ranges: dict[tuple, list[tuple[int, int]]] = {}
    thread_frame_scopes: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_CPU_INTERVAL:
            continue
        key = (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], r["thread_id"])
        thread_frame_ranges.setdefault(key, []).append((r["values"][0], r["values"][1]))
        thread_frame_scopes.setdefault(key, []).append(r)

    cpu_thread_frames = []
    merged_thread_ranges: dict[tuple, list[tuple[int, int]]] = {}
    cpu_thread_stat_groups: dict[tuple, list[tuple[float, float, float]]] = {}
    for key, ranges in thread_frame_ranges.items():
        source, stream_id, frame_id, sample_sequence, thread_id = key
        shard = shard_by_source.get(source)
        rows = thread_frame_scopes[key]
        if not shard or not shard.cpu_frequency or not rows:
            continue
        merged = merge_tick_ranges(ranges)
        merged_thread_ranges[key] = merged
        if not merged:
            continue
        busy_ticks = sum(end - start for start, end in merged)
        span_ticks = max(end for _, end in merged) - min(start for start, _ in merged)
        busy_ms = busy_ticks * 1000.0 / shard.cpu_frequency
        span_ms = span_ticks * 1000.0 / shard.cpu_frequency
        coverage = 100.0 * busy_ticks / span_ticks if span_ticks else 0.0
        representative = rows[0]
        cfg = configuration_for(representative)
        row = common(representative)
        row.update({
            "frame_id": frame_id, "sample_sequence": sample_sequence, "thread_id": thread_id,
            "configuration_hash": f"0x{cfg:016x}", "scope_count": len(rows),
            "busy_ms": busy_ms, "span_ms": span_ms, "coverage_percent": coverage,
            "source": source,
        })
        cpu_thread_frames.append(row)
        stat_key = (representative.get("process_id", 0), representative.get("session", ""),
                    representative.get("module", ""), representative["backend"], representative["component"],
                    stream_name(representative), cfg, thread_id)
        cpu_thread_stat_groups.setdefault(stat_key, []).append((busy_ms, span_ms, coverage))
    save("cpu_thread_frames.csv", cpu_thread_frames)

    cpu_thread_stats = []
    for key, values in sorted(cpu_thread_stat_groups.items()):
        process_id, session, module, backend, component, stream, cfg, thread_id = key
        busy = distribution_stats([v[0] for v in values])
        span = distribution_stats([v[1] for v in values])
        coverage = distribution_stats([v[2] for v in values])
        cpu_thread_stats.append({
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
            "thread_id": thread_id, "samples": len(values),
            **{f"busy_{name}_ms": value for name, value in busy.items() if name != "samples"},
            **{f"span_{name}_ms": value for name, value in span.items() if name != "samples"},
            **{f"coverage_{name}_percent": value for name, value in coverage.items() if name != "samples"},
        })
    cpu_thread_stats.sort(key=lambda row: float(row.get("busy_mean_ms", 0.0)), reverse=True)
    save("cpu_thread_stats.csv", cpu_thread_stats)

    frame_thread_keys: dict[tuple, list[tuple]] = {}
    for key in merged_thread_ranges:
        frame_key = key[:4]
        frame_thread_keys.setdefault(frame_key, []).append(key)

    cpu_concurrency = []
    cpu_concurrency_groups: dict[tuple, list[tuple[float, float, float, float, float]]] = {}
    for frame_key, thread_keys in frame_thread_keys.items():
        source, stream_id, frame_id, sample_sequence = frame_key
        shard = shard_by_source.get(source)
        if not shard or not shard.cpu_frequency:
            continue
        all_ranges: list[tuple[int, int]] = []
        sum_busy_ticks = 0
        scope_count = 0
        representative = None
        events: list[tuple[int, int]] = []
        for thread_key in thread_keys:
            merged = merged_thread_ranges[thread_key]
            all_ranges.extend(merged)
            sum_busy_ticks += sum(end - start for start, end in merged)
            scope_count += len(thread_frame_scopes.get(thread_key, []))
            if representative is None and thread_frame_scopes.get(thread_key):
                representative = thread_frame_scopes[thread_key][0]
            for start, end in merged:
                events.append((start, 1))
                events.append((end, -1))
        if not representative or not all_ranges:
            continue
        wall_start = min(start for start, _ in all_ranges)
        wall_end = max(end for _, end in all_ranges)
        wall_span_ticks = max(0, wall_end - wall_start)
        active_ranges = merge_tick_ranges(all_ranges)
        active_ticks = sum(end - start for start, end in active_ranges)
        # Process endings before starts at the same tick to avoid inventing a
        # one-instant overlap between touching intervals.
        active_threads = 0
        peak_threads = 0
        for _, delta in sorted(events, key=lambda item: (item[0], item[1])):
            active_threads += delta
            peak_threads = max(peak_threads, active_threads)
        wall_span_ms = wall_span_ticks * 1000.0 / shard.cpu_frequency
        active_ms = active_ticks * 1000.0 / shard.cpu_frequency
        busy_sum_ms = sum_busy_ticks * 1000.0 / shard.cpu_frequency
        average_parallelism_wall = sum_busy_ticks / wall_span_ticks if wall_span_ticks else 0.0
        average_parallelism_active = sum_busy_ticks / active_ticks if active_ticks else 0.0
        active_coverage = 100.0 * active_ticks / wall_span_ticks if wall_span_ticks else 0.0
        cfg = configuration_for(representative)
        row = common(representative)
        row.update({
            "frame_id": frame_id, "sample_sequence": sample_sequence,
            "configuration_hash": f"0x{cfg:016x}", "thread_count": len(thread_keys),
            "scope_count": scope_count, "profiled_cpu_span_ms": wall_span_ms,
            "profiled_active_wall_ms": active_ms, "profiled_thread_busy_sum_ms": busy_sum_ms,
            "active_coverage_percent": active_coverage,
            "average_profiled_parallelism_wall": average_parallelism_wall,
            "average_profiled_parallelism_active": average_parallelism_active,
            "peak_profiled_threads": peak_threads, "source": source,
        })
        cpu_concurrency.append(row)
        stat_key = (representative.get("process_id", 0), representative.get("session", ""),
                    representative.get("module", ""), representative["backend"], representative["component"],
                    stream_name(representative), cfg)
        cpu_concurrency_groups.setdefault(stat_key, []).append(
            (wall_span_ms, busy_sum_ms, active_coverage, average_parallelism_wall, float(peak_threads)))
    save("cpu_concurrency.csv", cpu_concurrency)

    cpu_concurrency_stats = []
    for key, values in sorted(cpu_concurrency_groups.items()):
        process_id, session, module, backend, component, stream, cfg = key
        span = distribution_stats([v[0] for v in values])
        busy = distribution_stats([v[1] for v in values])
        coverage = distribution_stats([v[2] for v in values])
        parallel = distribution_stats([v[3] for v in values])
        peak = distribution_stats([v[4] for v in values])
        cpu_concurrency_stats.append({
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
            "samples": len(values),
            **{f"span_{name}_ms": value for name, value in span.items() if name != "samples"},
            **{f"busy_sum_{name}_ms": value for name, value in busy.items() if name != "samples"},
            **{f"active_coverage_{name}_percent": value for name, value in coverage.items() if name != "samples"},
            **{f"parallelism_{name}": value for name, value in parallel.items() if name != "samples"},
            **{f"peak_threads_{name}": value for name, value in peak.items() if name != "samples"},
        })
    cpu_concurrency_stats.sort(key=lambda row: float(row.get("parallelism_mean", 0.0)), reverse=True)
    save("cpu_concurrency_stats.csv", cpu_concurrency_stats)

    gpu_workload_groups: dict[tuple, list[dict]] = {}
    for row in gpu_workload_rows:
        key = (row.get("process_id", 0), row.get("session", ""), row.get("module", ""),
               row.get("backend", ""), row.get("component", ""), row.get("stream_name", ""), row.get("name", ""))
        gpu_workload_groups.setdefault(key, []).append(row)
    gpu_workload_stats = []
    gpu_workload_lookup: dict[tuple, dict] = {}
    for key, rows in sorted(gpu_workload_groups.items()):
        process_id, session, module, backend, component, stream, name = key
        code_sizes = [int(row.get("code_bytes", 0) or 0) for row in rows if int(row.get("code_bytes", 0) or 0) > 0]
        shared_sizes = [int(row.get("shared_memory_bytes", 0) or 0) for row in rows if int(row.get("shared_memory_bytes", 0) or 0) > 0]
        subgroup_sizes = sorted({int(row.get("subgroup_size", 0) or 0) for row in rows if int(row.get("subgroup_size", 0) or 0) > 0})
        thread_shapes = sorted({
            f"{int(row.get('threads_x', 0) or 0)}x{int(row.get('threads_y', 0) or 0)}x{int(row.get('threads_z', 0) or 0)}"
            for row in rows if any(int(row.get(k, 0) or 0) > 0 for k in ("threads_x", "threads_y", "threads_z"))
        })
        code_hashes = sorted({row.get("code_hash", "") for row in rows if row.get("code_hash", "")})
        native_pipelines = sorted({row.get("native_pipeline", "") for row in rows if row.get("native_pipeline", "")})
        details = sorted({row.get("detail", "") for row in rows if row.get("detail", "")})
        stat = {
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "name": name,
            "records": len(rows), "variants": len(code_hashes) if code_hashes else len({row.get("workload_id", "") for row in rows}),
            "native_pipeline_count": len(native_pipelines),
            "code_bytes_min": min(code_sizes) if code_sizes else 0,
            "code_bytes_median": percentile([float(v) for v in code_sizes], 0.5) if code_sizes else 0.0,
            "code_bytes_max": max(code_sizes) if code_sizes else 0,
            "shared_memory_bytes_max": max(shared_sizes) if shared_sizes else 0,
            "subgroup_sizes": ";".join(str(v) for v in subgroup_sizes),
            "thread_shapes": ";".join(thread_shapes),
            "code_hashes": ";".join(code_hashes),
            "details": ";".join(details),
        }
        gpu_workload_stats.append(stat)
        gpu_workload_lookup[key] = stat
    save("gpu_workload_stats.csv", gpu_workload_stats)

    hotspot_groups: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_GPU_INTERVAL or not gpu_duration_status(r)[0]:
            continue
        cfg = configuration_for(r)
        key = (r.get("process_id", 0), r.get("session", ""), r.get("module", ""),
               r["backend"], r["component"], stream_name(r), cfg, r["name"])
        hotspot_groups.setdefault(key, []).append(r)
    hotspot_stats = []
    for key, rows in sorted(hotspot_groups.items()):
        process_id, session, module, backend, component, stream, cfg, name = key
        total = distribution_stats([r["values"][0] / 1e6 for r in rows])
        setup = distribution_stats([r["values"][1] / 1e6 for r in rows])
        execution = distribution_stats([r["values"][2] / 1e6 for r in rows])
        workgroups = [
            unpack_pair(r["values"][5])[0] * unpack_pair(r["values"][5])[1] * unpack_pair(r["values"][6])[0]
            for r in rows
        ]
        mean_workgroups = statistics.fmean(workgroups) if workgroups else 0.0
        mean_total_ns = float(total["mean"]) * 1e6
        mean_execution_s = float(execution["mean"]) / 1000.0
        mean_read = statistics.fmean(r["values"][3] for r in rows)
        mean_write = statistics.fmean(r["values"][4] for r in rows)
        workload = gpu_workload_lookup.get((process_id, session, module, backend, component, stream, name), {})
        hotspot_stats.append({
            "process_id": process_id, "session": session, "module": module, "backend": backend,
            "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}", "name": name,
            "samples": total["samples"],
            **{f"gpu_{k}_ms": v for k, v in total.items() if k != "samples"},
            **{f"setup_{k}_ms": v for k, v in setup.items() if k != "samples"},
            **{f"execution_{k}_ms": v for k, v in execution.items() if k != "samples"},
            "setup_share_percent": (100.0 * float(setup["mean"]) / float(total["mean"])) if total["mean"] else 0.0,
            "workgroups_mean": mean_workgroups,
            "gpu_ns_per_workgroup_mean": (mean_total_ns / mean_workgroups) if mean_workgroups else 0.0,
            # These bytes are APUSR's logical resource-traffic estimate, not a
            # hardware memory-controller byte count.  The derived throughput is
            # therefore a workload-normalization metric, not claimed DRAM BW.
            "read_bytes_mean": mean_read,
            "write_bytes_mean": mean_write,
            "logical_io_gbps_mean": ((mean_read + mean_write) / mean_execution_s / 1e9) if mean_execution_s else 0.0,
            "barriers_mean": statistics.fmean(unpack_pair(r["values"][7])[0] for r in rows),
            "descriptors_mean": statistics.fmean(unpack_pair(r["values"][7])[1] for r in rows),
            "workload_variants": workload.get("variants", 0),
            "code_bytes_min": workload.get("code_bytes_min", 0),
            "code_bytes_median": workload.get("code_bytes_median", 0.0),
            "code_bytes_max": workload.get("code_bytes_max", 0),
            "shared_memory_bytes_max": workload.get("shared_memory_bytes_max", 0),
            "subgroup_sizes": workload.get("subgroup_sizes", ""),
            "thread_shapes": workload.get("thread_shapes", ""),
        })
    save("hotspot_stats.csv", hotspot_stats)

    gpu_by_frame: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] == TYPE_GPU_INTERVAL and gpu_duration_status(r)[0]:
            gpu_by_frame.setdefault((r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]), []).append(r)
    frame_breakdown = []
    for r in records:
        if r["type"] != TYPE_FRAME:
            continue
        passes = gpu_by_frame.get((r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]), [])
        pass_sum = sum(p["values"][0] for p in passes) / 1e6
        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"],
            "configuration_hash": f"0x{r['values'][0]:016x}", "frame_gpu_ms": r["values"][4] / 1e6,
            "pass_count": len(passes), "pass_sum_ms": pass_sum,
            "setup_sum_ms": sum(p["values"][1] for p in passes) / 1e6,
            "execution_sum_ms": sum(p["values"][2] for p in passes) / 1e6,
            "frame_minus_pass_sum_ms": r["values"][4] / 1e6 - pass_sum,
            "read_bytes_sum": sum(p["values"][3] for p in passes),
            "write_bytes_sum": sum(p["values"][4] for p in passes),
            "source": r["source"],
        })
        frame_breakdown.append(row)
    save("frame_breakdown.csv", frame_breakdown)

    # Keep the actual sampled frames that define each configuration's tail.
    # This makes a p95/p99 regression immediately traceable back to concrete
    # frame IDs and their pass mix rather than leaving only aggregate numbers.
    tail_frames = []
    tail_frame_records: list[tuple[dict, float, float, bool, bool]] = []
    for key, rows in sorted(frame_groups.items()):
        process_id, session, module, backend, component, stream, cfg = key
        gpu_values = [r["values"][4] / 1e6 for r in rows if r["values"][4] > 0 and gpu_duration_status(r)[0]]
        cpu_values = [r["values"][5] / 1e6 for r in rows if r["values"][5] > 0]
        gpu_p95 = percentile(gpu_values, .95) if gpu_values else 0.0
        cpu_p95 = percentile(cpu_values, .95) if cpu_values else 0.0
        for r in rows:
            gpu_valid = gpu_duration_status(r)[0]
            gpu_ms = r["values"][4] / 1e6 if gpu_valid else 0.0
            cpu_ms = r["values"][5] / 1e6
            gpu_tail = gpu_valid and gpu_p95 > 0.0 and gpu_ms >= gpu_p95
            cpu_tail = cpu_p95 > 0.0 and cpu_ms >= cpu_p95
            if not gpu_tail and not cpu_tail:
                continue
            tail_frame_records.append((r, gpu_p95, cpu_p95, gpu_tail, cpu_tail))
            passes = gpu_by_frame.get((r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]), [])
            tail_frames.append({
                "process_id": process_id, "session": session, "module": module, "backend": backend,
                "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
                "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"],
                "burst_sequence": r["values"][1], "burst_frame": r["aux0"],
                "gpu_ms": gpu_ms, "gpu_p95_ms": gpu_p95, "gpu_tail": gpu_tail,
                "cpu_total_ms": cpu_ms, "cpu_p95_ms": cpu_p95, "cpu_tail": cpu_tail,
                "pass_count": len(passes),
                "pass_sum_ms": sum(p["values"][0] for p in passes) / 1e6,
                "setup_sum_ms": sum(p["values"][1] for p in passes) / 1e6,
                "execution_sum_ms": sum(p["values"][2] for p in passes) / 1e6,
                "source": r["source"],
            })
    save("tail_frames.csv", tail_frames)

    # Per-frame stutter forensics. This intentionally stays offline: it joins
    # the tail frame to the heaviest GPU pass, CPU scope/self-time, named waits,
    # queue dependency context, and errors without adding instrumentation cost.
    # The report exposes correlation/context; it does not label the largest
    # neighbor as the cause of the stutter.
    cpu_by_frame: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] == TYPE_CPU_INTERVAL:
            cpu_by_frame.setdefault((r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]), []).append(r)

    exclusive_by_frame: dict[tuple, list[dict]] = {}
    for row in cpu_exclusive_rows:
        exclusive_by_frame.setdefault(
            (row["source"], row["stream_id"], row["frame_id"], row["sample_sequence"]), []).append(row)

    queue_by_frame: dict[tuple, list[dict]] = {}
    error_by_frame: dict[tuple, list[dict]] = {}
    for r in records:
        if r["type"] == TYPE_QUEUE_EVENT:
            queue_by_frame.setdefault(
                (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"]), []).append(r)
        elif r["type"] == TYPE_ERROR:
            error_by_frame.setdefault((r["source"], r["stream_id"], r["frame_id"]), []).append(r)

    dependency_by_frame: dict[tuple, list[dict]] = {}
    for edge in reconstruct_queue_dependencies(shards, records):
        wait = edge["wait"]
        dependency_by_frame.setdefault(
            (wait["source"], wait["stream_id"], wait["frame_id"], wait["correlation_id"]), []).append(edge)

    # Display timing can be produced by a different APUSR module/stream from
    # the frame sample (notably Vulkan host capture versus FrameGen). Join it
    # by process/session/backend/frame rather than source shard so stutter
    # forensics can show whether a slow sampled frame actually disturbed
    # presentation cadence. Generated and source presents remain separate.
    display_by_process_frame: dict[tuple, list[dict]] = {}
    for item in display_timing_rows:
        display_by_process_frame.setdefault((
            int(item.get("process_id", 0) or 0), item.get("session", ""),
            item.get("backend", ""), int(item.get("frame_id", 0) or 0)), []).append(item)

    # Memory-budget snapshots are sampled once per burst and can likewise live
    # on another stream. Keep a backend/process timeline so each tail frame can
    # carry the most recent local/non-local pressure observation without adding
    # any new runtime instrumentation.
    memory_budget_timeline: dict[tuple, list[dict]] = {}
    for item in memory_budget_rows:
        segment = item.get("segment", "")
        if segment not in ("local", "nonlocal"):
            continue
        key = (int(item.get("process_id", 0) or 0), item.get("session", ""), item.get("backend", ""), segment)
        memory_budget_timeline.setdefault(key, []).append(item)
    for items in memory_budget_timeline.values():
        items.sort(key=lambda item: int(item.get("cpu_tick", 0) or 0))

    def latest_budget(process_id: int, session: str, backend: str, segment: str, cpu_tick: int) -> dict | None:
        items = memory_budget_timeline.get((process_id, session, backend, segment), [])
        if not items:
            return None
        ticks = [int(item.get("cpu_tick", 0) or 0) for item in items]
        index = bisect.bisect_right(ticks, cpu_tick) - 1
        return items[index] if index >= 0 else None

    stutter_frames = []
    for r, gpu_p95, cpu_p95, gpu_tail, cpu_tail in tail_frame_records:
        frame_key = (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"])
        passes = gpu_by_frame.get(frame_key, [])
        cpu_scopes = cpu_by_frame.get(frame_key, [])
        exclusive_scopes = exclusive_by_frame.get(frame_key, [])
        queue_events_for_frame = queue_by_frame.get(frame_key, [])
        dependencies_for_frame = dependency_by_frame.get(frame_key, [])
        errors_for_frame = error_by_frame.get((r["source"], r["stream_id"], r["frame_id"]), [])

        top_gpu = max(passes, key=lambda item: item["values"][0], default=None)
        top_cpu = max(cpu_scopes, key=lambda item: item["values"][2], default=None)
        top_self = max(exclusive_scopes, key=lambda item: float(item["exclusive_ms"]), default=None)
        waits = [item for item in cpu_scopes if any(token in item["name"].lower() for token in ("wait", "sync", "acquire"))]
        top_wait = max(waits, key=lambda item: item["values"][2], default=None)
        gpu_ms = r["values"][4] / 1e6 if gpu_duration_status(r)[0] else 0.0
        cpu_ms = r["values"][5] / 1e6
        matched_dependencies = sum(edge["signal"] is not None for edge in dependencies_for_frame)
        cross_queue_dependencies = sum(edge["signal"] is not None and edge["cross_queue"] for edge in dependencies_for_frame)

        process_id = int(r.get("process_id", 0) or 0)
        session = r.get("session", "")
        backend = r.get("backend", "")
        display_rows = display_by_process_frame.get((process_id, session, backend, r["frame_id"]), [])
        generated_display = [item for item in display_rows if bool(item.get("generated", False))]
        source_display = [item for item in display_rows if not bool(item.get("generated", False))]

        def max_display(items: list[dict], field: str) -> float:
            return max((float(item.get(field, 0.0) or 0.0) for item in items), default=0.0)

        margin_values = [
            float(item.get("present_margin_ms", 0.0) or 0.0) for item in display_rows
            if "present_margin_ms" in item
        ]
        local_budget = latest_budget(process_id, session, backend, "local", int(r.get("cpu_tick", 0) or 0))
        nonlocal_budget = latest_budget(process_id, session, backend, "nonlocal", int(r.get("cpu_tick", 0) or 0))

        row = common(r)
        row.update({
            "frame_id": r["frame_id"], "sample_sequence": r["correlation_id"],
            "configuration_hash": f"0x{r['values'][0]:016x}",
            "tail_kind": "gpu+cpu" if gpu_tail and cpu_tail else ("gpu" if gpu_tail else "cpu"),
            "gpu_ms": gpu_ms, "gpu_p95_ms": gpu_p95,
            "gpu_over_p95": (gpu_ms / gpu_p95) if gpu_p95 else 0.0,
            "cpu_total_ms": cpu_ms, "cpu_p95_ms": cpu_p95,
            "cpu_over_p95": (cpu_ms / cpu_p95) if cpu_p95 else 0.0,
            "frame_failed": bool(r["flags"] & FRAME_FAILED),
            "top_gpu_pass": top_gpu["name"] if top_gpu else "",
            "top_gpu_pass_ms": (top_gpu["values"][0] / 1e6) if top_gpu else 0.0,
            "top_gpu_setup_ms": (top_gpu["values"][1] / 1e6) if top_gpu else 0.0,
            "top_gpu_execution_ms": (top_gpu["values"][2] / 1e6) if top_gpu else 0.0,
            "top_cpu_scope": top_cpu["name"] if top_cpu else "",
            "top_cpu_scope_ms": (top_cpu["values"][2] / 1e6) if top_cpu else 0.0,
            "top_cpu_self_scope": top_self["name"] if top_self else "",
            "top_cpu_self_ms": float(top_self["exclusive_ms"]) if top_self else 0.0,
            "wait_total_ms": sum(item["values"][2] for item in waits) / 1e6,
            "top_wait": top_wait["name"] if top_wait else "",
            "top_wait_ms": (top_wait["values"][2] / 1e6) if top_wait else 0.0,
            "queue_event_count": len(queue_events_for_frame),
            "queue_wait_dependencies": len(dependencies_for_frame),
            "matched_queue_wait_dependencies": matched_dependencies,
            "cross_queue_wait_dependencies": cross_queue_dependencies,
            "display_event_count": len(display_rows),
            "display_generated_event_count": len(generated_display),
            "display_source_event_count": len(source_display),
            "display_generated_interval_ms": max_display(generated_display, "actual_present_interval_ms"),
            "display_source_interval_ms": max_display(source_display, "actual_present_interval_ms"),
            "display_generated_present_begin_to_sync_ms": max_display(generated_display, "present_begin_to_sync_ms"),
            "display_source_present_begin_to_sync_ms": max_display(source_display, "present_begin_to_sync_ms"),
            "display_min_present_margin_ms": min(margin_values) if margin_values else 0.0,
            "memory_local_pressure_pct": float(local_budget.get("pressure_pct", 0.0) or 0.0) if local_budget else 0.0,
            "memory_local_headroom_mib": float(local_budget.get("headroom_mib", 0.0) or 0.0) if local_budget else 0.0,
            "memory_nonlocal_pressure_pct": float(nonlocal_budget.get("pressure_pct", 0.0) or 0.0) if nonlocal_budget else 0.0,
            "memory_nonlocal_headroom_mib": float(nonlocal_budget.get("headroom_mib", 0.0) or 0.0) if nonlocal_budget else 0.0,
            "error_count": len(errors_for_frame),
            "error_stages": ";".join(dict.fromkeys(item["name"] for item in errors_for_frame)),
            "source": r["source"],
        })
        stutter_frames.append(row)
    stutter_frames.sort(
        key=lambda row: max(float(row["gpu_over_p95"]), float(row["cpu_over_p95"])), reverse=True)
    save("stutter_frames.csv", stutter_frames)

    # Attribute each configuration's p95 tail to the passes/scopes that grow
    # on those concrete frames. This is correlation, not proof of causation,
    # but it is far more actionable than a percentile with no frame context.
    tail_attribution = []
    for key, rows in sorted(frame_groups.items()):
        process_id, session, module, backend, component, stream, cfg = key
        gpu_metric = [(r, r["values"][4] / 1e6) for r in rows if r["values"][4] > 0 and gpu_duration_status(r)[0]]
        cpu_metric = [(r, r["values"][5] / 1e6) for r in rows if r["values"][5] > 0]
        metric_name = "gpu" if gpu_metric else "cpu_total"
        metric_rows = gpu_metric if gpu_metric else cpu_metric
        if not metric_rows:
            continue
        threshold = percentile([value for _, value in metric_rows], .95)
        tail_keys = {
            (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"])
            for r, value in metric_rows if value >= threshold
        }
        non_tail_keys = {
            (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"])
            for r, value in metric_rows if value < threshold
        }

        def attribute(kind: str, by_frame: dict[tuple, list[dict]], value_index: int) -> None:
            names = {
                item["name"]
                for frame_key in tail_keys | non_tail_keys
                for item in by_frame.get(frame_key, [])
            }
            for name in sorted(names):
                tail_values = [
                    item["values"][value_index] / 1e6
                    for frame_key in tail_keys
                    for item in by_frame.get(frame_key, [])
                    if item["name"] == name
                ]
                base_values = [
                    item["values"][value_index] / 1e6
                    for frame_key in non_tail_keys
                    for item in by_frame.get(frame_key, [])
                    if item["name"] == name
                ]
                if not tail_values or not base_values:
                    continue
                tail_mean = statistics.fmean(tail_values)
                base_mean = statistics.fmean(base_values)
                tail_attribution.append({
                    "process_id": process_id, "session": session, "module": module, "backend": backend,
                    "component": component, "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
                    "tail_metric": metric_name, "tail_threshold_ms": threshold,
                    "kind": kind, "name": name,
                    "tail_samples": len(tail_values), "non_tail_samples": len(base_values),
                    "tail_mean_ms": tail_mean, "non_tail_mean_ms": base_mean,
                    "tail_minus_non_tail_ms": tail_mean - base_mean,
                    "tail_over_non_tail": (tail_mean / base_mean) if base_mean else 0.0,
                })

        attribute("gpu_pass", gpu_by_frame, 0)
        attribute("cpu_interval", cpu_by_frame, 2)

    tail_attribution.sort(
        key=lambda row: (float(row["tail_minus_non_tail_ms"]), float(row["tail_over_non_tail"])),
        reverse=True)
    save("tail_attribution.csv", tail_attribution)

    def field_values(samples: list[tuple[float, float, float, int, int]], index: int) -> list[float]:
        return [float(sample[index]) for sample in samples]

    def comparison_row(prefix: dict, dx: list[tuple], vk: list[tuple]) -> dict:
        dx_total = field_values(dx, 0)
        vk_total = field_values(vk, 0)
        dx_stats = distribution_stats(dx_total)
        vk_stats = distribution_stats(vk_total)
        dx_mean, vk_mean = float(dx_stats["mean"]), float(vk_stats["mean"])
        return {
            **prefix,
            "dx12_samples": len(dx), "vulkan_samples": len(vk),
            "dx12_mean_ms": dx_mean, "vulkan_mean_ms": vk_mean,
            "vulkan_minus_dx12_ms": vk_mean - dx_mean,
            "vulkan_over_dx12": (vk_mean / dx_mean) if dx_mean else 0.0,
            "dx12_median_ms": dx_stats["median"], "vulkan_median_ms": vk_stats["median"],
            "dx12_p95_ms": dx_stats["p95"], "vulkan_p95_ms": vk_stats["p95"],
            "dx12_p99_ms": dx_stats["p99"], "vulkan_p99_ms": vk_stats["p99"],
            "dx12_setup_mean_ms": statistics.fmean(field_values(dx, 1)),
            "vulkan_setup_mean_ms": statistics.fmean(field_values(vk, 1)),
            "vulkan_over_dx12_setup": (statistics.fmean(field_values(vk, 1)) / statistics.fmean(field_values(dx, 1)))
                if statistics.fmean(field_values(dx, 1)) else 0.0,
            "dx12_execution_mean_ms": statistics.fmean(field_values(dx, 2)),
            "vulkan_execution_mean_ms": statistics.fmean(field_values(vk, 2)),
            "vulkan_over_dx12_execution": (statistics.fmean(field_values(vk, 2)) / statistics.fmean(field_values(dx, 2)))
                if statistics.fmean(field_values(dx, 2)) else 0.0,
            "dx12_read_bytes_mean": statistics.fmean(field_values(dx, 3)),
            "vulkan_read_bytes_mean": statistics.fmean(field_values(vk, 3)),
            "dx12_write_bytes_mean": statistics.fmean(field_values(dx, 4)),
            "vulkan_write_bytes_mean": statistics.fmean(field_values(vk, 4)),
        }

    # Live comparison: only same process + session. This is the strict report
    # for a game that actually switches backend while staying alive.
    live_groups: dict[tuple, dict[str, list[tuple]]] = {}
    for r in records:
        if r["type"] != TYPE_GPU_INTERVAL or not gpu_duration_status(r)[0]:
            continue
        cfg = configuration_for(r)
        key = (r.get("process_id", 0), r.get("session", ""), r["component"], stream_name(r), cfg, r["name"])
        sample = (r["values"][0] / 1e6, r["values"][1] / 1e6, r["values"][2] / 1e6,
                  r["values"][3], r["values"][4])
        live_groups.setdefault(key, {}).setdefault(r["backend"], []).append(sample)

    compare = []
    for (process_id, session, component, stream, cfg, name), by_backend in sorted(live_groups.items()):
        if "dx12" not in by_backend or "vulkan" not in by_backend:
            continue
        compare.append(comparison_row({
            "process_id": process_id, "session": session, "component": component,
            "stream_name": stream, "configuration_hash": f"0x{cfg:016x}", "name": name,
        }, by_backend["dx12"], by_backend["vulkan"]))
    save("backend_compare.csv", compare)

    # Cross-process comparison is opt-in by construction: only shards carrying
    # the exact same non-empty APUSR_PROFILE_SESSION are aggregated. This makes
    # separate D3D12/Vulkan launches comparable without silently mixing runs.
    session_groups: dict[tuple, dict[str, list[tuple]]] = {}
    session_processes: dict[tuple, dict[str, set[int]]] = {}
    for r in records:
        if r["type"] != TYPE_GPU_INTERVAL or not r.get("session", "") or not gpu_duration_status(r)[0]:
            continue
        cfg = configuration_for(r)
        key = (r["session"], r["component"], stream_name(r), cfg, r["name"])
        sample = (r["values"][0] / 1e6, r["values"][1] / 1e6, r["values"][2] / 1e6,
                  r["values"][3], r["values"][4])
        session_groups.setdefault(key, {}).setdefault(r["backend"], []).append(sample)
        session_processes.setdefault(key, {}).setdefault(r["backend"], set()).add(r.get("process_id", 0))

    session_compare = []
    for (session, component, stream, cfg, name), by_backend in sorted(session_groups.items()):
        if "dx12" not in by_backend or "vulkan" not in by_backend:
            continue
        process_sets = session_processes[(session, component, stream, cfg, name)]
        session_compare.append(comparison_row({
            "session": session, "component": component, "stream_name": stream,
            "configuration_hash": f"0x{cfg:016x}", "name": name,
            "dx12_processes": len(process_sets.get("dx12", set())),
            "vulkan_processes": len(process_sets.get("vulkan", set())),
        }, by_backend["dx12"], by_backend["vulkan"]))
    save("backend_compare_session.csv", session_compare)

    def frame_comparison_row(prefix: dict, dx: list[dict], vk: list[dict]) -> dict:
        def metric(rows: list[dict], value_index: int, scale: float = 1.0, positive_only: bool = True,
                   require_valid_gpu: bool = False) -> dict:
            values = [r["values"][value_index] / scale for r in rows
                      if (not positive_only or r["values"][value_index] > 0)
                      and (not require_valid_gpu or gpu_duration_status(r)[0])]
            return distribution_stats(values)

        dx_gpu, vk_gpu = metric(dx, 4, 1e6, require_valid_gpu=True), metric(vk, 4, 1e6, require_valid_gpu=True)
        dx_cpu, vk_cpu = metric(dx, 5, 1e6), metric(vk, 5, 1e6)
        dx_record, vk_record = metric(dx, 6, 1e6), metric(vk, 6, 1e6)
        dx_gpu_mean, vk_gpu_mean = float(dx_gpu["mean"]), float(vk_gpu["mean"])
        dx_cpu_mean, vk_cpu_mean = float(dx_cpu["mean"]), float(vk_cpu["mean"])
        dx_record_mean, vk_record_mean = float(dx_record["mean"]), float(vk_record["mean"])
        return {
            **prefix,
            "dx12_samples": len(dx), "vulkan_samples": len(vk),
            "dx12_gpu_samples": dx_gpu["samples"], "vulkan_gpu_samples": vk_gpu["samples"],
            "dx12_gpu_mean_ms": dx_gpu_mean, "vulkan_gpu_mean_ms": vk_gpu_mean,
            "vulkan_minus_dx12_gpu_ms": vk_gpu_mean - dx_gpu_mean if dx_gpu["samples"] and vk_gpu["samples"] else 0.0,
            "vulkan_over_dx12_gpu": (vk_gpu_mean / dx_gpu_mean) if dx_gpu_mean and vk_gpu["samples"] else 0.0,
            "dx12_gpu_median_ms": dx_gpu["median"], "vulkan_gpu_median_ms": vk_gpu["median"],
            "dx12_gpu_p95_ms": dx_gpu["p95"], "vulkan_gpu_p95_ms": vk_gpu["p95"],
            "dx12_gpu_p99_ms": dx_gpu["p99"], "vulkan_gpu_p99_ms": vk_gpu["p99"],
            "dx12_cpu_samples": dx_cpu["samples"], "vulkan_cpu_samples": vk_cpu["samples"],
            "dx12_cpu_mean_ms": dx_cpu_mean, "vulkan_cpu_mean_ms": vk_cpu_mean,
            "vulkan_minus_dx12_cpu_ms": vk_cpu_mean - dx_cpu_mean if dx_cpu["samples"] and vk_cpu["samples"] else 0.0,
            "vulkan_over_dx12_cpu": (vk_cpu_mean / dx_cpu_mean) if dx_cpu_mean and vk_cpu["samples"] else 0.0,
            "dx12_cpu_median_ms": dx_cpu["median"], "vulkan_cpu_median_ms": vk_cpu["median"],
            "dx12_cpu_p95_ms": dx_cpu["p95"], "vulkan_cpu_p95_ms": vk_cpu["p95"],
            "dx12_cpu_p99_ms": dx_cpu["p99"], "vulkan_cpu_p99_ms": vk_cpu["p99"],
            "dx12_cpu_record_mean_ms": dx_record_mean, "vulkan_cpu_record_mean_ms": vk_record_mean,
            "vulkan_over_dx12_cpu_record": (vk_record_mean / dx_record_mean) if dx_record_mean and vk_record["samples"] else 0.0,
            "dx12_workgroups_mean": statistics.fmean(r["values"][7] for r in dx),
            "vulkan_workgroups_mean": statistics.fmean(r["values"][7] for r in vk),
            "dx12_failed_samples": sum(bool(r["flags"] & FRAME_FAILED) for r in dx),
            "vulkan_failed_samples": sum(bool(r["flags"] & FRAME_FAILED) for r in vk),
        }

    live_frame_groups: dict[tuple, dict[str, list[dict]]] = {}
    for r in records:
        if r["type"] != TYPE_FRAME:
            continue
        key = (r.get("process_id", 0), r.get("session", ""), r["component"], stream_name(r), r["values"][0])
        live_frame_groups.setdefault(key, {}).setdefault(r["backend"], []).append(r)

    frame_compare = []
    for (process_id, session, component, stream, cfg), by_backend in sorted(live_frame_groups.items()):
        if "dx12" not in by_backend or "vulkan" not in by_backend:
            continue
        frame_compare.append(frame_comparison_row({
            "process_id": process_id, "session": session, "component": component,
            "stream_name": stream, "configuration_hash": f"0x{cfg:016x}",
        }, by_backend["dx12"], by_backend["vulkan"]))
    save("backend_frame_compare.csv", frame_compare)

    session_frame_groups: dict[tuple, dict[str, list[dict]]] = {}
    session_frame_processes: dict[tuple, dict[str, set[int]]] = {}
    for r in records:
        if r["type"] != TYPE_FRAME or not r.get("session", ""):
            continue
        key = (r["session"], r["component"], stream_name(r), r["values"][0])
        session_frame_groups.setdefault(key, {}).setdefault(r["backend"], []).append(r)
        session_frame_processes.setdefault(key, {}).setdefault(r["backend"], set()).add(r.get("process_id", 0))

    session_frame_compare = []
    for (session, component, stream, cfg), by_backend in sorted(session_frame_groups.items()):
        if "dx12" not in by_backend or "vulkan" not in by_backend:
            continue
        processes = session_frame_processes[(session, component, stream, cfg)]
        session_frame_compare.append(frame_comparison_row({
            "session": session, "component": component, "stream_name": stream,
            "configuration_hash": f"0x{cfg:016x}",
            "dx12_processes": len(processes.get("dx12", set())),
            "vulkan_processes": len(processes.get("vulkan", set())),
        }, by_backend["dx12"], by_backend["vulkan"]))
    save("backend_frame_compare_session.csv", session_frame_compare)

    backend_switch_rows = []
    for row in instant_rows:
        if row["name"] != "backend.switch":
            continue
        backend_switch_rows.append(dict(row))
    save("backend_switches.csv", backend_switch_rows)

    # Errors are most useful when they are immediately tied to the timing
    # state around the failure.  Find the closest sampled frame before/after
    # each error in the same process.  The component/stream are reported
    # explicitly because the nearest sample may intentionally come from a
    # different APUSR layer (for example XRT error -> runtime frame burst).
    frame_ticks_by_process: dict[int, list[int]] = {}
    frame_records_by_process: dict[int, list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_FRAME:
            continue
        frame_records_by_process.setdefault(r.get("process_id", 0), []).append(r)
    for pid, rows in frame_records_by_process.items():
        rows.sort(key=lambda r: (r["cpu_tick"], r["sequence"]))
        frame_ticks_by_process[pid] = [r["cpu_tick"] for r in rows]

    shard_by_source = {str(shard.path): shard for shard in shards}
    error_context_rows = []
    for r in records:
        if r["type"] != TYPE_ERROR:
            continue
        pid = r.get("process_id", 0)
        candidates = frame_records_by_process.get(pid, [])
        ticks = frame_ticks_by_process.get(pid, [])
        index = bisect.bisect_left(ticks, r["cpu_tick"]) if ticks else 0
        previous = candidates[index - 1] if index > 0 else None
        following = candidates[index] if index < len(candidates) else None
        shard = shard_by_source.get(r["source"])
        cpu_frequency = shard.cpu_frequency if shard else 0

        row = common(r)
        row.update({
            "error_cpu_tick": r["cpu_tick"], "error_frame_id": r["frame_id"],
            "stage": r["name"], "status_code": r["values"][0], "message": r["detail"],
        })

        def add_neighbor(prefix: str, neighbor: dict | None) -> None:
            if neighbor is None:
                row.update({
                    f"{prefix}_delta_ms": "", f"{prefix}_backend": "", f"{prefix}_component": "",
                    f"{prefix}_stream_name": "", f"{prefix}_frame_id": "",
                    f"{prefix}_configuration_hash": "", f"{prefix}_gpu_ms": "", f"{prefix}_cpu_total_ms": "",
                    f"{prefix}_failed": "",
                })
                return
            delta_ticks = neighbor["cpu_tick"] - r["cpu_tick"]
            delta_ms = (delta_ticks * 1000.0 / cpu_frequency) if cpu_frequency else 0.0
            row.update({
                f"{prefix}_delta_ms": delta_ms, f"{prefix}_backend": neighbor["backend"],
                f"{prefix}_component": neighbor["component"], f"{prefix}_stream_name": stream_name(neighbor),
                f"{prefix}_frame_id": neighbor["frame_id"],
                f"{prefix}_configuration_hash": f"0x{neighbor['values'][0]:016x}",
                f"{prefix}_gpu_ms": neighbor["values"][4] / 1e6,
                f"{prefix}_cpu_total_ms": neighbor["values"][5] / 1e6,
                f"{prefix}_failed": bool(neighbor["flags"] & FRAME_FAILED),
            })

        add_neighbor("previous_sample", previous)
        add_neighbor("next_sample", following)
        row["source"] = r["source"]
        error_context_rows.append(row)
    save("error_context.csv", error_context_rows)

    # Human landing page for a generated report. Keep it evidence-oriented: it
    # points to the tables and ranks observed cost, but does not manufacture a
    # root cause from correlation.
    index = [
        "# APUSR profiler report",
        "",
        "This page is an index into captured evidence. Timing rankings show observed cost; they are not automatic causal attribution.",
        "",
        "## Capture quality",
        "",
    ]
    quality_rank = {"error": 0, "warning": 1, "pass": 2, "info": 3}
    for row in sorted(capture_quality, key=lambda r: (quality_rank.get(str(r.get("status", "")), 9), str(r.get("check", ""))))[:24]:
        scope = "/".join(part for part in (str(row.get("backend", "")), str(row.get("component", "")), str(row.get("stream_name", ""))) if part)
        scope = f" [{scope}]" if scope else ""
        index.append(f"- **{row.get('status', 'info')}** `{row.get('check', '')}`{scope}: {row.get('detail', '')}")
    index.extend([
        "",
        "Key tables: [capture quality](capture_quality.csv), [stream coverage](stream_coverage.csv), "
        "[capability coverage](capability_coverage.csv), [record counts](record_counts.csv), [anomalies](anomalies.csv).",
        "",
        "## Largest sampled GPU hotspots",
        "",
        "| backend | component | stream | workload | p50 ms | p95 ms | samples | code bytes |",
        "|---|---|---|---|---:|---:|---:|---:|",
    ])
    for row in sorted(hotspot_stats, key=lambda r: float(r.get("gpu_p95_ms", 0.0)), reverse=True)[:16]:
        index.append(
            f"| {row.get('backend','')} | {row.get('component','')} | {row.get('stream_name','')} | "
            f"{row.get('name','')} | {float(row.get('gpu_median_ms',0.0)):.4f} | "
            f"{float(row.get('gpu_p95_ms',0.0)):.4f} | {row.get('samples',0)} | {row.get('code_bytes_max',0)} |")
    if not hotspot_stats:
        index.append("| _no sampled GPU intervals_ | | | | | | | |")
    index.extend([
        "",
        "GPU detail: [hotspot statistics](hotspot_stats.csv), [raw GPU intervals](gpu_intervals.csv), "
        "[GPU workload metadata](gpu_workload_stats.csv), [Vulkan pipeline/cache activity](vulkan_pipeline_activity.csv), "
        "[frame breakdown](frame_breakdown.csv).",
        "",
        "## Largest explicit waits",
        "",
        "| backend | component | stream | wait | kind | result | total ms | p95 ms |",
        "|---|---|---|---|---|---|---:|---:|",
    ])
    for row in sorted(wait_reason_stats, key=lambda r: float(r.get("total_ms", 0.0)), reverse=True)[:16]:
        index.append(
            f"| {row.get('backend','')} | {row.get('component','')} | {row.get('stream_name','')} | "
            f"{row.get('name','')} | {row.get('kind','')} | {row.get('result','')} | "
            f"{float(row.get('total_ms',0.0)):.4f} | {float(row.get('p95_ms',0.0)):.4f} |")
    if not wait_reason_stats:
        index.append("| _no explicit waits retained_ | | | | | | | |")
    index.extend([
        "",
        "Wait detail: [explicit waits](wait_intervals.csv), [wait statistics](wait_reason_stats.csv), "
        "[native dependency joins](wait_dependencies.csv), [thread activity](thread_activity.csv).",
        "",
        "## Semantic outcomes and lifetimes",
        "",
        "Start with [frame outcomes](frame_outcomes.csv) for per-frame disposition, "
        "[decision statistics](decision_stats.csv) for policy reasons, [flow summary](flow_summary.csv) "
        "for cross-thread/module work, [state-conditioned performance](state_conditioned_stats.csv) for "
        "timing distributions split by durable runtime state, and [resource balance](resource_balance.csv) / "
        "[resource lifetimes](resource_lifetimes.csv) for ownership problems.",
        "",
        "## Timeline",
        "",
        "Open `timeline.json` in Perfetto or a Chrome-trace viewer for calibrated CPU/GPU intervals, "
        "worker names, waits, queue causality, and logical flow links.",
        "",
        "For a controlled A/B, generate two reports and run `apusr_profile_compare.py`; its statistical "
        "comparison is intentionally based on semantic workload identity rather than repository or application names.",
        "",
    ])
    (output / "report_index.md").write_text("\n".join(index), encoding="utf-8")

def write_summary(shards: list[Shard], records: list[dict], output: Path) -> str:
    streams = {(r["source"], r["stream_id"]): r for r in records if r["type"] == TYPE_STREAM}

    def summary_stream_name(r: dict) -> str:
        stream = streams.get((r["source"], r["stream_id"]))
        return stream["name"] if stream else ""

    lines = ["APUSR profiler summary", "======================", ""]
    lines.append(f"trace shards: {len(shards)}")
    lines.append(
        f"profiler ring capacity: {sum(shard.capacity for shard in shards) * RECORD_BYTES / (1024 * 1024):.1f} MiB total "
        f"across {len(shards)} shard(s)")
    for shard in shards:
        overwritten = max(0, shard.write_sequence - shard.capacity)
        retained = min(shard.write_sequence, shard.capacity)
        lines.append(
            f"  {shard.path.name}: module={shard.module} pid={shard.process_id} "
            f"mode={mode_name(shard.mode)} "
            f"retained={retained}/{shard.write_sequence} overwritten={overwritten} "
            f"ring={shard.capacity * RECORD_BYTES / (1024 * 1024):.1f} MiB")
    wrapped = [shard for shard in shards if shard.write_sequence > shard.capacity]
    if wrapped:
        lines.append(f"capture warning: {len(wrapped)} shard(s) wrapped; oldest profiler records were overwritten")
    else:
        lines.append("capture health: no ring overwrite detected")
    lines.append("")

    # Frame streams are phase-bearing identities. In particular, Vulkan frame
    # generation records framegen.compute and framegen.prepare separately;
    # averaging those two distributions together materially understates the
    # generated-frame GPU cost. Preserve stream identity in the human summary
    # just as frame_stats.csv already does.
    frames_by = {}
    for r in records:
        if r["type"] == TYPE_FRAME and r["values"][4] > 0 and gpu_duration_status(r)[0]:
            frames_by.setdefault((r["backend"], r["component"], summary_stream_name(r)), []).append(r["values"][4] / 1e6)
    lines.append("GPU frame samples")
    if not frames_by:
        lines.append("  none valid retained/captured")
    for (backend, component, stream), values in sorted(frames_by.items()):
        stats = distribution_stats(values)
        tail_ratio = stats["p99"] / stats["median"] if stats["median"] else 0.0
        lines.append(
            f"  {backend:7s} {component:16s} {stream:24s} n={len(values):4d} "
            f"mean={stats['mean']:8.3f} ms p50={stats['median']:8.3f} "
            f"p95={stats['p95']:8.3f} p99={stats['p99']:8.3f} max={stats['max']:8.3f} "
            f"p99/p50={tail_ratio:5.2f}x")
    lines.append("")

    cpu_frames_by = {}
    for r in records:
        if r["type"] == TYPE_FRAME and r["values"][5] > 0:
            cpu_frames_by.setdefault((r["backend"], r["component"], summary_stream_name(r)), []).append(r["values"][5] / 1e6)
    lines.append("CPU frame samples")
    if not cpu_frames_by:
        lines.append("  none retained/captured")
    for (backend, component, stream), values in sorted(cpu_frames_by.items()):
        stats = distribution_stats(values)
        tail_ratio = stats["p99"] / stats["median"] if stats["median"] else 0.0
        lines.append(
            f"  {backend:7s} {component:16s} {stream:24s} n={len(values):4d} "
            f"mean={stats['mean']:8.3f} ms p50={stats['median']:8.3f} "
            f"p95={stats['p95']:8.3f} p99={stats['p99']:8.3f} max={stats['max']:8.3f} "
            f"p99/p50={tail_ratio:5.2f}x")
    lines.append("")

    pipeline_activity_path = output / "vulkan_pipeline_activity.csv"
    if pipeline_activity_path.exists() and pipeline_activity_path.stat().st_size:
        with pipeline_activity_path.open("r", newline="", encoding="utf-8") as f:
            pipeline_activity_rows = list(csv.DictReader(f))
        if pipeline_activity_rows:
            lines.append("Vulkan shader/pipeline/cache activity")
            operation_groups: dict[str, list[float]] = {}
            caller_counts: dict[str, int] = {}
            unique_shader_hashes: set[str] = set()
            no_cache_pipelines = 0
            cached_pipelines = 0
            cache_creates = 0
            cache_creates_with_initial_data = 0
            cache_gets = 0
            cache_merges = 0
            for row in pipeline_activity_rows:
                op = row.get("operation", "")
                try:
                    duration = float(row.get("duration_ms", "") or 0.0)
                except ValueError:
                    duration = 0.0
                operation_groups.setdefault(op, []).append(duration)
                caller = row.get("caller_module", "") or "unknown"
                caller_counts[caller] = caller_counts.get(caller, 0) + 1
                code_hash = row.get("code_hash", "")
                if code_hash:
                    unique_shader_hashes.add(code_hash)
                if op in ("vk.create_compute_pipelines", "vk.create_graphics_pipelines"):
                    if row.get("pipeline_cache", ""):
                        cached_pipelines += int(row.get("create_count", "0") or 0)
                    else:
                        no_cache_pipelines += int(row.get("create_count", "0") or 0)
                elif op == "vk.create_pipeline_cache":
                    cache_creates += 1
                    if int(row.get("flags", "0") or 0) & 1:
                        cache_creates_with_initial_data += 1
                elif op == "vk.get_pipeline_cache_data":
                    cache_gets += 1
                elif op == "vk.merge_pipeline_caches":
                    cache_merges += 1
            lines.append(
                f"  events={len(pipeline_activity_rows)} unique_shader_hashes={len(unique_shader_hashes)} "
                f"pipeline_creates_without_cache={no_cache_pipelines} with_cache={cached_pipelines}")
            lines.append(
                f"  pipeline_cache create={cache_creates} with_initial_data={cache_creates_with_initial_data} "
                f"get_data={cache_gets} merge={cache_merges}")
            for op, values in sorted(operation_groups.items(), key=lambda item: sum(item[1]), reverse=True)[:12]:
                stats = distribution_stats(values)
                lines.append(
                    f"  {op:30s} n={len(values):4d} total={sum(values):9.3f} ms "
                    f"mean={stats['mean']:8.3f} p95={stats['p95']:8.3f} max={stats['max']:8.3f}")
            if caller_counts:
                ranked_callers = sorted(caller_counts.items(), key=lambda item: item[1], reverse=True)[:8]
                lines.append("  caller modules: " + ", ".join(f"{name}={count}" for name, count in ranked_callers))
            lines.append("")

    reflex_conditioned_path = output / "state_conditioned_stats.csv"
    if reflex_conditioned_path.exists() and reflex_conditioned_path.stat().st_size:
        with reflex_conditioned_path.open("r", newline="", encoding="utf-8") as f:
            reflex_conditioned_rows = [
                row for row in csv.DictReader(f)
                if row.get("state_name") == "streamline.reflex.mode" and row.get("state_value") == "1"
                and row.get("family") in ("cpu", "wait")
                and (row.get("name", "").startswith("antilag.") or row.get("name", "").startswith("reflex."))
            ]
        if reflex_conditioned_rows:
            lines.append("Reflex / Anti-Lag conditioned CPU wall time (mode=on)")
            for row in sorted(reflex_conditioned_rows, key=lambda item: float(item.get("mean", "0") or 0), reverse=True)[:16]:
                lines.append(
                    f"  {row.get('backend',''):7s} {row.get('name',''):34s} "
                    f"n={int(float(row.get('samples','0') or 0)):4d} mean={float(row.get('mean','0') or 0):8.3f} ms "
                    f"p95={float(row.get('p95','0') or 0):8.3f} p99={float(row.get('p99','0') or 0):8.3f} "
                    f"max={float(row.get('max','0') or 0):8.3f}")
            lines.append("  Note: driver-call wall time can include intentional pacing/blocking; it is not pure CPU compute time.")
            lines.append("")

    switches = [r for r in records if r["type"] == TYPE_INSTANT and r["name"] == "backend.switch"]
    if switches:
        lines.append(f"backend switches observed: {len(switches)}")
        for r in switches[-12:]:
            lines.append(f"  pid={r.get('process_id', 0)} frame={r['frame_id']} {r['detail']}")
        lines.append("")

    failed_frames = [r for r in records if r["type"] == TYPE_FRAME and (r["flags"] & FRAME_FAILED)]
    if failed_frames:
        lines.append(f"sampled failed frames: {len(failed_frames)}")
        for r in failed_frames[-12:]:
            lines.append(
                f"  {r['backend']}/{r['component']} frame={r['frame_id']} stream={r['stream_id']} flags=0x{r['flags']:x}")
        lines.append("")

    decisions = [r for r in records if r["type"] == TYPE_DECISION]
    if decisions:
        lines.append(f"structured decisions: {len(decisions)}")
        decision_groups: dict[tuple[str, str, str, str], int] = {}
        for r in decisions:
            outcome = DECISION_OUTCOMES.get(r["aux0"], str(r["aux0"]))
            key = (r["backend"], r["component"], r["name"], f"{outcome}:{r['detail']}")
            decision_groups[key] = decision_groups.get(key, 0) + 1
        ranked_decisions = sorted(decision_groups.items(), key=lambda item: item[1], reverse=True)[:20]
        for (backend, component, name, outcome_reason), count in ranked_decisions:
            lines.append(f"  {backend:7s} {component:16s} n={count:4d} {name}: {outcome_reason}")
        lines.append("")

    coverage_path = output / "stream_coverage.csv"
    if coverage_path.exists() and coverage_path.stat().st_size:
        with coverage_path.open("r", newline="", encoding="utf-8") as f:
            coverage_rows = list(csv.DictReader(f))
        semantic_fields = ("decision_records", "state_transition_records", "resource_event_records",
                           "flow_event_records", "wait_interval_records", "thread_info_records")
        semantically_instrumented = [
            row for row in coverage_rows
            if any(int(row.get(field, "0") or 0) > 0 for field in semantic_fields)
        ]
        manifests = [row for row in coverage_rows if row.get("capability_manifest_present", "False") == "True"]
        lines.append(
            f"semantic stream coverage: {len(semantically_instrumented)}/{len(coverage_rows)} retained stream(s) "
            "emit decision/state/resource/flow/wait/thread records")
        lines.append(
            f"stream capability manifests: {len(manifests)}/{len(coverage_rows)} retained stream(s) declare semantic coverage")
        for row in semantically_instrumented[:12]:
            present = [field.removesuffix("_records") for field in semantic_fields
                       if int(row.get(field, "0") or 0) > 0]
            lines.append(
                f"  {row.get('backend',''):7s} {row.get('component',''):16s} "
                f"{row.get('stream_name','')}: {','.join(present)}")
        lines.append("")

    states = [r for r in records if r["type"] == TYPE_STATE_TRANSITION]
    if states:
        last_states: dict[tuple[str, int, str, int], dict] = {}
        for r in states:
            last_states[(r["source"], r["stream_id"], r["name"], r["values"][2])] = r
        lines.append(f"state transitions: {len(states)}; observed terminal states={len(last_states)}")
        for r in sorted(last_states.values(), key=lambda item: item["cpu_tick"], reverse=True)[:12]:
            lines.append(
                f"  {r['backend']:7s} {r['component']:16s} {r['name']} "
                f"subject={r['values'][2]} value={r['values'][1]} reason={r['detail']}")
        lines.append("")

    resource_balance_path = output / "resource_balance.csv"
    if resource_balance_path.exists() and resource_balance_path.stat().st_size:
        with resource_balance_path.open("r", newline="", encoding="utf-8") as f:
            resource_balance_rows = list(csv.DictReader(f))
        suspicious = [
            row for row in resource_balance_rows
            if int(row.get("outstanding_count", 0) or 0) != 0 or
               int(row.get("unmatched_close_count", 0) or 0) != 0
        ]
        lines.append(
            f"resource lifetime ledgers: {len(resource_balance_rows)}; "
            f"with outstanding/unmatched objects={len(suspicious)}")
        for row in suspicious[:12]:
            lines.append(
                f"  {row['backend']:7s} {row['component']:16s} {row['name']} "
                f"outstanding={row['outstanding_count']} ({int(row['outstanding_bytes']) / (1024**2):.2f} MiB) "
                f"unmatched_close={row['unmatched_close_count']} "
                f"indeterminate_pre_window_close={row.get('indeterminate_close_count', '0')}")
        lines.append("")

    flows = [r for r in records if r["type"] == TYPE_FLOW_EVENT]
    if flows:
        flow_groups: dict[tuple[int, str, int, str], list[dict]] = {}
        for r in flows:
            flow_groups.setdefault((r.get("process_id", 0), r.get("session", ""), r["correlation_id"], r["name"]), []).append(r)
        incomplete = 0
        cancelled = 0
        for rows in flow_groups.values():
            phases = {FLOW_PHASES.get(r["aux0"], str(r["aux0"])) for r in rows}
            if "begin" not in phases or not ({"end", "cancel"} & phases):
                incomplete += 1
            if "cancel" in phases:
                cancelled += 1
        lines.append(
            f"logical flows: {len(flow_groups)}; incomplete={incomplete} cancelled={cancelled}")
        lines.append("")

    thread_infos = [r for r in records if r["type"] == TYPE_THREAD_INFO]
    if thread_infos:
        unique_threads = {(r.get("process_id", 0), r["thread_id"]) for r in thread_infos}
        lines.append(f"named profiler threads: {len(unique_threads)}")
        for r in thread_infos[:16]:
            lines.append(
                f"  tid={r['thread_id']} {r['name']} role={r['detail']} priority={signed_u64(r['values'][2])}")
        lines.append("")

    explicit_waits = [r for r in records if r["type"] == TYPE_WAIT_INTERVAL]
    if explicit_waits:
        wait_groups: dict[tuple[str, str, str], list[float]] = {}
        wait_outcomes: dict[tuple[str, str, str], int] = {}
        for r in explicit_waits:
            key = (r["name"], WAIT_KINDS.get(r["aux0"], str(r["aux0"])),
                   WAIT_RESULTS.get(r["aux1"], str(r["aux1"])))
            wait_groups.setdefault(key, []).append(r["values"][2] / 1e6)
            wait_outcomes[key] = wait_outcomes.get(key, 0) + 1
        ranked_waits = sorted(wait_groups.items(), key=lambda item: sum(item[1]), reverse=True)[:16]
        lines.append(f"explicit waits: {len(explicit_waits)}")
        for (name, kind, result), values in ranked_waits:
            st = distribution_stats(values)
            lines.append(
                f"  n={len(values):4d} total={sum(values):8.3f} ms p50={st['median']:7.3f} "
                f"p95={st['p95']:7.3f} {kind}/{result} {name}")
        lines.append("")

    anomaly_path = output / "anomalies.csv"
    if anomaly_path.exists() and anomaly_path.stat().st_size:
        with anomaly_path.open("r", newline="", encoding="utf-8") as f:
            anomaly_rows = list(csv.DictReader(f))
        errors = sum(row.get("severity") == "error" for row in anomaly_rows)
        warnings = sum(row.get("severity") == "warning" for row in anomaly_rows)
        lines.append(f"automatic anomaly index: {len(anomaly_rows)} rows ({errors} errors, {warnings} warnings)")
        for row in anomaly_rows[:12]:
            lines.append(
                f"  {row['severity']:7s} {row['category']:9s} frame={row['frame_id']} {row['name']}: {row['detail']}")
        lines.append("")

    pass_by = {}
    for r in records:
        if r["type"] == TYPE_GPU_INTERVAL and r["values"][0] > 0 and gpu_duration_status(r)[0]:
            pass_by.setdefault((r["backend"], r["component"], r["name"]), []).append(r["values"][0] / 1e6)
    lines.append("Top GPU intervals by mean time")
    if not pass_by:
        lines.append("  none retained/captured")
    ranked = sorted(pass_by.items(), key=lambda kv: statistics.fmean(kv[1]), reverse=True)[:40]
    for (backend, component, name), values in ranked:
        stats = distribution_stats(values)
        lines.append(
            f"  {backend:7s} {component:16s} mean={stats['mean']:8.3f} ms "
            f"p95={stats['p95']:8.3f} p99={stats['p99']:8.3f} n={len(values):4d}  {name}")
    lines.append("")

    cpu_by = {}
    for r in records:
        if r["type"] == TYPE_CPU_INTERVAL and r["values"][2] > 0:
            cpu_by.setdefault((r["backend"], r["component"], r["name"]), []).append(r["values"][2] / 1e6)
    lines.append("Top CPU intervals by mean time")
    if not cpu_by:
        lines.append("  none retained/captured")
    ranked_cpu = sorted(cpu_by.items(), key=lambda kv: statistics.fmean(kv[1]), reverse=True)[:40]
    for (backend, component, name), values in ranked_cpu:
        stats = distribution_stats(values)
        lines.append(
            f"  {backend:7s} {component:16s} mean={stats['mean']:8.3f} ms "
            f"p95={stats['p95']:8.3f} p99={stats['p99']:8.3f} n={len(values):4d}  {name}")
    lines.append("")

    exclusive_path = output / "cpu_exclusive_stats.csv"
    if exclusive_path.exists() and exclusive_path.stat().st_size:
        with exclusive_path.open("r", newline="", encoding="utf-8") as f:
            exclusive_rows = list(csv.DictReader(f))
        exclusive_rows.sort(key=lambda row: float(row.get("exclusive_mean_ms", 0.0) or 0.0), reverse=True)
        if exclusive_rows:
            lines.append("Top CPU scopes by exclusive/self time")
            for row in exclusive_rows[:20]:
                lines.append(
                    f"  {row['backend']:7s} {row['component']:16s} "
                    f"self={float(row['exclusive_mean_ms']):8.3f} ms "
                    f"inclusive={float(row['inclusive_mean_ms']):8.3f} ms "
                    f"self-share={float(row['exclusive_share_percent_mean']):5.1f}% "
                    f"n={int(row['samples']):4d}  {row['name']}")
            lines.append("")

    concurrency_path = output / "cpu_concurrency_stats.csv"
    if concurrency_path.exists() and concurrency_path.stat().st_size:
        with concurrency_path.open("r", newline="", encoding="utf-8") as f:
            concurrency_rows = list(csv.DictReader(f))
        concurrency_rows.sort(
            key=lambda row: float(row.get("parallelism_mean", 0.0) or 0.0), reverse=True)
        interesting = [
            row for row in concurrency_rows
            if float(row.get("parallelism_mean", 0.0) or 0.0) > 1.02 or
               float(row.get("peak_threads_max", 0.0) or 0.0) > 1.0
        ]
        if interesting:
            lines.append("Profiled CPU concurrency (captured APUSR scopes, not process CPU utilization)")
            for row in interesting[:12]:
                lines.append(
                    f"  {row['backend']:7s} {row['component']:16s} "
                    f"parallel(mean)={float(row['parallelism_mean']):5.2f}x "
                    f"peak={float(row['peak_threads_max']):4.0f} threads "
                    f"active-coverage={float(row['active_coverage_mean_percent']):5.1f}% "
                    f"span-p95={float(row['span_p95_ms']):8.3f} ms n={int(row['samples']):4d}  "
                    f"{row['stream_name']}")
            lines.append("")

    stutter_path = output / "stutter_frames.csv"
    if stutter_path.exists() and stutter_path.stat().st_size:
        with stutter_path.open("r", newline="", encoding="utf-8") as f:
            stutter_rows = list(csv.DictReader(f))
        if stutter_rows:
            lines.append("Tail-frame forensics (context/correlation, not automatic causation)")
            for row in stutter_rows[:12]:
                pass_text = f" gpu={row['top_gpu_pass']} {float(row['top_gpu_pass_ms']):.3f}ms" if row.get("top_gpu_pass") else ""
                cpu_text = f" cpu={row['top_cpu_self_scope']} self {float(row['top_cpu_self_ms']):.3f}ms" if row.get("top_cpu_self_scope") else ""
                wait_text = f" wait={row['top_wait']} {float(row['top_wait_ms']):.3f}ms" if row.get("top_wait") else ""
                error_text = f" errors={row['error_count']}" if int(row.get("error_count", 0) or 0) else ""
                display_interval = max(
                    float(row.get("display_generated_interval_ms", 0.0) or 0.0),
                    float(row.get("display_source_interval_ms", 0.0) or 0.0))
                display_sync = max(
                    float(row.get("display_generated_present_begin_to_sync_ms", 0.0) or 0.0),
                    float(row.get("display_source_present_begin_to_sync_ms", 0.0) or 0.0))
                if display_interval > 0.0:
                    display_text = f" display_interval={display_interval:.3f}ms"
                elif display_sync > 0.0:
                    display_text = f" present_to_sync={display_sync:.3f}ms"
                else:
                    display_text = ""
                memory_pressure = float(row.get("memory_local_pressure_pct", 0.0) or 0.0)
                memory_text = f" local_mem={memory_pressure:.1f}%" if memory_pressure > 0.0 else ""
                lines.append(
                    f"  {row['backend']:7s} {row['component']:16s} frame={row['frame_id']} tail={row['tail_kind']} "
                    f"gpu={float(row['gpu_ms']):.3f}ms cpu={float(row['cpu_total_ms']):.3f}ms"
                    f"{pass_text}{cpu_text}{wait_text}{display_text}{memory_text}{error_text}")
            lines.append("")

    tail_signals = []
    for key, values in pass_by.items():
        if len(values) < 5:
            continue
        stats = distribution_stats(values)
        ratio = stats["p99"] / stats["median"] if stats["median"] else 0.0
        if ratio >= 1.25:
            tail_signals.append((ratio, key, stats, len(values)))
    if tail_signals:
        lines.append("GPU tail-latency signals (p99/p50 >= 1.25x)")
        for ratio, (backend, component, name), stats, count in sorted(tail_signals, reverse=True)[:12]:
            lines.append(
                f"  {backend:7s} {component:16s} p50={stats['median']:8.3f} ms "
                f"p99={stats['p99']:8.3f} ms ratio={ratio:5.2f}x n={count:4d}  {name}")
        lines.append("")

    setup_signals = []
    setup_groups: dict[tuple[str, str, str], list[tuple[float, float]]] = {}
    for r in records:
        if r["type"] == TYPE_GPU_INTERVAL and r["values"][0] > 0:
            setup_groups.setdefault((r["backend"], r["component"], r["name"]), []).append(
                (r["values"][0] / 1e6, r["values"][1] / 1e6))
    for key, samples in setup_groups.items():
        if len(samples) < 3:
            continue
        total_mean = statistics.fmean(v[0] for v in samples)
        setup_mean = statistics.fmean(v[1] for v in samples)
        share = setup_mean / total_mean if total_mean else 0.0
        if share >= .15:
            setup_signals.append((share, key, total_mean, setup_mean, len(samples)))
    if setup_signals:
        lines.append("Setup-heavy GPU intervals (setup >= 15% of measured interval)")
        for share, (backend, component, name), total_mean, setup_mean, count in sorted(setup_signals, reverse=True)[:12]:
            lines.append(
                f"  {backend:7s} {component:16s} setup={setup_mean:8.3f} ms "
                f"total={total_mean:8.3f} ms share={share * 100.0:5.1f}% n={count:4d}  {name}")
        lines.append("")

    wait_groups = {}
    for r in records:
        if r["type"] != TYPE_CPU_INTERVAL or r["values"][2] == 0:
            continue
        lower = r["name"].lower()
        if "wait" not in lower and "sync" not in lower and "acquire" not in lower:
            continue
        wait_groups.setdefault((r["backend"], r["component"], r["name"]), []).append(r["values"][2] / 1e6)
    if wait_groups:
        lines.append("CPU wait/synchronization intervals")
        ranked_waits = sorted(wait_groups.items(), key=lambda kv: percentile(kv[1], .95), reverse=True)[:20]
        for (backend, component, name), values in ranked_waits:
            stats = distribution_stats(values)
            lines.append(
                f"  {backend:7s} {component:16s} mean={stats['mean']:8.3f} ms "
                f"p95={stats['p95']:8.3f} p99={stats['p99']:8.3f} n={len(values):4d}  {name}")
        lines.append("")

    wsi_cpu: dict[tuple[int, int, str], dict[str, list]] = {}
    summary_shard_by_source = {str(shard.path): shard for shard in shards}
    stream_names = {
        (r["source"], r["stream_id"]): r["name"] for r in records if r["type"] == TYPE_STREAM
    }
    for r in records:
        if stream_names.get((r["source"], r["stream_id"])) != "vulkan.wsi":
            continue
        key = (r.get("process_id", 0), r["stream_id"], r.get("module", ""))
        group = wsi_cpu.setdefault(
            key, {"acquire": [], "present": [], "cadence": [], "present_events": []})
        if r["type"] == TYPE_CPU_INTERVAL and r["name"] == "wsi.acquire_call":
            group["acquire"].append(r["values"][2] / 1e6)
        elif r["type"] == TYPE_CPU_INTERVAL and r["name"] == "wsi.present_call":
            group["present"].append(r["values"][2] / 1e6)
        elif r["type"] == TYPE_COUNTER and r["name"] == "wsi.present_interval_ns":
            group["cadence"].append(r["values"][0] / 1e6)
        elif r["type"] == TYPE_QUEUE_EVENT and r["name"] == "wsi.present":
            group["present_events"].append(
                (r["frame_id"], r["cpu_tick"], r["source"]))
    if wsi_cpu:
        lines.append("Application-facing Vulkan WSI")
        for (process_id, stream_id, module), group in sorted(wsi_cpu.items()):
            acquire = distribution_stats(group["acquire"])
            present = distribution_stats(group["present"])
            # Sampled WSI profiling intentionally does not keep global
            # previous-present state on every call. Recover cadence from
            # consecutive sampled present serials so a burst still reveals the
            # application's actual source-frame rate.
            if not group["cadence"] and group["present_events"]:
                events = sorted(group["present_events"], key=lambda item: item[1])
                for previous, current in zip(events, events[1:]):
                    if current[0] != previous[0] + 1:
                        continue
                    shard = summary_shard_by_source.get(current[2])
                    if shard and shard.cpu_frequency and current[1] > previous[1]:
                        group["cadence"].append(
                            (current[1] - previous[1]) * 1000.0 / shard.cpu_frequency)
            cadence = distribution_stats(group["cadence"])
            lines.append(
                f"  pid={process_id} stream={stream_id} module={module or '-'} "
                f"acquire mean/p95={acquire['mean']:.3f}/{acquire['p95']:.3f} ms "
                f"present mean/p95={present['mean']:.3f}/{present['p95']:.3f} ms "
                f"present cadence p50/p95={cadence['median']:.3f}/{cadence['p95']:.3f} ms")
        lines.append("")

    generated_attempts = [
        r for r in records if r["type"] == TYPE_COUNTER and r["name"] == "presentation.generated_attempted"
    ]
    generation_not_ready = [
        r for r in records if r["type"] == TYPE_COUNTER and r["name"] == "presentation.generation_not_ready"
    ]
    generation_watchdog_timeout = [
        r for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.generation_watchdog_timeout"
    ]
    generation_deadline_miss = [
        r for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.generation_deadline_miss"
    ]
    generated_late = [
        r["values"][0] / 1e6 for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.generated_target_late_ns"
    ]
    source_late = [
        r["values"][0] / 1e6 for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.source_target_late_ns"
    ]
    source_interval_estimates = [
        r["values"][0] / 1e6 for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.source_interval_ns"
    ]
    generated_visible_targets = [
        r["values"][0] / 1e6 for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.generated_visible_ns"
    ]
    paced_present_intervals = [
        r["values"][0] / 1e6 for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "presentation.paced_present_interval_ns"
    ]
    blocking_fence = [
        r["values"][0] for r in records
        if r["type"] == TYPE_COUNTER and r["name"] == "interpolation.blocking_fence_wait"
    ]
    if (generated_attempts or generation_not_ready or generation_watchdog_timeout or
            generation_deadline_miss or source_interval_estimates or generated_visible_targets or
            paced_present_intervals or generated_late or source_late or blocking_fence):
        lines.append("Frame-generation pacing outcomes")
        if generated_attempts:
            attempted = sum(bool(r["values"][0]) for r in generated_attempts)
            presented = sum(bool(r["values"][1]) for r in generated_attempts)
            success = 100.0 * presented / attempted if attempted else 0.0
            lines.append(
                f"  Vulkan generated presentation: attempted={attempted} presented={presented} "
                f"accepted/attempted={success:.1f}% sampled decisions={len(generated_attempts)}")
        if generation_not_ready:
            lines.append(f"  Vulkan generation-not-ready opportunities: {len(generation_not_ready)}")
        if generation_watchdog_timeout:
            lines.append(f"  Vulkan generation watchdog timeouts: {len(generation_watchdog_timeout)}")
        if generation_deadline_miss:
            lines.append(f"  Vulkan generation deadline misses: {len(generation_deadline_miss)}")
        if source_interval_estimates:
            source_stats = distribution_stats(source_interval_estimates)
            line = (
                f"  Vulkan source cadence estimate: p50={source_stats['median']:.3f} ms "
                f"p95={source_stats['p95']:.3f} ms")
            if generated_visible_targets:
                target_stats = distribution_stats(generated_visible_targets)
                line += (
                    f"; generated target p50={target_stats['median']:.3f} ms "
                    f"p95={target_stats['p95']:.3f} ms")
            lines.append(line)
        if paced_present_intervals:
            paced_stats = distribution_stats(paced_present_intervals)
            lines.append(
                f"  Vulkan paced physical interval: p50={paced_stats['median']:.3f} ms "
                f"p95={paced_stats['p95']:.3f} ms")
        if generated_late:
            stats = distribution_stats(generated_late)
            lines.append(
                f"  Vulkan generated-target lateness: mean={stats['mean']:.3f} ms "
                f"p95={stats['p95']:.3f} ms p99={stats['p99']:.3f} ms")
        if source_late:
            stats = distribution_stats(source_late)
            lines.append(
                f"  Vulkan source-target lateness: mean={stats['mean']:.3f} ms "
                f"p95={stats['p95']:.3f} ms p99={stats['p99']:.3f} ms")
        if blocking_fence:
            lines.append(
                f"  D3D12 interpolation blocking-fence choice: "
                f"{100.0 * sum(bool(v) for v in blocking_fence) / len(blocking_fence):.1f}% "
                f"of {len(blocking_fence)} sampled interpolation(s)")
        lines.append("")

    display_latency_groups: dict[tuple[str, str], list[float]] = {}
    display_return_groups: dict[tuple[str, str], list[float]] = {}
    for r in records:
        if r["type"] != TYPE_COUNTER or r["name"] != "display.present_to_sync_ns":
            continue
        kind = "generated" if (r["flags"] & 1) else "real/source"
        display_latency_groups.setdefault((r["backend"], kind), []).append(r["values"][0] / 1e6)
        if r["values"][1]:
            display_return_groups.setdefault((r["backend"], kind), []).append(r["values"][1] / 1e6)
    if display_latency_groups:
        lines.append("Presentation-to-display synchronization observations")
        for key, values in sorted(display_latency_groups.items()):
            stats = distribution_stats(values)
            return_stats = distribution_stats(display_return_groups.get(key, []))
            lines.append(
                f"  {key[0]:7s} {key[1]:11s} present-call-begin->sync mean/p95="
                f"{stats['mean']:.3f}/{stats['p95']:.3f} ms; "
                f"Present-return->sync mean/p95={return_stats['mean']:.3f}/{return_stats['p95']:.3f} ms "
                f"n={len(values)}")
        lines.append("")

    vk_display_by_stream: dict[tuple[str, int], list[dict]] = {}
    for r in records:
        if r["type"] != TYPE_COUNTER or r["name"] not in (
                "display.vk_actual_present_ns", "display.vk_present_margin_ns"):
            continue
        vk_display_by_stream.setdefault((r["source"], r["stream_id"]), []).append(r)
    if vk_display_by_stream:
        lines.append("Vulkan actual-display cadence (VK_GOOGLE_display_timing)")
        for (_source, _stream_id), rows in sorted(vk_display_by_stream.items()):
            actual_records = sorted(
                (r for r in rows if r["name"] == "display.vk_actual_present_ns" and r["values"][0]),
                key=lambda r: r["values"][0])
            margins = [r["values"][0] / 1e6 for r in rows if r["name"] == "display.vk_present_margin_ns"]
            lateness = [
                (r["values"][0] - r["values"][1]) / 1e6
                for r in actual_records if r["values"][1] and r["values"][0] >= r["values"][1]
            ]
            intervals = [
                (b["values"][0] - a["values"][0]) / 1e6
                for a, b in zip(actual_records, actual_records[1:])
                if b["values"][0] >= a["values"][0]
            ]
            if not actual_records:
                continue
            stream = next((r for r in records if r["type"] == TYPE_STREAM and
                           r["source"] == _source and r["stream_id"] == _stream_id), None)
            label = stream["name"] if stream else f"stream {_stream_id}"
            interval_stats = distribution_stats(intervals)
            margin_stats = distribution_stats(margins)
            late_stats = distribution_stats(lateness)
            lines.append(
                f"  {label}: displayed={len(actual_records)} interval mean/p95/p99="
                f"{interval_stats['mean']:.3f}/{interval_stats['p95']:.3f}/{interval_stats['p99']:.3f} ms; "
                f"margin mean/p5~={margin_stats['mean']:.3f}/{percentile(margins, .05):.3f} ms; "
                f"actual-earliest mean/p95={late_stats['mean']:.3f}/{late_stats['p95']:.3f} ms")
        lines.append("  Raw Vulkan display timestamps share the presentation engine's monotonic-ns domain; "
                     "cadence/margin are reported without pretending they are calibrated to APUSR's CPU clock.")
        lines.append("")

    memory_budget_groups: dict[tuple[str, str], list[tuple[float, int, int]]] = {}
    for r in records:
        if r["type"] != TYPE_COUNTER or r["name"] not in ("memory.budget.local", "memory.budget.nonlocal"):
            continue
        usage = r["values"][0]
        budget = r["values"][1]
        pressure = 100.0 * usage / budget if budget else 0.0
        memory_budget_groups.setdefault((r["backend"], r["name"].rsplit(".", 1)[-1]), []).append(
            (pressure, usage, budget))
    if memory_budget_groups:
        lines.append("OS/driver memory budget snapshots")
        for (backend, segment), values in sorted(memory_budget_groups.items()):
            pressures = [v[0] for v in values]
            latest_pressure, latest_usage, latest_budget = values[-1]
            lines.append(
                f"  {backend:7s} {segment:8s} latest={latest_usage / (1024**2):8.1f}/"
                f"{latest_budget / (1024**2):8.1f} MiB ({latest_pressure:5.1f}%) "
                f"peak={max(pressures):5.1f}% snapshots={len(values)}")
        lines.append("")

    queue_events = [r for r in records if r["type"] == TYPE_QUEUE_EVENT]
    if queue_events:
        dependencies = reconstruct_queue_dependencies(shards, records)
        matched = [edge for edge in dependencies if edge["signal"] is not None]
        cross_queue = [edge for edge in matched if edge["cross_queue"]]
        lines.append(
            f"queue ordering events: {len(queue_events)}; synchronization waits={len(dependencies)} "
            f"matched={len(matched)} cross-queue={len(cross_queue)}")
        unmatched = len(dependencies) - len(matched)
        if unmatched:
            lines.append(
                f"  unmatched waits: {unmatched} (the satisfying signal may be outside the sampled/retained window)")
        gap_groups: dict[tuple[str, str, str], list[float]] = {}
        for edge in cross_queue:
            wait = edge["wait"]
            gap_groups.setdefault(
                (wait["backend"], wait["component"], wait["name"]), []).append(edge["enqueue_gap_us"])
        if gap_groups:
            lines.append("  largest cross-queue CPU enqueue gaps (causal spacing, not GPU stall duration)")
            ranked = sorted(
                gap_groups.items(), key=lambda kv: percentile(kv[1], .95), reverse=True)[:12]
            for (backend, component, name), values in ranked:
                stats = distribution_stats(values)
                lines.append(
                    f"    {backend:7s} {component:16s} mean={stats['mean']:9.1f} us "
                    f"p95={stats['p95']:9.1f} us n={len(values):4d}  {name}")
        lines.append("")

    gpu_gaps = observed_gpu_queue_gaps(shards, records)
    if gpu_gaps:
        wait_gaps = [gap for gap in gpu_gaps if gap["wait_associated"]]
        lines.append(
            f"observed calibrated GPU queue gaps: {len(gpu_gaps)}; wait-associated={len(wait_gaps)} "
            "(unprofiled spans, not asserted hardware-idle time)")
        ranked = sorted(gpu_gaps, key=lambda gap: gap["gap_us"], reverse=True)[:12]
        for gap in ranked:
            wait_context = f" wait={gap['wait_name']}" if gap["wait_associated"] else ""
            cross = " cross-queue" if gap["cross_queue_dependency"] else ""
            lines.append(
                f"  {gap['backend']}/{gap['component']} q=0x{gap['native_queue']:x} "
                f"{gap['previous_name']} -> {gap['next_name']} "
                f"gap={gap['gap_us']:.1f} us{wait_context}{cross}")
        lines.append("")

    calibrations = [r for r in records if r["type"] == TYPE_CALIBRATION]
    lines.append(f"GPU/CPU clock calibrations: {len(calibrations)}")
    calibration_by_backend: dict[str, list[float]] = {}
    for r in calibrations:
        calibration_by_backend.setdefault(r["backend"], []).append(float(r["values"][4]))
    for backend, deviations in sorted(calibration_by_backend.items()):
        stats = distribution_stats(deviations)
        lines.append(
            f"  {backend:7s} max-deviation mean={stats['mean'] / 1000.0:8.3f} us "
            f"p95={stats['p95'] / 1000.0:8.3f} us max={stats['max'] / 1000.0:8.3f} us")
    calibrated_samples = {
        (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue_override(r))
        for r in calibrations
    }
    timestamped_samples = {
        (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue_override(r))
        for r in records
        if (r["type"] == TYPE_FRAME and r["extra_values"][0] != 0 and r["extra_values"][2] != 0)
        or (r["type"] == TYPE_GPU_INTERVAL and r["extra_values"][0] != 0 and r["extra_values"][3] != 0)
    }
    if timestamped_samples:
        calibrated_count = 0
        for key in timestamped_samples:
            source, stream_id, frame_id, sample_sequence, native_queue = key
            if key in calibrated_samples or (
                    native_queue and (source, stream_id, frame_id, sample_sequence, 0) in calibrated_samples):
                calibrated_count += 1
        lines.append(
            f"  absolute timeline coverage: {calibrated_count}/{len(timestamped_samples)} "
            f"timestamped sample(s) calibrated ({100.0 * calibrated_count / len(timestamped_samples):.1f}%)")
    lines.append("")

    errors = [r for r in records if r["type"] == TYPE_ERROR]
    lines.append(f"errors: {len(errors)}")
    for r in errors[-20:]:
        native = (f" native={signed_u64(r['values'][1])} (0x{r['values'][1]:x})"
                  if r["values"][1] else "")
        context = f" context={r['values'][2]}" if r["values"][2] else ""
        lines.append(
            f"  {r['backend']}/{r['component']} frame={r['frame_id']} {r['name']}"
            f" status={r['values'][0]}{native}{context}: {r['detail']}")

    text = "\n".join(lines) + "\n"
    (output / "summary.txt").write_text(text, encoding="utf-8")
    return text


def write_chrome_trace(shards: list[Shard], records: list[dict], output: Path) -> None:
    # CPU intervals use the native monotonic/QPC clock. Native GPU timestamps
    # are mapped onto the profiler CPU clock whenever
    # a backend supplied a matching calibration pair. D3D12 uses queue clock
    # calibration; Vulkan uses KHR/EXT calibrated timestamps when the game
    # enabled the extension. Uncalibrated events stay explicitly anchored at
    # resolution time rather than masquerading as absolute timestamps.
    shard_by_source = {str(s.path): s for s in shards}
    streams = {(r["source"], r["stream_id"]): r for r in records if r["type"] == TYPE_STREAM}
    calibrations = {(r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue_override(r)): r
                    for r in records if r["type"] == TYPE_CALIBRATION}
    events = []

    stream_lane = {key: 0x70000000 + index * 2 for index, key in enumerate(sorted(streams))}
    gpu_queue_keys = sorted({
        (r["source"], r["stream_id"], native_queue_override(r))
        for r in records
        if r["type"] == TYPE_GPU_INTERVAL and native_queue_override(r) != 0
    })
    gpu_queue_lane = {key: 0x72000000 + index for index, key in enumerate(gpu_queue_keys)}
    queue_records = [r for r in records if r["type"] == TYPE_QUEUE_EVENT and r["values"][0] != 0]
    queue_keys = sorted({(r.get("process_id", 0), r["values"][0]) for r in queue_records})
    queue_lane = {key: 0x74000000 + index for index, key in enumerate(queue_keys)}

    def lane_id(r: dict, frame_lane: bool = False) -> int:
        base = stream_lane.get((r["source"], r["stream_id"]), 0x7F000000)
        return base + (1 if frame_lane else 0)

    def gpu_lane_id(r: dict) -> int:
        native_queue = native_queue_override(r)
        if native_queue:
            return gpu_queue_lane.get((r["source"], r["stream_id"], native_queue), lane_id(r))
        return lane_id(r)

    # Name GPU lanes in trace viewers.
    for key, stream in streams.items():
        shard = shard_by_source.get(stream["source"])
        if not shard:
            continue
        tid = lane_id(stream)
        lane_name = f"GPU {stream['backend']} {stream['component']} {stream['name']}"
        events.append({"name": "thread_name", "ph": "M", "pid": shard.process_id, "tid": tid,
                       "args": {"name": lane_name}})
        events.append({"name": "thread_name", "ph": "M", "pid": shard.process_id, "tid": lane_id(stream, True),
                       "args": {"name": lane_name + " frames"}})

    for (source, stream_id, native_queue), tid in gpu_queue_lane.items():
        stream = streams.get((source, stream_id))
        shard = shard_by_source.get(source)
        if not stream or not shard:
            continue
        events.append({
            "name": "thread_name", "ph": "M", "pid": shard.process_id, "tid": tid,
            "args": {"name": f"GPU {stream['backend']} {stream['component']} {stream['name']} queue 0x{native_queue:x}"},
        })

    # Queue lanes show CPU-side enqueue order. Flow arrows below connect a
    # signal enqueue to a matching wait enqueue; neither is presented as GPU
    # execution/stall duration.
    first_queue_record: dict[tuple[int, int], dict] = {}
    for r in queue_records:
        first_queue_record.setdefault((r.get("process_id", 0), r["values"][0]), r)
    for key, tid in queue_lane.items():
        pid, queue = key
        r = first_queue_record[key]
        events.append({
            "name": "thread_name", "ph": "M", "pid": pid, "tid": tid,
            "args": {"name": f"Queue {r['backend']} 0x{queue:x}"},
        })

    def calibrated_gpu_start(r: dict, raw_gpu_tick: int, gpu_frequency: int):
        native_queue = native_queue_override(r)
        key = (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], native_queue)
        calibration = calibrations.get(key)
        # Backward-compatible fallback for older v1 records with no queue tag.
        # Never borrow a calibration explicitly tagged to a different queue.
        if not calibration and native_queue:
            calibration = calibrations.get(
                (r["source"], r["stream_id"], r["frame_id"], r["correlation_id"], 0))
        if not calibration or raw_gpu_tick == 0 or gpu_frequency == 0:
            return None
        cpu_tick0 = calibration["values"][0]
        gpu_tick0 = calibration["values"][1]
        calibration_gpu_frequency = calibration["values"][2]
        cpu_frequency = calibration["values"][3]
        if cpu_frequency == 0 or calibration_gpu_frequency == 0:
            return None
        valid_bits = calibration["aux0"] or 64
        if 0 < valid_bits < 64:
            modulus = 1 << valid_bits
            mask = modulus - 1
            delta = ((raw_gpu_tick & mask) - (gpu_tick0 & mask)) & mask
            if delta >= modulus // 2:
                delta -= modulus
        else:
            delta = raw_gpu_tick - gpu_tick0
        frequency = calibration_gpu_frequency
        cpu_tick = cpu_tick0 + delta * cpu_frequency / frequency
        return cpu_tick * 1e6 / cpu_frequency

    for r in records:
        shard = shard_by_source.get(r["source"])
        if not shard or not shard.cpu_frequency:
            continue
        freq = shard.cpu_frequency
        ts_us = r["cpu_tick"] * 1e6 / freq
        pid = shard.process_id
        if r["type"] == TYPE_CPU_INTERVAL:
            start, end = r["values"][0], r["values"][1]
            events.append({"name": r["name"], "cat": f"cpu.{r['component']}", "ph": "X",
                           "ts": start * 1e6 / freq, "dur": r["values"][2] / 1e3,
                           "pid": pid, "tid": r["thread_id"],
                           "args": {"frame": r["frame_id"], "sample": r["correlation_id"], "flags": r["flags"]}})
        elif r["type"] == TYPE_FRAME and r["values"][4] > 0 and gpu_duration_status(r)[0]:
            raw_begin = r["extra_values"][0]
            gpu_frequency = r["extra_values"][2]
            calibrated_ts = calibrated_gpu_start(r, raw_begin, gpu_frequency)
            anchored = calibrated_ts is None
            events.append({"name": "frame", "cat": f"gpu.{r['backend']}.{r['component']}.frame", "ph": "X",
                           "ts": ts_us if anchored else calibrated_ts, "dur": r["values"][4] / 1e3,
                           "pid": pid, "tid": lane_id(r, True),
                           "args": {"frame": r["frame_id"], "sample": r["correlation_id"],
                                    "configuration_hash": f"0x{r['values'][0]:016x}",
                                    "native_queue": f"0x{native_queue_override(r):x}" if native_queue_override(r) else "",
                                    "calibrated": not anchored, "resolution_time_anchored": anchored}})
        elif r["type"] == TYPE_GPU_INTERVAL:
            raw_begin = r["extra_values"][0]
            gpu_frequency = r["extra_values"][3]
            calibrated_ts = calibrated_gpu_start(r, raw_begin, gpu_frequency)
            anchored = calibrated_ts is None
            gx, gy = unpack_pair(r["values"][5])
            gz, level = unpack_pair(r["values"][6])
            events.append({"name": r["name"], "cat": f"gpu.{r['backend']}.{r['component']}", "ph": "X",
                           "ts": ts_us if anchored else calibrated_ts, "dur": r["values"][0] / 1e3,
                           "pid": pid, "tid": gpu_lane_id(r),
                           "args": {"frame": r["frame_id"], "sample": r["correlation_id"],
                                    "setup_ms": r["values"][1] / 1e6, "execution_ms": r["values"][2] / 1e6,
                                    "groups": [gx, gy, gz], "profile_level": level,
                                    "native_queue": f"0x{native_queue_override(r):x}" if native_queue_override(r) else "",
                                    "calibrated": not anchored, "resolution_time_anchored": anchored}})
        elif r["type"] == TYPE_COUNTER:
            events.append({"name": r["name"], "cat": f"counter.{r['component']}", "ph": "C",
                           "ts": ts_us, "pid": pid, "tid": lane_id(r),
                           "args": {"value": r["values"][0], "value2": r["values"][1],
                                    "frame": r["frame_id"], "flags": r["flags"]}})
        elif r["type"] == TYPE_QUEUE_EVENT:
            native_queue = r["values"][0]
            tid = queue_lane.get((pid, native_queue), lane_id(r))
            events.append({
                "name": r["name"],
                "cat": f"queue.{r['backend']}.{r['component']}",
                "ph": "i", "s": "t", "ts": ts_us, "pid": pid, "tid": tid,
                "args": {
                    "frame": r["frame_id"], "sample": r["correlation_id"],
                    "operation": QUEUE_OPERATIONS.get(r["aux0"], str(r["aux0"])),
                    "queue": f"0x{native_queue:x}",
                    "sync_object": f"0x{r['values'][1]:x}" if r["values"][1] else "",
                    "sync_value": r["values"][2],
                    "related_queue": f"0x{r['values'][3]:x}" if r["values"][3] else "",
                    "submit_sequence": r["values"][4], "stage_mask": f"0x{r['aux1']:08x}",
                    "object_count": r["aux2"], "flags": r["flags"],
                    "cpu_enqueue_only": True,
                },
            })
        elif r["type"] == TYPE_DECISION:
            events.append({
                "name": r["name"], "cat": f"decision.{r['component']}", "ph": "i", "s": "t",
                "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                "args": {
                    "frame": r["frame_id"], "correlation": r["correlation_id"],
                    "outcome": DECISION_OUTCOMES.get(r["aux0"], str(r["aux0"])),
                    "reason": r["detail"], "value0": r["values"][0], "value1": r["values"][1],
                    "value2": r["values"][2], "value3": r["values"][3], "flags": r["flags"],
                },
            })
        elif r["type"] == TYPE_STATE_TRANSITION:
            events.append({
                "name": r["name"], "cat": f"state.{r['component']}", "ph": "i", "s": "t",
                "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                "args": {
                    "frame": r["frame_id"], "previous": r["values"][0], "next": r["values"][1],
                    "subject": r["values"][2], "epoch": r["values"][3], "reason": r["detail"],
                    "flags": r["flags"],
                },
            })
        elif r["type"] == TYPE_RESOURCE_EVENT:
            events.append({
                "name": r["name"], "cat": f"resource.{r['component']}", "ph": "i", "s": "t",
                "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                "args": {
                    "frame": r["frame_id"],
                    "operation": RESOURCE_OPERATIONS.get(r["aux0"], str(r["aux0"])),
                    "resource_id": r["values"][0],
                    "native_handle": f"0x{r['values'][1]:x}" if r["values"][1] else "",
                    "bytes": r["values"][2], "alignment": r["values"][3],
                    "heap_or_memory_type": r["aux1"], "detail": r["detail"], "flags": r["flags"],
                },
            })
        elif r["type"] == TYPE_FLOW_EVENT:
            phase = FLOW_PHASES.get(r["aux0"], str(r["aux0"]))
            chrome_phase = {"begin": "s", "step": "t", "end": "f", "cancel": "f"}.get(phase, "t")
            flow_id = f"apusr:{pid}:{r['name']}:{r['correlation_id']}"
            events.append({
                "name": r["name"], "cat": f"flow.{r['component']}", "ph": chrome_phase,
                "id": flow_id, "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                "args": {
                    "frame": r["frame_id"], "occurrence_id": r["values"][3],
                    "phase": phase, "detail": r["detail"],
                    "parent_flow_id": r["values"][0], "value0": r["values"][1],
                    "value1": r["values"][2], "flags": r["flags"],
                },
            })
        elif r["type"] == TYPE_THREAD_INFO:
            events.append({
                "name": "thread_name", "ph": "M", "pid": pid, "tid": r["thread_id"],
                "args": {"name": r["name"] or f"thread {r['thread_id']}"},
            })
            if r["detail"]:
                events.append({
                    "name": "apusr.thread_role", "cat": "metadata", "ph": "i", "s": "t",
                    "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                    "args": {"role": r["detail"], "priority": signed_u64(r["values"][2]),
                             "affinity_mask": f"0x{r['values'][1]:x}" if r["values"][1] else ""},
                })
        elif r["type"] == TYPE_WAIT_INTERVAL:
            start = r["values"][0]
            events.append({
                "name": r["name"], "cat": f"wait.{r['component']}", "ph": "X",
                "ts": start * 1e6 / freq, "dur": r["values"][2] / 1e3,
                "pid": pid, "tid": r["thread_id"],
                "args": {"frame": r["frame_id"], "correlation": r["correlation_id"],
                         "kind": WAIT_KINDS.get(r["aux0"], str(r["aux0"])),
                         "result": WAIT_RESULTS.get(r["aux1"], str(r["aux1"])),
                         "object": f"0x{r['values'][3]:x}" if r["values"][3] else "",
                         "value": r["values"][4], "timeout_ms": r["values"][5] / 1e6,
                         "observed_value": r["values"][6], "detail": r["detail"],
                         "flags": r["flags"]},
            })
        elif r["type"] == TYPE_METADATA:
            events.append({
                "name": r["name"], "cat": "metadata", "ph": "i", "s": "t",
                "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                "args": {"detail": r["detail"], "value0": r["values"][0],
                         "value1": r["values"][1], "frame": r["frame_id"], "flags": r["flags"]},
            })
        elif r["type"] == TYPE_GPU_WORKLOAD_INFO:
            tx, ty = unpack_pair(r["values"][4])
            tz, subgroup = unpack_pair(r["values"][5])
            events.append({
                "name": r["name"], "cat": f"gpu.workload.{r['backend']}.{r['component']}", "ph": "i", "s": "t",
                "ts": ts_us, "pid": pid, "tid": lane_id(r),
                "args": {"workload_id": r["correlation_id"], "native_pipeline": f"0x{r['values'][0]:x}" if r["values"][0] else "",
                         "code_hash": f"0x{r['values'][1]:016x}" if r["values"][1] else "",
                         "code_bytes": r["values"][2], "shared_memory_bytes": r["values"][3],
                         "threads": [tx, ty, tz], "subgroup_size": subgroup, "detail": r["detail"],
                         "flags": r["flags"]},
            })
        elif r["type"] in (TYPE_INSTANT, TYPE_ERROR):
            events.append({"name": r["name"], "cat": "error" if r["type"] == TYPE_ERROR else "instant", "ph": "i",
                           "s": "t", "ts": ts_us, "pid": pid, "tid": r["thread_id"],
                           "args": {"frame": r["frame_id"], "detail": r["detail"], "flags": r["flags"]}})

    for edge_index, edge in enumerate(reconstruct_queue_dependencies(shards, records), start=1):
        signal = edge["signal"]
        wait = edge["wait"]
        if signal is None:
            continue
        signal_shard = shard_by_source.get(signal["source"])
        wait_shard = shard_by_source.get(wait["source"])
        if not signal_shard or not wait_shard or not signal_shard.cpu_frequency or not wait_shard.cpu_frequency:
            continue
        signal_pid = signal_shard.process_id
        wait_pid = wait_shard.process_id
        if signal_pid != wait_pid:
            continue
        signal_tid = queue_lane.get((signal_pid, signal["values"][0]))
        wait_tid = queue_lane.get((wait_pid, wait["values"][0]))
        if signal_tid is None or wait_tid is None:
            continue
        flow_id = (signal_pid << 32) + edge_index
        sync_name = f"sync 0x{wait['values'][1]:x} value={wait['values'][2]}"
        common_args = {
            "signal": signal["name"], "wait": wait["name"],
            "enqueue_gap_us": edge["enqueue_gap_us"],
            "cross_queue": edge["cross_queue"],
            "cpu_enqueue_only": True,
        }
        events.append({
            "name": sync_name, "cat": "queue.dependency", "ph": "s", "id": flow_id,
            "ts": signal["cpu_tick"] * 1e6 / signal_shard.cpu_frequency,
            "pid": signal_pid, "tid": signal_tid, "args": common_args,
        })
        events.append({
            "name": sync_name, "cat": "queue.dependency", "ph": "f", "id": flow_id,
            "ts": wait["cpu_tick"] * 1e6 / wait_shard.cpu_frequency,
            "pid": wait_pid, "tid": wait_tid, "args": common_args,
        })
    (output / "timeline.json").write_text(json.dumps({"traceEvents": events}), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+", help=".aprof files or directories")
    parser.add_argument("-o", "--output", default="apusr_profile_report")
    parser.add_argument("--timeline", action="store_true", help="also emit timeline.json")
    args = parser.parse_args()

    paths = discover(args.inputs)
    if not paths:
        parser.error("no .aprof files found")
    shards = [read_shard(p) for p in paths]
    records = []
    for shard in shards:
        for record in shard.records:
            record["process_id"] = shard.process_id
            record["session"] = shard.session
            record["module"] = shard.module
            records.append(record)
    records.sort(key=lambda r: (r["cpu_tick"], r["sequence"]))
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    write_csvs(shards, records, output)
    summary = write_summary(shards, records, output)
    if args.timeline:
        write_chrome_trace(shards, records, output)
    print(summary, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
