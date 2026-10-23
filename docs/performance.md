<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Performance

APUSR treats performance as an architectural requirement, not a final polish pass.

The current production super-resolution lane is optimized packed INT8 DOT4/DP4A. WMMA/cooperative-matrix and NPU variants remain research lanes until they demonstrate a clear benefit against that baseline.

## Principles

### Require real acceleration

Product shader selection requires native acceleration for its packed-dot or matrix operations. Unsupported devices fail capability checks instead of silently executing scalar emulation.

### Measure the real hot path

Performance claims should separate:

- CPU setup/recording time;
- GPU pass execution;
- synchronization/waits;
- presentation/frame-generation work;
- host/game work outside APUSR.

A total-frame regression must not automatically be attributed to APUSR shader time, and a shader micro-optimization must not be advertised as a frame-level improvement without evidence.

### Prefer data movement wins over instruction-count theater

APUs are particularly sensitive to bandwidth and unnecessary copies. Important optimization classes include:

- aligned vector storage loads/stores;
- reuse of transformed source data;
- avoiding duplicate full-resolution images/copies;
- keeping descriptor and parameter data persistent;
- eliminating unnecessary host/device round trips;
- keeping resource state transitions scoped to real dependencies.

### Backend parity is semantic, not textual

D3D12 and Vulkan use the same model graph but do not need byte-for-byte equivalent shader source when one backend compiler lowers a spelling poorly. Backend-specific HLSL/SPIR-V lowering is acceptable when:

1. the model math and resource contract remain equivalent;
2. the optimization is proven safe for the affected layout/geometry;
3. the reference path remains testable;
4. the resulting code is measured rather than assumed faster.

## D3D12 and Vulkan

The Vulkan backend is optimized by identifying concrete code-generation and synchronization pathologies rather than treating API choice itself as a performance explanation. Current high-value techniques include:

- preserving packed integer dot-product operations through SPIR-V;
- replacing scalarized aligned raw-buffer vector accesses with vector storage operations;
- reducing duplicated transformed-input work in full-resolution bookend passes;
- narrowing synchronization only where the producer/consumer contract can be proven.

On matched APUSR workloads, the current Vulkan path is near D3D12 compute parity, with some individual Vulkan passes meeting or outperforming their D3D12 counterparts.

Whole-application D3D12/Vulkan frame-rate differences must be interpreted separately. GPU profiling has shown cases where most of the application-level Vulkan deficit exists in renderer work outside APUSR even when APUSR's own compute is near parity. The APUSR profiler therefore reports its own GPU interval and should not claim ownership of uninstrumented host work.

## Current optimization directions

### Super resolution

- Continue profiling `pre`/`post` and other large passes only when a specific resource/code-generation bottleneck is visible.
- Preserve the optimized DP4A path as the production baseline.
- Begin WMMA/cooperative-matrix experiments only from isolated, measured candidates.
- Keep D3D12 and Vulkan model semantics equivalent while allowing backend-specific lowering.

### Frame generation

- Reduce FI/OF GPU cost without hiding it behind queue/pacing changes.
- Keep presentation asynchronous where correctness permits it.
- Avoid full drains, queue-idle waits, and unbounded virtual-swapchain backlog.
- Maintain explicit profiling of compute, preparation, readiness, presentation, and source-frame flow.

### CPU/runtime

- Revisit frontend parameter translation and resource/descriptor setup for avoidable per-frame work.
- Keep DLL discovery, shader lookup, JSON parsing, and other cold-path work out of dispatch.
- Treat logging as opt-in and low-volume by default.
