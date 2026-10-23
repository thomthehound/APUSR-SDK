<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Project status

The implemented super-resolution model is FSR 4.0.2-derived INT8. GPU backends and host frontends are distinct from experimental accelerator and integration work.

| Component | Status | Scope |
| --- | --- | --- |
| D3D12 super resolution | Implemented | INT8 graph with offline DXIL; FidelityFX and NGX entry points |
| Vulkan super resolution | Implemented | Native SPIR-V compute and captured host device/queue state |
| D3D12 frame generation | Implemented | FidelityFX-compatible FI/OF provider and D3D12 presentation |
| Vulkan frame generation | Implemented; under refinement | Native FI/OF, virtual swapchain, WSI presentation, pacing |
| Combined compatibility packages | Implemented | D3D12/Vulkan staged integration components |
| Profiling and shader tools | Available | CPU/GPU events, pass timing, shader analysis |
| D3D11On12 | Planned | Host-resource/queue bridge into the D3D12 runtime |
| Linux distribution | Planned | Native Vulkan compute exists; Linux packaging is separate |
| WMMA/cooperative matrices | Research | No shipping matrix-kernel path |
| XRT/MLIR-AIE NPU | Experimental and paused | Kernel, layout and parameter research |
| FSR 4.1.x Full/Lite | Not implemented | Requires independent model/runtime validation |

## Compatibility scope

The FidelityFX D3D12, NGX D3D12, and Streamline/NGX Vulkan paths have different loader, device, and feature requirements. Frame-generation and latency-control coverage is not identical across the entry points. DLSS Ray Reconstruction and Multi Frame Generation are not supported.

CI and structural tests do not qualify every application, driver, GPU family, or present topology. See [host integration](integration/host_integration_strategy.md), [feature coverage](integration/streamline_feature_compatibility.md), and [testing](testing.md).

## Active work

- Vulkan frame-generation pacing, source admission, resource retirement, and native presentation recovery after FG disable.
- Broader host compatibility and queue/device qualification.
- Reproducible builds, compiler-generated shader consistency, release packaging.
- Profiling-driven CPU and GPU optimization.

See [engineering backlog](engineering_backlog.md) and [roadmap](roadmap.md).
