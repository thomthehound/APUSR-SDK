// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_backend.h"

#include "apusr_framegen_backend_dx12.h"

namespace apusr::framegen {

ApusrFramegenResult resourceAllocationSize(ApusrFramegenDevice device, const ApusrFramegenCreateResourceDescription& description, uint64_t& sizeInBytes) noexcept
{
    return apusrFramegenResourceSizeDX12(reinterpret_cast<ID3D12Device*>(device), &description, &sizeInBytes);
}

} // namespace apusr::framegen
