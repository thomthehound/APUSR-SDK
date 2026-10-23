// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#if defined(_WIN32)
#include <cstdint>

extern "C" void apusrInstallIdentitySpoofHooks();
extern "C" void apusrRefreshIdentitySpoofHooks();
// Arms only renderer-neutral LoadLibrary/GetProcAddress interception in the
// main executable. This is safe before Streamline has selected a renderer and
// lets hosts that initialize Streamline before their first DXGI call expose
// slInit to APUSR's normal renderer-aware bootstrap.
extern "C" bool apusrInstallIdentitySpoofLoaderHooksEarly();
extern "C" void apusrIdentitySpoofSetStreamlineRenderApi(std::uint32_t renderApi);
extern "C" std::uint32_t apusrIdentitySpoofGetStreamlineRenderApi();
#else
inline void apusrInstallIdentitySpoofHooks() {}
inline void apusrRefreshIdentitySpoofHooks() {}
inline bool apusrInstallIdentitySpoofLoaderHooksEarly() { return false; }
inline void apusrIdentitySpoofSetStreamlineRenderApi(std::uint32_t) {}
inline std::uint32_t apusrIdentitySpoofGetStreamlineRenderApi() { return 0xffffffffu; }
#endif
