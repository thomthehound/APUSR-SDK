<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR

**APU-first super resolution and frame generation for D3D12 and Vulkan.**

APUSR is a multi-backend upscaling runtime built to get more out of modern AMD APUs. It combines an optimized FSR4-derived INT8 inference path, native D3D12 and Vulkan compute backends, broad host-integration compatibility, source-built frame generation, and an architecture designed for future WMMA and NPU acceleration.

> **One model graph. Multiple host frontends. Two native GPU backends. No translation layer, no fake acceleration path.**

APUSR currently targets AMD Phoenix, Hawk Point, Strix Point, Krackan Point, and Strix Halo-class systems. These parts are interesting not because they are small discrete GPUs, but because they combine modern RDNA compute, unified memory, and—on supported devices—an on-package NPU. APUSR treats those properties as architectural inputs rather than afterthoughts.

## What makes APUSR different

- **Native D3D12 and Vulkan.** Both backends execute the same API-neutral model graph directly. Vulkan is not routed through D3D12 or another translation layer.
- **Fast packed-INT8 inference.** The current GPU path preserves accelerated DOT4/DP4A operations and uses backend-specific lowering where compiler output would otherwise leave measurable performance behind.
- **Near backend compute parity.** On matched APUSR workloads, Vulkan is close to the D3D12 compute path, and individual Vulkan passes can meet or beat their DX12 equivalents.
- **Multiple integration surfaces.** APUSR can enter through FidelityFX-compatible, NGX/DLSS-compatible, and Streamline-compatible host paths without requiring one monolithic integration strategy.
- **Source-built frame generation.** Frame interpolation, optical flow, presentation, pacing, and low-latency integration are part of the project rather than opaque renamed binaries.
- **APU-aware runtime design.** Copy avoidance, bandwidth pressure, queue ownership, synchronization scope, persistent resources, and future accelerator handoff are treated as first-class concerns.
- **Offline shader products.** Shipping paths use generated DXIL/SPIR-V products instead of compiling shaders in the frame hot path.
- **Serious profiling.** Structured CPU/GPU instrumentation, pass-level timing, frame-generation diagnostics, shader analysis, and RGP tooling are built into the development workflow.
- **No pretend fast paths.** If required hardware acceleration is unavailable, APUSR prefers an explicit unsupported path over silently substituting scalar emulation and calling it equivalent.

APUSR is not an SDK wrapper. It owns the runtime graph, backend lowering, shader packaging, compatibility frontends, profiling infrastructure, and frame-generation integration around its model implementation.

## Current capability

| Area | Status |
| --- | --- |
| D3D12 FSR4-derived INT8 super resolution | Working and optimized |
| Vulkan FSR4-derived INT8 super resolution | Working; near D3D12 compute parity on matched workloads |
| FidelityFX-compatible D3D12 frontend | Working |
| NGX/DLSS-compatible D3D12 frontend | Working |
| Streamline/DLSS-compatible Vulkan frontend | Working |
| Combined D3D12 + Vulkan compatibility package | Working |
| D3D12 frame generation | Working |
| Vulkan frame generation | Working; pacing/performance refinement continues |
| D3D11 / D3D11On12 | Planned |
| WMMA / cooperative-matrix kernels | Research/planned |
| NPU / XRT / MLIR-AIE acceleration | Experimental; paused while the GPU/runtime path is hardened |
| Linux distribution | Planned around the native Vulkan backend |

Windows is the current product environment. APUSR is still engineering software rather than a turnkey universal installer: host compatibility varies, and integration packages are intended for users who are comfortable working with game/runtime DLL layouts.

See [Project status](docs/status.md) and the [Roadmap](docs/roadmap.md) for the current project boundary.

## Architecture

```text
                 Host application
                       |
          +------------+-------------+
          |            |             |
     FidelityFX    NGX / DLSS    Streamline
          \            |             /
           +-----------+------------+
                       |
              APUSR frame contract
                       |
              FSR4 INT8 model graph
                 /             \
                /               \
          D3D12 backend      Vulkan backend
              DXIL              SPIR-V
                \               /
                 +-------------+
                       |
              profiling / packaging

Future acceleration lanes:
  WMMA / cooperative matrices
  XRT + MLIR-AIE NPU execution
  D3D11On12 host bridge
```

The model layer owns pass order, logical resources, dispatch geometry, constants, and model permutations. Backends own descriptors, barriers, pipelines, resource states, queues, and shader products. Frontends translate external ABI/state into the same internal frame contract.

That separation is deliberate: **host compatibility should not dictate model implementation, and backend-specific optimization should not fork the model graph.**

Read [Architecture](docs/architecture.md), [Backend portability](docs/runtime/backend_portability.md), and [Host integration](docs/integration/host_integration_strategy.md).

## Performance philosophy

APUSR is built around measured hardware behavior, especially the constraints that matter on APUs.

The current GPU path favors:

- accelerated packed integer dot products instead of scalar emulation;
- aligned/vectorized storage access where the layout proves it safe;
- reuse of transformed source data instead of recomputing it per thread;
- persistent descriptors, parameters, shaders, and model products;
- minimal full-resolution copies and redundant resource transitions;
- synchronization scoped to actual producer/consumer dependencies;
- backend-specific shader lowering when shared source compiles poorly;
- profiling that separates APUSR cost from host-renderer, synchronization, and presentation cost.

The goal is not merely feature parity between APIs. The goal is to make each backend **as fast as its native compute model allows** while preserving one semantic model/runtime contract.

See [Performance](docs/performance.md) and [Profiling](docs/profiling.md).

## Host integration

APUSR provides several package shapes because different applications expose different integration seams.

### FidelityFX-compatible D3D12 replacement

```text
apusr_package_ffx_dx12_replacement
```

The smallest D3D12 route: APUSR super resolution plus the source-built frame-generation provider through a FidelityFX-compatible surface.

### NGX/DLSS-compatible D3D12 package

```text
apusr_package_dlss_compatibility
```

Exposes APUSR through an NGX/DLSS-shaped discovery and evaluation path while preserving stock Streamline semantics when Streamline owns the host integration.

### Streamline/DLSS-compatible Vulkan package

```text
apusr_package_streamline_vulkan_compatibility
```

Keeps the host's Streamline runtime responsible for tags, options, and resource translation while APUSR captures the real Vulkan device/queue topology and executes native Vulkan compute below that boundary.

### Combined D3D12 + Vulkan package

```text
apusr_package_dlss_multibackend_compatibility
```

Stages the compatibility components needed by applications that can select either graphics backend without requiring a different APUSR installation.

See [Host integration strategy](docs/integration/host_integration_strategy.md), [NGX/DLSS frontend](docs/integration/ngx_dlss_frontend.md), and [Streamline frontend](docs/integration/dlss_streamline_frontend.md).

## Building

APUSR uses CMake and C++23. Windows is currently the primary product-build environment.

Typical Release configuration:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Useful package targets:

```powershell
cmake --build build --target apusr_package_ffx_dx12_replacement -j
cmake --build build --target apusr_package_dlss_compatibility -j
cmake --build build --target apusr_package_streamline_vulkan_compatibility -j
cmake --build build --target apusr_package_dlss_multibackend_compatibility -j
```

The Vulkan packages build a self-indexed SPIR-V shader pack. The D3D12-only FidelityFX package does not depend on Vulkan shader-pack generation.

See [Building APUSR](docs/building.md).

## Diagnostics and profiling

Runtime diagnostics are opt-in and disabled by default. Common switches include:

```text
APUSR_HOST_DISCOVERY_LOG=1
APUSR_NGX_TRACE=1
APUSR_NGX_TRACE=full
```

Use full tracing only for focused integration diagnosis. Profiling modes, output streams, frame-generation diagnostics, and analysis tools are documented in [Profiling](docs/profiling.md).

## Quality checks

Run the repository quality/regression suite before submitting changes:

```powershell
python .\tools\quality_check.py
```

or:

```bash
python3 tools/quality_check.py
```

The suite checks model/runtime contracts, frontends, shader products, D3D12/Vulkan behavior, frame generation, profiling, packaging, licensing, and integration invariants.

See [Testing](docs/testing.md).

## Repository layout

```text
include/apusr/                 public runtime and backend contracts
src/core/                      API-neutral scheduling and frame/model contracts
src/backends/                  graphics and accelerator backends
src/frontends/                 FidelityFX, NGX/DLSS, Streamline and compatibility entry points
src/framegen/                  interpolation, optical flow and presentation integration
models/                        model manifests, parameter metadata and reference material
shaders/                       APUSR-owned shader sources
kernels/                       experimental MLIR-AIE/NPU kernels
packaging/                     staged drop-in package definitions
experiments/                   isolated reproducible research
tools/                         shader/model build, profiling and analysis utilities
tests/                         behavior and regression contracts
docs/                          architecture, integration, development and research documentation
third_party/                   quarantined external source/reference imports
```

## Project direction

The near-term focus is straightforward:

1. make builds, packaging, CI, and releases reproducible;
2. broaden D3D12/Vulkan compatibility without weakening established paths;
3. continue reducing frame-generation GPU cost and pacing variance;
4. keep the DP4A path highly optimized as the production baseline;
5. evaluate WMMA/cooperative-matrix kernels only where measurements justify the complexity;
6. resume NPU work when same-frame queue ownership and data placement make offload worthwhile;
7. add a D3D11On12 integration lane without contaminating the API-neutral model graph.

See the [Engineering backlog](docs/engineering_backlog.md) for tracked technical debt and the [Roadmap](docs/roadmap.md) for project-level priorities.

## Documentation

Start with [docs/README.md](docs/README.md). Public documentation is organized around current architecture, supported behavior, reproducible research, and unresolved engineering work.

## License

APUSR is an independent project and is not affiliated with AMD, NVIDIA, Microsoft, or their SDK projects.

Original APUSR work is **Copyright (c) 2026 thomthehound** and licensed under the [Apache License 2.0](LICENSE). Contributions use the [Developer Certificate of Origin 1.1](DCO); no CLA is required.

Third-party and derived material retains its own copyright and license terms. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [NOTICE](NOTICE), and [third_party/README.md](third_party/README.md).
