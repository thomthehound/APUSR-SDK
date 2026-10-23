<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 reference DX12 shader / PSO plan

The DX12 reference backend has a real DXIL seam for the imported FSR4 INT8 model.  The repo no longer carries a checked-in DXIL cache slice.  Developer/source builds either use an explicitly generated packaged cache or, on product Windows builds, a build-time generated full embedded product shader table.

The model/runtime plan remains parameterized by:

- quality preset: native, quality, balanced, performance, ultra-performance, dynamic-resolution
- frame-capacity class: 1080, 2160, 4320
- DP4A/reference versus future WMMA flavor
- inverted-depth flag
- low-resolution versus display-resolution motion vectors
- auto-exposure flag
- jittered-motion-vector flag
- input color-space flags: linear, nonlinear, sRGB, PQ
- debug visualization flag

The runtime validates shader blobs by `preset + frameCapacity + permutationKey + dispatchName`. That is intentional. A Balanced/R2160 blob must never be accepted for another mode just because the entry-point name matches.

## Shader-bytecode sources

Product FFX/DX12 builds embed the complete DP4A reference shader matrix in the DLL in Windows product builds.  External packaged-cache targets remain available for shader-development workflows.

Generate the developer/CI packaged cache with:

```powershell
cmake --build build --target apusr_compile_fsr4_reference_dx12_packaged_shader_cache
```

Verify that packaged cache with:

```powershell
cmake --build build --target apusr_verify_fsr4_reference_dx12_packaged_shader_cache
```

## Matrix preflight

The shader source preflight intentionally defaults to a quick all-preset/all-capacity check. This keeps developer time bounded while preventing the reference backend from drifting into a single test-bed mode.

```powershell
cmake --build build --target apusr_verify_fsr4_reference_shaders
```

For a full pass over one selected slice:

```powershell
python tools/model/verify_fsr4_reference_shaders.py `
  --repo-root . `
  --preset quality `
  --capacity 4320 `
  --include-utility
```

For a non-default feature permutation:

```powershell
python tools/model/verify_fsr4_reference_shaders.py `
  --repo-root . `
  --preset balanced `
  --capacity 2160 `
  --display-res-motion-vectors `
  --no-auto-exposure `
  --color-space pq `
  --jittered-motion-vectors `
  --quick
```

## Product-safe rule

The runtime must not assume a narrow bring-up slice exists.  It should choose one of these product-safe paths:

1. packaged cache generated for the declared mode matrix;
2. generated full embedded product shader table; or
3. explicit developer runtime compilation outside the product hot path.

In all cases, lookup remains keyed by the full FSR4 permutation key.

## Shader provider update

The DX12 runner has an explicit shader-provider plan. The fastest/default path is precompiled bytecode: packaged cache first, generated embedded product table second, no runtime compilation on the product hot path. Shader and PSO lookup keys include preset, frame-capacity, full FSR4 permutation key, and dispatch name.

Runtime compilation remains a developer-only fallback and is deliberately not wired into `recordFrame()` yet. This keeps the reference runner from accidentally introducing first-run stutter or per-mode compile stalls on end-user systems.
