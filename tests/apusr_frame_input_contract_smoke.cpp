// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_frame_desc.h>

#include <cassert>
#include <cstdint>

namespace {

apusr::NativeHandle handle(std::uintptr_t value)
{
    return apusr::NativeHandle{reinterpret_cast<void*>(value)};
}

apusr::FrameInputSurfaceDesc surface(
    apusr::FrameInputSurfaceRole role,
    std::uintptr_t resource,
    apusr::SurfaceFormat format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t state)
{
    return apusr::FrameInputSurfaceDesc{
        .role = role,
        .resource = handle(resource),
        .format = format,
        .size = apusr::RenderSize{.width = width, .height = height},
        .nativeStateOrLayout = state,
    };
}

} // namespace

int main()
{
    apusr::FrameInputDesc frameInput;
    frameInput.frameIndex = 7;
    frameInput.command.api = apusr::FrameInputApi::Dx12;
    frameInput.command.device = handle(0x0100);
    frameInput.command.commandListOrBuffer = handle(0x0200);
    frameInput.constants.renderSize = apusr::RenderSize{.width = 1920, .height = 1080};
    frameInput.constants.displaySize = apusr::DisplaySize{.width = 3840, .height = 2160};
    frameInput.constants.motionVectorSize = apusr::RenderSize{.width = 3840, .height = 2160};
    frameInput.constants.jitter = apusr::Jitter{.x = 0.25f, .y = -0.25f};
    frameInput.constants.previousJitter = apusr::Jitter{.x = -0.25f, .y = 0.25f};
    frameInput.options.qualityMode = apusr::UpscalerQualityMode::Balanced;
    frameInput.options.motionVectorScale = apusr::MotionVectorScale{.x = 0.5f, .y = 0.5f};
    frameInput.options.features.depthInverted = true;
    frameInput.options.preExposure = 1.0f;

    assert(apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::Color, 0x1000, apusr::SurfaceFormat::Rgba16Float, 1920, 1080, 0x40)).succeeded());
    assert(apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::Depth, 0x2000, apusr::SurfaceFormat::R32Float, 1920, 1080, 0x40)).succeeded());
    assert(apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::MotionVectors, 0x3000, apusr::SurfaceFormat::Rg16Float, 3840, 2160, 0x40)).succeeded());
    assert(apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::Output, 0x4000, apusr::SurfaceFormat::Rgba16Float, 3840, 2160, 0x8)).succeeded());
    assert(apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::Exposure, 0x5000, apusr::SurfaceFormat::R32Float, 1, 1, 0x40)).succeeded());
    assert(apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::ReactiveMask, 0x6000, apusr::SurfaceFormat::R8Unorm, 1920, 1080, 0x40)).succeeded());

    apusr::FrameDesc frame;
    apusr::Status status = apusr::makeRuntimeFrameDescFromFrameInput(frameInput, frame);
    assert(status.succeeded());
    assert(frame.frameIndex == 7);
    assert(frame.borrowedCommandList.value == handle(0x0200).value);
    assert(frame.surfaces.color.resource.value == handle(0x1000).value);
    assert(frame.surfaces.depth.resource.value == handle(0x2000).value);
    assert(frame.surfaces.motionVectors.resource.value == handle(0x3000).value);
    assert(frame.surfaces.output.resource.value == handle(0x4000).value);
    assert(frame.options.hasDepth);
    assert(frame.options.hasExposure);
    assert(frame.options.hasReactiveMask);
    assert(frame.options.qualityMode == apusr::UpscalerQualityMode::Balanced);
    assert(frame.options.features.depthInverted);

    assert(!apusr::appendFrameInputSurface(frameInput, surface(apusr::FrameInputSurfaceRole::Color, 0x7000, apusr::SurfaceFormat::Rgba16Float, 1920, 1080, 0x40)).succeeded());

    apusr::FrameInputDesc missingOutput = frameInput;
    missingOutput.surfaceCount = 3;
    status = apusr::validateFrameInputForFsr4(missingOutput);
    assert(!status.succeeded());

    return 0;
}
