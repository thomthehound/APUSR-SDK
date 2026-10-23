// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

// Private APUSR-to-APUSR ABI. The attach layer always targets the exact stock
// Streamline PE. Normally that PE is the host-loaded sl.interposer.dll; the
// optional loader-only bootstrap may preserve it under a sidecar name and pass
// its module handle explicitly.
enum : std::uint32_t {
    APUSR_STREAMLINE_RENDER_API_D3D11 = 0u,
    APUSR_STREAMLINE_RENDER_API_D3D12 = 1u,
    APUSR_STREAMLINE_RENDER_API_VULKAN = 2u,
};

#if defined(_WIN32)
using ApusrAttachToLoadedStreamlineFn = bool(__cdecl*)();
using ApusrAttachToStreamlineModuleFn = bool(__cdecl*)(void* module);
using ApusrStreamlineAttachHintRenderApiFn = void(__cdecl*)(std::uint32_t renderApi);
using ApusrStreamlineAttachGetRenderApiFn = std::uint32_t(__cdecl*)();
using ApusrStreamlineAttachBindD3D12NativeDeviceFn = bool(__cdecl*)(void* nativeDevice);
using ApusrStreamlineVulkanDlssEvaluationActiveFn = bool(__cdecl*)();
using ApusrStreamlineAttachInstalledFn = bool(__cdecl*)();
#endif
