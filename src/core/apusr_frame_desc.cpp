// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_frame_desc.h>

#include <string>

namespace apusr {
namespace {

[[nodiscard]] bool surfaceProvided(const FrameInputSurfaceDesc* surface) noexcept
{
    return surface != nullptr && surface->resource.value != nullptr;
}

[[nodiscard]] Status validateProvidedSurface(const FrameInputSurfaceDesc& surface)
{
    if (surface.resource.value == nullptr) {
        return Status::invalidArgument(std::string("APUSR frame input missing ") + frameInputSurfaceRoleName(surface.role) + " resource");
    }
    if (surface.format == SurfaceFormat::Unknown) {
        return Status::invalidArgument(std::string("APUSR frame input ") + frameInputSurfaceRoleName(surface.role) + " has unknown format");
    }
    if (surface.size.width == 0 || surface.size.height == 0) {
        return Status::invalidArgument(std::string("APUSR frame input ") + frameInputSurfaceRoleName(surface.role) + " has zero extent");
    }
    return Status::ok();
}

[[nodiscard]] SurfaceDesc makeSurfaceDesc(const FrameInputSurfaceDesc& surface) noexcept
{
    std::uint64_t nativeState = surface.nativeStateOrLayout;
    if (nativeState == kFrameInputUnknownNativeState) {
        nativeState = kFrameInputUnknownNativeState;
    }
    return SurfaceDesc{
        .role = surfaceRoleForFrameInputRole(surface.role),
        .resource = surface.resource,
        .format = surface.format,
        .size = surface.size,
        .nativeState = nativeState,
    };
}

} // namespace

GraphicsApi graphicsApiForFrameInputApi(const FrameInputApi api) noexcept
{
    switch (api) {
    case FrameInputApi::Dx12:
        return GraphicsApi::Dx12;
    case FrameInputApi::Vulkan:
        return GraphicsApi::Vulkan;
    case FrameInputApi::Dx11On12:
        return GraphicsApi::Dx11On12;
    }
    return GraphicsApi::Dx12;
}

SurfaceRole surfaceRoleForFrameInputRole(const FrameInputSurfaceRole role) noexcept
{
    switch (role) {
    case FrameInputSurfaceRole::Color:
        return SurfaceRole::Color;
    case FrameInputSurfaceRole::Depth:
        return SurfaceRole::Depth;
    case FrameInputSurfaceRole::MotionVectors:
        return SurfaceRole::MotionVectors;
    case FrameInputSurfaceRole::Output:
        return SurfaceRole::Output;
    case FrameInputSurfaceRole::Exposure:
        return SurfaceRole::Exposure;
    case FrameInputSurfaceRole::ReactiveMask:
        return SurfaceRole::ReactiveMask;
    case FrameInputSurfaceRole::TransparencyCompositionMask:
        return SurfaceRole::TransparencyComposition;
    case FrameInputSurfaceRole::BiasCurrentColorMask:
    case FrameInputSurfaceRole::AnimatedTextureMask:
    case FrameInputSurfaceRole::ParticleMask:
        return SurfaceRole::ReactiveMask;
    }
    return SurfaceRole::Color;
}

const char* frameInputSurfaceRoleName(const FrameInputSurfaceRole role) noexcept
{
    switch (role) {
    case FrameInputSurfaceRole::Color:
        return "color";
    case FrameInputSurfaceRole::Depth:
        return "depth";
    case FrameInputSurfaceRole::MotionVectors:
        return "motion vectors";
    case FrameInputSurfaceRole::Output:
        return "output";
    case FrameInputSurfaceRole::Exposure:
        return "exposure";
    case FrameInputSurfaceRole::ReactiveMask:
        return "reactive mask";
    case FrameInputSurfaceRole::TransparencyCompositionMask:
        return "transparency/composition mask";
    case FrameInputSurfaceRole::BiasCurrentColorMask:
        return "bias-current-color mask";
    case FrameInputSurfaceRole::AnimatedTextureMask:
        return "animated-texture mask";
    case FrameInputSurfaceRole::ParticleMask:
        return "particle mask";
    }
    return "unknown";
}

bool frameInputSurfaceRequiredForFsr4(const FrameInputSurfaceRole role) noexcept
{
    switch (role) {
    case FrameInputSurfaceRole::Color:
    case FrameInputSurfaceRole::Depth:
    case FrameInputSurfaceRole::MotionVectors:
    case FrameInputSurfaceRole::Output:
        return true;
    case FrameInputSurfaceRole::Exposure:
    case FrameInputSurfaceRole::ReactiveMask:
    case FrameInputSurfaceRole::TransparencyCompositionMask:
    case FrameInputSurfaceRole::BiasCurrentColorMask:
    case FrameInputSurfaceRole::AnimatedTextureMask:
    case FrameInputSurfaceRole::ParticleMask:
        return false;
    }
    return false;
}

const FrameInputSurfaceDesc* findFrameInputSurface(const FrameInputDesc& desc, const FrameInputSurfaceRole role) noexcept
{
    for (std::uint32_t i = 0; i < desc.surfaceCount && i < desc.surfaces.size(); ++i) {
        if (desc.surfaces[i].role == role) {
            return &desc.surfaces[i];
        }
    }
    return nullptr;
}

Status appendFrameInputSurface(FrameInputDesc& desc, const FrameInputSurfaceDesc& surface)
{
    if (desc.surfaceCount >= desc.surfaces.size()) {
        return Status::invalidArgument("APUSR frame input contains too many surfaces");
    }
    if (findFrameInputSurface(desc, surface.role) != nullptr) {
        return Status::invalidArgument(std::string("APUSR frame input contains duplicate ") + frameInputSurfaceRoleName(surface.role) + " surface");
    }
    desc.surfaces[desc.surfaceCount++] = surface;
    return Status::ok();
}

Status validateFrameInputForFsr4(const FrameInputDesc& desc)
{
    if (desc.command.commandListOrBuffer.value == nullptr) {
        return Status::invalidArgument("APUSR frame input requires a command list/buffer");
    }
    if (desc.constants.renderSize.width == 0 || desc.constants.renderSize.height == 0) {
        return Status::invalidArgument("APUSR frame input requires a non-zero render size");
    }
    if (desc.constants.displaySize.width == 0 || desc.constants.displaySize.height == 0) {
        return Status::invalidArgument("APUSR frame input requires a non-zero display size");
    }

    for (const FrameInputSurfaceRole role : {
        FrameInputSurfaceRole::Color,
        FrameInputSurfaceRole::Depth,
        FrameInputSurfaceRole::MotionVectors,
        FrameInputSurfaceRole::Output}) {
        const FrameInputSurfaceDesc* surface = findFrameInputSurface(desc, role);
        if (surface == nullptr) {
            return Status::invalidArgument(std::string("APUSR frame input missing required ") + frameInputSurfaceRoleName(role) + " surface");
        }
        Status status = validateProvidedSurface(*surface);
        if (!status.succeeded()) {
            return status;
        }
    }

    for (const FrameInputSurfaceRole role : {
        FrameInputSurfaceRole::Exposure,
        FrameInputSurfaceRole::ReactiveMask,
        FrameInputSurfaceRole::TransparencyCompositionMask}) {
        const FrameInputSurfaceDesc* surface = findFrameInputSurface(desc, role);
        if (surfaceProvided(surface)) {
            Status status = validateProvidedSurface(*surface);
            if (!status.succeeded()) {
                return status;
            }
        }
    }

    return Status::ok();
}

Status makeRuntimeFrameDescFromFrameInput(const FrameInputDesc& desc, FrameDesc& outFrame)
{
    Status status = validateFrameInputForFsr4(desc);
    if (!status.succeeded()) {
        return status;
    }

    const FrameInputSurfaceDesc* color = findFrameInputSurface(desc, FrameInputSurfaceRole::Color);
    const FrameInputSurfaceDesc* depth = findFrameInputSurface(desc, FrameInputSurfaceRole::Depth);
    const FrameInputSurfaceDesc* motionVectors = findFrameInputSurface(desc, FrameInputSurfaceRole::MotionVectors);
    const FrameInputSurfaceDesc* output = findFrameInputSurface(desc, FrameInputSurfaceRole::Output);
    const FrameInputSurfaceDesc* exposure = findFrameInputSurface(desc, FrameInputSurfaceRole::Exposure);
    const FrameInputSurfaceDesc* reactiveMask = findFrameInputSurface(desc, FrameInputSurfaceRole::ReactiveMask);
    const FrameInputSurfaceDesc* transparencyMask = findFrameInputSurface(desc, FrameInputSurfaceRole::TransparencyCompositionMask);

    outFrame = FrameDesc{};
    outFrame.frameIndex = desc.frameIndex;
    outFrame.renderSize = desc.constants.renderSize;
    outFrame.displaySize = desc.constants.displaySize;
    outFrame.jitter = desc.constants.jitter;
    outFrame.previousJitter = desc.constants.previousJitter;
    outFrame.camera = desc.constants.camera;
    outFrame.options = desc.options;
    outFrame.options.hasDepth = true;
    outFrame.options.hasExposure = surfaceProvided(exposure);
    outFrame.options.hasReactiveMask = surfaceProvided(reactiveMask);
    outFrame.options.hasTransparencyMask = surfaceProvided(transparencyMask);
    outFrame.borrowedCommandList = desc.command.commandListOrBuffer;

    outFrame.surfaces.color = makeSurfaceDesc(*color);
    outFrame.surfaces.depth = makeSurfaceDesc(*depth);
    outFrame.surfaces.motionVectors = makeSurfaceDesc(*motionVectors);
    outFrame.surfaces.output = makeSurfaceDesc(*output);
    if (surfaceProvided(exposure)) {
        outFrame.surfaces.exposure = makeSurfaceDesc(*exposure);
    }
    if (surfaceProvided(reactiveMask)) {
        outFrame.surfaces.reactiveMask = makeSurfaceDesc(*reactiveMask);
    }
    if (surfaceProvided(transparencyMask)) {
        outFrame.surfaces.transparencyComposition = makeSurfaceDesc(*transparencyMask);
    }
    return Status::ok();
}

} // namespace apusr
