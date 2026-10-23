# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""gen_dma_plan.py

Purpose
-------
Convert the *logical* topology (graph_plan + schedule + fifo_plan) into a
*mostly-static DMA* plan skeleton.

What this is (v0)
-----------------
This generator does **not** attempt to emit MLIR-AIE or xclbin artifacts.
Instead, it produces a machine-readable `dma_plan.json` capturing the decisions
we want to keep stable while we iterate on the compiler/toolchain details:

  - which streams are direct (tile-to-tile) vs. go through memtiles
  - which stages need multicast duplication at the fork points
  - memtile ring sizing + initial skip delays (from fifo_plan)
  - a conservative per-stage cadence summary (from schedule_plan)

Downstream tools can consume this plan to generate MLIR-AIE, XRT host code, or
even a hand-written DMA bring-up harness.

Usage
-----
  # One permutation
  python tools/gen/gen_dma_plan.py out/dispatch_graphs/balanced__2160

  # All permutations
  python tools/gen/gen_dma_plan.py out/dispatch_graphs --all
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def _load_json(p: Path) -> Dict[str, Any]:
    return json.loads(p.read_text(encoding="utf-8"))


def _require(p: Path, what: str) -> Path:
    if not p.exists():
        raise FileNotFoundError(f"Missing {what}: {p}")
    return p


def _iter_perm_dirs(root_or_perm: Path, *, all_perms: bool) -> Iterable[Path]:
    if root_or_perm.is_file():
        yield root_or_perm.parent
        return

    if (root_or_perm / "dispatch_graph.json").exists():
        yield root_or_perm
        return

    if not all_perms:
        all_perms = True

    if all_perms:
        for p in sorted(root_or_perm.rglob("dispatch_graph.json")):
            yield p.parent


def _ratio(stage: Dict[str, Any], axis: str) -> Tuple[int, int]:
    r = (stage.get("cadence") or {}).get(f"{axis}_ratio") or {}
    return int(r.get("num", 1)), int(r.get("den", 1))


def _summarize_stage(stage: Dict[str, Any]) -> Dict[str, Any]:
    """Extract a compact, stable subset of schedule fields."""
    p = int(stage.get("pass"))
    kind = str(stage.get("kind"))
    tile = stage.get("tile") or {}
    out_t = stage.get("output") or {}
    prim = stage.get("primary") or {}
    skip = stage.get("skip")
    y_num, y_den = _ratio(stage, "y")
    x_num, x_den = _ratio(stage, "x")
    t_num, t_den = _ratio(stage, "tokens")
    return {
        "pass": p,
        "kind": kind,
        "tile": {
            "col": int(tile.get("col")),
            "row": int(tile.get("row")),
            "logical_col": int(tile.get("logical_col")),
            "logical_row": int(tile.get("logical_row")),
        },
        "primary": {
            "tokens_per_row": int(prim.get("tokens_per_row")),
            "H": int(prim.get("H")),
            "effective_C": int(prim.get("effective_C")),
            "elem_bytes": int(prim.get("elem_bytes")),
        },
        "skip": None
        if skip is None
        else {
            "tokens_per_row": int(skip.get("tokens_per_row")),
            "H": int(skip.get("H")),
            "effective_C": int(skip.get("effective_C")),
            "elem_bytes": int(skip.get("elem_bytes")),
        },
        "output": {
            "tokens_per_row": int(out_t.get("tokens_per_row")),
            "H": int(out_t.get("H")),
            "effective_C": int(out_t.get("effective_C")),
            "elem_bytes": int(out_t.get("elem_bytes")),
        },
        "cadence": {
            "x_ratio": {"num": x_num, "den": x_den},
            "y_ratio": {"num": y_num, "den": y_den},
            "tokens_ratio": {"num": t_num, "den": t_den},
            "macro_hint": (stage.get("cadence") or {}).get("macro_hint"),
        },
    }


def _build_dma_links(graph_plan: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Return links with stable stream names for downstream emission."""
    links = graph_plan.get("links") or []
    out: List[Dict[str, Any]] = []
    for i, l in enumerate(links):
        src = l.get("src") or {}
        dst = l.get("dst") or {}
        out.append(
            {
                "id": f"link{i}",
                "kind": str(l.get("kind")),
                "note": str(l.get("note", "")),
                "src": {"node": str(src.get("node")), "port": str(src.get("port"))},
                "dst": {"node": str(dst.get("node")), "port": str(dst.get("port"))},
            }
        )
    return out


def _plan_for_perm(
    perm_dir: Path,
    *,
    out_root: Path,
    fork_mode_override: Optional[str],
) -> Dict[str, Any]:
    perm_name = perm_dir.name

    gen_dir = out_root / perm_name
    graph_plan = _load_json(_require(gen_dir / "graph_plan.json", "graph_plan.json"))
    schedule_plan = _load_json(_require(gen_dir / "schedule_plan.json", "schedule_plan.json"))
    fifo_plan = _load_json(_require(gen_dir / "fifo_plan.json", "fifo_plan.json"))

    token_contract = graph_plan.get("token_contract") or {}
    tile_mapping = graph_plan.get("tile_mapping") or {}

    stages = schedule_plan.get("stages") or []
    stage_summaries = [_summarize_stage(s) for s in stages]

    # Memtile DMA skeleton: we keep this intentionally high-level.
    fifos_out: List[Dict[str, Any]] = []
    for f in fifo_plan.get("fifos") or []:
        memtile = f.get("memtile") or {}
        fifos_out.append(
            {
                "name": str(f.get("name")),
                "tensor": str(f.get("tensor")),
                "src_pass": int(f.get("src_pass")),
                "dst_pass": int(f.get("dst_pass")),
                "memtile": {
                    "col": int(memtile.get("col")),
                    "logical_col": int(memtile.get("logical_col")),
                    "capacity_bytes": int(memtile.get("capacity_bytes")),
                },
                "ring_bytes": int(f.get("ring_bytes")),
                "token_bytes": int(f.get("token_bytes")),
                "tokens_per_row": int(f.get("tokens_per_row")),
                "initial_delay_tokens": int(f.get("initial_delay_tokens", 0)),
                "delay_model": str(f.get("delay_model", "stage_distance")),
                "fork_mode": str(f.get("fork_mode")),
                "dma": {
                    "writer": {"kind": "S2MM", "ring": True},
                    "readers": f.get("dma_model", {}).get("readers") or [],
                    "notes": [
                        "v0 skeleton: exact BD counts/ctrl sequencing emitted by later toolchain",
                        "ring buffer assumes steady-state equal read/write token rates",
                    ],
                },
            }
        )

    plan: Dict[str, Any] = {
        "schema": "com.apusr.dma_plan.v0",
        "permutation": perm_name,
        "fork_mode": str(fork_mode_override or graph_plan.get("fork_mode") or "unknown"),
        "token_contract": {
            "token_bytes": int(token_contract.get("token_bytes")),
            "beat_bytes": int(token_contract.get("beat_bytes", 8)),
            "boundary_rules": token_contract.get("boundary_rules"),
        },
        "tile_mapping": tile_mapping,
        "stages": stage_summaries,
        "links": _build_dma_links(graph_plan),
        "memtile_fifos": fifos_out,
        "notes": [
            "This plan is intentionally conservative/high-level.",
            "Next step: emit MLIR-AIE DMA channels + BDs directly from this JSON.",
        ],
        "warnings": [],
    }

    gen_dir.mkdir(parents=True, exist_ok=True)
    (gen_dir / "dma_plan.json").write_text(json.dumps(plan, indent=2, sort_keys=True), encoding="utf-8")

    # A tiny human-readable companion.
    md = [
        f"# DMA plan (skeleton) — {perm_name}",
        "",
        "This file is generated from `graph_plan.json`, `schedule_plan.json`, and `fifo_plan.json`.",
        "",
        "## Skip FIFOs",
        "",
    ]
    for f in fifos_out:
        md.append(
            f"- **{f['name']}**: src_pass={f['src_pass']} -> dst_pass={f['dst_pass']}, "
            f"delay_tokens={f['initial_delay_tokens']}, ring_bytes={f['ring_bytes']} (memtile col {f['memtile']['col']})"
        )
    md.append("")
    md.append("## Stages")
    md.append("")
    md.append("Pass → kind @ tile (col,row) — primary (tpr,H,C,bytes) → output")
    for s in stage_summaries:
        prim = s["primary"]
        out_t = s["output"]
        md.append(
            f"- p{s['pass']:02d} {s['kind']:<18} @ ({s['tile']['col']},{s['tile']['row']}): "
            f"prim tpr={prim['tokens_per_row']} H={prim['H']} C={prim['effective_C']} e={prim['elem_bytes']} "
            f"-> out tpr={out_t['tokens_per_row']} H={out_t['H']} C={out_t['effective_C']} e={out_t['elem_bytes']}"
        )
    (gen_dir / "dma_plan.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    return plan


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Generate a DMA plan skeleton (dma_plan.json) from generator outputs")
    ap.add_argument(
        "path",
        type=Path,
        help="Permutation folder (containing dispatch_graph.json) or a root containing permutations",
    )
    ap.add_argument("--all", action="store_true", help="If PATH is a directory root, scan recursively")
    ap.add_argument(
        "--out-root",
        type=Path,
        default=Path("models/fsr4_int8/planning/aie_generated"),
        help="Where generator outputs live + where dma_plan will be written (default: models/fsr4_int8/planning/aie_generated)",
    )
    ap.add_argument(
        "--fork-mode",
        choices=["multicast", "memtile_tap"],
        default=None,
        help="Accepted for CLI consistency; if provided, stored into dma_plan.json metadata.",
    )
    args = ap.parse_args(argv)

    perm_dirs = list(_iter_perm_dirs(args.path, all_perms=args.all))
    if not perm_dirs:
        print(f"No permutations found under: {args.path}")
        return 2

    any_fail = False
    for perm_dir in perm_dirs:
        try:
            _plan_for_perm(perm_dir, out_root=args.out_root, fork_mode_override=args.fork_mode)
            print(f"[{perm_dir.name}] wrote {(args.out_root / perm_dir.name / 'dma_plan.json')}")
        except Exception as e:
            any_fail = True
            print(f"[{perm_dir.name}] ERROR: {e}")

    return 2 if any_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
