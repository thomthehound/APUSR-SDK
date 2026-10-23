// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_present_dx12.h"

#include "apusr_framegen_ffx_dx12.h"

namespace apusr::framegen::dx12 {
namespace {
FfxSwapchain asFfxSwapchain(IDXGISwapChain4* swapchain) noexcept
{
    return apusrFramegenSwapchainDX12(swapchain);
}
} // namespace

FfxErrorCode wrapSwapchain(ID3D12CommandQueue* gameQueue, IDXGISwapChain4*& swapchain) noexcept
{
    if (!gameQueue || !swapchain)
        return FFX_ERROR_INVALID_ARGUMENT;

    FfxSwapchain wrapped = asFfxSwapchain(swapchain);
    const FfxErrorCode error = ffxReplaceSwapchainForFrameinterpolationDX12(gameQueue, wrapped);
    if (error == FFX_OK)
        swapchain = apusrFramegenSwapchainPtrDX12(wrapped);
    return error;
}

FfxErrorCode createSwapchain(const DXGI_SWAP_CHAIN_DESC& description,
                             ID3D12CommandQueue* gameQueue,
                             IDXGIFactory* factory,
                             IDXGISwapChain4*& swapchain) noexcept
{
    if (!gameQueue || !factory)
        return FFX_ERROR_INVALID_ARGUMENT;

    FfxSwapchain created = nullptr;
    const FfxErrorCode error = ffxCreateFrameinterpolationSwapchainDX12(&description, gameQueue, factory, created);
    if (error == FFX_OK)
        swapchain = apusrFramegenSwapchainPtrDX12(created);
    return error;
}

FfxErrorCode createSwapchainForHwnd(HWND hwnd,
                                    const DXGI_SWAP_CHAIN_DESC1& description,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDescription,
                                    ID3D12CommandQueue* gameQueue,
                                    IDXGIFactory* factory,
                                    IDXGISwapChain4*& swapchain) noexcept
{
    if (!hwnd || !gameQueue || !factory)
        return FFX_ERROR_INVALID_ARGUMENT;

    FfxSwapchain created = nullptr;
    const FfxErrorCode error = ffxCreateFrameinterpolationSwapchainForHwndDX12(
        hwnd, &description, fullscreenDescription, gameQueue, factory, created);
    if (error == FFX_OK)
        swapchain = apusrFramegenSwapchainPtrDX12(created);
    return error;
}

void retainSwapchain(IDXGISwapChain4* swapchain) noexcept
{
    if (swapchain)
        swapchain->AddRef();
}

void releaseSwapchain(IDXGISwapChain4*& swapchain) noexcept
{
    if (!swapchain)
        return;
    swapchain->Release();
    swapchain = nullptr;
}

FfxErrorCode registerUiResource(IDXGISwapChain4* swapchain, FfxApiResource resource, uint32_t flags) noexcept
{
    if (!swapchain)
        return FFX_ERROR_INVALID_ARGUMENT;
    return ffxRegisterFrameinterpolationUiResourceDX12(asFfxSwapchain(swapchain), resource, flags);
}

FfxErrorCode configure(IDXGISwapChain4* swapchain, uint64_t key, void* value) noexcept
{
    if (!swapchain)
        return FFX_ERROR_INVALID_ARGUMENT;
    return ffxConfigureFrameInterpolationSwapchainDX12(
        asFfxSwapchain(swapchain), static_cast<FfxFrameInterpolationSwapchainConfigureKey>(key), value);
}

FfxErrorCode interpolationCommandList(IDXGISwapChain4* swapchain, void*& commandList) noexcept
{
    if (!swapchain)
        return FFX_ERROR_INVALID_ARGUMENT;

    FfxCommandList result = nullptr;
    const FfxErrorCode error = ffxGetFrameinterpolationCommandlistDX12(asFfxSwapchain(swapchain), result);
    if (error == FFX_OK)
        commandList = result;
    return error;
}

FfxApiResource interpolationTexture(IDXGISwapChain4* swapchain) noexcept
{
    if (!swapchain)
        return {};
    return ffxGetFrameinterpolationTextureDX12(asFfxSwapchain(swapchain));
}

FfxErrorCode gpuMemoryUsage(IDXGISwapChain4* swapchain, FfxApiEffectMemoryUsage& usage) noexcept
{
    if (!swapchain)
        return FFX_ERROR_INVALID_ARGUMENT;
    return ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12(asFfxSwapchain(swapchain), &usage);
}

FfxErrorCode gpuMemoryUsage(FfxDevice device,
                            FfxApiDimensions2D displaySize,
                            FfxApiSurfaceFormat backBufferFormat,
                            uint32_t backBufferCount,
                            FfxApiDimensions2D uiResourceSize,
                            FfxApiSurfaceFormat uiResourceFormat,
                            uint32_t flags,
                            FfxApiEffectMemoryUsage& usage) noexcept
{
    return ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12V2(
        device,
        &displaySize,
        backBufferFormat,
        backBufferCount,
        &uiResourceSize,
        uiResourceFormat,
        flags,
        &usage);
}

FfxErrorCode waitForPresents(IDXGISwapChain4* swapchain) noexcept
{
    if (!swapchain)
        return FFX_ERROR_INVALID_ARGUMENT;
    return ffxWaitForPresents(asFfxSwapchain(swapchain));
}

} // namespace apusr::framegen::dx12
