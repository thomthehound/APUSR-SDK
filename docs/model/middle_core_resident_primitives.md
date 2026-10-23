<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Experimental NPU resident primitive records

This page describes the experimental workflow for adding NPU-resident model stages; the current GPU runtime does not use these primitives.
It is not a per-stage notebook. Stage-specific facts belong in machine-readable
records and checks, not in one new document per logical stage.

The canonical record catalog is:

```text
models/fsr4_middle_core_resident_records_v0.json
```

Each record describes one physical primitive contract:

```text
logical stage coverage / resident group
input and output activation cell shape
AIE primitive entry point
source arena field layout
model-epoch hot-record transform policy
residual/skip epilogue shift policy
hot-path parameter stream count
static performance budget
```

The kernel owns the physical record format. The source FSR4 tensor names explain
meaning only; they do not dictate hot-layout order, vector chunking, epilogue
expansion, or tile-local placement.

## Workflow for adding a resident primitive

Use the same pattern for any stage we bring up:

```text
1. Add or update a record in fsr4_middle_core_resident_records_v0.json.
2. Bind the record to the stage ledger and placement plan.
3. Add the AIE primitive using the record's physical layout.
4. Keep model-epoch parameter loading outside the frame-hot sequence.
5. Audit the compiled ELF before asking for another expensive compile.
6. Promote only reusable findings into this document.
```

Do not create a standalone markdown file for every stage. If a stage needs a
large explanation, first try to encode the invariant in the record catalog,
ledger, placement plan, or a contract checker.

## Record coverage

The catalog now covers the full stage 1→12 resident arena. Several records are
still pending AIE primitive lowering, but their source layouts, epilogue policy,
and frame-hot budget are checked now. This keeps parameter work generalized
while Stage 1 remains the first active compile proof.

The first two C16 resident records now use the same workflow in two different
activation-context modes:

```text
Stage 1: zero-guarded C16 halo patch -> ConvNext16 -> m4x8 C16 cell
Stage 2: Stage-1 producer-owned C16 context -> ConvNext16 -> m4x8 C16 cell
```

Stage 2 is intentionally not another seam-halo synthesis experiment. The compatibility proof still consumes explicit 18x10xC16 carrier patches, and the plane-window proof keeps that same compact logical view while changing the source contract: the window should be read from a worker-local Stage-1 row-major producer ring. The rejected three-input top/center/bottom row-window carrier is recorded as a failed shape, not a product contract. The linked proof still has a tile-local bridge, but that bridge is now scaffolding to delete, not the Stage-2 contract.

The Stage 3/4 record now has three related graph contracts under the same
resident record. The first isolates the C16->C32 downsample. The second is the
standalone fused pressure proof. The third is the product contract for the same
fused body: Stage 3/4 consumes a 20x40xC16 producer-overlap context window from
the upstream C16 row ring. That window is the one-pixel C32 Stage-4 spatial
context after 2x downsampling, padded only to the vector-friendly 4-pixel group
boundary. It is intentionally smaller than a full 4x4 C16 cell slab.

The product rule is blunt: Stage 3/4 should reuse the C16 overlap Stage 2
already has available. Do not synthesize interior Stage-3/4 seam halos from
scratch. The standalone graph may use a row-major C16 carrier until Stage 2 is
linked, but that carrier is a compile harness, not the final dataflow.

The fused proof still excludes frame boundary cells. Boundary handling should be
added through an explicit front-guard or boundary contract after the interior
path is stable.

The current `stage1-resident`, `stage2-resident`, `stage2-plane-window-resident`, `stage1_2-linked-resident`, `stage1_2-fused-overlap-resident`, `stage1_2-worker-ring-resident`, `stage1_4-worker-ring-resident`, `stage1_2-to-stage3_4-worker-ring-resident`, and `stage1_2-to-stage3-paired-macroblock-resident` graph phases are compile
proofs. `stage2-plane-window-resident` is the compact Stage-2 input contract: one 18x10xC16 producer-plane window per output cell. It is deliberately not the rejected three-input row-window carrier and it does not require cross-worker hot-path exchange. The linked and worker-ring phases remain useful compile carriers, but the first product candidate is now `stage1_2-fused-overlap-resident`: Stage1 computes the exact local Stage1-domain patch Stage2 needs, then Stage2 runs immediately. That deletes the full-width Stage1->Stage2 activation ring from the product memory budget. The active spatial-core path is now `stage1_2-to-stage3-paired-macroblock-resident`: Stage1+2 fused-overlap produces the four Stage2 C16 cells needed by each Stage3 k2/s2 output, tees those same cells to SkipA, and passes a compact 2x2 C16 group to the adjacent Stage3 tile through shared tile memory. The rejected full-row Stage2 ring and fused Stage3/4 20x40xC16 mega-window remain diagnostic-only traps, not the primary path. Stage4 is split off until the C32 halo ring is wired. These proofs use tile-local seeded **hot records** so we can
test the primitives and lowering without blocking on the final live `load_params` graph implementation. The
product runtime contract still requires real parameter movement to happen at
model-epoch load time, never inside `executeFrame()`.

Resident records now distinguish two layouts:

```text
source arena record:
  compact package-facing payload stored in the model arena

tile-local hot record:
  model-epoch-expanded payload consumed by AIE kernels during frame work
```

The first source record is still 4048 bytes. Its tile-local hot record is 5776
bytes because bias/multiplier/residual epilogue fields are expanded into m4x8
blocks before frame execution. Later resident records must make the same
compact-source / expanded-hot decision from their primitive shape, not from
their logical stage number.

Residual epilogues now have explicit per-slice metadata in each resident arena
manifest. A hot record may omit the residual shift only when that residual shift
matches the output SRS shift. If a later slice has a distinct residual shift,
the hot record must keep that shift or prefold it during model-epoch loading.
That rule prevents a Stage-1 shortcut from becoming a hidden cross-stage bug.


## Activation context and halo policy

Resident primitives should reuse activation context that already exists in the
producer handoff, tile-local row ring, shared-memory boundary, or memory-tile
window. Do not generate interior seam halos as the default answer to a consumer
halo requirement. Synthetic halo construction burns local memory, DMA shape
budget, locks, and vector movement before it buys us model work.

Boundary padding is different from interior seam repair. Frame-edge padding may
come from the front guard or from an explicit boundary contract. Interior cells
should consume producer-provided overlap whenever that overlap is available.
Only use synthetic halo generation when the local case is proven simple and the
audit shows it is cheaper than carrying the overlap forward.

This is now a resident-record policy rather than a Stage-3/4 note: each record
declares that available activation context must be reused before synthesizing a
new halo.

## Audit-driven optimization notes

The Stage-1 2-column audit shows that the primitive lowers through the
expected vector/SRS path. The parameter-pack hoist reduced compact-parameter pack work from
176 to 96 watched `vpush.hi.32` instructions per core, but increased `.text` from
4080 to 6496 bytes per core. That is a useful trade study, not a final answer.

Known priority targets:

```text
1. Consume tile-local hot records with pre-expanded epilogue blocks.
2. Keep source arena records compact until expanded hot records are proven across more stages.
3. Use the resident epilogue metadata to decide whether residual shift can be omitted, kept, or prefolded.
4. Widen residual activations through vector load/unpack, not scalar i8 -> i32 gather loops.
5. Treat resident tile memory as cheaper than repeated hot unpack instructions, but verify code size after each primitive family changes.
```

Future primitive work should update the catalog and checks first, then add the
AIE kernel. Stage-specific prose belongs here only when it changes a reusable
rule.


`stage1_2-fused-overlap-resident` is the expanded-footprint fusion proof. It does not add a new parameter format. It asks one kernel to evaluate Stage1 over the local Stage2 footprint, materialize only the `18x10xC16` Stage1 patch that Stage2 needs, and run Stage2 immediately. The clean compile stays scalar-clean and parameter-resident, so this boundary is treated as deleted from the product ring budget rather than as a speculative replacement.
