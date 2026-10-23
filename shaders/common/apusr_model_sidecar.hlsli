// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#ifndef APUSR_MODEL_SIDECAR_HLSLI
#define APUSR_MODEL_SIDECAR_HLSLI

#ifndef APUSR_DEBUG_HALF4_SIDECAR
#define APUSR_DEBUG_HALF4_SIDECAR 0
#endif

#if APUSR_DEBUG_HALF4_SIDECAR

// Debug-only parity sidecars use half4 render targets. This mode is intentionally
// bandwidth-heavy and exists to compare the async bookends against a less lossy
// reference while the packed sidecar remains the product path.
#define APUSR_MODEL_SIDECAR_TEXEL float4

APUSR_MODEL_SIDECAR_TEXEL ApusrPackModelSidecarColor(float3 color)
{
    return float4(max(color, 0.0f), 1.0f);
}

float3 ApusrUnpackModelSidecarColor(APUSR_MODEL_SIDECAR_TEXEL texel)
{
    return max(texel.rgb, 0.0f);
}

#else

// Async bookend sidecars are frame-owned guidance textures, not final history.
// Store positive model-color values in one R32_UINT texel with a shared exponent
// so delayed import gets the frame-N data it needs without half4 bandwidth.
#define APUSR_MODEL_SIDECAR_TEXEL uint
#define APUSR_SIDECAR_MANTISSA_BITS 9u
#define APUSR_SIDECAR_MANTISSA_MASK 0x1ffu
#define APUSR_SIDECAR_EXPONENT_SHIFT 27u
#define APUSR_SIDECAR_EXPONENT_BIAS 15
#define APUSR_SIDECAR_MANTISSA_MAX 511.0f

float ApusrSidecarPowerOfTwo(int exponent)
{
    return asfloat((uint)(exponent + 127) << 23u);
}

int ApusrCeilLog2PositiveNormal(float value)
{
    const uint bits = asuint(value);
    const int floorExponent = (int)((bits >> 23u) & 0xffu) - 127;
    const bool exactPowerOfTwo = (bits & 0x7fffffu) == 0u;
    return floorExponent + (exactPowerOfTwo ? 0 : 1);
}

APUSR_MODEL_SIDECAR_TEXEL ApusrPackModelSidecarColor(float3 color)
{
    const float3 positiveColor = max(color, 0.0f);
    const float maxChannel = max(max(positiveColor.r, positiveColor.g), positiveColor.b);
    if (maxChannel <= 1.0e-20f) {
        return 0u;
    }

    const int exponent = clamp(ApusrCeilLog2PositiveNormal(maxChannel), -APUSR_SIDECAR_EXPONENT_BIAS, 16);
    const float encodeScale = APUSR_SIDECAR_MANTISSA_MAX / ApusrSidecarPowerOfTwo(exponent);
    const uint r = (uint)clamp(floor(positiveColor.r * encodeScale + 0.5f), 0.0f, APUSR_SIDECAR_MANTISSA_MAX);
    const uint g = (uint)clamp(floor(positiveColor.g * encodeScale + 0.5f), 0.0f, APUSR_SIDECAR_MANTISSA_MAX);
    const uint b = (uint)clamp(floor(positiveColor.b * encodeScale + 0.5f), 0.0f, APUSR_SIDECAR_MANTISSA_MAX);
    return (r & APUSR_SIDECAR_MANTISSA_MASK) |
        ((g & APUSR_SIDECAR_MANTISSA_MASK) << APUSR_SIDECAR_MANTISSA_BITS) |
        ((b & APUSR_SIDECAR_MANTISSA_MASK) << (APUSR_SIDECAR_MANTISSA_BITS * 2u)) |
        ((uint)(exponent + APUSR_SIDECAR_EXPONENT_BIAS) << APUSR_SIDECAR_EXPONENT_SHIFT);
}

float3 ApusrUnpackModelSidecarColor(APUSR_MODEL_SIDECAR_TEXEL packed)
{
    if (packed == 0u) {
        return 0.0f;
    }

    const uint r = packed & APUSR_SIDECAR_MANTISSA_MASK;
    const uint g = (packed >> APUSR_SIDECAR_MANTISSA_BITS) & APUSR_SIDECAR_MANTISSA_MASK;
    const uint b = (packed >> (APUSR_SIDECAR_MANTISSA_BITS * 2u)) & APUSR_SIDECAR_MANTISSA_MASK;
    const int exponent = (int)(packed >> APUSR_SIDECAR_EXPONENT_SHIFT) - APUSR_SIDECAR_EXPONENT_BIAS;
    const float decodeScale = ApusrSidecarPowerOfTwo(exponent) / APUSR_SIDECAR_MANTISSA_MAX;
    return float3((float)r, (float)g, (float)b) * decodeScale;
}

#endif

#endif // APUSR_MODEL_SIDECAR_HLSLI
