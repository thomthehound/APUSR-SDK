<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Product interface

APUSR exposes a frontend-compatible upscaler interface and internally normalizes it into a complete-frame-slot NPU runtime contract.

## Frontend

The current frontend is FSR4-compatible DX12. It accepts game resources and dispatch data through the FSR4-style API surface, normalizes feature flags, model preset, model resolution family, jitter, motion-vector mode, and surface descriptors, then records APUSR-owned GPU work.

## Runtime

The product runtime submits one NPU workload per exported frame slot:

```text
resident parameter arena
ingress frame slot
egress frame slot
frame descriptor / runtime metadata
```

The NPU hot frame run does not branch on quality preset and does not upload model parameters. A preset/model change is a model-epoch operation that loads a new resident parameter arena before subsequent frame runs.

## Backend

The XRT backend imports the frame slots and resident parameter arena, waits on the GPU export fence, enqueues the MLIR-AIE kernel run, signals NPU completion, and lets the GPU import pass consume the egress frame slot. CPU involvement is command submission and synchronization, not per-token scheduling.

## Product invariants

- Public work unit: complete frame slot.
- Public transport layout: zero-guarded row-major C16 ingress, cell-major `m4x8` C16 egress; physical-column stripes are placement metadata.
- Host-visible macro-token dispatch: forbidden.
- Active resolution: carried by frame metadata.
- Capacity class: selected by packaged kernel/model family.
- Quality preset: selected by resident parameter arena and bookend PSO/model permutation.
