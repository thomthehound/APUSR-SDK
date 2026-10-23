<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Streamline DLSS frontend

## Renderer-restart Reflex seam

Some hosts rebuild the plugin graph for a Vulkan -> D3D12 renderer switch without a second `slInit`. APUSR remembers only the latest explicit Reflex request and uses stock D3D12 `sl.dlss` startup as the cold rearm point. Streamline supplies a native `ID3D12Device*` there; the private attach bind is idempotent for an already-current device. Normal `slShutdown` still deinitializes Anti-Lag and detaches the prior Streamline image, so unload/reload safety is preserved and DLSS-SR semantics remain stock.

APUSR does **not** replace the game's Streamline **semantics**. The normal product path leaves the game's stock `sl.interposer.dll` at the loader boundary. A separate optional bootstrap exists only for hosts that enter `slInit` before any APUSR module can otherwise become visible: it preserves the exact stock PE as `sl_interposer_apusr_stock.dll`, forwards every export except `slInit` directly to that PE with the same names and ordinals, and uses `slInit` only to establish the private attachment before entering the same stock implementation. APUSR therefore keeps stock Streamline as the DLSS semantic owner on both D3D12 and Vulkan even when the optional loader alias is active.

```text
D3D12
stock sl.interposer.dll / stock sl.common / stock sl.dlss
    -> host-revision Streamline tags/options/resource translation
    -> unwrap Streamline D3D12 command-list/device proxies
    -> ordinary NGX calls
APUSR nvngx_dlss.dll
    -> APUSR frontend normalization
    -> native D3D12 FSR4 INT8 backend

APUSR early D3D compatibility hooks
    -> discovery/vendor visibility only
    -> do not own DLSS tags/options/resources/evaluate

Vulkan
stock sl.interposer.dll / stock sl.dlss
    -> host-revision Streamline tags/options/resource translation
    -> ordinary NGX calls
APUSR nvngx_dlss.dll
    -> APUSR frontend normalization
    -> native Vulkan FSR4 INT8 backend

APUSR apusr_streamline_attach.dll
    -> named-export Detours on the already-loaded stock interposer
    -> Vulkan DLSS requirement sanitization / host-capture bootstrap
    -> APUSR-owned Vulkan Reflex / DLSS-G compatibility
    -> D3D12 Reflex/PCL -> AMD Anti-Lag 2 only when Reflex is requested
```

By default the game's stock Streamline files remain at their normal loader boundary. When the optional early-bootstrap alias is required, only the `sl.interposer.dll` loader name changes; the exact stock interposer bytes remain the module to which APUSR attaches and the module that executes Streamline semantics. The private attachment resolves optional API entry points by exported name and simply leaves an entry point unattached when the loaded host revision does not provide it.

## Why stock Streamline remains the semantic owner

APUSR deliberately leaves Streamline semantics with the host-selected Streamline revision:

- the game-selected Streamline revision owns its exact PE/export/ordinal/loader/plugin-manager contract, so replacing it turns APUSR into the compatibility owner for every host revision it encounters;
- on D3D12, Streamline's common evaluator unwraps its private device/command-list proxies, establishes the borrowed-resource states expected by `sl.dlss`, and restores the host pipeline afterward; APUSR therefore must not replace `slEvaluateFeature` or bypass that contract;
- stock `sl.dlss` already owns the host revision's tag/options/resource-to-NGX translation on both D3D12 and Vulkan, so reimplementing it would create a second semantic path that can drift from the game integration;
- keeping stock Streamline lets APUSR stay at narrower compatibility boundaries (identity/discovery, NGX implementation, and renderer-specific feature translation) instead of becoming responsible for Streamline startup/teardown and every unrelated plugin.

Stock Streamline's automatic Vulkan device negotiation is intrinsically NVIDIA-specific where plugin-private requirements force `VK_NVX_*` / `VK_NV_low_latency` before the real AMD `vkCreateDevice` call. APUSR bypasses only that device-negotiation seam and uses Streamline's documented manual `slSetVulkanInfo` path while retaining stock plugin semantics.

**A Streamline fork is justified only if the narrow manual-device boundary cannot remain stable without host-revision-specific detours.** Any fork must preserve Streamline's evaluator/plugin lifecycle and change only the cross-vendor device-negotiation policy; it is not permission to duplicate Streamline semantics.

## D3D12 compatibility contract

D3D12 preserves the stock Streamline **semantic execution path** and, on the normal install, the stock loader boundary as well. If a multi-backend host requires the optional early-bootstrap alias for its Vulkan startup order, D3D12 still forwards into the exact preserved stock interposer and its preferences are not filtered. APUSR may relax the NVIDIA-only discovery/vendor gates needed to make the DLSS feature visible on AMD hardware, but it does not replace Streamline's DLSS options, tag/constant ownership, resource allocation, or `slEvaluateFeature` path.

This ownership boundary is required for correctness. Streamline's D3D12 common evaluator recognizes its private command-list proxy, unwraps it to the native `ID3D12GraphicsCommandList` before invoking `sl.dlss`, and restores the host pipeline after evaluation. Stock `sl.dlss` then performs the expected tagged-resource lookup, state transitions, and NGX parameter translation. On NVIDIA hardware, stock `sl.common` normally supplies an `NGXContext` backed by NVIDIA's loaded core. On the APUSR D3D12 compatibility path, the NVIDIA static client can reject an unsigned replacement core during its own signature verification before `LoadLibrary` is called. APUSR therefore supplies the same narrow context contract through `sl.param.global.ngxContext`: APUSR capability parameters plus create/evaluate/release/update callbacks. The stock evaluator still invokes those callbacks only after it has produced the native command list and prepared resources. The binding is epoch-scoped: if the host unloads/reloads Streamline or rebuilds its parameter bus during a quality-mode change, the context is republished into the current bus before stock `sl.dlss` startup. A successful earlier startup is never treated as a process-global latch.

APUSR must never treat a Streamline proxy device as the native execution device or bypass Streamline's proxy-unwrapping and resource-state contract. Doing so violates D3D12 device ownership and can surface as device removal or renderer recreation.

The stock evaluator also establishes the borrowed-resource state contract immediately before entering NGX. Its D3D12 `chi::eTextureRead` inputs are in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` and its `chi::eStorageRW` output is in `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`. The APUSR context bridge marks this provenance only for the duration of its synchronous NGX evaluation, and the NGX frontend transfers those native states into the backend frame description. Direct NGX callers do not inherit the marker. Likewise, game-owned D3D12 resource descriptors are authoritative for format/view construction; frontend default formats must never override a concrete `ID3D12Resource::GetDesc().Format`. External borrowed descriptor compatibility is a release-path correctness check because D3D12 SRV/UAV creation is void-returning.

A device-loss circuit breaker guards the compatibility seam. If one APUSR NGX evaluation leaves the native command-list device removed, the bridge is disabled for the remainder of that process and later Streamline reloads are not reinjected. This protects the host from an infinite renderer-recreation loop; it does not make the failed evaluation acceptable or close the underlying backend bug.

`apusr_streamline_attach.dll` does not own D3D12 DLSS-SR. D3D12 still uses the early DXGI/NVAPI identity layer for discovery/vendor visibility and a cold `NGXContext` parameter-bus bridge for the signed-core bootstrap gap. When Reflex is requested, the same private attachment is loaded only to translate Reflex/PCL to AMD Anti-Lag 2 on the native D3D12 device. SR execution remains stock Streamline evaluator -> stock `sl.dlss` translation -> APUSR NGX callbacks -> native APUSR D3D12 FSR4.

D3D discovery also has an ABI requirement, not just a value requirement. `NvAPI_GPU_GetLogicalGpuInfo` must preserve the real `NV_LOGICAL_GPU_DATA` layout used by the host revision: the caller supplies `pOSAdapterId`, APUSR writes the real DXGI adapter LUID through that pointer, and the fake NVAPI physical GPU is returned in the physical-handle array. This lets stock `sl.common` correlate its NVAPI GPU with the same DXGI adapter and then query `NvAPI_GPU_GetArchInfo`; APUSR reports a compatible architecture there. Do not embed an inline LUID into an invented version of that structure or patch `sl.dlss` JSON after `slOnPluginLoad` returns. By then `sl.common` has already decided whether any supported plugin needs NGX initialization.

## Attachment and bootstrap contract

`apusr_streamline_attach.dll` is a private renderer-specific module, not a host-facing Streamline facade. It attaches outside the loader lock. Vulkan must arm it before stock `slInit` can perform vendor-specific device/feature negotiation, so host capture is installed before the real `VkDevice` is created; the NGX Vulkan requirements path remains a redundant late-recovery bootstrap. D3D12 arms it only when Reflex was requested, and then only for Reflex/PCL -> AMD Anti-Lag 2 translation.

The normal attachment resolves functions from the canonical loaded `sl.interposer.dll` with `GetProcAddress` by name and Detours only functions that exist. The optional loader-only bootstrap passes an explicit `HMODULE` for the preserved stock sidecar instead. APUSR records that stock file's version for diagnosis and for APUSR-owned feature-version reporting when attachment begins after an in-flight `slInit`; it does **not** use that version to select product behavior or a package variant.

## Vulkan DLSS-SR contract

Stock `sl.dlss` is also retained on Vulkan because it already performs the host revision's correct Streamline-to-NGX translation and establishes/restores the external Vulkan image states around NGX evaluation. APUSR therefore does not duplicate that translator.

Two NVIDIA-specific bootstrap seams must be replaced while preserving those semantics. First, supported NVIDIA `sl.dlss` revisions append NVIDIA-only Vulkan device extensions during `slGetFeatureRequirements(kFeatureDLSS)` and through plugin-private device requirements. APUSR preserves the public tag contract, substitutes only APUSR's actual Vulkan requirements, bypasses stock Streamline's private NVIDIA-only `vkCreateDevice` preflight, and publishes the resulting real AMD device through `slSetVulkanInfo`. Second, stock `sl.common` still cannot create an NGX context backed by APUSR's unsigned replacement core. The D3D12 parameter-bus bridge therefore applies to Vulkan as well: immediately before stock `sl.dlss` startup, APUSR initializes its Vulkan NGX facade from Streamline's `VkDevices { instance, device, physical }` startup tuple, obtains APUSR Vulkan capability parameters, and publishes the ordinary `sl.param.global.ngxContext` callbacks. Stock `sl.dlss` then owns every per-frame tag/resource/layout transition and calls those callbacks from inside its normal evaluator.

The supported Vulkan integration contract is: bypass only Streamline's NVIDIA-specific device preflight, capture the real Vulkan device and queues, publish them through `slSetVulkanInfo`, publish the Vulkan NGX context, and leave stock `sl.dlss` in charge of evaluation semantics.

That Vulkan sanitization is gated by the host-selected renderer, not by `FeatureRequirements::eVulkanSupported`. The requirements flags are a support mask and stock DLSS may set the Vulkan bit during a D3D12 session. Treating that bit as renderer state has already regressed a D3D12 host by switching PCL/Reflex and DLSS attachment behavior to Vulkan mid-startup.

At NGX evaluation, APUSR queries its own private Vulkan host-capture interface directly. `nvngx_dlss.dll` queries `apusr_vulkan_host_capture.dll` directly for negotiated device flags and graphics/compute queue-family information. The private attach module contributes only one additional provenance bit: a thread-local marker proving that the current NGX call is nested inside stock `slEvaluateFeature(kFeatureDLSS)`. Only then is the state contract `NgxEvaluatePrepared`, because stock `sl.dlss` is the component that prepared and will restore the tagged external images. Direct/native NGX Vulkan image-state provenance remains explicitly unsupported rather than being invented.

## Optional Vulkan feature ownership

DLSS-SR and the optional cross-vendor Streamline features have different owners:

| Feature | Streamline semantic owner | APUSR role |
| --- | --- | --- |
| DLSS Super Resolution / DLAA | stock `sl.dlss` | NGX implementation + Vulkan requirement sanitization |
| Reflex | APUSR attach translation | `VK_AMD_anti_lag` on Vulkan; AMD Anti-Lag 2 on native D3D12 |
| PCL markers | APUSR marker sink + stock forwarding when available | frame correlation / timing coexistence |
| DLSS Frame Generation | APUSR attach translation | APUSR FI/OF + presentation owner |
| unrelated Streamline features | stock Streamline | passthrough |

This is also an initialization-ownership rule. On Vulkan, the preferences forwarded to stock
`slInit` retain DLSS-SR, PCL, and unrelated features, but exclude Reflex, DLSS-G, and unsupported
DLSS-RR. APUSR records the original requests before filtering and continues to expose the
host-facing Reflex/DLSS-G API through the attachment. Passing the full list to stock would load
NVIDIA's optional Vulkan implementations beside APUSR's WSI/presentation owner, creating two
incompatible device/present contracts. D3D12 preferences are forwarded unchanged.

Attachment may begin while stock `slInit` is performing its Vulkan NGX capability discovery. APUSR therefore has late arming for its own Vulkan Reflex/DLSS-G features when the host requests their feature functions, and derives the stock Streamline semantic version from the loaded PE for version reporting. This is recovery of missed cold-path state, not an invented capability result.

## Hot-path policy

On D3D12 and Vulkan, SR evaluation remains stock Streamline -> stock `sl.dlss` -> NGX -> APUSR. APUSR does not copy or reinterpret SR tags/options per frame at the Streamline layer. Vulkan is manual only at the device-negotiation boundary: stock Streamline's automatic `vkCreateDevice` proxy is bypassed because its loaded NVIDIA plugins inject NV-only device extensions that cannot exist on AMD; APUSR host capture creates the native device with the real APUSR requirements and immediately publishes the captured topology through stock `slSetVulkanInfo`. Public Vulkan feature requirements are still sanitized for host discovery, and APUSR-owned optional features remain translated by the attachment. D3D12's parameter-bus bridge is installed once at plugin startup; its per-frame callbacks are the NGX implementation boundary, not a second Streamline evaluator. DLL discovery, context injection, Detours installation, Vulkan feature negotiation, module loading, and private-ABI resolution are cold/bootstrap work only.

The shared NGX capability parameter object is scratch state, not a durable copy of the feature contract. In particular, stock `sl.dlss` uses it for optimal-settings queries and the callback writes recommended render dimensions into `OutWidth`/`OutHeight`. APUSR captures the true output extent when `createFeature` runs and uses that stored `feature.create.displaySize` during every evaluation; only render-subrect dimensions are allowed to vary frame-to-frame. Re-reading `OutWidth`/`OutHeight` during evaluation collapses non-native modes to their input size and is explicitly forbidden by the Streamline contract test.

The FFX replacement path remains separate and must not depend on Streamline/DLSS integration.


## Early renderer bootstrap

A Vulkan-capable host is not guaranteed to reach NGX before it decides whether the Streamline feature graph is usable. Waiting for `NVSDK_NGX_VULKAN_*` to load `apusr_streamline_attach.dll` can therefore be circular: stock Streamline may reject Vulkan before those entry points are called. APUSR must establish the renderer-aware attachment before the real stock `slInit` executes. The bootstrap reads only the explicit modern `Preferences::renderAPI` field; Vulkan arms host capture and attachment, while D3D12 preferences remain semantically unchanged.

There are two valid cold-start boundaries. If an APUSR-visible host DLL is loaded **before** the host resolves `slInit`, renderer-neutral `LoadLibrary*`/`GetProcAddress` interception can wrap that named lookup. This is opportunistic, not universal. If the first APUSR-visible DLL is loaded **from inside** stock `slInit`, no hook installed by that DLL can affect the `slInit` pointer already executing. This ordering can occur when a host has no static DXGI import: stock Streamline is loaded and `slInit` is entered before Streamline itself causes the APUSR DXGI proxy to load. A hook installed from that later DXGI load cannot affect the `slInit` call already on the stack.

For that ordering, the package stages an optional loader-only `sl.interposer.dll` bootstrap. Its installer preserves the game's exact interposer bytes as `sl_interposer_apusr_stock.dll`. The proxy's export surface is deliberately mechanical: every export except `slInit` is a PE forwarder to the stock sidecar with the same name and ordinal. The local `slInit` wrapper loads the private attach module, passes the explicit stock-module handle, applies the renderer hint, then resolves and calls the same stock `slInit` entry after the Detours transaction. This is a loader-boundary shim, **not** an APUSR Streamline evaluator or semantic replacement. It is inactive unless explicitly installed, so hosts whose normal loader ordering already works keep their existing path unchanged.

A host can also resolve and cache Vulkan loader/device trampolines before `slInit`. Installing Detours merely before `vkCreateDevice` is too late if the call site already holds a raw pointer returned by the original loader. Renderer-neutral loader interception may therefore arm `apusr_vulkan_host_capture.dll` when `vulkan-1.dll` is first loaded or queried whenever that boundary is visible. The optional Streamline loader shim supplies the earlier boundary when it is not. Neither mechanism selects a renderer merely from module visibility or takes ownership of stock Streamline semantics.

D3D12 Reflex uses the same attachment without taking ownership of DLSS-SR. `slSetD3DDevice` classifies the actual D3D interface, asks stock `slGetNativeInterface` for the native D3D12 device, and initializes AMD Anti-Lag 2 there. Reflex options map Off/On/Boost to Anti-Lag Off/On/On; `slReflexSleep` or PCL `SimulationStart` provides the input-side delay point, and PCL `RenderSubmitEnd` maps to Anti-Lag 2 end-of-render signaling.
