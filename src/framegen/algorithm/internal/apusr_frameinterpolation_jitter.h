// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
#pragma once

#include <cstdint>

namespace apusr::framegen::algorithm {

struct MotionVectorJitterCancellation {
    float x = 0.0f;
    float y = 0.0f;
};

inline constexpr MotionVectorJitterCancellation computeMotionVectorJitterCancellation(
    bool enabled,
    bool reset,
    bool havePreviousFrame,
    std::uint64_t previousFrameId,
    std::uint64_t frameId,
    float previousJitterX,
    float previousJitterY,
    float jitterX,
    float jitterY,
    std::int32_t motionVectorWidth,
    std::int32_t motionVectorHeight) noexcept
{
    const bool contiguous = havePreviousFrame && frameId > previousFrameId &&
        frameId - previousFrameId == 1u;
    if (!enabled || reset || !contiguous || motionVectorWidth <= 0 || motionVectorHeight <= 0) {
        return {};
    }
    return {
        (previousJitterX - jitterX) / static_cast<float>(motionVectorWidth),
        (previousJitterY - jitterY) / static_cast<float>(motionVectorHeight),
    };
}

} // namespace apusr::framegen::algorithm
