// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../common/apusr_token_layout.hlsli"
#include "../common/apusr_fsr4_color.hlsli"
#include "../common/apusr_model_sidecar.hlsli"

Texture2D<float4> ApusrInputColor : register(t0);
Texture2D<float> ApusrInputExposure : register(t1);
RWTexture2D<APUSR_MODEL_SIDECAR_TEXEL> ApusrModelInputColor : register(u0);
RWTexture2D<float> ApusrFrameExposure : register(u1);
ConstantBuffer<ApusrTokenBandConstants> ApusrBand : register(b0);

float ApusrCaptureExposureValue()
{
    if ((ApusrBand.featureFlags & APUSR_FLAG_NON_LINEAR_COLORSPACE) != 0u) {
        return 1.0f;
    }
    if ((ApusrBand.featureFlags & APUSR_FLAG_AUTO_EXPOSURE) != 0u) {
        const float exposure = ApusrInputExposure.Load(int3(0, 0, 0));
        return max(exposure, 1.0e-6f) / max(ApusrBand.preExposure, 1.0e-6f);
    }
    if (ApusrBand.hasExposure != 0u) {
        const float exposure = ApusrInputExposure.Load(int3(0, 0, 0));
        return ((exposure == 0.0f) ? 1.0f : exposure) / max(ApusrBand.preExposure, 1.0e-6f);
    }
    return 1.0f / max(ApusrBand.preExposure, 1.0e-6f);
}

[numthreads(16, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    const float exposureValue = ApusrCaptureExposureValue();

    if (pixel.x == 0u && pixel.y == 0u) {
        ApusrFrameExposure[uint2(0u, 0u)] = exposureValue;
        ApusrFrameExposure[uint2(1u, 0u)] = exposureValue;
    }

    if (pixel.x >= ApusrBand.renderWidth || pixel.y >= ApusrBand.renderHeight) {
        return;
    }

    const float3 inputColor = ApusrInputColor.Load(int3(pixel, 0)).rgb;
    ApusrModelInputColor[pixel] = ApusrPackModelSidecarColor(ApusrApplyModelColor(inputColor * exposureValue, ApusrBand.featureFlags));
}
