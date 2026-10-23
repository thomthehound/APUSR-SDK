// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_swapchain_provider_dx12.h"

#include "apusr_framegen_present_dx12.h"
#include "ffx_api_framegeneration_dx12.hpp"

#include <cstring>
#include <new>

namespace {
struct InternalFgScContext {
    ApusrFramegenContextHeader header;
    IDXGISwapChain4* swapchain = nullptr;
};

ffxReturnCode_t runtimeResult(FfxErrorCode error) noexcept
{
    return error == FFX_OK ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_RUNTIME_ERROR;
}

InternalFgScContext* allocateContext(const ApusrFramegenAllocator& alloc, const ApusrFramegenProvider* provider) noexcept
{
    void* storage = alloc.alloc(sizeof(InternalFgScContext));
    if (!storage)
        return nullptr;

    auto* context = ::new (storage) InternalFgScContext{};
    context->header.provider = provider;
    return context;
}

void freeContext(const ApusrFramegenAllocator& alloc, InternalFgScContext* context) noexcept
{
    if (!context)
        return;
    context->~InternalFgScContext();
    alloc.dealloc(context);
}
} // namespace

bool ApusrFramegenSwapchainProviderDx12::canProvide(ffxStructType_t type) const noexcept
{
    return (type & FFX_API_EFFECT_MASK) == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN &&
           (type & FFX_API_BACKEND_MASK) == FFX_API_BACKEND_ID_DX12;
}

std::uint64_t ApusrFramegenSwapchainProviderDx12::id() const noexcept
{
    // Preserve the FidelityFX-compatible swapchain provider ABI/version identity.
    return 0xF65C'DD12'0300'1005ULL;
}

const char* ApusrFramegenSwapchainProviderDx12::versionName() const noexcept
{
    return "3.1.5";
}

ffxReturnCode_t ApusrFramegenSwapchainProviderDx12::createContext(
    ffxContext* context, ffxCreateContextDescHeader* header, const ApusrFramegenAllocator& alloc) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(header, FFX_API_RETURN_ERROR_PARAMETER);
    *context = nullptr;

    InternalFgScContext* internal = allocateContext(alloc, this);
    VERIFY(internal, FFX_API_RETURN_ERROR_MEMORY);

    FfxErrorCode error = FFX_ERROR_INVALID_ARGUMENT;
    IDXGISwapChain4* swapchain = nullptr;

    if (auto* desc = ffx::DynamicCast<ffxCreateContextDescFrameGenerationSwapChainWrapDX12>(header)) {
        if (!desc->swapchain || !*desc->swapchain || !desc->gameQueue) {
            freeContext(alloc, internal);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        swapchain = *desc->swapchain;
        error = apusr::framegen::dx12::wrapSwapchain(desc->gameQueue, swapchain);
        if (error == FFX_OK)
            *desc->swapchain = swapchain;
    } else if (auto* desc = ffx::DynamicCast<ffxCreateContextDescFrameGenerationSwapChainNewDX12>(header)) {
        if (!desc->swapchain || !desc->desc || !desc->gameQueue || !desc->dxgiFactory) {
            freeContext(alloc, internal);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        error = apusr::framegen::dx12::createSwapchain(*desc->desc, desc->gameQueue, desc->dxgiFactory, swapchain);
        if (error == FFX_OK)
            *desc->swapchain = swapchain;
    } else if (auto* desc = ffx::DynamicCast<ffxCreateContextDescFrameGenerationSwapChainForHwndDX12>(header)) {
        if (!desc->swapchain || !desc->desc || !desc->gameQueue || !desc->dxgiFactory || !desc->hwnd) {
            freeContext(alloc, internal);
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        error = apusr::framegen::dx12::createSwapchainForHwnd(
            desc->hwnd, *desc->desc, desc->fullscreenDesc, desc->gameQueue, desc->dxgiFactory, swapchain);
        if (error == FFX_OK)
            *desc->swapchain = swapchain;
    } else {
        freeContext(alloc, internal);
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }

    if (error != FFX_OK) {
        freeContext(alloc, internal);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }

    internal->swapchain = swapchain;
    apusr::framegen::dx12::retainSwapchain(internal->swapchain);
    *context = internal;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ApusrFramegenSwapchainProviderDx12::destroyContext(ffxContext* context, const ApusrFramegenAllocator& alloc) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);

    auto* internal = static_cast<InternalFgScContext*>(*context);
    apusr::framegen::dx12::releaseSwapchain(internal->swapchain);
    freeContext(alloc, internal);
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ApusrFramegenSwapchainProviderDx12::configure(
    ffxContext* context, const ffxConfigureDescHeader* header) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(header, FFX_API_RETURN_ERROR_PARAMETER);

    auto* internal = static_cast<InternalFgScContext*>(*context);
    if (auto* desc = ffx::DynamicCast<ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12>(header))
        return runtimeResult(apusr::framegen::dx12::registerUiResource(internal->swapchain, desc->uiResource, desc->flags));

    if (auto* desc = ffx::DynamicCast<ffxConfigureDescFrameGenerationSwapChainKeyValueDX12>(header))
        return runtimeResult(apusr::framegen::dx12::configure(internal->swapchain, desc->key, desc->ptr));

    return FFX_API_RETURN_ERROR_PARAMETER;
}

ffxReturnCode_t ApusrFramegenSwapchainProviderDx12::query(ffxContext* context, ffxQueryDescHeader* header) const
{
    VERIFY(header, FFX_API_RETURN_ERROR_PARAMETER);

    if (auto* desc = ffx::DynamicCast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2>(header)) {
        VERIFY(desc->gpuMemoryUsageFrameGenerationSwapchain, FFX_API_RETURN_ERROR_PARAMETER);
        std::memset(desc->gpuMemoryUsageFrameGenerationSwapchain, 0, sizeof(FfxApiEffectMemoryUsage));
        return runtimeResult(apusr::framegen::dx12::gpuMemoryUsage(
            static_cast<FfxDevice>(desc->device),
            desc->displaySize,
            static_cast<FfxApiSurfaceFormat>(desc->backBufferFormat),
            desc->backBufferCount,
            desc->uiResourceSize,
            static_cast<FfxApiSurfaceFormat>(desc->uiResourceFormat),
            desc->flags,
            *desc->gpuMemoryUsageFrameGenerationSwapchain));
    }

    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);
    auto* internal = static_cast<InternalFgScContext*>(*context);

    if (auto* desc = ffx::DynamicCast<ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12>(header)) {
        VERIFY(desc->pOutCommandList, FFX_API_RETURN_ERROR_PARAMETER);
        return runtimeResult(apusr::framegen::dx12::interpolationCommandList(internal->swapchain, *desc->pOutCommandList));
    }

    if (auto* desc = ffx::DynamicCast<ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12>(header)) {
        VERIFY(desc->pOutTexture, FFX_API_RETURN_ERROR_PARAMETER);
        *desc->pOutTexture = apusr::framegen::dx12::interpolationTexture(internal->swapchain);
        return FFX_API_RETURN_OK;
    }

    if (auto* desc = ffx::DynamicCast<ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12>(header)) {
        VERIFY(desc->gpuMemoryUsageFrameGenerationSwapchain, FFX_API_RETURN_ERROR_PARAMETER);
        std::memset(desc->gpuMemoryUsageFrameGenerationSwapchain, 0, sizeof(FfxApiEffectMemoryUsage));
        return runtimeResult(apusr::framegen::dx12::gpuMemoryUsage(
            internal->swapchain, *desc->gpuMemoryUsageFrameGenerationSwapchain));
    }

    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

ffxReturnCode_t ApusrFramegenSwapchainProviderDx12::dispatch(
    ffxContext* context, const ffxDispatchDescHeader* header) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(header, FFX_API_RETURN_ERROR_PARAMETER);

    if (!ffx::DynamicCast<ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12>(header))
        return FFX_API_RETURN_ERROR;

    auto* internal = static_cast<InternalFgScContext*>(*context);
    return runtimeResult(apusr::framegen::dx12::waitForPresents(internal->swapchain));
}

ApusrFramegenSwapchainProviderDx12 ApusrFramegenSwapchainProviderDx12::Instance;
