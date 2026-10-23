// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstdint>

#if defined(_WIN32)
#define APUSR_FRAMEGEN_EXPORT extern "C" __declspec(dllexport)
#else
#define APUSR_FRAMEGEN_EXPORT extern "C"
#endif

namespace {
constexpr const char kProviderName[] = "APUSR Frame Generation DX12 provider (FidelityFX-compatible)";
constexpr const char kProviderStage[] = "apusr-maintained-framegen-core";
constexpr std::uint32_t kProviderAbiVersion = 1;
}

APUSR_FRAMEGEN_EXPORT const char* apusrFramegenProviderName()
{
    return kProviderName;
}

APUSR_FRAMEGEN_EXPORT const char* apusrFramegenProviderStage()
{
    return kProviderStage;
}

APUSR_FRAMEGEN_EXPORT std::uint32_t apusrFramegenProviderAbiVersion()
{
    return kProviderAbiVersion;
}
