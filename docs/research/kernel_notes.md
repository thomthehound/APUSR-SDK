<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Middle-core kernel notes

The product ABI is a complete frame-slot run:

```text
sequence(param_arena, ingress_frame, egress_frame)
load_params(param_arena, ...)
```

The host submits complete frame slots. Internal microtiles, cells, halos, stage
slices, and worker groups are AIE graph details and must not become host-visible
work again.

## Current kernel status

The non-product `apusr_middle_core_stage1_12_full_arena_2col` graph is quarantined under
`kernels/quarantine/apusr_middle_core_stage1_12_full_arena_2col_failed_v0`.
It is retained only as reference material.

Active replacement profiles:

```text
kernels/apusr_middle_core_stage1_12_spatial2col
kernels/apusr_middle_core_stage1_12_spatial4col
```

Both are resource-budgeted rewrites. The default `spatial-core` graph is the
Stage1-4 milestone spine. It emits the paired Stage1+2 macroblock producer:
one shared `34x18xC16` Stage1 patch from a `36x20xC16` source macroblock,
four Stage2 C16 cells, a compact `2x2` C16 handoff to split Stage3, and a
production-time SkipA arg6 tee into the combined SkipA/SkipB spill arena. C32
and C64 internal cells are `m4x8` block-major, not row-major.
`front-c16-row`, `cell-conveyor`, and `c16-halo` are diagnostics for build
isolation only.

## Rejected design

The quarantined graph got the public frame ABI mostly right, but used full
macro-package objects as normal internal FIFO elements. Several hot edges carried
32-50 KB elements at depth 2. That shape exceeds what a 64 KB compute tile can
safely reserve once stack, scratch, compiler allocation, and neighboring traffic
are considered. The same graph also overloaded bridge/worker cores with unrelated
stage work, increasing program-memory and route pressure.

Do not repair that graph by chasing compiler errors. Salvage the arithmetic
kernels, resident-parameter offsets, and complete-frame-slot ABI. Rewrite the graph
around spatial microtile/cell flow.

The active spatial-core spine uses resident-parameter vector/SRS primitives in
the real Stage1-4 order: fused Stage1+2 first, Stage2 row-block tee to SkipA,
and fused Stage3/4 consuming the Stage2 C16 carrier. The current parameter
buffers are still identity/debug packs in places; they exist to keep the
compiled graph product-shaped while generated model weights are still being
plumbed in. Keep the `m4x8` internal layout as the primitive contract. Do not
reintroduce scalar copy rows as the basis for real middle-core math.

Parameter broadcast staging is intentionally not solved yet. Compact resident
bias/scale arrays are expanded to `m4x8` blocks inside the primitive today.
Pre-expanded resident parameter blocks should be evaluated once the real
middle-row weight packs are active.

## Target fabric shape

The middle-core is a spatial frame engine:

```text
GPU export bookend
  -> capacity-class ingress frame slot
  -> NPU spatial microtile/cell graph
  -> capacity-class egress frame slot
  -> GPU import bookend
```

The four-column profile is the canonical product shape. It gives the graph enough memory-tile roles to keep producer-owned activation rings on-chip without overloading the ingress/skip/egress tiles. The two-column profile remains as a diagnostic compile/audit shape, not the product placement.

`APUSR_MIDDLE_CORE_WORKER_GROUPS=auto` fills the selected target with worker groups. Profile names describe worker width, not total physical columns:

```text
npu1 / 4 exposed columns:
  Spatial2Col -> two diagnostic worker groups
  Spatial4Col -> one product worker

npu2 / 8 exposed columns:
  Spatial2Col -> four diagnostic worker groups
  Spatial4Col -> two product worker groups
```

## Internal work-unit rules

The frame slot may still be capacity/package organized where that helps the GPU
bookends and DMA. The compute graph must not move whole packages as the normal
object-FIFO unit.

Initial hard rules:

- normal hot object-FIFO elements must be 16 KB or smaller;
- preferred hot object-FIFO elements are 4 KB or smaller;
- hot FIFO elements larger than 8 KB must use depth 1;
- conservative compute-tile data reservation is 48 KB before compiler proof;
- each compute tile may link at most two serious math kernels before compiler proof;
- parameters load once per model epoch and stay out of hot frame DMA;
- internal C32/C64 activation cells use `m4x8` block-major layout, so native 4-pixel × 8-channel vector results are contiguous hot-path handoffs;
- halos should be consumer-side views over nearby cells/rings, not full materialized packages;
- memory tiles are the large local arenas; compute-tile FIFOs are short handoff queues.

`tools/analysis/middle_core_resource_budget.py` enforces the first static version
of these rules against the active design contracts. It also checks memory-tile
DMA/lock pressure and the neighbor-sharing model: adjacent memory tiles may form
an aggregate planning group, but each physical memory tile still keeps its own
512 KiB cap.

## Hardware-shaped halo handling

The rewrite treats halos as a DMA view problem, not as package construction. The
front-row diagnostic path uses:

1. a GPU/export-written row-major C16 ingress frame with a one-pixel zero guard border;
2. shim/memory-tile tensor address generation to stream each logical 16×8 cell as an 18×10×C16 halo patch;
3. one compact 2880-byte patch object per front cell, consumed by the vector/SRS C16 spatial-3×3-plus-m4x8-C32-expansion primitive.

The product path must keep that halo policy when the same front primitive is
folded into the Stage1+2 carrier. It has no full-shard C16 row ring, no
left/right seam ObjectFifos, no joined seam stream, no duplicated patch-major
DDR scratch, and no per-cell
host-visible DMA tasks. Memory-tile MM2S zero-padding remains a hardware feature
worth using for later channel/granularity padding cases, but true frame-edge
zeros in the front stage come from the guarded ingress frame.

`tools/analysis/middle_core_spatial_plan.py` is the canonical source for current
cell shapes, halo policies, and per-profile worker budgets. The profile
generators import that plan so the code path starts from the same spatial
contract that the analyzer checks.

## Order of attack

1. Keep the public complete-frame-slot ABI and resident-parameter ABI intact.
2. Compile `spatial-core` on the 2-column and 4-column npu2 profiles after each graph-shape change.
3. Keep both profiles resource-budget clean before invoking MLIR-AIE on later stages.
4. Replace the remaining C32/C64/C32/C16 placeholder rows with real vector/SRS math on the current cell contract, preserving the internal m4x8 activation layout.
5. Compile a single-lane version when placement/debug isolation is needed, then the auto-filled target profile.
6. A/B the 2-column and 4-column shapes on real hardware before choosing the default product artifact.
