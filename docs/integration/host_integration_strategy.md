<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Host integration strategy

## In-process renderer restart

Do not assume every backend switch repeats `slInit`. A host may shut down one renderer attachment and rebuild stock `sl.common`/`sl.dlss` directly. When Reflex was explicitly requested, the authoritative D3D12 plugin-startup device rearms the private named-export attachment and restores the native AMD Anti-Lag 2 binding. This cold seam does not take ownership of DLSS-SR evaluation and does not weaken shutdown detachment.

APUSR implements four host-integration routes:

1. Native FidelityFX/FSR4 D3D12 replacement for games that already expose the AMD FFX provider ABI.
2. Direct NGX/DLSS D3D12 facade for games that already load `nvngx.dll` / `nvngx_dlss.dll`.
3. Broad D3D12 DLSS compatibility for games that hide DLSS before loading NGX.
4. Native Vulkan Streamline integration: the game's stock `sl.dlss` owns Streamline DLSS-SR semantics, APUSR owns the NGX implementation boundary, and the backend-level host-capture module owns Vulkan device/queue negotiation and the native FSR4 INT8 Vulkan recorder.

The Vulkan product path executes native accelerated packed DOT4 with no DX12 translation layer. APUSR integration policy is host-agnostic: compatibility decisions are based on observed API/loader contracts, not application identity.

For Streamline games that can switch renderer APIs, the multi-backend package deliberately unions the D3D12 visibility surfaces and Vulkan Streamline/capture surfaces so one install can survive a D3D12/Vulkan switch.

## Hot-path contract

The direct NGX per-frame path performs the following work:

```text
NVSDK_NGX_D3D12_EvaluateFeature
    -> lookup existing feature handle
    -> stack-build FrameInputDesc from NGX params
    -> project to backend frame desc
    -> recordFrame
```

No per-frame JSON parsing, backend probing, environment-variable requirements, present-hook rendering, or fake passthrough rendering is allowed.

## Packaging

The supported DLSS packages are now deliberately stock-Streamline-preserving:

```text
apusr_package_dlss_compatibility
  D3D12 visibility path: dxgi + NVAPI + NGX. No Streamline replacement.

apusr_package_streamline_vulkan_compatibility
  Vulkan path: NVAPI + NGX + private Streamline attach + Vulkan host capture.

apusr_package_dlss_multibackend_compatibility
  Renderer-switching union: DXGI + NVAPI + NGX + attach + Vulkan capture.
```

The canonical packages do **not** directly stage a file named `sl.interposer.dll`. The default installation therefore leaves the game's stock interposer at the loader boundary with its own plugin manager and revision-specific behavior. Vulkan-capable packages additionally stage an inactive `apusr_sl_interposer_bootstrap.dll` plus a fail-closed installer for hosts that enter `slInit` before any normal APUSR boundary can become visible. When explicitly activated, the installer preserves the exact game PE as `sl_interposer_apusr_stock.dll`; the loader alias forwards all unrelated exports directly to that PE and intercepts only `slInit` long enough to establish the private attachment. On Vulkan, `apusr_streamline_attach.dll` resolves only supported stock functions by **name** before installing Detours. Optional names such as `slSetTagForFrame` are attached only when present.

DLSS Super Resolution has the same semantic owner on both active backends: the game's stock Streamline runtime and stock `sl.dlss`. On **D3D12**, APUSR's early identity compatibility hooks relax the discovery/vendor gates needed to expose DLSS on AMD hardware. Stock `sl.common` unwraps Streamline's D3D12 command-list proxy before plugin evaluation and stock `sl.dlss` owns tags/options/resource transitions. The remaining cross-vendor gap is NGX-core bootstrap: NVIDIA's client code statically linked into `sl.common` verifies a candidate NGX core before loading it, so an unsigned APUSR replacement cannot be reached through that loader. APUSR therefore injects an APUSR-backed `common::NGXContext` through the Streamline parameter bus immediately before stock `sl.dlss` startup. Its create/evaluate callbacks receive the same native command list produced by stock `sl.common`; no per-frame Streamline translator is replaced.

On **Vulkan**, both bootstrap gaps apply. NVIDIA Streamline's automatic `vkCreateDevice` proxy cannot be the cross-vendor device-negotiation authority because it validates plugin-private NVIDIA requirements such as `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, and `VK_NV_low_latency` before forwarding to the real loader. APUSR therefore detours only that Vulkan device-creation export, lets `apusr_vulkan_host_capture.dll` negotiate the replacement implementation's real AMD/APUSR feature contract, and then initializes stock Streamline through its documented `slSetVulkanInfo` manual-hooking interface. After the real device is published, APUSR also supplies the same narrow `common::NGXContext` parameter-bus contract using Vulkan NGX init/capability/create/evaluate/release exports and Streamline's startup `VkDevices` tuple. Public `slGetFeatureRequirements` sanitization remains for host discovery; unsupported NV extensions are never fabricated. The supported Vulkan boundary is manual device publication plus APUSR-supplied NGX context, followed by stock Streamline evaluation into the native Vulkan backend. Device creation/publication and context injection are treated as compatibility contracts.

When DXGI is visible before Streamline initialization, the D3D12 `dxgi.dll` compatibility surface installs identity hooks early enough to observe Streamline discovery; `nvapi64.dll` supplies the NVIDIA capability answers that those host/plugin probes expect. That DXGI bootstrap is opportunistic: it cannot intercept a cached `slInit` pointer if DXGI itself is first loaded from inside the already-running stock `slInit`. The optional loader-only alias exists for that earlier ordering. These D3D12 surfaces must not replace `slEvaluateFeature`, tag/constants functions, feature-function lookup, or DLSS resource ownership. The one private D3D12 Streamline seam is cold-path `sl.param.global.ngxContext` injection after common startup has exhausted NVIDIA's signed-core loader. The startup wrapper derives the renderer from the same `deviceType` field in Streamline's plugin-manager JSON that stock `plugin::onStartup` consumes, so the decision is local to the DLL instance that owns the hook and cannot inherit stale state from the separate Vulkan attachment module. The context exposes APUSR capability parameters and NGX create/evaluate/release callbacks; stock `sl.dlss` still drives those callbacks from its normal evaluator. D3D12 does not use `apusr_streamline_attach.dll` for DLSS-SR. When the host explicitly requests Reflex, the early `slInit` wrapper may load the private attachment so Reflex/PCL can translate to AMD Anti-Lag 2 on the native D3D12 device; SR semantics remain stock. Vulkan boots the same attachment before the stock `slInit` body executes: either through the normal named-lookup hook when an APUSR module is already visible, or through the optional loader-only alias when it is not. This guarantees capture/attachment are armed before stock initialization can reject the Vulkan feature graph. The NGX Vulkan requirements path remains a redundant late-recovery bootstrap. Bootstrap work never executes from the attachment's `DllMain`. The Streamline parameter bus is not assumed to survive for the process lifetime: a host may rebuild `sl.common`/`sl.dlss` when settings change. APUSR therefore treats each bus delivered by `slOnPluginLoad` as a new epoch and republishes/verifies `sl.param.global.ngxContext` on every D3D12 plugin startup.


The D3D12 NGX callback is entered after stock `sl.dlss` has prepared tagged resource states. APUSR carries that state provenance into its backend (`PIXEL_SHADER_RESOURCE` for Streamline `eTextureRead` inputs and `UNORDERED_ACCESS` for the `eStorageRW` output) rather than assuming `COMMON`. Native game-resource descriptors likewise outrank frontend format defaults when APUSR constructs borrowed views. If an APUSR evaluation removes the native D3D12 device, context injection is circuit-broken until process restart so Streamline's host recovery cannot recursively recreate the device and immediately re-enter the same failing path. The circuit is a containment rule, not a fallback implementation.

If DLSS is hidden or Vulkan ingress fails, explicitly enable `APUSR_HOST_DISCOVERY_LOG=1`; `APUSR_LOG_DIR` selects a location but does not enable diagnostics. A successful stock-preserving attach records `stock_pe_preserved=1`.

## FFX route remains protected

The FFX replacement package remains independent:

```text
apusr_package_ffx_dx12_replacement
```

The FFX replacement owns the upscaling effect and stages a companion FidelityFX Frame Generation provider by default. Non-upscale FidelityFX contexts, including Frame Generation and Frame Generation Swapchain contexts, are delegated to that packaged provider when present, with sidecar/original-provider names retained only as fallback probes for unusual title layouts. This keeps the working FFX route independent from NGX, Streamline, NVAPI, and DLSS compatibility helpers while still allowing native FSR Frame Generation coexistence.

DLSS Frame Generation is a separate feature path from Super Resolution and must target APUSR's frame-generation compatibility seam rather than pretending the upscaler generated frames. Streamline Reflex/PCL likewise target APUSR's existing low-latency/marker seam, using the native platform mechanism where its behavior matches instead of growing a separate vendor-shaped latency backend. These optional-feature translations do not alter the backend-specific DLSS-SR ownership described above.


## Queue-capture seams

The DXGI compatibility DLL remains the cold-path queue-capture seam for D3D12: it records queue candidates when they are observed and the NGX evaluator caches the resolved queue. Vulkan now has its own backend-level host-capture seam below Streamline. `apusr_vulkan_host_capture.dll` intercepts the process's loaded Vulkan loader at instance/device/proc-address creation time, records every queue created with the logical device, and exposes that state to frontends. `slSetVulkanInfo` is supplemental metadata identifying Streamline's preferred graphics/compute queues rather than the source of queue discovery. D3D11 and D3D11On12 should likewise consume the strongest authoritative host boundary available rather than adding per-frame backend probing or speculative tracking.

The Vulkan capture core is deliberately deployment-independent. Windows currently bootstraps it with process-lifetime Detours on the host-selected Vulkan loader; the feature negotiation, instance/device ownership records, and queue model do not depend on Streamline. Loader interception must not self-intercept APUSR support modules or recursively reinstall capture while a Detours transaction is active; early capture is a one-way host boundary, not a recursively hooked internal service. The long-term loader-managed layer design and its deployment constraints are specified in `vulkan_layer_strategy.md`. A Vulkan layer is the preferred standards-defined capture surface, but it is not by itself a drop-in bootstrap: an unmodified game must still discover/enable the layer before instance creation. The final design therefore keeps one capture core with both a loader-managed layer path and a Windows drop-in bootstrap until installed/launcher deployment can guarantee layer activation. Game-name checks, global AMD-to-NVIDIA identity spoofing, and per-submit interception do not belong in this backend seam.

## Combined DLSS visibility and AMD Frame Generation

The useful combined route is now:

```text
DLSS compatibility package
    -> exposes DLSS Super Resolution / DLAA-shaped upscaling through APUSR
    -> maps DLSS sharpness into the same guarded RCAS path used by FFX sharpening
    -> no-ops unsupported DLSS/Streamline execution selections

FFX provider delegation
    -> preserves native AMD/FSR Frame Generation through the packaged provider
```

This lets a title expose DLSS upscaling through APUSR while continuing to use AMD Frame Generation instead of NVIDIA DLSS-G. The source-built companion provider removes the normal sidecar/original-provider requirement. A clean-room Frame Generation rewrite or DLSS-G translation is separate future work, not part of the current integration path.

### Renderer-switch and D3D device-lifetime rules

A process loading **or using** `dxgi.dll` does not prove that its selected renderer is D3D. Vulkan hosts may enumerate DXGI/KMT/display identity for adapter selection before Streamline establishes the render API. The compatibility proxy therefore keeps game-facing adapter identity real while the renderer is unknown. Broad Win32/KMT/display identity spoofing activates only after explicit D3D12 `Preferences::renderAPI`; on Vulkan it remains dormant. Stock Streamline's Vulkan capability discovery is a separate audience: only DXGI `GetDesc*` calls originating in stock Streamline modules receive the narrow NVIDIA-vendor/real-device-ID view needed for its generic vendor gate and physical-device correlation. Do not reintroduce process-global pre-renderer identity spoofing.

For D3D12 Streamline integration, neither the pointer supplied to `slSetD3DDevice` nor `ID3D12GraphicsCommandList::GetDevice()` on an arbitrary host-visible command list may be used to invent a new APUSR ownership boundary. Streamline can expose private D3D12 device/command-list proxies; its common evaluator deliberately unwraps those proxies to the native command list before invoking `sl.dlss`, and stock `slSetD3DDevice` similarly unwraps its device proxy before plugin initialization. APUSR therefore leaves the Streamline evaluate/tag/options/resource path intact and begins at the NGX boundary, where the device and command list are already native and coherent.

The D3D visibility shim must also preserve NVAPI's logical-GPU ABI exactly where stock `sl.common` consumes it. `NvAPI_GPU_GetLogicalGpuInfo` receives caller-owned storage through `pOSAdapterId`; APUSR writes the real DXGI LUID there and returns the fake physical handle through the official handle-array fields. Streamline then matches that LUID to its enumerated adapter before querying the spoofed architecture. This is a cold discovery contract. Rewriting plugin JSON or forcing `slIsSupported` after the fact is not an acceptable substitute, because it bypasses the common plugin manager's earlier NGX-initialization decision.

The NGX D3D12 frontend still verifies that its feature device is live and that the command list belongs to the same COM device identity before recording work. An already-removed device or mismatched command-list device is rejected explicitly instead of returning success and recording into invalid execution state.

Streamline motion-vector scale is signed. Normalized values such as `-1/renderWidth` and `-1/renderHeight` must retain their sign when converted to NGX pixel-space scale; stock `sl.dlss` now owns this translation for Streamline callers.


### Streamline renderer-declaration bootstrap

Do not make Vulkan attachment depend solely on NGX Vulkan discovery. Some hosts decide that the Vulkan Streamline feature graph is unavailable before loading any NGX Vulkan replacement. APUSR therefore supports two pre-stock-`slInit` observation boundaries: the combined DXGI compatibility layer can wrap a dynamically resolved stock `slInit` when DXGI is already early enough, while the optional loader-only `sl.interposer.dll` bootstrap handles hosts that call `slInit` before DXGI exists. Both routes publish the same host-provided `Preferences::renderAPI` into the private attachment before stock initialization. Vulkan arms process-lifetime host capture and named-export attachment at that point. D3D12 uses the same attachment only when Reflex is requested, for the Anti-Lag 2 translation; stock Streamline still owns DLSS-SR semantics. NVAPI must remain free of attachment loading. Renderer-specific capture/attachment, module enumeration/loading, and Detours work must not run from `DllMain`; the DXGI proxy may patch only already-resolved renderer-neutral loader imports there.

Renderer selection must never be inferred from `FeatureRequirements::flags`. Those bits describe APIs a feature supports, not the renderer currently active in the host. The explicit `Preferences::renderAPI` observed at `slInit` is the renderer authority for attachment behavior. Because the identity compatibility implementation is compiled into more than one APUSR DLL, a renderer value stored in one module is not automatically visible in another. The private Streamline attachment therefore exports the canonical current render API, and a DXGI compatibility module that loads after bootstrap queries that value before making any Streamline-scoped identity decision. Do not duplicate renderer state across DLL boundaries without an explicit synchronization contract.

Streamline's Vulkan system-capability path correlates its DXGI adapter record with `VkPhysicalDeviceProperties::deviceID`. Consequently the **Streamline-scoped** Vulkan identity view preserves the real DXGI `DeviceId` while presenting an NVIDIA vendor identity for Streamline's generic capability gate. Host/game callers continue to receive the real AMD vendor and device identity. An RTX-shaped fake device ID remains valid for the explicit D3D12 spoof but must never leak into pre-renderer or Vulkan host identity.
