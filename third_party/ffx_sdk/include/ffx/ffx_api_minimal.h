// This file is a minimal FidelityFX SDK ABI import for APUSR compatibility frontends.
// It is derived from the public FidelityFX SDK headers and remains governed by
// the FidelityFX SDK license preserved in third_party/ffx_sdk/LICENSE.md.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) && defined(APUSR_FSR4_DX12_FRONTEND_EXPORTS)
#define FFX_API_ENTRY __declspec(dllexport)
#elif defined(_WIN32)
#define FFX_API_ENTRY __declspec(dllimport)
#else
#define FFX_API_ENTRY __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum FfxApiReturnCodes {
    FFX_API_RETURN_OK = 0,
    FFX_API_RETURN_ERROR = 1,
    FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
    FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,
    FFX_API_RETURN_NO_PROVIDER = 4,
    FFX_API_RETURN_ERROR_MEMORY = 5,
    FFX_API_RETURN_ERROR_PARAMETER = 6,
    FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE = 7,
};

typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
typedef uint64_t ffxStructType_t;

typedef struct ffxApiHeader {
    ffxStructType_t type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxConfigureDescHeader;
typedef ffxApiHeader ffxQueryDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

enum FfxApiSurfaceFormat {
    FFX_API_SURFACE_FORMAT_UNKNOWN,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT,
    FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
    FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT,
    FFX_API_SURFACE_FORMAT_R32G32_FLOAT,
    FFX_API_SURFACE_FORMAT_R8_UINT,
    FFX_API_SURFACE_FORMAT_R32_UINT,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB,
    FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT,
    FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM,
    FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
    FFX_API_SURFACE_FORMAT_R16G16_UINT,
    FFX_API_SURFACE_FORMAT_R16G16_SINT,
    FFX_API_SURFACE_FORMAT_R16_FLOAT,
    FFX_API_SURFACE_FORMAT_R16_UINT,
    FFX_API_SURFACE_FORMAT_R16_UNORM,
    FFX_API_SURFACE_FORMAT_R16_SNORM,
    FFX_API_SURFACE_FORMAT_R8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8_UINT,
    FFX_API_SURFACE_FORMAT_R32_FLOAT,
    FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP,
    FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS,
    FFX_API_SURFACE_FORMAT_R32G32_TYPELESS,
    FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS,
    FFX_API_SURFACE_FORMAT_R16G16_TYPELESS,
    FFX_API_SURFACE_FORMAT_R16_TYPELESS,
    FFX_API_SURFACE_FORMAT_R8_TYPELESS,
    FFX_API_SURFACE_FORMAT_R8G8_TYPELESS,
    FFX_API_SURFACE_FORMAT_R32_TYPELESS,
};

enum FfxApiResourceState {
    FFX_API_RESOURCE_STATE_COMMON = (1 << 0),
    FFX_API_RESOURCE_STATE_UNORDERED_ACCESS = (1 << 1),
    FFX_API_RESOURCE_STATE_COMPUTE_READ = (1 << 2),
    FFX_API_RESOURCE_STATE_PIXEL_READ = (1 << 3),
    FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ = (FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COMPUTE_READ),
    FFX_API_RESOURCE_STATE_COPY_SRC = (1 << 4),
    FFX_API_RESOURCE_STATE_COPY_DEST = (1 << 5),
    FFX_API_RESOURCE_STATE_GENERIC_READ = (FFX_API_RESOURCE_STATE_COPY_SRC | FFX_API_RESOURCE_STATE_COMPUTE_READ),
    FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT = (1 << 6),
    FFX_API_RESOURCE_STATE_PRESENT = (1 << 7),
    FFX_API_RESOURCE_STATE_RENDER_TARGET = (1 << 8),
    FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT = (1 << 9),
};

enum FfxApiResourceType {
    FFX_API_RESOURCE_TYPE_BUFFER,
    FFX_API_RESOURCE_TYPE_TEXTURE1D,
    FFX_API_RESOURCE_TYPE_TEXTURE2D,
    FFX_API_RESOURCE_TYPE_TEXTURE_CUBE,
    FFX_API_RESOURCE_TYPE_TEXTURE3D,
};

struct FfxApiDimensions2D {
    uint32_t width;
    uint32_t height;
};

struct FfxApiFloatCoords2D {
    float x;
    float y;
};

struct FfxApiResourceDescription {
    uint32_t type;
    uint32_t format;
    union {
        uint32_t width;
        uint32_t size;
    };
    union {
        uint32_t height;
        uint32_t stride;
    };
    union {
        uint32_t depth;
        uint32_t alignment;
    };
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct FfxApiResource {
    void* resource;
    struct FfxApiResourceDescription description;
    uint32_t state;
};

typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);
typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);
typedef void (*ffxDealloc)(void* pUserData, void* pMem);

typedef struct ffxAllocationCallbacks {
    void* pUserData;
    ffxAlloc alloc;
    ffxDealloc dealloc;
} ffxAllocationCallbacks;

#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS 4u
struct ffxQueryDescGetVersions {
    ffxQueryDescHeader header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

#define FFX_API_DESC_TYPE_OVERRIDE_VERSION 5u
struct ffxOverrideVersion {
    ffxApiHeader header;
    uint64_t versionId;
};

#define FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION 6u
struct ffxQueryGetProviderVersion {
    ffxQueryDescHeader header;
    uint64_t versionId;
    const char* versionName;
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x0000002u
struct ffxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    void* device;
};

#define FFX_API_EFFECT_MASK 0x00ff0000u
#define FFX_API_EFFECT_ID_UPSCALE 0x00010000u
#define FFX_API_EFFECT_ID_FRAMEGENERATION 0x00020000u
#define FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN 0x00030000u
#define FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_VK 0x00040000u
#define FFX_API_MAKE_EFFECT_SUB_ID(effectId, subversion) ((effectId & FFX_API_EFFECT_MASK) | (subversion & ~FFX_API_EFFECT_MASK))

enum FfxApiUpscaleFlags {
    FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = (1 << 0),
    FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1 << 1),
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),
    FFX_UPSCALE_ENABLE_DEPTH_INVERTED = (1 << 3),
    FFX_UPSCALE_ENABLE_DEPTH_INFINITE = (1 << 4),
    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = (1 << 5),
    FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION = (1 << 6),
    FFX_UPSCALE_ENABLE_DEBUG_CHECKING = (1 << 7),
    FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE = (1 << 8),
    FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION = (1 << 9),
};

enum FfxApiUpscaleDispatchFlags {
    FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW = (1 << 0),
    FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB = (1 << 1),
    FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ = (1 << 2),
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE 0x00010000u
struct ffxCreateContextDescUpscale {
    ffxCreateContextDescHeader header;
    uint32_t flags;
    struct FfxApiDimensions2D maxRenderSize;
    struct FfxApiDimensions2D maxUpscaleSize;
    ffxApiMessage fpMessage;
};

#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE 0x00010001u
struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void* commandList;
    struct FfxApiResource color;
    struct FfxApiResource depth;
    struct FfxApiResource motionVectors;
    struct FfxApiResource exposure;
    struct FfxApiResource reactive;
    struct FfxApiResource transparencyAndComposition;
    struct FfxApiResource output;
    struct FfxApiFloatCoords2D jitterOffset;
    struct FfxApiFloatCoords2D motionVectorScale;
    struct FfxApiDimensions2D renderSize;
    struct FfxApiDimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
    float viewSpaceToMetersFactor;
    uint32_t flags;
};


#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE 0x00010002u
struct ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode {
    ffxQueryDescHeader header;
    uint32_t qualityMode;
    float* pOutUpscaleRatio;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE 0x00010003u
struct ffxQueryDescUpscaleGetRenderResolutionFromQualityMode {
    ffxQueryDescHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT 0x00010004u
struct ffxQueryDescUpscaleGetJitterPhaseCount {
    ffxQueryDescHeader header;
    uint32_t renderWidth;
    uint32_t displayWidth;
    int32_t* pOutPhaseCount;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET 0x00010005u
struct ffxQueryDescUpscaleGetJitterOffset {
    ffxQueryDescHeader header;
    int32_t index;
    int32_t phaseCount;
    float* pOutX;
    float* pOutY;
};

struct FfxApiEffectMemoryUsage {
    uint64_t totalUsageInBytes;
    uint64_t aliasableUsageInBytes;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE 0x00010008u
struct ffxQueryDescUpscaleGetGPUMemoryUsage {
    ffxQueryDescHeader header;
    struct FfxApiEffectMemoryUsage* gpuMemoryUsageUpscaler;
};

#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2 0x00010009u
struct ffxQueryDescUpscaleGetGPUMemoryUsageV2 {
    ffxQueryDescHeader header;
    void* device;
    struct FfxApiDimensions2D maxRenderSize;
    struct FfxApiDimensions2D maxUpscaleSize;
    uint32_t flags;
    struct FfxApiEffectMemoryUsage* gpuMemoryUsageUpscaler;
};

enum FfxApiQueryResourceIdentifiers {
    FFX_API_QUERY_RESOURCE_INPUT_COLOR = (1 << 0),
    FFX_API_QUERY_RESOURCE_INPUT_DEPTH = (1 << 1),
    FFX_API_QUERY_RESOURCE_INPUT_MV = (1 << 2),
    FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE = (1 << 3),
    FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK = (1 << 4),
    FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION = (1 << 5),
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x01u)
#define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x02u)
#define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x03u)
#define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x04u)
#define FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x05u)
#define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x06u)
#define FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x07u)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_FRAMEGENERATION, 0x09u)
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS 0x0001000au
struct ffxQueryDescUpscaleGetResourceRequirements {
    ffxQueryDescHeader header;
    uint64_t required_resources;
    uint64_t optional_resources;
};

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);
FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb);
FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc);
FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc);
FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc);

#ifdef __cplusplus
}
#endif
