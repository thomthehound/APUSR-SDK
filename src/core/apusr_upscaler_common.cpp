// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_upscaler_common.h>

#include <cmath>

namespace apusr {

namespace {

[[nodiscard]] std::uint32_t ceilDiv(const std::uint32_t numerator, const std::uint32_t denominator)
{
    return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
}

} // namespace

float halton(const std::int32_t index, const std::int32_t base)
{
    if (index <= 0 || base <= 1) {
        return 0.0f;
    }

    float fraction = 1.0f;
    float result = 0.0f;
    std::int32_t currentIndex = index;
    while (currentIndex > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(currentIndex % base);
        currentIndex /= base;
    }
    return result;
}

std::int32_t upscaleJitterPhaseCount(const RenderSize renderSize, const DisplaySize displaySize)
{
    if (renderSize.width == 0 || displaySize.width == 0) {
        return 0;
    }

    constexpr float kBasePhaseCount = 8.0f;
    const float widthScale = static_cast<float>(displaySize.width) / static_cast<float>(renderSize.width);
    return static_cast<std::int32_t>(kBasePhaseCount * widthScale * widthScale);
}

Jitter upscaleJitterOffset(const std::int32_t index, const std::int32_t phaseCount)
{
    if (phaseCount <= 0) {
        return Jitter{};
    }

    const std::int32_t sequenceIndex = (index % phaseCount) + 1;
    return Jitter{
        .x = halton(sequenceIndex, 2) - 0.5f,
        .y = halton(sequenceIndex, 3) - 0.5f,
    };
}

Jitter projectionJitterScale(const Jitter jitter, const RenderSize renderSize)
{
    if (renderSize.width == 0 || renderSize.height == 0) {
        return Jitter{};
    }

    return Jitter{
        .x = 2.0f * jitter.x / static_cast<float>(renderSize.width),
        .y = -2.0f * jitter.y / static_cast<float>(renderSize.height),
    };
}

RenderSize motionVectorSizeFor(
    const RenderSize renderSize,
    const DisplaySize displaySize,
    const MotionVectorResolution resolution)
{
    switch (resolution) {
    case MotionVectorResolution::RenderResolution:
        return renderSize;
    case MotionVectorResolution::DisplayResolution:
        return RenderSize{.width = displaySize.width, .height = displaySize.height};
    }
    return renderSize;
}

Jitter motionVectorJitterCancellation(
    const Jitter previousJitter,
    const Jitter currentJitter,
    const RenderSize motionVectorSize)
{
    if (motionVectorSize.width == 0 || motionVectorSize.height == 0) {
        return Jitter{};
    }

    return Jitter{
        .x = (previousJitter.x - currentJitter.x) / static_cast<float>(motionVectorSize.width),
        .y = (previousJitter.y - currentJitter.y) / static_cast<float>(motionVectorSize.height),
    };
}

DispatchGrid dispatchGrid(
    const RenderSize extent,
    const std::uint32_t tileWidth,
    const std::uint32_t tileHeight,
    const std::uint32_t slices)
{
    return DispatchGrid{
        .groupsX = ceilDiv(extent.width, tileWidth),
        .groupsY = ceilDiv(extent.height, tileHeight),
        .groupsZ = slices,
    };
}

UpscalerFrameConstants makeUpscalerFrameConstants(
    const RenderSize renderSize,
    const DisplaySize displaySize,
    const Jitter currentJitter,
    const Jitter previousJitter,
    const MotionVectorResolution motionVectorResolution)
{
    const RenderSize motionVectorSize = motionVectorSizeFor(renderSize, displaySize, motionVectorResolution);
    const float displayWidth = static_cast<float>(displaySize.width == 0 ? 1 : displaySize.width);
    const float displayHeight = static_cast<float>(displaySize.height == 0 ? 1 : displaySize.height);
    const float renderWidth = static_cast<float>(renderSize.width == 0 ? 1 : renderSize.width);
    const float renderHeight = static_cast<float>(renderSize.height == 0 ? 1 : renderSize.height);

    return UpscalerFrameConstants{
        .renderSize = renderSize,
        .displaySize = displaySize,
        .motionVectorSize = motionVectorSize,
        .jitter = currentJitter,
        .previousJitter = previousJitter,
        .motionVectorJitterCancellation = motionVectorJitterCancellation(previousJitter, currentJitter, motionVectorSize),
        .renderToDisplayScaleX = displayWidth / renderWidth,
        .renderToDisplayScaleY = displayHeight / renderHeight,
        .displayToRenderScaleX = renderWidth / displayWidth,
        .displayToRenderScaleY = renderHeight / displayHeight,
        .jitterPhaseCount = upscaleJitterPhaseCount(renderSize, displaySize),
    };
}

const char* motionVectorResolutionName(const MotionVectorResolution resolution)
{
    switch (resolution) {
    case MotionVectorResolution::RenderResolution:
        return "render-resolution";
    case MotionVectorResolution::DisplayResolution:
        return "display-resolution";
    }
    return "unknown";
}

} // namespace apusr
