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

#include <algorithm>    // for max used inside SPD CPU code.
#include <cstring>
#include <cwchar>
#include <iterator>
#include <cmath>        // for fabs, abs, sinf, sqrt, etc.
#include <cfloat>       // for FLT_EPSILON
#include "ffx_assert.h"
#include "ffx_message.h"

#define FFX_CPU

#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4838)
#endif

#include "gpu/ffx_core.h"
#include "gpu/spd/ffx_spd.h"
#include "ffx_frameinterpolation_private.h"

static_assert(sizeof(ApusrFrameInterpolationContextPrivate) <= APUSR_FRAMEINTERPOLATION_CONTEXT_STORAGE_SIZE,
    "APUSR Frame Interpolation context storage is too small for the private context.");
static_assert(alignof(ApusrFrameInterpolationContextPrivate) <= alignof(ApusrFrameInterpolationContext),
    "APUSR Frame Interpolation context storage alignment is insufficient.");
#include "ffx_frameinterpolation_shaderblobs.h"
#include "apusr_framegen_backend.h"
#include "internal/apusr_framegen_algorithm_storage.h"
#include "internal/apusr_frameinterpolation_jitter.h"

namespace {
template <std::size_t Size>
void copyWideName(wchar_t (&destination)[Size], const wchar_t* source) noexcept
{
    if (!source) {
        destination[0] = L'\0';
        return;
    }

    const std::size_t count = std::min<std::size_t>(Size - 1, std::wcslen(source));
    std::copy_n(source, count, destination);
    destination[count] = L'\0';
}
} // namespace


// lists to map shader resource bindpoint name to resource identifier
typedef struct ResourceBinding
{
    uint32_t    index;
    const char* name;
}ResourceBinding;

static const ResourceBinding srvResourceBindingTable[] =
{
    // Frame Interpolation textures
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEPTH,                                      "r_input_depth"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_MOTION_VECTORS,                             "r_input_motion_vectors"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISTORTION_FIELD,                           "r_input_distortion_field"},

    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH,                              "r_dilated_depth"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS,                     "r_dilated_motion_vectors"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME,         "r_reconstructed_depth_previous_frame"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME,     "r_reconstructed_depth_interpolated_frame"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PREVIOUS_INTERPOLATION_SOURCE,              "r_previous_interpolation_source"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE,               "r_current_interpolation_source"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISOCCLUSION_MASK,                          "r_disocclusion_mask"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_X,                 "r_game_motion_vector_field_x"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_Y,                 "r_game_motion_vector_field_y"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X,         "r_optical_flow_motion_vector_field_x"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y,         "r_optical_flow_motion_vector_field_y"},

    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_VECTOR,                        "r_optical_flow"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_CONFIDENCE,                    "r_optical_flow_confidence"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_GLOBAL_MOTION,                 "r_optical_flow_global_motion"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCENE_CHANGE_DETECTION,        "r_optical_flow_scd"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT,                                     "r_output"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_MASK,                            "r_inpainting_mask"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID,                         "r_inpainting_pyramid"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PRESENT_BACKBUFFER,                         "r_present_backbuffer"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNTERS,                                   "r_counters"},
};

static const ResourceBinding uavResourceBindingTable[] =
{
    // Frame Interpolation textures
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH,                              "rw_dilated_depth"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS,                     "rw_dilated_motion_vectors"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME,         "rw_reconstructed_depth_previous_frame"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME,     "rw_reconstructed_depth_interpolated_frame"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT,                                     "rw_output"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISOCCLUSION_MASK,                          "rw_disocclusion_mask"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_X,                 "rw_game_motion_vector_field_x"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_Y,                 "rw_game_motion_vector_field_y"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X,         "rw_optical_flow_motion_vector_field_x"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y,         "rw_optical_flow_motion_vector_field_y"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_MASK,                            "rw_inpainting_mask"},

    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNTERS,                                   "rw_counters"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_0,                "rw_inpainting_pyramid0"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_1,                "rw_inpainting_pyramid1"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_2,                "rw_inpainting_pyramid2"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_3,                "rw_inpainting_pyramid3"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_4,                "rw_inpainting_pyramid4"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_5,                "rw_inpainting_pyramid5"}, // extra declaration, as this is globallycoherent
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_6,                "rw_inpainting_pyramid6"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_7,                "rw_inpainting_pyramid7"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_8,                "rw_inpainting_pyramid8"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_9,                "rw_inpainting_pyramid9"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_10,               "rw_inpainting_pyramid10"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_11,               "rw_inpainting_pyramid11"},
    {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_12,               "rw_inpainting_pyramid12"},
};

static const ResourceBinding cbResourceBindingTable[] =
{
    {FFX_FRAMEINTERPOLATION_CONSTANTBUFFER_IDENTIFIER,                                      "cbFI"},
    {FFX_FRAMEINTERPOLATION_INPAINTING_PYRAMID_CONSTANTBUFFER_IDENTIFIER,                   "cbInpaintingPyramid"},
};

// Broad structure of the root signature.
typedef enum FrameInterpolationRootSignatureLayout {

    FRAMEINTERPOLATION_ROOT_SIGNATURE_LAYOUT_UAVS,
    FRAMEINTERPOLATION_ROOT_SIGNATURE_LAYOUT_SRVS,
    FRAMEINTERPOLATION_ROOT_SIGNATURE_LAYOUT_CONSTANTS,
    FRAMEINTERPOLATION_ROOT_SIGNATURE_LAYOUT_CONSTANTS_REGISTER_1,
    FRAMEINTERPOLATION_ROOT_SIGNATURE_LAYOUT_PARAMETER_COUNT
} FrameInterpolationRootSignatureLayout;

typedef union FrameInterpolationSecondaryUnion
{
    InpaintingPyramidConstants inpaintingPyramidConstants;
} FrameInterpolationSecondaryUnion;

// Lanczos
static float lanczos2(float value)
{
    return std::abs(value) < FFX_EPSILON ? 1.f : (sinf(FFX_PI * value) / (FFX_PI * value)) * (sinf(0.5f * FFX_PI * value) / (0.5f * FFX_PI * value));
}

// Calculate halton number for index and base.
static float halton(int32_t index, int32_t base)
{
    float f = 1.0f, result = 0.0f;

    for (int32_t currentIndex = index; currentIndex > 0;) {

        f /= (float)base;
        result = result + f * (float)(currentIndex % base);
        currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
    }

    return result;
}

static ApusrFramegenResult patchResourceBindings(ApusrFramegenPipeline* inoutPipeline)
{
    for (uint32_t srvIndex = 0; srvIndex < inoutPipeline->srvTextureCount; ++srvIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(srvResourceBindingTable); ++mapIndex)
        {
            if (0 == strcmp(srvResourceBindingTable[mapIndex].name, inoutPipeline->srvTextureBindings[srvIndex].name))
                break;
        }
        if (mapIndex == std::size(srvResourceBindingTable))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->srvTextureBindings[srvIndex].resourceIdentifier = srvResourceBindingTable[mapIndex].index;
    }

    // check for UAVs where mip chains are to be bound
    for (uint32_t uavIndex = 0; uavIndex < inoutPipeline->uavTextureCount; ++uavIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(uavResourceBindingTable); ++mapIndex)
        {
            if (0 == strcmp(uavResourceBindingTable[mapIndex].name, inoutPipeline->uavTextureBindings[uavIndex].name))
                break;
        }
        if (mapIndex == std::size(uavResourceBindingTable))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->uavTextureBindings[uavIndex].resourceIdentifier = uavResourceBindingTable[mapIndex].index;
    }

    for (uint32_t cbIndex = 0; cbIndex < inoutPipeline->constCount; ++cbIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(cbResourceBindingTable); ++mapIndex)
        {
            if (0 == strcmp(cbResourceBindingTable[mapIndex].name, inoutPipeline->constantBufferBindings[cbIndex].name))
                break;
        }
        if (mapIndex == std::size(cbResourceBindingTable))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->constantBufferBindings[cbIndex].resourceIdentifier = cbResourceBindingTable[mapIndex].index;
    }

    for (uint32_t uavBufferIndex = 0; uavBufferIndex < inoutPipeline->uavBufferCount; ++uavBufferIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(uavResourceBindingTable); ++mapIndex)
        {
            if (0 == strcmp(uavResourceBindingTable[mapIndex].name, inoutPipeline->uavBufferBindings[uavBufferIndex].name))
                break;
        }
        if (mapIndex == std::size(uavResourceBindingTable))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->uavBufferBindings[uavBufferIndex].resourceIdentifier = uavResourceBindingTable[mapIndex].index;
    }

    for (uint32_t srvBufferIndex = 0; srvBufferIndex < inoutPipeline->srvBufferCount; ++srvBufferIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(srvResourceBindingTable); ++mapIndex)
        {
            if (0 == strcmp(srvResourceBindingTable[mapIndex].name, inoutPipeline->srvBufferBindings[srvBufferIndex].name))
                break;
        }
        if (mapIndex == std::size(srvResourceBindingTable))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->srvBufferBindings[srvBufferIndex].resourceIdentifier = srvResourceBindingTable[mapIndex].index;
    }


    return APUSR_FRAMEGEN_OK;
}

static uint32_t getPipelinePermutationFlags(uint32_t contextFlags, ApusrFramegenPass, bool fp16, bool force64, bool)
{
    // work out what permutation to load.
    uint32_t flags = 0;
    flags |= (contextFlags & APUSR_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 0 : FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS;
    flags |= (contextFlags & APUSR_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS) ? FRAMEINTERPOLATION_SHADER_PERMUTATION_JITTER_MOTION_VECTORS : 0;
    flags |= (contextFlags & APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) ? FRAMEINTERPOLATION_SHADER_PERMUTATION_DEPTH_INVERTED : 0;
    flags |= (force64) ? FRAMEINTERPOLATION_SHADER_PERMUTATION_FORCE_WAVE64 : 0;
    flags |= (fp16) ? FRAMEINTERPOLATION_SHADER_PERMUTATION_ALLOW_FP16 : 0;
    return flags;
}

static ApusrFramegenResult createPipelineStates(ApusrFrameInterpolationContextPrivate* context)
{
    FFX_ASSERT(context);

    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);

    ApusrFramegenSamplerDescription samplerDescs[2] = {
        {APUSR_FRAMEGEN_FILTER_MINMAGMIP_POINT, APUSR_FRAMEGEN_ADDRESS_CLAMP, APUSR_FRAMEGEN_ADDRESS_CLAMP, APUSR_FRAMEGEN_ADDRESS_CLAMP},
        {APUSR_FRAMEGEN_FILTER_MINMAGMIP_LINEAR, APUSR_FRAMEGEN_ADDRESS_CLAMP, APUSR_FRAMEGEN_ADDRESS_CLAMP, APUSR_FRAMEGEN_ADDRESS_CLAMP},
    };
    ApusrFramegenPipelineDescription pipelineDescription{};
    pipelineDescription.samplerCount = std::size(samplerDescs);
    pipelineDescription.samplers = samplerDescs;

    const ApusrFramegenDeviceCapabilities& capabilities = context->deviceCapabilities;
    const bool haveShaderModel66 = capabilities.maximumSupportedShaderModel >= APUSR_FRAMEGEN_SHADER_MODEL_6_6;
    const bool supportedFP16 = capabilities.fp16Supported;
    const bool canForceWave64 = capabilities.waveLaneCountMin == 32 && capabilities.waveLaneCountMax == 64 && haveShaderModel66;
    const bool useLut = capabilities.waveLaneCountMin == 32 && capabilities.waveLaneCountMax == 64;
    const uint32_t contextFlags = context->contextDescription.flags;

    auto createComputePipeline = [&](ApusrFramegenPass pass, const wchar_t* name, ApusrFramegenPipeline* pipeline) -> ApusrFramegenResult {
        ApusrFramegenShaderBlob shaderBlob{};
        ApusrFramegenResult result = frameInterpolationGetPermutationBlobByIndex(
            pass,
            getPipelinePermutationFlags(contextFlags, pass, supportedFP16, canForceWave64, useLut),
            &shaderBlob);
        if (result != APUSR_FRAMEGEN_OK)
            return result;

        backendContext.releasePipeline(*pipeline);
        copyWideName(pipelineDescription.name, name);
        result = backendContext.createPipeline(shaderBlob, pipelineDescription, *pipeline);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
        return patchResourceBindings(pipeline);
    };

    struct PipelineSpec {
        ApusrFramegenPass pass;
        const wchar_t* name;
        ApusrFramegenPipeline* pipeline;
    };
    const PipelineSpec pipelines[] = {
        {APUSR_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE, L"RECONSTRUCT_AND_DILATE", &context->pipelineFiReconstructAndDilate},
        {APUSR_FRAMEINTERPOLATION_PASS_SETUP, L"SETUP", &context->pipelineFiSetup},
        {APUSR_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH, L"RECONSTRUCT_PREV_DEPTH", &context->pipelineFiReconstructPreviousDepth},
        {APUSR_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD, L"GAME_MOTION_VECTOR_FIELD", &context->pipelineFiGameMotionVectorField},
        {APUSR_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD, L"OPTICAL_FLOW_VECTOR_FIELD", &context->pipelineFiOpticalFlowVectorField},
        {APUSR_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK, L"DISOCCLUSION_MASK", &context->pipelineFiDisocclusionMask},
        {APUSR_FRAMEINTERPOLATION_PASS_INTERPOLATION, L"INTERPOLATION", &context->pipelineFiScfi},
        {APUSR_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID, L"INPAINTING_PYRAMID", &context->pipelineInpaintingPyramid},
        {APUSR_FRAMEINTERPOLATION_PASS_INPAINTING, L"INPAINTING", &context->pipelineInpainting},
        {APUSR_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID, L"GAME_VECTOR_FIELD_INPAINTING_PYRAMID", &context->pipelineGameVectorFieldInpaintingPyramid},
        {APUSR_FRAMEINTERPOLATION_PASS_DEBUG_VIEW, L"DEBUG_VIEW", &context->pipelineDebugView},
    };

    for (const PipelineSpec& pipeline : pipelines) {
        const ApusrFramegenResult result = createComputePipeline(pipeline.pass, pipeline.name, pipeline.pipeline);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
    }
    return APUSR_FRAMEGEN_OK;
}


// Format precision group for HUDless.
// Also format needs at least the 3 RGB channels to be valid
// int formats aren't accepted.
int GetFormatPrecisionGroup(ApusrFramegenSurfaceFormat format)
{
    switch (format)
    {
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32_FLOAT:
        return 0;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT:
        return 1;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_UNORM:
        return 2;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SNORM:
        return 3;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SRGB:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_SRGB:
        return 4;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R11G11B10_FLOAT:
        return 5;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_UNORM:
        return 6;

    case APUSR_FRAMEGEN_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP:
        return 7;

    // we don't accept the following formats
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_SNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN:
    default:
        return -1;
    }
}

static void getInternalResourceDescriptions(const ApusrFramegenDimensions2D* maxRenderSize, const ApusrFramegenDimensions2D* displaySize, ApusrFramegenSurfaceFormat previousInterpolationSourceFormat, ApusrFrameInterpolationInternalResourceDescriptions* internalResourceDescriptions)
{
    internalResourceDescriptions->reconstructedDepthInterpolatedFrame = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_ReconstructedDepthInterpolatedFrame", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->gameMotionVectorFieldX = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_GameMotionVectorFieldX", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_X, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->gameMotionVectorFieldY = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_GameMotionVectorFieldY", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_Y, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->inpaintingPyramid = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT, displaySize->width / 2, displaySize->height / 2, 1, 0, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_InpaintingPyramid", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->counters = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER, APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN, 8, 4, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_Counters", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNTERS, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->opticalFlowMotionVectorFieldX = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_OpticalFlowMotionVectorFieldX", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->opticalFlowMotionVectorFieldY = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_OpticalFlowMotionVectorFieldY", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->previousInterpolationSource = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, previousInterpolationSourceFormat, displaySize->width, displaySize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_PreviousInterpolationSouce", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PREVIOUS_INTERPOLATION_SOURCE, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->inpaintingMask = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM, displaySize->width, displaySize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_InpaintingMask", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_MASK, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    internalResourceDescriptions->disocclusionMask = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FI_DisocclusionMask", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISOCCLUSION_MASK, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };

    // Default data for resources with init data
    static uint8_t defaultDistortionFieldData[2] = { 0, 0 };
    internalResourceDescriptions->defaultDistortionField = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM, 1, 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY }, APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ, L"FI_DefaultDistortionField", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEFAULT_DISTORTION_FIELD, ApusrFramegenResourceInitData::fromBuffer(sizeof(defaultDistortionFieldData), defaultDistortionFieldData) };

}

static void getSharedResourceDescriptions(
    const ApusrFramegenDimensions2D* maxRenderSize,
    const ApusrFramegenDimensions2D* displaySize,
    const ApusrFramegenSurfaceFormat interpolationSourceFormat,
    const bool externalInterpolationHistory,
    ApusrFrameInterpolationSharedResourceDescriptions* sharedResourceDescriptions)
{
    *sharedResourceDescriptions = {};
    sharedResourceDescriptions->dilatedDepth = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, (APUSR_FRAMEGEN_RESOURCE_USAGE_RENDERTARGET | APUSR_FRAMEGEN_RESOURCE_USAGE_UAV | APUSR_FRAMEGEN_RESOURCE_USAGE_DCC_RENDERTARGET) },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FISHARED_DilatedDepth", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    sharedResourceDescriptions->dilatedMotionVectors = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, (APUSR_FRAMEGEN_RESOURCE_USAGE_RENDERTARGET | APUSR_FRAMEGEN_RESOURCE_USAGE_UAV | APUSR_FRAMEGEN_RESOURCE_USAGE_DCC_RENDERTARGET) },
            APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FISHARED_DilatedVelocity", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    sharedResourceDescriptions->reconstructedPrevNearestDepth = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, maxRenderSize->width, maxRenderSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, (APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) },
            APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"FISHARED_ReconstructedPrevNearestDepth", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    if (externalInterpolationHistory) {
        sharedResourceDescriptions->interpolationSource = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, interpolationSourceFormat, displaySize->width, displaySize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY },
            APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ, L"FISHARED_InterpolationSource", FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
    }
}

static ApusrFramegenResult frameinterpolationRelease(ApusrFrameInterpolationContextPrivate* context);

static ApusrFramegenResult frameinterpolationCreate(ApusrFrameInterpolationContextPrivate* context, const ApusrFrameInterpolationContextDescription* contextDescription)
{
    FFX_ASSERT(context);
    FFX_ASSERT(contextDescription);

    const int backBufferGroup = GetFormatPrecisionGroup(contextDescription->backBufferFormat);
    const int previousInterpolationSourceGroup = GetFormatPrecisionGroup(contextDescription->previousInterpolationSourceFormat);
    if (backBufferGroup < 0 || previousInterpolationSourceGroup < 0 || backBufferGroup != previousInterpolationSourceGroup)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    std::memset(context, 0, sizeof(ApusrFrameInterpolationContextPrivate));
    context->device = contextDescription->backendInterface.device;
    context->contextDescription = *contextDescription;

    if (apusr::framegen::backendVersion(context->contextDescription.backendInterface) != apusrFramegenMakeVersion(2, 0, 0))
        return APUSR_FRAMEGEN_ERROR_INVALID_VERSION;

    ApusrFramegenResult result = apusr::framegen::createEffectContext(
        context->contextDescription.backendInterface,
        context->effectContextId,
        context->deviceCapabilities);
    if (result != APUSR_FRAMEGEN_OK)
        return result;

    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);

    context->firstExecution = true;
    context->asyncSupported = (contextDescription->flags & APUSR_FRAMEINTERPOLATION_ENABLE_ASYNC_SUPPORT) != 0;
    context->constants.maxRenderSize[0] = contextDescription->maxRenderSize.width;
    context->constants.maxRenderSize[1] = contextDescription->maxRenderSize.height;
    context->constants.displaySize[0] = contextDescription->displaySize.width;
    context->constants.displaySize[1] = contextDescription->displaySize.height;
    context->constants.displaySizeRcp[0] = 1.0f / contextDescription->displaySize.width;
    context->constants.displaySizeRcp[1] = 1.0f / contextDescription->displaySize.height;
    context->constants.interpolationRectBase[0] = 0;
    context->constants.interpolationRectBase[1] = 0;
    context->constants.interpolationRectSize[0] = contextDescription->displaySize.width;
    context->constants.interpolationRectSize[1] = contextDescription->displaySize.height;

    ApusrFrameInterpolationInternalResourceDescriptions internalDescriptions{};
    getInternalResourceDescriptions(
        &contextDescription->maxRenderSize,
        &contextDescription->displaySize,
        contextDescription->previousInterpolationSourceFormat,
        &internalDescriptions);

    const ApusrFramegenCreateResourceDescription* resources[] = {
        &internalDescriptions.reconstructedDepthInterpolatedFrame,
        &internalDescriptions.gameMotionVectorFieldX,
        &internalDescriptions.gameMotionVectorFieldY,
        &internalDescriptions.inpaintingPyramid,
        &internalDescriptions.counters,
        &internalDescriptions.opticalFlowMotionVectorFieldX,
        &internalDescriptions.opticalFlowMotionVectorFieldY,
        &internalDescriptions.previousInterpolationSource,
        &internalDescriptions.inpaintingMask,
        &internalDescriptions.disocclusionMask,
        &internalDescriptions.defaultDistortionField,
    };

    const bool externalInterpolationHistory =
        (contextDescription->flags & APUSR_FRAMEINTERPOLATION_ENABLE_EXTERNAL_INTERPOLATION_HISTORY) != 0u;
    std::memset(context->srvResources, 0, sizeof(context->srvResources));
    for (const ApusrFramegenCreateResourceDescription* resource : resources) {
        if (externalInterpolationHistory &&
            resource->id == FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PREVIOUS_INTERPOLATION_SOURCE)
            continue;
        result = backendContext.createResource(*resource, context->srvResources[resource->id]);
        if (result != APUSR_FRAMEGEN_OK) {
            frameinterpolationRelease(context);
            return result;
        }
    }

    std::memcpy(context->uavResources, context->srvResources, sizeof(context->srvResources));
    context->refreshPipelineStates = false;
    result = createPipelineStates(context);
    if (result != APUSR_FRAMEGEN_OK)
        frameinterpolationRelease(context);
    return result;
}

static ApusrFramegenResult frameinterpolationRelease(ApusrFrameInterpolationContextPrivate* context)
{
    FFX_ASSERT(context);

    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);

    backendContext.releasePipeline(context->pipelineFiReconstructAndDilate);
    backendContext.releasePipeline(context->pipelineFiSetup);
    backendContext.releasePipeline(context->pipelineFiReconstructPreviousDepth);
    backendContext.releasePipeline(context->pipelineFiGameMotionVectorField);
    backendContext.releasePipeline(context->pipelineFiOpticalFlowVectorField);
    backendContext.releasePipeline(context->pipelineFiDisocclusionMask);
    backendContext.releasePipeline(context->pipelineFiScfi);
    backendContext.releasePipeline(context->pipelineInpaintingPyramid);
    backendContext.releasePipeline(context->pipelineInpainting);
    backendContext.releasePipeline(context->pipelineGameVectorFieldInpaintingPyramid);
    backendContext.releasePipeline(context->pipelineDebugView);

    // unregister resources not created internally
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE]          = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_VECTOR]                   = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_CONFIDENCE]               = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_GLOBAL_MOTION]            = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCENE_CHANGE_DETECTION]   = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT]                                = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT]                                = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH]                         = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH]                         = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS]                = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS]                = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME]    = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME]    = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISTORTION_FIELD]                      = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};

    // Release the copy resources for those that had init data
    backendContext.releaseCopyResource(context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNTERS]);
    backendContext.releaseCopyResource(context->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEFAULT_DISTORTION_FIELD]);

    // release internal resources
    for (int32_t currentResourceIndex = 0; currentResourceIndex < FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNT; ++currentResourceIndex) {

        backendContext.releaseResource(context->srvResources[currentResourceIndex]);
    }

    // Destroy the context
    backendContext.releaseContext();

    return APUSR_FRAMEGEN_OK;
}

static void scheduleDispatch(const apusr::framegen::BackendContext& backendContext, ApusrFrameInterpolationContextPrivate* context, const ApusrFramegenPipeline* pipeline, uint32_t dispatchX, uint32_t dispatchY)
{
    ApusrFramegenGpuJob dispatchJob;
    dispatchJob.jobType = APUSR_FRAMEGEN_JOB_COMPUTE;
    ApusrFramegenComputeJob& jobDescriptor = dispatchJob.computeJob;
    jobDescriptor.pipeline = pipeline;
    jobDescriptor.cmdArgument = {};
    jobDescriptor.cmdArgumentOffset = 0;
    jobDescriptor.flags = 0;
    jobDescriptor.profileLevel = UINT32_MAX;

    for (uint32_t currentShaderResourceViewIndex = 0; currentShaderResourceViewIndex < pipeline->srvTextureCount; ++currentShaderResourceViewIndex)
    {
        const uint32_t            currentResourceId               = pipeline->srvTextureBindings[currentShaderResourceViewIndex].resourceIdentifier;
        const ApusrFramegenResource currentResource = context->srvResources[currentResourceId];
        jobDescriptor.srvTextures[currentShaderResourceViewIndex].resource = currentResource;
    }

    for (uint32_t currentUnorderedAccessViewIndex = 0; currentUnorderedAccessViewIndex < pipeline->uavTextureCount; ++currentUnorderedAccessViewIndex) {

        const uint32_t currentResourceId = pipeline->uavTextureBindings[currentUnorderedAccessViewIndex].resourceIdentifier;

        if (currentResourceId >= FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_0 && currentResourceId <= FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_12)
        {
            const ApusrFramegenResource currentResource = context->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID];
            jobDescriptor.uavTextures[currentUnorderedAccessViewIndex].resource = currentResource;
            jobDescriptor.uavTextures[currentUnorderedAccessViewIndex].mip = currentResourceId - FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID_MIPMAP_0;
        }
        else
        {
            const ApusrFramegenResource currentResource = context->uavResources[currentResourceId];
            jobDescriptor.uavTextures[currentUnorderedAccessViewIndex].resource = currentResource;
            jobDescriptor.uavTextures[currentUnorderedAccessViewIndex].mip = 0;
        }
    }

    jobDescriptor.dimensions[0] = dispatchX;
    jobDescriptor.dimensions[1] = dispatchY;
    jobDescriptor.dimensions[2] = 1;
    jobDescriptor.pipeline = pipeline;

    for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < pipeline->constCount; ++currentRootConstantIndex) {
        jobDescriptor.constantBuffers[currentRootConstantIndex] = context->constantBuffers[pipeline->constantBufferBindings[currentRootConstantIndex].resourceIdentifier];
    }

    for (uint32_t currentUnorderedAccessViewIndex = 0; currentUnorderedAccessViewIndex < pipeline->uavBufferCount; ++currentUnorderedAccessViewIndex)
    {
        const uint32_t currentResourceId = pipeline->uavBufferBindings[currentUnorderedAccessViewIndex].resourceIdentifier;
        ApusrFramegenBufferUav& uav = jobDescriptor.uavBuffers[currentUnorderedAccessViewIndex];
        uav.offset = 0;
        uav.size = 0;
        uav.stride = 0;
        uav.resource = context->uavResources[currentResourceId];
    }

    for (uint32_t currentShaderResourceViewIndex = 0; currentShaderResourceViewIndex < pipeline->srvBufferCount; ++currentShaderResourceViewIndex)
    {
        const uint32_t currentResourceId = pipeline->srvBufferBindings[currentShaderResourceViewIndex].resourceIdentifier;
        ApusrFramegenBufferSrv& srv = jobDescriptor.srvBuffers[currentShaderResourceViewIndex];
        srv.offset = 0;
        srv.size = 0;
        srv.stride = 0;
        srv.resource = context->srvResources[currentResourceId];
    }

#if FFX_DX12_ENABLE_GPU_MARKERS
    APUSR_FRAMEGEN_SET_JOB_LABEL(dispatchJob, pipeline->name);
#endif

    backendContext.schedule(dispatchJob);
}

static ApusrFramegenResult frameInterpolationPrepareNative(
    ApusrFrameInterpolationContextPrivate* context,
    const ApusrFrameInterpolationPrepareDescription* description);
static ApusrFramegenResult frameInterpolationDispatchNative(
    ApusrFrameInterpolationContextPrivate* context,
    const ApusrFrameInterpolationDispatchDescription* description);


namespace apusr::framegen::algorithm {

ApusrFramegenResult frameInterpolationContextCreateStorage(void* contextStorage, const ApusrFrameInterpolationContextDescription* description) noexcept
{
    if (!contextStorage || !description)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (!description->backendInterface.getVersion || !description->backendInterface.getDeviceCapabilities ||
        !description->backendInterface.createContext || !description->backendInterface.destroyContext)
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    if (description->backendInterface.scratchBuffer && !description->backendInterface.scratchBufferSize)
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;

    return frameinterpolationCreate(static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage), description);
}

ApusrFramegenResult frameInterpolationContextDestroyStorage(void* contextStorage) noexcept
{
    if (!contextStorage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    return frameinterpolationRelease(static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage));
}

ApusrFramegenResult frameInterpolationContextGetGpuMemoryUsageStorage(void* contextStorage, ApusrFramegenMemoryUsage* usage) noexcept
{
    if (!contextStorage || !usage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    auto* context = static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage);
    if (!context->device)
        return APUSR_FRAMEGEN_ERROR_NULL_DEVICE;

    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);
    return backendContext.gpuMemoryUsage(*usage);
}

ApusrFramegenResult frameInterpolationGetSharedResourceDescriptionsStorage(
    void* contextStorage,
    ApusrFrameInterpolationSharedResourceDescriptions* descriptions) noexcept
{
    if (!contextStorage || !descriptions)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    auto* context = static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage);
    getSharedResourceDescriptions(
        &context->contextDescription.maxRenderSize,
        &context->contextDescription.displaySize,
        context->contextDescription.previousInterpolationSourceFormat,
        (context->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_EXTERNAL_INTERPOLATION_HISTORY) != 0u,
        descriptions);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult frameInterpolationPrepareStorage(
    void* contextStorage,
    const ApusrFrameInterpolationPrepareDescription* description) noexcept
{
    if (!contextStorage || !description || !description->commandList || !description->depth.resource ||
        !description->motionVectors.resource || !description->dilatedDepth.resource ||
        !description->dilatedMotionVectors.resource || !description->reconstructedPrevDepth.resource)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    const bool hasInterpolationSource = description->interpolationSource.resource != nullptr;
    const bool hasRetainedInterpolationSource = description->retainedInterpolationSource.resource != nullptr;
    const bool hasDistortionField = description->distortionField.resource != nullptr;
    const bool hasRetainedDistortionField = description->retainedDistortionField.resource != nullptr;
    if (hasInterpolationSource != hasRetainedInterpolationSource ||
        hasDistortionField != hasRetainedDistortionField)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    auto* context = static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage);
    if (!context->device)
        return APUSR_FRAMEGEN_ERROR_NULL_DEVICE;
    return frameInterpolationPrepareNative(context, description);
}

ApusrFramegenResult frameInterpolationDispatchStorage(
    void* contextStorage,
    const ApusrFrameInterpolationDispatchDescription* description) noexcept
{
    if (!contextStorage || !description || !description->commandList || !description->currentBackBuffer.resource ||
        !description->output.resource || !description->opticalFlowVector.resource ||
        !description->opticalFlowSceneChangeDetection.resource || !description->dilatedDepth.resource ||
        !description->dilatedMotionVectors.resource || !description->reconstructedPrevDepth.resource)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;

    auto* context = static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage);
    if (!context->device)
        return APUSR_FRAMEGEN_ERROR_NULL_DEVICE;
    if ((context->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_EXTERNAL_INTERPOLATION_HISTORY) != 0u &&
        !description->previousInterpolationSource.resource)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    return frameInterpolationDispatchNative(context, description);
}

ApusrFramegenResult frameInterpolationEnqueueRefreshPipelineRequestStorage(void* contextStorage) noexcept
{
    if (!contextStorage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    static_cast<ApusrFrameInterpolationContextPrivate*>(contextStorage)->refreshPipelineStates = true;
    return APUSR_FRAMEGEN_OK;
}

} // namespace apusr::framegen::algorithm


ApusrFramegenResult apusrFrameInterpolationContextCreate(ApusrFrameInterpolationContext* context, const ApusrFrameInterpolationContextDescription* description) noexcept
{
    return context ? apusr::framegen::algorithm::frameInterpolationContextCreateStorage(context->data, description) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrFrameInterpolationContextDestroy(ApusrFrameInterpolationContext* context) noexcept
{
    return context ? apusr::framegen::algorithm::frameInterpolationContextDestroyStorage(context->data) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrFrameInterpolationContextGetGpuMemoryUsage(ApusrFrameInterpolationContext* context, ApusrFramegenMemoryUsage* usage) noexcept
{
    return context ? apusr::framegen::algorithm::frameInterpolationContextGetGpuMemoryUsageStorage(context->data, usage) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrFrameInterpolationGetSharedResourceDescriptions(
    ApusrFrameInterpolationContext* context,
    ApusrFrameInterpolationSharedResourceDescriptions* descriptions) noexcept
{
    return context ? apusr::framegen::algorithm::frameInterpolationGetSharedResourceDescriptionsStorage(context->data, descriptions) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrFrameInterpolationPrepare(
    ApusrFrameInterpolationContext* context,
    const ApusrFrameInterpolationPrepareDescription* description) noexcept
{
    return context ? apusr::framegen::algorithm::frameInterpolationPrepareStorage(context->data, description) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrFrameInterpolationDispatch(
    ApusrFrameInterpolationContext* context,
    const ApusrFrameInterpolationDispatchDescription* description) noexcept
{
    return context ? apusr::framegen::algorithm::frameInterpolationDispatchStorage(context->data, description) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrFrameInterpolationGetGpuMemoryUsage(
    ApusrFramegenDevice device,
    ApusrFramegenDimensions2D maxRenderSize,
    ApusrFramegenDimensions2D displaySize,
    ApusrFramegenSurfaceFormat previousInterpolationSourceFormat,
    ApusrFramegenMemoryUsage* usage) noexcept
{
    if (!device || !usage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;

    ApusrFrameInterpolationInternalResourceDescriptions internalDescriptions{};
    getInternalResourceDescriptions(
        &maxRenderSize,
        &displaySize,
        previousInterpolationSourceFormat,
        &internalDescriptions);

    const ApusrFramegenCreateResourceDescription* resources[] = {
        &internalDescriptions.reconstructedDepthInterpolatedFrame,
        &internalDescriptions.gameMotionVectorFieldX,
        &internalDescriptions.gameMotionVectorFieldY,
        &internalDescriptions.inpaintingPyramid,
        &internalDescriptions.counters,
        &internalDescriptions.opticalFlowMotionVectorFieldX,
        &internalDescriptions.opticalFlowMotionVectorFieldY,
        &internalDescriptions.previousInterpolationSource,
        &internalDescriptions.inpaintingMask,
        &internalDescriptions.disocclusionMask,
        &internalDescriptions.defaultDistortionField,
    };

    uint64_t size = 0;
    for (const auto* resource : resources)
    {
        const ApusrFramegenResult result = apusr::framegen::resourceAllocationSize(device, *resource, size);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
        usage->totalUsageInBytes += size;
        if (resource->resourceDescription.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE)
            usage->aliasableUsageInBytes += size;
    }

    ApusrFrameInterpolationSharedResourceDescriptions sharedDescriptions{};
    getSharedResourceDescriptions(
        &maxRenderSize, &displaySize, previousInterpolationSourceFormat, false, &sharedDescriptions);
    const ApusrFramegenCreateResourceDescription* sharedResources[] = {
        &sharedDescriptions.dilatedDepth,
        &sharedDescriptions.dilatedMotionVectors,
        &sharedDescriptions.reconstructedPrevNearestDepth,
    };

    for (const auto* resource : sharedResources)
    {
        const ApusrFramegenResult result = apusr::framegen::resourceAllocationSize(device, *resource, size);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
        usage->totalUsageInBytes += 2 * size; // provider owns two history copies of each shared resource
        if (resource->resourceDescription.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE)
            usage->aliasableUsageInBytes += 2 * size;
    }

    return APUSR_FRAMEGEN_OK;
}

static void setupDeviceDepthToViewSpaceDepthParams(ApusrFrameInterpolationContextPrivate* context, const ApusrFrameInterpolationRenderDescription* params, FrameInterpolationConstants* constants)
{
    const bool bInverted = (context->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) == APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
    const bool bInfinite = (context->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE) == APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;

    // make sure it has no impact if near and far plane values are swapped in dispatch params
    // the flags "inverted" and "infinite" will decide what transform to use
    float fMin = std::min(params->cameraNear, params->cameraFar);
    float fMax = std::max(params->cameraNear, params->cameraFar);

    if (bInverted) {
        float tmp = fMin;
        fMin = fMax;
        fMax = tmp;
    }

    // a 0 0 0   x
    // 0 b 0 0   y
    // 0 0 c d   z
    // 0 0 e 0   1

    const float fQ = fMax / (fMin - fMax);
    const float d = -1.0f; // for clarity

    const float matrix_elem_c[2][2] = {
        fQ,                     // non reversed, non infinite
        -1.0f - FLT_EPSILON,    // non reversed, infinite
        fQ,                     // reversed, non infinite
        0.0f + FLT_EPSILON      // reversed, infinite
    };

    const float matrix_elem_e[2][2] = {
        fQ * fMin,              // non reversed, non infinite
        -fMin - FLT_EPSILON,    // non reversed, infinite
        fQ * fMin,              // reversed, non infinite
        fMax,                   // reversed, infinite
    };

    constants->deviceToViewDepth[0] = d * matrix_elem_c[bInverted][bInfinite];
    constants->deviceToViewDepth[1] = matrix_elem_e[bInverted][bInfinite] * params->viewSpaceToMetersFactor;

    // revert x and y coords
    const float aspect = params->renderSize.width / float(params->renderSize.height);
    const float cotHalfFovY = cosf(0.5f * params->cameraFovAngleVertical) / sinf(0.5f * params->cameraFovAngleVertical);
    const float a = cotHalfFovY / aspect;
    const float b = cotHalfFovY;

    constants->deviceToViewDepth[2] = (1.0f / a);
    constants->deviceToViewDepth[3] = (1.0f / b);
    
}

static const float debugBarColorSequence[] = {
    0.0f, 1.0f, 1.0f,   // teal
    1.0f, 0.42f, 0.0f,  // orange
    0.0f, 0.16f, 1.0f,  // blue
    0.74f, 1.0f, 0.0f,  // lime
    0.68f, 0.0f, 1.0f,  // purple
    0.0f, 1.0f, 0.1f,   // green
    1.0f, 1.0f, 0.48f   // bright yellow
};
const size_t debugBarColorSequenceLength = 7;

static void fsr3FrameInterpolationDebugCheckPrepare(ApusrFrameInterpolationContextPrivate* context, const ApusrFrameInterpolationPrepareDescription* params)
{
    
    static const float zeroVector3D[3] = {0.f, 0.f, 0.f};
    if ((std::memcmp(params->cameraPosition, zeroVector3D, sizeof(zeroVector3D)) == 0) &&
        (std::memcmp(params->cameraUp, zeroVector3D, sizeof(zeroVector3D)) == 0) &&
        (std::memcmp(params->cameraRight, zeroVector3D, sizeof(zeroVector3D)) == 0) &&
        (std::memcmp(params->cameraForward, zeroVector3D, sizeof(zeroVector3D)) == 0))
    {
        FFX_PRINT_MESSAGE(FFX_MESSAGE_TYPE_WARNING, L"ffxDispatchDescFrameGenerationPrepareCameraInfo needs to be passed as linked struct. This is a required input to FSR3.1.4 and onwards for best quality.");
    }
}

static ApusrFramegenResult frameInterpolationPrepareNative(ApusrFrameInterpolationContextPrivate* contextPrivate,
    const ApusrFrameInterpolationPrepareDescription* params)
{
    const apusr::framegen::BackendContext backendContext(contextPrivate->contextDescription.backendInterface, contextPrivate->effectContextId);

    if ((contextPrivate->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING) == APUSR_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING)
    {
        fsr3FrameInterpolationDebugCheckPrepare(contextPrivate, params);
    }
    
    contextPrivate->constants.renderSize[0]         = params->renderSize.width;
    contextPrivate->constants.renderSize[1]         = params->renderSize.height;
    contextPrivate->constants.jitter[0]             = params->jitterOffset.x;
    contextPrivate->constants.jitter[1]             = params->jitterOffset.y;

    const int32_t* motionVectorsTargetSize          = (contextPrivate->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
                                                        ? contextPrivate->constants.displaySize
                                                        : contextPrivate->constants.renderSize;
    contextPrivate->constants.motionVectorScale[0]  = (params->motionVectorScale.x / motionVectorsTargetSize[0]);
    contextPrivate->constants.motionVectorScale[1]  = (params->motionVectorScale.y / motionVectorsTargetSize[1]);

    // Input motion vectors may already include camera jitter. Remove its
    // previous-to-current delta in normalized UV space, but never carry history
    // through a first, reset, or disjoint frame.
    const auto jitterCancellation = apusr::framegen::algorithm::computeMotionVectorJitterCancellation(
        (contextPrivate->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS) != 0,
        params->reset,
        contextPrivate->dispatchCount != 0u,
        contextPrivate->previousFrameID,
        params->frameID,
        contextPrivate->previousJitterOffset[0],
        contextPrivate->previousJitterOffset[1],
        params->jitterOffset.x,
        params->jitterOffset.y,
        motionVectorsTargetSize[0],
        motionVectorsTargetSize[1]);
    contextPrivate->constants.motionVectorJitterCancellation[0] = jitterCancellation.x;
    contextPrivate->constants.motionVectorJitterCancellation[1] = jitterCancellation.y;
    contextPrivate->previousJitterOffset[0] = params->jitterOffset.x;
    contextPrivate->previousJitterOffset[1] = params->jitterOffset.y;

    backendContext.stageConstants(&contextPrivate->constants, sizeof(contextPrivate->constants), contextPrivate->constantBuffers[FFX_FRAMEINTERPOLATION_CONSTANTBUFFER_IDENTIFIER]);

    FFX_ASSERT(params->depth.resource != nullptr);
    FFX_ASSERT(params->motionVectors.resource != nullptr);

    backendContext.registerResource(params->depth, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEPTH]);

    backendContext.registerResource(params->motionVectors, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_MOTION_VECTORS]);

    backendContext.registerResource(params->dilatedDepth, contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH]);
    backendContext.registerResource(params->dilatedMotionVectors, contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS]);
    backendContext.registerResource(params->reconstructedPrevDepth, contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME]);

    ApusrFramegenResource interpolationSource{};
    ApusrFramegenResource retainedInterpolationSource{};
    ApusrFramegenResource distortionField{};
    ApusrFramegenResource retainedDistortionField{};

    const auto validRetentionPair = [](
        const ApusrFramegenExternalResource& source,
        const ApusrFramegenExternalResource& retained) noexcept {
        return source.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D &&
            retained.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D &&
            source.description.format == retained.description.format &&
            source.description.width == retained.description.width &&
            source.description.height == retained.description.height;
    };
    if (params->interpolationSource.resource &&
        !validRetentionPair(params->interpolationSource, params->retainedInterpolationSource))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (params->distortionField.resource &&
        !validRetentionPair(params->distortionField, params->retainedDistortionField))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    if (params->interpolationSource.resource) {
        ApusrFramegenResult result = backendContext.registerResource(params->interpolationSource, interpolationSource);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
        result = backendContext.registerResource(params->retainedInterpolationSource, retainedInterpolationSource);
        if (result != APUSR_FRAMEGEN_OK) {
            backendContext.unregisterResources(params->commandList);
            return result;
        }
    }
    if (params->distortionField.resource) {
        ApusrFramegenResult result = backendContext.registerResource(params->distortionField, distortionField);
        if (result != APUSR_FRAMEGEN_OK) {
            backendContext.unregisterResources(params->commandList);
            return result;
        }
        result = backendContext.registerResource(params->retainedDistortionField, retainedDistortionField);
        if (result != APUSR_FRAMEGEN_OK) {
            backendContext.unregisterResources(params->commandList);
            return result;
        }
    }

    const auto scheduleRetentionCopy = [&](
        const ApusrFramegenResource source,
        const ApusrFramegenResource retained,
        const wchar_t* label) noexcept {
        ApusrFramegenGpuJob copyJob = {APUSR_FRAMEGEN_JOB_COPY};
        copyJob.copyJob.src = source;
        copyJob.copyJob.dst = retained;
        APUSR_FRAMEGEN_SET_JOB_LABEL(copyJob, label);
        return backendContext.schedule(copyJob);
    };
    if (params->interpolationSource.resource) {
        const ApusrFramegenResult result = scheduleRetentionCopy(
            interpolationSource, retainedInterpolationSource, L"Retain Interpolation Source");
        if (result != APUSR_FRAMEGEN_OK) {
            backendContext.unregisterResources(params->commandList);
            return result;
        }
    }
    if (params->distortionField.resource) {
        const ApusrFramegenResult result = scheduleRetentionCopy(
            distortionField, retainedDistortionField, L"Retain Distortion Field");
        if (result != APUSR_FRAMEGEN_OK) {
            backendContext.unregisterResources(params->commandList);
            return result;
        }
    }

    // clear estimated depth resources
    {
        ApusrFramegenGpuJob clearJob = {APUSR_FRAMEGEN_JOB_CLEAR_FLOAT};
        const bool bInverted = (contextPrivate->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) == APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
        const float clearDepthValue[]{bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f};
        std::memcpy(clearJob.clearJob.color, clearDepthValue, 4 * sizeof(float));
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Reconstructed Previous Nearest Depth");
        clearJob.clearJob.target = contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME];
        backendContext.schedule(clearJob);
    }

    uint32_t                              renderDispatchSizeX = uint32_t(params->renderSize.width + 7) / 8;
    uint32_t                              renderDispatchSizeY = uint32_t(params->renderSize.height + 7) / 8;

    scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiReconstructAndDilate, renderDispatchSizeX, renderDispatchSizeY);

    backendContext.execute(params->commandList);

    // release dynamic resources
    backendContext.unregisterResources(params->commandList);

    contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEPTH]                              = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_MOTION_VECTORS]                     = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH]                      = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS]             = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};
    contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME] = {FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_NULL};

    return APUSR_FRAMEGEN_OK;
}

static ApusrFramegenResult frameInterpolationDispatchNative(ApusrFrameInterpolationContextPrivate* contextPrivate, const ApusrFrameInterpolationDispatchDescription* params)
{
    const ApusrFrameInterpolationRenderDescription* renderDesc     = &contextPrivate->renderDescription;
    const apusr::framegen::BackendContext backendContext(contextPrivate->contextDescription.backendInterface, contextPrivate->effectContextId);

    if (contextPrivate->refreshPipelineStates) {
        const ApusrFramegenResult result = createPipelineStates(contextPrivate);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
        contextPrivate->refreshPipelineStates = false;
    }

    const bool bReset = (contextPrivate->dispatchCount == 0) || params->reset;

    FFX_ASSERT_MESSAGE(!contextPrivate->asyncSupported || bReset || (params->frameID > contextPrivate->previousFrameID),
                       "When async support is enabled, and the reset flag is not set, frame ID must increment in each dispatch");

    // Detect disjoint frameID values
    const bool bFrameID_Decreased   = params->frameID < contextPrivate->previousFrameID;
    const bool bFrameID_Skipped     = (params->frameID - contextPrivate->previousFrameID) > 1;
    const bool bDisjointFrameID     = bFrameID_Decreased || bFrameID_Skipped;
    contextPrivate->previousFrameID = params->frameID;
    contextPrivate->dispatchCount++;

    contextPrivate->constants.renderSize[0]         = params->renderSize.width;
    contextPrivate->constants.renderSize[1]         = params->renderSize.height;
    contextPrivate->constants.displaySize[0]        = params->displaySize.width;
    contextPrivate->constants.displaySize[1]        = params->displaySize.height;
    contextPrivate->constants.displaySizeRcp[0]     = 1.0f / params->displaySize.width;
    contextPrivate->constants.displaySizeRcp[1]     = 1.0f / params->displaySize.height;
    contextPrivate->constants.upscalerTargetSize[0] = params->interpolationRect.width;
    contextPrivate->constants.upscalerTargetSize[1] = params->interpolationRect.height;
    contextPrivate->constants.Mode                  = 0;
    contextPrivate->constants.Reset                 = bReset || bDisjointFrameID;
    contextPrivate->constants.deltaTime             = params->frameTimeDelta;
    contextPrivate->constants.HUDLessAttachedFactor = params->currentBackBufferHudless.resource ? 1 : 0;

    contextPrivate->constants.opticalFlowScale[0]   = params->opticalFlowScale.x;
    contextPrivate->constants.opticalFlowScale[1]   = params->opticalFlowScale.y;
    contextPrivate->constants.opticalFlowBlockSize  = params->opticalFlowBlockSize;// displaySize.width / params->opticalFlowBufferSize.width;
    contextPrivate->constants.dispatchFlags         = params->flags;

    contextPrivate->constants.cameraNear            = params->cameraNear;
    contextPrivate->constants.cameraFar             = params->cameraFar;

    contextPrivate->constants.interpolationRectBase[0] = params->interpolationRect.left;
    contextPrivate->constants.interpolationRectBase[1] = params->interpolationRect.top;
    contextPrivate->constants.interpolationRectSize[0] = params->interpolationRect.width;
    contextPrivate->constants.interpolationRectSize[1] = params->interpolationRect.height;

    // Debug bar
    static size_t dbgIdx = 0;
    std::memcpy(contextPrivate->constants.debugBarColor, &debugBarColorSequence[dbgIdx * 3], 3 * sizeof(float));
    dbgIdx = (dbgIdx + 1) % debugBarColorSequenceLength;

    contextPrivate->constants.backBufferTransferFunction = params->backBufferTransferFunction;
    contextPrivate->constants.minMaxLuminance[0]         = params->minMaxLuminance[0];
    contextPrivate->constants.minMaxLuminance[1]         = params->minMaxLuminance[1];

    const float aspectRatio                             = (float)params->renderSize.width / (float)params->renderSize.height;
    const float cameraAngleHorizontal                   = atan(tan(params->cameraFovAngleVertical / 2) * aspectRatio) * 2;
    contextPrivate->constants.fTanHalfFOV               = tanf(cameraAngleHorizontal * 0.5f);

    const bool bUseExternalDistortionFieldResource = params->distortionField.resource != nullptr;
    if (bUseExternalDistortionFieldResource)
    {
        contextPrivate->constants.distortionFieldSize[0] = params->distortionField.description.width;
        contextPrivate->constants.distortionFieldSize[1] = params->distortionField.description.height;
    }
    else
    {
        contextPrivate->constants.distortionFieldSize[0] = 1;
        contextPrivate->constants.distortionFieldSize[1] = 1;
    }

    contextPrivate->renderDescription.cameraFar                 = params->cameraFar;
    contextPrivate->renderDescription.cameraNear                = params->cameraNear;
    contextPrivate->renderDescription.viewSpaceToMetersFactor = (params->viewSpaceToMetersFactor > 0.0f) ? params->viewSpaceToMetersFactor : 1.0f;
    contextPrivate->renderDescription.cameraFovAngleVertical    = params->cameraFovAngleVertical;
    contextPrivate->renderDescription.renderSize                = params->renderSize;
    contextPrivate->renderDescription.upscaleSize               = params->displaySize;
    setupDeviceDepthToViewSpaceDepthParams(contextPrivate, renderDesc, &contextPrivate->constants);

    backendContext.stageConstants(&contextPrivate->constants, sizeof(contextPrivate->constants), contextPrivate->constantBuffers[FFX_FRAMEINTERPOLATION_CONSTANTBUFFER_IDENTIFIER]);

    if (contextPrivate->constants.HUDLessAttachedFactor == 1) {

        FFX_ASSERT_MESSAGE(contextPrivate->contextDescription.previousInterpolationSourceFormat == params->currentBackBufferHudless.description.format,
                           "Dispatch FI param currentBackBuffer_HUDLess format and Create FG Context's hudlessBackBufferFormat have to be identical. Otherwise, CopyTextureRegion from FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE to FI_PreviousInterpolationSource would fail");

        backendContext.registerResource(params->currentBackBuffer, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PRESENT_BACKBUFFER]);
        backendContext.registerResource(params->currentBackBufferHudless, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE]);
    }
    else {
        FFX_ASSERT_MESSAGE(contextPrivate->contextDescription.previousInterpolationSourceFormat == params->currentBackBuffer.description.format,
                           "Dispatch FI param currentBackBuffer format and Create FG Context's backBufferFormat have to be identical. This assert can also be triggered if create FG Context with optional hudlessBackBufferFormat that is different from backBufferFormat and Dispatch FI param's currentBackBuffer_HUDLess is null.");
        backendContext.registerResource(params->currentBackBuffer, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE]);
        contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PRESENT_BACKBUFFER] = contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE];
    }

    if (params->previousInterpolationSource.resource != nullptr) {
        FFX_ASSERT_MESSAGE(
            contextPrivate->contextDescription.previousInterpolationSourceFormat ==
                params->previousInterpolationSource.description.format,
            "Explicit previous interpolation source format must match the context history format");
        const ApusrFramegenResult historyResult = backendContext.registerResource(
            params->previousInterpolationSource,
            contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PREVIOUS_INTERPOLATION_SOURCE]);
        if (historyResult != APUSR_FRAMEGEN_OK)
            return historyResult;
    }

    if (params->dilatedDepth.resource != nullptr)
    {
        backendContext.registerResource(params->dilatedDepth, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_DEPTH]);
    }
    if (params->dilatedMotionVectors.resource != nullptr)
    {
        backendContext.registerResource(params->dilatedMotionVectors, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS]);
    }
    if (params->reconstructedPrevDepth.resource != nullptr)
    {
        backendContext.registerResource(params->reconstructedPrevDepth, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME]);
    }

    // Register output as SRV and UAV
    backendContext.registerResource(params->output, contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT]);
    contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT] = contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OUTPUT];

    // set optical flow buffers
    if (params->opticalFlowScale.x > 0)
    {
        backendContext.registerResource(params->opticalFlowVector, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_VECTOR]);
        backendContext.registerResource(params->opticalFlowSceneChangeDetection, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCENE_CHANGE_DETECTION]);
    }
    else
    {
        contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_CONFIDENCE]             = {};
        contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_GLOBAL_MOTION]          = {};
        contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCENE_CHANGE_DETECTION] = {};
        contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_VECTOR] = {};
    }

    if (bUseExternalDistortionFieldResource)
    {
        backendContext.registerResource(params->distortionField, contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISTORTION_FIELD]);
    }
    else
    {
        contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISTORTION_FIELD] = contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DEFAULT_DISTORTION_FIELD];
    }

    uint32_t displayDispatchSizeX = uint32_t(params->displaySize.width + 7) / 8;
    uint32_t displayDispatchSizeY = uint32_t(params->displaySize.height + 7) / 8;

    uint32_t renderDispatchSizeX = uint32_t(params->renderSize.width + 7) / 8;
    uint32_t renderDispatchSizeY = uint32_t(params->renderSize.height + 7) / 8;

    uint32_t opticalFlowDispatchSizeX = uint32_t(params->displaySize.width / float(params->opticalFlowBlockSize) + 7) / 8;
    uint32_t opticalFlowDispatchSizeY = uint32_t(params->displaySize.height / float(params->opticalFlowBlockSize) + 7) / 8;

    const bool bExecutePreparationPasses = (false == contextPrivate->constants.Reset);

    // Schedule work for the interpolation command list
    {
        ApusrFramegenResource aliasableResources[] = {
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_X],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_GAME_MOTION_VECTOR_FIELD_Y],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_PYRAMID],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_INPAINTING_MASK],
            contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_DISOCCLUSION_MASK]
        };
        for (int i = 0; i < std::size(aliasableResources); ++i)
        {
            ApusrFramegenGpuJob discardJob        = {APUSR_FRAMEGEN_JOB_DISCARD};
            discardJob.discardJob.target = aliasableResources[i];
            backendContext.schedule(discardJob);
        }

        scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiSetup, renderDispatchSizeX, renderDispatchSizeY);

            // game vector field inpainting pyramid
        auto scheduleDispatchGameVectorFieldInpaintingPyramid = [&]() {
            // Auto exposure
            uint32_t dispatchThreadGroupCountXY[2];
            uint32_t workGroupOffset[2];
            uint32_t numWorkGroupsAndMips[2];
            uint32_t rectInfo[4] = {0, 0, params->renderSize.width, params->renderSize.height};
            ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);

            // downsample
            contextPrivate->inpaintingPyramidContants.numworkGroups      = numWorkGroupsAndMips[0];
            contextPrivate->inpaintingPyramidContants.mips               = numWorkGroupsAndMips[1];
            contextPrivate->inpaintingPyramidContants.workGroupOffset[0] = workGroupOffset[0];
            contextPrivate->inpaintingPyramidContants.workGroupOffset[1] = workGroupOffset[1];

            backendContext.stageConstants(&contextPrivate->inpaintingPyramidContants, sizeof(contextPrivate->inpaintingPyramidContants), contextPrivate->constantBuffers[FFX_FRAMEINTERPOLATION_INPAINTING_PYRAMID_CONSTANTBUFFER_IDENTIFIER]);

            scheduleDispatch(
                backendContext, contextPrivate, &contextPrivate->pipelineGameVectorFieldInpaintingPyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]);
        };

        // only execute FG data preparation passes when reset wasnt triggered
        if (bExecutePreparationPasses)
        {
            // clear estimated depth resources
            {
                ApusrFramegenGpuJob clearJob = {APUSR_FRAMEGEN_JOB_CLEAR_FLOAT};

                const bool bInverted =
                    (contextPrivate->contextDescription.flags & APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED) == APUSR_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
                const float clearDepthValue[]{bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f};
                std::memcpy(clearJob.clearJob.color, clearDepthValue, 4 * sizeof(float));

                APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Reconstructed Depth Interpolated Frame");
                clearJob.clearJob.target =
                    contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME];
                backendContext.schedule(clearJob);
            }

            scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiReconstructPreviousDepth, renderDispatchSizeX, renderDispatchSizeY);
            scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiGameMotionVectorField, renderDispatchSizeX, renderDispatchSizeY);

            scheduleDispatchGameVectorFieldInpaintingPyramid();

            scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiOpticalFlowVectorField, opticalFlowDispatchSizeX, opticalFlowDispatchSizeY);

            scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiDisocclusionMask, renderDispatchSizeX, renderDispatchSizeY);
        }

        scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineFiScfi, displayDispatchSizeX, displayDispatchSizeY);

        // inpainting pyramid
        {
            // Auto exposure
            uint32_t dispatchThreadGroupCountXY[2];
            uint32_t workGroupOffset[2];
            uint32_t numWorkGroupsAndMips[2];
            uint32_t rectInfo[4] = { 0, 0, params->displaySize.width, params->displaySize.height };
            ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);

            // downsample
            contextPrivate->inpaintingPyramidContants.numworkGroups      = numWorkGroupsAndMips[0];
            contextPrivate->inpaintingPyramidContants.mips               = numWorkGroupsAndMips[1];
            contextPrivate->inpaintingPyramidContants.workGroupOffset[0] = workGroupOffset[0];
            contextPrivate->inpaintingPyramidContants.workGroupOffset[1] = workGroupOffset[1];

            backendContext.stageConstants(&contextPrivate->inpaintingPyramidContants, sizeof(contextPrivate->inpaintingPyramidContants), contextPrivate->constantBuffers[FFX_FRAMEINTERPOLATION_INPAINTING_PYRAMID_CONSTANTBUFFER_IDENTIFIER]);

            scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineInpaintingPyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]);
        }

        scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineInpainting, displayDispatchSizeX, displayDispatchSizeY);

        if (params->flags & APUSR_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW)
        {
            scheduleDispatchGameVectorFieldInpaintingPyramid();
            scheduleDispatch(backendContext, contextPrivate, &contextPrivate->pipelineDebugView, displayDispatchSizeX, displayDispatchSizeY);
        }

        // The generic path retains the current interpolation source internally.
        // Vulkan's final-form provider owns a ping-pong history instead, so its
        // one full-resolution ingest happens in Phase A while transient HUD-less
        // input is valid and no second end-of-dispatch copy is emitted here.
        if ((contextPrivate->contextDescription.flags &
             APUSR_FRAMEINTERPOLATION_ENABLE_EXTERNAL_INTERPOLATION_HISTORY) == 0u) {
            ApusrFramegenGpuJob copyJob = {APUSR_FRAMEGEN_JOB_COPY};
            copyJob.copyJob.src = contextPrivate->srvResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_CURRENT_INTERPOLATION_SOURCE];
            copyJob.copyJob.dst = contextPrivate->uavResources[FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_PREVIOUS_INTERPOLATION_SOURCE];
            APUSR_FRAMEGEN_SET_JOB_LABEL(copyJob, L"Retain Interpolation Source");
            backendContext.schedule(copyJob);
        }

        // Internal resources stay in their last real access state. The DX12 backend tracks
        // that state and emits the transition required by the next actual binding.

        // schedule optical flow and frame interpolation
        backendContext.execute(params->commandList);
    }

    // release dynamic resources
    backendContext.unregisterResources(params->commandList);

    return APUSR_FRAMEGEN_OK;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
