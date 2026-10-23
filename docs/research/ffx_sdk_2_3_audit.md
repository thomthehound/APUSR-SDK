<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FidelityFX SDK 2.3.0 comparison (research snapshot)

This snapshot compares APUSR's 4.0.2 source reference against FidelityFX SDK 2.3.0 and its FSR 4.1.1 binaries. It records findings from that SDK release, not the latest FSR release or a current APUSR support statement. The purpose is narrow: identify changes that are useful to APUSR on RDNA 3/RDNA 3.5-class hardware without replacing a working open-source FSR4 implementation with newer opaque binaries or importing unrelated SDK architecture.

## Bottom line

There is no newer open-source FSR4 implementation to transplant. FidelityFX SDK 2.3 documents FSR Upscaling 4.1.1, but the implementation is distributed as the signed `Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll`. The older 4.0.2 source used by APUSR therefore remains the most useful open implementation for the model/pipeline itself.

The SDK comparison identified four relevant areas:

1. one FSR4 correctness fix to reproduce in APUSR;
2. Frame Generation API/reset behavior worth accepting compatibly;
3. Frame Generation swapchain lifetime and pacing fixes worth backporting;
4. a few implementation fixes that can be judged independently of the newer closed model.

## FSR Upscaling 4 changes

### 4.0.3: non-multiple-of-eight output correctness

FidelityFX SDK 2.1.0 records a 4.0.3 fix for a rendering error when surface dimensions are not multiples of 8. This is directly relevant because APUSR's 4.0.2-derived path still aligns internal dimensions to eight.

This should **not** be implemented as a blind padding change. The likely permanent shape is to keep aligned allocation/dispatch extents while preserving the logical active output extent for bounds, coordinates, and edge behavior. The exact 4.0.3 source is not present in the newer SDK, so APUSR must prove its own implementation with odd-size output cases rather than claiming AMD's exact fix.

Good regression dimensions include `1919x1079`, `2559x1439`, and other independently odd width/height combinations. The test must validate the final image edge, not merely prove that allocation or dispatch succeeds.

### 4.1.0: shader-performance and model-quality notes

FidelityFX SDK 2.2.0 says 4.1.0 updated inference quality in some titles and optimized several shaders. The implementation is not included as source, so there is nothing responsible to copy into APUSR. Treat these release notes as evidence that later tuning exists, not as a specification from which to invent changes.

### 4.1.1: RDNA 3 discrete support

FidelityFX SDK 2.3.0 adds FSR Upscaling 4.1 support for Radeon RX 7000-series discrete GPUs. This is useful confirmation that AMD's current binary path can target RDNA 3, but it does not expose the implementation changes and it does not provide an APU implementation for APUSR to inherit.

### Upscaler API-version negotiation

The newer upscaler header adds the required `ffxCreateContextDescUpscaleVersion` create-context descriptor and defines the current API layout as 4.1.1. The SDK 2.3 FSR sample chains that descriptor into context creation and sets it to `FFX_UPSCALER_VERSION`. APUSR's older FSR edge did not recognize it, which means a current SDK caller could be rejected before reaching an otherwise compatible 4.0.2-derived implementation.

APUSR now recognizes the descriptor without pretending that its model/provider implementation has become 4.1.1. An omitted descriptor preserves the legacy 4.0.2 contract; explicit versions from 4.0.2 through the current 4.1.1 API layout are accepted; invalid older or future versions fail with `FFX_API_RETURN_ERROR_PARAMETER`. This follows the inspectable version-negotiation policy used by the newer Frame Generation swapchain provider: preserve the last pre-versioned ABI when the descriptor is absent, accept the supported range, and reject contracts the provider cannot promise. Provider identity remains 4.0.2.

The same pass also moves SDK 2.3's null-context `GlobalDebug`/`GlobalDebug1` handling and the two new surface formats (`R32G32_UINT`, `R8_SNORM`) into the **FSR upscaler host edge**, rather than leaving those compatibility details only in Frame Generation-local code. Global debug is handled before context lookup, matching the generic SDK loader, and APUSR's ABI smoke proves the configured callback actually receives a rejected-version error.

### Resource-query audit

The open 4.0.2 provider was rechecked rather than inferred from newer documentation. Its upscaler resource-requirements query reports color, depth, motion vectors, and exposure as required and reports no optional resources. APUSR already matches that contract exactly, including when auto exposure is enabled; there is no resource-requirements fix to make here.

The GPU-memory-usage query is different: the open provider computes real totals from its resources, while APUSR's current FSR edge still reports zero. That is a genuine compatibility/observability gap. It should be implemented from APUSR's native resource plan during the performance/memory pass so the query and the allocator describe the same resources rather than maintaining a second bookkeeping model.

### Current published performance and memory figures

The 4.1.1 documentation reports the following example GPU times:

| GPU | Target | Time |
| --- | ---: | ---: |
| RX 9070 XT | 3840x2160 | 1316 us |
| RX 9070 XT | 1920x1080 | 352 us |
| RX 7900 XTX | 3840x2160 | 3070 us |
| RX 7800 XT | 2560x1440 | 2050 us |
| RX 7600 | 1920x1080 | 1998 us |

It also reports an approximate working set of 1274 MB at 8K, 318 MB at 4K, and 81 MB at 1080p on an RX 9070 XT. These are useful comparison points, not direct APUSR targets: APUSR uses a different open implementation and APU memory pressure is one of the project's primary constraints.

## Frame Generation 3.1.6 changes

### PrepareV2

The old `ffxDispatchDescFrameGenerationPrepare` and linked camera structure were replaced by `ffxDispatchDescFrameGenerationPrepareV2`. The V2 description carries reset and camera vectors directly.

APUSR accepts both layouts. Legacy `unused_reset` remains ignored because AMD explicitly never implemented that field; V2 `reset` is live. This avoids changing the behavior of existing 3.1.5 callers while accepting newer callers without a fake version claim.

### Frame-ID discontinuities

The newer provider resets Frame Generation when frame IDs become discontinuous. APUSR now mirrors that behavior: decreasing IDs, skipped IDs, or a generation ID inconsistent with the last configuration force the interpolation/optical-flow history reset.

### ABI shape and version descriptors

The SDK 2.3 headers make several compatibility details explicit that are useful even while APUSR continues to advertise the proven 3.1.5 provider contract. APUSR's local FFX edge now uses the newer effect/backend descriptor-ID partition (`0x00ff0000` effect bits and `0xff000000` backend bits), recognizes the newer Frame Generation and swapchain version descriptor layouts, and exposes the SDK 2.3 `PrepareV2` layout directly instead of maintaining a private shadow declaration. The numeric values of the existing D3D12 descriptors are unchanged.

This is deliberately **recognition, not version inflation**. SDK 2.3 also introduces a newer stable swapchain COM/internal-backend ABI. APUSR has not implemented that whole contract and therefore still reports the established 3.1.5 provider identity rather than claiming 3.1.6/3.1.7 support from a handful of compatible descriptors.

### Present callback extension

SDK 2.3 carries premultiplied-alpha presentation state in a `pNext` extension on the Frame Generation present callback. APUSR retains the descriptor definition locally, but the currently advertised 3.1.5 provider does **not** emit the newer extension. That keeps the active callback contract identical to the proven 3.1.5 path.

This is deferred functionality, not dead ABI. When APUSR implements the newer Frame Generation version-negotiation descriptor, the FFX compatibility edge should emit/accept the premultiplied-alpha extension only for the negotiated newer contract. The extension is decoded once into APUSR's existing direct `usePremulAlphaComposite` state; internal scheduling and DX12/Vulkan backends should not walk `pNext` chains. The required rendering distinction is real: straight-alpha UI multiplies source RGB by alpha during composition, while premultiplied-alpha UI must not multiply it a second time.

### Global debug configuration

The newer generic API accepts both `GlobalDebug` and legacy `GlobalDebug1` configuration without requiring an effect context. APUSR's localized host entry points now match that behavior. This matters during provider/debug setup, where a caller can establish the message callback before creating either a Frame Generation or FSR Upscaling context. The FSR edge handles these descriptors at the loader level, before context/provider routing, just as SDK 2.3 does.

### Surface-format additions

SDK 2.3 adds `R32G32_UINT` and `R8_SNORM` to the public surface-format enum. APUSR appends the same values without renumbering the older ABI and now carries both formats through the FSR compatibility boundary, native surface-format vocabulary, and D3D12 conversion/resource helpers. The local DX12 resource helper also treats `D3D12_RESOURCE_DIMENSION_UNKNOWN` defensively, matching the newer SDK.

## Frame Generation Swapchain 3.1.7 changes

### Frame-latency waitable object

3.1.7 fixes a shutdown race in `GetFrameLatencyWaitableObject()` and specifies that every call returns an independent handle whose lifetime belongs to the caller. Its internal pacing implementation uses a semaphore and signals that semaphore from GPU completion.

APUSR now has its own DX12 `FrameLatencyWaitable` implementation with those semantics:

- the internal object is a semaphore;
- every public query returns a duplicated handle;
- the caller may close one duplicate without invalidating another;
- swapchain shutdown closes APUSR's internal infrastructure without invalidating caller-owned duplicates;
- shutdown releases the semaphore so surviving waiters are not stranded;
- each runtime latency credit is released only after the current frame's GPU-completion event fires;
- completion registrations use a bounded pool of distinct events so several completions cannot coalesce into a single auto-reset event before the worker consumes them.

APUSR deliberately does **not** copy the 3.1.7 worker's `SignalObjectAndWait(semaphore, completionEvent, ...)` ordering. Signaling the semaphore first manufactures a credit before completion, and the upstream initially-signaled event can manufacture another startup credit. APUSR waits for completion first and then releases one semaphore token. The owned implementation also avoids reusing one binary event for every in-flight frame, because several fence completions can otherwise collapse into one signal and permanently lose latency credits. The Windows contract test checks exact startup/latency-credit accounting, three simultaneously-ready D3D12 fence completions, handle independence, and shutdown behavior. Actual command-queue/GPU completion sequencing still requires the real Windows runtime test because this development environment does not contain the Windows SDK.

### Fence wait in the present path

The newer path avoids waiting on the interpolation-completion fence for a real frame that is already queue ordered. APUSR carries the same distinction: the generated frame performs the additional interpolation completion wait; the real frame does not.

### Output selection bug

The newer DX12 helper does not release the current valid `IDXGIOutput6` before a replacement `QueryInterface` has succeeded. APUSR backports that fix so a failed query cannot discard the last valid output while walking monitor candidates.


### DX12 backend comparison

The newer SDK's generic DX12 backend is **not** a blanket upgrade for APUSR. The older backend APUSR started from already contains several CPU-side optimizations that are absent from the supplied SDK 2.3 backend: it copies only the live portions of scheduled compute jobs instead of assigning the entire job structure, caches root-signature/pipeline/descriptor-heap binds across consecutive jobs, caches descriptor increments/heap starts, and batches descriptor copies. APUSR therefore keeps those faster paths while localizing the backend.

This comparison also reinforced the decision to narrow the backend rather than replace it with current generic FidelityFX code. APUSR now carries a native FI/OF backend interface, native resource/pipeline/shader/job payloads, and native resource-state/format/usage/sampler vocabulary while preserving these proven scheduling and descriptor-binding optimizations. FFX resource descriptions are normalized at the compatibility edge before reaching the backend; newer generic SDK breadth is not a target in itself.

The FFX algorithm provider also no longer owns backend-specific D3D12 knowledge. Backend `pNext` parsing is isolated behind an APUSR compatibility factory, and swapchain configuration is invoked through an API-neutral APUSR contract. This is an intentional Vulkan preparation step: the external FFX vocabulary can select a backend while the FI/OF provider and native execution contract remain independent of the selected graphics API.

FI/Optical Flow context creation, Prepare, dispatch, shared-resource descriptions, and memory accounting are now expressed through APUSR-native algorithm descriptors too. FFX algorithm structures are compatibility wrappers around those entry points rather than the structures used by the active provider. This matters more than terminology: the product hot path no longer constructs an FFX FI/OF dispatch description only to convert it back into APUSR resources before execution, and a Vulkan provider can reuse the same native algorithm contract directly.

## Changes deliberately not copied

### Explicit UAV transitions before discard

Newer Frame Interpolation source adds explicit UAV transitions before discarding aliasable resources. APUSR's DX12 backend already performs the UAV transition and barrier flush inside its discard-job executor. Adding the newer front-end transitions would duplicate work and reverse an earlier barrier cleanup, so this change is intentionally not copied.

### New binary FSR4 model/inference

No attempt is made to infer, extract, or imitate implementation details hidden in the signed 4.1.1 upscaler DLL. APUSR continues to optimize the open 4.0.2-derived INT8 pipeline it can inspect and validate.

### Full 3.1.6/3.1.7 ABI advertisement

Behavioral backports and recognition of newer descriptor layouts do not justify a new compatibility version. APUSR remains 3.1.5-compatible until the newer stable swapchain COM/internal-backend ABI and its complete version-negotiation contract are implemented and validated together.

### New explicit barrier-job contract in isolation

SDK 2.3's Frame Interpolation algorithm schedules explicit UAV transition jobs before aliasable-resource discard, while its newer DX12 backend no longer performs that transition as part of discard execution. APUSR's native job contract can represent barriers, but the currently proven algorithm/backend semantics intentionally retain the older coherent behavior: the DX12 discard executor performs the UAV transition and flush itself. Adopting only the newer backend half would remove a required synchronization operation; adopting only the newer front-end half would duplicate it. If APUSR adopts the newer model, the FI scheduling and backend discard semantics must change together and be image/performance validated as one synchronization-contract change.

## Portability lesson from the newer SDK

The newer SDK remains strongly D3D12-shaped and does not supply the Vulkan frame-generation/backend implementation APUSR needs. The useful lesson is therefore not to adopt more SDK architecture. APUSR's frame scheduling/pacing policy should stay API-neutral, with D3D12, Vulkan, and later D3D11-facing adapters owning only their native queue, synchronization, swapchain, and resource operations.

## Actions

Already adopted at the shared/FSR compatibility edge:

- SDK 2.3 effect/backend descriptor-ID partition while preserving existing wire values;
- FSR Upscaling API-version descriptor recognition: legacy 4.0.2 by default, explicit 4.0.2 through 4.1.1 accepted, unsupported contracts rejected without inflating the 4.0.2 provider identity;
- null-context `GlobalDebug`/`GlobalDebug1` configuration at the FSR host edge with a functional callback path;
- `R32G32_UINT` and `R8_SNORM` carried through FFX, APUSR-native, and DX12 format vocabularies;
- source-verified 4.0.2 resource-requirements semantics retained unchanged.

Already adopted in the current APUSR Frame Generation work:

- public local PrepareV2 ABI without changing legacy reset semantics;
- SDK 2.3 premultiplied-alpha callback descriptor definition, held inactive while the provider remains 3.1.5;
- discontinuous-frame-ID reset;
- 3.1.7 independent frame-latency handle semantics and GPU-completion signaling;
- generated-frame-only extra interpolation fence wait;
- safer DXGI output replacement;
- no redundant discard barriers.

Still worth doing after this ownership pass:

- make the FSR GPU-memory-usage query report the native APUSR resource plan instead of zero;
- finish negotiated Frame Generation 3.1.6/3.1.7 compatibility and then enable the premultiplied-alpha present `pNext` extension at the FFX edge;
- implement and image-validate the non-multiple-of-eight FSR4 output fix without conflating active image extents with aligned tensor/dispatch extents;
- revisit 4.1-era quality/performance only when an inspectable implementation or independently demonstrated optimization exists;
- implement Vulkan from APUSR's portable scheduling/resource contracts rather than waiting for FidelityFX to supply it.
