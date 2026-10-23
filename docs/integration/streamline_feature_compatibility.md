<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Streamline feature compatibility

APUSR preserves the game's stock Streamline loader/API boundary and routes supported features to APUSR-owned implementations at explicit backend-specific seams. This document records the durable compatibility rules for Streamline hosts across supported API revisions.

## Feature ownership

The table below is specifically the **Vulkan optional-feature** ownership contract. DLSS-SR itself now follows the same stock semantic ownership on D3D12 and Vulkan: stock Streamline/`sl.dlss` translate the host contract and APUSR implements NGX, as documented in `dlss_streamline_frontend.md`.

| Feature | Vulkan owner | Contract |
| --- | --- | --- |
| DLSS Super Resolution / DLAA | stock `sl.dlss` -> APUSR NGX facade -> native Vulkan upscaler | stock Streamline semantics; APUSR NGX implementation |
| Reflex low latency | APUSR facade -> native platform low-latency interface when safely supported | APUSR-owned translation |
| PCL markers | APUSR marker sink, with backing PCL forwarding when available | shared/interposed |
| NIS and unrelated optional features | backing Streamline | passthrough |
| DLSS Frame Generation | APUSR facade -> APUSR FI/OF + APUSR presentation owner | APUSR-owned |
| Unsupported NVIDIA-only features | none | not advertised |

Feature ownership must be explicit. Do not infer ownership from vendor branding and do not suppress a cross-vendor service merely because it ships beside NVIDIA-specific plugins.

## Versioned Streamline structures are prefix contracts

Streamline structures are append-only and carry a declared `structVersion`. A compatibility attachment must read only fields guaranteed by the caller's version and write only the prefix/tail owned by the state version supplied by that caller.

This applies especially to Frame Generation options/state, where queue-parallelism and multi-frame-generation fields were added after the original one-generated-frame ABI. Reading a modern tail from an older caller is an out-of-bounds read; writing a modern tail into an older state object is memory corruption.

Capability discovery must also remain distinct from runtime failure. A low-latency dependency that is currently Off is not a runtime failure before Frame Generation has been enabled. Runtime-only errors are reported only once the relevant feature is actually running.

`apusr_streamline_dlssg_abi_contract` protects these boundaries.

## Vulkan queue-role discovery

Some Streamline Vulkan hosts gate Frame Generation before calling `GetState`. They first require non-empty graphics, compute, and optical-flow queue-role lists, and their optical-flow classifier may assume an NVIDIA-shaped queue capability.

APUSR's optical flow is ordinary Vulkan compute. A queue-role alias is therefore an integration shim only:

- advertise the minimum queue-role topology required to reach capability discovery;
- never advertise or enable `VK_NV_optical_flow` unless it is genuinely used;
- never translate APUSR's compute optical flow onto NVIDIA OF commands merely to satisfy discovery;
- revalidate the final logical-device queue counts before reserving APUSR-private queues;
- decline generation if the host consumes all required queue capacity.

Compatibility requirements describe APUSR's execution contract, not the implementation details of the plugin it replaces.

## Low-latency mapping and PCL frame correlation

When a native low-latency interface is available and safe for the current platform, Streamline low-latency options may be translated to it. Do not fabricate features the native API does not provide. In particular, an explicit sleep primitive, camera prediction, or an independently controlled limiter must remain unavailable if the native API has no equivalent.

PCL supplies the useful frame-correlation contract. APUSR owns `slPCLSetMarker` on Vulkan so marker availability does not depend on whether a backing plugin happened to initialize. The real Streamline `FrameToken` is the frame-index authority; APUSR must not maintain a second pointer-to-index numbering origin.

When backing PCL is available, APUSR consumes the marker locally and forwards the same call. The local marker sink and backing service therefore coexist rather than replacing one another.

Low-latency INPUT/PRESENT markers must use the same frame identity. Any platform-specific low-latency teardown behavior that has demonstrated driver/WSI regressions remains explicitly unresolved rather than being hidden behind repeated teardown experiments.

## One innermost real-present seam

APUSR needs one process-level seam immediately adjacent to the real Vulkan present while still preserving backing Streamline's before/after-present hooks.

The durable pattern is:

1. APUSR intercepts direct loader calls and proc-address lookups for `vkQueuePresentKHR`.
2. The stock Streamline interposer remains the outer/backing present callback.
3. APUSR calls the stock interposer's `vkQueuePresentKHR` export exactly once for the mandatory source present; Vulkan entry points are not Detoured by the private Streamline-API attachment.
4. When stock Streamline's device-dispatch path re-enters the APUSR Vulkan-present seam, a thread-local recursion guard identifies the innermost physical-present call.
5. APUSR performs its native marker/presentation work and then calls the original loader/driver function exactly once.

Untracked Vulkan queues bypass this coexistence path. The common captured presentation queue is cached so the per-frame path does not require a mutex merely to decide whether a queue belongs to APUSR.

## Frame Generation presentation ownership

The Frame Generation facade is not a capability stub. APUSR owns the feature state, resource translation, FI/OF execution, and generated/source presentation pair.

The presentation architecture uses APUSR-owned logical source storage and keeps physical WSI images private to the presentation path. Generated physical presents bypass backing Streamline; the mandatory source present traverses the backing present seam exactly once so source-frame PCL semantics remain intact.

APUSR-private queues are required for asynchronous work. Merely observing an extra application queue does not make it safe to submit APUSR work from another thread. If a private queue cannot be reserved at device creation, the asynchronous mode is unavailable rather than racing application `VkQueue` access.

A nonblocking-client-queue mode is an **input-lifetime** contract. The host receives an APUSR-owned completion synchronization point that proves tagged inputs may be reused. It must not be implemented as a full Frame Generation completion fence that serializes the game against all FI/OF work.

## Upscaler color and WSI color are separate contracts

The upscaler may consume HDR or pre-tonemap input while the physical swapchain remains an SDR presentation surface. Frame Generation must derive its transfer function and presentation support from the actual WSI color-space/format contract, not from the upscaler's input-color option.

Unsupported Vulkan color spaces fail explicitly until APUSR has the metadata and implementation needed to handle them correctly.

## Cross-backend coexistence

PCL is not inherently NVIDIA-only and may remain available through backing Streamline on D3D12. By contrast, a low-latency feature must stay hidden until APUSR has a real D3D12 translation for it. The same ownership rule applies to every optional feature: expose it only when there is a correct owner for the active backend.
