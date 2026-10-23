// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../common/apusr_token_layout.hlsli"
#include "../common/apusr_fsr4_color.hlsli"
#include "../common/apusr_model_sidecar.hlsli"
#include "generated/apusr_fsr4_bookend_weights.hlsli"

uint ApusrClampS8(float value)
{
    const int signedValue = (int)clamp(round(value), -128.0f, 127.0f);
    return (uint)(signedValue & 0xff);
}

float ApusrUnpackF16(uint packed, uint lane)
{
    return f16tof32((lane == 0u) ? (packed & 0xffffu) : ((packed >> 16u) & 0xffffu));
}

Texture2D<APUSR_MODEL_SIDECAR_TEXEL> ApusrInputColor : register(t0);
Texture2D<float> ApusrInputDepth : register(t1);
Texture2D<float2> ApusrInputMotionVectors : register(t2);
Texture2D<float> ApusrInputExposure : register(t3);
Texture2D<float4> ApusrHistoryColor : register(t4);
Texture2D<float4> ApusrRecurrentState : register(t5);
SamplerState ApusrLinearClampSampler : register(s0);
Texture2D<float> ApusrReactiveMask : register(t6);
Texture2D<float> ApusrTransparencyMask : register(t7);
RWByteAddressBuffer ApusrIngressTokens : register(u0);
RWTexture2D<APUSR_MODEL_SIDECAR_TEXEL> ApusrReprojectedColor : register(u1);
ConstantBuffer<ApusrTokenBandConstants> ApusrBand : register(b0);

float3 ApusrRgbToYCbCr(float3 rgb)
{
    const float y = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
    return float3(y, (rgb.b - y) / 1.8556f, (rgb.r - y) / 1.5748f);
}

float3 ApusrApplyMuLaw(float3 rgb)
{
    return ApusrApplyModelColor(rgb, ApusrBand.featureFlags);
}

// The async bookend capture pass stores render-resolution input in model color
// space. Stage0 reads that frame-owned texture so delayed imports never depend
// on game-owned color resources from an older frame.
int2 ApusrClampRenderPixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(max(ApusrBand.renderWidth, 1u) - 1u, max(ApusrBand.renderHeight, 1u) - 1u));
}

float3 ApusrLoadRenderColor(int2 pixel)
{
    const int2 clampedPixel = ApusrClampRenderPixel(pixel);
    return ApusrUnpackModelSidecarColor(ApusrInputColor.Load(int3(clampedPixel, 0)));
}

int2 ApusrClampDisplayPixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(max(ApusrBand.displayWidth, 1u) - 1u, max(ApusrBand.displayHeight, 1u) - 1u));
}

float4 ApusrLoadHistoryColor(int2 pixel)
{
    return ApusrHistoryColor.Load(int3(ApusrClampDisplayPixel(pixel), 0));
}

float4 ApusrLoadRecurrentState(int2 pixel)
{
    return ApusrRecurrentState.Load(int3(ApusrClampDisplayPixel(pixel), 0));
}

float4 ApusrSampleHistoryLinearTexelCenter(float2 texelCenter)
{
    const float2 historySize = float2(max(ApusrBand.displayWidth, 1u), max(ApusrBand.displayHeight, 1u));
    return ApusrHistoryColor.SampleLevel(ApusrLinearClampSampler, texelCenter / historySize, 0.0f);
}

float4 ApusrSampleHistoryCatmullRom(float2 pixelPosition)
{
    // Matches the FSR4 pre-shader 9-tap Catmull-Rom filter shape. The nine
    // taps are texture-filtered samples, not manual bilinear Load quads, so the
    // history path stays bandwidth-conscious on APUs while keeping a fixed
    // static sampler in the export root signature.
    const float2 samplePosition = pixelPosition + 0.5f;
    const float2 texel1 = floor(samplePosition - 0.5f) + 0.5f;
    const float2 fraction = samplePosition - texel1;

    const float2 weight0 = fraction * (-0.5f + fraction * (1.0f - 0.5f * fraction));
    const float2 weight1 = 1.0f + fraction * fraction * (-2.5f + 1.5f * fraction);
    const float2 weight2 = fraction * (0.5f + fraction * (2.0f - 1.5f * fraction));
    const float2 weight3 = fraction * fraction * (-0.5f + 0.5f * fraction);
    const float2 weight12 = weight1 + weight2;
    const float2 offset12 = weight2 / max(weight12, 1.0e-6f);

    const float2 texel0 = texel1 - 1.0f;
    const float2 texel3 = texel1 + 2.0f;
    const float2 texel12 = texel1 + offset12;

    float4 result = 0.0f;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel0.x, texel0.y)) * weight0.x * weight0.y;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel12.x, texel0.y)) * weight12.x * weight0.y;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel3.x, texel0.y)) * weight3.x * weight0.y;

    result += ApusrSampleHistoryLinearTexelCenter(float2(texel0.x, texel12.y)) * weight0.x * weight12.y;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel12.x, texel12.y)) * weight12.x * weight12.y;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel3.x, texel12.y)) * weight3.x * weight12.y;

    result += ApusrSampleHistoryLinearTexelCenter(float2(texel0.x, texel3.y)) * weight0.x * weight3.y;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel12.x, texel3.y)) * weight12.x * weight3.y;
    result += ApusrSampleHistoryLinearTexelCenter(float2(texel3.x, texel3.y)) * weight3.x * weight3.y;
    return result;
}

float4 ApusrSampleRecurrentBilinear(float2 pixelPosition)
{
    const float2 recurrentSize = float2(max(ApusrBand.displayWidth, 1u), max(ApusrBand.displayHeight, 1u));
    return ApusrRecurrentState.SampleLevel(ApusrLinearClampSampler, (pixelPosition + 0.5f) / recurrentSize, 0.0f);
}

float ApusrLoadMask(Texture2D<float> maskTexture, uint2 displayPixel)
{
    const float2 renderPosition = float2(displayPixel) * float2(ApusrBand.displayToRenderScaleX, ApusrBand.displayToRenderScaleY);
    const uint2 sourcePixel = uint2(ApusrClampRenderPixel(int2(renderPosition)));
    return saturate(maskTexture.Load(int3(sourcePixel, 0)));
}

float ApusrTemporalInstability(uint2 displayPixel)
{
    float instability = 0.0f;
    if (ApusrBand.hasReactiveMask != 0u) {
        instability = max(instability, ApusrLoadMask(ApusrReactiveMask, displayPixel));
    }
    if (ApusrBand.hasTransparencyMask != 0u) {
        instability = max(instability, ApusrLoadMask(ApusrTransparencyMask, displayPixel));
    }
    return saturate(instability);
}

float ApusrGaussianFilterWeight(float distance, float upscaleFactor)
{
    const float x = upscaleFactor * distance;
    return exp((-0.5f / (0.47f * 0.47f)) * x * x);
}

float4 ApusrOptimizedInputUpscale(uint2 displayPixel)
{
    const float2 inverseScale = float2(ApusrBand.displayToRenderScaleX, ApusrBand.displayToRenderScaleY);
    const float2 upscaleScale = float2(ApusrBand.renderToDisplayScaleX, ApusrBand.renderToDisplayScaleY);
    const float2 inputPosition = (-0.5f + 0.5f * inverseScale) + float2(displayPixel) * inverseScale + float2(ApusrBand.jitterX, ApusrBand.jitterY);
    const int2 inputPixel = int2(floor(inputPosition));

    const float3 topLeft = ApusrLoadRenderColor(inputPixel + int2(0, 0));
    const float3 topRight = ApusrLoadRenderColor(inputPixel + int2(1, 0));
    const float3 bottomLeft = ApusrLoadRenderColor(inputPixel + int2(0, 1));
    const float3 bottomRight = ApusrLoadRenderColor(inputPixel + int2(1, 1));

    const float weightLeft = ApusrGaussianFilterWeight(abs(float(inputPixel.x) - inputPosition.x), upscaleScale.x);
    const float weightRight = ApusrGaussianFilterWeight(abs(float(inputPixel.x + 1) - inputPosition.x), upscaleScale.x);
    const float weightTop = ApusrGaussianFilterWeight(abs(float(inputPixel.y) - inputPosition.y), upscaleScale.y);
    const float weightBottom = ApusrGaussianFilterWeight(abs(float(inputPixel.y + 1) - inputPosition.y), upscaleScale.y);

    const float w00 = weightLeft * weightTop;
    const float w10 = weightRight * weightTop;
    const float w01 = weightLeft * weightBottom;
    const float w11 = weightRight * weightBottom;
    const float weightSum = max(w00 + w10 + w01 + w11, 1.0e-6f);
    const float centerWeight = max(max(w00, w10), max(w01, w11));

    const float3 color = (topLeft * w00 + topRight * w10 + bottomLeft * w01 + bottomRight * w11) / weightSum;
    return float4(color, centerWeight);
}

int2 ApusrDepthDilatedMotionPixel(uint2 displayPixel)
{
    const bool lowResolutionMotionVectors = (ApusrBand.featureFlags & APUSR_FLAG_DISPLAY_RESOLUTION_MOTION_VECTORS) == 0u;
    if (!lowResolutionMotionVectors) {
        return int2(displayPixel);
    }

    const float2 inverseScale = float2(ApusrBand.displayToRenderScaleX, ApusrBand.displayToRenderScaleY);
    const float2 renderCenter = round(float2(displayPixel) * inverseScale + (0.5f * inverseScale - 0.5f));
    int2 selectedPixel = ApusrClampRenderPixel(int2(renderCenter));
    float selectedDepth = ApusrInputDepth.Load(int3(selectedPixel, 0));

    static const int2 offsets[8] = {
        int2(-1, -1), int2(-1, 0), int2(-1, 1),
        int2(0, -1),                 int2(0, 1),
        int2(1, -1),  int2(1, 0),  int2(1, 1)
    };

    [unroll]
    for (uint i = 0u; i < 8u; ++i) {
        const int2 candidatePixel = ApusrClampRenderPixel(int2(renderCenter) + offsets[i]);
        const float candidateDepth = ApusrInputDepth.Load(int3(candidatePixel, 0));
        const bool candidateCloser = ((ApusrBand.featureFlags & APUSR_FLAG_DEPTH_INVERTED) != 0u) ?
            (candidateDepth > selectedDepth) :
            (candidateDepth < selectedDepth);
        if (candidateCloser) {
            selectedDepth = candidateDepth;
            selectedPixel = candidatePixel;
        }
    }
    return selectedPixel;
}

bool ApusrTryLoadVelocity(int2 pixel, out float2 velocity)
{
    const bool displayResolutionMotionVectors = (ApusrBand.featureFlags & APUSR_FLAG_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0u;
    int2 clampedPixel = pixel;
    if (displayResolutionMotionVectors) {
        clampedPixel = clamp(pixel, int2(0, 0), int2(max(ApusrBand.displayWidth, 1u) - 1u, max(ApusrBand.displayHeight, 1u) - 1u));
    } else {
        clampedPixel = ApusrClampRenderPixel(pixel);
    }

    velocity = ApusrInputMotionVectors.Load(int3(clampedPixel, 0)).xy * float2(ApusrBand.motionVectorScaleX, ApusrBand.motionVectorScaleY);
    if ((ApusrBand.featureFlags & APUSR_FLAG_MOTION_VECTOR_JITTER_CANCELLATION) != 0u) {
        velocity -= float2(ApusrBand.motionVectorJitterCancellationX, ApusrBand.motionVectorJitterCancellationY);
    }

    const bool validVelocity = !any(isnan(velocity)) && !any(isinf(velocity)) && all(abs(velocity) <= 1000000.0f);
    if (!validVelocity) {
        velocity = 0.0f;
    }
    return validVelocity;
}

void ApusrMakeStage0Features(uint2 displayPixel, out float features[8])
{
    const float4 upscaledInput = ApusrOptimizedInputUpscale(displayPixel);
    const float instability = ApusrTemporalInstability(displayPixel);
    const float temporalSampleWeight = upscaledInput.a * 0.1f * (1.0f - instability);

    float3 historyColor = (ApusrBand.resetHistory != 0u) ? upscaledInput.rgb : 0.0f;
    float4 recurrent = 0.0f;

    const int2 mvPixel = ApusrDepthDilatedMotionPixel(displayPixel);
    float2 velocity = 0.0f;
    const bool validVelocity = ApusrTryLoadVelocity(mvPixel, velocity);
    const float2 historyPosition = float2(displayPixel) + velocity * float2(ApusrBand.displayWidth, ApusrBand.displayHeight);
    const bool historyOnscreen = historyPosition.x >= 0.0f && historyPosition.y >= 0.0f &&
        historyPosition.x <= float(max(ApusrBand.displayWidth, 1u) - 1u) &&
        historyPosition.y <= float(max(ApusrBand.displayHeight, 1u) - 1u);

    if (ApusrBand.resetHistory == 0u && validVelocity && historyOnscreen) {
        historyColor = max(ApusrSampleHistoryCatmullRom(historyPosition).rgb, 0.0f);
        recurrent = ApusrSampleRecurrentBilinear(historyPosition);
        historyColor = lerp(historyColor, upscaledInput.rgb, temporalSampleWeight);
    }

    // Stage 13 blends the learned model filter against this current-frame
    // reprojected history color. Reset frames do not read this sidecar, so skip
    // the write when history is explicitly invalid.
    if (ApusrBand.resetHistory == 0u) {
        ApusrReprojectedColor[displayPixel] = ApusrPackModelSidecarColor(historyColor);
    }

    const float3 currentYcbcr = ApusrRgbToYCbCr(upscaledInput.rgb);
    const float3 historyYcbcr = ApusrRgbToYCbCr(historyColor);
    const float2 chromaDelta = currentYcbcr.yz - historyYcbcr.yz;
    const float colorDistance = sqrt(dot(chromaDelta, chromaDelta) + 1.0e-6f);

    features[0] = 2.0f * currentYcbcr.x - 1.0f;
    features[1] = 2.0f * historyYcbcr.x - 1.0f;
    features[2] = colorDistance;
    features[3] = 2.0f * recurrent.x - 1.0f;
    features[4] = 2.0f * recurrent.y - 1.0f;
    features[5] = 2.0f * recurrent.z - 1.0f;
    features[6] = 2.0f * recurrent.w - 1.0f;
    features[7] = 0.0f;
}

float ApusrDotStage0Weights(uint preset, uint outputChannel, uint quadPixel, float features[8])
{
    const uint weightBase = (quadPixel + outputChannel * 4u) * 4u;
    float result = 0.0f;
    [unroll]
    for (uint dwordIndex = 0u; dwordIndex < 4u; ++dwordIndex) {
        const uint packed = ApusrStage0Weight(preset, weightBase + dwordIndex);
        result += ApusrUnpackF16(packed, 0u) * features[dwordIndex * 2u + 0u];
        result += ApusrUnpackF16(packed, 1u) * features[dwordIndex * 2u + 1u];
    }
    return result;
}

float ApusrStage0BiasValue(uint preset, uint outputChannel)
{
    const uint packed = ApusrStage0Bias(preset, outputChannel >> 1u);
    return ApusrUnpackF16(packed, outputChannel & 1u);
}

[numthreads(16, 4, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint pixelX = dispatchThreadId.x;
    const uint localRow = dispatchThreadId.y;
    if (pixelX >= ApusrBand.tokenPaddedWidth || localRow >= ApusrBand.inputRowCount) {
        return;
    }

    const bool zeroGuarded = ApusrBand.tokenLayout == APUSR_TOKEN_LAYOUT_ZERO_GUARDED_RASTER_NHWC_C16;
    const bool outsideLogicalX = zeroGuarded ?
        (pixelX == 0u || pixelX > ApusrBand.tokenLogicalWidth) :
        (pixelX >= ApusrBand.tokenLogicalWidth);
    const bool outsideLogicalY = zeroGuarded ?
        (localRow == 0u || localRow > ApusrBand.tokenHeight) :
        false;
    const uint sourcePixelX = zeroGuarded ? ((pixelX > 0u) ? (pixelX - 1u) : 0u) : pixelX;
    const uint sourceLocalRow = zeroGuarded ? ((localRow > 0u) ? (localRow - 1u) : 0u) : localRow;
    const uint modelY = ApusrBand.inputFirstRow + sourceLocalRow;
    const uint baseOffset = ApusrTokenOffsetBytes(
        ApusrBand.tokenLayout,
        ApusrBand.tokenByteOffsetLow,
        pixelX,
        localRow,
        localRow,
        0u,
        ApusrBand.tokenRowPitchBytes,
        ApusrBand.tokenTileColumns,
        ApusrBand.tokenTileRows,
        ApusrBand.tokenLaneGroups);
    if (outsideLogicalX || outsideLogicalY) {
        ApusrIngressTokens.Store(baseOffset + 0u, 0u);
        ApusrIngressTokens.Store(baseOffset + 4u, 0u);
        ApusrIngressTokens.Store(baseOffset + 8u, 0u);
        ApusrIngressTokens.Store(baseOffset + 12u, 0u);
        return;
    }

    const uint2 displayBase = uint2(sourcePixelX * 2u, modelY * 2u);
    const uint preset = ApusrBand.modelPreset;
    const float inverseQuantScale = 1.0f / ApusrStage0Scale(preset);
    float accumulators[16];
    [unroll]
    for (uint channel = 0u; channel < 16u; ++channel) {
        accumulators[channel] = ApusrStage0BiasValue(preset, channel);
    }

    [unroll]
    for (uint quadY = 0u; quadY < 2u; ++quadY) {
        [unroll]
        for (uint quadX = 0u; quadX < 2u; ++quadX) {
            const uint2 displayPixel = min(displayBase + uint2(quadX, quadY), uint2(max(ApusrBand.displayWidth, 1u) - 1u, max(ApusrBand.displayHeight, 1u) - 1u));
            float features[8];
            ApusrMakeStage0Features(displayPixel, features);
            const uint quadPixel = quadX + quadY * 2u;
            [unroll]
            for (uint channel = 0u; channel < 16u; ++channel) {
                accumulators[channel] += ApusrDotStage0Weights(preset, channel, quadPixel, features);
            }
        }
    }

    ApusrIngressTokens.Store(baseOffset + 0u, ApusrPackFourInt8(
        ApusrClampS8(accumulators[0] * inverseQuantScale),
        ApusrClampS8(accumulators[1] * inverseQuantScale),
        ApusrClampS8(accumulators[2] * inverseQuantScale),
        ApusrClampS8(accumulators[3] * inverseQuantScale)));
    ApusrIngressTokens.Store(baseOffset + 4u, ApusrPackFourInt8(
        ApusrClampS8(accumulators[4] * inverseQuantScale),
        ApusrClampS8(accumulators[5] * inverseQuantScale),
        ApusrClampS8(accumulators[6] * inverseQuantScale),
        ApusrClampS8(accumulators[7] * inverseQuantScale)));
    ApusrIngressTokens.Store(baseOffset + 8u, ApusrPackFourInt8(
        ApusrClampS8(accumulators[8] * inverseQuantScale),
        ApusrClampS8(accumulators[9] * inverseQuantScale),
        ApusrClampS8(accumulators[10] * inverseQuantScale),
        ApusrClampS8(accumulators[11] * inverseQuantScale)));
    ApusrIngressTokens.Store(baseOffset + 12u, ApusrPackFourInt8(
        ApusrClampS8(accumulators[12] * inverseQuantScale),
        ApusrClampS8(accumulators[13] * inverseQuantScale),
        ApusrClampS8(accumulators[14] * inverseQuantScale),
        ApusrClampS8(accumulators[15] * inverseQuantScale)));
}
