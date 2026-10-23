<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR

APUSR is an APU-focused super-resolution and frame-generation runtime with native D3D12 and Vulkan backends. The current super-resolution implementation runs an FSR 4.0.2-derived INT8 model with offline-compiled shader products and resident parameters. Compatibility frontends expose FidelityFX, NGX/DLSS, and Streamline integration to supported applications.

The principal hardware targets are AMD Phoenix, Hawk Point, Strix Point, Krackan Point, and Strix Halo systems. Windows is the product integration environment; Linux/Vulkan packaging and D3D11On12 integration remain development work.

## Capability status

| Area | Implementation | Status |
| --- | --- | --- |
| Super resolution | FSR4-derived INT8, D3D12/DXIL and Vulkan/SPIR-V | Implemented |
| D3D12 host integration | FidelityFX and NGX/DLSS compatibility | Implemented; host-dependent |
| Vulkan host integration | Streamline/NGX attachment and native compute | Implemented; host-dependent |
| Frame generation | FidelityFX-derived FI/OF and native D3D12/Vulkan presentation | Implemented; pacing and host coverage under refinement |
| Profiling and shader analysis | Structured CPU/GPU events, shader and RGP/PIX tooling | Available |
| D3D11On12 | Host-state bridge into the D3D12 backend | Planned |
| Linux distribution | Native Vulkan backend packaging and host integration | Planned |
| WMMA/cooperative matrices | Alternative accelerated neural kernels | Research |
| XRT/MLIR-AIE NPU | Kernel, parameter, and placement experiments | Not a production backend |
| FSR 4.1.x Full/Lite | Additional model variants | Not implemented |

## Architecture

```text
Host: FidelityFX / NGX / stock Streamline
                 |
       frontend ABI translation
                 |
     frame inputs and host state
                 |
       model / execution plan
         /             \
  D3D12 backend      Vulkan backend
  DXIL + resources   SPIR-V + resources

Frame generation: separate FI/OF, scheduling, and presentation
Profiling: shared CPU/GPU event and diagnostics infrastructure
```

Frontends translate external ABI, feature requests, resource state, and device/queue provenance. The model runtime selects pass order, quality preset, capacity, parameter epoch, scratch resources, and dispatch plan. Backends own descriptors, pipelines, resource transitions, synchronization, GPU submission, and retirement. Frame generation has its own runtime lifecycle; disabled frame generation does not schedule FI/OF.

The D3D12 and Vulkan upscalers execute the same logical graph. Shader code generation and memory layout may differ by backend when semantics and correctness tests remain intact. See [architecture](docs/architecture.md), [backend portability](docs/runtime/backend_portability.md), and [frame generation](docs/runtime/frame_generation.md).

## Build and test

APUSR uses CMake, Ninja, C++23, and Python for build tooling. Use an initialized x64 MSVC developer environment for product packages:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPUSR_ENABLE_XRT_BACKEND=OFF
cmake --build build --target apusr_package_ffx_dx12_replacement --parallel 4
cmake --build build --target apusr_package_dlss_compatibility --parallel 4
cmake --build build --target apusr_package_streamline_vulkan_compatibility --parallel 4
cmake --build build --target apusr_package_dlss_multibackend_compatibility --parallel 4
```

The D3D12 FidelityFX package does not require the Vulkan shader pack. Vulkan-capable packages build an indexed SPIR-V shader product. See [building](docs/building.md) and [package layouts](packaging/README.md).

Run the repository quality suite:

```powershell
python tools/quality_check.py
```

```bash
python3 tools/quality_check.py
```

The suite checks model and runtime contracts, frontends, shader products, frame generation, packaging, and licensing. Structural/CI coverage does not replace GPU, driver, or game-host validation. See [testing](docs/testing.md).

## Integration and diagnostics

The compatibility packages are development-oriented DLL distributions, not a universal game installer. Coverage depends on the host ABI, loader order, graphics backend, and device capabilities. See [integration strategy](docs/integration/host_integration_strategy.md) and [project status](docs/status.md).

Diagnostics are opt-in: `APUSR_HOST_DISCOVERY_LOG=1`, `APUSR_NGX_TRACE=1`, and `APUSR_NGX_TRACE=full` are available for host integration. The [profiling guide](docs/profiling.md) documents capture modes and analysis.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `include/apusr/` | Public frame, model, backend, and frontend contracts |
| `src/core/` | API-neutral frame state, execution plans, scheduling, diagnostics |
| `src/backends/` | Native D3D12/Vulkan execution and experimental NPU support |
| `src/frontends/` | FidelityFX, NGX/DLSS, Streamline, host capture |
| `src/framegen/` | FI/OF algorithms, scheduling, native presentation |
| `models/` | Parameters, manifests, reference fixtures |
| `shaders/` | Shader source; generated products are prepared offline |
| `kernels/` | Experimental MLIR-AIE/NPU kernels |
| `packaging/` | Drop-in package targets and deployment instructions |
| `tests/`, `tools/` | Contract testing, compiler, analysis, profiling tools |
| `docs/` | Architecture, integration, model and research documentation |

## Project information

[Documentation](docs/README.md) · [Status](docs/status.md) · [Roadmap](docs/roadmap.md) · [Contributing](CONTRIBUTING.md)

Original APUSR source is Apache-2.0. Imported code and model artifacts retain their recorded redistribution terms. See [licensing](docs/development/licensing.md), `THIRD_PARTY_NOTICES.md`, and `LICENSES/`.
