// SPDX-License-Identifier: MIT
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_framegen.h>
#include <apusr/apusr_profiler.h>

#include "ffx_compat/apusr_framegen_config_compat.h"

namespace apusr::framegen::dx12 {

// DX12/FFX callback payload paired with the API-neutral PresentSchedule.
// Native queue/fence objects remain in the DX12 presenter state rather than
// leaking into the scheduling contract.
struct PacingData final {
    PresentSchedule schedule;

    // Profiling causality is backend-side metadata, not part of the host's
    // PresentSchedule contract. frameId remains the host-provided semantic
    // frame identity; occurrenceId distinguishes repeated source Presents.
    apusr::profile::StreamHandle profileFlowStream{};
    std::uint64_t profileFlowId = 0u;
    std::uint64_t profileOccurrenceId = 0u;

    ApusrFramegenPresentCallbackFunc presentCallback = nullptr;
    void* presentCallbackContext = nullptr;
    FfxApiResource uiSurface{};

    void invalidate() noexcept
    {
        *this = {};
    }
};

} // namespace apusr::framegen::dx12
