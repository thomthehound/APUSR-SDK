// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_streamline_vk.h"

#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr std::uint32_t kVkImageUsageStorageBit = 0x00000008u;

std::uint32_t activeWidth(const ApusrFramegenStreamlineVulkanResource& resource) noexcept
{
    return resource.extentWidth != 0u ? resource.extentWidth : resource.width;
}

std::uint32_t activeHeight(const ApusrFramegenStreamlineVulkanResource& resource) noexcept
{
    return resource.extentHeight != 0u ? resource.extentHeight : resource.height;
}

ApusrFramegenResourceState stateFromLayout(const std::uint32_t layout) noexcept
{
    switch (static_cast<apusr::backends::vk::ImageLayout>(layout)) {
    case apusr::backends::vk::ImageLayoutGeneral:
        return APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS;
    case apusr::backends::vk::ImageLayoutColorAttachmentOptimal:
        return APUSR_FRAMEGEN_RESOURCE_STATE_RENDER_TARGET;
    case apusr::backends::vk::ImageLayoutDepthStencilAttachmentOptimal:
        return APUSR_FRAMEGEN_RESOURCE_STATE_DEPTH_ATTACHMENT;
    case apusr::backends::vk::ImageLayoutDepthStencilReadOnlyOptimal:
    case apusr::backends::vk::ImageLayoutShaderReadOnlyOptimal:
        return APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ;
    case apusr::backends::vk::ImageLayoutTransferSrcOptimal:
        return APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC;
    case apusr::backends::vk::ImageLayoutTransferDstOptimal:
        return APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST;
    case apusr::backends::vk::ImageLayoutPresentSrcKhr:
        return APUSR_FRAMEGEN_RESOURCE_STATE_PRESENT;
    default:
        return APUSR_FRAMEGEN_RESOURCE_STATE_COMMON;
    }
}

bool completeTexture(const ApusrFramegenStreamlineVulkanResource& resource) noexcept
{
    return resource.tagged && resource.image != nullptr && resource.imageView != nullptr &&
        resource.nativeFormat != 0u && resource.width != 0u && resource.height != 0u;
}

void fillMetadata(
    const ApusrFramegenStreamlineVulkanResource& resource,
    ApusrFramegenVulkanExternalResourceMetadata& metadata) noexcept
{
    metadata = {};
    metadata.imageView = resource.imageView;
    metadata.nativeFormat = resource.nativeFormat;
    metadata.nativeLayout = resource.nativeLayout;
    metadata.aspectMask = resource.aspectMask;
    metadata.baseMipLevel = resource.baseMipLevel;
    metadata.levelCount = resource.levelCount != 0u ? resource.levelCount : std::max(resource.mipLevels, 1u);
    metadata.baseArrayLayer = resource.baseArrayLayer;
    metadata.layerCount = resource.layerCount != 0u ? resource.layerCount : std::max(resource.arrayLayers, 1u);
}

ApusrFramegenExternalResource makeTexture(
    const ApusrFramegenStreamlineVulkanResource& source,
    const std::uint32_t usage,
    ApusrFramegenVulkanExternalResourceMetadata& metadata) noexcept
{
    if (!completeTexture(source)) {
        return {};
    }
    fillMetadata(source, metadata);
    ApusrFramegenResourceDescription description{};
    description.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
    description.format = apusrFramegenSurfaceFormatFromVulkan(source.nativeFormat);
    description.width = source.width;
    description.height = source.height;
    description.depth = 1u;
    description.mipCount = std::max(source.mipLevels, 1u);
    description.flags = APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    ApusrFramegenResourceState state = stateFromLayout(source.nativeLayout);
    if (static_cast<apusr::backends::vk::ImageLayout>(source.nativeLayout) ==
            apusr::backends::vk::ImageLayoutGeneral &&
        (usage & APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY) != 0u &&
        (usage & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) == 0u) {
        state = APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ;
    }
    return apusrFramegenExternalResourceVK(source.image, description, state, &metadata);
}

bool finitePositive(const float value) noexcept
{
    return std::isfinite(value) && value > 0.0f;
}

} // namespace

ApusrFramegenResult apusrFramegenAdaptStreamlineVulkanFrame(
    const ApusrFramegenStreamlineVulkanInputs& inputs,
    ApusrFramegenAdaptedStreamlineVulkanFrame* outFrame) noexcept
{
    if (!outFrame) {
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    }
    *outFrame = {};
    if (!inputs.commandBuffer || inputs.frameId == 0u || !inputs.constants.valid ||
        !completeTexture(inputs.depth) || !completeTexture(inputs.motionVectors) ||
        !completeTexture(inputs.presentColor) || !completeTexture(inputs.generatedOutput)) {
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }
    if ((inputs.generatedOutput.nativeUsage & kVkImageUsageStorageBit) == 0u) {
        // FI writes its output as a storage image. A normal swapchain image is
        // not assumed to have STORAGE usage; the present adapter must supply a
        // storage-capable generated-frame intermediate when the swapchain does
        // not expose it.
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    const std::uint32_t renderWidth = activeWidth(inputs.depth) != 0u
        ? activeWidth(inputs.depth) : activeWidth(inputs.motionVectors);
    const std::uint32_t renderHeight = activeHeight(inputs.depth) != 0u
        ? activeHeight(inputs.depth) : activeHeight(inputs.motionVectors);
    const std::uint32_t displayWidth = activeWidth(inputs.backbufferContract) != 0u
        ? activeWidth(inputs.backbufferContract) : activeWidth(inputs.presentColor);
    const std::uint32_t displayHeight = activeHeight(inputs.backbufferContract) != 0u
        ? activeHeight(inputs.backbufferContract) : activeHeight(inputs.presentColor);
    if (renderWidth == 0u || renderHeight == 0u || displayWidth == 0u || displayHeight == 0u ||
        renderWidth > inputs.depth.width || renderHeight > inputs.depth.height ||
        displayWidth > inputs.presentColor.width || displayHeight > inputs.presentColor.height) {
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    const ApusrFramegenSurfaceFormat backBufferFormat =
        apusrFramegenSurfaceFormatFromVulkan(inputs.presentColor.nativeFormat);
    if (backBufferFormat == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN ||
        apusrFramegenSurfaceFormatFromVulkan(inputs.depth.nativeFormat) == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN ||
        apusrFramegenSurfaceFormatFromVulkan(inputs.motionVectors.nativeFormat) == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN ||
        apusrFramegenSurfaceFormatFromVulkan(inputs.generatedOutput.nativeFormat) == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN) {
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    const bool infiniteDepth = inputs.constants.cameraFar >= std::numeric_limits<float>::max() * 0.5f;
    if (!finitePositive(inputs.constants.cameraNear) || !finitePositive(inputs.constants.cameraFov) ||
        (!infiniteDepth && !finitePositive(inputs.constants.cameraFar)) ||
        inputs.constants.orthographicProjection) {
        // The current FI math is perspective-camera based. Do not silently
        // invent camera constants for an unsupported/invalid Streamline frame.
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    ApusrFramegenVulkanSessionDescription& session = outFrame->sessionDescription;
    session.deviceContext = inputs.deviceContext;
    session.maxRenderWidth = inputs.depth.width;
    session.maxRenderHeight = inputs.depth.height;
    session.displayWidth = displayWidth;
    session.displayHeight = displayHeight;
    session.backBufferFormat = backBufferFormat;
    session.hudlessFormat = completeTexture(inputs.hudlessColor)
        ? apusrFramegenSurfaceFormatFromVulkan(inputs.hudlessColor.nativeFormat)
        : APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    if (completeTexture(inputs.hudlessColor) &&
        session.hudlessFormat == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (completeTexture(inputs.distortionField)) {
        session.distortionFormat = apusrFramegenSurfaceFormatFromVulkan(inputs.distortionField.nativeFormat);
        session.distortionWidth = activeWidth(inputs.distortionField);
        session.distortionHeight = activeHeight(inputs.distortionField);
        if (session.distortionFormat == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN ||
            session.distortionWidth == 0u || session.distortionHeight == 0u ||
            session.distortionWidth > inputs.distortionField.width ||
            session.distortionHeight > inputs.distortionField.height)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }
    session.depthInverted = inputs.constants.depthInverted;
    session.depthInfinite = infiniteDepth;
    session.motionVectorsAtDisplayResolution =
        activeWidth(inputs.motionVectors) == displayWidth && activeHeight(inputs.motionVectors) == displayHeight;
    session.motionVectorsJittered = inputs.constants.motionVectorsJittered;
    session.hdr = inputs.hdr;

    ApusrFramegenVulkanFrameDescription& frame = outFrame->frameDescription;
    frame.commandBuffer = inputs.commandBuffer;
    frame.frameId = inputs.frameId;
    frame.renderWidth = renderWidth;
    frame.renderHeight = renderHeight;
    frame.interpolationRect = {
        static_cast<std::int32_t>(inputs.backbufferContract.extentLeft),
        static_cast<std::int32_t>(inputs.backbufferContract.extentTop),
        static_cast<std::int32_t>(displayWidth),
        static_cast<std::int32_t>(displayHeight)};
    frame.jitterOffset = inputs.constants.jitterOffset;
    // Streamline's mvecScale already normalizes the tagged motion-vector values
    // into [-1, 1]. FidelityFX Frame Interpolation expects the pre-normalized
    // scale here and divides it by the motion-vector target dimensions during
    // prepare. Passing Streamline's value through directly therefore divided
    // by width/height twice and made game motion almost disappear. Convert at
    // the frontend boundary, exactly once.
    const float motionVectorTargetWidth = static_cast<float>(
        session.motionVectorsAtDisplayResolution ? displayWidth : renderWidth);
    const float motionVectorTargetHeight = static_cast<float>(
        session.motionVectorsAtDisplayResolution ? displayHeight : renderHeight);
    frame.motionVectorScale = {
        inputs.constants.motionVectorScale.x * motionVectorTargetWidth,
        inputs.constants.motionVectorScale.y * motionVectorTargetHeight};
    frame.frameTimeDelta = inputs.frameTimeDelta;
    frame.cameraNear = inputs.constants.cameraNear;
    frame.cameraFar = inputs.constants.cameraFar;
    frame.cameraFovAngleVertical = inputs.constants.cameraFov;
    frame.viewSpaceToMetersFactor = 1.0f;
    std::memcpy(frame.cameraPosition, inputs.constants.cameraPosition, sizeof(frame.cameraPosition));
    std::memcpy(frame.cameraUp, inputs.constants.cameraUp, sizeof(frame.cameraUp));
    std::memcpy(frame.cameraRight, inputs.constants.cameraRight, sizeof(frame.cameraRight));
    std::memcpy(frame.cameraForward, inputs.constants.cameraForward, sizeof(frame.cameraForward));
    frame.reset = inputs.constants.reset;
    frame.backbufferTransferFunction = inputs.backbufferTransferFunction;
    frame.minMaxLuminance[0] = inputs.minMaxLuminance[0];
    frame.minMaxLuminance[1] = inputs.minMaxLuminance[1];

    frame.depth = makeTexture(
        inputs.depth,
        APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY | APUSR_FRAMEGEN_RESOURCE_USAGE_DEPTHTARGET,
        outFrame->depthMetadata);
    frame.motionVectors = makeTexture(
        inputs.motionVectors,
        APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY,
        outFrame->motionVectorMetadata);
    frame.presentColor = makeTexture(
        inputs.presentColor,
        APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY,
        outFrame->presentColorMetadata);
    if (completeTexture(inputs.hudlessColor)) {
        frame.hudlessColor = makeTexture(
            inputs.hudlessColor,
            APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY,
            outFrame->hudlessColorMetadata);
    }
    if (completeTexture(inputs.distortionField)) {
        frame.distortionField = makeTexture(
            inputs.distortionField,
            APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY,
            outFrame->distortionMetadata);
    }
    frame.output = makeTexture(
        inputs.generatedOutput,
        APUSR_FRAMEGEN_RESOURCE_USAGE_UAV,
        outFrame->outputMetadata);

    if (!frame.depth.resource || !frame.motionVectors.resource || !frame.presentColor.resource || !frame.output.resource) {
        *outFrame = {};
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }
    return APUSR_FRAMEGEN_OK;
}
