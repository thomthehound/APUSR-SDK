// APUSR-maintained Frame Generation algorithm contract derived from the FidelityFX SDK.
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

#pragma once

#include "apusr_framegen_backend_interface.h"
#include "apusr_framegen_backend_types.h"
#include "apusr_framegen_core_types.h"
#include "apusr_framegen_resource_types.h"

#include <cstddef>
#include <cstdint>


inline constexpr std::size_t APUSR_FRAMEINTERPOLATION_CONTEXT_STORAGE_SIZE = 32u * 1024u;
inline constexpr std::size_t APUSR_OPTICALFLOW_CONTEXT_STORAGE_SIZE = 16u * 1024u;

struct alignas(std::uint64_t) ApusrFrameInterpolationContext {
    std::uint8_t data[APUSR_FRAMEINTERPOLATION_CONTEXT_STORAGE_SIZE]{};
};

struct alignas(std::uint64_t) ApusrOpticalFlowContext {
    std::uint8_t data[APUSR_OPTICALFLOW_CONTEXT_STORAGE_SIZE]{};
};

enum ApusrFrameInterpolationPass : ApusrFramegenPass {
    APUSR_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE = 0,
    APUSR_FRAMEINTERPOLATION_PASS_SETUP,
    APUSR_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH,
    APUSR_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD,
    APUSR_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD,
    APUSR_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK,
    APUSR_FRAMEINTERPOLATION_PASS_INTERPOLATION,
    APUSR_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID,
    APUSR_FRAMEINTERPOLATION_PASS_INPAINTING,
    APUSR_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID,
    APUSR_FRAMEINTERPOLATION_PASS_DEBUG_VIEW,
    APUSR_FRAMEINTERPOLATION_PASS_COUNT,
};

enum ApusrOpticalFlowPass : ApusrFramegenPass {
    APUSR_OPTICALFLOW_PASS_PREPARE_LUMA = 0,
    APUSR_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID,
    APUSR_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM,
    APUSR_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE,
    APUSR_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5,
    APUSR_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5,
    APUSR_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5,
    APUSR_OPTICALFLOW_PASS_COUNT,
};

enum ApusrFrameInterpolationInitializationFlagBits : std::uint32_t {
    APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED = (1u << 0),
    APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE = (1u << 1),
    APUSR_FRAMEINTERPOLATION_ENABLE_TEXTURE1D_USAGE = (1u << 2),
    APUSR_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT = (1u << 3),
    APUSR_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1u << 4),
    APUSR_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS = (1u << 5),
    APUSR_FRAMEINTERPOLATION_ENABLE_ASYNC_SUPPORT = (1u << 6),
    APUSR_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING = (1u << 7),
    // The provider owns a ping-pong interpolation-source history and supplies
    // the previous source explicitly at dispatch. This removes the algorithm's
    // internal full-resolution end-of-dispatch history copy.
    APUSR_FRAMEINTERPOLATION_ENABLE_EXTERNAL_INTERPOLATION_HISTORY = (1u << 8),
};

enum ApusrFrameInterpolationDispatchFlagBits : std::uint32_t {
    APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES = (1u << 0),
    APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS = (1u << 1),
    APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW = (1u << 2),
    APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_PACING_LINES = (1u << 3),
    APUSR_FRAMEINTERPOLATION_DISPATCH_RESERVED_1 = (1u << 4),
    APUSR_FRAMEINTERPOLATION_DISPATCH_RESERVED_2 = (1u << 5),
};

enum ApusrOpticalFlowInitializationFlagBits : std::uint32_t {
    APUSR_OPTICALFLOW_ENABLE_TEXTURE1D_USAGE = (1u << 0),
};

struct ApusrOpticalFlowContextDescription {
    ApusrFramegenBackendInterface backendInterface{};
    std::uint32_t flags = 0;
    ApusrFramegenDimensions2D resolution{};
};

struct ApusrOpticalFlowDispatchDescription {
    ApusrFramegenCommandList commandList = nullptr;
    ApusrFramegenExternalResource color{};
    ApusrFramegenExternalResource opticalFlowVector{};
    ApusrFramegenExternalResource opticalFlowSCD{};
    bool reset = false;
    ApusrFramegenBackbufferTransferFunction backbufferTransferFunction = APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    ApusrFramegenFloatCoords2D minMaxLuminance{};
};

struct ApusrOpticalFlowSharedResourceDescriptions {
    ApusrFramegenCreateResourceDescription opticalFlowVector{};
    ApusrFramegenCreateResourceDescription opticalFlowSCD{};
};

struct ApusrOpticalFlowInternalResourceDescriptions {
    ApusrFramegenCreateResourceDescription opticalFlowInput1{};
    ApusrFramegenCreateResourceDescription opticalFlowInput1Level1{};
    ApusrFramegenCreateResourceDescription opticalFlowInput1Level2{};
    ApusrFramegenCreateResourceDescription opticalFlowInput1Level3{};
    ApusrFramegenCreateResourceDescription opticalFlowInput1Level4{};
    ApusrFramegenCreateResourceDescription opticalFlowInput1Level5{};
    ApusrFramegenCreateResourceDescription opticalFlowInput1Level6{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2Level1{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2Level2{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2Level3{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2Level4{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2Level5{};
    ApusrFramegenCreateResourceDescription opticalFlowInput2Level6{};
    ApusrFramegenCreateResourceDescription opticalFlow1{};
    ApusrFramegenCreateResourceDescription opticalFlow1Level1{};
    ApusrFramegenCreateResourceDescription opticalFlow1Level2{};
    ApusrFramegenCreateResourceDescription opticalFlow1Level3{};
    ApusrFramegenCreateResourceDescription opticalFlow1Level4{};
    ApusrFramegenCreateResourceDescription opticalFlow1Level5{};
    ApusrFramegenCreateResourceDescription opticalFlow1Level6{};
    ApusrFramegenCreateResourceDescription opticalFlow2{};
    ApusrFramegenCreateResourceDescription opticalFlow2Level1{};
    ApusrFramegenCreateResourceDescription opticalFlow2Level2{};
    ApusrFramegenCreateResourceDescription opticalFlow2Level3{};
    ApusrFramegenCreateResourceDescription opticalFlow2Level4{};
    ApusrFramegenCreateResourceDescription opticalFlow2Level5{};
    ApusrFramegenCreateResourceDescription opticalFlow2Level6{};
    ApusrFramegenCreateResourceDescription opticalFlowSCDHistogram{};
    ApusrFramegenCreateResourceDescription opticalFlowSCDPreviousHistogram{};
    ApusrFramegenCreateResourceDescription opticalFlowSCDTemp{};
};

struct ApusrFrameInterpolationContextDescription {
    std::uint32_t flags = 0;
    ApusrFramegenDimensions2D maxRenderSize{};
    ApusrFramegenDimensions2D displaySize{};
    ApusrFramegenSurfaceFormat backBufferFormat = APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    ApusrFramegenSurfaceFormat previousInterpolationSourceFormat = APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    ApusrFramegenBackendInterface backendInterface{};
};

struct ApusrFrameInterpolationRenderDescription {
    ApusrFramegenDimensions2D renderSize{};
    ApusrFramegenDimensions2D upscaleSize{};
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFovAngleVertical = 0.0f;
    float viewSpaceToMetersFactor = 0.0f;
    ApusrFramegenFloatCoords2D motionVectorScale{};
};

struct ApusrFrameInterpolationPrepareDescription {
    std::uint32_t flags = 0;
    ApusrFramegenCommandList commandList = nullptr;
    ApusrFramegenDimensions2D renderSize{};
    ApusrFramegenFloatCoords2D jitterOffset{};
    ApusrFramegenFloatCoords2D motionVectorScale{};
    float frameTimeDelta = 0.0f;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float viewSpaceToMetersFactor = 0.0f;
    float cameraFovAngleVertical = 0.0f;
    ApusrFramegenExternalResource depth{};
    ApusrFramegenExternalResource motionVectors{};
    std::uint64_t frameID = 0;
    bool reset = false;
    ApusrFramegenExternalResource dilatedDepth{};
    ApusrFramegenExternalResource dilatedMotionVectors{};
    ApusrFramegenExternalResource reconstructedPrevDepth{};
    // Optional provider-owned interpolation history ingest. When both are
    // supplied, Prepare copies interpolationSource into retainedInterpolationSource
    // while the transient source is still valid.
    ApusrFramegenExternalResource interpolationSource{};
    ApusrFramegenExternalResource retainedInterpolationSource{};
    // Optional transient distortion retention. Distortion is consumed by Phase B,
    // so split-queue frontends can copy it once during Prepare and release the
    // client-owned image with the rest of the input contract.
    ApusrFramegenExternalResource distortionField{};
    ApusrFramegenExternalResource retainedDistortionField{};
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
};

struct ApusrFrameInterpolationDispatchDescription {
    std::uint32_t flags = 0;
    ApusrFramegenCommandList commandList = nullptr;
    ApusrFramegenDimensions2D displaySize{};
    ApusrFramegenDimensions2D renderSize{};
    ApusrFramegenExternalResource currentBackBuffer{};
    ApusrFramegenExternalResource currentBackBufferHudless{};
    ApusrFramegenExternalResource output{};
    ApusrFramegenRect2D interpolationRect{};
    ApusrFramegenExternalResource opticalFlowVector{};
    ApusrFramegenExternalResource opticalFlowSceneChangeDetection{};
    ApusrFramegenDimensions2D opticalFlowBufferSize{};
    ApusrFramegenFloatCoords2D opticalFlowScale{};
    int opticalFlowBlockSize = 0;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFovAngleVertical = 0.0f;
    float viewSpaceToMetersFactor = 0.0f;
    float frameTimeDelta = 0.0f;
    bool reset = false;
    ApusrFramegenBackbufferTransferFunction backBufferTransferFunction = APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    float minMaxLuminance[2]{};
    std::uint64_t frameID = 0;
    ApusrFramegenExternalResource dilatedDepth{};
    ApusrFramegenExternalResource dilatedMotionVectors{};
    ApusrFramegenExternalResource reconstructedPrevDepth{};
    ApusrFramegenExternalResource distortionField{};
    // Explicit previous interpolation source for providers that own history.
    // Required when EXTERNAL_INTERPOLATION_HISTORY was enabled at context create.
    ApusrFramegenExternalResource previousInterpolationSource{};
};

struct ApusrFrameInterpolationSharedResourceDescriptions {
    ApusrFramegenCreateResourceDescription reconstructedPrevNearestDepth{};
    ApusrFramegenCreateResourceDescription dilatedDepth{};
    ApusrFramegenCreateResourceDescription dilatedMotionVectors{};
    // Valid only for contexts created with EXTERNAL_INTERPOLATION_HISTORY.
    ApusrFramegenCreateResourceDescription interpolationSource{};
};

struct ApusrFrameInterpolationInternalResourceDescriptions {
    ApusrFramegenCreateResourceDescription reconstructedDepthInterpolatedFrame{};
    ApusrFramegenCreateResourceDescription gameMotionVectorFieldX{};
    ApusrFramegenCreateResourceDescription gameMotionVectorFieldY{};
    ApusrFramegenCreateResourceDescription inpaintingPyramid{};
    ApusrFramegenCreateResourceDescription counters{};
    ApusrFramegenCreateResourceDescription opticalFlowMotionVectorFieldX{};
    ApusrFramegenCreateResourceDescription opticalFlowMotionVectorFieldY{};
    ApusrFramegenCreateResourceDescription previousInterpolationSource{};
    ApusrFramegenCreateResourceDescription inpaintingMask{};
    ApusrFramegenCreateResourceDescription disocclusionMask{};
    ApusrFramegenCreateResourceDescription defaultDistortionField{};
};

// Native APUSR algorithm entry points. The compact context storage is independent
// of the much larger opaque buffers reserved by the external FidelityFX ABI.
ApusrFramegenResult apusrOpticalFlowContextCreate(ApusrOpticalFlowContext* context, const ApusrOpticalFlowContextDescription* description) noexcept;
ApusrFramegenResult apusrOpticalFlowContextDestroy(ApusrOpticalFlowContext* context) noexcept;
ApusrFramegenResult apusrOpticalFlowContextGetGpuMemoryUsage(ApusrOpticalFlowContext* context, ApusrFramegenMemoryUsage* usage) noexcept;
ApusrFramegenResult apusrOpticalFlowGetGpuMemoryUsage(ApusrFramegenDevice device, ApusrFramegenDimensions2D displaySize, ApusrFramegenMemoryUsage* usage) noexcept;
ApusrFramegenResult apusrOpticalFlowGetSharedResourceDescriptions(ApusrOpticalFlowContext* context, ApusrOpticalFlowSharedResourceDescriptions* descriptions) noexcept;
ApusrFramegenResult apusrOpticalFlowDispatch(ApusrOpticalFlowContext* context, const ApusrOpticalFlowDispatchDescription* description) noexcept;

ApusrFramegenResult apusrFrameInterpolationContextCreate(ApusrFrameInterpolationContext* context, const ApusrFrameInterpolationContextDescription* description) noexcept;
ApusrFramegenResult apusrFrameInterpolationContextDestroy(ApusrFrameInterpolationContext* context) noexcept;
ApusrFramegenResult apusrFrameInterpolationContextGetGpuMemoryUsage(ApusrFrameInterpolationContext* context, ApusrFramegenMemoryUsage* usage) noexcept;
ApusrFramegenResult apusrFrameInterpolationGetGpuMemoryUsage(
    ApusrFramegenDevice device,
    ApusrFramegenDimensions2D maxRenderSize,
    ApusrFramegenDimensions2D displaySize,
    ApusrFramegenSurfaceFormat previousInterpolationSourceFormat,
    ApusrFramegenMemoryUsage* usage) noexcept;
ApusrFramegenResult apusrFrameInterpolationGetSharedResourceDescriptions(ApusrFrameInterpolationContext* context, ApusrFrameInterpolationSharedResourceDescriptions* descriptions) noexcept;
ApusrFramegenResult apusrFrameInterpolationPrepare(ApusrFrameInterpolationContext* context, const ApusrFrameInterpolationPrepareDescription* description) noexcept;
ApusrFramegenResult apusrFrameInterpolationDispatch(ApusrFrameInterpolationContext* context, const ApusrFrameInterpolationDispatchDescription* description) noexcept;
