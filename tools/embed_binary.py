#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


def _bytes_literal(data: bytes) -> str:
    chunks: list[str] = []
    for offset in range(0, len(data), 16):
        row = data[offset : offset + 16]
        chunks.append("    " + ", ".join(f"0x{value:02x}" for value in row) + ",")
    return "\n".join(chunks)


def _read_optional_binary(path: Path | None, label: str, parser: argparse.ArgumentParser) -> bytes:
    if path is None:
        return b""
    if not path.exists():
        parser.error(f"embedded {label} binary does not exist: {path}")
    return path.read_bytes()


def _array_block(symbol: str, data: bytes) -> str:
    if not data:
        return ""
    return f"""alignas(64) constexpr std::uint8_t {symbol}[] = {{
{_bytes_literal(data)}
}};
"""


def _fields(data_field: str, size_field: str, symbol: str, data: bytes) -> str:
    if data:
        return f"""
        .{data_field} = {symbol},
        .{size_field} = sizeof({symbol}),"""
    return f"""
        .{data_field} = nullptr,
        .{size_field} = 0,"""


def _symbol_suffix(value: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z]+", "_", value).strip("_")
    if not cleaned:
        return "unnamed"
    if cleaned[0].isdigit():
        return f"_{cleaned}"
    return cleaned


def _load_json(path: Path, parser: argparse.ArgumentParser) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        parser.error(f"could not read resident parameter set manifest {path}: {error}")
    except json.JSONDecodeError as error:
        parser.error(f"invalid resident parameter set manifest {path}: {error}")
    raise AssertionError("unreachable")


def _resident_arena_entries(args: argparse.Namespace, parser: argparse.ArgumentParser) -> list[tuple[str, str, bytes]]:
    if args.resident_parameters and args.resident_parameter_set:
        parser.error("use either --resident-parameters or --resident-parameter-set, not both")
    if args.resident_parameters is None and args.resident_parameter_set is None:
        return []
    if args.resident_parameters is not None:
        data = _read_optional_binary(args.resident_parameters, "resident-parameter arena", parser)
        return [(args.resident_parameter_model, args.resident_parameter_profile, data)]

    manifest_path: Path = args.resident_parameter_set
    manifest = _load_json(manifest_path, parser)
    arenas = manifest.get("arenas")
    if not isinstance(arenas, list) or not arenas:
        parser.error(f"resident parameter set manifest has no arenas: {manifest_path}")

    entries: list[tuple[str, str, bytes]] = []
    root = manifest_path.parent
    for arena in arenas:
        if not isinstance(arena, dict):
            parser.error(f"resident parameter set manifest contains a non-object arena: {manifest_path}")
        model_permutation = str(arena.get("model_permutation", ""))
        parameter_profile = str(arena.get("parameter_profile", ""))
        if not model_permutation or not parameter_profile:
            parser.error(f"resident parameter set manifest entry is missing model_permutation or parameter_profile: {manifest_path}")
        arena_path = root / model_permutation / parameter_profile / "stage1_12_full_arena.bin"
        if not arena_path.exists():
            parser.error(f"resident parameter arena listed by {manifest_path} does not exist: {arena_path}")
        data = arena_path.read_bytes()
        expected_size = arena.get("arena_bytes")
        if expected_size is not None and len(data) != int(expected_size):
            parser.error(f"resident parameter arena size mismatch for {model_permutation}/{parameter_profile}: expected {expected_size}, got {len(data)}")
        entries.append((model_permutation, parameter_profile, data))
    return entries


def _resident_arena_blocks(entries: list[tuple[str, str, bytes]]) -> tuple[str, str]:
    if not entries:
        return "", ""

    blocks: list[str] = []
    table_rows: list[str] = []
    used_symbols: set[str] = set()
    for index, (model_permutation, parameter_profile, data) in enumerate(entries):
        suffix = _symbol_suffix(f"{model_permutation}_{parameter_profile}")
        symbol = f"kEmbeddedFsr4Dx12ResidentParameterArena_{suffix}"
        if symbol in used_symbols:
            symbol = f"{symbol}_{index}"
        used_symbols.add(symbol)
        blocks.append(_array_block(symbol, data))
        table_rows.append(
            "    EmbeddedFsr4Dx12ResidentParameterArena{"
            f'.modelPermutation = "{model_permutation}", '
            f'.parameterProfile = "{parameter_profile}", '
            f".data = {symbol}, .byteSize = sizeof({symbol})}},"
        )

    table = """constexpr EmbeddedFsr4Dx12ResidentParameterArena kEmbeddedFsr4Dx12ResidentParameterArenas[] = {
%s
};
""" % "\n".join(table_rows)
    return "".join(blocks), table


def main() -> int:
    parser = argparse.ArgumentParser(description="Embed APUSR runtime binary blobs as a C++ source file.")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--instructions", type=Path)
    parser.add_argument("--parameter-load-instructions", type=Path)
    parser.add_argument("--resident-parameters", type=Path)
    parser.add_argument("--resident-parameter-set", type=Path)
    parser.add_argument("--resident-parameter-model", default="manual")
    parser.add_argument("--resident-parameter-profile", default="manual")
    parser.add_argument("--kernel-name", required=True)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    data = _read_optional_binary(args.input, "xclbin", parser)
    instruction_data = _read_optional_binary(args.instructions, "instruction", parser)
    parameter_load_instruction_data = _read_optional_binary(
        args.parameter_load_instructions, "parameter-load instruction", parser
    )
    resident_entries = _resident_arena_entries(args, parser)
    resident_parameter_data = resident_entries[0][2] if len(resident_entries) == 1 else b""
    resident_blocks, resident_table = _resident_arena_blocks(resident_entries)

    if data:
        body = f'''// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
// Generated by tools/embed_binary.py. Do not edit by hand.

#include <apusr/frontends/apusr_fsr4_dx12_embedded_artifacts.h>

namespace apusr::frontends {{
namespace {{

{_array_block("kEmbeddedFsr4Dx12Xclbin", data)}{_array_block("kEmbeddedFsr4Dx12Instructions", instruction_data)}{_array_block("kEmbeddedFsr4Dx12ParameterLoadInstructions", parameter_load_instruction_data)}{resident_blocks}{resident_table}constexpr char kEmbeddedFsr4Dx12KernelName[] = "{args.kernel_name}";

}} // namespace

EmbeddedFsr4Dx12Xclbin embeddedFsr4Dx12Xclbin()
{{
    return EmbeddedFsr4Dx12Xclbin{{
        .data = kEmbeddedFsr4Dx12Xclbin,
        .byteSize = sizeof(kEmbeddedFsr4Dx12Xclbin),{_fields("instructionData", "instructionByteSize", "kEmbeddedFsr4Dx12Instructions", instruction_data)}{_fields("parameterLoadInstructionData", "parameterLoadInstructionByteSize", "kEmbeddedFsr4Dx12ParameterLoadInstructions", parameter_load_instruction_data)}{_fields("residentParameterArenaData", "residentParameterArenaByteSize", "kEmbeddedFsr4Dx12ResidentParameterArena_" + _symbol_suffix(resident_entries[0][0] + "_" + resident_entries[0][1]), resident_parameter_data) if len(resident_entries) == 1 else _fields("residentParameterArenaData", "residentParameterArenaByteSize", "", b"")}
        .residentParameterArenas = {"kEmbeddedFsr4Dx12ResidentParameterArenas" if resident_entries else "nullptr"},
        .residentParameterArenaCount = {"sizeof(kEmbeddedFsr4Dx12ResidentParameterArenas) / sizeof(kEmbeddedFsr4Dx12ResidentParameterArenas[0])" if resident_entries else "0"},
        .kernelName = kEmbeddedFsr4Dx12KernelName,
    }};
}}

}} // namespace apusr::frontends
'''
    else:
        body = '''// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
// Generated by tools/embed_binary.py. Do not edit by hand.

#include <apusr/frontends/apusr_fsr4_dx12_embedded_artifacts.h>

namespace apusr::frontends {

EmbeddedFsr4Dx12Xclbin embeddedFsr4Dx12Xclbin()
{
    return EmbeddedFsr4Dx12Xclbin{};
}

} // namespace apusr::frontends
'''
    args.output.write_text(body, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
