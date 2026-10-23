SPDX-License-Identifier: Apache-2.0
Copyright (c) 2026 thomthehound.

APUSR FFX/FSR4 D3D12 replacement package

Purpose:
  Correctness and performance fixture for games that already load the AMD FidelityFX provider ABI.

Files:
  amd_fidelityfx_dx12.dll
    APUSR-owned FSR4 INT8 DX12 runtime packaged under the FidelityFX provider name. This DLL owns the upscaling effect.

  amd_fidelityfx_framegeneration_dx12.dll
    Source-built FidelityFX-compatible Frame Generation provider. APUSR controls its packaging and provider selection; interpolation, optical flow, swapchain, and pacing remain modified MIT-licensed FidelityFX-derived source.

Optional files:
  amd_fidelityfx_upscaler_dx12.dll
    Only staged by the explicit apusr_package_ffx_dx12_compat_aliases target. Use only for titles that probe the historical child-provider alias.

  WinPixEventRuntime.dll
    Only present in marker/debug builds that explicitly enable GPU debug markers.

Notes:
  APUSR implements the FidelityFX upscaling effect. Frame Generation is packaged as a source-built companion provider; no prebuilt provider fallback is supported.

  The upscaler hot path does not route through the Frame Generation provider. Non-upscale FidelityFX contexts are delegated once the host creates them, then dispatch through direct function pointers for those contexts.

  This package is independent from NGX, NVAPI, Streamline, DXGI identity compatibility, and future queue-capture work. When intentionally combining DLSS compatibility with AMD Frame Generation, install the DLSS compatibility package as well. When testing the FFX drop-in path in isolation, remove only APUSR-added DLSS compatibility files such as local dxgi.dll, nvapi64.dll, nvngx.dll, nvngx_dlss.dll, apusr_streamline_attach.dll, and apusr_vulkan_host_capture.dll. Leave the game's stock sl.interposer.dll and other stock Streamline DLLs untouched.

Frame Generation status:
  The current provider is source-built, uses APUSR-owned provider selection, and includes the completed low-risk CPU/GPU cleanup pass. Further algorithmic or pacing work requires profiling and is not part of the current completion scope.
