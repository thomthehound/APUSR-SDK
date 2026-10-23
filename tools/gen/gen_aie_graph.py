# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""gen_aie_graph.py

Purpose
-------
Generate an initial *streaming topology skeleton* for the FSR4 14-pass pipeline.

This generator is intentionally split from the compiler toolchain:

  - Primary output is a `graph_plan.json` describing stages, tile placement,
    stream links, and FIFO usage.
  - Secondary output is a *starter* MLIR-AIE-like topology file
    (`fsr4_skeleton_topology.mlir`) that is meant to be a hand-editable template.

The goal is to unblock “next steps” by making the topology explicit and stable
early, while acknowledging that the exact MLIR-AIE/XRT plumbing will iterate.

Usage
-----
  # Generate for one permutation (expects you already ran graph_lifetime)
  python tools/gen/gen_aie_graph.py out/dispatch_graphs/balanced__2160

  # Generate for all permutations
  python tools/gen/gen_aie_graph.py out/dispatch_graphs --all

  # Match FIFO fork behavior used by gen_memtile_fifos.py
  python tools/gen/gen_aie_graph.py out/dispatch_graphs/balanced__2160 --fork-mode memtile_tap
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def _load_json(p: Path) -> Dict[str, Any]:
    return json.loads(p.read_text(encoding="utf-8"))

def _physical_origin(tile_mapping: Dict[str, Any]) -> Tuple[int, int]:
    """Return (col,row) offset mapping logical slab coords -> physical device coords.

    Generators keep the *logical* 4×4 mapping stable and apply this offset to
    produce physical coordinates that can later match the real device array.
    """
    rs = tile_mapping.get("reserved_slab") or {}
    po = rs.get("physical_origin") or {}
    return int(po.get("col", 0)), int(po.get("row", 0))



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


def _effective_channels(token_contract: Dict[str, Any], *, tensor_name: str, tensor_type: str, logical_c: int) -> int:
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
class TensorShape:
    name: str
    tensor_type: str
    W: int
    H: int
    C: int
    elem_bytes: int
    effective_C: int
    token_width_px: int
    tokens_per_row: int
    total_tokens: int


def _shape_from_tensor(
    tensors: Dict[str, Any],
    token_contract: Dict[str, Any],
    *,
    tensor_name: str,
    token_bytes: int,
) -> TensorShape:
    td = tensors.get(tensor_name)
    if td is None:
        raise KeyError(f"Unknown tensor: {tensor_name}")
    ls = td.get("logical_size")
    if not isinstance(ls, list) or len(ls) != 3:
        raise ValueError(f"Bad logical_size for tensor {tensor_name}")
    W, H, C = int(ls[0]), int(ls[1]), int(ls[2])
    tensor_type = str(td.get("tensor_type"))
    elem_bytes = int(td.get("elem_size_bytes"))

    eff_C = _effective_channels(token_contract, tensor_name=tensor_name, tensor_type=tensor_type, logical_c=C)
    token_w = _token_width_px(token_bytes, eff_C, elem_bytes)
    if W % token_w != 0:
        raise ValueError(f"W={W} not divisible by token_width={token_w} for tensor {tensor_name}")
    tpr = W // token_w
    total = tpr * H
    return TensorShape(
        name=tensor_name,
        tensor_type=tensor_type,
        W=W,
        H=H,
        C=C,
        elem_bytes=elem_bytes,
        effective_C=eff_C,
        token_width_px=token_w,
        tokens_per_row=tpr,
        total_tokens=total,
    )


def _build_in_edges(dg: Dict[str, Any]) -> Dict[int, List[Dict[str, Any]]]:
    edges = (dg.get("dataflow") or {}).get("edges") or []
    in_edges: Dict[int, List[Dict[str, Any]]] = {}
    for e in edges:
        dst = int(e.get("dst"))
        in_edges.setdefault(dst, []).append(e)
    for k in in_edges:
        in_edges[k].sort(key=lambda x: (int(x.get("src")), str(x.get("tensor"))))
    return in_edges


def _stage_kind(op_fn: str) -> str:
    # We only need a coarse classification for a skeleton.
    if op_fn in ("Conv2D_k2s2b", "FusedConv2D_k2s2b_QuantizedOutput"):
        return "downsample_k2s2"
    if op_fn in ("FNB_CT2D_ADD",):
        return "upsample_ct2d_add"
    if op_fn in ("CNB_CT2D",):
        return "upsample_ct2d"
    return "same_scale"


@dataclass(frozen=True)
class Stage:
    pass_index: int
    op_fn: str
    kind: str
    tile_col: int
    tile_row: int
    phys_tile_col: int
    phys_tile_row: int
    runtime_inputs: List[str]
    outputs: List[str]
    primary_in_tensor: Optional[TensorShape]
    skip_in_tensor: Optional[TensorShape]
    out_tensor: TensorShape


def _load_tile_placement(tile_mapping: Dict[str, Any]) -> Dict[int, Tuple[int, int]]:
    tiles = (tile_mapping.get("placement") or {}).get("tiles") or []
    out: Dict[int, Tuple[int, int]] = {}
    for t in tiles:
        out[int(t.get("pass"))] = (int(t.get("col")), int(t.get("row")))
    return out


def _load_fifo_plan(perm_dir: Path) -> Optional[Dict[str, Any]]:
    p = perm_dir / ".." / ".."  # retained placeholder; FIFO plans are loaded from the selected output root
    return None


def _read_fifo_plan(generated_dir: Path) -> Optional[Dict[str, Any]]:
    fp = generated_dir / "fifo_plan.json"
    if not fp.exists():
        return None
    return _load_json(fp)


def _plan_for_perm(
    perm_dir: Path,
    *,
    token_contract_path: Path,
    tile_mapping_path: Path,
    out_dir: Path,
    fork_mode: str,
) -> Dict[str, Any]:
    dg = _load_json(_require(perm_dir / "dispatch_graph.json", "dispatch_graph.json"))
    token_contract = _load_json(_require(token_contract_path, "token contract JSON"))
    tile_mapping = _load_json(_require(tile_mapping_path, "tile mapping JSON"))
    origin_col, origin_row = _physical_origin(tile_mapping)
    tensors = dg.get("tensors") or {}
    passes = dg.get("passes") or []
    if not passes:
        raise ValueError("dispatch_graph.json has no passes")

    token_bytes = int(token_contract.get("token_bytes"))

    # Optional: incorporate fifo plan if it was generated already.
    fifo_plan = _read_fifo_plan(out_dir)

    placement = _load_tile_placement(tile_mapping)
    in_edges = _build_in_edges(dg)

    stages: List[Stage] = []
    for op in passes:
        idx = int(op.get("pass_index"))
        op_fn = str(op.get("op_fn"))
        kind = _stage_kind(op_fn)
        if idx not in placement:
            raise ValueError(f"tile_mapping is missing placement for pass {idx}")
        col, row = placement[idx]
        phys_col = col + origin_col
        phys_row = row + origin_row
        runtime_inputs = list((op.get("pass_io") or {}).get("runtime_inputs") or [])
        outputs = list((op.get("pass_io") or {}).get("outputs") or [])
        if len(outputs) != 1:
            raise ValueError(f"pass {idx} has {len(outputs)} outputs (expected 1): {outputs}")
        out_t = _shape_from_tensor(tensors, token_contract, tensor_name=outputs[0], token_bytes=token_bytes)

        # Infer primary/skip inputs from topology rather than trusting runtime_inputs ordering.
        ins = in_edges.get(idx) or []
        primary_name: Optional[str] = None
        skip_name: Optional[str] = None

        if idx == 0:
            # Boundary input comes from host.
            if len(runtime_inputs) != 1:
                raise ValueError("pass0 expected exactly one runtime input")
            primary_name = runtime_inputs[0]
        else:
            if len(ins) == 1:
                primary_name = str(ins[0]["tensor"])
            elif len(ins) == 2:
                # Merges: we treat the edge from the immediately previous pass as primary.
                # Everything else is skip.
                srcs = sorted([(int(e["src"]), str(e["tensor"])) for e in ins], key=lambda x: x[0])
                # Heuristic: the larger src index is the "main" bottleneck path.
                primary_name = srcs[-1][1]
                skip_name = srcs[0][1]
            else:
                raise ValueError(f"pass {idx} has {len(ins)} incoming edges (expected 1 or 2)")

        primary_t = _shape_from_tensor(tensors, token_contract, tensor_name=primary_name, token_bytes=token_bytes) if primary_name else None
        skip_t = _shape_from_tensor(tensors, token_contract, tensor_name=skip_name, token_bytes=token_bytes) if skip_name else None

        stages.append(
            Stage(
                pass_index=idx,
                op_fn=op_fn,
                kind=kind,
                tile_col=col,
                tile_row=row,
                phys_tile_col=phys_col,
                phys_tile_row=phys_row,
                runtime_inputs=runtime_inputs,
                outputs=outputs,
                primary_in_tensor=primary_t,
                skip_in_tensor=skip_t,
                out_tensor=out_t,
            )
        )

    # Build explicit links (main chain + skips). We express endpoints as (node, port).
    # Node names: stage<P>, fifo<name>.
    links: List[Dict[str, Any]] = []

    # Canonical skip identification based on pass indices.
    skip_by_pair = {
        (2, 11): "skipA",
        (5, 9): "skipB",
    }

    # Helper: add link
    def add_link(src: str, src_port: str, dst: str, dst_port: str, *, kind: str, note: str = "") -> None:
        links.append({
            "kind": kind,
            "src": {"node": src, "port": src_port},
            "dst": {"node": dst, "port": dst_port},
            "note": note,
        })

    # Main chain: 0->1->2->...->13, with optional memtile tap substitution.
    for i in range(0, len(stages) - 1):
        a = stages[i].pass_index
        b = stages[i + 1].pass_index

        # Fork points: pass2->pass3 and pass5->pass6.
        if fork_mode == "memtile_tap" and (a, b) in ((2, 3), (5, 6)):
            fifo_name = skip_by_pair[(2, 11)] if (a, b) == (2, 3) else skip_by_pair[(5, 9)]
            add_link(f"stage{a}", "out", f"fifo{fifo_name}", "in", kind="stream", note="write once into memtile FIFO")
            add_link(f"fifo{fifo_name}", "tap_main", f"stage{b}", "in", kind="stream", note="immediate tap")
        else:
            add_link(f"stage{a}", "out", f"stage{b}", "in", kind="stream", note="main chain")

    # Skip links: producer->fifo->consumer
    for (src_pass, dst_pass), fifo_name in skip_by_pair.items():
        if fork_mode == "memtile_tap":
            # Producer already links to fifo in the main chain rewrite above.
            add_link(f"fifo{fifo_name}", "tap_skip", f"stage{dst_pass}", "skip_in", kind="stream", note="delayed skip tap")
        else:
            add_link(f"stage{src_pass}", "out", f"fifo{fifo_name}", "in", kind="stream", note="requires fabric multicast/dup")
            add_link(f"fifo{fifo_name}", "skip_only", f"stage{dst_pass}", "skip_in", kind="stream", note="delayed skip")

    plan: Dict[str, Any] = {
        "schema": "com.apusr.aie_graph_plan.v0",
        "permutation": perm_dir.name,
        "fork_mode": fork_mode,
        "token_contract": {
            "path": str(token_contract_path.as_posix()),
            "token_bytes": token_bytes,
            "beat_bytes": int(token_contract.get("beat_bytes", 8)),
            # Include boundary rules inline so downstream generators don't need to re-open the
            # decisions file just to decide how to treat C7->C8, etc.
            "boundary_rules": token_contract.get("boundary_rules") or {},
        },
        "tile_mapping": {
            "path": str(tile_mapping_path.as_posix()),
            "reserved_slab": tile_mapping.get("reserved_slab"),
            "utility_tiles": (tile_mapping.get("placement") or {}).get("utility_tiles") or [],
        },
        "stages": [
            {
                "pass": s.pass_index,
                "op_fn": s.op_fn,
                "kind": s.kind,
                "tile": {"col": s.phys_tile_col, "row": s.phys_tile_row, "logical_col": s.tile_col, "logical_row": s.tile_row},
                "inputs": {
                    "primary": None if s.primary_in_tensor is None else s.primary_in_tensor.__dict__,
                    "skip": None if s.skip_in_tensor is None else s.skip_in_tensor.__dict__,
                },
                "output": s.out_tensor.__dict__,
                "notes": (
                    ["boundary_input_channel_padding: C%d -> C%d (assumed memtile MM2S constant padding)" % (s.primary_in_tensor.C, s.primary_in_tensor.effective_C)]
                    if (s.pass_index == 0 and s.primary_in_tensor is not None and s.primary_in_tensor.C != s.primary_in_tensor.effective_C)
                    else []
                ),
            }
            for s in stages
        ],
        "links": links,
        "fifo_plan": fifo_plan,
        "warnings": [],
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "graph_plan.json").write_text(json.dumps(plan, indent=2, sort_keys=True), encoding="utf-8")
    _write_topology_mlir(plan, out_dir)
    _write_kernel_skeletons(plan, out_dir)
    return plan


def _write_topology_mlir(plan: Dict[str, Any], out_dir: Path) -> None:
    """Write a hand-editable MLIR-AIE-style topology template.

    We avoid being too "clever" here because the exact dialect details/tooling
    will evolve (and XDNA2 != Versal AIE in a few important ways).

    This file is meant to be an *authoritative wiring diagram* that can be kept
    close to the generated JSON plan.
    """

    perm = plan["permutation"]
    fork_mode = plan["fork_mode"]
    token_bytes = plan["token_contract"]["token_bytes"]

    lines: List[str] = []
    lines.append(f"// Auto-generated: tools/gen/gen_aie_graph.py")
    lines.append(f"// Permutation: {perm}")
    lines.append(f"// fork_mode: {fork_mode}")
    lines.append(f"// token_bytes: {token_bytes}")
    lines.append("//")
    lines.append("// NOTE:")
    lines.append("// - Tile coordinates are PHYSICAL (logical + reserved_slab.physical_origin).")
    lines.append("// - Memtile coordinates/rows are placeholders.")
    lines.append("// - This file is a wiring template; JSON is the source of truth.")
    lines.append("")

    lines.append("module {")
    lines.append("  // Replace with the correct device op once the toolchain is selected.")
    lines.append("  // aie.device(xdna2) { ... }")
    lines.append("")

    lines.append("  // === Compute tiles (logical) ===")
    for s in plan["stages"]:
        t = s["tile"]
        lines.append(f"  // stage{s['pass']:02d} {s['op_fn']} @ tile({t['col']},{t['row']}) logical=({t.get('logical_col')},{t.get('logical_row')}) kind={s['kind']}")
    lines.append("")

    lines.append("  // === Memtile FIFOs (logical) ===")
    fifo_plan = plan.get("fifo_plan") or {}
    for f in (fifo_plan.get("fifos") or []):
        m = f.get("memtile") or {}
        lines.append(
            f"  // fifo{f['name']}: tensor={f['tensor']} ring={f['ring_bytes']}B token_w={f['token_width_px']}px memtile_col={m.get('col')} logical_col={m.get('logical_col')}"
        )
    if not (fifo_plan.get("fifos") or []):
        lines.append("  // (fifo_plan.json not found; run gen_memtile_fifos.py first)")
    lines.append("")

    lines.append("  // === Links ===")
    for l in plan["links"]:
        s = l["src"]
        d = l["dst"]
        note = l.get("note") or ""
        lines.append(f"  // {l['kind']}: {s['node']}:{s['port']}  ->  {d['node']}:{d['port']}    {note}")

    lines.append("}")
    (out_dir / "fsr4_skeleton_topology.mlir").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_kernel_skeletons(plan: Dict[str, Any], out_dir: Path) -> None:
    """Emit placeholder per-stage kernel stubs.

    These are deliberately minimalist and avoid runtime pointer math:
    - all loop bounds are compile-time constants per permutation
    - each token is a fixed number of 64-bit beats

    The stubs are *not* guaranteed to compile against your current AIE headers
    yet; they are meant as a concrete starting point that makes the token/row
    cadence explicit.
    """

    token_bytes = int(plan["token_contract"]["token_bytes"])
    beat_bytes = int(plan["token_contract"].get("beat_bytes", 8))
    beats_per_token = token_bytes // beat_bytes

    kdir = out_dir / "kernels"
    kdir.mkdir(parents=True, exist_ok=True)

    common_h = kdir / "skeleton_stream_io.h"
    if not common_h.exists():
        common_h.write_text(
            """// skeleton_stream_io.h
//
// This header is intentionally tiny: it defines *abstract* stream read/write
// helpers so the generated kernels have a consistent shape.
//
// When you wire these kernels into a real AIE toolchain, replace the bodies of
// read_beat()/write_beat() with the correct AIE stream intrinsics.

#pragma once

#include <cstdint>

struct InStream  { /* toolchain-specific */ };
struct OutStream { /* toolchain-specific */ };

static inline uint64_t read_beat(InStream* /*s*/) {
  // TODO: replace with AIE intrinsic (e.g., readincr)
  return 0;
}

static inline void write_beat(OutStream* /*s*/, uint64_t /*v*/) {
  // TODO: replace with AIE intrinsic (e.g., writeincr)
}
""",
            encoding="utf-8",
        )

    for s in plan["stages"]:
        p = int(s["pass"])
        kind = str(s["kind"])
        op_fn = str(s["op_fn"])
        out_shape = s["output"]
        out_rows = int(out_shape["H"])
        out_tpr = int(out_shape["tokens_per_row"])

        # The skeleton behavior is defined in terms of *row cadence*.
        # We keep the loops explicit for readability.
        if kind == "downsample_k2s2":
            in_rows = int(s["inputs"]["primary"]["H"])
            in_tpr = int(s["inputs"]["primary"]["tokens_per_row"])
            body = _kernel_body_downsample(beats_per_token, in_rows, in_tpr, out_rows, out_tpr)
        elif kind == "upsample_ct2d_add":
            in_rows = int(s["inputs"]["primary"]["H"])
            in_tpr = int(s["inputs"]["primary"]["tokens_per_row"])
            skip_rows = int(s["inputs"]["skip"]["H"])
            skip_tpr = int(s["inputs"]["skip"]["tokens_per_row"])
            body = _kernel_body_upsample_merge(beats_per_token, in_rows, in_tpr, out_rows, out_tpr, skip_rows, skip_tpr)
        elif kind == "upsample_ct2d":
            in_rows = int(s["inputs"]["primary"]["H"])
            in_tpr = int(s["inputs"]["primary"]["tokens_per_row"])
            body = _kernel_body_upsample(beats_per_token, in_rows, in_tpr, out_rows, out_tpr)
        else:
            in_rows = int(s["inputs"]["primary"]["H"])
            in_tpr = int(s["inputs"]["primary"]["tokens_per_row"])
            body = _kernel_body_passthrough(beats_per_token, in_rows, in_tpr)

        src = "\n".join(
            [
                f"// Auto-generated kernel stub for stage{p:02d} ({op_fn})",
                f"// kind={kind}",
                f"// token_bytes={token_bytes} beats_per_token={beats_per_token}",
                "",
                "#include \"skeleton_stream_io.h\"",
                "",
                f"extern \"C\" void stage{p:02d}(InStream* in0, InStream* in1, OutStream* out0) {{",
                body,
                "}",
                "",
            ]
        )

        (kdir / f"stage{p:02d}_{kind}.cc").write_text(src, encoding="utf-8")


def _kernel_body_passthrough(beats_per_token: int, rows: int, tokens_per_row: int) -> str:
    return "\n".join(
        [
            f"  // same_scale passthrough: {rows} rows × {tokens_per_row} tokens/row",
            f"  constexpr int ROWS = {rows};",
            f"  constexpr int TOKENS_PER_ROW = {tokens_per_row};",
            f"  constexpr int BEATS_PER_TOKEN = {beats_per_token};",
            "  (void)in1; // unused",
            "  for (int y = 0; y < ROWS; ++y) {",
            "    for (int t = 0; t < TOKENS_PER_ROW; ++t) {",
            "      // Copy one token (fixed-length payload).",
            "      for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "        uint64_t v = read_beat(in0);",
            "        write_beat(out0, v);",
            "      }",
            "    }",
            "  }",
        ]
    )


def _kernel_body_downsample(
    beats_per_token: int,
    in_rows: int,
    in_tpr: int,
    out_rows: int,
    out_tpr: int,
) -> str:
    return "\n".join(
        [
            f"  // downsample_k2s2 skeleton: forward even rows, discard odd rows",
            f"  // in:  {in_rows} rows × {in_tpr} tokens/row",
            f"  // out: {out_rows} rows × {out_tpr} tokens/row",
            f"  constexpr int IN_ROWS = {in_rows};",
            f"  constexpr int IN_TOKENS_PER_ROW = {in_tpr};",
            f"  constexpr int OUT_ROWS = {out_rows};",
            f"  constexpr int OUT_TOKENS_PER_ROW = {out_tpr};",
            f"  constexpr int BEATS_PER_TOKEN = {beats_per_token};",
            "  (void)in1; // unused",
            "  // Consume 2 input rows per output row.",
            "  for (int y_out = 0; y_out < OUT_ROWS; ++y_out) {",
            "    // Even row: forward.",
            "    for (int t = 0; t < OUT_TOKENS_PER_ROW; ++t) {",
            "      for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "        uint64_t v = read_beat(in0);",
            "        write_beat(out0, v);",
            "      }",
            "    }",
            "    // Odd row: discard.",
            "    for (int t = 0; t < IN_TOKENS_PER_ROW; ++t) {",
            "      for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "        (void)read_beat(in0);",
            "      }",
            "    }",
            "  }",
            "  // IN_ROWS is expected to be exactly 2*OUT_ROWS.",
            "  (void)IN_ROWS;",
        ]
    )


def _kernel_body_upsample(
    beats_per_token: int,
    in_rows: int,
    in_tpr: int,
    out_rows: int,
    out_tpr: int,
) -> str:
    return "\n".join(
        [
            f"  // upsample_ct2d skeleton: duplicate each input row",
            f"  // in:  {in_rows} rows × {in_tpr} tokens/row",
            f"  // out: {out_rows} rows × {out_tpr} tokens/row",
            f"  constexpr int IN_ROWS = {in_rows};",
            f"  constexpr int TOKENS_PER_ROW_IN = {in_tpr};",
            f"  constexpr int OUT_ROWS = {out_rows};",
            f"  constexpr int TOKENS_PER_ROW_OUT = {out_tpr};",
            f"  constexpr int BEATS_PER_TOKEN = {beats_per_token};",
            "  (void)in1; // unused",
            "  // Skeleton approach: buffer exactly one *row* so we can emit it twice.",
            "  // Real kernels will likely use line buffers / pipelining to reduce storage.",
            "  uint64_t rowbuf[TOKENS_PER_ROW_OUT][BEATS_PER_TOKEN];",
            "  for (int y_in = 0; y_in < IN_ROWS; ++y_in) {",
            "    // Read one input row.",
            "    for (int t = 0; t < TOKENS_PER_ROW_OUT; ++t) {",
            "      for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "        rowbuf[t][b] = read_beat(in0);",
            "      }",
            "    }",
            "    // Emit it twice as two output rows.",
            "    for (int rep = 0; rep < 2; ++rep) {",
            "      for (int t = 0; t < TOKENS_PER_ROW_OUT; ++t) {",
            "        for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "          write_beat(out0, rowbuf[t][b]);",
            "        }",
            "      }",
            "    }",
            "  }",
            "  (void)TOKENS_PER_ROW_IN; // expected to match TOKENS_PER_ROW_OUT for v0 tokens",
        ]
    )


def _kernel_body_upsample_merge(
    beats_per_token: int,
    in_rows: int,
    in_tpr: int,
    out_rows: int,
    out_tpr: int,
    skip_rows: int,
    skip_tpr: int,
) -> str:
    return "\n".join(
        [
            f"  // upsample_ct2d_add skeleton: duplicate each input row; drain skip at output rate",
            f"  // main in:  {in_rows} rows × {in_tpr} tokens/row",
            f"  // skip in:  {skip_rows} rows × {skip_tpr} tokens/row",
            f"  // out:      {out_rows} rows × {out_tpr} tokens/row",
            f"  constexpr int IN_ROWS = {in_rows};",
            f"  constexpr int TOKENS_PER_ROW_IN = {in_tpr};",
            f"  constexpr int OUT_ROWS = {out_rows};",
            f"  constexpr int TOKENS_PER_ROW_OUT = {out_tpr};",
            f"  constexpr int SKIP_ROWS = {skip_rows};",
            f"  constexpr int SKIP_TOKENS_PER_ROW = {skip_tpr};",
            f"  constexpr int BEATS_PER_TOKEN = {beats_per_token};",
            "  // Expect skip stream to already be at OUT resolution.",
            "  (void)IN_ROWS;",
            "  // Skeleton approach: buffer one main input row, then emit it twice while draining skip.",
            "  uint64_t rowbuf[TOKENS_PER_ROW_OUT][BEATS_PER_TOKEN];",
            "  for (int y_in = 0; y_in < IN_ROWS; ++y_in) {",
            "    // Read one main input row (token count per row should match output for v0 tokens).",
            "    for (int t = 0; t < TOKENS_PER_ROW_OUT; ++t) {",
            "      for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "        rowbuf[t][b] = read_beat(in0);",
            "      }",
            "    }",
            "    // Emit two output rows (duplicate), draining skip at output cadence.",
            "    for (int rep = 0; rep < 2; ++rep) {",
            "      for (int t = 0; t < TOKENS_PER_ROW_OUT; ++t) {",
            "        for (int b = 0; b < BEATS_PER_TOKEN; ++b) {",
            "          (void)read_beat(in1); // drain skip beat",
            "          write_beat(out0, rowbuf[t][b]);",
            "        }",
            "      }",
            "    }",
            "  }",
            "  // Informative only (compile-time constants):",
            "  (void)TOKENS_PER_ROW_IN;",
            "  (void)SKIP_ROWS;",
            "  (void)SKIP_TOKENS_PER_ROW;",
        ]
    )


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Generate AIE streaming topology skeleton (graph_plan + MLIR template)")
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
        help="Must match gen_memtile_fifos.py (see that help for meaning)",
    )
    ap.add_argument(
        "--token-contract",
        type=Path,
        default=Path("docs/decisions/token_contract_v0.json"),
        help="Path to token contract JSON",
    )
    ap.add_argument(
        "--tile-mapping",
        type=Path,
        default=Path("docs/decisions/tile_mapping_v0.json"),
        help="Path to tile mapping JSON",
    )
    ap.add_argument(
        "--out-root",
        type=Path,
        default=Path("models/fsr4_int8/planning/aie_generated"),
        help="Where to write outputs (default: models/fsr4_int8/planning/aie_generated)",
    )
    args = ap.parse_args(argv)

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
                token_contract_path=args.token_contract,
                tile_mapping_path=args.tile_mapping,
                out_dir=out_dir,
                fork_mode=args.fork_mode,
            )
            print(f"[{perm_dir.name}] wrote {out_dir / 'graph_plan.json'}")
        except Exception as e:
            any_fail = True
            print(f"[{perm_dir.name}] ERROR: {e}")

    return 2 if any_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
