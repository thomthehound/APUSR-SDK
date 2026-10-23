// This APUSR-maintained source is derived from the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "apusr_framegen_backend_compat.h"
#include "apusr_framegen_config_compat.h"
#include "apusr_framegen_resource_compat.h"
#include "ffx_framegeneration.hpp"
#include "ffx_provider_fsr3framegeneration.h"
#include "ffx_frameinterpolation.h"
#include "ffx_opticalflow.h"
#include "apusr_framegen_backend.h"
#include "algorithm/apusr_framegen_algorithm_types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
// Preserve the FidelityFX 3 shared-resource slot numbering because the active
// provider still exchanges ApusrFramegenResource handles with the compatibility
// backend. Slots 3-4 are intentionally reserved by the original contract.
constexpr uint32_t kSharedOpticalFlowVector = 1;
constexpr uint32_t kSharedOpticalFlowSceneChange = 2;
constexpr uint32_t kSharedDilatedDepthBase = 5;
constexpr uint32_t kSharedDilatedMotionVectorsBase = 6;
constexpr uint32_t kSharedReconstructedPreviousDepthBase = 7;
constexpr uint32_t kSharedHistoryStride = 3;
constexpr uint32_t kSharedResourceCount = 17;

inline FfxErrorCode toFfxError(ApusrFramegenResult result) noexcept
{
    return static_cast<FfxErrorCode>(result);
}
}

bool ffxProvider_Fsr3FrameGeneration::canProvide(ffxStructType_t type) const noexcept
{
    return (type & FFX_API_EFFECT_MASK) == FFX_API_EFFECT_ID_FRAMEGENERATION;
}

constexpr uint32_t MAX_QUEUED_FRAMES = 2;

struct PrepareState
{
    uint64_t frameID = 0;
    uint32_t flags = 0;
    ApusrFramegenCommandList commandList = nullptr;
    ApusrFramegenDimensions2D renderSize{};
    ApusrFramegenFloatCoords2D jitterOffset{};
    ApusrFramegenFloatCoords2D motionVectorScale{};
    float frameTimeDelta = 0.0f;
    bool reset = false;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFovAngleVertical = 0.0f;
    float viewSpaceToMetersFactor = 0.0f;
    ApusrFramegenExternalResource depth{};
    ApusrFramegenExternalResource motionVectors{};
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
};

struct InternalFgContext
{
    ApusrFramegenContextHeader header;

    ApusrFramegenBackendInterface backendInterfaceFi;
    ApusrFramegenBackendInterface backendInterfaceShared;
    ApusrOpticalFlowContext ofContext;
    ApusrFrameInterpolationContext fiContext;
    ApusrFramegenResource sharedResources[kSharedResourceCount];
    uint32_t sharedResourceFrameToggle;
    uint32_t effectContextIdShared;

    ApusrFramegenExternalResource HUDLessColor;
    ApusrFramegenExternalResource distortionField;

    bool frameGenEnabled;
    uint32_t frameGenFlags;
    PrepareState prepareDescriptions[MAX_QUEUED_FRAMES]{};

    struct Callbacks {
        FfxApiPresentCallbackFunc presentCallback;
        void* presentCallbackUserContext;
        FfxApiFrameGenerationDispatchFunc frameGenerationCallback;
        void* frameGenerationCallbackUserContext;
    } callbacks[MAX_QUEUED_FRAMES];

    uint64_t lastConfigureFrameID;
    uint64_t lastFrameID;
};

#define STRINGIFY_(X) #X
#define STRINGIFY(X) STRINGIFY_(X) 
#define MAKE_VERSION_STRING(major, minor, patch) STRINGIFY major "." STRINGIFY minor "." STRINGIFY patch

std::uint64_t ffxProvider_Fsr3FrameGeneration::id() const noexcept
{
    // FG, version from header
    return 0xF600'0000ULL << 32u | (apusrFramegenMakeVersion(FFX_FRAMEINTERPOLATION_VERSION_MAJOR, FFX_FRAMEINTERPOLATION_VERSION_MINOR, FFX_FRAMEINTERPOLATION_VERSION_PATCH) & 0xFFFF'FFFF);
}

const char* ffxProvider_Fsr3FrameGeneration::versionName() const noexcept
{
    return MAKE_VERSION_STRING(FFX_FRAMEINTERPOLATION_VERSION_MAJOR, FFX_FRAMEINTERPOLATION_VERSION_MINOR, FFX_FRAMEINTERPOLATION_VERSION_PATCH);
}

ffxReturnCode_t ffxProvider_Fsr3FrameGeneration::createContext(ffxContext* context, ffxCreateContextDescHeader* header, const ApusrFramegenAllocator& alloc) const
{
    if (auto desc = ffx::DynamicCast<ffxCreateContextDescFrameGeneration>(header))
    {
        InternalFgContext* internal_context = alloc.construct<InternalFgContext>();
        VERIFY(internal_context, FFX_API_RETURN_ERROR_MEMORY);
        internal_context->header.provider = this;

        ApusrFramegenResult backendResult = apusrFramegenCreateBackendFromFfx(
            header->pNext, &internal_context->backendInterfaceShared, 1, alloc);
        if (backendResult != APUSR_FRAMEGEN_OK) {
            alloc.destroy(internal_context);
            return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        }

        backendResult = apusrFramegenCreateBackendFromFfx(
            header->pNext, &internal_context->backendInterfaceFi, 2, alloc);
        if (backendResult != APUSR_FRAMEGEN_OK) {
            alloc.dealloc(internal_context->backendInterfaceShared.scratchBuffer);
            alloc.destroy(internal_context);
            return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        }

        bool sharedContextCreated = false;
        bool opticalFlowCreated = false;
        bool frameInterpolationCreated = false;

        const auto failCreate = [&](ffxReturnCode_t error) {
            if (sharedContextCreated) {
                for (ApusrFramegenResource resource : internal_context->sharedResources) {
                    if (resource.internalIndex > 0)
                        (void)internal_context->backendInterfaceShared.destroyResource(
                            &internal_context->backendInterfaceShared, resource, internal_context->effectContextIdShared);
                }
            }
            if (frameInterpolationCreated)
                (void)apusrFrameInterpolationContextDestroy(&internal_context->fiContext);
            if (opticalFlowCreated)
                (void)apusrOpticalFlowContextDestroy(&internal_context->ofContext);
            if (sharedContextCreated)
                (void)internal_context->backendInterfaceShared.destroyContext(
                    &internal_context->backendInterfaceShared, internal_context->effectContextIdShared);

            alloc.dealloc(internal_context->backendInterfaceFi.scratchBuffer);
            alloc.dealloc(internal_context->backendInterfaceShared.scratchBuffer);
            alloc.destroy(internal_context);
            return error;
        };

        if (internal_context->backendInterfaceShared.createContext(
                &internal_context->backendInterfaceShared, &internal_context->effectContextIdShared) != APUSR_FRAMEGEN_OK)
            return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);
        sharedContextCreated = true;

        ApusrOpticalFlowContextDescription ofDescription{};
        ofDescription.backendInterface = internal_context->backendInterfaceFi;
        ofDescription.resolution = {desc->displaySize.width, desc->displaySize.height};
        if (apusrOpticalFlowContextCreate(&internal_context->ofContext, &ofDescription) != APUSR_FRAMEGEN_OK)
            return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);
        opticalFlowCreated = true;

        ApusrFrameInterpolationContextDescription fiDescription{};
        fiDescription.backendInterface = internal_context->backendInterfaceFi;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS : 0;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE : 0;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT : 0;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_ASYNC_SUPPORT : 0;
        fiDescription.flags |= (desc->flags & FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING)
            ? APUSR_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING : 0;
        fiDescription.maxRenderSize = {desc->maxRenderSize.width, desc->maxRenderSize.height};
        fiDescription.displaySize = {desc->displaySize.width, desc->displaySize.height};
        fiDescription.backBufferFormat = static_cast<ApusrFramegenSurfaceFormat>(desc->backBufferFormat);
        fiDescription.previousInterpolationSourceFormat = fiDescription.backBufferFormat;
        for (auto it = header; it; it = it->pNext)
        {
            if (auto descHudless = ffx::DynamicCast<ffxCreateContextDescFrameGenerationHudless>(it))
                fiDescription.previousInterpolationSourceFormat =
                    static_cast<ApusrFramegenSurfaceFormat>(descHudless->hudlessBackBufferFormat);
        }
        if (apusrFrameInterpolationContextCreate(&internal_context->fiContext, &fiDescription) != APUSR_FRAMEGEN_OK)
            return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);
        frameInterpolationCreated = true;

        ApusrOpticalFlowSharedResourceDescriptions ofResourceDescs{};
        if (apusrOpticalFlowGetSharedResourceDescriptions(&internal_context->ofContext, &ofResourceDescs) != APUSR_FRAMEGEN_OK)
            return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);

        if (internal_context->backendInterfaceShared.createResource(
                &internal_context->backendInterfaceShared, &ofResourceDescs.opticalFlowVector,
                internal_context->effectContextIdShared, &internal_context->sharedResources[kSharedOpticalFlowVector]) != APUSR_FRAMEGEN_OK ||
            internal_context->backendInterfaceShared.createResource(
                &internal_context->backendInterfaceShared, &ofResourceDescs.opticalFlowSCD,
                internal_context->effectContextIdShared, &internal_context->sharedResources[kSharedOpticalFlowSceneChange]) != APUSR_FRAMEGEN_OK)
            return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);

        ApusrFrameInterpolationSharedResourceDescriptions fiResourceDescs{};
        if (apusrFrameInterpolationGetSharedResourceDescriptions(&internal_context->fiContext, &fiResourceDescs) != APUSR_FRAMEGEN_OK)
            return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);

        internal_context->sharedResourceFrameToggle = 0;
        wchar_t Name[256] = {};
        for (std::uint32_t i = 0; i < 2; i++)
        {
            ApusrFramegenCreateResourceDescription dilD = fiResourceDescs.dilatedDepth;
            swprintf(Name, 255, L"%s%d", fiResourceDescs.dilatedDepth.name, i);
            dilD.name = Name;
            if (internal_context->backendInterfaceShared.createResource(
                    &internal_context->backendInterfaceShared, &dilD, internal_context->effectContextIdShared,
                    &internal_context->sharedResources[kSharedDilatedDepthBase + (i * kSharedHistoryStride)]) != APUSR_FRAMEGEN_OK)
                return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);

            ApusrFramegenCreateResourceDescription dilMVs = fiResourceDescs.dilatedMotionVectors;
            swprintf(Name, 255, L"%s%d", fiResourceDescs.dilatedMotionVectors.name, i);
            dilMVs.name = Name;
            if (internal_context->backendInterfaceShared.createResource(
                    &internal_context->backendInterfaceShared, &dilMVs, internal_context->effectContextIdShared,
                    &internal_context->sharedResources[kSharedDilatedMotionVectorsBase + (i * kSharedHistoryStride)]) != APUSR_FRAMEGEN_OK)
                return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);

            ApusrFramegenCreateResourceDescription recND = fiResourceDescs.reconstructedPrevNearestDepth;
            swprintf(Name, 255, L"%s%d", fiResourceDescs.reconstructedPrevNearestDepth.name, i);
            recND.name = Name;
            if (internal_context->backendInterfaceShared.createResource(
                    &internal_context->backendInterfaceShared, &recND, internal_context->effectContextIdShared,
                    &internal_context->sharedResources[kSharedReconstructedPreviousDepthBase + (i * kSharedHistoryStride)]) != APUSR_FRAMEGEN_OK)
                return failCreate(FFX_API_RETURN_ERROR_RUNTIME_ERROR);
        }

        *context = internal_context;
        return FFX_API_RETURN_OK;
    }
    else
    {
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

ffxReturnCode_t ffxProvider_Fsr3FrameGeneration::destroyContext(ffxContext* context, const ApusrFramegenAllocator& alloc) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);

    InternalFgContext* internal_context = reinterpret_cast<InternalFgContext*>(*context);
    
    { // copied from ffxFsr3ContextDestroy, simplified.
        for (std::uint32_t i = 0; i < kSharedResourceCount; i++)
        {
            TRY2(internal_context->backendInterfaceShared.destroyResource(&internal_context->backendInterfaceShared, internal_context->sharedResources[i], internal_context->effectContextIdShared));
        }

        TRY2(toFfxError(apusrFrameInterpolationContextDestroy(&internal_context->fiContext)));

        TRY2(toFfxError(apusrOpticalFlowContextDestroy(&internal_context->ofContext)));

        TRY2(internal_context->backendInterfaceShared.destroyContext(&internal_context->backendInterfaceShared, internal_context->effectContextIdShared));
    }

    alloc.dealloc(internal_context->backendInterfaceFi.scratchBuffer);
    alloc.dealloc(internal_context->backendInterfaceShared.scratchBuffer);
    alloc.dealloc(internal_context);

    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxProvider_Fsr3FrameGeneration::configure(ffxContext* context, const ffxConfigureDescHeader* header) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(header, FFX_API_RETURN_ERROR_PARAMETER);

    InternalFgContext* internal_context = reinterpret_cast<InternalFgContext*>(*context);
    if (auto desc = ffx::DynamicCast<ffxConfigureDescFrameGeneration>(header))
    {
        ApusrFramegenConfig config{};
        config.allowAsyncWorkloads = desc->allowAsyncWorkloads;
        config.flags = desc->flags;

        size_t callbacksIndex = desc->frameID % MAX_QUEUED_FRAMES;

        bool const bPresentCallbackChanged = (internal_context->callbacks[callbacksIndex].presentCallback != desc->presentCallback) || (desc->presentCallback && (internal_context->callbacks[callbacksIndex].presentCallbackUserContext != desc->presentCallbackUserContext));
        bool const bFrameGenerationCallback = (internal_context->callbacks[callbacksIndex].frameGenerationCallback != desc->frameGenerationCallback) || (desc->frameGenerationCallback && (internal_context->callbacks[callbacksIndex].frameGenerationCallbackUserContext != desc->frameGenerationCallbackUserContext));
        internal_context->callbacks[callbacksIndex].presentCallback = desc->presentCallback;
        internal_context->callbacks[callbacksIndex].frameGenerationCallback = desc->frameGenerationCallback;
        internal_context->callbacks[callbacksIndex].presentCallbackUserContext = desc->presentCallbackUserContext;
        internal_context->callbacks[callbacksIndex].frameGenerationCallbackUserContext = desc->frameGenerationCallbackUserContext;

        config.frameGenerationCallback = nullptr;
        config.frameGenerationCallbackContext = nullptr;
        if (desc->frameGenerationCallback != nullptr)
        {
            config.frameGenerationCallback = [](const FfxFrameGenerationDispatchDescription* desc, void* ctx) -> FfxErrorCode {
                size_t callbacksIndex = desc->frameID % MAX_QUEUED_FRAMES;
                InternalFgContext* internal_context = reinterpret_cast<InternalFgContext*>(ctx);
                auto callbacks = &internal_context->callbacks[callbacksIndex];
                VERIFY(callbacks->frameGenerationCallback, FFX_ERROR_BACKEND_API_ERROR);
                
                ffx::DispatchDescFrameGeneration dispatchDesc{};
                
                dispatchDesc.backbufferTransferFunction = desc->backBufferTransferFunction;
                dispatchDesc.commandList = desc->commandList;
                dispatchDesc.minMaxLuminance[0] = desc->minMaxLuminance[0];
                dispatchDesc.minMaxLuminance[1] = desc->minMaxLuminance[1];
                dispatchDesc.numGeneratedFrames = desc->numInterpolatedFrames;
                dispatchDesc.outputs[0] = desc->outputs[0];
                dispatchDesc.outputs[1] = desc->outputs[1];
                dispatchDesc.outputs[2] = desc->outputs[2];
                dispatchDesc.outputs[3] = desc->outputs[3];
                dispatchDesc.presentColor = desc->presentColor;
                dispatchDesc.reset = desc->reset;
                dispatchDesc.generationRect.top = desc->interpolationRect.top;
                dispatchDesc.generationRect.left = desc->interpolationRect.left;
                dispatchDesc.generationRect.height = desc->interpolationRect.height;
                dispatchDesc.generationRect.width = desc->interpolationRect.width;
                dispatchDesc.frameID = desc->frameID;
                
                if (FFX_API_RETURN_OK != callbacks->frameGenerationCallback(&dispatchDesc, callbacks->frameGenerationCallbackUserContext))
                    return FFX_ERROR_BACKEND_API_ERROR;
                return FFX_OK;
            };
            config.frameGenerationCallbackContext = internal_context;
        }

        config.presentCallback = nullptr;
        config.presentCallbackContext = nullptr;
        if (desc->presentCallback != nullptr)
        {
            config.presentCallback = [](const FfxPresentCallbackDescription* params, void* ctx) -> FfxErrorCode {
                size_t callbacksIndex = params->frameID % MAX_QUEUED_FRAMES;
                InternalFgContext* internal_context = reinterpret_cast<InternalFgContext*>(ctx);
                auto callbacks = &internal_context->callbacks[callbacksIndex];
                VERIFY(callbacks->presentCallback, FFX_ERROR_BACKEND_API_ERROR);
                
                // Keep the advertised 3.1.5 callback contract exact. The SDK 2.3
                // premultiplied-alpha extension remains defined locally for a future
                // negotiated provider update, but is not emitted by this provider.
                ffxCallbackDescFrameGenerationPresent cbDesc{};
                cbDesc.header.pNext = nullptr;
                cbDesc.header.type = FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT;

                cbDesc.commandList = params->commandList;
                cbDesc.currentBackBuffer = params->currentBackBuffer;
                cbDesc.currentUI = params->currentUI;
                cbDesc.device = params->device;
                cbDesc.isGeneratedFrame = params->isInterpolatedFrame;
                cbDesc.outputSwapChainBuffer = params->outputSwapChainBuffer;
                cbDesc.frameID = params->frameID;

                if (FFX_API_RETURN_OK != callbacks->presentCallback(&cbDesc, callbacks->presentCallbackUserContext))
                    return FFX_ERROR_BACKEND_API_ERROR;
                return FFX_OK;
            };
            config.presentCallbackContext = internal_context;
        }

        config.drawDebugPacingLines = false;
        if (desc->flags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES)
        {
            config.drawDebugPacingLines = true;
        }

        config.frameGenerationEnabled = desc->frameGenerationEnabled;
        config.HUDLessColor = apusrFramegenResourceFromFfx(desc->HUDLessColor);
        config.onlyPresentInterpolated = desc->onlyPresentGenerated;
        config.swapChain = desc->swapChain;

        config.interpolationRect.top    = desc->generationRect.top;
        config.interpolationRect.left   = desc->generationRect.left;
        config.interpolationRect.width  = desc->generationRect.width;
        config.interpolationRect.height = desc->generationRect.height;

        config.frameID = desc->frameID;

        { // copied from ffxFsr3ConfigureFrameGeneration
            internal_context->frameGenFlags = config.flags;
            internal_context->HUDLessColor = config.HUDLessColor;

            if (config.flags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW)
            {
                config.onlyPresentInterpolated = true;
            }

            internal_context->frameGenEnabled = config.frameGenerationEnabled;

            if (!(config.flags & FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY))
            {
                // When the frame ID is not incrementing by 1 we could end up overwriting a pointer that is in-use, so reset the swap-chain state
                if (((internal_context->lastConfigureFrameID + 1) != desc->frameID) && (bPresentCallbackChanged || bFrameGenerationCallback))
                {
                    ApusrFramegenConfig resetConfig = config;
                    resetConfig.frameGenerationCallback = nullptr;
                    resetConfig.frameGenerationCallbackContext = nullptr;
                    resetConfig.presentCallback = nullptr;
                    resetConfig.presentCallbackContext = nullptr;

                    TRY2(apusrFramegenApplySwapchainConfig(&resetConfig));
                }

                TRY2(apusrFramegenApplySwapchainConfig(&config));

                internal_context->lastConfigureFrameID = desc->frameID;
            }
        }

        internal_context->distortionField = {};
        for (auto it = header; it; it = it->pNext)
        {
            if (auto distortionFieldDesc = ffx::DynamicCast<ffxConfigureDescFrameGenerationRegisterDistortionFieldResource>(it))
            {
                if (distortionFieldDesc->distortionField.resource)
                {
                    internal_context->distortionField = apusrFramegenResourceFromFfx(distortionFieldDesc->distortionField);
                }
            }
        }

        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_PARAMETER;
}

ffxReturnCode_t ffxProvider_Fsr3FrameGeneration::query(ffxContext* context, ffxQueryDescHeader* header) const
{
    VERIFY(header, FFX_API_RETURN_ERROR_PARAMETER);
    
    if (auto desc = ffx::DynamicCast<ffxQueryDescFrameGenerationGetGPUMemoryUsage>(header))
    {
        VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
        VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);
        VERIFY(desc->gpuMemoryUsageFrameGeneration, FFX_API_RETURN_ERROR_PARAMETER);

        memset(desc->gpuMemoryUsageFrameGeneration, 0, sizeof(FfxApiEffectMemoryUsage));

        InternalFgContext* internal_context = reinterpret_cast<InternalFgContext*>(*context);
        ApusrFramegenMemoryUsage frameInterpolationUsage{};
        ApusrFramegenMemoryUsage opticalFlowUsage{};
        ApusrFramegenMemoryUsage sharedUsage{};

        TRY2(toFfxError(apusrFrameInterpolationContextGetGpuMemoryUsage(&internal_context->fiContext, &frameInterpolationUsage)));
        TRY2(toFfxError(apusrOpticalFlowContextGetGpuMemoryUsage(&internal_context->ofContext, &opticalFlowUsage)));
        const apusr::framegen::BackendContext sharedBackend(internal_context->backendInterfaceShared, internal_context->effectContextIdShared);
        TRY2(toFfxError(sharedBackend.gpuMemoryUsage(sharedUsage)));
        desc->gpuMemoryUsageFrameGeneration->totalUsageInBytes =
            frameInterpolationUsage.totalUsageInBytes + opticalFlowUsage.totalUsageInBytes + sharedUsage.totalUsageInBytes;
        desc->gpuMemoryUsageFrameGeneration->aliasableUsageInBytes =
            frameInterpolationUsage.aliasableUsageInBytes + opticalFlowUsage.aliasableUsageInBytes + sharedUsage.aliasableUsageInBytes;

        return FFX_API_RETURN_OK;
    }
    if (auto desc = ffx::DynamicCast<ffxQueryDescFrameGenerationGetGPUMemoryUsageV2>(header))
    {
        VERIFY(desc->gpuMemoryUsageFrameGeneration, FFX_API_RETURN_ERROR_PARAMETER);

        memset(desc->gpuMemoryUsageFrameGeneration, 0, sizeof(FfxApiEffectMemoryUsage));

        ApusrFramegenMemoryUsage frameInterpolationUsage{};
        ApusrFramegenMemoryUsage opticalFlowUsage{};
        const auto backBufferFormat = static_cast<ApusrFramegenSurfaceFormat>(desc->backBufferFormat);
        const auto hudlessBackBufferFormat = static_cast<ApusrFramegenSurfaceFormat>(desc->hudlessBackBufferFormat);
        const auto previousInterpolationSourceFormat =
            hudlessBackBufferFormat != APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN ? hudlessBackBufferFormat : backBufferFormat;

        TRY2(toFfxError(apusrFrameInterpolationGetGpuMemoryUsage(
            static_cast<ApusrFramegenDevice>(desc->device),
            {desc->maxRenderSize.width, desc->maxRenderSize.height},
            {desc->displaySize.width, desc->displaySize.height},
            previousInterpolationSourceFormat,
            &frameInterpolationUsage)));
        TRY2(toFfxError(apusrOpticalFlowGetGpuMemoryUsage(
            static_cast<ApusrFramegenDevice>(desc->device),
            {desc->displaySize.width, desc->displaySize.height},
            &opticalFlowUsage)));
        desc->gpuMemoryUsageFrameGeneration->totalUsageInBytes =
            frameInterpolationUsage.totalUsageInBytes + opticalFlowUsage.totalUsageInBytes;
        desc->gpuMemoryUsageFrameGeneration->aliasableUsageInBytes =
            frameInterpolationUsage.aliasableUsageInBytes + opticalFlowUsage.aliasableUsageInBytes;

        return FFX_API_RETURN_OK;
    }
    else
    {
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

ffxReturnCode_t ffxProvider_Fsr3FrameGeneration::dispatch(ffxContext* context, const ffxDispatchDescHeader* header) const
{
    VERIFY(context, FFX_API_RETURN_ERROR_PARAMETER);
    VERIFY(*context, FFX_API_RETURN_ERROR_PARAMETER);

    InternalFgContext* internal_context = reinterpret_cast<InternalFgContext*>(*context);
    if (auto desc = ffx::DynamicCast<ffxDispatchDescFrameGeneration>(header))
    {
        const PrepareState* prepDesc = &internal_context->prepareDescriptions[desc->frameID % MAX_QUEUED_FRAMES];

        const bool frameIdDecreased = desc->frameID < internal_context->lastFrameID;
        const bool frameIdSkipped = (desc->frameID - internal_context->lastFrameID) > 1;
        const bool frameIdDisjoint =
            frameIdDecreased || frameIdSkipped || desc->frameID != internal_context->lastConfigureFrameID;
        const bool reset = desc->reset || prepDesc->reset || frameIdDisjoint;

        // Optical flow
        {
            ApusrOpticalFlowDispatchDescription ofDispatchDesc{};
            ofDispatchDesc.commandList = desc->commandList;
            ofDispatchDesc.color = internal_context->HUDLessColor.resource
                ? internal_context->HUDLessColor
                : apusrFramegenResourceFromFfx(desc->presentColor);
            ofDispatchDesc.reset = reset;
            ofDispatchDesc.backbufferTransferFunction =
                static_cast<ApusrFramegenBackbufferTransferFunction>(desc->backbufferTransferFunction);
            ofDispatchDesc.minMaxLuminance.x = desc->minMaxLuminance[0];
            ofDispatchDesc.minMaxLuminance.y = desc->minMaxLuminance[1];
            ofDispatchDesc.opticalFlowVector = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedOpticalFlowVector]);
            ofDispatchDesc.opticalFlowSCD = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedOpticalFlowSceneChange]);

            TRY2(toFfxError(apusrOpticalFlowDispatch(&internal_context->ofContext, &ofDispatchDesc)));
        }

        // Frame interpolation
        {
            ApusrFrameInterpolationDispatchDescription fiDispatchDesc{};

            // don't dispatch interpolation async for now: use the same commandlist for copy and interpolate
            fiDispatchDesc.commandList = desc->commandList;
            fiDispatchDesc.displaySize = {desc->presentColor.description.width, desc->presentColor.description.height};
            fiDispatchDesc.currentBackBuffer = apusrFramegenResourceFromFfx(desc->presentColor);
            fiDispatchDesc.currentBackBufferHudless = internal_context->HUDLessColor;
            fiDispatchDesc.reset = reset;

            fiDispatchDesc.renderSize = prepDesc->renderSize;
            fiDispatchDesc.output = apusrFramegenResourceFromFfx(desc->outputs[0]);
            fiDispatchDesc.opticalFlowVector = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedOpticalFlowVector]);
            fiDispatchDesc.opticalFlowSceneChangeDetection = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedOpticalFlowSceneChange]);
            fiDispatchDesc.opticalFlowBlockSize = 8;
            fiDispatchDesc.opticalFlowScale = { 1.f / fiDispatchDesc.displaySize.width, 1.f / fiDispatchDesc.displaySize.height };
            fiDispatchDesc.frameTimeDelta = prepDesc->frameTimeDelta;
            fiDispatchDesc.cameraNear = prepDesc->cameraNear;
            fiDispatchDesc.cameraFar = prepDesc->cameraFar;
            fiDispatchDesc.viewSpaceToMetersFactor = prepDesc->viewSpaceToMetersFactor;
            fiDispatchDesc.cameraFovAngleVertical = prepDesc->cameraFovAngleVertical;
            
            fiDispatchDesc.dilatedDepth = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedDilatedDepthBase + (internal_context->sharedResourceFrameToggle * kSharedHistoryStride)]);
            fiDispatchDesc.dilatedMotionVectors = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedDilatedMotionVectorsBase + (internal_context->sharedResourceFrameToggle * kSharedHistoryStride)]);
            fiDispatchDesc.reconstructedPrevDepth = internal_context->backendInterfaceShared.getResource(
                &internal_context->backendInterfaceShared,
                internal_context->sharedResources[kSharedReconstructedPreviousDepthBase + (internal_context->sharedResourceFrameToggle * kSharedHistoryStride)]);

            if (desc->generationRect.height == 0 && desc->generationRect.width == 0)
            {
                fiDispatchDesc.interpolationRect.left   = 0;
                fiDispatchDesc.interpolationRect.top    = 0;
                fiDispatchDesc.interpolationRect.width  = desc->presentColor.description.width;
                fiDispatchDesc.interpolationRect.height = desc->presentColor.description.height;
            }
            else
            {
                fiDispatchDesc.interpolationRect.top    = desc->generationRect.top;
                fiDispatchDesc.interpolationRect.left   = desc->generationRect.left;
                fiDispatchDesc.interpolationRect.width  = desc->generationRect.width;
                fiDispatchDesc.interpolationRect.height = desc->generationRect.height;
            }
            
            if (internal_context->frameGenFlags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES)
            {
                fiDispatchDesc.flags |= APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES;
            }

            
            if (internal_context->frameGenFlags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS)
            {
                fiDispatchDesc.flags |= APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS;
            }

            if (internal_context->frameGenFlags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW)
            {
                fiDispatchDesc.flags |= APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW;
            }

            if (internal_context->frameGenFlags & FFX_FRAMEGENERATION_FLAG_RESERVED_1)
            {
                fiDispatchDesc.flags |= APUSR_FRAMEINTERPOLATION_DISPATCH_RESERVED_1;
            }

            if (internal_context->frameGenFlags & FFX_FRAMEGENERATION_FLAG_RESERVED_2)
            {
                fiDispatchDesc.flags |= APUSR_FRAMEINTERPOLATION_DISPATCH_RESERVED_2;
            }

            fiDispatchDesc.backBufferTransferFunction =
                static_cast<ApusrFramegenBackbufferTransferFunction>(desc->backbufferTransferFunction);
            fiDispatchDesc.minMaxLuminance[0]         = desc->minMaxLuminance[0];
            fiDispatchDesc.minMaxLuminance[1]         = desc->minMaxLuminance[1];

            fiDispatchDesc.frameID = desc->frameID;

            if (internal_context->distortionField.resource)
            {
                fiDispatchDesc.distortionField = internal_context->distortionField;
            }
            TRY2(toFfxError(apusrFrameInterpolationDispatch(&internal_context->fiContext, &fiDispatchDesc)));
            internal_context->lastFrameID = desc->frameID;
        }

        return FFX_API_RETURN_OK;
    }
    else if (header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE ||
             header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2)
    {
        PrepareState& prepare = internal_context->prepareDescriptions[
            (header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE
                 ? reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare*>(header)->frameID
                 : reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2*>(header)->frameID) %
            MAX_QUEUED_FRAMES];
        prepare = {};

        if (header->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE)
        {
            const auto* desc = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare*>(header);
            prepare.frameID = desc->frameID;
            prepare.flags = desc->flags;
            prepare.commandList = desc->commandList;
            prepare.renderSize = {desc->renderSize.width, desc->renderSize.height};
            prepare.jitterOffset = {desc->jitterOffset.x, desc->jitterOffset.y};
            prepare.motionVectorScale = {desc->motionVectorScale.x, desc->motionVectorScale.y};
            prepare.frameTimeDelta = desc->frameTimeDelta;
            prepare.reset = desc->unused_reset;
            prepare.cameraNear = desc->cameraNear;
            prepare.cameraFar = desc->cameraFar;
            prepare.cameraFovAngleVertical = desc->cameraFovAngleVertical;
            prepare.viewSpaceToMetersFactor = desc->viewSpaceToMetersFactor;
            prepare.depth = apusrFramegenResourceFromFfx(desc->depth);
            prepare.motionVectors = apusrFramegenResourceFromFfx(desc->motionVectors);

            for (auto it = header; it; it = it->pNext)
            {
                if (auto* cameraInfo = ffx::DynamicCast<ffxDispatchDescFrameGenerationPrepareCameraInfo>(it))
                {
                    std::memcpy(prepare.cameraPosition, cameraInfo->cameraPosition, sizeof(FfxFloat32x3));
                    std::memcpy(prepare.cameraUp, cameraInfo->cameraUp, sizeof(FfxFloat32x3));
                    std::memcpy(prepare.cameraRight, cameraInfo->cameraRight, sizeof(FfxFloat32x3));
                    std::memcpy(prepare.cameraForward, cameraInfo->cameraForward, sizeof(FfxFloat32x3));
                    break;
                }
            }
        }
        else
        {
            const auto* desc = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2*>(header);
            prepare.frameID = desc->frameID;
            prepare.flags = desc->flags;
            prepare.commandList = desc->commandList;
            prepare.renderSize = {desc->renderSize.width, desc->renderSize.height};
            prepare.jitterOffset = {desc->jitterOffset.x, desc->jitterOffset.y};
            prepare.motionVectorScale = {desc->motionVectorScale.x, desc->motionVectorScale.y};
            prepare.frameTimeDelta = desc->frameTimeDelta;
            prepare.reset = desc->reset;
            prepare.cameraNear = desc->cameraNear;
            prepare.cameraFar = desc->cameraFar;
            prepare.cameraFovAngleVertical = desc->cameraFovAngleVertical;
            prepare.viewSpaceToMetersFactor = desc->viewSpaceToMetersFactor;
            prepare.depth = apusrFramegenResourceFromFfx(desc->depth);
            prepare.motionVectors = apusrFramegenResourceFromFfx(desc->motionVectors);
            std::memcpy(prepare.cameraPosition, desc->cameraPosition, sizeof(FfxFloat32x3));
            std::memcpy(prepare.cameraUp, desc->cameraUp, sizeof(FfxFloat32x3));
            std::memcpy(prepare.cameraRight, desc->cameraRight, sizeof(FfxFloat32x3));
            std::memcpy(prepare.cameraForward, desc->cameraForward, sizeof(FfxFloat32x3));
        }

        internal_context->sharedResourceFrameToggle = (internal_context->sharedResourceFrameToggle + 1) & 1;

        ApusrFrameInterpolationPrepareDescription dispatchDesc{};
        dispatchDesc.flags = prepare.flags;
        dispatchDesc.commandList = prepare.commandList;
        dispatchDesc.renderSize = prepare.renderSize;
        dispatchDesc.jitterOffset = prepare.jitterOffset;
        dispatchDesc.motionVectorScale = prepare.motionVectorScale;
        dispatchDesc.frameTimeDelta = prepare.frameTimeDelta;
        dispatchDesc.cameraNear = prepare.cameraNear;
        dispatchDesc.cameraFar = prepare.cameraFar;
        dispatchDesc.viewSpaceToMetersFactor = prepare.viewSpaceToMetersFactor;
        dispatchDesc.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
        dispatchDesc.depth = prepare.depth;
        dispatchDesc.motionVectors = prepare.motionVectors;
        dispatchDesc.frameID = prepare.frameID;
        dispatchDesc.reset = prepare.reset;

        dispatchDesc.dilatedDepth = internal_context->backendInterfaceShared.getResource(
            &internal_context->backendInterfaceShared,
            internal_context->sharedResources[kSharedDilatedDepthBase +
                                              (internal_context->sharedResourceFrameToggle * kSharedHistoryStride)]);
        dispatchDesc.dilatedMotionVectors = internal_context->backendInterfaceShared.getResource(
            &internal_context->backendInterfaceShared,
            internal_context->sharedResources[kSharedDilatedMotionVectorsBase +
                                              (internal_context->sharedResourceFrameToggle * kSharedHistoryStride)]);
        dispatchDesc.reconstructedPrevDepth = internal_context->backendInterfaceShared.getResource(
            &internal_context->backendInterfaceShared,
            internal_context->sharedResources[kSharedReconstructedPreviousDepthBase +
                                              (internal_context->sharedResourceFrameToggle * kSharedHistoryStride)]);

        std::memcpy(dispatchDesc.cameraPosition, prepare.cameraPosition, sizeof(FfxFloat32x3));
        std::memcpy(dispatchDesc.cameraUp, prepare.cameraUp, sizeof(FfxFloat32x3));
        std::memcpy(dispatchDesc.cameraRight, prepare.cameraRight, sizeof(FfxFloat32x3));
        std::memcpy(dispatchDesc.cameraForward, prepare.cameraForward, sizeof(FfxFloat32x3));

        TRY2(toFfxError(apusrFrameInterpolationPrepare(&internal_context->fiContext, &dispatchDesc)));
        return FFX_API_RETURN_OK;
    }
    else
    {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
}

ffxProvider_Fsr3FrameGeneration ffxProvider_Fsr3FrameGeneration::Instance;
