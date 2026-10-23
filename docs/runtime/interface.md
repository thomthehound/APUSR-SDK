<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Runtime interfaces

## Implemented GPU path

APUSR frontends translate FidelityFX or NGX/Streamline host resources into API-neutral frame inputs. The current GPU upscalers use the FSR 4.0.2-derived INT8 execution graph and native D3D12 or Vulkan backends.

```text
host API / device and queue state
           |
frontend translation (FrameInputDesc, frame options)
           |
model and resource plan
     /           \
D3D12 backend  Vulkan backend
```

Frame inputs include the render/display extents, quality preset or selection information, jitter, motion-vector mode, surface descriptors, and resource-state provenance. Core planning selects logical passes and resources; backends own their API-specific resource bindings, command recording, synchronization, and lifetime.

The model is executed on the GPU. Neither the D3D12 nor the Vulkan product path requires XRT or an NPU. See [architecture](../architecture.md) and [backend portability](backend_portability.md).

## Experimental NPU frame-slot contract

The experimental XRT/MLIR-AIE work defines one complete frame-slot workload per submission:

```text
resident parameter arena
ingress frame slot
egress frame slot
frame descriptor / runtime metadata
```

- Input layout: zero-guarded, row-major C16.
- Output layout: cell-major `m4x8` C16.
- Active extents: supplied as frame metadata.
- Capacity and quality: selected by packaged kernel/model permutation and resident parameter epoch.
- Per-frame work: references a previously programmed parameter epoch; it does not reload model parameters.
- Synchronization: GPU export completion precedes NPU processing; GPU import follows NPU completion.

These are proposed/experimental NPU integration rules, not the contract executed by the current GPU product. See [frame-slot transport](transport.md), [model epoch](../model/model_epoch_runtime_contract.md), and [NPU research](../research/mlir_aie_notes.md).
