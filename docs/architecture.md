<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Architecture

APUSR separates host compatibility, model scheduling, native GPU execution, and frame generation. The interfaces let multiple frontends use one logical upscaler while each backend manages its own graphics-API state.

## Execution flow

```text
FidelityFX / NGX / Streamline host
               |
     feature/resource translation
               |
FrameInputDesc + UpscalerFrameOptions
               |
      FSR4 INT8 execution plan
        /               \
  DX12 recorder      Vulkan recorder
  DXIL pipelines     SPIR-V pipelines
  D3D12 bindings     Vulkan bindings

Frame generation: independent FI/OF and presentation subsystem
```

### Component ownership

| Component | Responsibility |
| --- | --- |
| `src/frontends/` | Host ABI, feature discovery, tagged resource/state mapping, device and queue capture |
| `src/core/` and `include/apusr/` | Per-frame input contracts, graph scheduling, frame options, diagnostics, execution plan |
| `models/` and model headers | Model permutation, parameters, pass dependencies, scratch layout |
| `src/backends/` | Native API resource allocation, descriptors, shaders, barriers, command recording and retirement |
| `src/framegen/` | Frame interpolation, optical flow, admission control, swapchain/presentation lifecycle |

## Super-resolution execution

1. The frontend translates host resources, render/display extents, quality preset, and known queue/state provenance into an APUSR frame. Where the host does not supply a preset, selection uses the active resolution ratio.
2. The model layer selects the FSR 4.0.2-derived INT8 graph, quality/capacity/permutation key, initializer, logical passes, and scratch allocation plan. Parameters are prepared on model-epoch changes, outside the frame hot path.
3. The backend maps logical read/write roles to GPU resources, binds prepared shader products, and records dispatches in dependency order.
4. GPU resources, descriptors, and borrowed host state remain valid until the last actual consumer retires. Synchronization implements the model's producer/consumer edges using native API primitives.

The model graph specifies operations and logical resources. It does not own `ID3D12Resource`, `VkImage`, command lists/buffers, descriptor allocations, or API barriers. A backend optimization can change shader lowering or physical scratch layout only with a corresponding contract and parity validation.

### Shader and model variants

D3D12 uses offline DXIL, Vulkan uses indexed SPIR-V shader products. Both implement the same graph with accelerated packed signed DOT4 arithmetic. Native capability negotiation must establish the required dot-product and subgroup features before pipeline creation.

WMMA and cooperative-matrix kernels are research alternatives to measured GPU bottlenecks. XRT/MLIR-AIE work is an experimental accelerator lane. Neither is a production replacement for the GPU INT8 implementation. Additional FSR model versions need validated topology, tensor layouts, quantization, initializer, temporal state, and scheduling before runtime selection.

## Host integration

- **FidelityFX D3D12:** compatible effect/provider ABI enters the D3D12 runtime.
- **NGX/DLSS D3D12:** facade translates NGX evaluation/resource state while preserving host queue ownership.
- **Streamline/NGX Vulkan:** stock Streamline supplies tags and options; APUSR captures Vulkan device/queue state and records native compute through its own backend.

Streamline's existing semantic layer remains authoritative for the data it owns. The frontend does not duplicate the model or build a parallel Streamline runtime. See [host integration](integration/host_integration_strategy.md) and [Streamline frontend](integration/dlss_streamline_frontend.md).

## Frame generation

Frame generation has a separate runtime and lifetime from super resolution. D3D12 uses source-built FidelityFX-compatible FI/OF and native presentation integration. The Vulkan path can expose APUSR-owned virtual swapchain images while keeping the real WSI swapchain private:

```text
Game renders retained virtual source
           |
      producer-ready
         /       \
  source replay  Phase A: consume transient inputs
         |           |
   real WSI source  Phase B: FI/OF on private queue
                     |
                optional generated image
```

Source delivery has priority. Generated frames are optional and must not queue behind obsolete work or delay a valid source. Phase A releases transient game-owned inputs at an explicit completion boundary; Phase B consumes provider-owned history. Actual GPU consumer timelines govern reuse and retirement.

A host that retains its virtual swapchain after FG disable can incur source-replay overhead. Safe native-swapchain restoration remains open work. The detailed [Vulkan FG architecture](runtime/vulkan_framegen_architecture.md) defines queue topology, producer-ready fan-out, pacing, WSI resource ownership, and recovery requirements.

## Resource and hot-path contracts

- Borrowed game resources retain their producer/consumer and queue-family constraints.
- Descriptor bindings and resident parameters referenced by in-flight commands cannot be overwritten.
- Cross-queue dependencies use fences/timeline semaphores. Full queue/device waits belong to cold teardown or recovery paths.
- API barriers follow actual resource access and state provenance, rather than a blanket pass-by-pass policy.
- DLL discovery, backend capability probing, shader compilation, manifest parsing, and model setup belong to startup or model-epoch transitions.
- Per-frame work uses prepared plans; diagnostics are disabled by default.

See [backend portability](runtime/backend_portability.md), [model notes](model/models_fsr4.md), [shader ownership](runtime/shader_ownership.md), and [performance](performance.md).
