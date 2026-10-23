# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Contract test for APUSR's bounded SPIR-V DOT4 accumulation lowering."""

from pathlib import Path
import struct
import sys


repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo / "tools/model"))
import spirv_dot_accumulate as lowering  # noqa: E402


def instruction(opcode: int, *operands: int) -> list[int]:
    return [((len(operands) + 1) << 16) | opcode, *operands]


def module(*instructions: list[int]) -> bytes:
    words = [lowering.SPIRV_MAGIC, lowering.SPIRV_1_5, 0, 1000, 0]
    for value in instructions:
        words.extend(value)
    return struct.pack(f"<{len(words)}I", *words)


def opcodes(data: bytes) -> list[int]:
    words = struct.unpack(f"<{len(data) // 4}I", data)
    result = []
    cursor = 5
    while cursor < len(words):
        result.append(words[cursor] & 0xFFFF)
        cursor += words[cursor] >> 16
    return result


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> int:
    # %10 is a one-dot root. %12 and %14 are exact depth-2/depth-3 sums.
    exact_chain = [
        instruction(lowering.OP_SDOT, 1, 10, 200, 201, lowering.PACKED_VECTOR_FORMAT_4X8BIT),
        instruction(lowering.OP_SDOT, 1, 11, 202, 203, lowering.PACKED_VECTOR_FORMAT_4X8BIT),
        instruction(lowering.OP_IADD, 1, 12, 11, 10),
        instruction(lowering.OP_SDOT, 1, 13, 204, 205, lowering.PACKED_VECTOR_FORMAT_4X8BIT),
        instruction(lowering.OP_IADD, 1, 14, 13, 12),
    ]
    # An arbitrary accumulator is not bounded by the pass and must stay split.
    unbounded = [
        instruction(lowering.OP_SDOT, 1, 20, 210, 211, lowering.PACKED_VECTOR_FORMAT_4X8BIT),
        instruction(lowering.OP_IADD, 1, 21, 20, 999),
    ]
    # A dot result with two consumers cannot be removed and must stay split.
    multiply_used = [
        instruction(lowering.OP_SDOT, 1, 30, 220, 221, lowering.PACKED_VECTOR_FORMAT_4X8BIT),
        instruction(lowering.OP_IADD, 1, 31, 30, 10),
        instruction(lowering.OP_IADD, 1, 32, 30, 10),
    ]
    original = module(*(exact_chain + unbounded + multiply_used))
    transformed, result = lowering.lower_bytes(original, "synthetic-contract")
    transformed_opcodes = opcodes(transformed)

    require(result.transformed_pairs == 2, f"expected two bounded fusions, got {result.transformed_pairs}")
    require(result.output.signed_dot_accumulate_instructions == 2, "bounded chains must become OpSDotAccSat")
    require(result.output.split_dot_add_pairs == 3, "unbounded and multiply-used dots must remain split")
    require(result.output.max_proven_chain_depth == 3, "chain-depth proof must survive the rewrite")
    require(
        result.output.max_proven_abs_accumulator == 3 * lowering.SIGNED_DOT4_ABS_BOUND,
        "accumulator bound accounting mismatch",
    )
    require(transformed_opcodes.count(lowering.OP_SDOT_ACC_SAT) == 2, "missing accumulating-dot opcodes")
    require(transformed_opcodes.count(lowering.OP_SDOT) == 3, "only single-use fused dot nodes may be removed")
    require(len(transformed) < len(original), "fused module should remove one instruction per transformed pair")

    second, second_result = lowering.lower_bytes(transformed, "synthetic-contract-idempotence")
    require(second == transformed, "bounded DOT4 lowering must be idempotent")
    require(second_result.transformed_pairs == 0, "second lowering pass must not report new work")

    max_safe = lowering.MAX_EXACT_SIGNED_DOT4_CHAIN_DEPTH * lowering.SIGNED_DOT4_ABS_BOUND
    require(max_safe <= lowering.SIGNED_INT32_MAX, "declared chain limit must fit signed int32")
    require(
        max_safe + lowering.SIGNED_DOT4_ABS_BOUND > lowering.SIGNED_INT32_MAX,
        "declared chain limit must be the maximal mathematical bound",
    )

    print(
        "APUSR SPIR-V DOT4 accumulate contract passed: "
        "bounded SSA chains fuse, arbitrary/multiply-used accumulators stay exact, and the pass is idempotent"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
