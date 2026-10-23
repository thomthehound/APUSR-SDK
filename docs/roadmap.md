<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Roadmap

Development priorities follow runtime dependencies and measured bottlenecks.

## Release and runtime hardening

1. **CI and packaging:** maintain Windows/Linux quality checks, deterministic shader products, third-party provenance, release artifacts, and versioning.
2. **Host compatibility:** expand loader-order, queue/device, graphics backend, and resource-state qualification.
3. **Frame generation:** reduce FI/OF cost, preserve source continuity, improve Vulkan pacing, and establish native swapchain recovery when FG is disabled.
4. **Runtime optimization:** profile and remove avoidable copies, allocations, synchronization, and shader-codegen overhead.

## Platform and model coverage

5. **Linux/Vulkan distribution:** build packaging and host integration around existing native Vulkan compute.
6. **D3D11On12:** translate D3D11 resource/state/queue ownership into the D3D12 runtime.
7. **FSR 4.1.x Full and Lite:** characterize network topology, tensor shapes, quantization, parameter formats, pass schedule, and temporal state. Validate each version before adding runtime selection.

## Accelerator research

8. **WMMA/cooperative matrices:** compare selected bottleneck passes against the production packed-DOT4 path on target hardware.
9. **XRT/MLIR-AIE NPU:** revisit contiguous model partitions when full-frame memory transfer and synchronization costs can be measured.

See [status](status.md) and [engineering backlog](engineering_backlog.md).
