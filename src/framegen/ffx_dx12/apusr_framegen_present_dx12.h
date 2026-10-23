// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "ffx_api_framegeneration_dx12.h"
#include "ffx_api_types.h"
#include "ffx_error.h"

namespace apusr::framegen::dx12 {

FfxErrorCode wrapSwapchain(ID3D12CommandQueue* gameQueue, IDXGISwapChain4*& swapchain) noexcept;
FfxErrorCode createSwapchain(const DXGI_SWAP_CHAIN_DESC& description,
                             ID3D12CommandQueue* gameQueue,
                             IDXGIFactory* factory,
                             IDXGISwapChain4*& swapchain) noexcept;
FfxErrorCode createSwapchainForHwnd(HWND hwnd,
                                    const DXGI_SWAP_CHAIN_DESC1& description,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDescription,
                                    ID3D12CommandQueue* gameQueue,
                                    IDXGIFactory* factory,
                                    IDXGISwapChain4*& swapchain) noexcept;

void retainSwapchain(IDXGISwapChain4* swapchain) noexcept;
void releaseSwapchain(IDXGISwapChain4*& swapchain) noexcept;

FfxErrorCode registerUiResource(IDXGISwapChain4* swapchain, FfxApiResource resource, uint32_t flags) noexcept;
FfxErrorCode configure(IDXGISwapChain4* swapchain, uint64_t key, void* value) noexcept;
FfxErrorCode interpolationCommandList(IDXGISwapChain4* swapchain, void*& commandList) noexcept;
FfxApiResource interpolationTexture(IDXGISwapChain4* swapchain) noexcept;
FfxErrorCode gpuMemoryUsage(IDXGISwapChain4* swapchain, FfxApiEffectMemoryUsage& usage) noexcept;
FfxErrorCode gpuMemoryUsage(FfxDevice device,
                            FfxApiDimensions2D displaySize,
                            FfxApiSurfaceFormat backBufferFormat,
                            uint32_t backBufferCount,
                            FfxApiDimensions2D uiResourceSize,
                            FfxApiSurfaceFormat uiResourceFormat,
                            uint32_t flags,
                            FfxApiEffectMemoryUsage& usage) noexcept;
FfxErrorCode waitForPresents(IDXGISwapChain4* swapchain) noexcept;

} // namespace apusr::framegen::dx12
