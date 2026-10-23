// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_allocator.h"
#include "apusr_framegen_backend_interface.h"
#include "ffx_api.h"

#include <cstddef>

// Translate the backend descriptor in an FFX pNext chain once at the
// compatibility edge. The selected backend implementation remains native.
ApusrFramegenResult apusrFramegenCreateBackendFromFfx(
    const ffxApiHeader* chain,
    ApusrFramegenBackendInterface* backend,
    std::size_t maxContexts,
    const ApusrFramegenAllocator& allocator) noexcept;
