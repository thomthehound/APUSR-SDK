// This file is part of the FidelityFX SDK.
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

#define GROUP_SIZE 8
#define FSR_RCAS_DENOISE 1
#define FSR4UPSCALER_BIND_CB_RCAS 0

#ifndef FFX_GPU
#define FFX_GPU
#endif
#ifndef FFX_HLSL
#define FFX_HLSL
#endif
#include "ffx_core.h"
#include "fsr4/ffx_fsr4_upscale_resources.h"
#include "fsr4/ffx_fsr4_gpu_colorspace_common.h"

#define FSR_RCAS_F 1

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p)
{
    uint width = 0;
    uint height = 0;
    r_rcas_input.GetDimensions(width, height);
    const int2 maxCoord = int2(max(width, 1u) - 1u, max(height, 1u) - 1u);
    p = clamp(p, int2(0, 0), maxCoord);
    return float4(r_rcas_input[p], 0.f);
}

void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "fsr1/ffx_fsr1.h"

void CurrFilter(FFX_MIN16_U2 pos)
{
    const uint2 px = uint2(pos);
    const uint width = outputWidth != 0u ? outputWidth : 1u;
    const uint height = outputHeight != 0u ? outputHeight : 1u;
    if (px.x >= width || px.y >= height)
        return;

    FfxFloat32x3 c;
    FsrRcasF(c.r, c.g, c.b, px, rcasConfig);

    c = RemoveMuLaw(c.rgb) / LoadExposureValue();

    rw_rcas_output[px] = c.rgb;
}

void RCAS(FfxUInt32x3 LocalThreadId, FfxUInt32x3 WorkGroupId, FfxUInt32x3 Dtid)
{
    // Do remapping of local xy in workgroup for a more PS-like swizzle pattern.
    FfxUInt32x2 gxy = ffxRemapForQuad(LocalThreadId.x) + FfxUInt32x2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);
    CurrFilter(FFX_MIN16_U2(gxy));
    gxy.x += 8u;
    CurrFilter(FFX_MIN16_U2(gxy));
    gxy.y += 8u;
    CurrFilter(FFX_MIN16_U2(gxy));
    gxy.x -= 8u;
    CurrFilter(FFX_MIN16_U2(gxy));
}

[numthreads(64, 1, 1)]
void main(uint3 LocalThreadId : SV_GroupThreadID, uint3 WorkGroupId : SV_GroupID, uint3 Dtid : SV_DispatchThreadID)
{
    RCAS(LocalThreadId, WorkGroupId, Dtid);
}
