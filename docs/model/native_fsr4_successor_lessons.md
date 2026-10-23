<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Native FSR4-style successor lessons

These notes are for the future APUSR-native, highly portable APU upscaler that will be based on the FSR4 INT8 model contract but will not be forced to keep FFX's exact DX12 binding shape forever.

The current reference runner has one job: behave like the imported `ffxint8` INT8 implementation.  For that path, the generated shaders define the binding contract.  If `u11` means `ScratchBuffer` in one dispatch and `rw_rcas_output` in another, the backend must bind a dispatch-local descriptor table and preserve descriptor lifetime until the GPU has consumed the command list.

For a clean-sheet APUSR-native path, the model contract and the API binding contract should be separated more aggressively.

## What is not optional

These rules are GPU/API reality, not FFX-specific institutional baggage:

- A recorded command buffer/list binds concrete descriptor addresses or descriptor-set contents.
- Descriptor contents referenced by in-flight GPU work must not be overwritten before that work retires.
- Resource states/layouts must match the access mode used by each dispatch.
- Constants and upload memory referenced by in-flight GPU work must remain valid until GPU retirement.
- The ML model must receive the tensor/resource contents, quantization constants, recurrent state, history, exposure, depth, motion-vector interpretation, and reset behavior it was trained/exported to expect.

A future backend may make these rules cheaper or cleaner, but it cannot remove them.

## What is optional FFX shape

The following are implementation choices in the imported FFX shader/backend style, not laws of the model:

- Pass-local register reuse, such as `u11` meaning different resources in different shaders.
- Many small generated dispatches sharing a compact root signature.
- Descriptor copying into a per-job/per-pass ring at execution time.
- FFX-specific resource-name tables that are not themselves register-allocation tables.
- DX12-specific root-signature and descriptor-heap lowering.

These are valid and performant enough for the reference path, but they are not necessarily the best long-term APUSR-native design.

## Preferred clean-sheet binding model

For APUSR-native WMMA/NPU/Vulkan/Linux work, prefer a stable logical resource namespace:

```text
input_color
input_depth
input_motion_vectors
input_exposure_or_default
history_color
reprojected_color
recurrent_0
scratch_tensor_arena
initializer_weights
spd_atomic
spd_auto_exposure
rcas_intermediate
output_color
debug_information
```

The model compiler/exporter should emit a backend-neutral manifest containing:

```text
passes
  name
  model stage / utility role
  dispatch shape
  constants
  logical SRV inputs
  logical UAV outputs
  scratch tensor regions
  required barriers/dependencies
  precision/quantization contract
  replacement eligibility: reference | WMMA | NPU island
```

Backends lower that manifest into DX12 descriptors/root parameters, Vulkan descriptor sets or descriptor buffers, and any D3D11 bridge resource views.  The manifest is the source of truth; backend code should not rediscover bindings in the frame path.

## Descriptor policy for future backends

A cleaner native implementation can avoid FFX's confusing register aliasing in two ways:

1. Use stable per-pipeline descriptor indices for logical resources.
2. Use bindless/descriptor-indexed resources where the pass constants provide logical descriptor indices.

For Vulkan, this maps naturally to descriptor indexing or descriptor buffers.  For DX12, it maps to a global shader-visible heap with stable descriptor indices or to per-pass descriptor-table bases generated from the same manifest.  For D3D11 bridge work, the bridge should translate host views to the native backend's logical resource namespace before the model recorder runs.

## Fusion and replacement policy

Pass fusion is allowed only when it is proven against the imported reference dispatches it replaces.  Replacements must state exactly what they replace:

```text
reference pass range -> native implementation
pre -> native pre bookend
middle stages 1-3 -> NPU island
middle stage N -> WMMA shader
post+RCAS -> fused native post/RCAS
```

A fused pass may reduce descriptor churn, barriers, scratch traffic, and CPU dispatch overhead.  It must preserve externally visible model behavior: output, recurrent state, history, exposure behavior, reset behavior, and debug behavior where applicable.

## Current reference-performance lesson

The reference path working correctly does not mean it is performance-equivalent yet.  The current APUSR DX12 reference runtime still carries avoidable overhead relative to `ffxint8`:

- It rebuilds a large backend plan every frame instead of caching stable graph/resource/binding state and updating only frame constants/options.
- It currently materializes descriptors directly for the active frame/dispatch table; the FFX backend keeps CPU descriptor heaps for registered resources and copies descriptors into the shader-visible ring.
- It still uses generic C++ containers and string keys in places where the hot path should use precomputed dispatch records and fixed-size arrays.
- Route tracing is useful once to prove the route, but any FPS number taken with route tracing enabled is not a clean performance measurement.

Correctness came first because wrong descriptor scope caused black frames.  The next reference-runtime work should make the backend both FFX-correct and FFX-shaped in performance: persistent CPU descriptors for stable resources, descriptor-ring copies rather than repeated view creation where possible, cached frame-plan structure keyed by model state, and no per-dispatch string/container work in release.


## Current split in code

APUSR now names two lanes explicitly:

```text
PipelineVariant::StrictReference
PipelineVariant::Dp4aTunedCandidate
```

The strict reference lane remains the oracle and must stay boring.  The DP4A tuned candidate lane starts graph-equivalent to the strict reference lane, then receives experiments only when they are documented as contract-equivalent or intentionally candidate-only.  This avoids cross-contaminating the working drop-in baseline while still letting us move toward the clean-sheet native design.
