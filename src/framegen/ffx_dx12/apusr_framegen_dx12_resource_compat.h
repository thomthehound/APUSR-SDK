// APUSR-maintained FidelityFX resource adapter for the owned DX12 FrameGen path.
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

#include "apusr_framegen_backend_dx12.h"
#include "apusr_framegen_resource_compat.h"

inline FfxApiResourceDescription apusrFramegenResourceDescriptionDX12(
    const ID3D12Resource* resource,
    FfxApiResourceUsage additionalUsages = FFX_API_RESOURCE_USAGE_READ_ONLY)
{
    return apusrFramegenResourceDescriptionToFfx(
        apusrFramegenNativeResourceDescriptionDX12(resource, static_cast<ApusrFramegenResourceUsage>(additionalUsages)));
}

inline FfxApiResource apusrFramegenResourceDX12(
    const ID3D12Resource* resource,
    FfxApiResourceDescription description,
    const wchar_t* = nullptr,
    std::uint32_t state = FFX_API_RESOURCE_STATE_COMPUTE_READ)
{
    return apusrFramegenResourceToFfx({
        const_cast<ID3D12Resource*>(resource),
        apusrFramegenResourceDescriptionFromFfx(description),
        static_cast<ApusrFramegenResourceState>(state),
    });
}

inline DXGI_FORMAT apusrFramegenDxgiFormat(FfxApiSurfaceFormat format)
{
    return apusrFramegenDxgiFormat(apusrFramegenSurfaceFormatFromFfx(format));
}
