// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../common/apusr_token_layout.hlsli"
#include "../common/apusr_fsr4_color.hlsli"
#include "../common/apusr_model_sidecar.hlsli"
#include "generated/apusr_fsr4_bookend_weights.hlsli"

ByteAddressBuffer ApusrEgressTokens : register(t0);
Texture2D<APUSR_MODEL_SIDECAR_TEXEL> ApusrInputColor : register(t1);
Texture2D<float> ApusrInputExposure : register(t2);
Texture2D<APUSR_MODEL_SIDECAR_TEXEL> ApusrReprojectedColor : register(t3);
RWTexture2D<float4> ApusrOutput : register(u0);
RWTexture2D<float4> ApusrHistoryWrite : register(u1);
RWTexture2D<float4> ApusrRecurrentWrite : register(u2);
ConstantBuffer<ApusrTokenBandConstants> ApusrBand : register(b0);

uint ApusrClampS8(float value)
{
    const int signedValue = (int)clamp(round(value), -128.0f, 127.0f);
    return (uint)(signedValue & 0xff);
}

uint ApusrClampReluS8(float value)
{
    const int signedValue = (int)clamp(round(value), 0.0f, 127.0f);
    return (uint)(signedValue & 0xff);
}

float ApusrUnpackF16(uint packed, uint lane)
{
    return f16tof32((lane == 0u) ? (packed & 0xffffu) : ((packed >> 16u) & 0xffffu));
}

float4 ApusrLoadHalf4FromPackedPairs(uint pair0, uint pair1)
{
    return float4(
        ApusrUnpackF16(pair0, 0u),
        ApusrUnpackF16(pair0, 1u),
        ApusrUnpackF16(pair1, 0u),
        ApusrUnpackF16(pair1, 1u));
}

uint ApusrPackS8Float4(float4 value)
{
    return ApusrPackFourInt8(
        ApusrClampS8(value.x),
        ApusrClampS8(value.y),
        ApusrClampS8(value.z),
        ApusrClampS8(value.w));
}

uint ApusrPackReluS8Float4(float4 value)
{
    return ApusrPackFourInt8(
        ApusrClampReluS8(value.x),
        ApusrClampReluS8(value.y),
        ApusrClampReluS8(value.z),
        ApusrClampReluS8(value.w));
}

float4 ApusrUnpackS8Float4(uint packed)
{
    return float4(
        (float)ApusrUnpackSignedInt8Raw(packed, 0u),
        (float)ApusrUnpackSignedInt8Raw(packed, 1u),
        (float)ApusrUnpackSignedInt8Raw(packed, 2u),
        (float)ApusrUnpackSignedInt8Raw(packed, 3u));
}

float3 ApusrApplyMuLaw(float3 rgb)
{
    return ApusrApplyModelColor(rgb, ApusrBand.featureFlags);
}

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

int2 ApusrClampRenderPixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(max(ApusrBand.renderWidth, 1u) - 1u, max(ApusrBand.renderHeight, 1u) - 1u));
}

float3 ApusrLoadRenderColor(int2 pixel)
{
    const int2 clampedPixel = ApusrClampRenderPixel(pixel);
    return ApusrUnpackModelSidecarColor(ApusrInputColor.Load(int3(clampedPixel, 0)));
}

float ApusrGaussianFilterWeight(float distance, float upscaleFactor)
{
    const float x = upscaleFactor * distance;
    return exp((-0.5f / (0.47f * 0.47f)) * x * x);
}

int2 ApusrModelFilterCenter(uint2 displayPixel)
{
    const float2 inverseScale = float2(ApusrBand.displayToRenderScaleX, ApusrBand.displayToRenderScaleY);
    const float2 inputPosition = (-0.5f + 0.5f * inverseScale) + float2(displayPixel) * inverseScale + float2(ApusrBand.jitterX, ApusrBand.jitterY);
    return int2(round(inputPosition));
}

void ApusrLoadModelFilterInputCache(uint2 displayBase, float exposureValue, out int2 cacheBase, out float3 inputColorCache[4][4])
{
    const int2 center00 = ApusrModelFilterCenter(displayBase);
    const int2 center10 = ApusrModelFilterCenter(displayBase + uint2(1u, 0u));
    const int2 center01 = ApusrModelFilterCenter(displayBase + uint2(0u, 1u));
    const int2 center11 = ApusrModelFilterCenter(displayBase + uint2(1u, 1u));
    cacheBase = min(min(center00, center10), min(center01, center11)) - int2(1, 1);

    [unroll]
    for (uint y = 0u; y < 4u; ++y) {
        [unroll]
        for (uint x = 0u; x < 4u; ++x) {
            inputColorCache[y][x] = ApusrLoadRenderColor(cacheBase + int2(x, y));
        }
    }
}

float3 ApusrApplyModelFilter(uint2 displayPixel, float4 filter, int2 cacheBase, float3 inputColorCache[4][4])
{
    const float correlationFactor = tanh(filter.x);
    const float scaleFactorX = 1.0f + tanh(0.5f * filter.y);
    const float scaleFactorY = 1.0f + tanh(0.5f * filter.z);
    const float scaleFactorX2 = scaleFactorX * scaleFactorX;
    const float scaleFactorY2 = scaleFactorY * scaleFactorY;
    const float scaleFactorXY = scaleFactorX * scaleFactorY;

    const float2 inverseScale = float2(ApusrBand.displayToRenderScaleX, ApusrBand.displayToRenderScaleY);
    const float2 upscaleScale = float2(ApusrBand.renderToDisplayScaleX, ApusrBand.renderToDisplayScaleY);
    const float2 inputPosition = (-0.5f + 0.5f * inverseScale) + float2(displayPixel) * inverseScale + float2(ApusrBand.jitterX, ApusrBand.jitterY);
    const int2 inputCenter = int2(round(inputPosition));

    float3 upscaledColor = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (uint dy = 0u; dy < 3u; ++dy) {
        const int yInput = inputCenter.y + int(dy) - 1;
        const float yDistance = (float(yInput) - inputPosition.y) * upscaleScale.y;
        const float yDistance2 = yDistance * yDistance;
        const uint cacheY = (uint)(yInput - cacheBase.y);
        [unroll]
        for (uint dx = 0u; dx < 3u; ++dx) {
            const int xInput = inputCenter.x + int(dx) - 1;
            const float xDistance = (float(xInput) - inputPosition.x) * upscaleScale.x;
            const float xDistance2 = xDistance * xDistance;
            const uint cacheX = (uint)(xInput - cacheBase.x);
            const float gaussianWeight = exp((-0.5f / (0.47f * 0.47f)) *
                (xDistance2 * scaleFactorX2 + 2.0f * correlationFactor * (xDistance * yDistance) * scaleFactorXY + yDistance2 * scaleFactorY2));
            totalWeight += gaussianWeight;
            upscaledColor += inputColorCache[cacheY][cacheX] * gaussianWeight;
        }
    }
    upscaledColor /= max(totalWeight, 1.0e-6f);

    if (ApusrBand.resetHistory != 0u) {
        return upscaledColor;
    }

    const float blend = 0.5f * (1.0f + tanh(0.5f * filter.w));
    const float3 reprojectedColor = ApusrUnpackModelSidecarColor(ApusrReprojectedColor.Load(int3(min(displayPixel, uint2(max(ApusrBand.displayWidth, 1u) - 1u, max(ApusrBand.displayHeight, 1u) - 1u)), 0)));
    return lerp(upscaledColor, reprojectedColor, saturate(blend));
}

uint4 ApusrLoadStage13Packed(int2 modelPixel)
{
    if (modelPixel.x < 0 || modelPixel.y < 0 ||
        modelPixel.x >= int(ApusrBand.tokenLogicalWidth) || modelPixel.y >= int(ApusrBand.tokenHeight) ||
        modelPixel.y < int(ApusrBand.egressFirstRow) || modelPixel.y >= int(ApusrBand.egressFirstRow + ApusrBand.egressRowCount)) {
        return uint4(0u, 0u, 0u, 0u);
    }
    const uint absoluteRow = uint(modelPixel.y);
    const uint localEgressRow = absoluteRow - ApusrBand.egressFirstRow;
    if (ApusrBand.tokenLayout == APUSR_TOKEN_LAYOUT_CELL_M4X8_C16) {
        const uint pixelX = uint(modelPixel.x);
        return uint4(
            ApusrEgressTokens.Load(ApusrCellM4x8C16TokenOffsetBytes(
                ApusrBand.tokenByteOffsetLow, pixelX, absoluteRow, 0u, ApusrBand.tokenTileColumns)),
            ApusrEgressTokens.Load(ApusrCellM4x8C16TokenOffsetBytes(
                ApusrBand.tokenByteOffsetLow, pixelX, absoluteRow, 4u, ApusrBand.tokenTileColumns)),
            ApusrEgressTokens.Load(ApusrCellM4x8C16TokenOffsetBytes(
                ApusrBand.tokenByteOffsetLow, pixelX, absoluteRow, 8u, ApusrBand.tokenTileColumns)),
            ApusrEgressTokens.Load(ApusrCellM4x8C16TokenOffsetBytes(
                ApusrBand.tokenByteOffsetLow, pixelX, absoluteRow, 12u, ApusrBand.tokenTileColumns)));
    }

    const uint baseOffset = ApusrTokenOffsetBytes(
        ApusrBand.tokenLayout,
        ApusrBand.tokenByteOffsetLow,
        uint(modelPixel.x),
        absoluteRow,
        localEgressRow,
        0u,
        ApusrBand.tokenRowPitchBytes,
        ApusrBand.tokenTileColumns,
        ApusrBand.tokenTileRows,
        ApusrBand.tokenLaneGroups);
    return uint4(
        ApusrEgressTokens.Load(baseOffset + 0u),
        ApusrEgressTokens.Load(baseOffset + 4u),
        ApusrEgressTokens.Load(baseOffset + 8u),
        ApusrEgressTokens.Load(baseOffset + 12u));
}

uint4 ApusrStage13ConvNext(uint2 modelPixel)
{
    const uint preset = ApusrBand.modelPreset;
    int accumulator0[16];
    int reluAccumulator[32];
    int accumulator2[16];

    [unroll]
    for (uint i = 0u; i < 16u; ++i) {
        accumulator0[i] = 0;
        accumulator2[i] = 0;
    }
    [unroll]
    for (uint i = 0u; i < 32u; ++i) {
        reluAccumulator[i] = 0;
    }

    [unroll]
    for (uint ky = 0u; ky < 3u; ++ky) {
        [unroll]
        for (uint kx = 0u; kx < 3u; ++kx) {
            const uint4 inputPacked = ApusrLoadStage13Packed(int2(modelPixel) + int2(kx, ky) - int2(1, 1));
            [unroll]
            for (uint oc = 0u; oc < 16u; ++oc) {
                const uint weightBase = (((ky * 3u + kx) * 16u + oc) * 4u);
                accumulator0[oc] = dot4add_i8packed(inputPacked.x, ApusrStage13Weight0(preset, weightBase + 0u), accumulator0[oc]);
                accumulator0[oc] = dot4add_i8packed(inputPacked.y, ApusrStage13Weight0(preset, weightBase + 1u), accumulator0[oc]);
                accumulator0[oc] = dot4add_i8packed(inputPacked.z, ApusrStage13Weight0(preset, weightBase + 2u), accumulator0[oc]);
                accumulator0[oc] = dot4add_i8packed(inputPacked.w, ApusrStage13Weight0(preset, weightBase + 3u), accumulator0[oc]);
            }
        }
    }

    uint4 convResult;
    const float conv0Scale = ApusrStage13InputScale(preset) * ApusrStage13Weight0Scale(preset);
    [unroll]
    for (uint f = 0u; f < 16u; f += 4u) {
        const float4 bias = ApusrLoadHalf4FromPackedPairs(ApusrStage13Bias0(preset, f >> 1u), ApusrStage13Bias0(preset, (f >> 1u) + 1u));
        const float4 value = (float4(accumulator0[f + 0u], accumulator0[f + 1u], accumulator0[f + 2u], accumulator0[f + 3u]) * conv0Scale + bias) * ApusrStage13RcprOutputScale0(preset);
        convResult[f >> 2u] = ApusrPackS8Float4(value);
    }

    [unroll]
    for (uint oc1 = 0u; oc1 < 32u; ++oc1) {
        const uint weightBase = oc1 * 4u;
        reluAccumulator[oc1] = dot4add_i8packed(convResult.x, ApusrStage13Weight1(preset, weightBase + 0u), reluAccumulator[oc1]);
        reluAccumulator[oc1] = dot4add_i8packed(convResult.y, ApusrStage13Weight1(preset, weightBase + 1u), reluAccumulator[oc1]);
        reluAccumulator[oc1] = dot4add_i8packed(convResult.z, ApusrStage13Weight1(preset, weightBase + 2u), reluAccumulator[oc1]);
        reluAccumulator[oc1] = dot4add_i8packed(convResult.w, ApusrStage13Weight1(preset, weightBase + 3u), reluAccumulator[oc1]);
    }

    uint reluResult[8];
    const float conv1Scale = ApusrStage13InputScale1(preset) * ApusrStage13Weight1Scale(preset);
    [unroll]
    for (uint f1 = 0u; f1 < 32u; f1 += 4u) {
        const float4 bias = ApusrLoadHalf4FromPackedPairs(ApusrStage13Bias1(preset, f1 >> 1u), ApusrStage13Bias1(preset, (f1 >> 1u) + 1u));
        const float4 value = (float4(reluAccumulator[f1 + 0u], reluAccumulator[f1 + 1u], reluAccumulator[f1 + 2u], reluAccumulator[f1 + 3u]) * conv1Scale + bias) * ApusrStage13RcprOutputScale1(preset);
        reluResult[f1 >> 2u] = ApusrPackReluS8Float4(value);
    }

    [unroll]
    for (uint oc2 = 0u; oc2 < 16u; ++oc2) {
        const uint weightBase = oc2 * 8u;
        accumulator2[oc2] = dot4add_i8packed(reluResult[0], ApusrStage13Weight2(preset, weightBase + 0u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[1], ApusrStage13Weight2(preset, weightBase + 1u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[2], ApusrStage13Weight2(preset, weightBase + 2u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[3], ApusrStage13Weight2(preset, weightBase + 3u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[4], ApusrStage13Weight2(preset, weightBase + 4u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[5], ApusrStage13Weight2(preset, weightBase + 5u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[6], ApusrStage13Weight2(preset, weightBase + 6u), accumulator2[oc2]);
        accumulator2[oc2] = dot4add_i8packed(reluResult[7], ApusrStage13Weight2(preset, weightBase + 7u), accumulator2[oc2]);
    }

    const uint4 centerInput = ApusrLoadStage13Packed(int2(modelPixel));
    uint4 packedVss;
    const float conv2Scale = ApusrStage13InputScale2(preset) * ApusrStage13Weight2Scale(preset);
    const float outputInverseScale = 1.0f / ApusrStage13CnbOutputScale(preset);
    [unroll]
    for (uint f2 = 0u; f2 < 16u; f2 += 4u) {
        const float4 bias = ApusrLoadHalf4FromPackedPairs(ApusrStage13Bias2(preset, f2 >> 1u), ApusrStage13Bias2(preset, (f2 >> 1u) + 1u));
        const float4 skip = ApusrUnpackS8Float4(centerInput[f2 >> 2u]) * ApusrStage13InputScale(preset);
        const float4 value = (float4(accumulator2[f2 + 0u], accumulator2[f2 + 1u], accumulator2[f2 + 2u], accumulator2[f2 + 3u]) * conv2Scale + bias + skip) * outputInverseScale;
        packedVss[f2 >> 2u] = ApusrPackS8Float4(value);
    }
    return packedVss;
}

float4 ApusrStage13Ct2d4(uint2 pixelPhase, uint outputChannelBase, uint4 packedVss)
{
    const uint preset = ApusrBand.modelPreset;
    int4 accumulator = int4(0, 0, 0, 0);
    [unroll]
    for (uint lane = 0u; lane < 4u; ++lane) {
        const uint outputChannel = outputChannelBase + lane;
        const uint weightBase = (((pixelPhase.y * 2u + pixelPhase.x) * 8u + outputChannel) * 4u);
        accumulator[lane] = dot4add_i8packed(packedVss.x, ApusrStage13Ct2dWeight(preset, weightBase + 0u), accumulator[lane]);
        accumulator[lane] = dot4add_i8packed(packedVss.y, ApusrStage13Ct2dWeight(preset, weightBase + 1u), accumulator[lane]);
        accumulator[lane] = dot4add_i8packed(packedVss.z, ApusrStage13Ct2dWeight(preset, weightBase + 2u), accumulator[lane]);
        accumulator[lane] = dot4add_i8packed(packedVss.w, ApusrStage13Ct2dWeight(preset, weightBase + 3u), accumulator[lane]);
    }
    const uint biasBase = outputChannelBase >> 1u;
    const float4 bias = ApusrLoadHalf4FromPackedPairs(ApusrStage13Ct2dBias(preset, biasBase), ApusrStage13Ct2dBias(preset, biasBase + 1u));
    return float4(accumulator) * (ApusrStage13CnbOutputScale(preset) * ApusrStage13Ct2dWeightScale(preset)) + bias;
}

void ApusrStoreStage13Output(uint2 modelPixel, uint2 pixelPhase, uint2 displayPixel, uint4 packedVss, int2 cacheBase, float3 inputColorCache[4][4], float exposureValue)
{
    if (displayPixel.x >= ApusrBand.displayWidth || displayPixel.y >= ApusrBand.displayHeight) {
        return;
    }

    const float4 modelParameters = ApusrStage13Ct2d4(pixelPhase, 0u, packedVss);
    float4 recurrent = ApusrStage13Ct2d4(pixelPhase, 4u, packedVss);
    recurrent = 0.5f * (1.0f + tanh(0.5f * recurrent));
    ApusrRecurrentWrite[displayPixel] = recurrent;

    float3 modelColor = ApusrApplyModelFilter(displayPixel, modelParameters, cacheBase, inputColorCache);
    ApusrHistoryWrite[displayPixel] = float4(modelColor, 1.0f);

    // The post pass writes display-space color directly when sharpening is disabled.
    // With sharpening enabled, this UAV is the RCAS input texture and remains in
    // model color space; the RCAS pass owns sharpening and final color conversion.
    const bool shouldConvertColorSpace = ApusrBand.sharpeningRequested == 0u;
    float3 finalColor = shouldConvertColorSpace ? (ApusrRemoveMuLaw(modelColor) / max(exposureValue, 1.0e-6f)) : modelColor;
    finalColor = clamp(finalColor, 0.0f, 64000.0f);
    ApusrOutput[displayPixel] = float4(finalColor, 1.0f);
}

[numthreads(16, 4, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint pixelX = dispatchThreadId.x;
    const uint localRow = dispatchThreadId.y;
    if (pixelX >= ApusrBand.tokenLogicalWidth || localRow >= ApusrBand.outputRowCount) {
        return;
    }

    const bool diagnosticTileOverlay = ApusrBand.bandIndex == 0xffffffffu;
    const bool diagnosticFrameOverlay = ApusrBand.bandIndex == 0xfffffffeu;
    if (diagnosticTileOverlay || diagnosticFrameOverlay) {
        const uint absoluteRow = ApusrBand.egressFirstRow + localRow;
        const uint baseOffset = ApusrTokenOffsetBytes(
            ApusrBand.tokenLayout,
            ApusrBand.tokenByteOffsetLow,
            pixelX,
            absoluteRow,
            localRow,
            0u,
            ApusrBand.tokenRowPitchBytes,
            ApusrBand.tokenTileColumns,
            ApusrBand.tokenTileRows,
            ApusrBand.tokenLaneGroups);
        const uint channels0To3 = ApusrEgressTokens.Load(baseOffset + 0u);
        const float3 displayRgb = float3(
            ApusrUnpackDiagnostic3x3Byte(channels0To3, 0u),
            ApusrUnpackDiagnostic3x3Byte(channels0To3, 1u),
            ApusrUnpackDiagnostic3x3Byte(channels0To3, 2u));
        const float4 outputValue = float4(displayRgb, 1.0f);
        if (diagnosticFrameOverlay) {
            const uint tokenWidth = max(ApusrBand.tokenLogicalWidth, 1u);
            const uint tokenHeight = max(ApusrBand.outputRowCount, 1u);
            const uint x0 = (pixelX * ApusrBand.displayWidth) / tokenWidth;
            const uint x1 = ((pixelX + 1u) * ApusrBand.displayWidth + tokenWidth - 1u) / tokenWidth;
            const uint y0 = (localRow * ApusrBand.displayHeight) / tokenHeight;
            const uint y1 = ((localRow + 1u) * ApusrBand.displayHeight + tokenHeight - 1u) / tokenHeight;
            for (uint y = y0; y < min(y1, ApusrBand.displayHeight); ++y) {
                for (uint x = x0; x < min(x1, ApusrBand.displayWidth); ++x) {
                    ApusrOutput[uint2(x, y)] = outputValue;
                }
            }
            return;
        }
        const uint2 outputPixelBase = uint2(32u + pixelX * 8u, 32u + localRow * 8u);
        for (uint dy = 0u; dy < 8u; ++dy) {
            for (uint dx = 0u; dx < 8u; ++dx) {
                const uint2 outputPixel = outputPixelBase + uint2(dx, dy);
                if (outputPixel.x < ApusrBand.displayWidth && outputPixel.y < ApusrBand.displayHeight) {
                    ApusrOutput[outputPixel] = outputValue;
                }
            }
        }
        return;
    }

    const uint absoluteModelY = ApusrBand.outputFirstRow + localRow;
    const uint2 modelPixel = uint2(pixelX, absoluteModelY);
    const uint2 displayBase = modelPixel * 2u;
    const uint4 packedVss = ApusrStage13ConvNext(modelPixel);
    const float exposureValue = ApusrLoadExposureValue();
    int2 cacheBase;
    float3 inputColorCache[4][4];
    ApusrLoadModelFilterInputCache(displayBase, exposureValue, cacheBase, inputColorCache);

    [unroll]
    for (uint py = 0u; py < 2u; ++py) {
        [unroll]
        for (uint px = 0u; px < 2u; ++px) {
            const uint2 phase = uint2(px, py);
            ApusrStoreStage13Output(modelPixel, phase, displayBase + phase, packedVss, cacheBase, inputColorCache, exposureValue);
        }
    }
}
