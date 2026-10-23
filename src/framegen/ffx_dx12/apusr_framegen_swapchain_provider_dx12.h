// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_provider.h"

class ApusrFramegenSwapchainProviderDx12 final : public ApusrFramegenProvider {
public:
    bool canProvide(ffxStructType_t type) const noexcept override;
    std::uint64_t id() const noexcept override;
    const char* versionName() const noexcept override;
    ffxReturnCode_t createContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ApusrFramegenAllocator& allocator) const override;
    ffxReturnCode_t destroyContext(ffxContext* context, const ApusrFramegenAllocator& allocator) const override;
    ffxReturnCode_t configure(ffxContext* context, const ffxConfigureDescHeader* desc) const override;
    ffxReturnCode_t query(ffxContext* context, ffxQueryDescHeader* desc) const override;
    ffxReturnCode_t dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const override;

    static ApusrFramegenSwapchainProviderDx12 Instance;
};
