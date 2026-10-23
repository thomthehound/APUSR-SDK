# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""gen_memtile_fifos.py

Purpose
-------
Generate a *machine-readable* plan for the two on-die skip FIFOs.

We already know (from `streaming_feasibility.json`) which edges are skips and we
have a first-cut sizing heuristic (depth ~= stage distance + elastic). This tool
turns that into a concrete per-memtile allocation plan:

  - which memtile column each FIFO lives in
  - ring buffer byte size + base offset
  - token geometry (token width, tokens/row) derived from the v0 token contract
  - an explicit description of whether the FIFO is used as a *pure skip* or as a
    *tap* feeding the main chain as well (fork_mode)

The output is intentionally *not* MLIR-AIE yet. It is the thing we want to keep
stable while we iterate on compilation details.

Usage
-----
  # Generate a FIFO plan for one permutation
  python tools/gen/gen_memtile_fifos.py out/dispatch_graphs/balanced__2160

  # Generate FIFO plans for all permutations under a root
  python tools/gen/gen_memtile_fifos.py out/dispatch_graphs --all

  # If you want to avoid relying on stream-switch multicast at fork points
  python tools/gen/gen_memtile_fifos.py out/dispatch_graphs/balanced__2160 --fork-mode memtile_tap

Notes
-----
fork_mode
  multicast   : stage output is duplicated by the stream fabric; FIFO is used
                only for the delayed consumer.
  memtile_tap : stage output is written once into the FIFO; the next stage and
                the delayed consumer each get their own MM2S tap.

`memtile_tap` is slower but more "bring-up robust" because it avoids assuming a
1-to-N stream connection feature that may or may not exist in the final toolchain.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


MEMTILE_CAPACITY_BYTES_DEFAULT = 512 * 1024
ELASTIC_ROWS_DEFAULT = 2


def _load_json(p: Path) -> Dict[str, Any]:
    return json.loads(p.read_text(encoding="utf-8"))


def _maybe_load_schedule(out_root: Path, perm_name: str) -> Optional[Dict[str, Any]]:
    """Load schedule_plan.json if it exists.

    The FIFO sizing can be improved significantly if we know the per-stage
    cadence (especially the vertical down/up phases). That plan is produced by
    `gen_stream_schedule.py` and written alongside other generator outputs.

    We intentionally keep this optional so `gen-fifos` can still run in early
    bring-up flows.
    """
    p = out_root / perm_name / "schedule_plan.json"
    if not p.exists():
        return None
    return _load_json(p)


def _ratio(stage: Dict[str, Any], axis: str) -> Tuple[int, int]:
    r = (stage.get("cadence") or {}).get(f"{axis}_ratio") or {}
    return int(r.get("num", 1)), int(r.get("den", 1))


def _input_row_needed_for_output_row(stage: Dict[str, Any], out_row: int) -> int:
    """Conservative row mapping for our v0 cadence model.

    We only use the stage's integer y_ratio from `schedule_plan.json`.

    Rules
    -----
    - downsample (y_ratio < 1): output row depends on `in_factor` input rows.
      We assume the *last* of those rows must be available to produce the row.
      Example: y_ratio=1/2 -> in_factor=2 -> in_row = out_row*2 + 1.

    - upsample (y_ratio > 1): output row depends on one input row.
      Example: y_ratio=2 -> out_factor=2 -> in_row = floor(out_row/2).

    - same-scale: in_row = out_row.

    This is not trying to model kernel micro-latency; it's a stable, simple,
    and intentionally *conservative* alignment estimate.
    """

    y_num, y_den = _ratio(stage, "y")
    if y_num <= 0 or y_den <= 0:
        return int(out_row)

    # downsample
    if y_num < y_den:
        # in_factor must be integer for our current graphs.
        in_factor = y_den // y_num
        return int(out_row * in_factor + (in_factor - 1))

    # upsample or same
    out_factor = y_num // y_den
    if out_factor <= 1:
        return int(out_row)
    return int(out_row // out_factor)


def _compute_skip_delay_tokens_from_schedule(
    schedule: Dict[str, Any],
    *,
    src_pass: int,
    dst_pass: int,
    elastic_rows: int,
) -> Tuple[int, int, int]:
    """Return (delay_rows, delay_tokens, ring_tokens).

    delay_rows is expressed in the skip tensor's row space.
    delay_tokens = delay_rows * tokens_per_row_at_src.
    ring_tokens includes an additional elastic margin (in whole rows).
    """

    stages: List[Dict[str, Any]] = schedule.get("stages") or []
    by_pass: Dict[int, Dict[str, Any]] = {int(s.get("pass")): s for s in stages}
    if src_pass not in by_pass or dst_pass not in by_pass:
        raise ValueError(f"schedule missing src/dst passes: {src_pass}->{dst_pass}")

    dst_stage = by_pass[dst_pass]
    src_stage = by_pass[src_pass]

    # We size delay to align skip with the *start* of the merge stage's output.
    # For our graphs, dst_pass is the merge/consumer for the skip edge.
    out_row_needed_at_dst_output = 0

    # First map the required merge-output row into the merge stage's primary input.
    row_needed = _input_row_needed_for_output_row(dst_stage, out_row_needed_at_dst_output)

    # Walk upstream along the primary chain until we reach src_pass.
    # Our v0 topology is linear in pass order; if that changes later we'll
    # switch this traversal to use graph_plan links.
    for p in range(dst_pass - 1, src_pass, -1):
        st = by_pass.get(p)
        if st is None:
            raise ValueError(f"schedule missing pass {p} while solving delay")
        row_needed = _input_row_needed_for_output_row(st, row_needed)

    delay_rows = int(row_needed)

    tpr = int((src_stage.get("output") or {}).get("tokens_per_row"))
    if tpr <= 0:
        raise ValueError(f"bad tokens_per_row for src_pass={src_pass}")

    delay_tokens = delay_rows * tpr
    ring_tokens = delay_tokens + max(0, int(elastic_rows)) * tpr
    return delay_rows, delay_tokens, ring_tokens

def _physical_origin(tile_mapping: Dict[str, Any]) -> Tuple[int, int]:
    """Return (col,row) offset mapping logical slab coords -> physical device coords.

    If the mapping does not specify a physical origin, we treat logical and
    physical as identical (origin = 0,0).
    """
    rs = tile_mapping.get("reserved_slab") or {}
    po = rs.get("physical_origin") or {}
    return int(po.get("col", 0)), int(po.get("row", 0))



def _iter_perm_dirs(root_or_perm: Path, *, all_perms: bool) -> Iterable[Path]:
    if root_or_perm.is_file():
        # Treat a single file as "its parent permutation directory".
        yield root_or_perm.parent
        return

    if (root_or_perm / "dispatch_graph.json").exists():
        # Caller passed a single permutation folder.
        yield root_or_perm
        return

    if not all_perms:
        # Convenience: behave like --all.
        all_perms = True

    if all_perms:
        for p in sorted(root_or_perm.rglob("dispatch_graph.json")):
            yield p.parent


def _require(p: Path, what: str) -> Path:
    if not p.exists():
        raise FileNotFoundError(f"Missing {what}: {p}")
    return p


def _effective_channels(token_contract: Dict[str, Any], *, tensor_name: str, tensor_type: str, logical_c: int) -> int:
    """Apply any token-contract boundary rules (e.g. pad fp16 input C=7 -> C=8)."""

    rules = (token_contract.get("boundary_rules") or {}).get("fp16_input_pad") or {}
    if (
        tensor_type == "Tensor3h_NHWC"
        and tensor_name == rules.get("tensor_name")
        and int(logical_c) == int(rules.get("from_channels", -1))
    ):
        return int(rules.get("to_channels", logical_c))
    return int(logical_c)


def _token_width_px(token_bytes: int, channels: int, elem_bytes: int) -> int:
    denom = channels * elem_bytes
    if denom <= 0 or token_bytes % denom != 0:
        raise ValueError(
            f"TOKEN_BYTES={token_bytes} does not yield integer token width for C={channels} E={elem_bytes}"
        )
    w = token_bytes // denom
    if w < 1:
        raise ValueError("token width computed < 1 (unexpected)")
    return int(w)


@dataclass(frozen=True)
class SkipEdge:
    name: str
    src_pass: int
    dst_pass: int
    tensor: str
    tensor_type: str
    logical_size: Tuple[int, int, int]  # (W,H,C)
    elem_size_bytes: int
    fifo_min_tokens: int
    fifo_recommended_tokens: int


def _load_skip_edges(perm_dir: Path) -> List[SkipEdge]:
    feas = _load_json(_require(perm_dir / "streaming_feasibility.json", "streaming_feasibility.json"))
    out: List[SkipEdge] = []
    for e in feas.get("skip_edges") or []:
        ls = e.get("logical_size")
        if not isinstance(ls, list) or len(ls) != 3:
            raise ValueError(f"bad logical_size in skip edge: {e}")
        out.append(
            SkipEdge(
                name=str(e.get("tensor")),
                src_pass=int(e.get("src_pass")),
                dst_pass=int(e.get("dst_pass")),
                tensor=str(e.get("tensor")),
                tensor_type=str(e.get("tensor_type")),
                logical_size=(int(ls[0]), int(ls[1]), int(ls[2])),
                elem_size_bytes=int(e.get("elem_size_bytes")),
                fifo_min_tokens=int(e.get("fifo_min_tokens")),
                fifo_recommended_tokens=int(e.get("fifo_recommended_tokens")),
            )
        )
    # Stable order: by src_pass then dst_pass.
    out.sort(key=lambda x: (x.src_pass, x.dst_pass))
    return out


def _plan_for_perm(
    perm_dir: Path,
    *,
    token_contract_path: Path,
    tile_mapping_path: Path,
    out_dir: Path,
    fork_mode: str,
    memtile_capacity_bytes: int,
    delay_model: str,
    elastic_rows: int,
) -> Dict[str, Any]:
    dg = _load_json(_require(perm_dir / "dispatch_graph.json", "dispatch_graph.json"))
    token_contract = _load_json(_require(token_contract_path, "token contract JSON"))
    tile_mapping = _load_json(_require(tile_mapping_path, "tile mapping JSON"))

    origin_col, origin_row = _physical_origin(tile_mapping)

    token_bytes = int(token_contract.get("token_bytes"))
    if token_bytes <= 0:
        raise ValueError("token_bytes must be > 0")

    skip_edges = _load_skip_edges(perm_dir)
    schedule = _maybe_load_schedule(out_dir.parent, perm_dir.name) if delay_model == "cadence_rows" else None
    mapping_skips = tile_mapping.get("skip_fifos") or []
    memtile_by_name: Dict[str, int] = {}
    for s in mapping_skips:
        memtile_by_name[str(s.get("name"))] = int(s.get("memtile_col"))

    # Map feasibility edges to our canonical names "skipA" / "skipB" by src/dst pass.
    # (We keep this mapping local and explicit so it doesn't silently drift.)
    canonical: Dict[Tuple[int, int], str] = {
        (2, 11): "skipA",
        (5, 9): "skipB",
    }

    fifos: List[Dict[str, Any]] = []
    tensors = dg.get("tensors") or {}

    for se in skip_edges:
        canon = canonical.get((se.src_pass, se.dst_pass))
        if canon is None:
            canon = f"skip_{se.src_pass}_to_{se.dst_pass}"

        W, H, C = se.logical_size
        td = tensors.get(se.tensor) or {}
        # Prefer tensor table for element size (sometimes feasibility was computed from it).
        elem_bytes = int(td.get("elem_size_bytes", se.elem_size_bytes))
        tensor_type = str(td.get("tensor_type", se.tensor_type))
        eff_C = _effective_channels(token_contract, tensor_name=se.tensor, tensor_type=tensor_type, logical_c=C)
        token_w = _token_width_px(token_bytes, eff_C, elem_bytes)
        if W % token_w != 0:
            raise ValueError(f"tensor width W={W} is not divisible by token_width={token_w} for {se.tensor}")

        tokens_per_row = W // token_w
        total_tokens = tokens_per_row * H

        # Decide how much the skip stream must be delayed to align with its consumer.
        #
        # - stage_distance: keep the legacy feasibility-provided recommendation.
        # - cadence_rows : solve a conservative row-based alignment using schedule_plan.json.
        warnings: List[str] = []
        delay_rows = 0
        delay_tokens = 0
        ring_tokens = int(se.fifo_recommended_tokens)

        if delay_model == "cadence_rows" and schedule is not None:
            delay_rows, delay_tokens, solved_ring_tokens = _compute_skip_delay_tokens_from_schedule(
                schedule,
                src_pass=se.src_pass,
                dst_pass=se.dst_pass,
                elastic_rows=elastic_rows,
            )
            ring_tokens = max(ring_tokens, int(solved_ring_tokens))
        elif delay_model == "cadence_rows":
            warnings.append("schedule_plan.json missing; falling back to stage_distance sizing")

        # Ensure the ring is always large enough to actually represent the intended delay.
        fifo_min_tokens = max(int(se.fifo_min_tokens), int(delay_tokens))
        ring_tokens = max(int(ring_tokens), int(fifo_min_tokens))

        ring_bytes = ring_tokens * token_bytes
        if ring_bytes > memtile_capacity_bytes:
            raise ValueError(
                f"FIFO {canon} ring_bytes={ring_bytes} exceeds memtile capacity={memtile_capacity_bytes}"
            )

        memtile_col = int(memtile_by_name.get(canon, -1))
        if memtile_col < 0:
            # Fall back to producer column if not specified.
            memtile_col = int(se.src_pass)  # not a real column, but at least stable

        fifo_entry: Dict[str, Any] = {
            "name": canon,
            "tensor": se.tensor,
            "src_pass": se.src_pass,
            "dst_pass": se.dst_pass,
            "tensor_type": tensor_type,
            "logical_size": [W, H, C],
            "elem_size_bytes": elem_bytes,
            "effective_channels": eff_C,
            "token_bytes": token_bytes,
            "token_width_px": token_w,
            "tokens_per_row": tokens_per_row,
            "total_tokens_per_frame": total_tokens,
            "fifo_min_tokens": fifo_min_tokens,
            "fifo_recommended_tokens": ring_tokens,
            "delay_model": delay_model,
            "initial_delay_rows": delay_rows,
            "initial_delay_tokens": delay_tokens,
            "ring_bytes": ring_bytes,
            "memtile": {
                "logical_col": memtile_col,
                "col": memtile_col + origin_col,
                "origin_col": origin_col,
                "capacity_bytes": memtile_capacity_bytes,
                "base_offset_bytes": 0
            },
            "fork_mode": fork_mode,
            "dma_model": {
                "writer": {
                    "kind": "S2MM",
                    "desc": "Stream-to-memory ring writer (producer side)"
                },
                "readers": []
            }
        }

        # Decide the reader's startup delay. If we solved cadence, prefer that explicit delay;
        # otherwise keep the legacy "delay == ring" approximation.
        reader_delay_tokens = delay_tokens if (delay_model == "cadence_rows" and schedule is not None) else ring_tokens

        if fork_mode == "memtile_tap":
            fifo_entry["dma_model"]["readers"] = [
                {
                    "name": "tap_main",
                    "kind": "MM2S",
                    "initial_delay_tokens": 0,
                    "desc": "Immediate tap feeding the next stage (avoids stream-switch multicast)"
                },
                {
                    "name": "tap_skip",
                    "kind": "MM2S",
                    "initial_delay_tokens": int(reader_delay_tokens),
                    "desc": "Delayed tap feeding the skip consumer"
                }
            ]
        else:
            fifo_entry["dma_model"]["readers"] = [
                {
                    "name": "skip_only",
                    "kind": "MM2S",
                    "initial_delay_tokens": int(reader_delay_tokens),
                    "desc": "Delayed tap feeding the skip consumer"
                }
            ]

        if warnings:
            fifo_entry["warnings"] = warnings

        fifos.append(fifo_entry)

    plan: Dict[str, Any] = {
        "schema": "com.apusr.memtile_fifo_plan.v0",
        "permutation": perm_dir.name,
        "fork_mode": fork_mode,
        "token_contract": {
            "path": str(token_contract_path.as_posix()),
            "token_bytes": token_bytes,
            "beat_bytes": int(token_contract.get("beat_bytes", 8))
        },
        "tile_mapping": {
            "path": str(tile_mapping_path.as_posix()),
            "reserved_slab": tile_mapping.get("reserved_slab")
        },
        "fifos": fifos,
        "warnings": []
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "fifo_plan.json"
    out_path.write_text(json.dumps(plan, indent=2, sort_keys=True), encoding="utf-8")
    return plan


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Generate skip FIFO plans (memtile ring buffers) for FSR4 streaming")
    ap.add_argument(
        "path",
        type=Path,
        help="Permutation folder (containing dispatch_graph.json) or a root containing permutations",
    )
    ap.add_argument("--all", action="store_true", help="If PATH is a directory root, scan recursively")
    ap.add_argument(
        "--fork-mode",
        choices=["multicast", "memtile_tap"],
        default="multicast",
        help=(
            "How to handle fork points (a tensor feeding next stage + skip). "
            "multicast keeps main chain as direct stream and assumes fabric duplication; "
            "memtile_tap writes once to memtile and taps main+skip as two MM2S readers."
        ),
    )
    ap.add_argument(
        "--token-contract",
        type=Path,
        default=Path("docs/decisions/token_contract_v0.json"),
        help="Path to token contract JSON (default: docs/decisions/token_contract_v0.json)",
    )
    ap.add_argument(
        "--tile-mapping",
        type=Path,
        default=Path("docs/decisions/tile_mapping_v0.json"),
        help="Path to tile mapping JSON (default: docs/decisions/tile_mapping_v0.json)",
    )
    ap.add_argument(
        "--out-root",
        type=Path,
        default=Path("models/fsr4_int8/planning/aie_generated"),
        help="Where to write outputs (default: models/fsr4_int8/planning/aie_generated)",
    )
    ap.add_argument(
        "--memtile-capacity-bytes",
        type=int,
        default=MEMTILE_CAPACITY_BYTES_DEFAULT,
        help=f"Memtile capacity assumption (default: {MEMTILE_CAPACITY_BYTES_DEFAULT})",
    )
    ap.add_argument(
        "--delay-model",
        choices=["stage_distance", "cadence_rows"],
        default="cadence_rows",
        help=(
            "How to estimate the initial skip delay. "
            "stage_distance uses the older feasibility heuristic. "
            "cadence_rows uses schedule_plan.json to solve a conservative row-based alignment."
        ),
    )
    ap.add_argument(
        "--elastic-rows",
        type=int,
        default=ELASTIC_ROWS_DEFAULT,
        help=(
            "Extra ring depth (in whole rows) added on top of the solved delay. "
            "This is a safety margin for micro-latency and backpressure (default: %(default)s)."
        ),
    )
    args = ap.parse_args(argv)

    # Resolve paths relative to repo root if the user runs from the repo.
    token_contract_path = args.token_contract
    tile_mapping_path = args.tile_mapping

    perm_dirs = list(_iter_perm_dirs(args.path, all_perms=args.all))
    if not perm_dirs:
        print(f"No permutations found under: {args.path}")
        return 2

    any_fail = False
    for perm_dir in perm_dirs:
        try:
            out_dir = args.out_root / perm_dir.name
            _plan_for_perm(
                perm_dir,
                token_contract_path=token_contract_path,
                tile_mapping_path=tile_mapping_path,
                out_dir=out_dir,
                fork_mode=args.fork_mode,
                memtile_capacity_bytes=int(args.memtile_capacity_bytes),
                delay_model=str(args.delay_model),
                elastic_rows=int(args.elastic_rows),
            )
            print(f"[{perm_dir.name}] wrote {out_dir / 'fifo_plan.json'}")
        except Exception as e:
            any_fail = True
            print(f"[{perm_dir.name}] ERROR: {e}")

    return 2 if any_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
