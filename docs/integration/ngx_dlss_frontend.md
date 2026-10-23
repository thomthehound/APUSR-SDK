<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# NGX/DLSS frontend

The NGX/DLSS frontend is the primary broad game-compatibility route for APUSR.

The DLL exports the DLSS-shaped NGX entry points used by DLSS-main and by existing game integrations:

- `NVSDK_NGX_D3D12_Init*`
- `NVSDK_NGX_D3D12_GetParameters`
- `NVSDK_NGX_D3D12_GetCapabilityParameters`
- `NVSDK_NGX_D3D12_CreateFeature`
- `NVSDK_NGX_D3D12_EvaluateFeature`
- `NVSDK_NGX_D3D12_ReleaseFeature`
- `NVSDK_NGX_D3D12_Shutdown*`

The facade does not forward into NVIDIA DLSS.  It translates the caller's NGX parameter bag into APUSR's internal frame contract and records APUSR-owned backend work.

```
NVSDK_NGX_D3D12_EvaluateFeature
    -> NgxDlssEvaluateDesc
    -> FrameInputDesc
    -> FrameDesc
    -> Dx12Fsr4ReferenceRuntime::recordFrame
```

The Vulkan route shares the same ingress contract and reaches APUSR's native recorder:

```
NVSDK_NGX_VULKAN_EvaluateFeature
    -> NVSDK_NGX_Resource_VK decode
    -> NgxDlssVulkanEvaluateDesc
    -> FrameInputDesc
    -> FrameDesc
    -> VulkanFsr4ReferenceRuntime::recordFrame
```

The Vulkan recorder has been validated end-to-end in a game through the Streamline route. It consumes the Streamline pre-NGX image-state/resource contract, backend-level host-captured device/queue state, native device features negotiated before `VkDevice` creation, and the backend-owned memory-mapped product shader pack. `slSetVulkanInfo` is supplemental preferred-queue/association metadata rather than the authoritative queue-discovery source.


## Feature surface

The current NGX/DLSS frontend implements DLSS Super Resolution / DLAA-shaped evaluation only. It should not advertise Ray Reconstruction, Reflex, Frame Generation, Multi Frame Generation, or NVIDIA model-selection semantics as supported features.

DLSS Frame Generation translation is a future layer on top of a working FidelityFX Frame Generation route, not a feature to claim early. The planned shape is:

```text
DLSS-G / Streamline FG request
    -> APUSR frame-generation compatibility descriptor
    -> packaged FidelityFX Frame Generation provider or future APUSR FG backend
```

Until that bridge exists, DLSS-G requests must fail closed or remain hidden by capability reporting. The FFX drop-in route can coexist with a game's native FSR Frame Generation by delegating non-upscale FidelityFX contexts to the packaged frame-generation provider.

Reflex/PCL has the same roadmap status rather than being permanently discarded. The intended APUSR translation is Streamline's latency/marker contract -> APUSR low-latency contract -> AMD Anti-Lag 2 where that backend contract is available. Reflex-specific features that do not map cleanly to Anti-Lag 2 must remain unsupported until APUSR has an explicit equivalent; the translation must not fake Reflex semantics merely to satisfy discovery.

The active Windows package provides both common drop-in names without duplicating the implementation payload:

```
nvngx_dlss.dll                    # real frontend
nvngx.dll                         # PE export-forwarder to nvngx_dlss.dll
```

Next work should preserve the proven Vulkan semantics while broadening compatibility and measuring performance:

- Treat the working native Vulkan recorder/Streamline/NGX contract as the reference rather than reopening integration architecture without evidence.
- Add non-zero Vulkan resource-subrect origins only when the recorder can apply them to every sampled/storage access correctly.
- Add D3D11 only after a real D3D11 or D3D11On12 execution strategy is selected.
- Reconsider a plugin-layer replacement only as a fallback when the NGX facade cannot be reached.

Per-frame evaluation should remain the simple ABI-to-frame-contract path shown above. Future API support should register host queues/contexts outside the frame evaluator and cache resolved execution state in the feature context.

For NGX Super Resolution, the target display/output extent is part of feature creation. Per-frame evaluation may update the render subrect for dynamic resolution, but it must not reinterpret mutable `OutWidth`/`OutHeight` parameter values as a new display size. Streamline and other NGX clients reuse the capability parameter object for optimal-settings queries, where those same keys are intentionally overwritten with the recommended *render* resolution. The D3D12 and Vulkan paths therefore both keep `feature.create.displaySize` authoritative until the feature is recreated.

## User-facing feature policy

The NGX/DLSS facade implements DLSS Super Resolution / DLAA-shaped evaluation only. DLSS sharpness is accepted as compatibility input and mapped into the same guarded RCAS path used by the FFX frontend. The menu/packaging layer should avoid exposing duplicate sharpening controls, but execution honors a real DLSS sharpness value when a host supplies one.

Feature discovery still fails closed for non-SR features where the host listens. In practice, some games expose menu entries from static integration logic rather than capability answers. When that happens, APUSR fails unsupported Streamline execution calls closed rather than forwarding them into stock NVIDIA plugins or pretending unsupported work succeeded. This is compatibility containment, not support for those features. Ray Reconstruction, Reflex, DLSS Frame Generation, and Multi Frame Generation remain unimplemented in the DLSS facade.

Native AMD Frame Generation is different: APUSR now packages a FidelityFX Frame Generation provider and can delegate non-upscale FFX contexts while still servicing DLSS-shaped upscaling. That combined route is valid and is the intended short-term compatibility behavior. A future source-built APUSR frame-generation backend should replace the carried provider artifact.
