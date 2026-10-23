// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../common/apusr_auto_exposure.hlsli"

RWByteAddressBuffer ApusrAutoExposureTiles : register(u0);
RWTexture2D<float> ApusrAutoExposureTexture : register(u1);
ConstantBuffer<ApusrAutoExposureConstants> ApusrAutoExposure : register(b0);

groupshared float gLogLuma[256];
groupshared uint gSampleCount[256];

[numthreads(256, 1, 1)]
void main(uint groupIndex : SV_GroupIndex)
{
    float logLuma = 0.0f;
    uint sampleCount = 0u;

    for (uint tileIndex = groupIndex; tileIndex < ApusrAutoExposure.tileCount; tileIndex += 256u) {
        const uint2 packed = ApusrAutoExposureTiles.Load2(tileIndex * 8u);
        logLuma += asfloat(packed.x);
        sampleCount += packed.y;
    }

    gLogLuma[groupIndex] = logLuma;
    gSampleCount[groupIndex] = sampleCount;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (groupIndex < stride) {
            gLogLuma[groupIndex] += gLogLuma[groupIndex + stride];
            gSampleCount[groupIndex] += gSampleCount[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0u) {
        const float averageLogLuma = gLogLuma[0] / max(float(gSampleCount[0]), 1.0f);
        const float previousExposure = ApusrAutoExposureTexture[int2(0, 0)];
        ApusrAutoExposureTexture[int2(0, 0)] = ApusrExposureFromAverageLogLuma(averageLogLuma);
        ApusrAutoExposureTexture[int2(1, 0)] = previousExposure;
    }
}
