// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
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
#include "apusr_framegen_provider.h"

class ffxProvider_Fsr3FrameGeneration final : public ApusrFramegenProvider
{
public:
    ffxProvider_Fsr3FrameGeneration() = default;
    virtual ~ffxProvider_Fsr3FrameGeneration() = default;

    bool canProvide(ffxStructType_t type) const noexcept override;

    std::uint64_t id() const noexcept override;

    const char* versionName() const noexcept override;

    ffxReturnCode_t createContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ApusrFramegenAllocator& allocator) const override;

    ffxReturnCode_t destroyContext(ffxContext* context, const ApusrFramegenAllocator& allocator) const override;

    ffxReturnCode_t configure(ffxContext* context, const ffxConfigureDescHeader* desc) const override;

    ffxReturnCode_t query(ffxContext* context, ffxQueryDescHeader* desc) const override;

    ffxReturnCode_t dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const override;

    static ffxProvider_Fsr3FrameGeneration Instance;
};
