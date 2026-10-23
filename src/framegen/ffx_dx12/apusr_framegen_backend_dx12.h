// APUSR-maintained DX12 FrameGen backend derived from the FidelityFX SDK.
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

#include <d3d12.h>
#include <dxgi1_6.h>

#include "apusr_framegen_allocator.h"
#include "apusr_framegen_backend_interface.h"
#include "apusr_framegen_resource_types.h"

ApusrFramegenResult apusrFramegenResourceSizeDX12(
    ID3D12Device* device,
    const ApusrFramegenCreateResourceDescription* description,
    uint64_t* sizeInBytes);

ApusrFramegenResult apusrFramegenCreateDx12Backend(
    ID3D12Device* device,
    ApusrFramegenBackendInterface* backendInterface,
    size_t contexts,
    const ApusrFramegenAllocator& allocator) noexcept;

ApusrFramegenCommandList apusrFramegenCommandListDX12(ID3D12CommandList* commandList);
ApusrFramegenExternalResource apusrFramegenExternalResourceDX12(
    const ID3D12Resource* resource,
    ApusrFramegenResourceDescription description,
    ApusrFramegenResourceState state = APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ);
ApusrFramegenResourceDescription apusrFramegenNativeResourceDescriptionDX12(
    const ID3D12Resource* resource,
    ApusrFramegenResourceUsage additionalUsages = APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY);
DXGI_FORMAT apusrFramegenDxgiFormat(ApusrFramegenSurfaceFormat surfaceFormat);
D3D12_RESOURCE_STATES apusrFramegenDx12State(ApusrFramegenResourceState state);
