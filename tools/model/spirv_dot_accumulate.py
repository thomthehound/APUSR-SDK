# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Lower provably bounded signed DOT4 chains to SPIR-V accumulating dots.

DXC must preserve HLSL ``dot4add_i8packed`` wraparound semantics, so its
SPIR-V backend emits ``OpSDot`` followed by ``OpIAdd``. RDNA exposes a native
three-source packed dot instruction, but SPIR-V's explicit accumulating form
is saturating. This pass only selects that form when the accumulator is an SSA
sum rooted in another ``OpSDot`` and the worst-case signed-int8 bound proves
that saturation is unreachable.

The rewrite is deliberately narrower than a generic peephole. Loads, phi
nodes, arbitrary integer values, unsigned dots, and multiply-used dot results
remain in DXC's exact split form.
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


SPIRV_MAGIC = 0x07230203
SPIRV_1_5 = 0x00010500

OP_IADD = 128
OP_SDOT = 4450
OP_SDOT_ACC_SAT = 4453
PACKED_VECTOR_FORMAT_4X8BIT = 0

SIGNED_INT32_MAX = (1 << 31) - 1
SIGNED_DOT4_ABS_BOUND = 4 * 128 * 128
MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH = SIGNED_INT32_MAX // SIGNED_DOT4_ABS_BOUND


@dataclass(frozen=True)
class ModuleAudit:
    byte_size: int
    signed_dot_instructions: int
    signed_dot_accumulate_instructions: int
    split_dot_add_pairs: int
    max_proven_chain_depth: int

    @property
    def max_proven_abs_accumulator(self) -> int:
        return self.max_proven_chain_depth * SIGNED_DOT4_ABS_BOUND

    def manifest(self) -> dict[str, int]:
        result = asdict(self)
        result["max_proven_abs_accumulator"] = self.max_proven_abs_accumulator
        return result


@dataclass(frozen=True)
class LoweringResult:
    transformed_pairs: int
    input: ModuleAudit
    output: ModuleAudit

    def manifest(self) -> dict[str, object]:
        return {
            "transformed_pairs": self.transformed_pairs,
            "input": self.input.manifest(),
            "output": self.output.manifest(),
        }


def _decode(data: bytes, source: str) -> tuple[list[int], list[list[int]]]:
    if len(data) < 20 or len(data) % 4:
        raise ValueError(f"invalid SPIR-V byte length for {source}: {len(data)}")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != SPIRV_MAGIC:
        raise ValueError(f"invalid SPIR-V magic for {source}")
    if words[1] != SPIRV_1_5:
        raise ValueError(f"expected SPIR-V 1.5 for {source}, got 0x{words[1]:08x}")

    instructions: list[list[int]] = []
    cursor = 5
    while cursor < len(words):
        first = words[cursor]
        word_count = first >> 16
        if word_count == 0 or cursor + word_count > len(words):
            raise ValueError(f"malformed SPIR-V instruction stream for {source} at word {cursor}")
        instructions.append(words[cursor : cursor + word_count])
        cursor += word_count
    return words[:5], instructions


def _opcode(instruction: list[int]) -> int:
    return instruction[0] & 0xFFFF


def _compatible_sdot(instruction: list[int]) -> bool:
    return (
        _opcode(instruction) == OP_SDOT
        and len(instruction) == 6
        and instruction[5] == PACKED_VECTOR_FORMAT_4X8BIT
    )


def _compatible_sdot_acc_sat(instruction: list[int]) -> bool:
    return (
        _opcode(instruction) == OP_SDOT_ACC_SAT
        and len(instruction) == 7
        and instruction[6] == PACKED_VECTOR_FORMAT_4X8BIT
    )


def _dot_definitions(instructions: list[list[int]]) -> dict[int, int]:
    return {
        instruction[2]: index
        for index, instruction in enumerate(instructions)
        if _compatible_sdot(instruction)
    }


def _dot_use_counts(instructions: list[list[int]], dot_definitions: dict[int, int]) -> dict[int, int]:
    # Scanning every non-definition word can conservatively count an integer
    # literal as an ID. That can only suppress a rewrite; it cannot make an
    # unsafe multiply-used dot look single-use.
    uses = {result_id: 0 for result_id in dot_definitions}
    for instruction in instructions:
        opcode = _opcode(instruction)
        for operand_index, operand in enumerate(instruction[1:], start=1):
            if opcode == OP_SDOT and operand_index == 2:
                continue
            if operand in uses:
                uses[operand] += 1
    return uses


def _split_dot_operand(
    instruction: list[int],
    dot_definitions: dict[int, int],
) -> tuple[int, int] | None:
    if _opcode(instruction) != OP_IADD or len(instruction) != 5:
        return None
    lhs, rhs = instruction[3], instruction[4]
    if lhs in dot_definitions:
        return lhs, rhs
    if rhs in dot_definitions:
        return rhs, lhs
    return None


def _audit_instructions(instructions: list[list[int]], byte_size: int) -> ModuleAudit:
    dot_definitions = _dot_definitions(instructions)
    split_pairs = 0
    proven_depth: dict[int, int] = {}
    max_depth = 0

    for instruction in instructions:
        opcode = _opcode(instruction)
        if _compatible_sdot(instruction):
            proven_depth[instruction[2]] = 1
            max_depth = max(max_depth, 1)
            continue
        if _compatible_sdot_acc_sat(instruction):
            accumulator = instruction[5]
            if accumulator in proven_depth:
                depth = proven_depth[accumulator] + 1
                if depth <= MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH:
                    proven_depth[instruction[2]] = depth
                    max_depth = max(max_depth, depth)
            continue
        split = _split_dot_operand(instruction, dot_definitions)
        if split is None:
            continue
        split_pairs += 1
        _dot_result, accumulator = split
        if accumulator in proven_depth:
            depth = proven_depth[accumulator] + 1
            if depth <= MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH:
                proven_depth[instruction[2]] = depth
                max_depth = max(max_depth, depth)

    return ModuleAudit(
        byte_size=byte_size,
        signed_dot_instructions=sum(_opcode(inst) == OP_SDOT for inst in instructions),
        signed_dot_accumulate_instructions=sum(_opcode(inst) == OP_SDOT_ACC_SAT for inst in instructions),
        split_dot_add_pairs=split_pairs,
        max_proven_chain_depth=max_depth,
    )


def audit_bytes(data: bytes, source: str = "<memory>") -> ModuleAudit:
    _header, instructions = _decode(data, source)
    return _audit_instructions(instructions, len(data))


def audit_file(path: Path) -> ModuleAudit:
    return audit_bytes(path.read_bytes(), str(path))


def lower_bytes(data: bytes, source: str = "<memory>") -> tuple[bytes, LoweringResult]:
    header, instructions = _decode(data, source)
    input_audit = _audit_instructions(instructions, len(data))
    dot_definitions = _dot_definitions(instructions)
    dot_uses = _dot_use_counts(instructions, dot_definitions)

    proven_depth: dict[int, int] = {}
    removed: set[int] = set()
    replacements: dict[int, list[int]] = {}
    transformed = 0

    for index, instruction in enumerate(instructions):
        if _compatible_sdot(instruction):
            proven_depth[instruction[2]] = 1
            continue
        if _compatible_sdot_acc_sat(instruction):
            accumulator = instruction[5]
            if accumulator in proven_depth:
                depth = proven_depth[accumulator] + 1
                if depth <= MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH:
                    proven_depth[instruction[2]] = depth
            continue

        split = _split_dot_operand(instruction, dot_definitions)
        if split is None:
            continue
        dot_result, accumulator = split
        if dot_uses[dot_result] != 1 or accumulator not in proven_depth:
            continue
        depth = proven_depth[accumulator] + 1
        if depth > MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH:
            continue

        dot_index = dot_definitions[dot_result]
        dot_instruction = instructions[dot_index]
        # OpSDotAccSat result type/result ID/vector operands/accumulator/format.
        replacements[index] = [
            (7 << 16) | OP_SDOT_ACC_SAT,
            instruction[1],
            instruction[2],
            dot_instruction[3],
            dot_instruction[4],
            accumulator,
            dot_instruction[5],
        ]
        removed.add(dot_index)
        proven_depth[instruction[2]] = depth
        transformed += 1

    output_words = list(header)
    for index, instruction in enumerate(instructions):
        if index in removed:
            continue
        output_words.extend(replacements.get(index, instruction))
    output = struct.pack(f"<{len(output_words)}I", *output_words)
    output_audit = audit_bytes(output, source)

    if output_audit.max_proven_abs_accumulator > SIGNED_INT32_MAX:
        raise AssertionError("bounded DOT4 lowering exceeded the signed-int32 proof envelope")
    if output_audit.signed_dot_accumulate_instructions != (
        input_audit.signed_dot_accumulate_instructions + transformed
    ):
        raise AssertionError("DOT4 lowering accounting mismatch")
    return output, LoweringResult(transformed, input_audit, output_audit)


def lower_file_in_place(path: Path) -> LoweringResult:
    output, result = lower_bytes(path.read_bytes(), str(path))
    path.write_bytes(output)
    return result


def _aggregate(audits: Iterable[ModuleAudit]) -> dict[str, int]:
    values = list(audits)
    return {
        "modules": len(values),
        "bytes": sum(item.byte_size for item in values),
        "signed_dot_instructions": sum(item.signed_dot_instructions for item in values),
        "signed_dot_accumulate_instructions": sum(item.signed_dot_accumulate_instructions for item in values),
        "split_dot_add_pairs": sum(item.split_dot_add_pairs for item in values),
        "max_proven_chain_depth": max((item.max_proven_chain_depth for item in values), default=0),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spirv", type=Path, nargs="+")
    parser.add_argument("--apply", action="store_true", help="Rewrite each module in place before reporting it.")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    audits: list[ModuleAudit] = []
    results: dict[str, object] = {}
    for path in args.spirv:
        if args.apply:
            result = lower_file_in_place(path)
            audit = result.output
            results[str(path)] = result.manifest()
        else:
            audit = audit_file(path)
            results[str(path)] = audit.manifest()
        audits.append(audit)

    payload = {"modules": results, "aggregate": _aggregate(audits)}
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        for path, result in results.items():
            print(f"{path}\t{json.dumps(result, sort_keys=True)}")
        print(f"aggregate\t{json.dumps(payload['aggregate'], sort_keys=True)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
