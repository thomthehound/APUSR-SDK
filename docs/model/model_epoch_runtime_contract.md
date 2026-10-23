<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Experimental NPU model-epoch contract

This document specifies the experimental XRT/MLIR-AIE model-epoch design. The production GPU upscalers do not execute this NPU frame-slot path.

A model epoch is the resident NPU state for one model permutation, parameter profile, and compiled middle-core ABI. Epoch changes are rare control-plane work. Frame execution is the hot path and must not carry parameter traffic.

## Frame-execution rule

`executeFrame` must not apply a model epoch.

A frame submission may reference the active resident epoch. It must not allocate a parameter arena, upload parameters, run the parameter-load instruction stream, reload an xclbin, or wait for an epoch change to finish. Preset/model changes are control-plane events, not frame-conveyor work.

## Epoch-change path

The runtime programs resident parameters during configuration or an explicit epoch switch:

1. Select the model permutation and parameter profile.
2. Copy the packed resident parameter arena into one aligned control BO.
3. Import the arena as `ResidentParameterArena`.
4. Run the model-epoch parameter-load instruction stream.
5. Publish the non-zero model epoch through the runtime, frame dispatches, backend statistics, and doorbell header.

The epoch load may DMA parameter bytes. That is allowed because it is not a frame-hot operation.

## Quiescent switching

The first dynamic switch mode is intentionally conservative:

```text
switchModelEpochQuiescent(DrainAndResetOutputHistory)
```

The switch path drains submitted NPU work, loads the replacement resident arena, publishes the new epoch, and drops pending old-epoch output/history. This may create one or two visually discontinuous frames around a graphics-mode change. That is acceptable for games. It is not acceptable to corrupt resident parameter ownership or hide a parameter-load wait inside `executeFrame`.

A quiescent switch is safe when no submitted NPU run can still read the old resident parameters. `retiredSequence` is not enough proof; frame-worklist lowering retires descriptors after submission, not after NPU completion. The backend must expose an explicit drain for the submitted work it owns.

Persistent-control-ring execution cannot switch resident epochs yet. It needs a controller pause/drain/ack/resume protocol before dynamic switching is valid.

## Frame-hot path

Each submitted frame carries only payload and synchronization:

1. The GPU exports one complete zero-guarded C16 ingress frame slot.
2. The NPU receives one frame work item that references the already-resident model epoch.
3. The NPU writes one complete C16 egress frame slot.
4. The GPU imports the previous completed egress frame when model egress is enabled.

The frame-hot parameter DMA budget is exactly zero. A resident-parameter dispatch must fail if the NPU backend has not completed the epoch-load path or if the dispatch epoch does not match the loaded resident epoch.

## Visual discontinuity policy

APUSR does not promise frame-perfect output across a model-epoch switch. A preset change may reset temporal continuity and drop pending old-epoch output. That is the preferred product behavior because it avoids expensive state stitching and protects the conveyor.

Allowed at an epoch switch:

```text
old output is dropped
history is reset
one or two frames may look discontinuous
```

Not allowed:

```text
resident parameters are overwritten while old work can still read them
executeFrame performs the epoch switch
hot-path parameter DMA appears
persistent controllers are mutated without an explicit pause/drain/ack protocol
```

## Rollback boundary

If loading a replacement epoch fails after resident state may have been touched, the runtime attempts to reload the previous active arena. If that reload fails, the resident epoch is marked not ready and the NPU path must not submit resident-parameter frame work until the backend is reconfigured.

## Current implementation boundary

`RuntimeConfig::modelEpoch` names the initial resident epoch. `RuntimeContext` programs that epoch once when the resident arena is configured, then stamps that epoch onto all frame dispatches and the doorbell header's `reconfigureEpoch` field.

`RuntimeContext::switchModelEpochQuiescent` is the first explicit switch API. It supports only `DrainAndResetOutputHistory`; smarter apply-when-idle switching can be layered later without changing the frame-hot contract.


## Resident compile-proof boundary

The standalone `stage1-resident` graph currently seeds the first resident record
as a tile-local buffer so the AIE primitive and hot C16-halo path can compile
without parameter DMA. This is a proof of the resident primitive contract, not a
separate Stage-1 parameter system.

The product contract remains unchanged: real parameter movement is model-epoch
work, and `executeFrame` must not load parameters. Later resident records should
use the same compact-source / expanded-hot split and the same per-slice residual
epilogue checks.
