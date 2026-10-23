// APUSR-maintained FidelityFX Optical Flow compatibility facade.
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

#include "algorithm/include/ffx_opticalflow.h"
#include "algorithm/internal/apusr_framegen_algorithm_storage.h"
#include "ffx_compat/apusr_framegen_algorithm_compat.h"

#include <cstddef>
#include <cstdint>

namespace {

static_assert(sizeof(FfxOpticalflowContext) >=
    APUSR_OPTICALFLOW_CONTEXT_STORAGE_SIZE + alignof(ApusrOpticalFlowContext) - 1);


void* legacyContextStorage(FfxOpticalflowContext* context) noexcept
{
    auto address = reinterpret_cast<std::uintptr_t>(context->data);
    constexpr std::uintptr_t alignment = alignof(ApusrOpticalFlowContext);
    address = (address + alignment - 1u) & ~(alignment - 1u);
    return reinterpret_cast<void*>(address);
}

} // namespace

FFX_API FfxErrorCode ffxOpticalflowContextCreate(
    FfxOpticalflowContext* context,
    FfxOpticalflowContextDescription* contextDescription)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(contextDescription, FFX_ERROR_INVALID_POINTER);

    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.getVersion, FFX_ERROR_INCOMPLETE_INTERFACE);
    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.getDeviceCapabilities, FFX_ERROR_INCOMPLETE_INTERFACE);
    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.createContext, FFX_ERROR_INCOMPLETE_INTERFACE);
    FFX_RETURN_ON_ERROR(contextDescription->backendInterface.destroyContext, FFX_ERROR_INCOMPLETE_INTERFACE);
    if (contextDescription->backendInterface.scratchBuffer)
        FFX_RETURN_ON_ERROR(contextDescription->backendInterface.scratchBufferSize, FFX_ERROR_INCOMPLETE_INTERFACE);

    const ApusrOpticalFlowContextDescription nativeDescription =
        apusrOpticalFlowContextDescriptionFromFfx(*contextDescription);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::opticalFlowContextCreateStorage(
        legacyContextStorage(context), &nativeDescription));
}

FFX_API FfxErrorCode ffxOpticalflowContextGetGpuMemoryUsage(
    FfxOpticalflowContext* context,
    FfxApiEffectMemoryUsage* vramUsage)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(vramUsage, FFX_ERROR_INVALID_POINTER);

    ApusrFramegenMemoryUsage nativeUsage{};
    const ApusrFramegenResult result = apusr::framegen::algorithm::opticalFlowContextGetGpuMemoryUsageStorage(
        legacyContextStorage(context), &nativeUsage);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    vramUsage->totalUsageInBytes = nativeUsage.totalUsageInBytes;
    vramUsage->aliasableUsageInBytes = nativeUsage.aliasableUsageInBytes;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxOpticalflowGetGpuMemoryUsage(
    FfxDevice device,
    FfxApiDimensions2D* displaySize,
    FfxApiEffectMemoryUsage* vramUsage)
{
    FFX_RETURN_ON_ERROR(displaySize, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(vramUsage, FFX_ERROR_INVALID_POINTER);

    ApusrFramegenMemoryUsage nativeUsage{};
    const ApusrFramegenResult result = apusrOpticalFlowGetGpuMemoryUsage(
        static_cast<ApusrFramegenDevice>(device),
        apusrFramegenDimensionsFromFfx(*displaySize),
        &nativeUsage);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    vramUsage->totalUsageInBytes += nativeUsage.totalUsageInBytes;
    vramUsage->aliasableUsageInBytes += nativeUsage.aliasableUsageInBytes;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxOpticalflowContextDestroy(FfxOpticalflowContext* context)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::opticalFlowContextDestroyStorage(legacyContextStorage(context)));
}

FFX_API bool ffxOpticalflowResourceIsNull(FfxApiResource resource)
{
    return resource.resource == nullptr;
}

FFX_API FfxErrorCode ffxOpticalflowGetSharedResourceDescriptions(
    FfxOpticalflowContext* context,
    FfxOpticalflowSharedResourceDescriptions* sharedResources)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(sharedResources, FFX_ERROR_INVALID_POINTER);

    ApusrOpticalFlowSharedResourceDescriptions nativeDescriptions{};
    const ApusrFramegenResult result = apusr::framegen::algorithm::opticalFlowGetSharedResourceDescriptionsStorage(
        legacyContextStorage(context), &nativeDescriptions);
    FFX_RETURN_ON_ERROR(result == APUSR_FRAMEGEN_OK, apusrFramegenResultToFfx(result));

    sharedResources->opticalFlowVector = nativeDescriptions.opticalFlowVector;
    sharedResources->opticalFlowSCD = nativeDescriptions.opticalFlowSCD;
    return FFX_OK;
}

FFX_API FfxErrorCode ffxOpticalflowContextDispatch(
    FfxOpticalflowContext* context,
    const FfxOpticalflowDispatchDescription* dispatchParams)
{
    FFX_RETURN_ON_ERROR(context, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(dispatchParams, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(dispatchParams->commandList, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(dispatchParams->color.resource, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(dispatchParams->color.description.type == FFX_API_RESOURCE_TYPE_TEXTURE2D, FFX_ERROR_INVALID_ARGUMENT);
    FFX_RETURN_ON_ERROR(dispatchParams->opticalFlowVector.resource, FFX_ERROR_INVALID_POINTER);
    FFX_RETURN_ON_ERROR(dispatchParams->opticalFlowSCD.resource, FFX_ERROR_INVALID_POINTER);

    const ApusrOpticalFlowDispatchDescription nativeDispatch =
        apusrOpticalFlowDispatchDescriptionFromFfx(*dispatchParams);
    return apusrFramegenResultToFfx(apusr::framegen::algorithm::opticalFlowDispatchStorage(
        legacyContextStorage(context), &nativeDispatch));
}
