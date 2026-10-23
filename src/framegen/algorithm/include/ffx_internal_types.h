// APUSR-maintained FrameGen compatibility code derived from the FidelityFX SDK.
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

#include "ffx_api_types.h"

#include <stdint.h>

#if defined(FFX_GCC) || !defined(FFX_BUILD_AS_DLL)
#define FFX_API
#else
#define FFX_API __declspec(dllexport)
#endif

// Legacy FI/OF wrappers must retain the FidelityFX opaque-context ABI.
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128)

// Retained for the optional FidelityFX-derived DX12 marker path.
#define FFX_RESOURCE_NAME_SIZE 64

#ifdef _MSC_VER
#pragma warning(disable : 4365 4710 4820 5039)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef bool FfxBoolean;
typedef uint8_t FfxUInt8;
typedef uint16_t FfxUInt16;
typedef uint32_t FfxUInt32;
typedef uint64_t FfxUInt64;
typedef int8_t FfxInt8;
typedef int16_t FfxInt16;
typedef int32_t FfxInt32;
typedef int64_t FfxInt64;
typedef float FfxFloat32;
typedef float FfxFloat32x2[2];
typedef float FfxFloat32x3[3];
typedef float FfxFloat32x4[4];
typedef float FfxFloat32x4x4[16];
typedef uint32_t FfxUInt32x2[2];
typedef uint32_t FfxUInt32x3[3];
typedef uint32_t FfxUInt32x4[4];
typedef int32_t FfxInt32x2[2];
typedef int32_t FfxInt32x3[3];
typedef int32_t FfxInt32x4[4];

typedef void (*ffxMessageCallback)(uint32_t type, const wchar_t* message);

typedef enum FfxMsgType {
    FFX_MESSAGE_TYPE_ERROR = 0,
    FFX_MESSAGE_TYPE_WARNING = 1,
    FFX_MESSAGE_TYPE_COUNT
} FfxMsgType;

// Opaque handles retained only for the FidelityFX-facing ABI/presentation façade.
typedef void* FfxDevice;
typedef void* FfxCommandQueue;
typedef void* FfxSwapchain;
typedef void* FfxCommandList;

typedef struct FfxPresentCallbackDescription
{
    FfxDevice       device;
    FfxCommandList  commandList;
    FfxApiResource  currentBackBuffer;
    FfxApiResource  currentUI;
    FfxApiResource  outputSwapChainBuffer;
    bool            isInterpolatedFrame;
    bool            usePremulAlpha;
    uint64_t        frameID;
} FfxPresentCallbackDescription;

typedef struct FfxFrameGenerationDispatchDescription {
    FfxCommandList                     commandList;
    FfxApiResource                     presentColor;
    FfxApiResource                     outputs[4];
    uint32_t                           numInterpolatedFrames;
    bool                               reset;
    FfxApiBackbufferTransferFunction  backBufferTransferFunction;
    float                              minMaxLuminance[2];
    FfxApiRect2D                       interpolationRect;
    uint64_t                           frameID;
} FfxFrameGenerationDispatchDescription;

#ifdef __cplusplus
}
#endif
