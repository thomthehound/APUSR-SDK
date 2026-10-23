<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 Reference Import

APUSR carries the FFX `fsr4_model_v07_i8` shader artifacts as a model reference, intentionally kept behind an APUSR pass-graph boundary rather than treated as the project architecture.

## Current location

```text
models/fsr4_int8/reference/fsr4_4_0_2_int8/
  manifest.json
  quality/
    pre.hlsl
    passes_1080.hlsl
    passes_2160.hlsl
    passes_4320.hlsl
    post.hlsl
    initializers.bin
  balanced/
    ...
  performance/
    ...
  ultraperf/
    ...
  native/
    ...
  drs/
    ...
  common/
    spd_auto_exposure.hlsl
    rcas.hlsl
    debug_view.hlsl
```

The imported artifacts are the `ffxint8` INT8/DP4a model-family files. The import includes the preset-specific pre, generated middle passes, post, and initializer payloads, plus shared optional auto-exposure, RCAS, and debug-view shaders.

## Architecture boundary

The model is represented in C++ by:

```text
include/apusr/models/apusr_fsr4_int8_graph.h
src/models/fsr4_int8/apusr_fsr4_int8_graph.cpp
```

That layer knows about:

- model preset selection,
- frame-capacity shader selection (`1080`, `2160`, `4320`),
- the provider-equivalent DP4a dispatch grid,
- which passes are required reference GPU passes,
- which passes are future WMMA candidates,
- which early middle passes are future NPU-island candidates.

That layer must not know about `ID3D12Resource`, `VkImage`, NGX parameter bags, or FFX ABI structs.  Frontends normalize game inputs; backends implement execution; the model graph only describes what needs to run.

## Dispatch shape copied from the reference provider

For the DP4a reference path:

- stage 0 uses `pre.hlsl`, dispatched at `ceil(output / 16x16)`.
- stages 1 through 12 use `passes_<capacity>.hlsl`.
- stage 13 uses `post.hlsl`, dispatched at `ceil(output / 16x16)`.
- optional RCAS uses `rcas.hlsl` after stage 13.
- optional debug view uses `debug_view.hlsl`.
- optional auto exposure uses `spd_auto_exposure.hlsl` before the model passes.

The initial NPU-island candidates are only logical stages 1 through 3.  That matches the current AIE work on the C16/C32 early middle-core area.  Later C64 stages stay GPU-reference until halo and memory-traffic costs are proven.

## Product policy

The direct FFX replacement path is the correctness fixture.  The NGX/DLSS facade is the broad game-compatibility route.  Both run the same APUSR model graph.

The reference runner must execute this imported graph or fail clearly; it must not substitute a synthetic model path.

The reference-path shader sources remain the correctness source. Product-optimized shader variants live under APUSR-owned shader paths and are selected explicitly by the optimized pipeline rather than being folded into the reference path. The current optimized RCAS variant preserves the reference-path RCAS filter math and boundary behavior but hoists the dispatch-constant exposure fetch from four loads per shader thread to one; strict-reference jobs continue to compile `common/rcas.hlsl`.

## Include closure and resource plan

The reference import is now treated as a compile-ready artifact set, not just a
set of top-level model shaders.  The import tool also stages the FFX shader
include closure used by the INT8 graph:

```text
models/fsr4_int8/reference/fsr4_4_0_2_int8/include/
  ffx_core.h
  ffx_common_types.h
  fsr4/
  fsr1/
  spd/
  ml2code_runtime/
  AmdExtD3D/
```

Backends should compile reference FSR4 shaders with at least these include roots:

```text
<preset root>
common
include
include/fsr4
```

The graph layer exposes these through `ReferenceArtifactPaths::includeRoots` so
DX12 and Vulkan can use the same artifact manifest without hard-coding FFX paths
inside backend code.

The import also adds `apusr_fsr4_int8_resources`.  This is the API-neutral resource
plan corresponding to the FFX FSR4 context resources: temporal history surfaces,
RCAS intermediary, SPD auto-exposure resources, initializer buffers, and the
ML2Code scratch buffer.  The resource plan keeps FFX resource identifiers for
reflection/binding patching, but the public description uses APUSR-owned enums
and `SurfaceFormat`, so the model layer remains backend-neutral.

The immediate DX12 reference runner should use this order:

1. Create `GraphDesc` and `ResourcePlan` for the selected preset/max size.
2. Compile/load shader pipelines using graph pass descriptors and include roots.
3. Allocate/import resources from `ResourcePlan`.
4. Load the selected preset initializer payload from loose `initializers.bin` files when a valid repo/model root is available, otherwise from the embedded initializer table, then upload it into `InitializerBuffer`.
5. Dispatch the real graph.  Do not fall back to any synthetic shader/model chain.
