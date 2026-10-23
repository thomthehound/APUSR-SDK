<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Building APUSR

## Toolchain

APUSR uses CMake and C++23. The primary product build is Windows x64 with a modern MSVC toolchain and Ninja. Python is used for shader/model generation and validation.

A normal configuration does not require the experimental XRT/NPU backend.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAPUSR_ENABLE_XRT_BACKEND=OFF
```

## Product targets

### FidelityFX-compatible D3D12 package

```powershell
cmake --build build --target apusr_package_ffx_dx12_replacement -j
```

This is the lightweight D3D12 package and does not require Vulkan shader-pack generation.

### NGX/DLSS-compatible D3D12 package

```powershell
cmake --build build --target apusr_package_dlss_compatibility -j
```

### Streamline/DLSS-compatible Vulkan package

```powershell
cmake --build build --target apusr_package_streamline_vulkan_compatibility -j
```

This target builds the native Vulkan runtime and the self-indexed SPIR-V shader product required by it.

### Combined D3D12 + Vulkan package

```powershell
cmake --build build --target apusr_package_dlss_multibackend_compatibility -j
```

Use this for hosts that can select either graphics API while retaining one staged compatibility installation.

## Shader products

D3D12 and Vulkan product shaders are compiled offline. Runtime code does not compile model shaders or use source-path names as shader identity.

The Vulkan build emits one self-indexed shader pack containing the supported logical model/permutation matrix. Logical shader identity is derived from model/runtime keys; physical SPIR-V blobs are deduplicated by generated content.

Shader compiler parallelism is automatic by default. `APUSR_SHADER_COMPILE_JOBS` can override the worker count when memory pressure or CI scheduling requires a fixed value.

## Optional components

### Profiler

The structured profiler is controlled by the relevant `APUSR_ENABLE_*` build options and runtime environment variables documented in [profiling.md](profiling.md).

### GPU debug markers

RGP/PIX marker support is optional and must not become a dependency of ordinary distribution builds. When enabled, the Windows build may require WinPixEventRuntime and compatible DirectX 12 headers.

### NPU/XRT

`APUSR_ENABLE_XRT_BACKEND` and `APUSR_BUILD_AIE_KERNELS` are experimental. Keep them disabled unless actively working on the NPU lane and the required XRT/MLIR-AIE toolchain is installed.

## Quality suite

Run the repository regression suite with:

```powershell
python .\tools\quality_check.py
```

The script configures a clean test build by default, builds the quality targets, and runs the CTest `quality` label.
