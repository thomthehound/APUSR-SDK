<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 CPU/DP4A tuning history

This document records a reference/tuning experiment, not an independently selectable production pipeline. The CPU/DP4A tuned candidate was an early parallel experiment lane.  The name is intentionally broad: early wins may come from CPU recording/descriptor/PSO overhead as much as from shader-side DOT4/DP4A changes.  It is intentionally separate from the strict reference baseline.

Working name in code:

```text
PipelineVariant::Dp4aTunedCandidate
UpscalerExecutionPath::GpuFsr4Int8Dp4aTuned
```

This remains an API-neutral experimental/reference lane used by planning and regression tests. It is no longer a deployable CMake frontend configuration: the product FFX/DLSS frontends always select `PipelineVariant::Dp4aOptimized`, which now includes the recorder optimizations that survived this experiment.

The candidate graph remains mathematically identical to the strict reference graph.  The first implemented tuning pass is CPU/backend-side only: the DX12 recorder now precomputes per-dispatch PSO keys, descriptor-table bases, and resource-transition lists for this lane so it does not search the full dispatch/descriptor patch plan during command recording.

## Purpose

The candidate lane is for DOT4/DP4A-side experiments that may improve performance while preserving the FSR4-style model contract.  It can try work such as:

- descriptor and record-plan specialization beyond the strict reference path
- CPU-side plan caching, descriptor-copy reduction, PSO lookup cleanup, and upload packing
- shader-side DOT4/DP4A scheduling changes
- pass fusion when tensor/resource contracts are proved equivalent
- alternate scratch packing
- reduced dispatch/barrier overhead
- backend-neutral binding-manifest cleanup


## Implemented tuning pass 1: record-plan specialization

The CPU/DP4A tuned DX12 recorder uses fields precomputed in `Dx12Fsr4ReferenceDispatchRecordPlan`:

```text
pipelineCacheKey
srvDescriptorTableBase
uavDescriptorTableBase
resourceTransitions
```

This removes command-recording hot-path scans that strict reference still keeps for conservatism:

```text
recordPlanForDispatch(): dispatch-name scan
descriptorTableBaseForDispatch(): descriptor-patch scan for SRV/UAV table bases
transitionResourcesForDispatch(): descriptor-patch scan and per-dispatch state merge
```

The tuning is contract-equivalent.  It does not change shader bytecode, constants, descriptor contents, resource state targets, dispatch grids, model weights, initializer handling, or reset/history/exposure behavior.  It only moves derivation work from command recording into the already-built backend plan.

## Rules

The strict reference path remains the oracle.  Candidate changes must state exactly what they replace and how they are verified.

Allowed candidate changes:

- reduce CPU recording overhead
- reduce descriptor copies or descriptor table pressure
- reduce per-frame container/string work and redundant state derivation
- reduce barriers without weakening dependencies
- specialize DP4A shader variants
- fuse passes only with explicit reference comparison

Not allowed without a new non-reference model contract:

- changing input feature interpretation
- changing pass-0 weights or initializer semantics
- changing model tensor values without proving equivalence
- changing recurrent/history/exposure/reset semantics
- hiding failures behind passthrough or fake output

## Portability goal

The candidate lane should stay API-neutral as long as possible.  A DX12-only optimization may be useful, but the preferred shape is:

```text
candidate model/pass plan
  -> DX12 lowering
  -> Vulkan lowering
  -> D3D11 bridge lowering
```

Any useful DX12 optimization that is not intrinsically DX12 should be represented in the API-neutral plan so Vulkan and D3D11 bridge work inherit it automatically.

## Implemented tuning pass 2: descriptor template + persistent constants

The CPU/DP4A tuned DX12 lane now keeps the strict-reference command contract but removes two pieces of per-frame backend overhead:

- Internal/persistent SRV/UAV descriptors are prebuilt into a CPU-only descriptor-template heap whenever the resource generation or descriptor layout changes. Each frame copies that template into the active shader-visible descriptor ring slot, then patches only borrowed game resources that can change per frame.
- The tuned lane persistently maps the constants upload ring and reuses the mapped pointer instead of mapping and unmapping the upload buffer every frame. Root CBV binding uses the precomputed current GPU base address for the frame.

This does not change shader bytecode, pass order, constants, resource states, descriptor lifetime, or model math. It is a backend CPU-overhead reduction only. Strict-reference remains available as the frozen oracle lane.

## Implemented tuning pass 3: native dispatch cache + constant-binding offsets

The CPU/DP4A tuned DX12 lane now removes a few more command-recording hot-path lookups without changing the GPU contract:

- Per-dispatch root-CBV upload byte offsets are populated into `Dx12Fsr4ReferenceDispatchCbvBinding` when the backend plan is built. Dispatch recording no longer scans the constant-upload slot list to bind each CBV.
- The tuned lane builds a native dispatch cache keyed by preset/capacity/permutation/dispatch shape after PSO preparation. The recorder uses the cached `ID3D12PipelineState*` sequence instead of hashing string PSO keys during every dispatch.
- Descriptor patches now carry an `externalBorrowed` flag derived from the resource plan. Tuned descriptor preparation uses that flag directly instead of rediscovering borrowed-resource ownership by scanning realized resources while patching descriptors.
- Tuned record-plan order checks are compiled under `APUSR_ENABLE_HOT_PATH_GUARDS`; release builds trust the precomputed plan rather than spending cycles on invariant checks during every dispatch.

This is intentionally still CPU/backend-only. It does not change shader bytecode, dispatch grids, descriptor contents, descriptor lifetime, resource state targets, constants, weights, recurrent/history/exposure/reset behavior, or output math. The strict reference lane keeps the conservative lookup path.

## Implemented tuning pass 4: fast resource/state lookup

The CPU/DP4A tuned DX12 path now keeps the descriptor-template and native-record caches from earlier passes and also removes several small per-dispatch backend lookups:

- realized resources are indexed by `ResourceId` after each frame/resource-plan update instead of repeatedly scanning the realized-resource vector while recording transitions and descriptors;
- internally owned native resources are cached by `ResourceId`, avoiding unordered-map lookups for every transition/view creation;
- internally owned resource states are cached by `ResourceId`, so the transition path no longer hashes the owned-resource state map on every dispatch transition.

This is intentionally a conservative cleanup. It does not change descriptor contents, descriptor lifetime, resource barriers, pass order, PSOs, constants, shaders, weights, or reset/history/exposure behavior. It may be smaller than the descriptor-template win, but it keeps the tuned lane moving toward a backend that records from compact precomputed state rather than generic maps and scans.
