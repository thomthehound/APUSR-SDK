<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Middle-core stage ledger

`models/fsr4_middle_core_stage_ledger_v0.json` is the canonical planning
contract between the logical FSR4 INT8 middle core and the physical APUSR
spatial-core implementation.

The ledger exists because the resident parameter arena is not a generic bag of
weights. It is a physical contract between logical stages, tile ownership,
activation layout, epilogue math, skip live ranges, and model-epoch loading.

## Rules

- The NPU owns logical stages 1 through 12.
- GPU bookends own stage 0 and stage 13.
- Every logical transition stage is first-class work. Stages 3, 6, 9, and 11 are
  not packing details.
- Kernel primitives own the physical parameter format. The FSR4 logical model
  only owns tensor meaning.
- Resident parameters load at model epoch. Frame-hot runs must not stream
  parameter bytes.
- Current resident arena slices are seed contracts. They may be split or
  reshaped when the real tile-local kernels demand it.

## Logical stage map

| Stage | Role | Physical owner | Current resident group |
|---:|---|---|---|
| 1 | C16 ConvNext block | `front_c16` | `stage1_convnext16` |
| 2 | C16 ConvNext block, SkipA source | `front_c16` | `stage2_convnext16` |
| 3 | C16→C32 k2/s2 downsample | `down_c32` | `stage3_4_down_fnb32` |
| 4 | C32 FasterNet block | `mid_c32` | `stage3_4_down_fnb32` |
| 5 | C32 FasterNet block, SkipB source | `mid_c32` | `stage5_fnb32_c8group` |
| 6 | C32→C64 k2/s2 downsample | `down_c64` | `stage6_7_down_fnb64` |
| 7 | C64 FasterNet block | `bottleneck_c64` | `stage6_7_down_fnb64` |
| 8 | C64 FasterNet block | `bottleneck_c64` | `stage8_fnb64` |
| 9 | C64→C32 CT2D + SkipB join | `up_c32` | `stage9_ct2d32_chunks` |
| 10 | C32 FasterNet block | `up_c32` | `stage10_fnb32_c8group` |
| 11 | C32→C16 CT2D + SkipA join | `decoder_c16` | `stage11_fnb32_ct2d16` |
| 12 | C16 ConvNext block and egress cell store | `decoder_c16` | `stage12_convnext16` |

The grouped stage 3/4 and 6/7 slices are explicitly provisional. They reflect
the current arena seed, not a requirement that transition stages share a
physical tile-local resident record with the following same-scale block.

`models/fsr4_middle_core_stage_placement_v0.json` builds on this ledger and
classifies the current fusion/fission boundaries. The placement file owns the
2col/4col connectivity verdict; this ledger owns stage identity and parameter
meaning.

## Physical rows

The current spatial-core plan uses these physical roles:

```text
front_c16       stages 1, 2    C16 front blocks and SkipA production
down_c32        stage 3        C16->C32 transition
mid_c32         stages 4, 5    C32 encoder and SkipB production
down_c64        stage 6        C32->C64 transition
bottleneck_c64  stages 7, 8    C64 bottleneck
up_c32          stages 9, 10   C64->C32 decoder and C32 block
decoder_c16     stages 11, 12  C32->C16 decoder, SkipA join, final C16 block
egress_pack     no logical stage; drains m4x8 C16 cells
```

This row map is allowed to change when measurement or compilation proves a
better placement. The ledger must be updated first so parameter packing and
kernel work stay aligned.

## Relationship to older planning files

`models/fsr4_middle_core_v0.json` is now only a compact historical/planning
summary of same-scale blocks and skip FIFOs. It is not the complete model-stage
source of truth. Use the ledger when deciding where parameters go or which stage
owns a primitive.

## Skip live-range policy

Stage 2 is both a normal main-path producer and the SkipA source. Its output
feeds the Stage3/4 boundary immediately, but it must also survive until the
Stage11 skip join. Those are different lifetimes. The main-path Stage2->Stage3/4
context should stay in worker-local memory-tile boundary rings. The long-lived
SkipA path should be teed to its spill store as Stage2 produces those rows.

Stage 5 has the same pattern for SkipB: immediate main-path work continues
through Stage6/7/8, while the SkipB value must survive until Stage9. Treat skip
spill as a deliberate side path, not as a reason to spill every stage boundary.

## Validation

Run:

```text
python tools/analysis/middle_core_stage_ledger_check.py
```

The checker enforces that the ledger covers stages 1..12, uses known physical
roles and activation shapes, keeps parameter loads out of the frame-hot path,
and matches the checked-in resident arena slice offsets and payload sizes.

## Resident proof instance

`stage1-resident` is the first standalone implementation target backed by the ledger. It consumes the first resident record and emits a C16 cell without assuming immediate fusion with the next C16 consumer. The same record/catalog/checker path is the intended route for later resident primitives.
