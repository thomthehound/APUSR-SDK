// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include "ffx_api.h"
#include "ffx_api_framegeneration_dx12.h"
#include "ffx_api_types.h"
#include "ffx_error.h"
#include "ffx_internal_types.h"
#include "ffx_compat/apusr_framegen_config_compat.h"

FfxCommandQueue apusrFramegenCommandQueueDX12(ID3D12CommandQueue* commandQueue);
FfxSwapchain apusrFramegenSwapchainDX12(IDXGISwapChain4* swapchain);
IDXGISwapChain4* apusrFramegenSwapchainPtrDX12(FfxSwapchain swapchain);

FfxErrorCode ffxReplaceSwapchainForFrameinterpolationDX12(FfxCommandQueue gameQueue, FfxSwapchain& gameSwapChain);
FfxErrorCode ffxCreateFrameinterpolationSwapchainDX12(const DXGI_SWAP_CHAIN_DESC* desc, ID3D12CommandQueue* queue, IDXGIFactory* dxgiFactory, FfxSwapchain& outGameSwapChain);
FfxErrorCode ffxCreateFrameinterpolationSwapchainForHwndDX12(HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc1, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, ID3D12CommandQueue* queue, IDXGIFactory* dxgiFactory, FfxSwapchain& outGameSwapChain);
FfxErrorCode ffxRegisterFrameinterpolationUiResourceDX12(FfxSwapchain gameSwapChain, FfxApiResource uiResource, uint32_t flags);
FfxErrorCode ffxGetFrameinterpolationCommandlistDX12(FfxSwapchain gameSwapChain, FfxCommandList& gameCommandList);
FfxApiResource ffxGetFrameinterpolationTextureDX12(FfxSwapchain gameSwapChain);

typedef enum FfxFrameInterpolationSwapchainConfigureKey {
    FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK = 0,
    FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING = 2,
} FfxFrameInterpolationSwapchainConfigureKey;

FfxErrorCode ffxConfigureFrameInterpolationSwapchainDX12(FfxSwapchain gameSwapChain, FfxFrameInterpolationSwapchainConfigureKey key, void* valuePtr);
FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12(FfxSwapchain gameSwapChain, FfxApiEffectMemoryUsage* outVramUsage);
FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12V2(
    FfxDevice device,
    FfxApiDimensions2D* displaySize,
    FfxApiSurfaceFormat backbufferFormat,
    uint32_t backBufferCount,
    FfxApiDimensions2D* uiResourceSize,
    FfxApiSurfaceFormat uiResourceFormat,
    uint32_t flags,
    FfxApiEffectMemoryUsage* outVramUsage);
FfxErrorCode ffxWaitForPresents(FfxSwapchain gameSwapChain);
