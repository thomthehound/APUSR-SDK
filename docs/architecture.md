<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Architecture

APUSR is structured around three independent concerns:

1. **host integration** — how an application reaches APUSR and how graphics-device/queue ownership is discovered;
2. **model/runtime** — the API-neutral frame contract and FSR4 INT8 pass graph;
3. **backend execution** — how the graph is lowered to D3D12, Vulkan, and future accelerators.

Keeping those concerns separate is the central architectural rule. A new frontend should not require a new model implementation, and a backend optimization should not leak host-specific behavior into the graph.

## Layering

```text
Application / host
    |
    +-- FidelityFX-compatible frontend
    +-- NGX/DLSS-compatible frontend
    +-- Streamline-compatible attachment
    |
    v
Host resource/state translation
    |
    v
FrameInputDesc + API-neutral runtime state
    |
    v
FSR4 INT8 graph / execution plan
    |
    +-- D3D12 backend -> DXIL / D3D12 resources and barriers
    +-- Vulkan backend -> SPIR-V / Vulkan resources and barriers
    +-- future D3D11On12 bridge
    +-- experimental XRT/MLIR-AIE accelerator lane
```

## API-neutral model graph

The graph owns the durable model contract:

- pass order;
- logical resource roles and lifetimes;
- presets and capacity classes;
- dispatch geometry;
- constants and permutation keys;
- initializer and parameter data;
- read/write intent.

It does **not** own `ID3D12Resource`, `VkImage`, descriptor heaps, descriptor sets, command lists, command buffers, or API-specific barriers.

Backend implementations map the same logical graph to their native execution model.

## Frontend ownership

Frontends translate external ABI/state into APUSR's internal frame description. They may also provide compatibility surfaces required for feature discovery, but they should not become alternate model runtimes.

### FidelityFX-compatible frontend

The D3D12 FidelityFX route is the smallest direct integration path. It accepts the expected provider ABI, translates host resources/constants, and enters the APUSR D3D12 runtime.

### NGX/DLSS-compatible frontend

The NGX facade translates NGX parameter/resource state into the same internal frame contract. On systems using Streamline, APUSR prefers to preserve stock Streamline's tag/options/resource semantics rather than reimplementing the entire interposer.

### Streamline attachment

The Streamline-compatible path is deliberately narrow. It exists to:

- establish APUSR below the host's stock Streamline semantic layer;
- publish/consume real graphics-device information;
- perform cross-vendor feature-policy translation where APUSR owns the implementation;
- capture Vulkan device/queue topology when required by the native Vulkan backend.

It is not intended to become a second independent Streamline state machine.

## Graphics backend ownership

Backends own:

- API resources and allocation;
- descriptors/bindings;
- pipeline creation and shader products;
- state/layout transitions;
- synchronization and retirement;
- GPU timestamp instrumentation;
- backend capability negotiation.

### D3D12

D3D12 is the most mature backend and remains the reference for many correctness fixtures. Product shaders are compiled to DXIL offline.

### Vulkan

Vulkan is a native compute backend. It uses the same graph, offline-generated SPIR-V products, explicit descriptor/resource mappings, and captured host queue/device topology. It does not translate through D3D12.

Vulkan capability negotiation is fail-closed for operations the shader pack requires, including accelerated packed integer dot products and compute-subgroup features.

## Frame generation

Frame generation is a separate runtime subsystem with shared scheduling policy and backend-specific resource/presentation implementations.

The Vulkan path can virtualize application-visible swapchain images while retaining the loader's real WSI images as presentation targets. Source replay, FI/OF computation, generated-frame presentation, and virtual-slot reuse are synchronized explicitly rather than by global queue drains.

The D3D12 and Vulkan implementations should share frame-generation policy where practical without forcing one API's presentation model onto the other.

## Accelerator research

### WMMA/cooperative matrices

Matrix-instruction work is a candidate backend optimization, not a replacement architecture. It should consume the same logical model/parameter contract and prove a measured advantage against the production DP4A path.

### NPU/XRT/MLIR-AIE

The NPU lane is an experimental second accelerator. The intended architecture offloads meaningful model regions while preserving direct host queue/device ownership and avoiding host-visible frame-delay workarounds. It must justify synchronization and data movement, not merely kernel execution time.

## Lifetime and synchronization rules

Resource/binding lifetime is part of correctness:

- descriptors referenced by in-flight GPU work must not be overwritten;
- borrowed command buffers/lists cannot be treated as retired until the host submission relationship is known;
- cross-queue resources require explicit ownership/sharing semantics;
- full-device or full-queue waits are not acceptable substitutes for a missing lifetime model in the product hot path.

Backend synchronization should be as narrow as the known producer/consumer contract permits, but never guessed from insufficient state provenance.

## Cold path versus hot path

The frame hot path must not perform:

- DLL discovery;
- shader compilation;
- source parsing;
- JSON/model-manifest parsing;
- backend probing;
- plugin discovery;
- unbounded diagnostic logging.

Those operations belong to startup, packaging, or developer tooling.

## Related documents

- [Backend portability](runtime/backend_portability.md)
- [Host integration strategy](integration/host_integration_strategy.md)
- [Shader ownership](runtime/shader_ownership.md)
- [Frame generation](runtime/frame_generation.md)
- [Vulkan frame-generation architecture](runtime/vulkan_framegen_architecture.md)
- [Performance](performance.md)
