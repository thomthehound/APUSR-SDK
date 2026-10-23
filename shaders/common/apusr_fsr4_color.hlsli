// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#ifndef APUSR_FSR4_COLOR_HLSLI
#define APUSR_FSR4_COLOR_HLSLI

float ApusrApplySrgbScalar(float value)
{
    return (value < 0.0031308f) ? (12.92f * value) : (1.055f * pow(value, 1.0f / 2.4f) - 0.055f);
}

float3 ApusrApplySrgb(float3 value)
{
    return float3(
        ApusrApplySrgbScalar(value.r),
        ApusrApplySrgbScalar(value.g),
        ApusrApplySrgbScalar(value.b));
}

float ApusrRemoveSrgbScalar(float value)
{
    return (value < 0.04045f) ? (value / 12.92f) : pow((value + 0.055f) / 1.055f, 2.4f);
}

float3 ApusrRemoveSrgb(float3 value)
{
    return float3(
        ApusrRemoveSrgbScalar(value.r),
        ApusrRemoveSrgbScalar(value.g),
        ApusrRemoveSrgbScalar(value.b));
}

float3 ApusrApplyPq(float3 value)
{
    const float paperWhite = 300.0f;
    const float nitMax = 10000.0f;
    const float m1 = 2610.0f / 4096.0f / 4.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;

    const float3 lp = pow(max(value * (paperWhite / nitMax), 1.0e-10f), m1);
    return max(pow((c1 + c2 * lp) / (1.0f + c3 * lp), m2), 0.0f);
}

float3 ApusrRemovePq(float3 value)
{
    const float paperWhite = 300.0f;
    const float nitMax = 10000.0f;
    const float m1 = 2610.0f / 4096.0f / 4.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;

    const float3 np = pow(max(value, 0.0f), 1.0f / m2);
    const float3 linearColor = pow(max(np - c1, 0.0f) / (c2 - c3 * np), 1.0f / m1) * (nitMax / paperWhite);
    return max(linearColor, 0.0f);
}

float3 ApusrApplyModelColor(float3 rgb, uint featureFlags)
{
    if ((featureFlags & APUSR_FLAG_NON_LINEAR_COLORSPACE) != 0u) {
        return max(rgb, 0.0f);
    }
    if ((featureFlags & APUSR_FLAG_COLORSPACE_SRGB) != 0u) {
        rgb = ApusrRemoveSrgb(rgb);
    } else if ((featureFlags & APUSR_FLAG_COLORSPACE_PQ) != 0u) {
        rgb = ApusrRemovePq(rgb);
    }
    return max(0.1174f * log(1.0f + 150.0f * max(rgb, 0.0f)), 0.0f);
}

float3 ApusrRemoveModelColor(float3 rgb, uint featureFlags)
{
    if ((featureFlags & APUSR_FLAG_NON_LINEAR_COLORSPACE) != 0u) {
        return max(rgb, 0.0f);
    }

    rgb = max((exp(max(rgb, 0.0f) / 0.1174f) - 1.0f) / 150.0f, 0.0f);
    if ((featureFlags & APUSR_FLAG_COLORSPACE_SRGB) != 0u) {
        rgb = ApusrApplySrgb(rgb);
    } else if ((featureFlags & APUSR_FLAG_COLORSPACE_PQ) != 0u) {
        rgb = ApusrApplyPq(rgb);
    }
    return rgb;
}

#endif
