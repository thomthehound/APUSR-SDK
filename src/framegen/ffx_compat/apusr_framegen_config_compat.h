// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "abi/ffx_framegeneration.h"
#include "ffx_error.h"
#include "ffx_internal_types.h"
#include "../apusr_framegen_resource_types.h"
#include "../apusr_framegen_core_types.h"

#include <cstdint>

using ApusrFramegenPresentCallbackFunc = FfxErrorCode (*)(const FfxPresentCallbackDescription* params, void* userContext);
using ApusrFramegenDispatchCallbackFunc = FfxErrorCode (*)(const FfxFrameGenerationDispatchDescription* params, void* userContext);

enum ApusrFramegenUiCompositionFlagBits : std::uint32_t {
    APUSR_FRAMEGEN_UI_COMPOSITION_USE_PREMUL_ALPHA = 1u << 0,
    APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER = 1u << 1,
};

struct ApusrFramegenConfig {
    FfxSwapchain swapChain{};
    ApusrFramegenPresentCallbackFunc presentCallback = nullptr;
    void* presentCallbackContext = nullptr;
    ApusrFramegenDispatchCallbackFunc frameGenerationCallback = nullptr;
    void* frameGenerationCallbackContext = nullptr;
    bool frameGenerationEnabled = false;
    bool allowAsyncWorkloads = false;
    bool allowAsyncPresent = false;
    ApusrFramegenExternalResource HUDLessColor{};
    std::uint32_t flags = 0;
    bool onlyPresentInterpolated = false;
    FfxApiRect2D interpolationRect{};
    std::uint64_t frameID = 0;
    bool drawDebugPacingLines = false;
};

ApusrFramegenResult apusrFramegenApplySwapchainConfig(const ApusrFramegenConfig* config) noexcept;
