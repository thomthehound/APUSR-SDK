#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the APUSR FSR4 middle-core tile-array contract.

The contract is intentionally narrower than the full FSR4 dispatch graph. It
freezes the NPU-owned graph only:

    GPU bookend stage 0 -> C16 seam -> NPU stages 1..12 -> C16 seam -> GPU stage 13

The output is a machine-readable JSON file used by playground simulators and
future MLIR-AIE generators. It does not prescribe one kernel per logical pass;
it records the first full-array reference topology so fusion and tile-local
rewrites can still be made deliberately.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_AIE_ROOT = REPO_ROOT / "models" / "fsr4_int8" / "planning" / "aie_generated"
DEFAULT_OUT_ROOT = REPO_ROOT / "models" / "fsr4_int8" / "planning" / "middle_core_contracts"
MIDDLE_CORE_FIRST_PASS = 1
MIDDLE_CORE_LAST_PASS = 12


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def require(path: Path, what: str) -> Path:
    if not path.exists():
        raise FileNotFoundError(f"missing {what}: {path}")
    return path


def iter_perm_dirs(root_or_perm: Path, *, all_perms: bool) -> Iterable[Path]:
    if (root_or_perm / "graph_plan.json").exists():
        yield root_or_perm
        return
    if not all_perms:
        raise FileNotFoundError(f"expected one generated permutation directory containing graph_plan.json: {root_or_perm}")
    for graph_plan in sorted(root_or_perm.rglob("graph_plan.json")):
        yield graph_plan.parent


def compact_tensor(tensor: dict[str, Any] | None) -> dict[str, Any] | None:
    if tensor is None:
        return None
    keys = (
        "name",
        "tensor_type",
        "W",
        "H",
        "C",
        "effective_C",
        "elem_bytes",
        "token_width_px",
        "tokens_per_row",
        "total_tokens",
    )
    return {key: tensor[key] for key in keys if key in tensor}


def ratio(num: int, den: int) -> dict[str, int]:
    if den <= 0:
        raise ValueError("ratio denominator must be positive")
    if num % den == 0:
        return {"num": num // den, "den": 1}
    if den % num == 0:
        return {"num": 1, "den": den // num}
    raise ValueError(f"non-integral token cadence: {num}/{den}")


def stage_token_cadence(stage: dict[str, Any]) -> dict[str, Any]:
    primary = stage["inputs"]["primary"]
    output = stage["output"]
    primary_tokens = int(primary["total_tokens"])
    output_tokens = int(output["total_tokens"])
    primary_per_output = ratio(primary_tokens, output_tokens)
    output_per_primary = ratio(output_tokens, primary_tokens)

    skip = stage["inputs"].get("skip")
    skip_tokens = int(skip["total_tokens"]) if skip else 0
    skip_per_output = ratio(skip_tokens, output_tokens) if skip else None

    return {
        "primary_tokens": primary_tokens,
        "skip_tokens": skip_tokens,
        "output_tokens": output_tokens,
        "primary_per_output": primary_per_output,
        "output_per_primary": output_per_primary,
        "skip_per_output": skip_per_output,
    }


def compact_stage(stage: dict[str, Any]) -> dict[str, Any]:
    return {
        "pass": int(stage["pass"]),
        "op_fn": stage["op_fn"],
        "kind": stage["kind"],
        "tile": stage["tile"],
        "primary": compact_tensor(stage["inputs"]["primary"]),
        "skip": compact_tensor(stage["inputs"].get("skip")),
        "output": compact_tensor(stage["output"]),
        "token_cadence": stage_token_cadence(stage),
        "notes": stage.get("notes") or [],
    }


def compact_fifo(fifo: dict[str, Any]) -> dict[str, Any]:
    return {
        "name": fifo["name"],
        "tensor": fifo["tensor"],
        "src_pass": int(fifo["src_pass"]),
        "dst_pass": int(fifo["dst_pass"]),
        "logical_size": fifo["logical_size"],
        "effective_channels": int(fifo["effective_channels"]),
        "elem_size_bytes": int(fifo["elem_size_bytes"]),
        "token_bytes": int(fifo["token_bytes"]),
        "token_width_px": int(fifo["token_width_px"]),
        "tokens_per_row": int(fifo["tokens_per_row"]),
        "total_tokens_per_frame": int(fifo["total_tokens_per_frame"]),
        "initial_delay_rows": int(fifo["initial_delay_rows"]),
        "initial_delay_tokens": int(fifo["initial_delay_tokens"]),
        "fifo_min_tokens": int(fifo["fifo_min_tokens"]),
        "fifo_recommended_tokens": int(fifo["fifo_recommended_tokens"]),
        "ring_bytes": int(fifo["ring_bytes"]),
        "memtile": fifo["memtile"],
        "fork_mode": fifo["fork_mode"],
        "delay_model": fifo["delay_model"],
    }


def validate_middle_stages(stages: list[dict[str, Any]]) -> None:
    passes = [int(stage["pass"]) for stage in stages]
    expected = list(range(MIDDLE_CORE_FIRST_PASS, MIDDLE_CORE_LAST_PASS + 1))
    if passes != expected:
        raise ValueError(f"middle-core passes must be {expected}, got {passes}")

    for prev, cur in zip(stages, stages[1:]):
        prev_out = prev["output"]
        cur_in = cur["inputs"]["primary"]
        if int(prev_out["total_tokens"]) != int(cur_in["total_tokens"]):
            raise ValueError(f"token mismatch stage{prev['pass']} -> stage{cur['pass']}")
        if [prev_out["W"], prev_out["H"], prev_out["effective_C"]] != [cur_in["W"], cur_in["H"], cur_in["effective_C"]]:
            raise ValueError(f"shape mismatch stage{prev['pass']} -> stage{cur['pass']}")


def build_contract(perm_dir: Path) -> dict[str, Any]:
    graph = load_json(require(perm_dir / "graph_plan.json", "graph_plan.json"))
    fifo_plan = graph.get("fifo_plan") or {}
    all_stages = [stage for stage in graph.get("stages") or []]
    middle_stages = [stage for stage in all_stages if MIDDLE_CORE_FIRST_PASS <= int(stage["pass"]) <= MIDDLE_CORE_LAST_PASS]
    validate_middle_stages(middle_stages)

    by_pass = {int(stage["pass"]): stage for stage in all_stages}
    if 0 not in by_pass or 13 not in by_pass:
        raise ValueError("full graph must include GPU bookend-adjacent stages 0 and 13")

    skip_fifos = []
    for fifo in fifo_plan.get("fifos") or []:
        src_pass = int(fifo["src_pass"])
        dst_pass = int(fifo["dst_pass"])
        if MIDDLE_CORE_FIRST_PASS <= src_pass <= MIDDLE_CORE_LAST_PASS and MIDDLE_CORE_FIRST_PASS <= dst_pass <= MIDDLE_CORE_LAST_PASS:
            skip_fifos.append(compact_fifo(fifo))

    contract = {
        "schema": "com.apusr.fsr4_middle_core_tile_array_contract.v0",
        "permutation": graph["permutation"],
        "npu_owned_pass_range": [MIDDLE_CORE_FIRST_PASS, MIDDLE_CORE_LAST_PASS],
        "bookends": {
            "gpu_export_pass": 0,
            "gpu_import_pass": 13,
            "note": "Stage 0 and stage 13 remain GPU bookends for the current FSR4-compatible product split.",
        },
        "token_contract": graph["token_contract"],
        "tile_mapping": graph["tile_mapping"],
        "ingress": {
            "from": "gpu_export_stage0",
            "tensor": compact_tensor(by_pass[MIDDLE_CORE_FIRST_PASS]["inputs"]["primary"]),
        },
        "egress": {
            "to": "gpu_import_stage13",
            "tensor": compact_tensor(by_pass[MIDDLE_CORE_LAST_PASS]["output"]),
        },
        "stages": [compact_stage(stage) for stage in middle_stages],
        "main_chain": [
            {"src": "ingress", "dst": "stage1", "kind": "stream"},
            *[
                {"src": f"stage{stage_pass}", "dst": f"stage{stage_pass + 1}", "kind": "stream"}
                for stage_pass in range(MIDDLE_CORE_FIRST_PASS, MIDDLE_CORE_LAST_PASS)
            ],
            {"src": "stage12", "dst": "egress", "kind": "stream"},
        ],
        "skip_fifos": skip_fifos,
        "programming_policy": {
            "first_shape": "full tile-array token skeleton, then replace stage bodies with real primitives",
            "no_full_frame_internal_spills": True,
            "outer_frame_bridge_chunk_bytes": 4096,
            "middle_core_token_bytes": int(graph["token_contract"]["token_bytes"]),
            "skip_storage": "memtile ring delay lines",
        },
    }
    return contract


def emit_contract(perm_dir: Path, out_root: Path) -> Path:
    contract = build_contract(perm_dir)
    out_path = out_root / contract["permutation"] / "middle_core_contract.json"
    write_json(out_path, contract)
    return out_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, default=DEFAULT_AIE_ROOT, help="generated aie root or one permutation directory")
    parser.add_argument("--all", action="store_true", help="generate contracts for every permutation below path")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT, help="contract output root")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    emitted = [emit_contract(path, args.out_root) for path in iter_perm_dirs(args.path, all_perms=args.all)]
    for path in emitted:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
