<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Middle-core stage placement

`models/fsr4_middle_core_stage_placement_v0.json` is the first-order placement
and fusion/fission plan for the stage ledger. It answers a narrower question
than the ledger:

```text
Given the logical stages and activation shapes, which boundaries look like hard
splits, which look like same-tile candidates, and which profile is still viable
from AM020-class tile-memory and DMA connectivity alone?
```

This is not a timing oracle. It does not claim final xclbin viability,
instruction-memory fit, register pressure, or measured throughput. Generated
MLIR/xclbin evidence is still required before promoting a profile or fusing a
boundary permanently.

## Current profile verdict

`spatial4col` is now the canonical product profile. The Stage1→Stage2
producer-ring work changed this from a taste question into a physical placement
question. Stage 2 needs a row-major C16 producer activation ring from Stage 1.
That ring belongs in memory-tile storage, not compute-tile L1 and not DDR.

The four-column worker gives the product graph separate memory-tile roles:

```text
mem0: ingress / front C16 producer ring
mem1: SkipA
mem2: SkipB / transition staging
mem3: egress
```

On npu1 this means one 4-column worker. On npu2 this means two
4-column worker groups with `APUSR_MIDDLE_CORE_WORKER_GROUPS=auto`.

`spatial2col` remains selectable as a diagnostic profile. It is useful for
small compile proofs and scalar/vector audits, but it is not the product
placement. Its two memory-tile roles couple ingress with SkipA and SkipB with
egress, which leaves no clean home for the C16 producer row ring without
turning the graph back into a scheduling fight.

`Spatial2Col` and `Spatial4Col` name worker widths, not total physical
columns. With `worker_groups=auto`, the 2-column reference profile filled all exposed
columns by replicating a two-column worker. That was useful for pressure
proofs, but it was not the literal two-physical-column product shape originally
intended.

## Physical placement rule

The current `spatial4col` footprint reserves four physical columns, which means
16 compute-tile slots in rows 2-5. That reservation is not permission to clone
whatever two tiles happen to compile. Each physical tile is an individual slot
with an explicit status: active, reserved-empty, or unavailable.

The current Stage1+2 -> Stage3 graph has only two programmed compute tiles:

```text
        col0                       col1             col2             col3
row5   reserved-empty             reserved-empty   reserved-empty   reserved-empty
row4   reserved-empty             reserved-empty   reserved-empty   reserved-empty
row3   Stage3 downsample          reserved-empty   reserved-empty   reserved-empty
row2   Stage1+2 macroblock        reserved-empty   reserved-empty   reserved-empty
row1   memtile                    memtile          memtile          memtile
row0   shim                       shim             shim             shim
```

Compile proofs must instantiate only the active physical tiles. Future stages fill
specific empty slots in this same footprint one at a time. They must not appear
as implicit copies across every reserved column.


## Stage1→Stage2 fused overlap versus DDR spill

The first Stage1→Stage2 ring is no longer the product candidate. The
`stage1_2-fused-overlap-resident` compile proof made the better answer clear:
compute the Stage1-domain `18x10xC16` patch that Stage2 needs from one
`20x12xC16` source-domain window, then run Stage2 immediately.

That does not change the trained model. Stage1 and Stage2 still run in order,
with their original resident parameters and quantization semantics. It also is
not best described as wasted overcompute: those Stage1-domain pixels are exactly
the pixels Stage2 needs. The fused proof simply computes them in one local
footprint instead of writing a full Stage1 activation plane into a memory-tile
ring and reading it back as Stage2 windows.

The deleted boundary matters because the DDR fallback was expensive even for
this first same-scale transition:

```text
Stage1->Stage2 C16 plane DDR write+read at 2160 capacity:
  minimal row-reuse estimate:      about 66 MB/frame
  standalone patch-shaped estimate: about 80 MB/frame
  at 120 FPS:                       about 8-10 GB/s
```

The active early main path is now the paired Stage1+2 macroblock producer:

```text
36x20xC16 source-domain macroblock
  -> shared 34x18 Stage1-domain patch
  -> four Stage2 C16 cells, produced once
  -> compact 2x2 C16 group for Stage3
  -> SkipA arg6 tensor-raster tee
```

Run the current comparison model with:

```text
python tools/analysis/middle_core_profile_tradeoff.py --profile spatial4col --device npu1 --capacity 2160
python tools/analysis/middle_core_profile_tradeoff.py --profile spatial4col --device npu2 --capacity 2160
```

## Fusion rule

Same physical role does not mean one monolithic fused kernel.

For same-scale spatial blocks, the consumer needs a halo of the producer's
output. A fused primitive is only correct if it computes an expanded
intermediate footprint or materializes an internal halo view. Prefer carrying
producer overlap forward through the local handoff or row ring before
synthesizing a new seam halo. Interior seam halos are a last resort, not the
default placement strategy.

That rule applies to these pairs:

```text
1 -> 2     C16 halo of stage 1 output
4 -> 5     C32 halo of stage 4 output
7 -> 8     C64 halo of stage 7 output
9 -> 10    C32 halo of CT2D output
11 -> 12   C16 halo of CT2D output
```

So the default plan is same-role/local placement where useful, but with explicit
internal halo boundaries until expanded-footprint fusion is proven.

## Hard and likely splits

Hard first-product-path splits:

```text
2 -> 3     SkipA source then C16->C32 transition
5 -> 6     SkipB source then C32->C64 transition
8 -> 9     bottleneck to C64->C32 CT2D/SkipB join
10 -> 11   C32 block to C32->C16 CT2D/SkipA join
```

Stage 3/4 has moved from “likely split” to “fused interior path is worth
product-shaping.” The compiled proof is small enough and scalar-clean enough to
keep pushing the fused path, provided the activation context comes from the
upstream C16 producer-overlap row ring. The split Stage-3 downsample proof stays
as the fallback path.

`stage6_7_down_fnb64` remains deliberately splittable until it gets the same
compile evidence.

## Near-term consequence

The next real product target is no longer a Stage1->Stage2 worker ring. The
preferred early path is:

```text
36x20xC16 source-domain macroblock
resident Stage1 hot record
resident Stage2 hot record
shared 34x18 Stage1-domain patch
fused Stage1+2 paired local evaluation
four Stage2 C16 output cells
compact Stage2->Stage3 2x2 C16 cell group
resident Stage3 downsample hot record
C16->C32 downsample
C32 cell out
zero frame-hot parameter DMA
```

`stage1_2-worker-ring-resident`, `stage1_4-worker-ring-resident`, and
`stage1_2-to-stage3_4-worker-ring-resident` remain diagnostic carriers for
tensor-window experiments, but they are no longer the preferred product shape
for the first boundary. `stage1_2-to-stage3-paired-macroblock-resident` is the current
early-pipeline carrier: Stage1+2 materializes one shared `34x18xC16` Stage1-domain
patch from a `36x20xC16` source macroblock, reuses that patch to produce the
four Stage2-output C16 cells needed by one Stage3 k2/s2 output cell, tees those
cells to SkipA, and passes a compact 2x2 C16 group to the adjacent Stage3 tile through shared tile memory.
Stage4 is split from this path; the C32 halo ring is the next boundary to wire.

The SkipA tap should not be implemented as a scalar eviction callback. The
cheap control shape is to tee Stage2 cells to the long-lived skip store as
those cells are produced for the Stage2->Stage3 group. Ring eviction then
becomes ordinary slot reuse: before a slot is overwritten, the cells have
already been made visible to both the next-stage context group and the
long-lived skip path. The fallback is a bounded group spill before slot reuse,
not per-cell eviction work.

## Worker-local ring budgeting

Memory budgeting must use Phoenix/npu1 as the floor. On npu1, one independent
4-column worker covers the full seam width. On npu2/Strix, `auto` creates two
independent 4-column workers and each walks a vertical stripe, which halves the
per-worker ring width. The easier npu2 numbers must not become the product
budget.

The planning tool now reports both the full per-worker ring and the per-memory
tile shard inside a 4-column worker. That distinction matters because the
Stage2->Stage3/4 C16 ring is about 615 KB for the full-width Phoenix worker,
which is too large for one memory tile, but about 155 KB per column shard when
spread across the four memory tiles in the worker. That is the physical shape we
should target before considering a DDR spill.

Current product-ring checks:

```text
python tools/analysis/middle_core_activation_ring_budget.py --profile spatial4col --device npu1 --capacity 2160 --require-all-product-rings-fit --require-worker-independence
python tools/analysis/middle_core_activation_ring_budget.py --profile spatial4col --device npu2 --capacity 2160 --require-all-product-rings-fit --require-worker-independence
```

Product ring schedule checks:

```text
python tools/analysis/middle_core_memtile_ring_schedule.py --profile spatial4col --device npu1 --capacity 2160 --require-ring-fit --require-tee-skip --require-no-per-cell-dma
python tools/analysis/middle_core_memtile_ring_schedule.py --profile spatial4col --device npu2 --capacity 2160 --require-ring-fit --require-tee-skip --require-no-per-cell-dma
```

The schedule model treats the Stage2->Stage3/4 ring as 20 logical C16 rows.
Stage3/4 advances by 16 C16 rows for each C32 output-cell row, retaining the
four overlap rows that form the next window halo. Wrapped 20-row views are split
into at most two row segments; they are not assembled by a scalar kernel.



## Skip-spill BO contract

The long-lived skip path needs an explicit persistent NPU-visible spill arena.
It must not be treated as part of the Stage2->Stage3/4 memory-tile ring, and it
must not alias the frame ingress or egress slot. The ring is the short-lived
main-path context; the skip arena is the long-lived delay store.

Current product contract:

```text
arg3: resident parameter arena
arg4: complete frame-slot ingress
arg5: complete frame-slot egress
arg6: combined SkipA/SkipB spill arena, when the graph exposes skip spill
```

The preferred write policy is still cheap: tee Stage2 rows into SkipA storage
when those rows are produced for the Stage3/4 boundary ring. Ring eviction then
remains ordinary slot reuse, not a scalar callback. Stage5 should use the same
policy for SkipB once that part of the graph is implemented.

For the current R2160-aligned capacity, the skip spill BO reserves exactly one
combined SkipA/SkipB payload:

```text
SkipA C16, Stage2 -> Stage11: 33,423,360 bytes
SkipB C32, Stage5 -> Stage9:  16,711,680 bytes
combined skip spill BO:        50,135,040 bytes
```

APUSR keeps only one frame inside the NPU core. The GPU may already be building
the next frame while the NPU drains the current one, but that does not create a
second NPU-visible skip lifetime. Do not reserve hidden skip-frame slots. If a
later stage needs scratch storage, add that scratch contract explicitly instead
of burying it inside the skip arena.

This is a separate BO allocation contract. The current Stage1+2 -> Stage3 paired
macroblock carrier exposes the skip spill arena explicitly and drains each
2x2 Stage2 C16 macroblock to the correct SkipA tensor-raster addresses. This is a
production-time tee, not a ring-eviction callback and not a Stage2 row spill.

Older compile carriers still do not prove final skip storage unless the selected
kernel signature exposes the skip spill arena argument and the runtime binds the
arena for that graph.

Check it with:

```text
python tools/analysis/middle_core_skip_spill_buffer_contract.py --profile spatial4col --device npu1 --capacity 2160 --require-explicit-bo --require-combined-skip-arena
python tools/analysis/middle_core_skip_spill_buffer_contract.py --profile spatial4col --device npu2 --capacity 2160 --require-explicit-bo --require-combined-skip-arena
```

## Row-lifetime pressure

The current two-ring discussion exposed the real product risk: a design that
turns every producer boundary into a full-width row ring will not fit the
Phoenix floor. This is not a reason to spill every boundary to DDR; it is a
reason to stop treating rings as the default answer.

For the 2160 Phoenix floor, one logical activation row is roughly constant
across the pyramid after sharding across the four memory tiles in a 4-column
worker:

```text
C16 at W/2, C32 at W/4, and C64 at W/8:
  about 7.5 KB per memory-tile shard per logical row
```

The naive all-boundary plan wants about 110 logical rows of simultaneous
short-lived context, or about 845 KB per memory-tile shard before useful
reserves. That cannot be the product schedule.

Even eliminating only the obvious same-scale rings is not quite enough on the
Phoenix floor once reserve space is kept. The transition-only row set is the
first plausible target:

```text
keep as real boundary rings:
  Stage2 -> Stage3/4   20 rows, also SkipA spill tap
  Stage5 -> Stage6/7   20 rows, also SkipB spill tap

avoid as full boundary rings unless measurement proves otherwise:
  Stage1 -> Stage2
  Stage4 -> Stage5
  Stage7 -> Stage8
  Stage9 -> Stage10
  Stage11 -> Stage12
  decoder transition candidates until their CT2D contracts are proven
```

The first concrete pivot was expanded-footprint fusion for Stage1+2, and the
compile proof is now clean enough to treat that boundary as deleted from the
product ring budget. The next equivalent targets are the other same-scale
boundaries: Stage4->5, Stage7->8, Stage9->10, and Stage11->12. Each one should
be proven with the same rule: preserve model math, compute the consumer footprint
locally, and delete the full-width boundary ring only after the audit stays
scalar-clean and parameter-resident.

Run the row-lifetime planner with:

```text
python tools/analysis/middle_core_row_lifetime_budget.py --profile spatial4col --device npu1 --capacity 2160
python tools/analysis/middle_core_row_lifetime_budget.py --profile spatial4col --device npu2 --capacity 2160
```

## Validation

Run:

```text
python tools/analysis/middle_core_stage_placement_check.py
```

The checker verifies that the plan matches the stage ledger, keeps 4col canonical for product work, keeps 2col diagnostic-only, tracks AM020-class connectivity assumptions against the active design contracts, and preserves the zero hot-path parameter-stream rule.


## Worker-group semantics

A worker group is one independent frame-walking copy of the selected physical placement. For the canonical `Spatial4Col` profile, one worker group occupies four physical AIE columns: sixteen compute tiles plus four memory/shim columns behind it. `auto` instantiates as many independent workers as fit on the selected device and splits the frame into that many distinct horizontal worker stripes. On Phoenix/npu1 this is one 4-column worker. On Strix/npu2 this is two independent 4-column workers.

`lane_groups` remains only as a compatibility alias in tooling; the name is avoided in the product contract because it can be confused with a vector/SIMD lane count or a logical lane inside the worker.

A worker is the product floor, not a half-worker that can borrow from a peer. The npu1/Phoenix case has exactly one 4-column worker, so every product path must work with no cross-worker frame-hot dependency. On npu2/Strix, `auto` creates two independent copies of the same 4-column worker and splits the frame. Boundary guard columns between those workers must be owned or redundantly produced inside each worker's local domain. Do not design a product path that requires worker 0 to feed worker 1, or vice versa, during frame execution.

## Skip spill policy

The Stage2 -> Stage3 boundary is now paired to Stage3's real k2/s2 footprint.
Stage1+2 produces Stage2 C16 cells in non-overlapping 2x2 macroblocks. Each
macroblock feeds the split Stage3 downsample directly and writes the same four
Stage2 cells to their long-lived SkipA addresses. This avoids both full Stage2
row ObjectFifos in compute-tile L1 and the rejected fused Stage3/4 20x40xC16
mega-window.

Stage2 output is also the long-lived SkipA source for stage 11. That lifetime is
much longer than the immediate Stage3 handoff. The product policy is to tee
Stage2 output as the macroblock is produced. Ring eviction is ordinary slot
reuse, not the spill trigger. The skip side path must not turn the main
Stage2 -> Stage3 boundary into a DDR write/read pair.

Use the same distinction later for SkipB: short-lived C32/C64 main-path context
stays on-chip when possible, while the Stage5 -> Stage9 skip has a separate
long-lived storage policy.

Run the skip budget with:

```text
python tools/analysis/middle_core_skip_spill_budget.py --profile spatial4col --device npu1 --capacity 2160
python tools/analysis/middle_core_skip_spill_budget.py --profile spatial4col --device npu2 --capacity 2160
```


### Stage1+2 expanded-footprint proof

The Stage1→Stage2 boundary ring is now deleted from the product ring budget. `stage1_2-fused-overlap-resident` computes the exact Stage1-domain `18x10xC16` patch that Stage2 needs from a `20x12xC16` source-domain window, then immediately runs Stage2. This uses the same ConvNext parameters and quantization contract as the unfused blocks. It is an evaluation-footprint/scheduling change, not a training-spec change.

The clean compile confirms that this does not create scalar-byte staging or hot parameter packing. Static counts were lower than the separate Stage1 + Stage2 proofs in several watched categories, so the boundary deletion is a product candidate rather than a desperation fallback.

### Vertical-pass fallback

A 4-column worker remains the atomic product unit. A worker can also walk its assigned rectangle in multiple vertical stripes. That reduces row-ring width and memory-tile pressure, but it serializes each worker across more passes and adds local guard overhead. It is a fallback/tuning lever, not the first product schedule. It must still be addressed before product hardening because unusually wide output modes can exceed fixed memory-tile ring reservations unless the worker pass count, maximum width, or fallback policy is explicit.

## Loud note: shared-object fanout gap

AIE-ML hardware supports neighboring-tile shared-memory communication with locks;
AM020 describes this as a path where DMA and AXI4-Stream are not needed for
neighboring AIE-ML communication. APUSR now uses that shape for the Stage1+2 to
Stage3 handoff. The next capability gap is true multi-consumer ownership of one
shared object: Stage3 should be able to consume the Stage2 macroblock while the
SkipA spill path drains the same produced object, without a second L1 sideband
copy. If IRON/ObjectFifo cannot express that ownership model cleanly, we should
upstream or add a lower-level primitive for shared-memory ObjectFifo fanout with
explicit locks/lifetimes. Until then, the primary graph keeps a small rasterized
SkipA sideband and documents it as a software limitation, not a hardware ideal.
