// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstddef>
#include <type_traits>

#include <apusr/apusr_framegen.h>

#include "apusr_framegen_backend.h"
#include "algorithm/apusr_framegen_algorithm_types.h"
#include "algorithm/internal/apusr_frameinterpolation_jitter.h"
#include "algorithm/include/ffx_frameinterpolation.h"
#include "algorithm/include/ffx_opticalflow.h"
#include "ffx_framegeneration.h"
#include "ffx_error.h"

static_assert(std::is_pointer_v<decltype(ApusrFramegenComputeJob::pipeline)>,
              "queued framegen jobs must reference stable pipeline metadata");
static_assert(std::is_trivially_copyable_v<ApusrFramegenShaderBlob>,
              "shader metadata must remain safely copyable by the generated permutation selectors");
static_assert(APUSR_FRAMEGEN_MAX_SRVS == 16 && APUSR_FRAMEGEN_MAX_UAVS == 16,
              "the framegen target must keep the measured binding limits");
static_assert(sizeof(ApusrFramegenGpuJob) <= 1024,
              "queued APUSR FrameGen jobs must remain compact enough for per-pass scheduling");
static_assert(sizeof(ApusrFramegenPipeline) <= 2048,
              "APUSR FrameGen pipelines must not regress to copied per-binding name storage");
static_assert(sizeof(ApusrFrameInterpolationContext) == 32u * 1024u &&
              sizeof(ApusrOpticalFlowContext) == 16u * 1024u &&
              sizeof(ApusrFrameInterpolationContext) + sizeof(ApusrOpticalFlowContext) == 48u * 1024u,
              "provider-internal FI/OF contexts must stay compact instead of regressing to legacy FFX opaque storage");
static_assert(std::is_trivially_copyable_v<ApusrOpticalFlowDispatchDescription> &&
              std::is_trivially_copyable_v<ApusrFrameInterpolationPrepareDescription> &&
              std::is_trivially_copyable_v<ApusrFrameInterpolationDispatchDescription>,
              "native FI/OF descriptors must remain direct value payloads without hidden ownership");
static_assert(static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_TEXTURE1D_USAGE) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_TEXTURE1D_USAGE) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_ASYNC_SUPPORT) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_ASYNC_SUPPORT) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING),
              "FFX create flags must translate losslessly into the native FI contract");
static_assert(static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_PACING_LINES) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_PACING_LINES) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_DISPATCH_RESERVED_1) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_DISPATCH_RESERVED_1) &&
              static_cast<std::uint32_t>(APUSR_FRAMEINTERPOLATION_DISPATCH_RESERVED_2) == static_cast<std::uint32_t>(FFX_FRAMEINTERPOLATION_DISPATCH_RESERVED_2),
              "FFX dispatch flags must translate losslessly into the native FI contract");
static_assert(static_cast<std::uint32_t>(APUSR_OPTICALFLOW_ENABLE_TEXTURE1D_USAGE) ==
                  static_cast<std::uint32_t>(FFX_OPTICALFLOW_ENABLE_TEXTURE1D_USAGE) &&
              static_cast<std::uint32_t>(APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB) ==
                  static_cast<std::uint32_t>(FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB) &&
              static_cast<std::uint32_t>(APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_PQ) ==
                  static_cast<std::uint32_t>(FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ) &&
              static_cast<std::uint32_t>(APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SCRGB) ==
                  static_cast<std::uint32_t>(FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB),
              "FFX OF/transfer-function values must translate losslessly into the native algorithm contract");
static_assert(APUSR_FRAMEGEN_OK == FFX_OK &&
              APUSR_FRAMEGEN_ERROR_INVALID_POINTER == FFX_ERROR_INVALID_POINTER &&
              APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY == FFX_ERROR_OUT_OF_MEMORY &&
              APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT == FFX_ERROR_INVALID_ARGUMENT &&
              APUSR_FRAMEGEN_ERROR_BACKEND_API == FFX_ERROR_BACKEND_API_ERROR &&
              APUSR_FRAMEGEN_ERROR_INSUFFICIENT_MEMORY == FFX_ERROR_INSUFFICIENT_MEMORY,
              "native backend results returned through the FFX facade must stay wire-compatible");
static_assert(FFX_API_EFFECT_MASK == 0x00ff0000u && FFX_API_BACKEND_MASK == 0xff000000u,
              "FFX compatibility IDs must preserve independent effect/backend fields");
static_assert(FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 == 0x0002000cu &&
              FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA == 0x0002000du,
              "FrameGen 3.1.6+ compatibility descriptor IDs must stay wire-compatible");
static_assert(sizeof(ffxDispatchDescFrameGenerationPrepareV2) == 232 &&
              offsetof(ffxDispatchDescFrameGenerationPrepareV2, reset) == 68 &&
              offsetof(ffxDispatchDescFrameGenerationPrepareV2, cameraPosition) == 184,
              "PrepareV2 must match the SDK 2.3 x64 wire layout");
static_assert(sizeof(ffxCallbackDescFrameGenerationPresentPremulAlpha) == 24 &&
              offsetof(ffxCallbackDescFrameGenerationPresentPremulAlpha, usePremulAlpha) == 16,
              "premultiplied-alpha callback extension must match the SDK 2.3 x64 wire layout");

namespace {

struct Probe {
    uint32_t effectContextId = 0;
    ApusrFramegenCommandList commandList = nullptr;
    int32_t destroyedResource = -1;
    int createContextCalls = 0;
    int destroyContextCalls = 0;
    int capabilityCalls = 0;
    int createResourceCalls = 0;
    int destroyResourceCalls = 0;
    int createPipelineCalls = 0;
    int destroyPipelineCalls = 0;
    int hotPathCalls = 0;
    bool failCapabilities = false;
};


Probe& probe(ApusrFramegenBackendInterface* backend)
{
    return *static_cast<Probe*>(backend->scratchBuffer);
}

ApusrFramegenVersion getSdkVersion(ApusrFramegenBackendInterface*)
{
    return APUSR_FRAMEGEN_BACKEND_VERSION;
}

ApusrFramegenResult getMemoryUsage(ApusrFramegenBackendInterface* backend, uint32_t effectContextId, ApusrFramegenMemoryUsage* out)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    out->totalUsageInBytes = 4096;
    out->aliasableUsageInBytes = 1024;
    return FFX_OK;
}

ApusrFramegenResult createContext(ApusrFramegenBackendInterface* backend, uint32_t* outId)
{
    Probe& p = probe(backend);
    ++p.createContextCalls;
    *outId = 23;
    p.effectContextId = *outId;
    return FFX_OK;
}

ApusrFramegenResult getCapabilities(ApusrFramegenBackendInterface* backend, ApusrFramegenDeviceCapabilities* out)
{
    Probe& p = probe(backend);
    ++p.capabilityCalls;
    if (p.failCapabilities)
        return FFX_ERROR_INVALID_ARGUMENT;
    out->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_6;
    return FFX_OK;
}

ApusrFramegenResult destroyContext(ApusrFramegenBackendInterface* backend, uint32_t effectContextId)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    ++p.destroyContextCalls;
    return FFX_OK;
}

ApusrFramegenResult createResource(ApusrFramegenBackendInterface* backend, const ApusrFramegenCreateResourceDescription*, uint32_t effectContextId, ApusrFramegenResource* out)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    ++p.createResourceCalls;
    out->internalIndex = 31;
    return FFX_OK;
}

ApusrFramegenResult destroyResource(ApusrFramegenBackendInterface* backend, ApusrFramegenResource resource, uint32_t effectContextId)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    p.destroyedResource = resource.internalIndex;
    ++p.destroyResourceCalls;
    return FFX_OK;
}

ApusrFramegenResult createPipeline(ApusrFramegenBackendInterface* backend, ApusrFramegenShaderBlob*, const ApusrFramegenPipelineDescription*, uint32_t effectContextId, ApusrFramegenPipeline*)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    ++p.createPipelineCalls;
    return FFX_OK;
}

ApusrFramegenResult destroyPipeline(ApusrFramegenBackendInterface* backend, ApusrFramegenPipeline*, uint32_t effectContextId)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    ++p.destroyPipelineCalls;
    return FFX_OK;
}

ApusrFramegenResult registerResource(ApusrFramegenBackendInterface* backend, const ApusrFramegenExternalResource*, uint32_t effectContextId, ApusrFramegenResource* out)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    ++p.hotPathCalls;
    out->internalIndex = 17;
    return FFX_OK;
}

ApusrFramegenResult unregisterResources(ApusrFramegenBackendInterface* backend, ApusrFramegenCommandList commandList, uint32_t effectContextId)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    p.commandList = commandList;
    ++p.hotPathCalls;
    return FFX_OK;
}

ApusrFramegenResourceDescription getResourceDescription(ApusrFramegenBackendInterface* backend, ApusrFramegenResource resource)
{
    ++probe(backend).hotPathCalls;
    ApusrFramegenResourceDescription desc = {};
    desc.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
    desc.width = static_cast<uint32_t>(resource.internalIndex);
    return desc;
}

ApusrFramegenResult stageConstants(ApusrFramegenBackendInterface* backend, void* data, FfxUInt32 size, ApusrFramegenConstantBuffer* out)
{
    ++probe(backend).hotPathCalls;
    out->data = static_cast<uint32_t*>(data);
    out->num32BitEntries = size / sizeof(uint32_t);
    return FFX_OK;
}

ApusrFramegenResult scheduleJob(ApusrFramegenBackendInterface* backend, const ApusrFramegenGpuJob*)
{
    ++probe(backend).hotPathCalls;
    return FFX_OK;
}

ApusrFramegenResult executeJobs(ApusrFramegenBackendInterface* backend, ApusrFramegenCommandList commandList, uint32_t effectContextId)
{
    Probe& p = probe(backend);
    p.effectContextId = effectContextId;
    p.commandList = commandList;
    ++p.hotPathCalls;
    return FFX_OK;
}

ApusrFramegenBackendInterface makeBackend(Probe& state)
{
    ApusrFramegenBackendInterface backend = {};
    backend.scratchBuffer = &state;
    backend.getVersion = getSdkVersion;
    backend.getGpuMemoryUsage = getMemoryUsage;
    backend.createContext = createContext;
    backend.getDeviceCapabilities = getCapabilities;
    backend.destroyContext = destroyContext;
    backend.createResource = createResource;
    backend.destroyResource = destroyResource;
    backend.createPipeline = createPipeline;
    backend.destroyPipeline = destroyPipeline;
    backend.registerResource = registerResource;
    backend.unregisterResources = unregisterResources;
    backend.getResourceDescription = getResourceDescription;
    backend.stageConstants = stageConstants;
    backend.scheduleJob = scheduleJob;
    backend.executeJobs = executeJobs;
    return backend;
}

} // namespace

int main()
{
    using apusr::framegen::algorithm::computeMotionVectorJitterCancellation;
    const auto cancellation = computeMotionVectorJitterCancellation(
        true, false, true, 40, 41, 0.25f, -0.25f, -0.25f, 0.25f, 100, 200);
    if (cancellation.x != 0.005f || cancellation.y != -0.0025f)
        return 23;
    if (computeMotionVectorJitterCancellation(
            true, true, true, 40, 41, 0.25f, -0.25f, -0.25f, 0.25f, 100, 200).x != 0.0f ||
        computeMotionVectorJitterCancellation(
            true, false, true, 40, 42, 0.25f, -0.25f, -0.25f, 0.25f, 100, 200).y != 0.0f ||
        computeMotionVectorJitterCancellation(
            false, false, true, 40, 41, 0.25f, -0.25f, -0.25f, 0.25f, 100, 200).x != 0.0f)
        return 24;

    ApusrFramegenGpuJob releaseJob = {APUSR_FRAMEGEN_JOB_CLEAR_FLOAT};
    APUSR_FRAMEGEN_SET_JOB_LABEL(releaseJob, L"release labels compile out");

    Probe state;
    ApusrFramegenBackendInterface backend = makeBackend(state);

    if (apusr::framegen::backendVersion(backend) != APUSR_FRAMEGEN_BACKEND_VERSION)
        return 1;

    uint32_t effectContextId = 0;
    ApusrFramegenDeviceCapabilities capabilities = {};
    if (apusr::framegen::createEffectContext(backend, effectContextId, capabilities) != APUSR_FRAMEGEN_OK ||
        effectContextId != 23 || state.createContextCalls != 1 || state.capabilityCalls != 1 || state.destroyContextCalls != 0 ||
        capabilities.maximumSupportedShaderModel != APUSR_FRAMEGEN_SHADER_MODEL_6_6)
        return 2;

    apusr::framegen::BackendContext backendContext(backend, effectContextId);

    ApusrFramegenCreateResourceDescription resourceDescription = {};
    ApusrFramegenResource ownedResource = {};
    if (backendContext.createResource(resourceDescription, ownedResource) != APUSR_FRAMEGEN_OK || ownedResource.internalIndex != 31 || state.createResourceCalls != 1)
        return 3;

    ApusrFramegenShaderBlob shaderBlob = {};
    ApusrFramegenPipelineDescription pipelineDescription = {};
    ApusrFramegenPipeline pipeline = {};
    if (backendContext.createPipeline(shaderBlob, pipelineDescription, pipeline) != APUSR_FRAMEGEN_OK || state.createPipelineCalls != 1)
        return 4;

    ApusrFramegenMemoryUsage memoryUsage = {};
    if (backendContext.gpuMemoryUsage(memoryUsage) != APUSR_FRAMEGEN_OK || memoryUsage.totalUsageInBytes != 4096 || memoryUsage.aliasableUsageInBytes != 1024)
        return 5;

    ApusrFramegenExternalResource externalResource = {};
    externalResource.description.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
    externalResource.state = APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ;
    ApusrFramegenResource internalResource = {};
    uint32_t constants[4] = {};
    ApusrFramegenConstantBuffer constantBuffer = {};
    ApusrFramegenCommandList commandList = reinterpret_cast<ApusrFramegenCommandList>(0x1);

    if (backendContext.registerResource(externalResource, internalResource) != APUSR_FRAMEGEN_OK || internalResource.internalIndex != 17)
        return 6;
    if (backendContext.resourceDescription(internalResource).width != 17)
        return 7;
    if (backendContext.stageConstants(constants, sizeof(constants), constantBuffer) != APUSR_FRAMEGEN_OK ||
        constantBuffer.data != constants || constantBuffer.num32BitEntries != 4)
        return 8;
    if (backendContext.schedule(releaseJob) != APUSR_FRAMEGEN_OK)
        return 9;
    if (backendContext.execute(commandList) != APUSR_FRAMEGEN_OK || state.effectContextId != effectContextId || state.commandList != commandList)
        return 10;
    if (backendContext.unregisterResources(commandList) != APUSR_FRAMEGEN_OK || state.effectContextId != effectContextId || state.hotPathCalls != 6)
        return 11;

    backendContext.releaseCopyResource(ownedResource);
    if (state.destroyResourceCalls != 1 || state.destroyedResource != 32)
        return 13;
    backendContext.releaseResource(ownedResource);
    if (state.destroyResourceCalls != 2 || state.destroyedResource != 31)
        return 14;
    backendContext.releasePipeline(pipeline);
    if (state.destroyPipelineCalls != 1)
        return 15;
    backendContext.releaseContext();
    if (state.destroyContextCalls != 1 || state.effectContextId != effectContextId)
        return 16;

    // Capability failure after context allocation must not leak that context.
    Probe failureState;
    failureState.failCapabilities = true;
    ApusrFramegenBackendInterface failureBackend = makeBackend(failureState);
    uint32_t failedContextId = 99;
    ApusrFramegenDeviceCapabilities failedCapabilities = {};
    if (apusr::framegen::createEffectContext(failureBackend, failedContextId, failedCapabilities) != APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT ||
        failedContextId != 0 || failureState.createContextCalls != 1 || failureState.capabilityCalls != 1 || failureState.destroyContextCalls != 1)
        return 17;

    // The backend-neutral pacing policy must preserve the active half-frame
    // target used by the DX12 presenter and reset without stale history.
    apusr::framegen::PacingEstimator estimator;
    for (int i = 0; i < 10; ++i)
        estimator.update(2000.0);
    if (estimator.targetDelta(1'000'000, 0.0001, 0.1) != 900)
        return 18;
    estimator.reset();
    if (estimator.targetDelta(1'000'000, 0.0001, 0.1) != 0)
        return 19;
    if (!apusr::framegen::shouldBlockForFence(10'000, 5'000, 12'000, 1'000'000, true) ||
        apusr::framegen::shouldBlockForFence(10'000, 3'000, 12'000, 1'000'000, true))
        return 20;

    apusr::framegen::PresentSchedule schedule;
    schedule.frame(apusr::framegen::PresentFrameType::Generated).enabled = true;
    schedule.frame(apusr::framegen::PresentFrameType::Generated).presentIndex = 7;
    if (!schedule.frame(apusr::framegen::PresentFrameType::Generated).enabled ||
        schedule.frame(apusr::framegen::PresentFrameType::Generated).presentIndex != 7)
        return 21;
    schedule.clear();
    if (schedule.frame(apusr::framegen::PresentFrameType::Generated).enabled)
        return 22;

    return 0;
}
