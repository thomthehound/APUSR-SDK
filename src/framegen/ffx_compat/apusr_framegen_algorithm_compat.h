// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "algorithm/apusr_framegen_algorithm_types.h"
#include "algorithm/include/ffx_frameinterpolation.h"
#include "algorithm/include/ffx_opticalflow.h"
#include "ffx_compat/apusr_framegen_resource_compat.h"

#include <cstring>

#define APUSR_ASSERT_FFX_RESULT_VALUE(apusrResult, ffxResult) \
    static_assert(static_cast<std::uint32_t>(apusrResult) == static_cast<std::uint32_t>(ffxResult))

APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_OK, FFX_OK);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INVALID_POINTER, FFX_ERROR_INVALID_POINTER);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INVALID_ALIGNMENT, FFX_ERROR_INVALID_ALIGNMENT);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INVALID_SIZE, FFX_ERROR_INVALID_SIZE);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INVALID_PATH, FFX_ERROR_INVALID_PATH);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY, FFX_ERROR_OUT_OF_MEMORY);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE, FFX_ERROR_INCOMPLETE_INTERFACE);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, FFX_ERROR_INVALID_ARGUMENT);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE, FFX_ERROR_OUT_OF_RANGE);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_NULL_DEVICE, FFX_ERROR_NULL_DEVICE);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_BACKEND_API, FFX_ERROR_BACKEND_API_ERROR);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INSUFFICIENT_MEMORY, FFX_ERROR_INSUFFICIENT_MEMORY);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_INVALID_VERSION, FFX_ERROR_INVALID_VERSION);
APUSR_ASSERT_FFX_RESULT_VALUE(APUSR_FRAMEGEN_ERROR_ACCESS_DENIED, FFX_ERROR_ACCESS_DENIED);

#undef APUSR_ASSERT_FFX_RESULT_VALUE

inline FfxErrorCode apusrFramegenResultToFfx(ApusrFramegenResult result) noexcept
{
    return static_cast<FfxErrorCode>(result);
}

#define APUSR_ASSERT_FFX_PASS_VALUE(apusrPass, ffxPass) \
    static_assert(static_cast<ApusrFramegenPass>(apusrPass) == static_cast<ApusrFramegenPass>(ffxPass))

APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE, FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_SETUP, FFX_FRAMEINTERPOLATION_PASS_SETUP);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH, FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD, FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD, FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK, FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_INTERPOLATION, FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID, FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_INPAINTING, FFX_FRAMEINTERPOLATION_PASS_INPAINTING);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID, FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_DEBUG_VIEW, FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_FRAMEINTERPOLATION_PASS_COUNT, FFX_FRAMEINTERPOLATION_PASS_COUNT);

APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_PREPARE_LUMA, FFX_OPTICALFLOW_PASS_PREPARE_LUMA);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID, FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM, FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE, FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5, FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5, FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5, FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5);
APUSR_ASSERT_FFX_PASS_VALUE(APUSR_OPTICALFLOW_PASS_COUNT, FFX_OPTICALFLOW_PASS_COUNT);

#undef APUSR_ASSERT_FFX_PASS_VALUE

inline ApusrFramegenDimensions2D apusrFramegenDimensionsFromFfx(FfxApiDimensions2D value) noexcept
{
    return {value.width, value.height};
}

inline ApusrFramegenFloatCoords2D apusrFramegenFloatCoordsFromFfx(FfxApiFloatCoords2D value) noexcept
{
    return {value.x, value.y};
}

inline ApusrFramegenRect2D apusrFramegenRectFromFfx(FfxApiRect2D value) noexcept
{
    return {value.left, value.top, value.width, value.height};
}

inline ApusrOpticalFlowContextDescription apusrOpticalFlowContextDescriptionFromFfx(const FfxOpticalflowContextDescription& value) noexcept
{
    ApusrOpticalFlowContextDescription result{};
    result.backendInterface = value.backendInterface;
    result.flags = value.flags;
    result.resolution = apusrFramegenDimensionsFromFfx(value.resolution);
    return result;
}

inline ApusrOpticalFlowDispatchDescription apusrOpticalFlowDispatchDescriptionFromFfx(const FfxOpticalflowDispatchDescription& value) noexcept
{
    ApusrOpticalFlowDispatchDescription result{};
    result.commandList = value.commandList;
    result.color = apusrFramegenResourceFromFfx(value.color);
    result.opticalFlowVector = apusrFramegenResourceFromFfx(value.opticalFlowVector);
    result.opticalFlowSCD = apusrFramegenResourceFromFfx(value.opticalFlowSCD);
    result.reset = value.reset;
    result.backbufferTransferFunction =
        static_cast<ApusrFramegenBackbufferTransferFunction>(value.backbufferTransferFunction);
    result.minMaxLuminance = apusrFramegenFloatCoordsFromFfx(value.minMaxLuminance);
    return result;
}

inline ApusrFrameInterpolationContextDescription apusrFrameInterpolationContextDescriptionFromFfx(const FfxFrameInterpolationContextDescription& value) noexcept
{
    ApusrFrameInterpolationContextDescription result{};
    result.flags = value.flags;
    result.maxRenderSize = apusrFramegenDimensionsFromFfx(value.maxRenderSize);
    result.displaySize = apusrFramegenDimensionsFromFfx(value.displaySize);
    result.backBufferFormat = static_cast<ApusrFramegenSurfaceFormat>(value.backBufferFormat);
    result.previousInterpolationSourceFormat = static_cast<ApusrFramegenSurfaceFormat>(value.previousInterpolationSourceFormat);
    result.backendInterface = value.backendInterface;
    return result;
}

inline ApusrFrameInterpolationPrepareDescription apusrFrameInterpolationPrepareDescriptionFromFfx(const FfxFrameInterpolationPrepareDescription& value) noexcept
{
    ApusrFrameInterpolationPrepareDescription result{};
    result.flags = value.flags;
    result.commandList = value.commandList;
    result.renderSize = apusrFramegenDimensionsFromFfx(value.renderSize);
    result.jitterOffset = apusrFramegenFloatCoordsFromFfx(value.jitterOffset);
    result.motionVectorScale = apusrFramegenFloatCoordsFromFfx(value.motionVectorScale);
    result.frameTimeDelta = value.frameTimeDelta;
    result.cameraNear = value.cameraNear;
    result.cameraFar = value.cameraFar;
    result.viewSpaceToMetersFactor = value.viewSpaceToMetersFactor;
    result.cameraFovAngleVertical = value.cameraFovAngleVertical;
    result.depth = apusrFramegenResourceFromFfx(value.depth);
    result.motionVectors = apusrFramegenResourceFromFfx(value.motionVectors);
    result.frameID = value.frameID;
    result.dilatedDepth = apusrFramegenResourceFromFfx(value.dilatedDepth);
    result.dilatedMotionVectors = apusrFramegenResourceFromFfx(value.dilatedMotionVectors);
    result.reconstructedPrevDepth = apusrFramegenResourceFromFfx(value.reconstructedPrevDepth);
    std::memcpy(result.cameraPosition, value.cameraPosition, sizeof(result.cameraPosition));
    std::memcpy(result.cameraUp, value.cameraUp, sizeof(result.cameraUp));
    std::memcpy(result.cameraRight, value.cameraRight, sizeof(result.cameraRight));
    std::memcpy(result.cameraForward, value.cameraForward, sizeof(result.cameraForward));
    return result;
}

inline ApusrFrameInterpolationDispatchDescription apusrFrameInterpolationDispatchDescriptionFromFfx(const FfxFrameInterpolationDispatchDescription& value) noexcept
{
    ApusrFrameInterpolationDispatchDescription result{};
    result.flags = value.flags;
    result.commandList = value.commandList;
    result.displaySize = apusrFramegenDimensionsFromFfx(value.displaySize);
    result.renderSize = apusrFramegenDimensionsFromFfx(value.renderSize);
    result.currentBackBuffer = apusrFramegenResourceFromFfx(value.currentBackBuffer);
    result.currentBackBufferHudless = apusrFramegenResourceFromFfx(value.currentBackBuffer_HUDLess);
    result.output = apusrFramegenResourceFromFfx(value.output);
    result.interpolationRect = apusrFramegenRectFromFfx(value.interpolationRect);
    result.opticalFlowVector = apusrFramegenResourceFromFfx(value.opticalFlowVector);
    result.opticalFlowSceneChangeDetection = apusrFramegenResourceFromFfx(value.opticalFlowSceneChangeDetection);
    result.opticalFlowBufferSize = apusrFramegenDimensionsFromFfx(value.opticalFlowBufferSize);
    result.opticalFlowScale = apusrFramegenFloatCoordsFromFfx(value.opticalFlowScale);
    result.opticalFlowBlockSize = value.opticalFlowBlockSize;
    result.cameraNear = value.cameraNear;
    result.cameraFar = value.cameraFar;
    result.cameraFovAngleVertical = value.cameraFovAngleVertical;
    result.viewSpaceToMetersFactor = value.viewSpaceToMetersFactor;
    result.frameTimeDelta = value.frameTimeDelta;
    result.reset = value.reset;
    result.backBufferTransferFunction = static_cast<ApusrFramegenBackbufferTransferFunction>(value.backBufferTransferFunction);
    result.minMaxLuminance[0] = value.minMaxLuminance[0];
    result.minMaxLuminance[1] = value.minMaxLuminance[1];
    result.frameID = value.frameID;
    result.dilatedDepth = apusrFramegenResourceFromFfx(value.dilatedDepth);
    result.dilatedMotionVectors = apusrFramegenResourceFromFfx(value.dilatedMotionVectors);
    result.reconstructedPrevDepth = apusrFramegenResourceFromFfx(value.reconstructedPrevDepth);
    result.distortionField = apusrFramegenResourceFromFfx(value.distortionField);
    return result;
}
