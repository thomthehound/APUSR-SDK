<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# RGP / PIX GPU marker profiling

This is a profiling-only build path for naming APUSR-owned DX12 reference-runtime GPU work in Radeon GPU Profiler and PIX-style captures.

Normal product builds keep marker code compiled out:

```powershell
-DAPUSR_ENABLE_GPU_DEBUG_MARKERS=OFF
```

For an RGP capture build, enable marker support at configure time and opt in at runtime:

```powershell
-DAPUSR_ENABLE_GPU_DEBUG_MARKERS=ON
$env:APUSR_DX12_GPU_MARKERS = "1"
```

When enabled, the DX12 reference runtime emits one outer event:

```text
APUSR_FSR4_FRAME
```

and one nested event per generated FSR4 dispatch:

```text
fsr4_pre
fsr4_pass_0
...
fsr4_pass_12
fsr4_post
fsr4_rcas
```

## WinPixEventRuntime dependency

The marker build uses Microsoft WinPixEventRuntime / `pix3.h` because Radeon GPU Profiler expects PIX3-style DX12 user markers. The full PIX application does not need to be installed, but the WinPixEventRuntime development package must be available when `APUSR_ENABLE_GPU_DEBUG_MARKERS=ON`.

If CMake cannot find it, configure fails with an explicit message instead of a cryptic compiler/linker error. In the normal case, install or restore the `WinPixEventRuntime` NuGet package into the standard NuGet global package cache and configure with only:

```powershell
-DAPUSR_ENABLE_GPU_DEBUG_MARKERS=ON
```

No `APUSR_WINPIX_EVENT_RUNTIME_ROOT` cache flag should be needed when the package is under the normal NuGet cache. CMake searches these locations automatically:

```text
%NUGET_PACKAGES%\winpixeventruntime\*
%USERPROFILE%\.nuget\packages\winpixeventruntime\*
<repo>\deps / external / third_party / packages
<repo>\..\apusr_deps
```

Use an explicit override only for custom/unusual layouts:

```powershell
-DAPUSR_ENABLE_GPU_DEBUG_MARKERS=ON `
-DAPUSR_WINPIX_EVENT_RUNTIME_ROOT="C:\deps\WinPixEventRuntime.<version>"
```

You can also set either environment variable before configure:

```powershell
$env:WINPIX_EVENT_RUNTIME_ROOT = "C:\deps\WinPixEventRuntime.<version>"
# or, if your global NuGet cache is redirected:
$env:NUGET_PACKAGES = "D:\nuget-packages"
```

If `WinPixEventRuntime.dll` is found, CMake copies it next to APUSR shared-library targets for profiling builds. If the DLL is not copied, put the package `bin\x64` directory on `PATH` before launching the game.


## D3D12 Agility SDK headers for RGP annotation mode

WinPixEventRuntime supplies `pix3.h`, but RGP's DX12 user-marker path also needs newer D3D12 header declarations for the user-defined annotation mode API. These declarations are provided by the `Microsoft.Direct3D.D3D12` NuGet package. Without those headers, the runtime CSV status row will report:

```text
annotation_mode=header_unavailable
```

That is not a hard build failure because markers can still compile, but RGP may not show the user marker hierarchy. In the normal case, restore/install `Microsoft.Direct3D.D3D12` into the normal NuGet cache or repo-local `deps` folder and configure with only:

```powershell
-DAPUSR_ENABLE_GPU_DEBUG_MARKERS=ON
```

CMake searches these locations automatically:

```text
%NUGET_PACKAGES%\microsoft.direct3d.d3d12\*
%USERPROFILE%\.nuget\packages\microsoft.direct3d.d3d12\*
<repo>\deps / external / third_party / packages
<repo>\..\apusr_deps
```

Use an explicit override only for unusual layouts:

```powershell
-DAPUSR_D3D12_AGILITY_SDK_ROOT="C:\deps\Microsoft.Direct3D.D3D12.<version>"
```

If the headers are found, CMake prints the include directory and the timestamp reset row should move past `header_unavailable` to a real runtime result such as `annotation_mode=configured`, `unsupported`, or `query_interface_failed`.

## Resource names for RMV/RGP

Profiling builds can also name borrowed and owned DX12 resources with an `APUSR.FSR4.*` prefix so Radeon Memory Visualizer and RGP resource views are easier to read. Resource naming is runtime-gated with the same marker variable, and can also be enabled independently:

```powershell
$env:APUSR_DX12_GPU_MARKERS = "1"       # enables markers and profiling resource names
# or
$env:APUSR_DX12_RESOURCE_NAMES = "1"    # names resources without event markers
```

Resource names are profiling metadata only. Keep `APUSR_ENABLE_GPU_DEBUG_MARKERS=OFF` for normal tuned/distribution builds.

The marker option is deliberately separate from timestamp CSV profiling. Use both when you want a local CSV and named RGP regions:

```powershell
$env:APUSR_DX12_GPU_MARKERS = "1"
$env:APUSR_DX12_GPU_TIMESTAMPS = "1"
```

Do not ship distribution builds with `APUSR_ENABLE_GPU_DEBUG_MARKERS=ON`. Even with runtime environment variables unset, the product policy is to compile debug marker code out of the hot path.
