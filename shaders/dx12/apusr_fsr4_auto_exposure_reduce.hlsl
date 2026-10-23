// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../common/apusr_token_layout.hlsli"
#include "../common/apusr_auto_exposure.hlsli"

Texture2D<float4> ApusrInputColor : register(t0);
RWByteAddressBuffer ApusrAutoExposureTiles : register(u0);
ConstantBuffer<ApusrAutoExposureConstants> ApusrAutoExposure : register(b0);

groupshared float gLogLuma[256];
groupshared uint gSampleCount[256];

[numthreads(16, 16, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex)
{
    float logLuma = 0.0f;
    uint sampleCount = 0u;

    if (dispatchThreadId.x < ApusrAutoExposure.renderWidth && dispatchThreadId.y < ApusrAutoExposure.renderHeight) {
        const float3 color = ApusrInputColor.Load(int3(dispatchThreadId.xy, 0)).rgb;
        logLuma = ApusrAutoExposureLogLuma(color, ApusrAutoExposure.preExposure);
        sampleCount = 1u;
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
        const uint tileIndex = groupId.y * ApusrAutoExposure.tileColumns + groupId.x;
        ApusrAutoExposureTiles.Store2(tileIndex * 8u, uint2(asuint(gLogLuma[0]), gSampleCount[0]));
    }
}
