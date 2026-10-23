# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Contract for the canonical stock-preserving Streamline integration."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
ATTACH = (ROOT / "src/frontends/streamline_attach/apusr_streamline_attach.cpp").read_text(encoding="utf-8")
FEATURE_OWNERSHIP = (ROOT / "src/frontends/streamline_attach/apusr_streamline_feature_ownership.h").read_text(encoding="utf-8")
NGX = (ROOT / "src/frontends/ngx_dlss_dx12/apusr_ngx_dlss_dx12_frontend.cpp").read_text(encoding="utf-8")
NVAPI = (ROOT / "src/frontends/nvapi_host_probe/apusr_nvapi_host_proxy.cpp").read_text(encoding="utf-8")
DXGI = (ROOT / "src/frontends/dxgi_spoof_proxy/apusr_dxgi_spoof_proxy.cpp").read_text(encoding="utf-8")
IDENTITY = (ROOT / "src/frontends/identity_spoof/apusr_identity_spoof_hooks.cpp").read_text(encoding="utf-8")
DX12_BACKEND = (ROOT / "src/backends/gfx_dx12/apusr_fsr4_reference_dx12.cpp").read_text(encoding="utf-8")
VK_HOST_CAPTURE = (ROOT / "src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_win32.cpp").read_text(encoding="utf-8")


def target_body(name: str) -> str:
    marker = f"add_custom_target({name}"
    start = CMAKE.find(marker)
    assert start >= 0, f"missing canonical package target {name}"
    next_target = CMAKE.find("add_custom_target(", start + len(marker))
    if next_target < 0:
        next_target = len(CMAKE)
    return CMAKE[start:next_target]


def _rva_to_offset(rva: int, sections: list[tuple[int, int, int, int]]) -> int:
    for virtual_address, virtual_size, raw_offset, raw_size in sections:
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            delta = rva - virtual_address
            assert delta < raw_size, f"RVA 0x{rva:x} points outside section raw data"
            return raw_offset + delta
    raise AssertionError(f"RVA 0x{rva:x} is not backed by a PE section")


def load_pe_exports(dll_path: Path) -> dict[str, int]:
    data = dll_path.read_bytes()
    assert len(data) >= 0x40 and data[:2] == b"MZ", f"not a PE image: {dll_path}"
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off : pe_off + 4] == b"PE\0\0", f"missing PE signature: {dll_path}"
    coff_off = pe_off + 4
    _, section_count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, coff_off)
    optional_off = coff_off + 20
    magic = struct.unpack_from("<H", data, optional_off)[0]
    assert magic in (0x10B, 0x20B), f"unsupported PE optional-header magic 0x{magic:x}"
    data_dir_off = optional_off + (96 if magic == 0x10B else 112)
    export_rva, export_size = struct.unpack_from("<II", data, data_dir_off)
    assert export_rva and export_size, f"PE image has no export directory: {dll_path}"
    section_off = optional_off + optional_size
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        off = section_off + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, off + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))
    export_off = _rva_to_offset(export_rva, sections)
    fields = struct.unpack_from("<IIHHIIIIIII", data, export_off)
    ordinal_base, number_of_functions, number_of_names = fields[5], fields[6], fields[7]
    address_of_names, address_of_name_ordinals = fields[9], fields[10]
    assert number_of_functions >= number_of_names > 0
    names_off = _rva_to_offset(address_of_names, sections)
    ordinals_off = _rva_to_offset(address_of_name_ordinals, sections)
    exports: dict[str, int] = {}
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", data, names_off + 4 * index)[0]
        name_off = _rva_to_offset(name_rva, sections)
        end = data.index(b"\0", name_off)
        name = data[name_off:end].decode("ascii")
        ordinal_index = struct.unpack_from("<H", data, ordinals_off + 2 * index)[0]
        exports[name] = ordinal_base + ordinal_index
    return exports


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--stock-dll", type=Path)
    args = parser.parse_args()
    assert "add_library(apusr_streamline_attach SHARED" in CMAKE
    assert "APUSR_STREAMLINE_ATTACH_MODE=1" in CMAKE
    assert "$<TARGET_FILE:apusr_streamline_attach>" in CMAKE
    attach_def = (ROOT / "src/frontends/streamline_attach/apusr_streamline_attach.def").read_text(encoding="utf-8")
    expected_private_exports = {
        "apusrAttachToLoadedStreamline",
        "apusrAttachToStreamlineModule",
        "apusrStreamlineAttachHintRenderApi",
        "apusrStreamlineAttachGetRenderApi",
        "apusrStreamlineAttachBindD3D12NativeDevice",
        "apusrStreamlineVulkanDlssEvaluationActive",
        "apusrStreamlineAttachInstalled",
    }
    declared_private_exports = {
        line.strip().split()[0]
        for line in attach_def.splitlines()
        if line.strip() and not line.lstrip().startswith(";")
        and line.strip().upper() != "EXPORTS"
        and not line.strip().upper().startswith("LIBRARY")
    }
    assert declared_private_exports == expected_private_exports

    for name in (
        "apusr_package_streamline_vulkan_compatibility",
        "apusr_package_dlss_multibackend_compatibility",
    ):
        body = target_body(name)
        assert "apusr_streamline_attach" in body, f"{name} must stage the private attach module"
        assert "apusr_streamline_bootstrap_proxy" in body, f"{name} must stage the inactive loader-only bootstrap"
        assert "install_apusr_streamline_bootstrap.bat" in body, f"{name} must stage the explicit bootstrap installer"
        assert "apusr_vulkan_host_capture" in body, f"{name} must stage Vulkan host capture"
        assert "apusr_ngx_dlss_dx12" in body, f"{name} must stage the NGX implementation"
        assert "apusr_nvapi_host_proxy" in body, f"{name} must stage the NVAPI compatibility surface"
        assert "/sl.interposer.dll" not in body and "\\sl.interposer.dll" not in body, f"{name} must not directly stage the canonical Streamline loader name"

    # Attachment must be revision-adaptive at the API level, not at the PE ordinal level.
    assert 'GetModuleHandleW(L"sl.interposer.dll")' in ATTACH
    assert re.search(r'GetProcAddress\(module,\s*entry\.name\)', ATTACH)
    assert 'GetProcAddress(module, "slSetTagForFrame")' in ATTACH
    assert "DetourAttach(&entry.trampoline, detour)" in ATTACH
    assert "DetourDetach(&entry.trampoline, detour)" in ATTACH
    assert "GetModuleFileNameW(" in ATTACH
    assert "ownedModuleRef = LoadLibraryW(modulePath.data())" in ATTACH
    assert "g_streamlineAttachRequestedModule" in ATTACH
    assert "apusrAttachToStreamlineModule" in ATTACH
    assert "FreeLibrary(ownedModuleRef)" in ATTACH
    assert "attach.detached" in ATTACH
    assert "stock_pe_preserved=1" in ATTACH
    assert "isLegacyApusrStreamlineProxy" in ATTACH
    assert '"apusrSlResolveVulkanQueue"' in ATTACH
    assert '"apusrSlResolveVulkanStreamlineContext"' in ATTACH
    assert '"apusrSlVulkanDlssEvaluationActive"' in ATTACH
    assert "reason=legacy_apusr_sl_interposer_detected" in ATTACH

    # DLSS-SR must remain on stock Streamline semantics and cross into APUSR at NGX.
    assert "Preserve stock sl.dlss evaluation" in ATTACH
    assert "attach.dlss.requirements_sanitized" in ATTACH
    assert "if (feature == kFeatureDLSS)" in ATTACH

    # The stock-preserving Vulkan path still has split feature ownership. Stock
    # sl.dlss owns SR, while APUSR owns cross-vendor Reflex and DLSS-G. Do not
    # pass those NVIDIA plugins (or unsupported RR) into stock slInit, where
    # they can install a second Vulkan presentation/device contract. PCL remains
    # available as the optional backing marker/statistics path, and D3D slInit
    # continues to receive the original preferences unchanged.
    ownership_start = FEATURE_OWNERSHIP.find("constexpr bool excludeFromStockVulkanInitialization")
    ownership_end = FEATURE_OWNERSHIP.find("static_assert", ownership_start)
    assert ownership_start >= 0 and ownership_end > ownership_start
    ownership_body = FEATURE_OWNERSHIP[ownership_start:ownership_end]
    assert "feature == kReflex" in ownership_body
    assert "feature == kDlssG" in ownership_body
    assert "feature == kDlssRr" in ownership_body
    assert re.search(r"feature == kDlss\b", ownership_body) is None
    assert re.search(r"feature == kPcl\b", ownership_body) is None

    attach_slinit_start = ATTACH.find("APUSR_SL_EXPORT int __cdecl slInit")
    attach_slinit_end = ATTACH.find("APUSR_SL_EXPORT int __cdecl slShutdown", attach_slinit_start)
    assert attach_slinit_start >= 0 and attach_slinit_end > attach_slinit_start
    attach_slinit_body = ATTACH[attach_slinit_start:attach_slinit_end]
    attach_mode_start = attach_slinit_body.find("#if defined(APUSR_STREAMLINE_ATTACH_MODE)")
    attach_mode_end = attach_slinit_body.find("#else", attach_mode_start)
    assert attach_mode_start >= 0 and attach_mode_end > attach_mode_start
    attach_mode_body = attach_slinit_body[attach_mode_start:attach_mode_end]
    assert "renderApi == kSlRenderApiVulkan" in attach_mode_body
    assert "feature_ownership::excludeFromStockVulkanInitialization(feature)" in attach_mode_body
    assert "filteredPreferences = prefs;" in attach_mode_body
    assert "fn(stockPreferences, sdkVersion)" in attach_mode_body
    assert '"vulkan.feature_ownership"' in attach_mode_body
    assert "stock_dlss_sr_preserved=1" in attach_mode_body

    # The loader bootstrap can run before dxgi.dll exists. Because the identity
    # compatibility source is compiled separately into dxgi.dll and the private
    # attach DLL, renderer state must cross that DLL boundary explicitly. The
    # attach module owns the authoritative render API; late-loaded DXGI queries
    # it without changing module load order. This is required for the narrow
    # Streamline-only Vulkan NVIDIA vendor view that keeps stock sl.dlss visible.
    assert "apusrStreamlineAttachGetRenderApi" in ATTACH
    assert 'GetModuleHandleW(L"apusr_streamline_attach.dll")' in IDENTITY
    assert 'GetProcAddress(attach, "apusrStreamlineAttachGetRenderApi")' in IDENTITY
    assert '"renderer_state.bridged"' in IDENTITY
    assert "currentStreamlineRenderApi() == kSlRenderApiVulkan" in IDENTITY

    # Stock Streamline's Vulkan vkCreateDevice proxy consumes plugin-private
    # external requirements, not the public slGetFeatureRequirements result.
    # NVIDIA's DLSS/Reflex plugins therefore inject NV-only extensions and can
    # return VK_ERROR_EXTENSION_NOT_PRESENT before the real loader is called on
    # AMD. Cross-vendor Vulkan must bypass only that device-creation proxy, let
    # APUSR host capture negotiate the replacement implementation's native
    # contract, then initialize stock Streamline through slSetVulkanInfo.
    assert '{"vkCreateDevice", nullptr}' in ATTACH
    assert 'APUSR_SL_DETOUR("vkCreateDevice", apusrVkCreateDevice)' in ATTACH
    assert 'vulkanSystemFn<VkCreateDeviceFn>("vkCreateDevice")' in ATTACH
    assert 'originalFn<VkCreateDeviceFn>("vkCreateDevice")' in ATTACH
    assert '"streamline.device_proxy_bypass"' in ATTACH
    assert 'apusrVulkanHostGetDeviceInfo' in ATTACH
    assert 'apusrVulkanHostResolveQueue' in ATTACH
    assert 'slSetVulkanInfo(&info)' in ATTACH
    # Keep the Vulkan result constants unique across the nested anonymous
    # namespaces. Duplicate definitions compile on some non-Windows contract
    # paths but are ambiguous under MSVC when the device bridge is enabled.
    assert ATTACH.count('constexpr std::int32_t kVkSuccess = 0;') == 1
    assert ATTACH.count('constexpr std::int32_t kVkErrorInitializationFailed = -3;') == 1
    assert '"streamline.device_published"' in ATTACH
    assert 'VK_NVX_binary_import' in ATTACH
    assert 'VK_NV_low_latency' in ATTACH

    # Virtualized Vulkan Present returns asynchronously, so a small physical
    # source lead bound is required to preserve ordinary WSI back-pressure and
    # prevent native Anti-Lag INPUT from absorbing several frames of delayed
    # PRESENT pairing in one burst. Keep the hard ownership capacity separate.
    assert ".maxPendingPresentCount = maxPendingPresentCount" in ATTACH
    assert ".maxApplicationPresentQueueDepth = std::min<std::uint32_t>(2u, maxPendingPresentCount)" in ATTACH
    assert '"presentation.wait_application_lead"' in (ROOT / "src/framegen/ffx_vk/apusr_framegen_presentation_vk.cpp").read_text(encoding="utf-8")

    # The NGX Vulkan seam queries host capture directly for device/queue state,
    # but it must still prove that stock sl.dlss owns the active evaluation so
    # direct/native NGX callers cannot invent external-image state provenance.
    assert "resolveVulkanHostIngress" in NGX
    assert "apusrVulkanHostDeviceNegotiationFlags" in NGX
    assert "apusrVulkanHostResolveQueue" in NGX
    assert "apusrStreamlineVulkanDlssEvaluationActive" in NGX
    assert "direct/native NGX Vulkan image-state provenance is not implemented" in NGX

    # Stock Streamline's Vulkan CHI owns the external-image transitions around
    # DLSS but emits its generic batches as ALL_COMMANDS -> ALL_COMMANDS. The
    # host-capture layer may narrow only the endpoint APUSR actually knows:
    # ingress destination and egress source are compute. The host endpoint stays
    # conservative because Streamline's resource tag carries layout/state but no
    # producer/consumer pipeline-stage provenance. Never apply this rewrite to
    # arbitrary host barriers or to mixed memory/buffer batches.
    assert 'GetModuleHandleW(L"apusr_streamline_attach.dll")' in VK_HOST_CAPTURE
    assert 'GetProcAddress(attach, "apusrStreamlineVulkanDlssEvaluationActive")' in VK_HOST_CAPTURE
    assert "srcStageMask == vk::PipelineStageAllCommandsBit" in VK_HOST_CAPTURE
    assert "dstStageMask == vk::PipelineStageAllCommandsBit" in VK_HOST_CAPTURE
    assert "memoryBarrierCount == 0u && bufferMemoryBarrierCount == 0u" in VK_HOST_CAPTURE
    assert "classifyStreamlineDlssBarrierBatch" in VK_HOST_CAPTURE
    assert "effectiveDstStageMask = vk::PipelineStageComputeShaderBit" in VK_HOST_CAPTURE
    assert "effectiveSrcStageMask = vk::PipelineStageComputeShaderBit" in VK_HOST_CAPTURE
    assert '"dlss.streamline_barrier_narrowed"' in VK_HOST_CAPTURE
    assert "effectiveSrcStageMask, effectiveDstStageMask" in VK_HOST_CAPTURE
    assert "Stock sl.dlss establishes these evaluate layouts" in NGX
    assert "it does not take ownership of the transitions" in NGX
    assert "NgxEvaluatePrepared" in NGX
    resolve_start = NGX.find("[[nodiscard]] apusr::Status resolveVulkanHostIngress(\n    apusr::frontends::NgxDlssVulkanEvaluateDesc& desc)\n{")
    assert resolve_start >= 0
    resolve_end = NGX.find("[[nodiscard]] std::string getenvString", resolve_start)
    assert resolve_end > resolve_start
    resolve_body = NGX[resolve_start:resolve_end]
    assert "bootstrapApusrVulkanIngress" not in resolve_body
    assert "GetProcAddress(" not in resolve_body
    assert "gApusrVulkanIngressFunctionsReady" in resolve_body
    for legacy_bridge in (
        "apusrSlVulkanDlssEvaluationActive",
        "apusrSlResolveVulkanStreamlineContext",
    ):
        assert legacy_bridge not in NGX, f"NGX must not depend on obsolete private Streamline bridge {legacy_bridge}"

    # D3D preserves stock Streamline semantics too. APUSR may relax
    # discovery/vendor gates, but sl.interposer/sl.common/sl.dlss must own
    # options, tags, resource transitions, proxy unwrapping, and evaluate.
    # The NVIDIA NGX client statically linked into sl.common can reject an
    # unsigned replacement core before LoadLibrary. D3D12 and Vulkan therefore
    # supply APUSR's equivalent common::NGXContext through Streamline's parameter
    # bus; stock sl.dlss still owns every per-frame semantic translation.
    assert "apusrInstallIdentitySpoofHooks" in DXGI
    # Merely loading the DXGI proxy must not start D3D identity hooks. Vulkan
    # hosts can load dxgi.dll incidentally before Streamline selects Vulkan.
    dllmain = DXGI[DXGI.find("BOOL WINAPI DllMain"):]
    assert "CreateThread(" not in dllmain
    assert "apusrInstallIdentitySpoofHooks();" not in dllmain
    # The DXGI proxy still arms the main executable's loader IAT opportunistically.
    # That is useful when DXGI is visible before slInit, but it is not considered
    # a universal early-bootstrap guarantee: some hosts first load DXGI from
    # inside stock slInit, in which case the optional loader-only Streamline shim
    # is the causal pre-slInit boundary.
    assert "apusrInstallIdentitySpoofLoaderHooksEarly();" in dllmain
    early_start = IDENTITY.find('extern "C" bool apusrInstallIdentitySpoofLoaderHooksEarly()')
    early_end = IDENTITY.find('extern "C" void apusrIdentitySpoofSetStreamlineRenderApi', early_start)
    assert early_start >= 0 and early_end > early_start
    early_body = IDENTITY[early_start:early_end]
    assert "GetModuleHandleW(nullptr)" in early_body
    for loader_import in (
        "GetProcAddress",
        "LoadLibraryA",
        "LoadLibraryW",
        "LoadLibraryExA",
        "LoadLibraryExW",
    ):
        assert f'patchImportByName(\n        mainModule, "KERNEL32.dll", "{loader_import}"' in early_body
    assert '"early_main_hooks.ready"' in early_body
    assert "renderer_identity=deferred" in early_body
    for forbidden_under_loader_lock in (
        "patchLoadedModules()",
        "CreateToolhelp32Snapshot(",
        "LoadLibraryExW(",
        "DetourTransactionBegin(",
        "bootstrapStreamlineAttach(",
    ):
        assert forbidden_under_loader_lock not in early_body
    assert 'patchImportByName(module, "sl.interposer.dll", "slEvaluateFeature"' not in IDENTITY
    assert 'patchImportByName(module, "sl.interposer.dll", "slSetTag"' not in IDENTITY
    assert 'patchImportByName(module, "sl.interposer.dll", "slSetConstants"' not in IDENTITY
    assert 'patchImportByName(module, "sl.interposer.dll", "slGetFeatureFunction"' not in IDENTITY
    assert 'patchImportByName(module, "sl.common.dll", "slGetPluginFunction"' in IDENTITY
    # D3D discovery must become valid before stock sl.dlss builds its plugin JSON.
    # Preserve the exact NV_LOGICAL_GPU_DATA_V1 x64 prefix that sl.common consumes:
    # version, pOSAdapterId, physicalGpuCount, then the physical handles.
    assert "void* pOSAdapterId;" in NVAPI
    assert "offsetof(NvLogicalGpuDataLoose, pOSAdapterId) == 8" in NVAPI
    assert "offsetof(NvLogicalGpuDataLoose, physicalGpuCount) == 16" in NVAPI
    assert "offsetof(NvLogicalGpuDataLoose, physicalGpuHandles) == 24" in NVAPI
    assert "sizeof(NvLogicalGpuDataLoose) == 568" in NVAPI
    assert "data->pOSAdapterId" in NVAPI
    assert "std::memcpy(data->pOSAdapterId, &g_primaryAdapter.luid" in NVAPI
    assert "LUID osAdapterId;" not in NVAPI
    assert '"nvapi", "logical_gpu.info"' in NVAPI
    # A post-return plugin-JSON patch is too late to make sl.common initialize NGX.
    assert "patchPluginJson" not in IDENTITY
    assert "g_patchedDlssJson" not in IDENTITY
    plugin_load_start = IDENTITY.find("bool __cdecl hkSlOnPluginLoad")
    plugin_load_end = IDENTITY.find("int __cdecl hkSlIsSupported", plugin_load_start)
    assert plugin_load_start >= 0 and plugin_load_end > plugin_load_start
    plugin_load_body = IDENTITY[plugin_load_start:plugin_load_end]
    assert "g_slOnPluginLoad(params, loaderJSON, pluginJSON)" in plugin_load_body
    assert "return ok;" in plugin_load_body
    assert "patched=0 forced_ok=0" in plugin_load_body
    is_supported_start = IDENTITY.find("int __cdecl hkSlIsSupported")
    is_supported_end = IDENTITY.find("bool __cdecl hkSlOnPluginStartup", is_supported_start)
    assert is_supported_start >= 0 and is_supported_end > is_supported_start
    is_supported_body = IDENTITY[is_supported_start:is_supported_end]
    assert "return result;" in is_supported_body
    assert "forced_result" not in is_supported_body
    startup_start = IDENTITY.find("bool __cdecl hkSlOnPluginStartup")
    startup_end = IDENTITY.find("void __cdecl hkSlOnPluginShutdown", startup_start)
    assert startup_start >= 0 and startup_end > startup_start
    startup_body = IDENTITY[startup_start:startup_end]
    assert "g_slOnPluginStartup(jsonConfig, device)" in startup_body
    assert "apusrStreamlineRenderApiFromStartupJson(jsonConfig)" in startup_body
    assert "apusrInstallStreamlineNgxContextBridge(renderApi, device)" in startup_body
    assert "renderApi == kSlRenderApiD3d12 || renderApi == kSlRenderApiVulkan" in startup_body
    assert "bridgeInstalled ? 1 : 0" in startup_body
    assert "rearmD3d12ReflexAfterRendererRestart(device)" in startup_body
    assert startup_body.find("rearmD3d12ReflexAfterRendererRestart(device)") < startup_body.find("g_slOnPluginStartup(jsonConfig, device)")
    assert startup_body.find("apusrInstallStreamlineNgxContextBridge(renderApi, device)") < startup_body.find("g_slOnPluginStartup(jsonConfig, device)")
    assert "skipped_original" not in startup_body
    assert 'constexpr char key[]' in IDENTITY and 'deviceType' in IDENTITY

    # The bridge is cold-path context injection, not a resurrection of APUSR's
    # retired Streamline evaluator. It must expose the exact 2.x parameter key,
    # use APUSR capability params, and hand stock sl.dlss callbacks that operate
    # on the native command list commonInterface already unwrapped.
    assert 'kStreamlineNgxContextParameter[] = "sl.param.global.ngxContext"' in IDENTITY
    assert "struct StreamlineNgxContextBridge" in IDENTITY
    assert "struct StreamlineParameterInterface" in IDENTITY
    assert "virtual void set(const char*, void*) = 0;" in IDENTITY
    assert "NVSDK_NGX_D3D12_GetCapabilityParameters" in IDENTITY
    assert "NVSDK_NGX_VULKAN_GetCapabilityParameters" in IDENTITY
    assert "NVSDK_NGX_VULKAN_Init" in IDENTITY
    assert "struct StreamlineVkDevicesBridge" in IDENTITY
    assert "sizeof(StreamlineVkDevicesBridge) == 24" in IDENTITY
    assert "offsetof(StreamlineVkDevicesBridge, device) == 8" in IDENTITY
    assert "offsetof(StreamlineVkDevicesBridge, physical) == 16" in IDENTITY
    assert "apusrStreamlineNgxCreateFeature" in IDENTITY
    assert "apusrStreamlineNgxVulkanCreateFeature" in IDENTITY
    assert "apusrStreamlineNgxVulkanEvaluateFeature" in IDENTITY
    assert "apusrStreamlineNgxVulkanReleaseFeature" in IDENTITY
    assert "apusrStreamlineNgxEvaluateFeature" in IDENTITY
    assert "apusrStreamlineNgxReleaseFeature" in IDENTITY
    assert "apusrStreamlineNgxUpdateFeature" in IDENTITY
    assert "signed_core_loader_bypassed=1" in IDENTITY
    assert "source=native_command_list" in IDENTITY
    assert "source=slOnPluginStartup_vk_devices" in IDENTITY
    assert 'backend=vulkan reason=missing_vk_devices' in IDENTITY
    assert 'g_streamlineNgxContextRenderApi != renderApi' in IDENTITY
    assert 'renderApi == kSlRenderApiD3d12 &&' in IDENTITY
    assert 'backend=d3d12 reason=prior_device_loss' in IDENTITY
    # Stock sl.dlss prepares D3D12 resource states immediately before entering
    # NGX. Preserve that provenance across the bridge rather than fabricating
    # COMMON, and stop reinjection after any APUSR evaluation removes the host
    # device so a renderer reset cannot become an infinite loop.
    assert 'kApusrStreamlineD3D12PreparedStatesParameter[]' in IDENTITY
    assert '"APUSR.Streamline.D3D12.PreparedStates"' in IDENTITY
    assert 'g_streamlineNgxContext.params->Set(kApusrStreamlineD3D12PreparedStatesParameter, 1u);' in IDENTITY
    assert 'g_streamlineNgxContext.params->Set(kApusrStreamlineD3D12PreparedStatesParameter, 0u);' in IDENTITY
    assert 'g_streamlineNgxDeviceLossCircuitOpen' in IDENTITY
    assert '"ngx_context.circuit_open"' in IDENTITY
    assert '"ngx_context.inject.blocked"' in IDENTITY
    assert 'GetDeviceRemovedReason()' in IDENTITY
    assert 'getUInt(params, "APUSR.Streamline.D3D12.PreparedStates", streamlinePreparedStates)' in NGX
    assert 'D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE' in NGX
    assert 'D3D12_RESOURCE_STATE_UNORDERED_ACCESS' in NGX

    # NGX capability/optimal-settings parameters are mutable scratch state.
    # Stock sl.dlss writes the recommended render extent into OutWidth/OutHeight
    # during settings queries, so D3D12 evaluation must keep the feature-create
    # display extent authoritative just like the Vulkan path already does.
    eval_desc_start = NGX.find("[[nodiscard]] apusr::frontends::NgxDlssEvaluateDesc evaluateDescFromParams(")
    eval_desc_end = NGX.find("[[nodiscard]] apusr::Status resolveVulkanHostIngress", eval_desc_start)
    assert eval_desc_start >= 0 and eval_desc_end > eval_desc_start
    eval_desc_body = NGX[eval_desc_start:eval_desc_end]
    assert "const unsigned int displayWidth = feature.create.displaySize.width;" in eval_desc_body
    assert "const unsigned int displayHeight = feature.create.displaySize.height;" in eval_desc_body
    assert "getUIntAny(params, {keyOutWidth()" not in eval_desc_body
    assert "getUIntAny(params, {keyOutHeight()" not in eval_desc_body
    assert "Vulkan already preserves feature.create.displaySize" in eval_desc_body

    # A concrete game-owned D3D12 resource format is authoritative. Do not
    # substitute a frontend default for R8G8B8A8 and create an incompatible
    # void-returning SRV/UAV call. External borrowed descriptors are validated
    # in release builds, independently of optional hot-path guards.
    native_format_start = DX12_BACKEND.find("DXGI_FORMAT nativeViewBaseFormat(")
    native_format_end = DX12_BACKEND.find("bool resourceFormatSupportsSrv", native_format_start)
    assert native_format_start >= 0 and native_format_end > native_format_start
    native_format_body = DX12_BACKEND[native_format_start:native_format_end]
    assert "return nativeFormat;" in native_format_body
    assert "case DXGI_FORMAT_R8G8B8A8_UNORM:" not in native_format_body
    descriptor_start = DX12_BACKEND.find("Status prepareDescriptors(")
    descriptor_end = DX12_BACKEND.find("Status prepareConstantUploads(", descriptor_start)
    assert descriptor_start >= 0 and descriptor_end > descriptor_start
    descriptor_body = DX12_BACKEND[descriptor_start:descriptor_end]
    external_validation = descriptor_body.find("validateExternalBorrowedDescriptorViewCached(")
    hot_guard = descriptor_body.find("#if APUSR_ENABLE_HOT_PATH_GUARDS")
    assert external_validation >= 0
    assert hot_guard < 0 or external_validation < hot_guard
    assert 'validateDeviceHealth("record_frame.prepare_descriptors")' in DX12_BACKEND
    # Bridge installation is scoped to the current Streamline parameter bus,
    # not a process-global one-shot boolean. Games may rebuild sl.common/sl.dlss
    # when the user changes DLSS quality without restarting the process.
    assert "g_streamlineNgxContextInstalledBus" in IDENTITY
    assert "g_streamlineParameterBusEpoch" in IDENTITY
    assert "g_streamlineNgxContextInstalled{" not in IDENTITY
    assert "g_streamlineNgxContextInstallFailed{" not in IDENTITY
    assert '"ngx_context.parameter_bus.changed"' in IDENTITY
    assert "ngx_context_republish_required=1" in IDENTITY
    bridge_install_start = IDENTITY.find("bool apusrInstallStreamlineNgxContextBridge(const std::uint32_t renderApi, void* startupDevice)\n{")
    bridge_install_end = IDENTITY.find("void apusrSetNgxPointer", bridge_install_start)
    assert bridge_install_start >= 0 and bridge_install_end > bridge_install_start
    bridge_install_body = IDENTITY[bridge_install_start:bridge_install_end]
    assert "parameterBus->set(" in bridge_install_body
    assert "parameterBus->get(" in bridge_install_body
    assert "if (g_streamlineNgxContext.params == nullptr)" in bridge_install_body
    assert "g_streamlineNgxContextInstalledBus == parameterBus" in bridge_install_body
    core_resolver_start = IDENTITY.find("FARPROC apusrCoreStreamlineProc")
    core_resolver_end = IDENTITY.find("FARPROC WINAPI hkGetProcAddress", core_resolver_start)
    assert core_resolver_start >= 0 and core_resolver_end > core_resolver_start
    core_resolver_body = IDENTITY[core_resolver_start:core_resolver_end]
    assert "return nullptr;" in core_resolver_body
    assert 'std::strcmp(procName, "slEvaluateFeature")' not in core_resolver_body
    assert "streamlineScale == 0.0f" in IDENTITY
    assert "streamlineScale <= 0.0f" not in IDENTITY
    assert "validateLiveD3D12ExecutionObjects" in NGX
    assert '"D3D12.device_removed"' in NGX
    assert '"D3D12.command_device_mismatch"' in NGX
    assert "apusrAttachToLoadedStreamline" not in NVAPI
    assert "apusr_streamline_attach.dll" not in NVAPI

    # A host can reject Vulkan DLSS before any NGX Vulkan entrypoint is reached.
    # Break that bootstrap cycle at stock slInit: the DXGI identity layer may
    # observe slInit via GetProcAddress, inspect the explicit renderAPI, and arm
    # only renderer-appropriate private attachment before stock device creation.
    assert 'std::strcmp(procName, "slInit") == 0' in IDENTITY
    assert "hkSlInit" in IDENTITY
    assert 'bootstrapStreamlineAttach(renderApi, "slInit_vulkan_pre_device")' in IDENTITY
    assert 'bootstrapStreamlineAttach(renderApi, "slInit_d3d12_reflex")' in IDENTITY
    assert "renderApi == kSlRenderApiVulkan" in IDENTITY
    assert "renderApi == kSlRenderApiD3d12 && reflexRequested" in IDENTITY
    slinit_start = IDENTITY.find("int __cdecl hkSlInit")
    slinit_end = IDENTITY.find("FARPROC WINAPI hkGetProcAddress", slinit_start)
    assert slinit_start >= 0 and slinit_end > slinit_start
    slinit_body = IDENTITY[slinit_start:slinit_end]
    assert "g_apusrStreamlineRenderApi.store(renderApi, std::memory_order_release);" in slinit_body
    assert slinit_body.find("g_apusrStreamlineRenderApi.store") < slinit_body.find("bootstrapStreamlineAttach")
    assert "apusrStreamlineAttachHintRenderApi" in IDENTITY
    assert "apusrAttachToLoadedStreamline" in IDENTITY
    assert "loadStreamlineAttachForBootstrap" in IDENTITY
    assert 'moduleDirectory(GetModuleHandleW(L"sl.interposer.dll"))' in IDENTITY
    identity_dllmain = IDENTITY[IDENTITY.rfind("BOOL WINAPI DllMain"):]
    assert "bootstrapStreamlineAttach" not in identity_dllmain, "attachment bootstrap must never run under loader lock"

    # Keep the NGX-side Vulkan bootstrap as a late/redundant recovery seam, but
    # D3D NGX still must never own Streamline attachment.
    assert "bootstrapApusrVulkanIngress" in NGX
    assert "NVSDK_NGX_VULKAN_GetFeatureRequirements" in NGX
    assert 'LoadLibraryW(path.c_str())' in NGX
    d3d_start = NGX.find("NVSDK_NGX_D3D12_Init(")
    vulkan_start = NGX.find("NVSDK_NGX_VULKAN_Init(")
    assert d3d_start >= 0 and vulkan_start > d3d_start
    assert "bootstrapApusrVulkanIngress" not in NGX[d3d_start:vulkan_start], (
        "D3D NGX must not bootstrap Streamline attachment"
    )
    assert "bootstrapApusrVulkanIngress" in NGX[vulkan_start:]

    # D3D12 Reflex is now an APUSR-owned translation to AMD Anti-Lag 2 while
    # DLSS-SR remains stock Streamline + APUSR NGXContext. The attachment must
    # unwrap Streamline's D3D12 proxy before initializing the AMD driver API.
    assert '#include "../../framegen/ffx_dx12/antilag2/ffx_antilag2_dx12.h"' in ATTACH
    assert '{"slGetNativeInterface", nullptr}' in ATTACH
    assert 'APUSR_SL_DETOUR("slGetNativeInterface", slGetNativeInterface);' in ATTACH
    assert 'originalFn<SlGetNativeInterfaceFn>("slGetNativeInterface")' in ATTACH
    assert "AMD::AntiLag2DX12::Initialize" in ATTACH
    assert "AMD::AntiLag2DX12::Update" in ATTACH
    assert "AMD::AntiLag2DX12::MarkEndOfFrameRendering" in ATTACH
    assert "bindD3d12AntiLagNativeDevice" in ATTACH
    assert 'GetProcAddress(module, "apusrStreamlineAttachBindD3D12NativeDevice")' in IDENTITY
    assert '"attach.d3d12_reflex_rearmed"' in IDENTITY
    assert "g_streamlineReflexRequested.store(reflexRequested, std::memory_order_release);" in slinit_body
    assert "kSlRenderApiD3D12" not in IDENTITY
    assert "kSlRenderApiD3D12" not in ATTACH
    assert 'bootstrapStreamlineAttach(kSlRenderApiD3d12, "plugin_startup_d3d12_reflex_rearm")' in IDENTITY
    assert "g_slRenderApi.store(kSlRenderApiD3d12, std::memory_order_relaxed);" in ATTACH
    assert "isD3d12StreamlinePath()" in ATTACH
    assert "isReflexStreamlinePath()" in ATTACH
    assert '"reflex.d3d12.antilag_ready"' in ATTACH
    assert '"reflex.d3d12.sleep_failed"' in ATTACH
    assert "feature == kFeatureReflex && isReflexStreamlinePath()" in ATTACH
    assert "feature == kFeaturePCL && isReflexStreamlinePath()" in ATTACH

    # FeatureRequirements::flags describes which rendering APIs a feature can
    # support; it is not the renderer currently selected by the host. A previous
    # implementation treated eVulkanSupported as an active-renderer signal and
    # could flip a working D3D12 session to Vulkan during a support query.
    req_start = ATTACH.find("APUSR_SL_EXPORT int __cdecl slGetFeatureRequirements")
    req_end = ATTACH.find("APUSR_SL_EXPORT int __cdecl slGetFeatureVersion", req_start)
    assert req_start >= 0 and req_end > req_start
    req_body = ATTACH[req_start:req_end]
    assert "if (!isVulkanStreamlinePath())" in req_body
    assert "stockSaysVulkan" not in req_body
    assert "requirements->flags & (1u << 2)" not in req_body
    assert "g_slRenderApi.store(kSlRenderApiVulkan" not in req_body

    # Some hosts enumerate DXGI/KMT identity before Streamline tells us which renderer
    # it will use. That early identity must stay real: otherwise Vulkan later
    # sees an AMD VkPhysicalDevice while the engine has cached an RTX adapter and
    # falls back before creating a VkDevice. Once Vulkan is explicit, only stock
    # Streamline's own DXGI consumers get the NVIDIA vendor shape; host/game
    # callers keep the real AMD identity. D3D12 retains the established global
    # spoof after its renderer is explicit.
    assert "apusrIdentitySpoofGetStreamlineRenderApi" in DXGI
    assert "shouldSpoofDxgiIdentityForCaller" in DXGI
    assert "isStreamlineIdentityConsumer" in DXGI
    assert "adapter.desc.pre_renderer_passthrough" in DXGI
    assert "adapter.desc.vulkan_host_passthrough" in DXGI
    assert "adapter.desc.vulkan_streamline_scoped_spoof" in DXGI
    assert "renderApi == kSlRenderApiD3d12" in DXGI
    assert "renderApi == kSlRenderApiVulkan" in DXGI
    assert "spoofDeviceIdForCurrentRenderer" in DXGI
    assert "adapter.desc.vulkan_identity_bridge" in DXGI
    assert "return realDeviceId;" in DXGI
    assert "desc->DeviceId = spoofDeviceIdForCurrentRenderer(desc->DeviceId);" in DXGI
    assert "shouldSpoofGlobalD3dIdentityNow" in IDENTITY
    assert "== kSlRenderApiD3d12" in IDENTITY
    assert "result && shouldSpoofGlobalD3dIdentityNow()" in IDENTITY
    assert "result >= 0 && shouldSpoofGlobalD3dIdentityNow()" in IDENTITY

    # Some hosts resolve a persistent Vulkan dispatch table before their Streamline
    # slInit call. A capture install that waits for slInit can therefore observe
    # vkCreateInstance/enumeration yet miss the later vkCreateDevice call through
    # an already-cached loader trampoline. Loader/proc interception must arm the
    # passive Vulkan capture at the first real vulkan-1.dll load/proc lookup,
    # without selecting Vulkan or changing D3D identity.
    assert "ensureEarlyVulkanHostCapture" in IDENTITY
    assert '"vulkan-1.dll"' in IDENTITY
    assert '"capture.early_ready"' in IDENTITY
    assert '"vulkan_loader_getproc"' in IDENTITY
    assert '"LoadLibraryW_vulkan_loader"' in IDENTITY
    assert '"LoadLibraryExW_vulkan_loader"' in IDENTITY
    assert "renderer-neutral loader hooks above remain active" in IDENTITY

    # Early capture installation itself calls loader APIs. Those calls must not
    # recursively enter the same installer or patch APUSR support DLLs while a
    # Detours transaction is in progress; a prior crash exposed
    # this as MSVC's "resource deadlock would occur" same-thread mutex relock.
    assert "g_vulkanEarlyCaptureInstallingOnThisThread" in IDENTITY
    assert '"capture.early_reentrant_suppressed"' in IDENTITY
    assert 'return _wcsnicmp(base, L"apusr_", 6u) == 0;' in IDENTITY
    assert "if (g_vulkanEarlyCaptureInstallingOnThisThread)" in IDENTITY

    patch_start = IDENTITY.find("void patchModule(HMODULE module)")
    patch_end = IDENTITY.find("void patchLoadedModules()", patch_start)
    assert patch_start >= 0 and patch_end > patch_start
    patch_body = IDENTITY[patch_start:patch_end]
    getproc_pos = patch_body.find('"GetProcAddress"')
    vulkan_return_pos = patch_body.find("if (streamlineVulkan)")
    assert getproc_pos >= 0 and vulkan_return_pos > getproc_pos, (
        "renderer-neutral loader hooks must be installed before Vulkan skips D3D identity hooks"
    )

    # A Streamline 2.12 integration can issue a transient
    # Vulkan-1.1 adapter probe failing with VK_ERROR_INCOMPATIBLE_DRIVER while
    # a following Vulkan-1.3 instance succeeds. The compatibility retry must be
    # tightly scoped to that probe shape and only run after the original call
    # returned -9; normal application instance contracts remain untouched.
    VK_CAPTURE = (ROOT / "src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_core.cpp").read_text(encoding="utf-8")
    VK_CAPTURE_H = (ROOT / "src/frontends/vulkan_host_capture/apusr_vulkan_host_capture_core.h").read_text(encoding="utf-8")
    assert "VkErrorIncompatibleDriver = -9" in VK_CAPTURE_H
    assert "looksLikeStreamlineVulkanAdapterProbe" in VK_CAPTURE
    assert 'kVkKhrWin32SurfaceExtension[] = "VK_KHR_win32_surface"' in VK_CAPTURE
    assert "app->apiVersion != kVkApiVersion11" in VK_CAPTURE
    assert "createInfo.enabledExtensionCount != 2u" in VK_CAPTURE
    assert "result == VkErrorIncompatibleDriver && streamlineAdapterProbe" in VK_CAPTURE
    assert "retryApp.apiVersion = kVkApiVersion13" in VK_CAPTURE
    assert '"streamline.adapter_probe.retry"' in VK_CAPTURE
    assert "Do not rewrite normal" in VK_CAPTURE
    assert '"physical_device.identity"' in VK_CAPTURE
    assert "properties.vendorID" in VK_CAPTURE
    assert "properties.deviceID" in VK_CAPTURE

    if args.dll is not None:
        exports = load_pe_exports(args.dll)
        assert set(exports) == expected_private_exports, (
            f"attach PE must export only its private APUSR entry points, got {sorted(exports)}"
        )

    if args.stock_dll is not None:
        stock_exports = load_pe_exports(args.stock_dll)
        required_stock_exports = {
            "slInit",
            "slEvaluateFeature",
            "slGetFeatureFunction",
            "slGetFeatureRequirements",
            "slIsFeatureSupported",
            "slSetVulkanInfo",
            "vkCreateDevice",
        }
        assert required_stock_exports <= set(stock_exports), (
            f"stock Streamline image is missing the named attach contract: "
            f"{sorted(required_stock_exports - set(stock_exports))}"
        )
        legacy_markers = {
            "apusrSlResolveVulkanQueue",
            "apusrSlResolveVulkanStreamlineContext",
            "apusrSlVulkanDlssEvaluationActive",
        }
        assert legacy_markers.isdisjoint(stock_exports), (
            "stock Streamline image contains legacy APUSR private exports: "
            f"{sorted(legacy_markers & set(stock_exports))}"
        )

    print("APUSR stock Streamline attach contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
