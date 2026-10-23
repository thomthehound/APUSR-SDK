# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""gen_stream_schedule.py

Purpose
-------
Generate a *static* stream cadence plan for the 14-pass pipeline.

Why this exists
---------------
Our topology is a U-Net-like streaming graph with stride-2 downsamples and
upsamples. Even with a fixed token size (bytes/token), the number of tokens
per frame changes at scale transitions, which implies *rate conversion*.

XDNA2 tiles do have a scalar RISC, but it is not the place to do heavy
branching/scheduling logic. The intent is therefore to:

  - compute cadence ratios offline (in Python)
  - keep runtime control as DMA-friendly and as static as possible

This tool reads the generated `graph_plan.json` and emits a
`schedule_plan.json` alongside it.

Outputs
-------
For each permutation, writes:
  models/fsr4_int8/planning/aie_generated/<perm>/schedule_plan.json

The schedule plan is descriptive (not executable) and is meant to inform
how we later build DMA BD chains and/or minimal RISC loops.

Usage
-----
  # One permutation
  python tools/gen/gen_stream_schedule.py out/dispatch_graphs/balanced__2160

  # All permutations
  python tools/gen/gen_stream_schedule.py out/dispatch_graphs --all

Notes
-----
This tool intentionally does *not* attempt to be a cycle-accurate model.
It focuses on integer-friendly ratios and obvious invariants:
  - tokens_per_row ratios (X)
  - row count ratios (Y)
  - total token ratios per frame

Those are the pieces we need to build a clean, conventional streaming pipe.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def _load_json(p: Path) -> Dict[str, Any]:
    return json.loads(p.read_text(encoding="utf-8"))


def _dump_json(p: Path, obj: Dict[str, Any]) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _iter_perm_dirs(root_or_perm: Path, *, all_perms: bool) -> Iterable[Path]:
    """Iterate permutation dirs given either a single perm dir or the root."""
    if root_or_perm.is_file():
        yield root_or_perm.parent
        return

    if (root_or_perm / "dispatch_graph.json").exists():
        yield root_or_perm
        return

    if not all_perms:
        all_perms = True

    for p in sorted(root_or_perm.rglob("dispatch_graph.json")):
        yield p.parent


def _repo_root_from_this_file() -> Path:
    # tools/gen/.. /.. -> repo
    return Path(__file__).resolve().parents[2]


def _generated_dir_for_perm(perm_dir: Path) -> Path:
    """Return models/fsr4_int8/planning/aie_generated/<perm> for a dispatch_graphs/<perm> dir."""
    repo = _repo_root_from_this_file()
    return repo / "models" / "fsr4_int8" / "planning" / "aie_generated" / perm_dir.name


def _reduce_frac(num: int, den: int) -> Tuple[int, int]:
    if den == 0:
        raise ZeroDivisionError("fraction denominator is zero")
    if num == 0:
        return 0, 1
    g = math.gcd(abs(num), abs(den))
    num //= g
    den //= g
    if den < 0:
        num *= -1
        den *= -1
    return num, den


@dataclass(frozen=True)
class ShapeSummary:
    W: int
    H: int
    tokens_per_row: int
    total_tokens: int
    effective_C: int
    elem_bytes: int


def _shape_summary(d: Dict[str, Any]) -> ShapeSummary:
    return ShapeSummary(
        W=int(d["W"]),
        H=int(d["H"]),
        tokens_per_row=int(d["tokens_per_row"]),
        total_tokens=int(d["total_tokens"]),
        effective_C=int(d["effective_C"]),
        elem_bytes=int(d["elem_bytes"]),
    )


def _cadence_from_primary(primary: ShapeSummary, out: ShapeSummary) -> Dict[str, Any]:
    """Compute integer-friendly cadence ratios from primary input to output."""
    x_num, x_den = _reduce_frac(out.tokens_per_row, primary.tokens_per_row)
    y_num, y_den = _reduce_frac(out.H, primary.H)
    t_num, t_den = _reduce_frac(out.total_tokens, primary.total_tokens)

    # Convenience representation for stride-2 style patterns.
    # (These are hints; they are not the only possible schedules.)
    macro: Dict[str, Any] = {}

    # Downsample patterns
    if x_num == 1 and x_den == 2 and y_num == 1 and y_den == 2:
        macro = {"kind": "down2x2", "in_macro": [2, 2], "out_macro": [1, 1]}
    elif x_num == 1 and x_den == 1 and y_num == 1 and y_den == 2:
        macro = {"kind": "down2y", "in_macro": [1, 2], "out_macro": [1, 1]}

    # Upsample patterns
    elif x_num == 2 and x_den == 1 and y_num == 2 and y_den == 1:
        macro = {"kind": "up2x2", "in_macro": [1, 1], "out_macro": [2, 2]}
    elif x_num == 1 and x_den == 1 and y_num == 2 and y_den == 1:
        macro = {"kind": "up2y", "in_macro": [1, 1], "out_macro": [1, 2]}

    return {
        "x_ratio": {"num": x_num, "den": x_den},
        "y_ratio": {"num": y_num, "den": y_den},
        "tokens_ratio": {"num": t_num, "den": t_den},
        "macro_hint": macro or None,
    }


def _plan_one_perm(perm_dir: Path) -> Tuple[Path, Dict[str, Any]]:
    gen_dir = _generated_dir_for_perm(perm_dir)
    gp_path = gen_dir / "graph_plan.json"
    if not gp_path.exists():
        raise FileNotFoundError(
            f"Missing graph_plan.json for {perm_dir.name}. Run 'gen-aie' first: {gp_path}"
        )

    gp = _load_json(gp_path)
    stages = gp.get("stages") or []
    if not stages:
        raise ValueError(f"graph_plan.json has no stages: {gp_path}")

    warnings: List[str] = []
    stage_rows: List[Dict[str, Any]] = []

    for s in stages:
        pidx = int(s.get("pass"))
        kind = str(s.get("kind"))

        primary_d = (s.get("inputs") or {}).get("primary")
        out_d = s.get("output")
        if primary_d is None or out_d is None:
            raise ValueError(f"stage {pidx} missing primary input or output")

        primary = _shape_summary(primary_d)
        out = _shape_summary(out_d)
        cadence = _cadence_from_primary(primary, out)

        skip_d = (s.get("inputs") or {}).get("skip")
        skip_summary: Optional[ShapeSummary] = None
        if skip_d is not None:
            skip_summary = _shape_summary(skip_d)
            # For *_ADD stages, skip should match output shape in token-space.
            if skip_summary.tokens_per_row != out.tokens_per_row or skip_summary.H != out.H:
                warnings.append(
                    f"pass {pidx}: skip shape (H={skip_summary.H}, tpr={skip_summary.tokens_per_row}) "
                    f"does not match output (H={out.H}, tpr={out.tokens_per_row})"
                )

        notes = list(s.get("notes") or [])
        stage_rows.append(
            {
                "pass": pidx,
                "kind": kind,
                "tile": s.get("tile"),
                "primary": {
                    "H": primary.H,
                    "tokens_per_row": primary.tokens_per_row,
                    "total_tokens": primary.total_tokens,
                    "effective_C": primary.effective_C,
                    "elem_bytes": primary.elem_bytes,
                },
                "skip": None
                if skip_summary is None
                else {
                    "H": skip_summary.H,
                    "tokens_per_row": skip_summary.tokens_per_row,
                    "total_tokens": skip_summary.total_tokens,
                    "effective_C": skip_summary.effective_C,
                    "elem_bytes": skip_summary.elem_bytes,
                },
                "output": {
                    "H": out.H,
                    "tokens_per_row": out.tokens_per_row,
                    "total_tokens": out.total_tokens,
                    "effective_C": out.effective_C,
                    "elem_bytes": out.elem_bytes,
                },
                "cadence": cadence,
                "notes": notes,
            }
        )

    plan = {
        "schema": "com.apusr.stream_schedule_plan.v0",
        "permutation": str(gp.get("permutation")),
        "fork_mode": str(gp.get("fork_mode")),
        "token_contract": gp.get("token_contract"),
        "tile_mapping": gp.get("tile_mapping"),
        "assumptions": [
            "Cadence is expressed in token-space ratios only (not cycle-accurate).",
            "Implementation should prefer DMA-friendly static schedules; avoid heavy scalar control flow on tile RISC.",
            "Rate conversion is handled inside stride-2 stages using conventional line buffers and phased emission.",
        ],
        "warnings": warnings,
        "stages": stage_rows,
    }

    out_path = gen_dir / "schedule_plan.json"
    return out_path, plan


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate stream cadence plan from graph_plan.json")
    ap.add_argument(
        "path",
        type=Path,
        help="Permutation dir under models/fsr4_int8/planning/dispatch_graphs, or that root with --all",
    )
    ap.add_argument("--all", action="store_true", help="Generate for all permutations under PATH")
    ap.add_argument("--quiet", action="store_true", help="Do not print per-permutation summaries")
    args = ap.parse_args()

    perm_dirs = list(_iter_perm_dirs(args.path, all_perms=args.all))
    if not perm_dirs:
        raise SystemExit(f"No permutations found under: {args.path}")

    any_warnings = 0

    for perm in perm_dirs:
        out_path, plan = _plan_one_perm(perm)
        _dump_json(out_path, plan)
        if plan.get("warnings"):
            any_warnings += 1
        if not args.quiet:
            w = len(plan.get("warnings") or [])
            print(f"[{perm.name}] wrote {out_path.relative_to(_repo_root_from_this_file())} (warnings={w})")

    # Non-zero exit if we emitted any warnings: we want to notice cadence mismatches early.
    return 0 if any_warnings == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
