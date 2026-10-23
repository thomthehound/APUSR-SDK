// This file includes presentation logic derived from the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include "ffx_api_types.h"

class FrameInterpolationSwapChainDX12;

namespace apusr::framegen::dx12 {

// Owns the active D3D12 presentation policy while the FidelityFX-compatible
// IDXGISwapChain façade remains responsible for COM/DXGI compatibility.
struct PresentPath final {
    static HRESULT present(FrameInterpolationSwapChainDX12& swapchain, UINT syncInterval, UINT flags);

private:
    struct ProfileContext;

    static HRESULT presentWithUiComposition(
        FrameInterpolationSwapChainDX12& swapchain, UINT syncInterval, UINT flags, const ProfileContext& profile);
    static void dispatchInterpolationCommands(FrameInterpolationSwapChainDX12& swapchain,
                                              FfxApiResource* interpolatedFrame,
                                              FfxApiResource* realFrame,
                                              const ProfileContext& profile);
    static void presentInterpolated(
        FrameInterpolationSwapChainDX12& swapchain, UINT syncInterval, const ProfileContext& profile);
    static bool verifyUiDuplicateResource(FrameInterpolationSwapChainDX12& swapchain);
    static void copyUiResource(FrameInterpolationSwapChainDX12& swapchain);
};

} // namespace apusr::framegen::dx12
