<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 reference DX12 shader provider policy

APUSR's FSR4 INT8 reference runner is universal in graph shape, but the runtime must stay performance-first and drop-in friendly.  The reference DLL must be distributable as the canonical FidelityFX provider file:

```text
amd_fidelityfx_dx12.dll
```

It must not require `APUSR_REPO_ROOT`, a build directory, a shader-cache directory, or runtime shader compilation to run in a game.

## Product policy

Product FFX/DX12 builds embed the complete DP4A INT8 reference shader product matrix into the DLL in Windows product builds.  The embedded matrix is generated from the imported `ffxint8` shaders by `tools/model/compile_fsr4_reference_dx12_shaders.py` with:

```text
--preset all --capacity all --product-matrix
```

Source/developer builds no longer carry a checked-in DXIL slice.  Use the packaged-cache target for validation overrides, or let product Windows builds generate and embed the full matrix during DLL compilation.

Runtime provider order is:

1. Optional packaged cache, when explicitly configured.
2. Embedded DLL shader cache.
3. Failure.

Runtime shader compilation is not a product path.

## Cache key

The runtime key for a shader is:

```text
<preset>/<capacity>/p<permutationKey>/<dispatchName>
```

A packaged cache entry uses the corresponding file layout:

```text
<cacheRoot>/<preset>/<capacity>/p<permutationKey>/<dispatchName>.dxil
```

The key includes the full FSR4 permutation key, not just preset and resolution capacity.  This prevents a Balanced/R2160/default DP4A reference blob from being reused for PQ, sRGB, no-auto-exposure, display-resolution motion vectors, jittered motion vectors, create-time debug-visualization capability, WMMA, or other non-matching modes.  Per-frame draw-debug-view toggles the optional debug pass only; it must not produce a different model/PSO key.

## Runtime behavior

`Dx12Fsr4ReferenceShaderProviderConfig` defaults to:

```text
enablePackagedCache = true
enableEmbeddedCache = true
allowRuntimeCompilation = false
requireCompletePrecompiledCache = true
```

An end-user/product path either finds all dispatch shaders in precompiled bytecode or rejects the frame before recording work.  It does not compile shaders in the frame path.

## Pipeline cache keys

Compute PSOs must be cached with the same full key used by the shader provider.  Caching only by dispatch name is invalid because `fsr4_pre`, `fsr4_pass_1`, etc. can represent different compiled bytecode across preset/capacity/permutation combinations.

## Build targets

The single-file product handoff is built through the normal FFX replacement package target:

```powershell
cmake --build build_dx12 --target apusr_package_ffx_dx12_replacement
```

With the default output basename, the canonical DLL is staged as:

```text
<build>/package_ffx_dx12_replacement/amd_fidelityfx_dx12.dll
```

The package target may also stage loader aliases for games that probe different FidelityFX module names, but `amd_fidelityfx_dx12.dll` is the single-file drop-in artifact.

Developer/CI packaged-cache generation remains available:

```powershell
cmake --build build_dx12 --target apusr_compile_fsr4_reference_dx12_packaged_shader_cache
```

That cache is useful for validation and overrides, but it is not required by the bundled product DLL.

## Validation boundary

Reflection manifests are package-validation artifacts.  They are useful for CI and cache generation, but the DX12 reference runtime defaults manifest validation off and only parses a manifest when explicitly configured.  The release frame path must resolve precompiled bytecode and cached PSOs without doing reflection work.

## Future Vulkan note

The cache policy is backend-specific, but the concept should carry to Vulkan: ship precompiled SPIR-V/pipeline artifacts inside the distributable backend package where practical, and keep source compilation/debug reflection out of the end-user hot path.


## Embedded blob compiler note

The embedded product DXIL table is emitted as ordinary `const` static storage, not `constexpr`. Large full-matrix DXIL arrays can trip MSVC constant-evaluation limits when emitted as `constexpr`; these blobs are never used in compile-time expressions and only need static lifetime and read-only storage.
