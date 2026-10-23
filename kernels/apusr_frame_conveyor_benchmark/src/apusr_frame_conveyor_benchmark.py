#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the standalone APUSR frame-conveyor bandwidth benchmark kernel."""

from __future__ import annotations

import argparse
import os

import numpy as np
from aie.helpers.taplib.tap import TensorAccessPattern
from aie.iron import ObjectFifo, Program, Runtime
from aie.iron.device import NPU1, NPU2, Tile

CHUNK_BYTES = 4096
WORD_BYTES = 4
CHUNK_WORDS = CHUNK_BYTES // WORD_BYTES
DEFAULT_FRAME_BYTES = 19_906_560
DEFAULT_NPU1_COLUMNS = 4
DEFAULT_NPU2_COLUMNS = 8
DEFAULT_CHANNELS = 2


def _configured_positive_int(name: str, default: int) -> int:
    raw = os.environ.get(name, str(default))
    try:
        value = int(raw, 10)
    except ValueError as error:
        raise ValueError(f"{name} must be an integer, got {raw!r}") from error
    if value <= 0:
        raise ValueError(f"{name} must be positive")
    return value


def _align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def _resolve_device(device_name: str):
    if device_name in {"npu", "npu1"}:
        return NPU1(), "npu1", DEFAULT_NPU1_COLUMNS
    if device_name == "npu2":
        return NPU2(), "npu2", DEFAULT_NPU2_COLUMNS
    raise ValueError(f"unknown AIE device: {device_name}")


def _parse_column_map(raw: str) -> list[int]:
    try:
        columns = [int(part.strip(), 10) for part in raw.split(",") if part.strip()]
    except ValueError as error:
        raise ValueError(
            f"APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP must be a comma-separated integer list, got {raw!r}"
        ) from error
    if not columns:
        raise ValueError("APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP must not be empty when set")
    if len(set(columns)) != len(columns):
        raise ValueError("APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP columns must be unique")
    return columns


def _resolve_column_map(target_name: str, columns: int, max_columns: int) -> list[int]:
    raw = os.environ.get("APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMN_MAP")
    physical_columns = list(range(max_columns)) if raw is None or raw.strip() == "" else _parse_column_map(raw)

    if len(physical_columns) < columns:
        raise ValueError(
            f"selected {target_name} column map exposes {len(physical_columns)} columns, "
            f"got APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS={columns}"
        )
    selected = physical_columns[:columns]
    for column in selected:
        if column < 0 or column >= max_columns:
            raise ValueError(f"selected {target_name} column map contains out-of-range column {column}")
    return selected


def _validate_geometry(valid_frame_bytes: int, transport_frame_bytes: int, columns: int, channels: int, max_columns: int) -> int:
    if columns > max_columns:
        raise ValueError(f"selected device exposes at most {max_columns} columns, got {columns}")
    if channels not in {1, 2}:
        raise ValueError("APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS must be 1 or 2")
    if transport_frame_bytes % CHUNK_BYTES != 0:
        raise ValueError("internal error: frame transport bytes must be a multiple of 4096")

    frame_chunks = transport_frame_bytes // CHUNK_BYTES
    parallel_streams = columns * channels
    if frame_chunks < parallel_streams:
        raise ValueError("frame transport chunk count must be at least columns * channels")
    if valid_frame_bytes > transport_frame_bytes:
        raise ValueError("internal error: frame transport capacity is smaller than the semantic frame")
    return transport_frame_bytes // WORD_BYTES


def _stream_chunk_span(stream_index: int, stream_count: int, frame_chunks: int) -> tuple[int, int]:
    base_chunks = frame_chunks // stream_count
    extra_chunks = frame_chunks % stream_count
    stream_chunks = base_chunks + (1 if stream_index < extra_chunks else 0)
    first_chunk = stream_index * base_chunks + min(stream_index, extra_chunks)
    return first_chunk, stream_chunks


def build_kernel(device_name: str, valid_frame_bytes: int, columns: int, channels: int):
    device, target_name, max_columns = _resolve_device(device_name)
    column_map = _resolve_column_map(target_name, columns, max_columns)
    transport_frame_bytes = _align_up(valid_frame_bytes, CHUNK_BYTES)
    transport_frame_words = _validate_geometry(valid_frame_bytes, transport_frame_bytes, columns, channels, max_columns)

    xfr_dtype = np.int32
    line_type = np.ndarray[(CHUNK_WORDS,), np.dtype[xfr_dtype]]
    transfer_type = np.ndarray[(transport_frame_words,), np.dtype[xfr_dtype]]
    frame_chunks = transport_frame_bytes // CHUNK_BYTES

    streams = [
        (logical_column, physical_column, channel)
        for logical_column, physical_column in enumerate(column_map)
        for channel in range(channels)
    ]

    of_ins = [
        ObjectFifo(line_type, name=f"apusr_fc_bench_in{logical_column}_{channel}")
        for logical_column, _physical_column, channel in streams
    ]

    # This benchmark measures the frame-conveyor transport envelope only. The
    # ObjectFifo forward path keeps compute tiles out of the measurement.
    shim_tiles = [Tile(physical_column, 0) for physical_column in column_map]
    mem_tiles = [Tile(physical_column, 1) for physical_column in column_map]
    of_outs = [
        of_ins[index]
        .cons()
        .forward(
            tile=mem_tiles[logical_column],
            name=f"apusr_fc_bench_out{logical_column}_{channel}",
        )
        for index, (logical_column, _physical_column, channel) in enumerate(streams)
    ]

    taps = []
    for stream_index, (_logical_column, _physical_column, _channel) in enumerate(streams):
        first_chunk, stream_chunks = _stream_chunk_span(stream_index, len(streams), frame_chunks)
        taps.append(
            TensorAccessPattern(
                (1, transport_frame_words),
                first_chunk * CHUNK_WORDS,
                [1, 1, 1, stream_chunks * CHUNK_WORDS],
                [0, 0, 0, 1],
            )
        )

    runtime = Runtime()
    with runtime.sequence(transfer_type, transfer_type) as (ingress, egress):
        for index, (logical_column, _physical_column, _channel) in enumerate(streams):
            runtime.fill(of_ins[index].prod(), ingress, taps[index], tile=shim_tiles[logical_column])
        for index, (logical_column, _physical_column, _channel) in enumerate(streams):
            runtime.drain(of_outs[index].cons(), egress, taps[index], wait=True, tile=shim_tiles[logical_column])

    return Program(device, runtime).resolve_program()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="npu2", choices=["npu", "npu1", "npu2"])
    args = parser.parse_args()

    _, target_name, max_columns = _resolve_device(args.device)
    default_columns = DEFAULT_NPU1_COLUMNS if target_name == "npu1" else min(DEFAULT_NPU2_COLUMNS, max_columns)
    frame_bytes = _configured_positive_int("APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES", DEFAULT_FRAME_BYTES)
    columns = _configured_positive_int("APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS", default_columns)
    channels = _configured_positive_int("APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS", DEFAULT_CHANNELS)
    print(build_kernel(args.device, frame_bytes, columns, channels))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
