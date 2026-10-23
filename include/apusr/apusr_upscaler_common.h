// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>

#include <cstdint>

namespace apusr {

enum class UpscalerQualityMode : std::uint32_t {
    Auto = 0,
    NativeAA,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
    DynamicResolution,
};

enum class MotionVectorResolution : std::uint32_t {
    RenderResolution,
    DisplayResolution,
};

struct DispatchGrid {
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
    std::uint32_t groupsZ = 1;
};

struct MotionVectorScale {
    float x = 1.0f;
    float y = 1.0f;
};

struct UpscalerFeatureFlags {
    bool highDynamicRange = false;
    bool displayResolutionMotionVectors = false;
    bool motionVectorJitterCancellation = false;
    bool depthInverted = false;
    bool depthInfinite = false;
    bool autoExposure = false;
    bool dynamicResolution = false;
    bool debugChecking = false;
    bool nonLinearColorSpace = false;
    bool colorSpaceSrgb = false;
    bool colorSpacePq = false;
    bool debugVisualization = false;
};

struct UpscalerFrameOptions {
    MotionVectorScale motionVectorScale;
    UpscalerQualityMode qualityMode = UpscalerQualityMode::Auto;
    UpscalerFeatureFlags features;
    bool resetHistory = false;
    bool hasDepth = false;
    bool hasExposure = false;
    bool hasReactiveMask = false;
    bool hasTransparencyMask = false;
    bool sharpeningRequested = false;
    float sharpness = 0.0f;
    float frameTimeDeltaMs = 0.0f;
    float preExposure = 1.0f;
    float viewSpaceToMetersFactor = 1.0f;
};

struct UpscalerFrameConstants {
    RenderSize renderSize;
    DisplaySize displaySize;
    RenderSize motionVectorSize;
    Jitter jitter;
    Jitter previousJitter;
    Jitter motionVectorJitterCancellation;
    float renderToDisplayScaleX = 1.0f;
    float renderToDisplayScaleY = 1.0f;
    float displayToRenderScaleX = 1.0f;
    float displayToRenderScaleY = 1.0f;
    std::int32_t jitterPhaseCount = 0;
};

[[nodiscard]] constexpr bool upscaleExtentMeetsRatio(
    const std::uint32_t renderExtent,
    const std::uint32_t displayExtent,
    const std::uint32_t ratioNumerator,
    const std::uint32_t ratioDenominator) noexcept
{
    if (renderExtent == 0 || displayExtent == 0 || ratioNumerator == 0 || ratioDenominator == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(displayExtent) * ratioDenominator >=
        static_cast<std::uint64_t>(renderExtent) * ratioNumerator;
}

// Integer render sizes can differ by one pixel while representing the same nominal quality ratio.
[[nodiscard]] constexpr bool upscaleExtentWithinOnePixelOfRatio(
    const std::uint32_t renderExtent,
    const std::uint32_t displayExtent,
    const std::uint32_t ratioNumerator,
    const std::uint32_t ratioDenominator) noexcept
{
    if (renderExtent == 0 || displayExtent == 0 || ratioNumerator == 0 || ratioDenominator == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(displayExtent) * ratioDenominator >=
        static_cast<std::uint64_t>(renderExtent - 1u) * ratioNumerator;
}

[[nodiscard]] float halton(std::int32_t index, std::int32_t base);
[[nodiscard]] std::int32_t upscaleJitterPhaseCount(RenderSize renderSize, DisplaySize displaySize);
[[nodiscard]] Jitter upscaleJitterOffset(std::int32_t index, std::int32_t phaseCount);
[[nodiscard]] Jitter projectionJitterScale(Jitter jitter, RenderSize renderSize);
[[nodiscard]] RenderSize motionVectorSizeFor(RenderSize renderSize, DisplaySize displaySize, MotionVectorResolution resolution);
[[nodiscard]] Jitter motionVectorJitterCancellation(Jitter previousJitter, Jitter currentJitter, RenderSize motionVectorSize);
[[nodiscard]] DispatchGrid dispatchGrid(RenderSize extent, std::uint32_t tileWidth, std::uint32_t tileHeight, std::uint32_t slices = 1);
[[nodiscard]] UpscalerFrameConstants makeUpscalerFrameConstants(
    RenderSize renderSize,
    DisplaySize displaySize,
    Jitter currentJitter,
    Jitter previousJitter,
    MotionVectorResolution motionVectorResolution);

[[nodiscard]] const char* motionVectorResolutionName(MotionVectorResolution resolution);

} // namespace apusr
