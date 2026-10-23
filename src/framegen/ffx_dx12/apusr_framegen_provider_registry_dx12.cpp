// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_provider_registry.h"
#include "ffx_provider_fsr3framegeneration.h"
#include "apusr_framegen_swapchain_provider_dx12.h"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
constexpr std::array<const ApusrFramegenProvider*, 2> kProviders = {
    &ffxProvider_Fsr3FrameGeneration::Instance,
    &ApusrFramegenSwapchainProviderDx12::Instance,
};

const ApusrFramegenProvider* findProviderById(ffxStructType_t type, std::uint64_t id) noexcept
{
    for (const ApusrFramegenProvider* provider : kProviders)
        if (provider->id() == id && provider->canProvide(type))
            return provider;
    return nullptr;
}

const ApusrFramegenProvider* findSupportedProvider(ffxStructType_t type, void* device) noexcept
{
    for (const ApusrFramegenProvider* provider : kProviders)
        if (provider->canProvide(type) && provider->isSupported(device))
            return provider;
    return nullptr;
}
} // namespace

const ApusrFramegenProvider* apusrFramegenFindProvider(ffxStructType_t type,
                                                       std::uint64_t overrideId,
                                                       void* device) noexcept
{
    return overrideId ? findProviderById(type, overrideId) : findSupportedProvider(type, device);
}

const ApusrFramegenProvider* apusrFramegenAssociatedProvider(ffxContext* context) noexcept
{
    if (!context || !*context)
        return nullptr;
    return static_cast<const ApusrFramegenContextHeader*>(*context)->provider;
}

std::uint64_t apusrFramegenProviderCount(ffxStructType_t type, void* device) noexcept
{
    return apusrFramegenProviderVersions(type,
                                         device,
                                         std::numeric_limits<std::uint64_t>::max(),
                                         nullptr,
                                         nullptr);
}

std::uint64_t apusrFramegenProviderVersions(ffxStructType_t type,
                                            void* device,
                                            std::uint64_t capacity,
                                            std::uint64_t* versionIds,
                                            const char** versionNames) noexcept
{
    std::uint64_t count = 0;
    for (const ApusrFramegenProvider* provider : kProviders) {
        if (count >= capacity)
            break;
        if (!provider->canProvide(type) || !provider->isSupported(device))
            continue;

        if (versionIds)
            versionIds[count] = provider->id();
        if (versionNames)
            versionNames[count] = provider->versionName();
        ++count;
    }

    if (versionIds) {
        for (std::uint64_t i = 1; i < count; ++i) {
            std::uint64_t j = i;
            while (j > 0 && (versionIds[j] & 0x7fffffffu) >= (versionIds[j - 1] & 0x7fffffffu)) {
                std::swap(versionIds[j], versionIds[j - 1]);
                if (versionNames)
                    std::swap(versionNames[j], versionNames[j - 1]);
                --j;
            }
        }
    }
    return count;
}
