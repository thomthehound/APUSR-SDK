// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_session_vk.h"

#include <cstring>
#include <cwchar>

namespace {

constexpr std::uint32_t kSharedOpticalFlowVector = 1;
constexpr std::uint32_t kSharedOpticalFlowSceneChange = 2;
constexpr std::uint32_t kSharedDilatedDepthBase = 5;
constexpr std::uint32_t kSharedDilatedMotionVectorsBase = 6;
constexpr std::uint32_t kSharedReconstructedPreviousDepthBase = 7;
constexpr std::uint32_t kSharedHistoryStride = 3;
constexpr std::uint32_t kSharedInterpolationSourceBase = 17;
constexpr std::uint32_t kSharedDistortionField = 19;
constexpr std::uint32_t kSharedResourceCount = 20;

} // namespace

struct ApusrFramegenVulkanSession {
    ApusrFramegenBackendInterface backendFi{};
    ApusrFramegenBackendInterface backendShared{};
    ApusrOpticalFlowContext opticalFlow{};
    ApusrFrameInterpolationContext frameInterpolation{};
    ApusrFramegenResource sharedResources[kSharedResourceCount]{};
    std::uint32_t sharedFrameToggle = 0;
    std::uint32_t sharedEffectContext = 0;
    std::uint64_t lastFrameId = 0;
    bool haveLastFrameId = false;
    bool sharedContextCreated = false;
    bool opticalFlowCreated = false;
    bool frameInterpolationCreated = false;
    bool sharedInitializationPending = false;
    bool framePrepared = false;
    std::uint64_t preparedFrameId = 0;
    std::uint32_t preparedHistoryBase = 0;
    std::uint32_t preparedHistoryIndex = 0;
    bool preparedReset = false;
    ApusrFramegenVulkanSessionDescription description{};
};

namespace {

void destroySessionContents(ApusrFramegenVulkanSession& session, const ApusrFramegenAllocator& allocator) noexcept
{
    if (session.sharedContextCreated) {
        for (ApusrFramegenResource& resource : session.sharedResources) {
            if (resource.internalIndex > 0) {
                (void)session.backendShared.destroyResource(
                    &session.backendShared, resource, session.sharedEffectContext);
                resource = {};
            }
        }
    }
    if (session.frameInterpolationCreated) {
        (void)apusrFrameInterpolationContextDestroy(&session.frameInterpolation);
        session.frameInterpolationCreated = false;
    }
    if (session.opticalFlowCreated) {
        (void)apusrOpticalFlowContextDestroy(&session.opticalFlow);
        session.opticalFlowCreated = false;
    }
    if (session.sharedContextCreated) {
        (void)session.backendShared.destroyContext(&session.backendShared, session.sharedEffectContext);
        session.sharedContextCreated = false;
    }
    if (session.backendFi.scratchBuffer) {
        allocator.dealloc(session.backendFi.scratchBuffer);
        session.backendFi = {};
    }
    if (session.backendShared.scratchBuffer) {
        allocator.dealloc(session.backendShared.scratchBuffer);
        session.backendShared = {};
    }
}

ApusrFramegenResult createSharedResources(ApusrFramegenVulkanSession& session) noexcept
{
    ApusrOpticalFlowSharedResourceDescriptions ofDescriptions{};
    ApusrFramegenResult result = apusrOpticalFlowGetSharedResourceDescriptions(&session.opticalFlow, &ofDescriptions);
    if (result != APUSR_FRAMEGEN_OK)
        return result;

    result = session.backendShared.createResource(
        &session.backendShared, &ofDescriptions.opticalFlowVector, session.sharedEffectContext,
        &session.sharedResources[kSharedOpticalFlowVector]);
    if (result != APUSR_FRAMEGEN_OK)
        return result;
    result = session.backendShared.createResource(
        &session.backendShared, &ofDescriptions.opticalFlowSCD, session.sharedEffectContext,
        &session.sharedResources[kSharedOpticalFlowSceneChange]);
    if (result != APUSR_FRAMEGEN_OK)
        return result;

    ApusrFrameInterpolationSharedResourceDescriptions fiDescriptions{};
    result = apusrFrameInterpolationGetSharedResourceDescriptions(&session.frameInterpolation, &fiDescriptions);
    if (result != APUSR_FRAMEGEN_OK)
        return result;

    for (std::uint32_t history = 0; history < 2; ++history) {
        wchar_t name[256]{};

        ApusrFramegenCreateResourceDescription dilatedDepth = fiDescriptions.dilatedDepth;
        if (fiDescriptions.dilatedDepth.name) {
            std::swprintf(name, 256, L"%ls%u", fiDescriptions.dilatedDepth.name, history);
            dilatedDepth.name = name;
        }
        result = session.backendShared.createResource(
            &session.backendShared, &dilatedDepth, session.sharedEffectContext,
            &session.sharedResources[kSharedDilatedDepthBase + history * kSharedHistoryStride]);
        if (result != APUSR_FRAMEGEN_OK)
            return result;

        ApusrFramegenCreateResourceDescription dilatedMotionVectors = fiDescriptions.dilatedMotionVectors;
        if (fiDescriptions.dilatedMotionVectors.name) {
            std::swprintf(name, 256, L"%ls%u", fiDescriptions.dilatedMotionVectors.name, history);
            dilatedMotionVectors.name = name;
        }
        result = session.backendShared.createResource(
            &session.backendShared, &dilatedMotionVectors, session.sharedEffectContext,
            &session.sharedResources[kSharedDilatedMotionVectorsBase + history * kSharedHistoryStride]);
        if (result != APUSR_FRAMEGEN_OK)
            return result;

        ApusrFramegenCreateResourceDescription reconstructedDepth = fiDescriptions.reconstructedPrevNearestDepth;
        if (fiDescriptions.reconstructedPrevNearestDepth.name) {
            std::swprintf(name, 256, L"%ls%u", fiDescriptions.reconstructedPrevNearestDepth.name, history);
            reconstructedDepth.name = name;
        }
        result = session.backendShared.createResource(
            &session.backendShared, &reconstructedDepth, session.sharedEffectContext,
            &session.sharedResources[kSharedReconstructedPreviousDepthBase + history * kSharedHistoryStride]);
        if (result != APUSR_FRAMEGEN_OK)
            return result;

        if (fiDescriptions.interpolationSource.resourceDescription.width != 0 &&
            fiDescriptions.interpolationSource.resourceDescription.height != 0) {
            ApusrFramegenCreateResourceDescription interpolationSource = fiDescriptions.interpolationSource;
            if (fiDescriptions.interpolationSource.name) {
                std::swprintf(name, 256, L"%ls%u", fiDescriptions.interpolationSource.name, history);
                interpolationSource.name = name;
            }
            result = session.backendShared.createResource(
                &session.backendShared, &interpolationSource, session.sharedEffectContext,
                &session.sharedResources[kSharedInterpolationSourceBase + history]);
            if (result != APUSR_FRAMEGEN_OK)
                return result;
        }
    }

    if (session.description.distortionFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN) {
        if (session.description.distortionWidth == 0u || session.description.distortionHeight == 0u)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        const ApusrFramegenCreateResourceDescription retainedDistortion{
            APUSR_FRAMEGEN_HEAP_DEFAULT,
            {APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, session.description.distortionFormat,
             session.description.distortionWidth, session.description.distortionHeight,
             1u, 1u, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY},
            APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ,
            L"FISHARED_DistortionField",
            0u,
            {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}};
        result = session.backendShared.createResource(
            &session.backendShared, &retainedDistortion, session.sharedEffectContext,
            &session.sharedResources[kSharedDistortionField]);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
    }

    session.sharedInitializationPending = true;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenExternalResource sharedResource(
    ApusrFramegenVulkanSession& session,
    const std::uint32_t index) noexcept
{
    return session.backendShared.getResource(&session.backendShared, session.sharedResources[index]);
}

bool validTexture(const ApusrFramegenExternalResource& resource) noexcept
{
    return resource.resource && resource.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D &&
        resource.description.width != 0 && resource.description.height != 0;
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanSession(
    const ApusrFramegenVulkanSessionDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanSession** outSession) noexcept
{
    if (!outSession)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outSession = nullptr;
    if (!description.maxRenderWidth || !description.maxRenderHeight || !description.displayWidth || !description.displayHeight ||
        description.backBufferFormat == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    ApusrFramegenVulkanSession* session = allocator.construct<ApusrFramegenVulkanSession>();
    if (!session)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    session->description = description;

    auto fail = [&](const ApusrFramegenResult result) noexcept {
        destroySessionContents(*session, allocator);
        allocator.destroy(session);
        return result;
    };

    ApusrFramegenResult result = apusrFramegenCreateVulkanBackend(
        description.deviceContext, &session->backendShared, 1, allocator);
    if (result != APUSR_FRAMEGEN_OK)
        return fail(result);
    result = apusrFramegenCreateVulkanBackend(description.deviceContext, &session->backendFi, 2, allocator);
    if (result != APUSR_FRAMEGEN_OK)
        return fail(result);

    result = session->backendShared.createContext(&session->backendShared, &session->sharedEffectContext);
    if (result != APUSR_FRAMEGEN_OK)
        return fail(result);
    session->sharedContextCreated = true;

    ApusrOpticalFlowContextDescription opticalFlowDescription{};
    opticalFlowDescription.backendInterface = session->backendFi;
    opticalFlowDescription.resolution = {description.displayWidth, description.displayHeight};
    result = apusrOpticalFlowContextCreate(&session->opticalFlow, &opticalFlowDescription);
    if (result != APUSR_FRAMEGEN_OK)
        return fail(result);
    session->opticalFlowCreated = true;

    ApusrFrameInterpolationContextDescription interpolationDescription{};
    interpolationDescription.backendInterface = session->backendFi;
    interpolationDescription.maxRenderSize = {description.maxRenderWidth, description.maxRenderHeight};
    interpolationDescription.displaySize = {description.displayWidth, description.displayHeight};
    interpolationDescription.backBufferFormat = description.backBufferFormat;
    interpolationDescription.previousInterpolationSourceFormat =
        description.hudlessFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN ? description.hudlessFormat : description.backBufferFormat;
    if (description.depthInverted)
        interpolationDescription.flags |= APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
    if (description.depthInfinite)
        interpolationDescription.flags |= APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
    if (description.motionVectorsAtDisplayResolution)
        interpolationDescription.flags |= APUSR_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (description.motionVectorsJittered)
        interpolationDescription.flags |= APUSR_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS;
    if (description.hdr)
        interpolationDescription.flags |= APUSR_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT;
    if (description.hudlessFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN)
        interpolationDescription.flags |= APUSR_FRAMEINTERPOLATION_ENABLE_EXTERNAL_INTERPOLATION_HISTORY;
    result = apusrFrameInterpolationContextCreate(&session->frameInterpolation, &interpolationDescription);
    if (result != APUSR_FRAMEGEN_OK)
        return fail(result);
    session->frameInterpolationCreated = true;

    result = createSharedResources(*session);
    if (result != APUSR_FRAMEGEN_OK)
        return fail(result);

    *outSession = session;
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenDestroyVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!session)
        return;
    destroySessionContents(*session, allocator);
    allocator.destroy(session);
}

ApusrFramegenResult apusrFramegenPrepareVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenVulkanFrameDescription& frame) noexcept
{
    if (!session || !frame.commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (session->framePrepared)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (!frame.renderWidth || !frame.renderHeight || frame.renderWidth > session->description.maxRenderWidth ||
        frame.renderHeight > session->description.maxRenderHeight || !validTexture(frame.depth) ||
        !validTexture(frame.motionVectors))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const ApusrFramegenCommandList commandList = apusrFramegenCommandListVK(frame.commandBuffer);
    if (session->sharedInitializationPending) {
        const ApusrFramegenResult initializeResult = session->backendShared.executeJobs(
            &session->backendShared, commandList, session->sharedEffectContext);
        if (initializeResult != APUSR_FRAMEGEN_OK)
            return initializeResult;
        session->sharedInitializationPending = false;
    }

    const bool disjointFrame = session->haveLastFrameId &&
        (frame.frameId <= session->lastFrameId || frame.frameId - session->lastFrameId != 1);
    const bool reset = frame.reset || disjointFrame;
    const std::uint32_t nextFrameToggle = (session->sharedFrameToggle + 1u) & 1u;
    const std::uint32_t historyBase = nextFrameToggle * kSharedHistoryStride;

    ApusrFrameInterpolationPrepareDescription prepare{};
    prepare.flags = frame.prepareFlags;
    prepare.commandList = commandList;
    prepare.renderSize = {frame.renderWidth, frame.renderHeight};
    prepare.jitterOffset = frame.jitterOffset;
    prepare.motionVectorScale = frame.motionVectorScale;
    prepare.frameTimeDelta = frame.frameTimeDelta;
    prepare.cameraNear = frame.cameraNear;
    prepare.cameraFar = frame.cameraFar;
    prepare.viewSpaceToMetersFactor = frame.viewSpaceToMetersFactor;
    prepare.cameraFovAngleVertical = frame.cameraFovAngleVertical;
    prepare.depth = frame.depth;
    prepare.motionVectors = frame.motionVectors;
    prepare.frameID = frame.frameId;
    prepare.reset = reset;
    prepare.dilatedDepth = sharedResource(*session, kSharedDilatedDepthBase + historyBase);
    prepare.dilatedMotionVectors = sharedResource(*session, kSharedDilatedMotionVectorsBase + historyBase);
    prepare.reconstructedPrevDepth = sharedResource(*session, kSharedReconstructedPreviousDepthBase + historyBase);
    if (session->description.hudlessFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN) {
        if (!validTexture(frame.hudlessColor))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        prepare.interpolationSource = frame.hudlessColor;
        prepare.retainedInterpolationSource = sharedResource(*session, kSharedInterpolationSourceBase + nextFrameToggle);
    }
    if (session->description.distortionFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN) {
        if (!validTexture(frame.distortionField) ||
            frame.distortionField.description.format != session->description.distortionFormat ||
            frame.distortionField.description.width != session->description.distortionWidth ||
            frame.distortionField.description.height != session->description.distortionHeight)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        prepare.distortionField = frame.distortionField;
        prepare.retainedDistortionField = sharedResource(*session, kSharedDistortionField);
    }
    std::memcpy(prepare.cameraPosition, frame.cameraPosition, sizeof(prepare.cameraPosition));
    std::memcpy(prepare.cameraUp, frame.cameraUp, sizeof(prepare.cameraUp));
    std::memcpy(prepare.cameraRight, frame.cameraRight, sizeof(prepare.cameraRight));
    std::memcpy(prepare.cameraForward, frame.cameraForward, sizeof(prepare.cameraForward));

    const ApusrFramegenVulkanGpuProfileFrameInfo prepareProfileInfo{
        .frameId = frame.frameId,
        .renderWidth = frame.renderWidth,
        .renderHeight = frame.renderHeight,
        .displayWidth = session->description.displayWidth,
        .displayHeight = session->description.displayHeight,
        .interpolationX = frame.interpolationRect.left,
        .interpolationY = frame.interpolationRect.top,
        .interpolationWidth = frame.interpolationRect.width,
        .interpolationHeight = frame.interpolationRect.height,
        .frameTimeDeltaMs = frame.frameTimeDelta,
        .jitterX = frame.jitterOffset.x,
        .jitterY = frame.jitterOffset.y,
        .motionVectorScaleX = frame.motionVectorScale.x,
        .motionVectorScaleY = frame.motionVectorScale.y,
        .prepareFlags = frame.prepareFlags,
        .dispatchFlags = frame.dispatchFlags,
        .hasHudlessColor = session->description.hudlessFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN,
        .motionVectorsAtDisplayResolution = session->description.motionVectorsAtDisplayResolution,
        .motionVectorsJittered = session->description.motionVectorsJittered,
        .hdr = session->description.hdr,
        .reset = reset,
        .phase = APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_PREPARE,
    };
    apusrFramegenBeginVulkanGpuProfile(&session->backendFi, frame.commandBuffer, prepareProfileInfo);
    const ApusrFramegenResult result = apusrFrameInterpolationPrepare(&session->frameInterpolation, &prepare);
    if (result != APUSR_FRAMEGEN_OK) {
        apusrFramegenCancelVulkanGpuProfile(&session->backendFi, frame.commandBuffer);
        return result;
    }
    apusrFramegenEndVulkanGpuProfile(&session->backendFi, frame.commandBuffer);

    session->sharedFrameToggle = nextFrameToggle;
    session->preparedFrameId = frame.frameId;
    session->preparedHistoryBase = historyBase;
    session->preparedHistoryIndex = nextFrameToggle;
    session->preparedReset = reset;
    session->framePrepared = true;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenDispatchPreparedVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenVulkanFrameDescription& frame) noexcept
{
    if (!session || !frame.commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (!session->framePrepared || frame.frameId != session->preparedFrameId)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (!frame.renderWidth || !frame.renderHeight || frame.renderWidth > session->description.maxRenderWidth ||
        frame.renderHeight > session->description.maxRenderHeight || !validTexture(frame.presentColor) ||
        !validTexture(frame.output))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const ApusrFramegenCommandList commandList = apusrFramegenCommandListVK(frame.commandBuffer);
    const bool reset = session->preparedReset;
    const std::uint32_t historyBase = session->preparedHistoryBase;
    const bool externalInterpolationHistory =
        session->description.hudlessFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    const std::uint32_t currentInterpolationHistory = session->preparedHistoryIndex;
    const std::uint32_t previousInterpolationHistory = currentInterpolationHistory ^ 1u;
    const ApusrFramegenExternalResource retainedHudless = externalInterpolationHistory
        ? sharedResource(*session, kSharedInterpolationSourceBase + currentInterpolationHistory)
        : ApusrFramegenExternalResource{};

    const ApusrFramegenVulkanGpuProfileFrameInfo profileInfo{
        .frameId = frame.frameId,
        .renderWidth = frame.renderWidth,
        .renderHeight = frame.renderHeight,
        .displayWidth = session->description.displayWidth,
        .displayHeight = session->description.displayHeight,
        .interpolationX = frame.interpolationRect.left,
        .interpolationY = frame.interpolationRect.top,
        .interpolationWidth = frame.interpolationRect.width,
        .interpolationHeight = frame.interpolationRect.height,
        .frameTimeDeltaMs = frame.frameTimeDelta,
        .jitterX = frame.jitterOffset.x,
        .jitterY = frame.jitterOffset.y,
        .motionVectorScaleX = frame.motionVectorScale.x,
        .motionVectorScaleY = frame.motionVectorScale.y,
        .prepareFlags = frame.prepareFlags,
        .dispatchFlags = frame.dispatchFlags,
        .hasHudlessColor = externalInterpolationHistory || validTexture(frame.hudlessColor),
        .motionVectorsAtDisplayResolution = session->description.motionVectorsAtDisplayResolution,
        .motionVectorsJittered = session->description.motionVectorsJittered,
        .hdr = session->description.hdr,
        .reset = reset,
        .phase = APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_COMPUTE,
    };
    apusrFramegenBeginVulkanGpuProfile(&session->backendFi, frame.commandBuffer, profileInfo);

    ApusrOpticalFlowDispatchDescription opticalFlow{};
    opticalFlow.commandList = commandList;
    opticalFlow.color = externalInterpolationHistory
        ? retainedHudless
        : (validTexture(frame.hudlessColor) ? frame.hudlessColor : frame.presentColor);
    opticalFlow.opticalFlowVector = sharedResource(*session, kSharedOpticalFlowVector);
    opticalFlow.opticalFlowSCD = sharedResource(*session, kSharedOpticalFlowSceneChange);
    opticalFlow.reset = reset;
    opticalFlow.backbufferTransferFunction = frame.backbufferTransferFunction;
    opticalFlow.minMaxLuminance = {frame.minMaxLuminance[0], frame.minMaxLuminance[1]};
    ApusrFramegenResult result = apusrOpticalFlowDispatch(&session->opticalFlow, &opticalFlow);
    if (result != APUSR_FRAMEGEN_OK) {
        apusrFramegenCancelVulkanGpuProfile(&session->backendFi, frame.commandBuffer);
        return result;
    }

    ApusrFrameInterpolationDispatchDescription interpolation{};
    interpolation.flags = frame.dispatchFlags;
    interpolation.commandList = commandList;
    interpolation.displaySize = {session->description.displayWidth, session->description.displayHeight};
    interpolation.renderSize = {frame.renderWidth, frame.renderHeight};
    interpolation.currentBackBuffer = frame.presentColor;
    interpolation.currentBackBufferHudless = externalInterpolationHistory ? retainedHudless : frame.hudlessColor;
    if (externalInterpolationHistory) {
        interpolation.previousInterpolationSource =
            sharedResource(*session, kSharedInterpolationSourceBase + previousInterpolationHistory);
    }
    interpolation.output = frame.output;
    interpolation.interpolationRect = frame.interpolationRect;
    if (interpolation.interpolationRect.width == 0 || interpolation.interpolationRect.height == 0) {
        interpolation.interpolationRect = {
            0, 0,
            static_cast<std::int32_t>(session->description.displayWidth),
            static_cast<std::int32_t>(session->description.displayHeight)};
    }
    interpolation.opticalFlowVector = sharedResource(*session, kSharedOpticalFlowVector);
    interpolation.opticalFlowSceneChangeDetection = sharedResource(*session, kSharedOpticalFlowSceneChange);
    interpolation.opticalFlowBufferSize = {session->description.displayWidth, session->description.displayHeight};
    interpolation.opticalFlowScale = {
        1.0f / static_cast<float>(session->description.displayWidth),
        1.0f / static_cast<float>(session->description.displayHeight)};
    interpolation.opticalFlowBlockSize = 8;
    interpolation.cameraNear = frame.cameraNear;
    interpolation.cameraFar = frame.cameraFar;
    interpolation.cameraFovAngleVertical = frame.cameraFovAngleVertical;
    interpolation.viewSpaceToMetersFactor = frame.viewSpaceToMetersFactor;
    interpolation.frameTimeDelta = frame.frameTimeDelta;
    interpolation.reset = reset;
    interpolation.backBufferTransferFunction = frame.backbufferTransferFunction;
    interpolation.minMaxLuminance[0] = frame.minMaxLuminance[0];
    interpolation.minMaxLuminance[1] = frame.minMaxLuminance[1];
    interpolation.frameID = frame.frameId;
    interpolation.dilatedDepth = sharedResource(*session, kSharedDilatedDepthBase + historyBase);
    interpolation.dilatedMotionVectors = sharedResource(*session, kSharedDilatedMotionVectorsBase + historyBase);
    interpolation.reconstructedPrevDepth = sharedResource(*session, kSharedReconstructedPreviousDepthBase + historyBase);
    interpolation.distortionField =
        session->description.distortionFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN
            ? sharedResource(*session, kSharedDistortionField)
            : frame.distortionField;
    result = apusrFrameInterpolationDispatch(&session->frameInterpolation, &interpolation);
    if (result != APUSR_FRAMEGEN_OK) {
        apusrFramegenCancelVulkanGpuProfile(&session->backendFi, frame.commandBuffer);
        return result;
    }
    apusrFramegenEndVulkanGpuProfile(&session->backendFi, frame.commandBuffer);

    session->lastFrameId = frame.frameId;
    session->haveLastFrameId = true;
    session->framePrepared = false;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenDispatchVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenVulkanFrameDescription& frame) noexcept
{
    const ApusrFramegenResult prepareResult = apusrFramegenPrepareVulkanSession(session, frame);
    return prepareResult == APUSR_FRAMEGEN_OK
        ? apusrFramegenDispatchPreparedVulkanSession(session, frame)
        : prepareResult;
}
