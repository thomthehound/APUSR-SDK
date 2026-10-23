// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_upscaler_common.h>

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

bool almostEqual(const float lhs, const float rhs, const float epsilon = 0.000001f)
{
    return std::fabs(lhs - rhs) <= epsilon;
}

bool requireJitter(const apusr::Jitter value, const float expectedX, const float expectedY, const char* name)
{
    if (!almostEqual(value.x, expectedX) || !almostEqual(value.y, expectedY)) {
        std::cerr << name << " mismatch: got (" << value.x << ", " << value.y << ") expected ("
                  << expectedX << ", " << expectedY << ")\n";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const apusr::RenderSize renderSize{1920, 1080};
    const apusr::DisplaySize displaySize{3840, 2160};

    const std::int32_t phaseCount = apusr::upscaleJitterPhaseCount(renderSize, displaySize);
    if (phaseCount != 32) {
        std::cerr << "unexpected jitter phase count: " << phaseCount << '\n';
        return 1;
    }

    if (!requireJitter(apusr::upscaleJitterOffset(0, phaseCount), 0.0f, -1.0f / 6.0f, "jitter[0]")) {
        return 1;
    }
    if (!requireJitter(apusr::upscaleJitterOffset(1, phaseCount), -0.25f, 1.0f / 6.0f, "jitter[1]")) {
        return 1;
    }

    const apusr::Jitter projectionScale = apusr::projectionJitterScale(apusr::Jitter{.x = 0.25f, .y = -0.5f}, renderSize);
    if (!requireJitter(projectionScale, 2.0f * 0.25f / 1920.0f, 2.0f * 0.5f / 1080.0f, "projection jitter")) {
        return 1;
    }

    const apusr::Jitter cancellation = apusr::motionVectorJitterCancellation(
        apusr::Jitter{.x = 0.25f, .y = -0.25f},
        apusr::Jitter{.x = -0.25f, .y = 0.25f},
        renderSize);
    if (!requireJitter(cancellation, 0.5f / 1920.0f, -0.5f / 1080.0f, "motion-vector jitter cancellation")) {
        return 1;
    }

    const apusr::DispatchGrid grid = apusr::dispatchGrid(apusr::RenderSize{1921, 1080}, 8, 8);
    if (grid.groupsX != 241 || grid.groupsY != 135 || grid.groupsZ != 1) {
        std::cerr << "unexpected dispatch grid: " << grid.groupsX << 'x' << grid.groupsY << 'x' << grid.groupsZ << '\n';
        return 1;
    }

    const apusr::UpscalerFrameConstants constants = apusr::makeUpscalerFrameConstants(
        renderSize,
        displaySize,
        apusr::Jitter{.x = 0.0f, .y = -1.0f / 6.0f},
        apusr::Jitter{.x = -0.25f, .y = 1.0f / 6.0f},
        apusr::MotionVectorResolution::RenderResolution);
    if (constants.jitterPhaseCount != 32 || constants.motionVectorSize.width != 1920 || constants.motionVectorSize.height != 1080) {
        std::cerr << "derived frame constants are not using the expected render-resolution motion-vector contract\n";
        return 1;
    }

    std::cout << "APUSR upscaler common smoke: native jitter, dispatch, and frame constants validated\n";
    return 0;
}
