// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_ffx_dx12.h"

FfxCommandQueue apusrFramegenCommandQueueDX12(ID3D12CommandQueue* commandQueue)
{
    return reinterpret_cast<FfxCommandQueue>(commandQueue);
}

FfxSwapchain apusrFramegenSwapchainDX12(IDXGISwapChain4* swapchain)
{
    return reinterpret_cast<FfxSwapchain>(swapchain);
}

IDXGISwapChain4* apusrFramegenSwapchainPtrDX12(FfxSwapchain swapchain)
{
    return reinterpret_cast<IDXGISwapChain4*>(swapchain);
}
