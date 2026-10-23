// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "ffx_compat/apusr_framegen_backend_compat.h"

#include "apusr_framegen_backend_dx12.h"
#include "ffx_api_dx12.h"

ApusrFramegenResult apusrFramegenCreateBackendFromFfx(
    const ffxApiHeader* chain,
    ApusrFramegenBackendInterface* backend,
    std::size_t maxContexts,
    const ApusrFramegenAllocator& allocator) noexcept
{
    const ffxCreateBackendDX12Desc* dx12 = nullptr;
    for (const ffxApiHeader* it = chain; it; it = it->pNext) {
        if (it->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12)
            continue;
        if (dx12)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        dx12 = reinterpret_cast<const ffxCreateBackendDX12Desc*>(it);
    }

    if (!dx12 || !dx12->device)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    return apusrFramegenCreateDx12Backend(
        reinterpret_cast<ID3D12Device*>(dx12->device),
        backend,
        maxContexts,
        allocator);
}
