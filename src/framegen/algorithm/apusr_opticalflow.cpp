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

#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4838)
#endif

#define FFX_CPU
#include "gpu/ffx_core.h"
#include "gpu/spd/ffx_spd.h"
#include "gpu/opticalflow/ffx_opticalflow_callbacks_hlsl.h"

#define FFX_OPTICALFLOW_MAX_QUEUED_FRAMES 16

#include "ffx_opticalflow_private.h"

static_assert(sizeof(ApusrOpticalFlowContextPrivate) <= APUSR_OPTICALFLOW_CONTEXT_STORAGE_SIZE,
    "APUSR Optical Flow context storage is too small for the private context.");
static_assert(alignof(ApusrOpticalFlowContextPrivate) <= alignof(ApusrOpticalFlowContext),
    "APUSR Optical Flow context storage alignment is insufficient.");
#include "ffx_opticalflow_shaderblobs.h"
#include "apusr_framegen_backend.h"
#include "internal/apusr_framegen_algorithm_storage.h"

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



typedef struct Binding
{
    uint32_t    index;
    const char* name;
}Binding;

static const Binding srvBindingNames[] =
{
    {FFX_OF_BINDING_IDENTIFIER_INPUT_COLOR,                           "r_input_color"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT,                    "r_optical_flow_input"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS_INPUT,           "r_optical_flow_previous_input"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW,                          "r_optical_flow"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS,                 "r_optical_flow_previous"},
};

static const Binding uavBindingNames[] =
{
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT,                      "rw_optical_flow_input"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_1,              "rw_optical_flow_input_level_1"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_2,              "rw_optical_flow_input_level_2"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_3,              "rw_optical_flow_input_level_3"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_4,              "rw_optical_flow_input_level_4"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_5,              "rw_optical_flow_input_level_5"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_6,              "rw_optical_flow_input_level_6"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW,                            "rw_optical_flow"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_NEXT_LEVEL,                 "rw_optical_flow_next_level"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_HISTOGRAM,              "rw_optical_flow_scd_histogram"}, // scene change detection histogram
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM,     "rw_optical_flow_scd_previous_histogram"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_TEMP,                   "rw_optical_flow_scd_temp"},
    {FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_OUTPUT,                 "rw_optical_flow_scd_output"},
};

static const Binding cbBindingNames[] =
{
    {FFX_OPTICALFLOW_CONSTANTBUFFER_IDENTIFIER,       "cbOF"},
    {FFX_OPTICALFLOW_CONSTANTBUFFER_IDENTIFIER_SPD,   "cbOF_SPD"}
};

// Broad structure of the root signature.
typedef enum OpticalFlowRootSignatureLayout {

    OPTICALFLOW_ROOT_SIGNATURE_LAYOUT_UAVS,
    OPTICALFLOW_ROOT_SIGNATURE_LAYOUT_SRVS,
    OPTICALFLOW_ROOT_SIGNATURE_LAYOUT_CONSTANTS,
    OPTICALFLOW_ROOT_SIGNATURE_LAYOUT_CONSTANTS_REGISTER_1,
    OPTICALFLOW_ROOT_SIGNATURE_LAYOUT_PARAMETER_COUNT
} OpticalFlowRootSignatureLayout;

typedef struct OpticalFlowSpdConstants
{
    uint32_t                    mips;
    uint32_t                    numworkGroups;
    uint32_t                    workGroupOffset[2];

    uint32_t                    numworkGroupsOpticalFlowInputPyramid;
    uint32_t pad0_;
    uint32_t pad1_;
    uint32_t pad2_;

} OpticalFlowSpdConstants;

static ApusrFramegenResult patchResourceBindings(ApusrFramegenPipeline* inoutPipeline)
{
    for (uint32_t srvIndex = 0; srvIndex < inoutPipeline->srvTextureCount; ++srvIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(srvBindingNames); ++mapIndex)
        {
            if (0 == strcmp(srvBindingNames[mapIndex].name, inoutPipeline->srvTextureBindings[srvIndex].name))
                break;
        }
        FFX_ASSERT(mapIndex < std::size(srvBindingNames));
        if (mapIndex == std::size(srvBindingNames))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->srvTextureBindings[srvIndex].resourceIdentifier = srvBindingNames[mapIndex].index;
    }

    for (uint32_t uavIndex = 0; uavIndex < inoutPipeline->uavTextureCount; ++uavIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(uavBindingNames); ++mapIndex)
        {
            if (0 == strcmp(uavBindingNames[mapIndex].name, inoutPipeline->uavTextureBindings[uavIndex].name))
                break;
        }
        FFX_ASSERT(mapIndex < std::size(uavBindingNames));
        if (mapIndex == std::size(uavBindingNames))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->uavTextureBindings[uavIndex].resourceIdentifier = uavBindingNames[mapIndex].index;
    }

    for (uint32_t cbIndex = 0; cbIndex < inoutPipeline->constCount; ++cbIndex)
    {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < std::size(cbBindingNames); ++mapIndex)
        {
            if (0 == strcmp(cbBindingNames[mapIndex].name, inoutPipeline->constantBufferBindings[cbIndex].name))
                break;
        }
        FFX_ASSERT(mapIndex < std::size(cbBindingNames));
        if (mapIndex == std::size(cbBindingNames))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

        inoutPipeline->constantBufferBindings[cbIndex].resourceIdentifier = cbBindingNames[mapIndex].index;
    }

    return APUSR_FRAMEGEN_OK;
}

static uint32_t getPipelinePermutationFlags(uint32_t, ApusrFramegenPass, bool fp16, bool force64, bool)
{
    uint32_t flags = 0;
    flags |= (force64) ? OPTICALFLOW_SHADER_PERMUTATION_FORCE_WAVE64 : 0;
    flags |= (fp16) ? OPTICALFLOW_SHADER_PERMUTATION_ALLOW_FP16 : 0;
    return flags;
}

static ApusrFramegenResult createPipelineStates(ApusrOpticalFlowContextPrivate* context)
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
        backendContext.releasePipeline(*pipeline);
        copyWideName(pipelineDescription.name, name);

        ApusrFramegenShaderBlob shaderBlob{};
        ApusrFramegenResult result = opticalflowGetPermutationBlobByIndex(
            pass,
            getPipelinePermutationFlags(contextFlags, pass, supportedFP16, canForceWave64, useLut),
            &shaderBlob);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
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
        {APUSR_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID, L"Opticalflow_InputPyramid", &context->pipelineGenerateOpticalFlowInputPyramid},
        {APUSR_OPTICALFLOW_PASS_PREPARE_LUMA, L"Opticalflow_Luma", &context->pipelinePrepareLuma},
        {APUSR_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM, L"Opticalflow_SCD_Histogram", &context->pipelineGenerateSCDHistogram},
        {APUSR_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE, L"Opticalflow_SCD_Divergence", &context->pipelineComputeSCDDivergence},
        {APUSR_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5, L"Opticalflow_Search", &context->pipelineComputeOpticalFlowAdvancedV5},
        {APUSR_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5, L"Opticalflow_Filter", &context->pipelineFilterOpticalFlowV5},
        {APUSR_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5, L"Opticalflow_Upscale", &context->pipelineScaleOpticalFlowAdvancedV5},
    };

    for (const PipelineSpec& pipeline : pipelines) {
        const ApusrFramegenResult result = createComputePipeline(pipeline.pass, pipeline.name, pipeline.pipeline);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
    }
    return APUSR_FRAMEGEN_OK;
}

constexpr uint32_t OpticalFlowMaxPyramidLevels = 7;
constexpr uint32_t HistogramBins = 256;
constexpr uint32_t HistogramsPerDim = 3;
constexpr uint32_t HistogramShifts = 3;

static ApusrFramegenDimensions2D GetOpticalFlowTextureSize(const ApusrFramegenDimensions2D& displaySize, const uint32_t opticalFlowBlockSize)
{
    uint32_t width = (displaySize.width + opticalFlowBlockSize - 1) / opticalFlowBlockSize;
    uint32_t height = (displaySize.height + opticalFlowBlockSize - 1) / opticalFlowBlockSize;
    return { width, height };
}

static ApusrFramegenDimensions2D GetOpticalFlowHistogramSize(int level)
{
    const uint32_t searchRadius = 8;
    uint32_t maxVelocity = searchRadius * (1 << (OpticalFlowMaxPyramidLevels - 1 - level));
    uint32_t binsPerDimension = 2 * maxVelocity + 1;
    return { binsPerDimension, binsPerDimension };
}

static ApusrFramegenDimensions2D GetGlobalMotionSearchDispatchSize(int level)
{
    const uint32_t threadGroupSizeX = 16;
    const uint32_t threadGroupSizeY = 16;
    const ApusrFramegenDimensions2D opticalFlowHistogramSize = GetOpticalFlowHistogramSize(level);
    const uint32_t additionalElementsDueToShiftsX = opticalFlowHistogramSize.width / threadGroupSizeX;
    const uint32_t additionalElementsDueToShiftsY = opticalFlowHistogramSize.height / threadGroupSizeY;
    const uint32_t dispatchX = (opticalFlowHistogramSize.width + additionalElementsDueToShiftsX + threadGroupSizeX - 1) / threadGroupSizeX;
    const uint32_t dispatchY = (opticalFlowHistogramSize.height + additionalElementsDueToShiftsY + threadGroupSizeY - 1) / threadGroupSizeY;
    return { dispatchX, dispatchY };
}

static uint32_t GetSCDHistogramTextureWidth()
{
    return HistogramBins * (HistogramsPerDim * HistogramsPerDim);
}

static void getInternalResourceDescriptions(const ApusrFramegenDimensions2D* displaySize, ApusrOpticalFlowInternalResourceDescriptions* internalResourceDescriptions)
{
    ApusrFramegenDimensions2D opticalFlowInputTextureSize = *displaySize;
    uint32_t minBlockSize = 8;
    const ApusrFramegenDimensions2D opticalFlowTextureSize = GetOpticalFlowTextureSize(opticalFlowInputTextureSize, minBlockSize);

    const ApusrFramegenDimensions2D opticalFlowLevel1TextureSize = { FFX_ALIGN_UP(opticalFlowTextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowTextureSize.height, 2) / 2 };
    const ApusrFramegenDimensions2D opticalFlowLevel2TextureSize = { FFX_ALIGN_UP(opticalFlowLevel1TextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowLevel1TextureSize.height, 2) / 2 };
    const ApusrFramegenDimensions2D opticalFlowLevel3TextureSize = { FFX_ALIGN_UP(opticalFlowLevel2TextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowLevel2TextureSize.height, 2) / 2 };
    const ApusrFramegenDimensions2D opticalFlowLevel4TextureSize = { FFX_ALIGN_UP(opticalFlowLevel3TextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowLevel3TextureSize.height, 2) / 2 };
    const ApusrFramegenDimensions2D opticalFlowLevel5TextureSize = { FFX_ALIGN_UP(opticalFlowLevel4TextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowLevel4TextureSize.height, 2) / 2 };
    const ApusrFramegenDimensions2D opticalFlowLevel6TextureSize = { FFX_ALIGN_UP(opticalFlowLevel5TextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowLevel5TextureSize.height, 2) / 2 };
    const ApusrFramegenDimensions2D opticalFlowLevel7TextureSize = { FFX_ALIGN_UP(opticalFlowLevel6TextureSize.width, 2) / 2, FFX_ALIGN_UP(opticalFlowLevel6TextureSize.height, 2) / 2 };

    // Fill out the internal resource descriptions for optical flow
    internalResourceDescriptions->opticalFlowInput1 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width, opticalFlowInputTextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput1Level1 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 1, opticalFlowInputTextureSize.height >> 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1Level1", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_1, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput1Level2 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 2, opticalFlowInputTextureSize.height >> 2, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1Level2", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_2, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput1Level3 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 3, opticalFlowInputTextureSize.height >> 3, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1Level3", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_3, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput1Level4 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 4, opticalFlowInputTextureSize.height >> 4, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1Level4", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_4, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput1Level5 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 5, opticalFlowInputTextureSize.height >> 5, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1Level5", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_5, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput1Level6 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 6, opticalFlowInputTextureSize.height >> 6, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput1Level6", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_6, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width, opticalFlowInputTextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2Level1 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 1, opticalFlowInputTextureSize.height >> 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2Level1", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_1, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2Level2 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 2, opticalFlowInputTextureSize.height >> 2, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2Level2", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_2, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2Level3 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 3, opticalFlowInputTextureSize.height >> 3, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2Level3", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_3, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2Level4 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 4, opticalFlowInputTextureSize.height >> 4, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2Level4", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_4, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2Level5 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 5, opticalFlowInputTextureSize.height >> 5, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2Level5", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_5, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlowInput2Level6 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT, opticalFlowInputTextureSize.width >> 6, opticalFlowInputTextureSize.height >> 6, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowInput2Level6", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_6, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowTextureSize.width, opticalFlowTextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1Level1 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel1TextureSize.width, opticalFlowLevel1TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1Level1", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1_LEVEL_1, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1Level2 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel2TextureSize.width, opticalFlowLevel2TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1Level2", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1_LEVEL_2, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1Level3 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel3TextureSize.width, opticalFlowLevel3TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1Level3", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1_LEVEL_3, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1Level4 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel4TextureSize.width, opticalFlowLevel4TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1Level4", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1_LEVEL_4, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1Level5 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel5TextureSize.width, opticalFlowLevel5TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1Level5", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1_LEVEL_5, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow1Level6 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel6TextureSize.width, opticalFlowLevel6TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow1Level6", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1_LEVEL_6, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowTextureSize.width, opticalFlowTextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2Level1 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel1TextureSize.width, opticalFlowLevel1TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2Level1", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2_LEVEL_1, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2Level2 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel2TextureSize.width, opticalFlowLevel2TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2Level2", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2_LEVEL_2, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2Level3 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel3TextureSize.width, opticalFlowLevel3TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2Level3", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2_LEVEL_3, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2Level4 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel4TextureSize.width, opticalFlowLevel4TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2Level4", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2_LEVEL_4, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2Level5 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel5TextureSize.width, opticalFlowLevel5TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2Level5", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2_LEVEL_5, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };
    internalResourceDescriptions->opticalFlow2Level6 = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowLevel6TextureSize.width, opticalFlowLevel6TextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlow2Level6", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2_LEVEL_6, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };

    internalResourceDescriptions->opticalFlowSCDHistogram = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, GetSCDHistogramTextureWidth(), 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowSCDHistogram", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_HISTOGRAM, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };

    internalResourceDescriptions->opticalFlowSCDPreviousHistogram = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT, GetSCDHistogramTextureWidth(), 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowSCDPreviousHistogram", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };

    internalResourceDescriptions->opticalFlowSCDTemp = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, 3, 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_OpticalFlowSCDTemp", FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_TEMP, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED}
    };

}

static void getSharedResourceDescriptions(const ApusrFramegenDimensions2D* displaySize, ApusrOpticalFlowSharedResourceDescriptions* sharedResourceDescriptions)
{
    uint32_t minBlockSize = 8;
    const ApusrFramegenDimensions2D opticalFlowTextureSize = GetOpticalFlowTextureSize(*displaySize, minBlockSize);

    sharedResourceDescriptions->opticalFlowVector = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT, opticalFlowTextureSize.width, opticalFlowTextureSize.height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_Result", 0, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };

    sharedResourceDescriptions->opticalFlowSCD = {
        APUSR_FRAMEGEN_HEAP_DEFAULT,
        { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT, 3, 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV },
        APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"OPTICALFLOW_SCDOutput", 0, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
}

static ApusrFramegenResult opticalflowRelease(ApusrOpticalFlowContextPrivate* context);

static ApusrFramegenResult opticalflowCreate(ApusrOpticalFlowContextPrivate* context, const ApusrOpticalFlowContextDescription* contextDescription)
{
    FFX_ASSERT(context);
    FFX_ASSERT(contextDescription);

    std::memset(context, 0, sizeof(ApusrOpticalFlowContextPrivate));
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
    context->resourceFrameIndex = 0;
    context->constants.inputLumaResolution[0] = context->contextDescription.resolution.width;
    context->constants.inputLumaResolution[1] = context->contextDescription.resolution.height;

    ApusrOpticalFlowInternalResourceDescriptions internalDescriptions{};
    getInternalResourceDescriptions(&contextDescription->resolution, &internalDescriptions);
    const ApusrFramegenCreateResourceDescription* resources[] = {
        &internalDescriptions.opticalFlowInput1,
        &internalDescriptions.opticalFlowInput1Level1,
        &internalDescriptions.opticalFlowInput1Level2,
        &internalDescriptions.opticalFlowInput1Level3,
        &internalDescriptions.opticalFlowInput1Level4,
        &internalDescriptions.opticalFlowInput1Level5,
        &internalDescriptions.opticalFlowInput1Level6,
        &internalDescriptions.opticalFlowInput2,
        &internalDescriptions.opticalFlowInput2Level1,
        &internalDescriptions.opticalFlowInput2Level2,
        &internalDescriptions.opticalFlowInput2Level3,
        &internalDescriptions.opticalFlowInput2Level4,
        &internalDescriptions.opticalFlowInput2Level5,
        &internalDescriptions.opticalFlowInput2Level6,
        &internalDescriptions.opticalFlow1,
        &internalDescriptions.opticalFlow1Level1,
        &internalDescriptions.opticalFlow1Level2,
        &internalDescriptions.opticalFlow1Level3,
        &internalDescriptions.opticalFlow1Level4,
        &internalDescriptions.opticalFlow1Level5,
        &internalDescriptions.opticalFlow1Level6,
        &internalDescriptions.opticalFlow2,
        &internalDescriptions.opticalFlow2Level1,
        &internalDescriptions.opticalFlow2Level2,
        &internalDescriptions.opticalFlow2Level3,
        &internalDescriptions.opticalFlow2Level4,
        &internalDescriptions.opticalFlow2Level5,
        &internalDescriptions.opticalFlow2Level6,
        &internalDescriptions.opticalFlowSCDHistogram,
        &internalDescriptions.opticalFlowSCDPreviousHistogram,
        &internalDescriptions.opticalFlowSCDTemp,
    };

    std::memset(context->resources, 0, sizeof(context->resources));
    for (const ApusrFramegenCreateResourceDescription* resource : resources) {
        result = backendContext.createResource(*resource, context->resources[resource->id]);
        if (result != APUSR_FRAMEGEN_OK) {
            opticalflowRelease(context);
            return result;
        }
    }

    std::memset(context->srvBindings, 0, sizeof(context->srvBindings));
    std::memset(context->uavBindings, 0, sizeof(context->uavBindings));
    context->refreshPipelineStates = false;
    result = createPipelineStates(context);
    if (result != APUSR_FRAMEGEN_OK)
        opticalflowRelease(context);
    return result;
}

static ApusrFramegenResult opticalflowRelease(ApusrOpticalFlowContextPrivate* context)
{
    FFX_ASSERT(context);

    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);

    backendContext.releasePipeline(context->pipelinePrepareLuma);
    backendContext.releasePipeline(context->pipelineGenerateOpticalFlowInputPyramid);
    backendContext.releasePipeline(context->pipelineGenerateSCDHistogram);
    backendContext.releasePipeline(context->pipelineComputeSCDDivergence);
    backendContext.releasePipeline(context->pipelineComputeOpticalFlowAdvancedV5);
    backendContext.releasePipeline(context->pipelineFilterOpticalFlowV5);
    backendContext.releasePipeline(context->pipelineScaleOpticalFlowAdvancedV5);

    for (int32_t currentResourceIndex = 0; currentResourceIndex < FFX_OF_RESOURCE_IDENTIFIER_COUNT; ++currentResourceIndex) {

        backendContext.releaseResource(context->resources[currentResourceIndex]);
    }

    backendContext.releaseContext();

    return APUSR_FRAMEGEN_OK;
}

static void scheduleDispatch(
    const apusr::framegen::BackendContext& backendContext,
    ApusrOpticalFlowContextPrivate* context,
    const ApusrFramegenPipeline* pipeline,
    const wchar_t* pipelineName,
    uint32_t dispatchX,
    uint32_t dispatchY,
    uint32_t dispatchZ = 1,
    uint32_t profileLevel = UINT32_MAX)
{
    ApusrFramegenGpuJob dispatchJob;
    dispatchJob.jobType = APUSR_FRAMEGEN_JOB_COMPUTE;
    ApusrFramegenComputeJob& jobDescriptor = dispatchJob.computeJob;
    jobDescriptor.pipeline = pipeline;
    jobDescriptor.cmdArgument = {};
    jobDescriptor.cmdArgumentOffset = 0;
    jobDescriptor.flags = 0;
    jobDescriptor.profileLevel = profileLevel;

    for (uint32_t currentShaderResourceViewIndex = 0; currentShaderResourceViewIndex < pipeline->srvTextureCount; ++currentShaderResourceViewIndex) {

        const uint32_t bindingIdentifier = pipeline->srvTextureBindings[currentShaderResourceViewIndex].resourceIdentifier;
        const ApusrFramegenResource currentResource = context->srvBindings[bindingIdentifier];
        jobDescriptor.srvTextures[currentShaderResourceViewIndex].resource = currentResource;

        FFX_ASSERT(bindingIdentifier != FFX_OF_BINDING_IDENTIFIER_NULL);
        FFX_ASSERT(bindingIdentifier < FFX_OF_BINDING_IDENTIFIER_COUNT);
    }

    for (uint32_t currentUnorderedAccessViewIndex = 0; currentUnorderedAccessViewIndex < pipeline->uavTextureCount; ++currentUnorderedAccessViewIndex) {

        const uint32_t bindingIdentifier = pipeline->uavTextureBindings[currentUnorderedAccessViewIndex].resourceIdentifier;
        const ApusrFramegenResource currentResource = context->uavBindings[bindingIdentifier];
        jobDescriptor.uavTextures[currentUnorderedAccessViewIndex].resource = currentResource;
        jobDescriptor.uavTextures[currentUnorderedAccessViewIndex].mip = 0;

        FFX_ASSERT(bindingIdentifier != FFX_OF_BINDING_IDENTIFIER_NULL);
        FFX_ASSERT(bindingIdentifier < FFX_OF_BINDING_IDENTIFIER_COUNT);
    }
    
    jobDescriptor.dimensions[0] = dispatchX;
    jobDescriptor.dimensions[1] = dispatchY;
    jobDescriptor.dimensions[2] = dispatchZ;
    jobDescriptor.pipeline = pipeline;

    for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < pipeline->constCount; ++currentRootConstantIndex) {
        jobDescriptor.constantBuffers[currentRootConstantIndex] = context->constantBuffers[pipeline->constantBufferBindings[currentRootConstantIndex].resourceIdentifier];
    }

#if FFX_DX12_ENABLE_GPU_MARKERS
    APUSR_FRAMEGEN_SET_JOB_LABEL(dispatchJob, pipelineName);
#endif

    backendContext.schedule(dispatchJob);
}

static ApusrFramegenResult dispatch(ApusrOpticalFlowContextPrivate* context, const ApusrOpticalFlowDispatchDescription* params)
{
    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);
    backendContext.registerResource(params->opticalFlowVector, context->uavBindings[FFX_OF_BINDING_IDENTIFIER_SHARED_OPTICAL_FLOW_VECTOR]);
    backendContext.registerResource(params->opticalFlowSCD, context->uavBindings[FFX_OF_BINDING_IDENTIFIER_SHARED_OPTICAL_FLOW_SCD_OUTPUT]);

    backendContext.registerResource(params->color, context->srvBindings[FFX_OF_BINDING_IDENTIFIER_INPUT_COLOR]);

    ApusrFramegenCommandList commandList = params->commandList;
    int advancedAlgorithmIterations = 7;
    uint32_t opticalFlowBlockSize = 8;

    if (context->refreshPipelineStates) {

        context->refreshPipelineStates = false;

        const ApusrFramegenResult result = createPipelineStates(context);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
    }

    const ApusrFramegenResourceDescription resourceDescInputColor = backendContext.resourceDescription(context->srvBindings[FFX_OF_BINDING_IDENTIFIER_INPUT_COLOR]);
    FFX_ASSERT(resourceDescInputColor.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D);

    context->constants.backbufferTransferFunction = params->backbufferTransferFunction;
    context->constants.minMaxLuminance[0] = params->minMaxLuminance.x;
    context->constants.minMaxLuminance[1] = params->minMaxLuminance.y;

    const bool resetAccumulation = params->reset || context->firstExecution;
    context->firstExecution = false;

    if (resetAccumulation) {
        context->constants.frameIndex = 0;
    }
    else {
        context->constants.frameIndex++;
    }

    if (resetAccumulation)
    {
        const float clearValuesToZeroFloat[]{ 0.f, 0.f, 0.f, 0.f };
        ApusrFramegenGpuJob clearJob = { APUSR_FRAMEGEN_JOB_CLEAR_FLOAT };
        std::memcpy(clearJob.clearJob.color, clearValuesToZeroFloat, 4 * sizeof(float));

        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow SCD Temp");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_TEMP];
        backendContext.schedule(clearJob);
        clearJob.clearJob.target = context->uavBindings[FFX_OF_BINDING_IDENTIFIER_SHARED_OPTICAL_FLOW_SCD_OUTPUT];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow SCD Histogram");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_HISTOGRAM];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow SCD Previous histogram");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1 Level 1");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_1];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1 Level 2");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_2];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1 Level 3");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_3];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1 Level 4");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_4];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1 Level 5");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_5];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 1 Level 6");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1_LEVEL_6];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2 Level 1");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_1];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2 Level 2");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_2];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2 Level 3");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_3];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2 Level 4");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_4];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2 Level 5");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_5];
        backendContext.schedule(clearJob);
        APUSR_FRAMEGEN_SET_JOB_LABEL(clearJob, L"Clear Optical Flow Input 2 Level 6");
        clearJob.clearJob.target = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2_LEVEL_6];
        backendContext.schedule(clearJob);
    }

    uint32_t resolutionMultiplier = 1;

    uint32_t threadGroupSizeOpticalFlowInputPyramid[2];
    uint32_t workGroupOffset[2];
    uint32_t numWorkGroupsAndMips[2];
    uint32_t rectInfo[4] = { 0, 0,
        context->contextDescription.resolution.width * resolutionMultiplier,
        context->contextDescription.resolution.height * resolutionMultiplier };
    ffxSpdSetup(threadGroupSizeOpticalFlowInputPyramid, workGroupOffset, numWorkGroupsAndMips, rectInfo, 4);

    OpticalFlowSpdConstants luminancePyramidConstants;
    luminancePyramidConstants.numworkGroups = numWorkGroupsAndMips[0];
    luminancePyramidConstants.mips = numWorkGroupsAndMips[1];
    luminancePyramidConstants.workGroupOffset[0] = workGroupOffset[0];
    luminancePyramidConstants.workGroupOffset[1] = workGroupOffset[1];
    luminancePyramidConstants.numworkGroupsOpticalFlowInputPyramid = numWorkGroupsAndMips[0];

    backendContext.stageConstants(&context->constants, sizeof(context->constants), context->constantBuffers[FFX_OPTICALFLOW_CONSTANTBUFFER_IDENTIFIER]);
    backendContext.stageConstants(&luminancePyramidConstants, sizeof(luminancePyramidConstants), context->constantBuffers[FFX_OPTICALFLOW_CONSTANTBUFFER_IDENTIFIER_SPD]);

    {
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_HISTOGRAM] = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_HISTOGRAM];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM] = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_TEMP] = context->resources[FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_SCD_TEMP];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_SCD_OUTPUT] = context->uavBindings[FFX_OF_BINDING_IDENTIFIER_SHARED_OPTICAL_FLOW_SCD_OUTPUT];

        const bool isOddFrame = !!(context->resourceFrameIndex & 1);

        uint32_t opticalFlowInputResourceIndex = isOddFrame ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1;
        uint32_t opticalFlowPreviousInputResourceIndex = isOddFrame ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2;

        uint32_t opticalFlowResourceIndex = isOddFrame ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1;
        uint32_t opticalFlowPreviousResourceIndex = isOddFrame ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2;

        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT] = context->resources[opticalFlowInputResourceIndex];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_1] = context->resources[opticalFlowInputResourceIndex + 1];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_2] = context->resources[opticalFlowInputResourceIndex + 2];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_3] = context->resources[opticalFlowInputResourceIndex + 3];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_4] = context->resources[opticalFlowInputResourceIndex + 4];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_5] = context->resources[opticalFlowInputResourceIndex + 5];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT_LEVEL_6] = context->resources[opticalFlowInputResourceIndex + 6];

        context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT] = context->resources[opticalFlowInputResourceIndex];
        context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS_INPUT] = context->resources[opticalFlowPreviousInputResourceIndex];

        context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW] = context->resources[opticalFlowResourceIndex];
        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW] = context->resources[opticalFlowResourceIndex];
        context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS] = context->resources[opticalFlowPreviousResourceIndex];

        {
            int32_t threadGroupSizeX = 16;
            int32_t threadGroupSizeY = 16;
            uint32_t threadPixelsX = 2;
            uint32_t threadPixelsY = 2;
            int32_t dispatchX = ((context->contextDescription.resolution.width + (threadPixelsX - 1)) / threadPixelsX + (threadGroupSizeX - 1)) / threadGroupSizeX;
            int32_t dispatchY = ((context->contextDescription.resolution.height + (threadPixelsY - 1)) / threadPixelsY + (threadGroupSizeY - 1)) / threadGroupSizeY;
            scheduleDispatch(backendContext, context, &context->pipelinePrepareLuma, L"OF PrepareLuma", dispatchX, dispatchY);
        }

        {
            {
                scheduleDispatch(backendContext, context,
                                 &context->pipelineGenerateOpticalFlowInputPyramid,
                                 L"OF GenerateOpticalFlowInputPyramid",
                                 threadGroupSizeOpticalFlowInputPyramid[0],
                                 threadGroupSizeOpticalFlowInputPyramid[1]
                );
            }

            {
                {
                    const uint32_t threadGroupSizeX = 32;
                    const uint32_t threadGroupSizeY = 8;
                    const uint32_t strataWidth = (context->contextDescription.resolution.width / 4) / HistogramsPerDim;
                    const uint32_t strataHeight = context->contextDescription.resolution.height / HistogramsPerDim;
                    const uint32_t dispatchX = (strataWidth + threadGroupSizeX - 1) / threadGroupSizeX;
                    const uint32_t dispatchY = 16;
                    const uint32_t dispatchZ = HistogramsPerDim * HistogramsPerDim;
                    scheduleDispatch(backendContext, context, &context->pipelineGenerateSCDHistogram, L"OF GenerateSCDHistogram", dispatchX, dispatchY, dispatchZ);
                }
                {
                    const uint32_t dispatchX = HistogramsPerDim * HistogramsPerDim;
                    const uint32_t dispatchY = HistogramShifts;
                    scheduleDispatch(backendContext, context, &context->pipelineComputeSCDDivergence, L"OF ComputeSCDDivergence", dispatchX, dispatchY);
                }
            }

            ApusrFramegenDimensions2D opticalFlowTextureSizes[OpticalFlowMaxPyramidLevels];
            const int pyramidMaxIterations = advancedAlgorithmIterations;
            FFX_ASSERT(pyramidMaxIterations <= OpticalFlowMaxPyramidLevels);

            opticalFlowTextureSizes[0] = GetOpticalFlowTextureSize(context->contextDescription.resolution, opticalFlowBlockSize);
            for (int i = 1; i < pyramidMaxIterations; i++)
            {
                opticalFlowTextureSizes[i] = {
                    (opticalFlowTextureSizes[i - 1].width + 1) / 2,
                    (opticalFlowTextureSizes[i - 1].height + 1) / 2
                };
            }

            for (int level = pyramidMaxIterations - 1; level >= 0; level--)
            {
                bool isOddLevel = !!(level & 1);

                uint32_t opticalFlowInputResourceIndexA = isOddFrame ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1;
                uint32_t opticalFlowInputResourceIndexB = isOddFrame ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_1 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_INPUT_2;
                uint32_t opticalFlowResourceIndexA = (isOddFrame != isOddLevel) ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1;
                uint32_t opticalFlowResourceIndexB = (isOddFrame != isOddLevel) ? FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_1 : FFX_OF_RESOURCE_IDENTIFIER_OPTICAL_FLOW_2;
                context->constants.opticalFlowPyramidLevel = level;
                context->constants.opticalFlowPyramidLevelCount = pyramidMaxIterations;

                backendContext.stageConstants(&context->constants, sizeof(context->constants), context->constantBuffers[FFX_OPTICALFLOW_CONSTANTBUFFER_IDENTIFIER]);

                context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_INPUT] = context->resources[opticalFlowInputResourceIndexA + level];
                context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS_INPUT] = context->resources[opticalFlowInputResourceIndexB + level];
                context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW] = context->resources[opticalFlowResourceIndexA + level];

                context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS] = context->resources[opticalFlowResourceIndexB + level];

                {
                    const uint32_t inputLumaWidth = ffxMax(context->contextDescription.resolution.width >> level, 1);
                    const uint32_t inputLumaHeight = ffxMax(context->contextDescription.resolution.height >> level, 1);
                    static constexpr const wchar_t* kSearchLabels[OpticalFlowMaxPyramidLevels] = {
                        L"OF 0 Search", L"OF 1 Search", L"OF 2 Search", L"OF 3 Search",
                        L"OF 4 Search", L"OF 5 Search", L"OF 6 Search"};

                    {
                        uint32_t threadPixels = 4;
                        FFX_ASSERT(opticalFlowBlockSize >= threadPixels);
                        uint32_t threadGroupSizeY = 16;
                        uint32_t threadGroupSize = 64;
                        uint32_t dispatchX = ((inputLumaWidth + threadPixels - 1) / threadPixels * threadGroupSizeY + (threadGroupSize - 1)) / threadGroupSize;
                        uint32_t dispatchY = (inputLumaHeight + (threadGroupSizeY - 1)) / threadGroupSizeY;
                        scheduleDispatch(backendContext, context, &context->pipelineComputeOpticalFlowAdvancedV5, kSearchLabels[level], dispatchX, dispatchY, 1, level);
                    }
                }

                {
                    context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_PREVIOUS] = context->resources[opticalFlowResourceIndexA + level];
                    context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW] = context->resources[opticalFlowResourceIndexB + level];
                }

                {
                    if (level == 0)
                    {
                        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW] = context->uavBindings[FFX_OF_BINDING_IDENTIFIER_SHARED_OPTICAL_FLOW_VECTOR];
                    }

                    const uint32_t levelWidth = opticalFlowTextureSizes[level].width;
                    const uint32_t levelHeight = opticalFlowTextureSizes[level].height;

                    const uint32_t threadGroupSizeX = 16;
                    const uint32_t threadGroupSizeY = 4;
                    const uint32_t dispatchX = (levelWidth + threadGroupSizeX - 1) / threadGroupSizeX;
                    const uint32_t dispatchY = (levelHeight + threadGroupSizeY - 1) / threadGroupSizeY;
                    static constexpr const wchar_t* kFilterLabels[OpticalFlowMaxPyramidLevels] = {
                        L"OF 0 Filter", L"OF 1 Filter", L"OF 2 Filter", L"OF 3 Filter",
                        L"OF 4 Filter", L"OF 5 Filter", L"OF 6 Filter"};

                    {
                        scheduleDispatch(backendContext, context, &context->pipelineFilterOpticalFlowV5, kFilterLabels[level], dispatchX, dispatchY, 1, level);
                    }
                }

                if (level > 0)
                {
                    context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_ALIAS_LEVEL_1 + level - 1] = context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW];
                }

                if (level > 0)
                {
                    {
                        context->srvBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW] = context->resources[opticalFlowResourceIndexB + level];
                        context->uavBindings[FFX_OF_BINDING_IDENTIFIER_OPTICAL_FLOW_NEXT_LEVEL] = level > 0 ? context->resources[opticalFlowResourceIndexB + level - 1] : ApusrFramegenResource{ FFX_OF_RESOURCE_IDENTIFIER_NULL };
                    }

                    FFX_ASSERT(opticalFlowBlockSize >= 2);
                    const uint32_t nextLevelWidth = opticalFlowTextureSizes[level - 1].width;
                    const uint32_t nextLevelHeight = opticalFlowTextureSizes[level - 1].height;

                    const uint32_t threadGroupSizeX = opticalFlowBlockSize / 2;
                    const uint32_t threadGroupSizeY = opticalFlowBlockSize / 2;
                    const uint32_t threadGroupSizeZ = 4;
                    const uint32_t dispatchX = (nextLevelWidth + threadGroupSizeX - 1) / threadGroupSizeX;
                    const uint32_t dispatchY = (nextLevelHeight + threadGroupSizeY - 1) / threadGroupSizeY;
                    const uint32_t dispatchZ = 1;
                    static constexpr const wchar_t* kScaleLabels[OpticalFlowMaxPyramidLevels] = {
                        L"OF 0 Scale", L"OF 1 Scale", L"OF 2 Scale", L"OF 3 Scale",
                        L"OF 4 Scale", L"OF 5 Scale", L"OF 6 Scale"};

                    {
                        const uint32_t dispatchX = (nextLevelWidth + 3) / 4;
                        const uint32_t dispatchY = (nextLevelHeight + 3) / 4;
                        scheduleDispatch(backendContext, context, &context->pipelineScaleOpticalFlowAdvancedV5, kScaleLabels[level], dispatchX, dispatchY, dispatchZ, level);
                    }

                }

            }
        }
    }

    // Both input pyramids finish in compute-read state. Leave them there: the next
    // frame transitions only its new write target to UAV, while the history pyramid
    // remains directly readable. Proactively forcing both ping-pong sets to UAV
    // creates fourteen transitions here and seven avoidable transitions next frame.

    context->resourceFrameIndex = (context->resourceFrameIndex + 1) % FFX_OPTICALFLOW_MAX_QUEUED_FRAMES;

    const ApusrFramegenResult executeResult = backendContext.execute(commandList);
    if (executeResult != APUSR_FRAMEGEN_OK)
        return executeResult;

    backendContext.unregisterResources(commandList);
    return APUSR_FRAMEGEN_OK;
}

namespace apusr::framegen::algorithm {

ApusrFramegenResult opticalFlowContextCreateStorage(void* contextStorage, const ApusrOpticalFlowContextDescription* description) noexcept
{
    if (!contextStorage || !description)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (!description->backendInterface.getVersion || !description->backendInterface.getDeviceCapabilities ||
        !description->backendInterface.createContext || !description->backendInterface.destroyContext)
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    if (description->backendInterface.scratchBuffer && !description->backendInterface.scratchBufferSize)
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;

    auto* context = static_cast<ApusrOpticalFlowContextPrivate*>(contextStorage);
    return opticalflowCreate(context, description);
}

ApusrFramegenResult opticalFlowContextDestroyStorage(void* contextStorage) noexcept
{
    if (!contextStorage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    return opticalflowRelease(static_cast<ApusrOpticalFlowContextPrivate*>(contextStorage));
}

ApusrFramegenResult opticalFlowContextGetGpuMemoryUsageStorage(void* contextStorage, ApusrFramegenMemoryUsage* usage) noexcept
{
    if (!contextStorage || !usage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    auto* context = static_cast<ApusrOpticalFlowContextPrivate*>(contextStorage);
    if (!context->device)
        return APUSR_FRAMEGEN_ERROR_NULL_DEVICE;
    const apusr::framegen::BackendContext backendContext(context->contextDescription.backendInterface, context->effectContextId);
    return backendContext.gpuMemoryUsage(*usage);
}

ApusrFramegenResult opticalFlowGetSharedResourceDescriptionsStorage(void* contextStorage, ApusrOpticalFlowSharedResourceDescriptions* descriptions) noexcept
{
    if (!contextStorage || !descriptions)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    auto* context = static_cast<ApusrOpticalFlowContextPrivate*>(contextStorage);
    getSharedResourceDescriptions(&context->contextDescription.resolution, descriptions);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult opticalFlowDispatchStorage(void* contextStorage, const ApusrOpticalFlowDispatchDescription* description) noexcept
{
    if (!contextStorage || !description || !description->commandList || !description->color.resource ||
        !description->opticalFlowVector.resource || !description->opticalFlowSCD.resource)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;

    auto* context = static_cast<ApusrOpticalFlowContextPrivate*>(contextStorage);
    if (!context->device)
        return APUSR_FRAMEGEN_ERROR_NULL_DEVICE;
    if (description->color.description.type != APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D ||
        description->color.description.width > context->contextDescription.resolution.width ||
        description->color.description.height > context->contextDescription.resolution.height)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    return dispatch(context, description);
}

} // namespace apusr::framegen::algorithm


ApusrFramegenResult apusrOpticalFlowContextCreate(ApusrOpticalFlowContext* context, const ApusrOpticalFlowContextDescription* description) noexcept
{
    return context ? apusr::framegen::algorithm::opticalFlowContextCreateStorage(context->data, description) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrOpticalFlowContextDestroy(ApusrOpticalFlowContext* context) noexcept
{
    return context ? apusr::framegen::algorithm::opticalFlowContextDestroyStorage(context->data) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrOpticalFlowContextGetGpuMemoryUsage(ApusrOpticalFlowContext* context, ApusrFramegenMemoryUsage* usage) noexcept
{
    return context ? apusr::framegen::algorithm::opticalFlowContextGetGpuMemoryUsageStorage(context->data, usage) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrOpticalFlowGetSharedResourceDescriptions(
    ApusrOpticalFlowContext* context,
    ApusrOpticalFlowSharedResourceDescriptions* descriptions) noexcept
{
    return context ? apusr::framegen::algorithm::opticalFlowGetSharedResourceDescriptionsStorage(context->data, descriptions) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrOpticalFlowDispatch(
    ApusrOpticalFlowContext* context,
    const ApusrOpticalFlowDispatchDescription* description) noexcept
{
    return context ? apusr::framegen::algorithm::opticalFlowDispatchStorage(context->data, description) : APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

ApusrFramegenResult apusrOpticalFlowGetGpuMemoryUsage(
    ApusrFramegenDevice device,
    ApusrFramegenDimensions2D displaySize,
    ApusrFramegenMemoryUsage* usage) noexcept
{
    if (!device || !usage)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;

    ApusrOpticalFlowInternalResourceDescriptions opticalflowInternalResourceDescs{};
    getInternalResourceDescriptions(&displaySize, &opticalflowInternalResourceDescs);

    const ApusrFramegenCreateResourceDescription* resources[] = {
        &opticalflowInternalResourceDescs.opticalFlowInput1,
        &opticalflowInternalResourceDescs.opticalFlowInput1Level1,
        &opticalflowInternalResourceDescs.opticalFlowInput1Level2,
        &opticalflowInternalResourceDescs.opticalFlowInput1Level3,
        &opticalflowInternalResourceDescs.opticalFlowInput1Level4,
        &opticalflowInternalResourceDescs.opticalFlowInput1Level5,
        &opticalflowInternalResourceDescs.opticalFlowInput1Level6,
        &opticalflowInternalResourceDescs.opticalFlowInput2,
        &opticalflowInternalResourceDescs.opticalFlowInput2Level1,
        &opticalflowInternalResourceDescs.opticalFlowInput2Level2,
        &opticalflowInternalResourceDescs.opticalFlowInput2Level3,
        &opticalflowInternalResourceDescs.opticalFlowInput2Level4,
        &opticalflowInternalResourceDescs.opticalFlowInput2Level5,
        &opticalflowInternalResourceDescs.opticalFlowInput2Level6,
        &opticalflowInternalResourceDescs.opticalFlow1,
        &opticalflowInternalResourceDescs.opticalFlow1Level1,
        &opticalflowInternalResourceDescs.opticalFlow1Level2,
        &opticalflowInternalResourceDescs.opticalFlow1Level3,
        &opticalflowInternalResourceDescs.opticalFlow1Level4,
        &opticalflowInternalResourceDescs.opticalFlow1Level5,
        &opticalflowInternalResourceDescs.opticalFlow1Level6,
        &opticalflowInternalResourceDescs.opticalFlow2,
        &opticalflowInternalResourceDescs.opticalFlow2Level1,
        &opticalflowInternalResourceDescs.opticalFlow2Level2,
        &opticalflowInternalResourceDescs.opticalFlow2Level3,
        &opticalflowInternalResourceDescs.opticalFlow2Level4,
        &opticalflowInternalResourceDescs.opticalFlow2Level5,
        &opticalflowInternalResourceDescs.opticalFlow2Level6,
        &opticalflowInternalResourceDescs.opticalFlowSCDHistogram,
        &opticalflowInternalResourceDescs.opticalFlowSCDPreviousHistogram,
        &opticalflowInternalResourceDescs.opticalFlowSCDTemp
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

    ApusrOpticalFlowSharedResourceDescriptions sharedDescriptions{};
    getSharedResourceDescriptions(&displaySize, &sharedDescriptions);
    const ApusrFramegenCreateResourceDescription* sharedResources[] = {
        &sharedDescriptions.opticalFlowVector,
        &sharedDescriptions.opticalFlowSCD
    };

    for (const auto* resource : sharedResources)
    {
        const ApusrFramegenResult result = apusr::framegen::resourceAllocationSize(device, *resource, size);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
        usage->totalUsageInBytes += size;
        if (resource->resourceDescription.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE)
            usage->aliasableUsageInBytes += size;
    }

    return APUSR_FRAMEGEN_OK;
}


#ifdef _MSC_VER
#pragma warning(pop)
#endif
