// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../common/apusr_token_layout.hlsli"
#include "../common/apusr_fsr4_color.hlsli"

Texture2D<float4> ApusrRcasInput : register(t0);
Texture2D<float> ApusrInputExposure : register(t1);
RWTexture2D<float4> ApusrOutput : register(u0);
ConstantBuffer<ApusrTokenBandConstants> ApusrBand : register(b0);

float3 ApusrRemoveMuLaw(float3 rgb)
{
    return ApusrRemoveModelColor(rgb, ApusrBand.featureFlags);
}

float ApusrLoadExposureValue()
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

int2 ApusrClampDisplayPixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(max(ApusrBand.displayWidth, 1u) - 1u, max(ApusrBand.displayHeight, 1u) - 1u));
}

float3 ApusrLoadRcasRgbRaw(int2 pixel)
{
    return ApusrRcasInput.Load(int3(pixel, 0)).rgb;
}

float3 ApusrLoadRcasRgbClamped(int2 pixel)
{
    return ApusrRcasInput.Load(int3(ApusrClampDisplayPixel(pixel), 0)).rgb;
}

float ApusrSafeRcp(float value)
{
    return rcp(max(abs(value), 1.0e-6f)) * ((value < 0.0f) ? -1.0f : 1.0f);
}

float3 ApusrRcasFilter(uint2 pixel)
{
    const int2 centerPixel = int2(pixel);

    float3 b;
    float3 d;
    float3 e;
    float3 f;
    float3 h;

    // Almost every pixel is interior.  Keep the common path free of per-sample
    // coordinate clamps and only pay for edge replication on the frame border.
    [branch]
    if (pixel.x > 0u && pixel.y > 0u && pixel.x + 1u < ApusrBand.displayWidth && pixel.y + 1u < ApusrBand.displayHeight) {
        b = ApusrLoadRcasRgbRaw(centerPixel + int2(0, -1));
        d = ApusrLoadRcasRgbRaw(centerPixel + int2(-1, 0));
        e = ApusrLoadRcasRgbRaw(centerPixel);
        f = ApusrLoadRcasRgbRaw(centerPixel + int2(1, 0));
        h = ApusrLoadRcasRgbRaw(centerPixel + int2(0, 1));
    } else {
        b = ApusrLoadRcasRgbClamped(centerPixel + int2(0, -1));
        d = ApusrLoadRcasRgbClamped(centerPixel + int2(-1, 0));
        e = ApusrLoadRcasRgbClamped(centerPixel);
        f = ApusrLoadRcasRgbClamped(centerPixel + int2(1, 0));
        h = ApusrLoadRcasRgbClamped(centerPixel + int2(0, 1));
    }

    const float bL = b.b * 0.5f + (b.r * 0.5f + b.g);
    const float dL = d.b * 0.5f + (d.r * 0.5f + d.g);
    const float eL = e.b * 0.5f + (e.r * 0.5f + e.g);
    const float fL = f.b * 0.5f + (f.r * 0.5f + f.g);
    const float hL = h.b * 0.5f + (h.r * 0.5f + h.g);

    const float maxLuma = max(max(max(bL, dL), max(eL, fL)), hL);
    const float minLuma = min(min(min(bL, dL), min(eL, fL)), hL);
    float noise = 0.25f * (bL + dL + fL + hL) - eL;
    noise = saturate(abs(noise) / max(maxLuma - minLuma, 1.0e-6f));
    noise = -0.5f * noise + 1.0f;

    const float3 minRing = min(min(b, d), min(f, h));
    const float3 maxRing = max(max(b, d), max(f, h));
    const float lowerLimiterMultiplier = saturate(eL / max(min(min(bL, dL), min(fL, hL)), 1.0e-6f));
    const float3 hitMin = minRing * (1.0f / max(4.0f * maxRing, 1.0e-6f)) * lowerLimiterMultiplier;
    const float3 hitMax = (1.0f - maxRing) / min(4.0f * minRing - 4.0f, -1.0e-6f);
    const float3 lobeRgb = max(-hitMin, hitMax);

    // ApusrBand.sharpness is precomputed on the CPU as the RCAS filter strength
    // exp2(-((-2 * userSharpness) + 2)).  Do not pay an exp2 per output pixel.
    const float sharpness = ApusrBand.sharpness;
    const float rcasLimit = 0.25f - (1.0f / 16.0f);
    float lobe = max(-rcasLimit, min(max(max(lobeRgb.r, lobeRgb.g), lobeRgb.b), 0.0f)) * sharpness;
    lobe *= noise;

    const float reciprocalLobe = rcp(4.0f * lobe + 1.0f);
    return (lobe * (b + d + f + h) + e) * reciprocalLobe;
}

[numthreads(16, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= ApusrBand.displayWidth || pixel.y >= ApusrBand.displayHeight) {
        return;
    }

    const float exposureValue = ApusrLoadExposureValue();
    float3 color = ApusrRcasFilter(pixel);
    color = ApusrRemoveMuLaw(color) / max(exposureValue, 1.0e-6f);
    color = clamp(color, 0.0f, 64000.0f);
    ApusrOutput[pixel] = float4(color, 1.0f);
}
