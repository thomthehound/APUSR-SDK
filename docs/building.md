<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Building APUSR

## Requirements

APUSR uses CMake, Ninja, C++23, and Python. Windows x64 with an initialized MSVC developer environment is the primary product build environment. Linux/GCC quality builds validate source and contracts, but are not a complete Linux game-integration distribution.

XRT/MLIR-AIE tooling is not required for the standard GPU runtime. DXIL and SPIR-V shader products are generated or staged at build time.

## Configure

In an x64 MSVC developer PowerShell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPUSR_ENABLE_XRT_BACKEND=OFF
```

On Linux, configure and run the quality targets separately:

```bash
cmake -S . -B build-quality -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPUSR_BUILD_TESTS=ON -DAPUSR_BUILD_FRONTENDS=ON -DAPUSR_ENABLE_XRT_BACKEND=OFF
cmake --build build-quality --target apusr_quality_tests --parallel 4
ctest --test-dir build-quality -L quality --output-on-failure
```

## Product packages

```powershell
cmake --build build --target apusr_package_ffx_dx12_replacement --parallel 4
cmake --build build --target apusr_package_dlss_compatibility --parallel 4
cmake --build build --target apusr_package_streamline_vulkan_compatibility --parallel 4
cmake --build build --target apusr_package_dlss_multibackend_compatibility --parallel 4
```

| Target | Purpose |
| --- | --- |
| `apusr_package_ffx_dx12_replacement` | FidelityFX-compatible D3D12 super resolution and frame generation |
| `apusr_package_dlss_compatibility` | D3D12 NGX/DLSS compatibility |
| `apusr_package_streamline_vulkan_compatibility` | Native Vulkan, Streamline/NGX modules and indexed SPIR-V |
| `apusr_package_dlss_multibackend_compatibility` | Combined D3D12 and Vulkan host components |

The D3D12 FidelityFX-only package does not need the Vulkan shader pack. `APUSR_SHADER_COMPILE_JOBS` overrides shader compiler parallelism when needed.

See [packaging](../packaging/README.md) for deployed files and installation/restore contracts.

## Optional features

- Profiler: runtime controls and build flags in [profiling](profiling.md).
- GPU debug markers: optional PIX/RGP integration; see [marker tooling](development/rgp_gpu_markers.md).
- NPU/XRT: `APUSR_ENABLE_XRT_BACKEND` and `APUSR_BUILD_AIE_KERNELS` require the experimental toolchain.

## Quality gate

```powershell
python tools/quality_check.py
```

```bash
python3 tools/quality_check.py
```

The script performs a clean quality build by default and runs CTest with the `quality` label. Use `--reuse-build` for an existing test build. Generated shader products are tied to compiler behavior; use the documented compiler payload and regeneration process before changing checked-in products.
