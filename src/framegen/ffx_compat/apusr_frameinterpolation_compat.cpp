// APUSR-maintained FidelityFX Frame Interpolation compatibility facade.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
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

#include "algorithm/include/ffx_frameinterpolation.h"
#include "algorithm/include/ffx_message.h"
#include "algorithm/internal/apusr_framegen_algorithm_storage.h"
#include "apusr_framegen_backend.h"
#include "ffx_compat/apusr_framegen_algorithm_compat.h"

#include <cstddef>
#include <cstdint>

namespace {

static_assert(sizeof(FfxFrameInterpolationContext) >=
    APUSR_FRAMEINTERPOLATION_CONTEXT_STORAGE_SIZE + alignof(ApusrFrameInterpolationContext) - 1);


void* legacyContextStorage(FfxFrameInterpolationContext* context) noexcept
{
    auto address = reinterpret_cast<std::uintptr_t>(context->data);
    constexpr std::uintptr_t alignment = alignof(ApusrFrameInterpolationContext);
    address = (address + alignment - 1u) & ~(alignment - 1u);
    return reinterpret_cast<void*>(address);
}

} // namespace

FFX_API FfxErrorCode ffxFrameInterpolationGetSharedResourceDescriptions(
    FfxFrameInterpolationContext* context,
    FfxFrameInterpolationSharedResourceDescriptions* sharedResources)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(sharedResources, FFX_ERROR_INVALID_POINTER);

    ApusrFrameInterpolationSharedResourceDescriptions nativeDescriptions{};
    const ApusrFramegenResult result = apusr::framegen::algorithm::frameInterpolationGetSharedResourceDescriptionsStorage(
        legacyContextStorage(context), &nativeDescriptions);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    sharedResources->reconstructedPrevNearestDepth = nativeDescriptions.reconstructedPrevNearestDepth;
    sharedResources->dilatedDepth = nativeDescriptions.dilatedDepth;
    sharedResources->dilatedMotionVectors = nativeDescriptions.dilatedMotionVectors;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxFrameInterpolationContextCreate(
    FfxFrameInterpolationContext* context,
    FfxFrameInterpolationContextDescription* contextDescription)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(contextDescription, FFX_ERROR_INVALID_POINTER);

    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.getVersion, FFX_ERROR_INCOMPLETE_INTERFACE);
    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.getDeviceCapabilities, FFX_ERROR_INCOMPLETE_INTERFACE);
    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.createContext, FFX_ERROR_INCOMPLETE_INTERFACE);
    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.destroyContext, FFX_ERROR_INCOMPLETE_INTERFACE);
    if (contextDescription->backendInterface.scratchBuffer)
        FFX_RETURN_ON_ERROR(contextDescription->backendInterface.scratchBufferSize, FFX_ERROR_INCOMPLETE_INTERFACE);

    const ApusrFrameInterpolationContextDescription nativeDescription =
        apusrFrameInterpolationContextDescriptionFromFfx(*contextDescription);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::frameInterpolationContextCreateStorage(
        legacyContextStorage(context), &nativeDescription));
}

FFX_API FfxErrorCode ffxFrameInterpolationContextGetGpuMemoryUsage(
    FfxFrameInterpolationContext* context,
    FfxApiEffectMemoryUsage* vramUsage)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(vramUsage, FFX_ERROR_INVALID_POINTER);

    ApusrFramegenMemoryUsage nativeUsage{};
    const ApusrFramegenResult result = apusr::framegen::algorithm::frameInterpolationContextGetGpuMemoryUsageStorage(
        legacyContextStorage(context), &nativeUsage);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    vramUsage->totalUsageInBytes = nativeUsage.totalUsageInBytes;
    vramUsage->aliasableUsageInBytes = nativeUsage.aliasableUsageInBytes;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxSharedContextGetGpuMemoryUsage(
    ApusrFramegenBackendInterface* backendInterfaceShared,
    FfxApiEffectMemoryUsage* vramUsage)
{
    FFX_RETURN_ON_ERROR(backendInterfaceShared, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(vramUsage, FFX_ERROR_INVALID_POINTER);

    const apusr::framegen::BackendContext backendContext(*backendInterfaceShared, 0);
    ApusrFramegenMemoryUsage nativeUsage{};
    const ApusrFramegenResult result = backendContext.gpuMemoryUsage(nativeUsage);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    vramUsage->totalUsageInBytes = nativeUsage.totalUsageInBytes;
    vramUsage->aliasableUsageInBytes = nativeUsage.aliasableUsageInBytes;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxFrameInterpolationGetGpuMemoryUsage(
    FfxDevice device,
    FfxApiDimensions2D* maxRenderSize,
    FfxApiDimensions2D* displaySize,
    FfxApiSurfaceFormat previousInterpolationSourceFormat,
    FfxApiEffectMemoryUsage* vramUsage)
{
    FFX_RETURN_ON_ERROR(maxRenderSize, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(displaySize, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(vramUsage, FFX_ERROR_INVALID_POINTER);

    ApusrFramegenMemoryUsage nativeUsage{};
    const ApusrFramegenResult result = apusrFrameInterpolationGetGpuMemoryUsage(
        static_cast<ApusrFramegenDevice>(device),
        apusrFramegenDimensionsFromFfx(*maxRenderSize),
        apusrFramegenDimensionsFromFfx(*displaySize),
        static_cast<ApusrFramegenSurfaceFormat>(previousInterpolationSourceFormat),
        &nativeUsage);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    vramUsage->totalUsageInBytes += nativeUsage.totalUsageInBytes;
    vramUsage->aliasableUsageInBytes += nativeUsage.aliasableUsageInBytes;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxFrameInterpolationContextDestroy(FfxFrameInterpolationContext* context)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::frameInterpolationContextDestroyStorage(legacyContextStorage(context)));
}

FFX_API FfxErrorCode ffxFrameInterpolationContextEnqueueRefreshPipelineRequest(FfxFrameInterpolationContext* context)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::frameInterpolationEnqueueRefreshPipelineRequestStorage(
        legacyContextStorage(context)));
}

FFX_API bool ffxFrameInterpolationResourceIsNull(FfxApiResource resource)
{
    return resource.resource == nullptr;
}

FFX_API FfxErrorCode ffxFrameInterpolationPrepare(
    FfxFrameInterpolationContext* context,
    const FfxFrameInterpolationPrepareDescription* params)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(params, FFX_ERROR_INVALID_POINTER);

    const ApusrFrameInterpolationPrepareDescription nativeParams =
        apusrFrameInterpolationPrepareDescriptionFromFfx(*params);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::frameInterpolationPrepareStorage(
        legacyContextStorage(context), &nativeParams));
}

FFX_API FfxErrorCode ffxFrameInterpolationDispatch(
    FfxFrameInterpolationContext* context,
    const FfxFrameInterpolationDispatchDescription* params)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(params, FFX_ERROR_INVALID_POINTER);

    const ApusrFrameInterpolationDispatchDescription nativeParams =
        apusrFrameInterpolationDispatchDescriptionFromFfx(*params);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::frameInterpolationDispatchStorage(
        legacyContextStorage(context), &nativeParams));
}

FFX_API FfxErrorCode ffxFrameInterpolationSetGlobalDebugMessage(ffxMessageCallback callback, uint32_t debugLevel)
{
    ffxSetPrintMessageCallback(callback, debugLevel);
    return FFX_OK;
}
