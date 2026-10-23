// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_provider_registry.h"

#include "ffx_api.h"
#include "ffx_api.hpp"
#include "ffx_api_dx12.h"
#include "ffx_api_framegeneration_dx12.h"
#include "ffx_framegeneration.h"
#include "ffx_message.h"

#include <cstdint>

namespace {

ApusrFramegenAllocator makeAllocator(const ffxAllocationCallbacks* callbacks) noexcept
{
    if (!callbacks)
        return {};
    return {callbacks->pUserData, callbacks->alloc, callbacks->dealloc};
}
std::uint64_t versionOverride(const ffxApiHeader* header) noexcept
{
    for (auto* it = header; it; it = it->pNext)
        if (auto* version = ffx::DynamicCast<ffxOverrideVersion>(it))
            return version->versionId;
    return 0;
}

void* descriptorDevice(const ffxApiHeader* header) noexcept
{
    for (auto* it = header; it; it = it->pNext) {
        switch (it->type) {
        case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS:
            return reinterpret_cast<const ffxQueryDescGetVersions*>(it)->device;
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12:
            return reinterpret_cast<const ffxCreateBackendDX12Desc*>(it)->device;
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2:
            return reinterpret_cast<const ffxQueryDescFrameGenerationGetGPUMemoryUsageV2*>(it)->device;
        case FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2:
            return reinterpret_cast<const ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2*>(it)->device;
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12:
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12:
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: {
            ID3D12CommandQueue* queue = nullptr;
            if (it->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12)
                queue = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*>(it)->gameQueue;
            else if (it->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12)
                queue = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12*>(it)->gameQueue;
            else
                queue = reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12*>(it)->gameQueue;

            if (!queue)
                return nullptr;
            ID3D12Device* device = nullptr;
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
                return nullptr;
            device->Release();
            return device; // identity only; caller never dereferences this borrowed value.
        }
        default:
            break;
        }
    }
    return nullptr;
}

ffxReturnCode_t queryFallback(ffxContext* context,
                              ffxQueryDescHeader* header,
                              ffxReturnCode_t result) noexcept
{
    if (!context) {
        if (result == FFX_API_RETURN_OK)
            return result;

        if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE) {
            auto* desc = reinterpret_cast<ffxQueryDescFrameGenerationGetGPUMemoryUsage*>(header);
            if (desc->gpuMemoryUsageFrameGeneration)
                *desc->gpuMemoryUsageFrameGeneration = {};
        } else if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2) {
            auto* desc = reinterpret_cast<ffxQueryDescFrameGenerationGetGPUMemoryUsageV2*>(header);
            if (desc->gpuMemoryUsageFrameGeneration)
                *desc->gpuMemoryUsageFrameGeneration = {};
            result = FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE;
        } else if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12) {
            auto* desc = reinterpret_cast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12*>(header);
            if (desc->gpuMemoryUsageFrameGenerationSwapchain)
                *desc->gpuMemoryUsageFrameGenerationSwapchain = {};
            result = FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE;
        } else if (header->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2) {
            auto* desc = reinterpret_cast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2*>(header);
            if (desc->gpuMemoryUsageFrameGenerationSwapchain)
                *desc->gpuMemoryUsageFrameGenerationSwapchain = {};
            result = FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE;
        }
        return result;
    }

    if (result == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE &&
        header->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) {
        const ApusrFramegenProvider* provider = apusrFramegenAssociatedProvider(context);
        if (!provider)
            return FFX_API_RETURN_NO_PROVIDER;
        auto* desc = reinterpret_cast<ffxQueryGetProviderVersion*>(header);
        desc->versionId = provider->id();
        desc->versionName = provider->versionName();
        return FFX_API_RETURN_OK;
    }
    return result;
}
} // namespace

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context,
                                               ffxCreateContextDescHeader* desc,
                                               const ffxAllocationCallbacks* callbacks)
{
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    *context = nullptr;

    const ApusrFramegenProvider* provider =
        apusrFramegenFindProvider(desc->type, versionOverride(desc), descriptorDevice(desc));
    if (!provider)
        return FFX_API_RETURN_NO_PROVIDER;

    return provider->createContext(context, desc, makeAllocator(callbacks));
}

FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context,
                                                const ffxAllocationCallbacks* callbacks)
{
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    const ApusrFramegenProvider* provider = apusrFramegenAssociatedProvider(context);
    return provider ? provider->destroyContext(context, makeAllocator(callbacks))
                    : FFX_API_RETURN_NO_PROVIDER;
}

FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;

    if (auto* global = ffx::DynamicCast<ffxConfigureDescGlobalDebug>(desc)) {
        ffxSetPrintMessageCallback(reinterpret_cast<ffxMessageCallback>(global->fpMessage), global->debugLevel);
        return FFX_API_RETURN_OK;
    }
    if (auto* global = ffx::DynamicCast<ffxConfigureDescGlobalDebug1>(desc)) {
        ffxSetPrintMessageCallback(reinterpret_cast<ffxMessageCallback>(global->fpMessage), global->debugLevel);
        return FFX_API_RETURN_OK;
    }

    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    const ApusrFramegenProvider* provider = apusrFramegenAssociatedProvider(context);
    return provider ? provider->configure(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* header)
{
    if (!header)
        return FFX_API_RETURN_ERROR_PARAMETER;

    if (!context) {
        if (auto* versions = ffx::DynamicCast<ffxQueryDescGetVersions>(header)) {
            if (versions->outputCount && (*versions->outputCount == 0 || (!versions->versionIds && !versions->versionNames))) {
                *versions->outputCount = apusrFramegenProviderCount(versions->createDescType, versions->device);
            } else if (versions->outputCount && *versions->outputCount > 0) {
                const std::uint64_t capacity = *versions->outputCount;
                *versions->outputCount = apusrFramegenProviderVersions(versions->createDescType,
                                                                       versions->device,
                                                                       capacity,
                                                                       versions->versionIds,
                                                                       versions->versionNames);
            }
            return FFX_API_RETURN_OK;
        }

        const ApusrFramegenProvider* provider =
            apusrFramegenFindProvider(header->type, versionOverride(header), descriptorDevice(header));
        const ffxReturnCode_t result = provider ? provider->query(nullptr, header) : FFX_API_RETURN_NO_PROVIDER;
        return queryFallback(nullptr, header, result);
    }

    if (!*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    const ApusrFramegenProvider* provider = apusrFramegenAssociatedProvider(context);
    const ffxReturnCode_t result = provider ? provider->query(context, header) : FFX_API_RETURN_NO_PROVIDER;
    return queryFallback(context, header, result);
}

FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    const ApusrFramegenProvider* provider = apusrFramegenAssociatedProvider(context);
    return provider ? provider->dispatch(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}
