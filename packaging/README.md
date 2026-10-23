<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR packaging

Package targets are the supported game-drop-in boundary. They are separated by the bootstrap/frontends they actually contain rather than by assets they cannot use.

- `apusr_package_ffx_dx12_replacement` — native FFX/FSR4 D3D12 package, including the source-built FidelityFX-compatible Frame Generation provider.
- `apusr_package_ngx_dlss` — direct NGX/DLSS **D3D12** facade for hosts that already load NGX.
- `apusr_package_dlss_compatibility` — broad **D3D12** DLSS visibility package (`dxgi` + NVAPI + NGX); preserves the game's stock Streamline files and their DLSS-SR semantic path while APUSR supplies cross-vendor discovery identity and the NGX implementation.
- `apusr_package_streamline_vulkan_compatibility` — native **Vulkan** Streamline package; preserves the game's stock `sl.interposer.dll` and attaches APUSR in-process by named exports.
- `apusr_package_dlss_multibackend_compatibility` — D3D12 + Vulkan union using that same stock-preserving attachment architecture.

The canonical Vulkan-capable packages never stage or rename `sl.interposer.dll`; they preserve the game's stock loader boundary and add APUSR's private Streamline attachment beside it.

The D3D12-only packages do not generate or carry `apusr_fsr4_int8_vulkan.shaderpack`. This avoids a large unnecessary Vulkan shader build and prevents a package from advertising a backend asset it cannot bootstrap by itself.

The Vulkan-capable packages carry `apusr_streamline_attach.dll`, `apusr_vulkan_host_capture.dll`, the APUSR NGX facade/alias, NVAPI compatibility support, and one `apusr_fsr4_int8_vulkan.shaderpack`. On Vulkan, stock `sl.dlss` remains the SR translator. APUSR bypasses only stock Streamline's NVIDIA-only automatic device preflight, publishes the real AMD device through `slSetVulkanInfo`, and supplies an APUSR-backed `sl.param.global.ngxContext` so stock `sl.dlss` reaches the replacement Vulkan NGX implementation without NVIDIA's signed core. The attach layer otherwise owns only cross-vendor Vulkan requirements/Reflex/DLSS-G compatibility work. On D3D12, stock `sl.interposer`/`sl.common`/`sl.dlss` also remain the semantic owner. APUSR's DXGI/NVAPI identity layer makes feature discovery coherent, and the same cold parameter-bus NGXContext pattern bypasses only NVIDIA's signed-core bootstrap when necessary; stock tags/options/transitions/evaluation remain untouched. The private attachment is not used for D3D12 DLSS-SR. In the multi-backend package it is loaded on D3D12 only when Reflex is requested, for Reflex/PCL -> AMD Anti-Lag 2 translation.

Package README files are copied into each staged directory so the install explains which files are intentional and which diagnostics are opt-in.
