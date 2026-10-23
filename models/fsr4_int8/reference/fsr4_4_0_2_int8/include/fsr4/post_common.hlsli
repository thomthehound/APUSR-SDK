#include "mlsr_optimized_includes.hlsli"

#define RESOLUTION_1080 0
#define RESOLUTION_2160 1
#define RESOLUTION_4320 2

#ifndef APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT
#define APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT 0
#endif

#ifndef APUSR_FSR4_I8_POST_PAIRED_CT2D
#define APUSR_FSR4_I8_POST_PAIRED_CT2D 0
#endif

#ifndef APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE
#define APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE 0
#endif

int2 model_filter_center(uint2 tid)
{
    const float2 lr_pos_f = (-0.5 + inv_scale / 2 + float2(tid) * inv_scale) + jitter;
    return int2(round(lr_pos_f));
}

void load_model_filter_input_cache(const uint2 tidBase, const float exposureValue, out int2 cacheBase, out float3 inputColorCache[4][4])
{
    const int2 center00 = model_filter_center(tidBase);
    const int2 center10 = model_filter_center(tidBase + uint2(1, 0));
    const int2 center01 = model_filter_center(tidBase + uint2(0, 1));
    const int2 center11 = model_filter_center(tidBase + uint2(1, 1));

    cacheBase = min(min(center00, center10), min(center01, center11)) - int2(1, 1);

    [unroll]
    for (uint cacheY = 0; cacheY < 4; ++cacheY)
    {
        [unroll]
        for (uint cacheX = 0; cacheX < 4; ++cacheX)
        {
            inputColorCache[cacheY][cacheX] = ApplyMuLaw(LoadInputColor(cacheBase + int2(cacheX, cacheY)) * exposureValue);
        }
    }
}

#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT
#ifndef APUSR_FSR4_I8_POST_MODEL_FILTER_INPUT_TILE_SIZE
#if APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE
#define APUSR_FSR4_I8_POST_MODEL_FILTER_INPUT_TILE_SIZE 18
#else
#define APUSR_FSR4_I8_POST_MODEL_FILTER_INPUT_TILE_SIZE 24
#endif
#endif
#ifndef APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA
#define APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA 0
#endif
static const uint APUSR_FSR4_POST_FILTER_INPUT_TILE_W = APUSR_FSR4_I8_POST_MODEL_FILTER_INPUT_TILE_SIZE;
static const uint APUSR_FSR4_POST_FILTER_INPUT_TILE_H = APUSR_FSR4_I8_POST_MODEL_FILTER_INPUT_TILE_SIZE;
static const uint APUSR_FSR4_POST_FILTER_INPUT_PIXELS = APUSR_FSR4_POST_FILTER_INPUT_TILE_W * APUSR_FSR4_POST_FILTER_INPUT_TILE_H;
#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA
groupshared float apusr_post_filter_input_tile_r[APUSR_FSR4_POST_FILTER_INPUT_PIXELS];
groupshared float apusr_post_filter_input_tile_g[APUSR_FSR4_POST_FILTER_INPUT_PIXELS];
groupshared float apusr_post_filter_input_tile_b[APUSR_FSR4_POST_FILTER_INPUT_PIXELS];
#else
groupshared float3 apusr_post_filter_input_tile[APUSR_FSR4_POST_FILTER_INPUT_PIXELS];
#endif

uint apusrPostFilterInputTileOffset(uint x, uint y)
{
    return y * APUSR_FSR4_POST_FILTER_INPUT_TILE_W + x;
}

void apusrPreloadModelFilterInputTile(
    const float exposureValue,
    const uint3 numThreads,
    const uint3 groupThreadID,
    const uint3 groupID,
    out int2 cacheBase)
{
    // One post thread produces a 2x2 high-resolution output quad.  Therefore an
    // 8x8 post group covers a 16x16 output region.  The model-filter sample
    // coordinate is monotonic in output x/y, so the corner centers bound the
    // complete set of 3x3 sample neighborhoods used by the group.
    const uint2 outputBase = groupID.xy * numThreads.xy * 2;
    const uint2 outputMax = outputBase + numThreads.xy * 2 - uint2(1, 1);

    const int2 center00 = model_filter_center(outputBase);
    const int2 center10 = model_filter_center(uint2(outputMax.x, outputBase.y));
    const int2 center01 = model_filter_center(uint2(outputBase.x, outputMax.y));
    const int2 center11 = model_filter_center(outputMax);

    const int2 minCenter = min(min(center00, center10), min(center01, center11));
    const int2 maxCenter = max(max(center00, center10), max(center01, center11));
    cacheBase = minCenter - int2(1, 1);
#if APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE
    const uint2 activeExtent = uint2(maxCenter - minCenter + int2(3, 3));
#endif

    const uint flatThread = groupThreadID.y * numThreads.x + groupThreadID.x;
    const uint threadCount = numThreads.x * numThreads.y;
    for (uint tileIndex = flatThread; tileIndex < APUSR_FSR4_POST_FILTER_INPUT_PIXELS; tileIndex += threadCount)
    {
        const uint localX = tileIndex % APUSR_FSR4_POST_FILTER_INPUT_TILE_W;
        const uint localY = tileIndex / APUSR_FSR4_POST_FILTER_INPUT_TILE_W;
#if APUSR_VULKAN_POST_ADAPTIVE_INPUT_CACHE
        if (localX >= activeExtent.x || localY >= activeExtent.y)
            continue;
#endif
        const float3 cachedColor = ApplyMuLaw(LoadInputColor(cacheBase + int2(localX, localY)) * exposureValue);
#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA
        apusr_post_filter_input_tile_r[tileIndex] = cachedColor.r;
        apusr_post_filter_input_tile_g[tileIndex] = cachedColor.g;
        apusr_post_filter_input_tile_b[tileIndex] = cachedColor.b;
#else
        apusr_post_filter_input_tile[tileIndex] = cachedColor;
#endif
    }

    GroupMemoryBarrierWithGroupSync();
}

float3 apusrLoadModelFilterInputTile(const int2 samplePos, const int2 cacheBase)
{
    const int2 local = samplePos - cacheBase;
    const uint tileOffset = apusrPostFilterInputTileOffset(uint(local.x), uint(local.y));
#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA
    return float3(
        apusr_post_filter_input_tile_r[tileOffset],
        apusr_post_filter_input_tile_g[tileOffset],
        apusr_post_filter_input_tile_b[tileOffset]);
#else
    return apusr_post_filter_input_tile[tileOffset];
#endif
}
#endif

struct ApusrPostGaussian3x3Delta
{
    float linearX;
    float linearY;
    float quadraticX;
    float quadraticY;
    float crossXY;
};

ApusrPostGaussian3x3Delta apusrPostGaussian3x3Delta(
    const float centerX,
    const float centerY,
    const float xStep,
    const float yStep,
    const float correlationFactor,
    const float scaleFactorX,
    const float scaleFactorY)
{
    // The filter consumes only normalized Gaussian weights. Factor out the
    // center tap, whose common exp(Ecenter) therefore cancels, and evaluate the
    // other taps from E(tap)-E(center). Standard FSR quality modes cap the
    // upscale ratio at 3x; with tanh-bounded model parameters this keeps every
    // positive center-relative exponent inside finite fp32 exp() range.
    const float gaussianExponent = -0.5 / (0.47 * 0.47);
    const float scaleFactorX2 = scaleFactorX * scaleFactorX;
    const float scaleFactorY2 = scaleFactorY * scaleFactorY;
    const float crossFactor = 2.0 * correlationFactor * scaleFactorX * scaleFactorY;

    ApusrPostGaussian3x3Delta delta;
    delta.linearX = gaussianExponent *
        (2.0 * scaleFactorX2 * centerX * xStep + crossFactor * centerY * xStep);
    delta.linearY = gaussianExponent *
        (2.0 * scaleFactorY2 * centerY * yStep + crossFactor * centerX * yStep);
    delta.quadraticX = gaussianExponent * scaleFactorX2 * xStep * xStep;
    delta.quadraticY = gaussianExponent * scaleFactorY2 * yStep * yStep;
    delta.crossXY = gaussianExponent * crossFactor * xStep * yStep;
    return delta;
}

float apusrPostGaussianWeight(
    const int dx,
    const int dy,
    const ApusrPostGaussian3x3Delta delta)
{
    const float fx = float(dx);
    const float fy = float(dy);
    const float exponentDelta =
        fx * delta.linearX + fy * delta.linearY +
        fx * fx * delta.quadraticX + fy * fy * delta.quadraticY +
        fx * fy * delta.crossXY;
    return (dx == 0 && dy == 0) ? 1.0 : exp(exponentDelta);
}

float3 apply_model_filter(uint x, uint y, const float4 filter, const int2 cacheBase, const float3 inputColorCache[4][4])
{
    const uint kernelSize = 3;

    const float correlationFactor = tanh(filter[0]);
    const float scaleFactorX = 1.0 + tanh(0.5 * filter[1]);
    const float scaleFactorY = 1.0 + tanh(0.5 * filter[2]);

    const float2 lrPosF = (-0.5 + inv_scale / 2 + float2(x, y) * inv_scale) + jitter;
    const int2 lrPosI = int2(round(lrPosF));
    const int kernelRadius = int(kernelSize >> 1);
    const int2 sampleBase = lrPosI - kernelRadius;
    const float centerX = (float(lrPosI.x) - lrPosF.x) * scale.x;
    const float centerY = (float(lrPosI.y) - lrPosF.y) * scale.y;
    const ApusrPostGaussian3x3Delta delta = apusrPostGaussian3x3Delta(
        centerX, centerY, scale.x, scale.y, correlationFactor, scaleFactorX, scaleFactorY);

    float3 upscaledColor = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint dy = 0; dy < kernelSize; ++dy)
    {
        const int yIn = sampleBase.y + int(dy);
        const uint cacheY = uint(yIn - cacheBase.y);

        [unroll]
        for (uint dx = 0; dx < kernelSize; ++dx)
        {
            const int xIn = sampleBase.x + int(dx);
            const uint cacheX = uint(xIn - cacheBase.x);
            const float weight = apusrPostGaussianWeight(
                int(dx) - kernelRadius, int(dy) - kernelRadius, delta);
            totalWeight += weight;
            upscaledColor += inputColorCache[cacheY][cacheX] * weight;
        }
    }
    upscaledColor /= totalWeight;

    const float blendingFactor = 0.5 * (1.0 + tanh(0.5 * filter[3]));
    return upscaledColor * (1.0 - blendingFactor) + r_reprojected_color[uint2(x, y)] * blendingFactor;
}

#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT
float3 apply_model_filter_from_shared_input(uint x, uint y, const float4 filter, const int2 cacheBase, const float exposureValue)
{
    const uint kernelSize = 3;

    const float correlationFactor = tanh(filter[0]);
    const float scaleFactorX = 1.0 + tanh(0.5 * filter[1]);
    const float scaleFactorY = 1.0 + tanh(0.5 * filter[2]);

    const float2 lrPosF = (-0.5 + inv_scale / 2 + float2(x, y) * inv_scale) + jitter;
    const int2 lrPosI = int2(round(lrPosF));
    const int kernelRadius = int(kernelSize >> 1);
    const int2 sampleBase = lrPosI - kernelRadius;
    const float centerX = (float(lrPosI.x) - lrPosF.x) * scale.x;
    const float centerY = (float(lrPosI.y) - lrPosF.y) * scale.y;
    const ApusrPostGaussian3x3Delta delta = apusrPostGaussian3x3Delta(
        centerX, centerY, scale.x, scale.y, correlationFactor, scaleFactorX, scaleFactorY);

    float3 upscaledColor = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint dy = 0; dy < kernelSize; ++dy)
    {
        const int yIn = sampleBase.y + int(dy);

        [unroll]
        for (uint dx = 0; dx < kernelSize; ++dx)
        {
            const int xIn = sampleBase.x + int(dx);
            const float weight = apusrPostGaussianWeight(
                int(dx) - kernelRadius, int(dy) - kernelRadius, delta);
            totalWeight += weight;
            upscaledColor += apusrLoadModelFilterInputTile(int2(xIn, yIn), cacheBase) * weight;
        }
    }
    upscaledColor /= totalWeight;

    const float blendingFactor = 0.5 * (1.0 + tanh(0.5 * filter[3]));
    return upscaledColor * (1.0 - blendingFactor) + r_reprojected_color[uint2(x, y)] * blendingFactor;
}
#endif


#include "ml2code_runtime/tensor_float16.hlsli"
#include "ml2code_runtime/tensor_int8.hlsli"
#include "ml2code_runtime/scalar_functions.hlsli"

#ifndef APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT
#define APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT 0
#endif

#ifndef APUSR_FSR4_I8_POST_LDS_CNB
#define APUSR_FSR4_I8_POST_LDS_CNB 0
#endif

#if APUSR_FSR4_I8_POST_LDS_CNB
static const uint APUSR_FSR4_POST_CNB_TILE_W = 8;
static const uint APUSR_FSR4_POST_CNB_TILE_H = 8;
static const uint APUSR_FSR4_POST_CNB_HALO = 1;
static const uint APUSR_FSR4_POST_CNB_LOAD_W = APUSR_FSR4_POST_CNB_TILE_W + 2 * APUSR_FSR4_POST_CNB_HALO;
static const uint APUSR_FSR4_POST_CNB_LOAD_H = APUSR_FSR4_POST_CNB_TILE_H + 2 * APUSR_FSR4_POST_CNB_HALO;
static const uint APUSR_FSR4_POST_CNB_C16_DWORDS = 4;
static const uint APUSR_FSR4_POST_CNB_LOAD_PIXELS = APUSR_FSR4_POST_CNB_LOAD_W * APUSR_FSR4_POST_CNB_LOAD_H;
groupshared uint apusr_post_cnb_c16_halo_tile[APUSR_FSR4_POST_CNB_LOAD_PIXELS * APUSR_FSR4_POST_CNB_C16_DWORDS];

uint apusrPostCnbTileOffset(uint x, uint y, uint c4)
{
    return ((y * APUSR_FSR4_POST_CNB_LOAD_W + x) * APUSR_FSR4_POST_CNB_C16_DWORDS) + c4;
}

uint apusrPostCnbLoadSharedC16(uint x, uint y, uint c4)
{
    return apusr_post_cnb_c16_halo_tile[apusrPostCnbTileOffset(x, y, c4)];
}
#endif

template<typename SW>
half4 ct2d_iteration_4(
    const uint z,
    const int2 pk,
    const uint4 packedVs,
    const QuantizedTensor4i8_HWCN<SW> weights,
    const float inputsScale)
{
    int4 accumulator = 0;
    uint weightsOffset = weights.OffsetOf(uint4(pk, z, 0));

    [unroll]
    for (uint c = 0 ; c != 16; c += 16)
    {
        const uint4 w0 = weights.storage.Load4(weightsOffset);
        weightsOffset += 16;

        accumulator.x = dot4add_i8packed(w0.x, packedVs.x, accumulator.x);
        accumulator.x = dot4add_i8packed(w0.y, packedVs.y, accumulator.x);
        accumulator.x = dot4add_i8packed(w0.z, packedVs.z, accumulator.x);
        accumulator.x = dot4add_i8packed(w0.w, packedVs.w, accumulator.x);
    }

    [unroll]
    for (uint c = 0 ; c != 16; c += 16)
    {
        const uint4 w1 = weights.storage.Load4(weightsOffset);
        weightsOffset += 16*weights.storageByteStrides.w;

        accumulator.y = dot4add_i8packed(w1.x, packedVs.x, accumulator.y);
        accumulator.y = dot4add_i8packed(w1.y, packedVs.y, accumulator.y);
        accumulator.y = dot4add_i8packed(w1.z, packedVs.z, accumulator.y);
        accumulator.y = dot4add_i8packed(w1.w, packedVs.w, accumulator.y);
    }

    [unroll]
    for (uint c = 0 ; c != 16; c += 16)
    {
        const uint4 w2 = weights.storage.Load4(weightsOffset);
        weightsOffset += 16*weights.storageByteStrides.w;

        accumulator.z = dot4add_i8packed(w2.x, packedVs.x, accumulator.z);
        accumulator.z = dot4add_i8packed(w2.y, packedVs.y, accumulator.z);
        accumulator.z = dot4add_i8packed(w2.z, packedVs.z, accumulator.z);
        accumulator.z = dot4add_i8packed(w2.w, packedVs.w, accumulator.z);
    }


    [unroll]
    for (uint c = 0 ; c != 16; c += 16)
    {
        const uint4 w3 = weights.storage.Load4(weightsOffset);
        weightsOffset += 16*weights.storageByteStrides.w;

        accumulator.w = dot4add_i8packed(w3.x, packedVs.x, accumulator.w);
        accumulator.w = dot4add_i8packed(w3.y, packedVs.y, accumulator.w);
        accumulator.w = dot4add_i8packed(w3.z, packedVs.z, accumulator.w);
        accumulator.w = dot4add_i8packed(w3.w, packedVs.w, accumulator.w);
    }

    return half4(accumulator * inputsScale.xxxx);
}


#if APUSR_FSR4_I8_POST_PAIRED_CT2D
template<typename SW>
void ct2d_iteration_4_pair_0_4(
    const int2 pk,
    const uint4 packedVs,
    const QuantizedTensor4i8_HWCN<SW> weights,
    const float inputsScale,
    out half4 modelParameters,
    out half4 recurrentChannels)
{
    int4 modelAccumulator = 0;
    int4 recurrentAccumulator = 0;
    uint modelWeightsOffset = weights.OffsetOf(uint4(pk, 0, 0));
    uint recurrentWeightsOffset = weights.OffsetOf(uint4(pk, 4, 0));

    {
        const uint4 wm = weights.storage.Load4(modelWeightsOffset);
        const uint4 wr = weights.storage.Load4(recurrentWeightsOffset);
        modelWeightsOffset += 16;
        recurrentWeightsOffset += 16;

        modelAccumulator.x = dot4add_i8packed(wm.x, packedVs.x, modelAccumulator.x);
        modelAccumulator.x = dot4add_i8packed(wm.y, packedVs.y, modelAccumulator.x);
        modelAccumulator.x = dot4add_i8packed(wm.z, packedVs.z, modelAccumulator.x);
        modelAccumulator.x = dot4add_i8packed(wm.w, packedVs.w, modelAccumulator.x);

        recurrentAccumulator.x = dot4add_i8packed(wr.x, packedVs.x, recurrentAccumulator.x);
        recurrentAccumulator.x = dot4add_i8packed(wr.y, packedVs.y, recurrentAccumulator.x);
        recurrentAccumulator.x = dot4add_i8packed(wr.z, packedVs.z, recurrentAccumulator.x);
        recurrentAccumulator.x = dot4add_i8packed(wr.w, packedVs.w, recurrentAccumulator.x);
    }

    {
        const uint4 wm = weights.storage.Load4(modelWeightsOffset);
        const uint4 wr = weights.storage.Load4(recurrentWeightsOffset);
        modelWeightsOffset += 16 * weights.storageByteStrides.w;
        recurrentWeightsOffset += 16 * weights.storageByteStrides.w;

        modelAccumulator.y = dot4add_i8packed(wm.x, packedVs.x, modelAccumulator.y);
        modelAccumulator.y = dot4add_i8packed(wm.y, packedVs.y, modelAccumulator.y);
        modelAccumulator.y = dot4add_i8packed(wm.z, packedVs.z, modelAccumulator.y);
        modelAccumulator.y = dot4add_i8packed(wm.w, packedVs.w, modelAccumulator.y);

        recurrentAccumulator.y = dot4add_i8packed(wr.x, packedVs.x, recurrentAccumulator.y);
        recurrentAccumulator.y = dot4add_i8packed(wr.y, packedVs.y, recurrentAccumulator.y);
        recurrentAccumulator.y = dot4add_i8packed(wr.z, packedVs.z, recurrentAccumulator.y);
        recurrentAccumulator.y = dot4add_i8packed(wr.w, packedVs.w, recurrentAccumulator.y);
    }

    {
        const uint4 wm = weights.storage.Load4(modelWeightsOffset);
        const uint4 wr = weights.storage.Load4(recurrentWeightsOffset);
        modelWeightsOffset += 16 * weights.storageByteStrides.w;
        recurrentWeightsOffset += 16 * weights.storageByteStrides.w;

        modelAccumulator.z = dot4add_i8packed(wm.x, packedVs.x, modelAccumulator.z);
        modelAccumulator.z = dot4add_i8packed(wm.y, packedVs.y, modelAccumulator.z);
        modelAccumulator.z = dot4add_i8packed(wm.z, packedVs.z, modelAccumulator.z);
        modelAccumulator.z = dot4add_i8packed(wm.w, packedVs.w, modelAccumulator.z);

        recurrentAccumulator.z = dot4add_i8packed(wr.x, packedVs.x, recurrentAccumulator.z);
        recurrentAccumulator.z = dot4add_i8packed(wr.y, packedVs.y, recurrentAccumulator.z);
        recurrentAccumulator.z = dot4add_i8packed(wr.z, packedVs.z, recurrentAccumulator.z);
        recurrentAccumulator.z = dot4add_i8packed(wr.w, packedVs.w, recurrentAccumulator.z);
    }

    {
        const uint4 wm = weights.storage.Load4(modelWeightsOffset);
        const uint4 wr = weights.storage.Load4(recurrentWeightsOffset);

        modelAccumulator.w = dot4add_i8packed(wm.x, packedVs.x, modelAccumulator.w);
        modelAccumulator.w = dot4add_i8packed(wm.y, packedVs.y, modelAccumulator.w);
        modelAccumulator.w = dot4add_i8packed(wm.z, packedVs.z, modelAccumulator.w);
        modelAccumulator.w = dot4add_i8packed(wm.w, packedVs.w, modelAccumulator.w);

        recurrentAccumulator.w = dot4add_i8packed(wr.x, packedVs.x, recurrentAccumulator.w);
        recurrentAccumulator.w = dot4add_i8packed(wr.y, packedVs.y, recurrentAccumulator.w);
        recurrentAccumulator.w = dot4add_i8packed(wr.z, packedVs.z, recurrentAccumulator.w);
        recurrentAccumulator.w = dot4add_i8packed(wr.w, packedVs.w, recurrentAccumulator.w);
    }

    modelParameters = half4(modelAccumulator * inputsScale.xxxx);
    recurrentChannels = half4(recurrentAccumulator * inputsScale.xxxx);
}
#endif

template<typename SI, typename SW0, typename SB0, typename SW1, typename SB1, typename SW2, typename SW3, typename SB2, typename SB3, typename SO>
void FusedConv2D_DW_Conv2D_PW_Relu_Conv2D_Add_ConvTranspose2D(
    const float rcprOutputScale0,
    const float inputScale1,
    const float rcprOutputScale1,
    const float inputScale2,
    const float cnbOutputScale,
    const QuantizedTensor3i8_NHWC<SI> input,
    const QuantizedTensor4i8_NHWC<SW0> weights0,
    const Tensor1h<SB0> bias0,
    const QuantizedTensor4i8_NHWC<SW1> weights1,
    const Tensor1h<SB1> bias1,
    const QuantizedTensor4i8_NHWC<SW2> weights2,
    const Tensor1h<SB2> bias2,
    const QuantizedTensor4i8_HWCN<SW3> ct2d_weights,
    const Tensor1h<SB3> ct2d_bias,
    const Tensor3h_NHWC<SO> output,
    const uint3 numThreads,
    const uint3 groupThreadID,
    const uint3 groupID,
    const uint3 dispatchThreadID)
{
    const uint numFeatures = weights0.logicalSize.w;
    const int3 workOffset = int3(dispatchThreadID.xy, 0);

    const int3 poBase = workOffset;
    const int3 piBase = int3(poBase.xy, 0) - int3(1, 1, 0);

    // CNB
    int accumulator[16];
#if !APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT
    int relu_output[32];
#endif
    [unroll]
    for (uint i = 0; i < 16; ++i) { accumulator[i] = 0; }
#if !APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT
    [unroll]
    for (uint i = 0; i < 32; ++i) { relu_output[i] = 0; }
#endif

#if APUSR_FSR4_I8_POST_LDS_CNB
    // Preload the 8x8 group's 10x10 C16 halo exactly once.  This preserves the
    // generated post dispatch and the original ValidPosition semantics: cells
    // outside the model tensor are zero, cells inside are loaded from the same
    // scratch locations the generated per-thread path would have used.
    const uint flatThread = groupThreadID.y * numThreads.x + groupThreadID.x;
    const uint threadCount = numThreads.x * numThreads.y;
    const int2 tileBase = int2(groupID.xy * uint2(APUSR_FSR4_POST_CNB_TILE_W, APUSR_FSR4_POST_CNB_TILE_H)) - int2(1, 1);
    for (uint tileIndex = flatThread; tileIndex < APUSR_FSR4_POST_CNB_LOAD_PIXELS; tileIndex += threadCount)
    {
        const uint localX = tileIndex % APUSR_FSR4_POST_CNB_LOAD_W;
        const uint localY = tileIndex / APUSR_FSR4_POST_CNB_LOAD_W;
        const int3 inputPos = int3(tileBase + int2(localX, localY), 0);
        const uint4 packed = ValidPosition(input, inputPos) ? input.storage.Load4(input.OffsetOf(inputPos)) : uint4(0, 0, 0, 0);
        const uint sharedBase = tileIndex * APUSR_FSR4_POST_CNB_C16_DWORDS;
        apusr_post_cnb_c16_halo_tile[sharedBase + 0] = packed.x;
        apusr_post_cnb_c16_halo_tile[sharedBase + 1] = packed.y;
        apusr_post_cnb_c16_halo_tile[sharedBase + 2] = packed.z;
        apusr_post_cnb_c16_halo_tile[sharedBase + 3] = packed.w;
    }
    GroupMemoryBarrierWithGroupSync();

    // First convolution
    [unroll]
    for (uint ky = 0; ky < 3; ++ky)
        [unroll]
        for (uint kx = 0; kx < 3; ++kx)
        {
            const uint4 inputDwords = uint4(
                apusrPostCnbLoadSharedC16(groupThreadID.x + kx, groupThreadID.y + ky, 0),
                apusrPostCnbLoadSharedC16(groupThreadID.x + kx, groupThreadID.y + ky, 1),
                apusrPostCnbLoadSharedC16(groupThreadID.x + kx, groupThreadID.y + ky, 2),
                apusrPostCnbLoadSharedC16(groupThreadID.x + kx, groupThreadID.y + ky, 3));

            [unroll]
            for (uint f = 0; f < 16; ++f)
            {
                const uint weightsOffset = weights0.OffsetOf(uint4(kx, ky, 0, f));
                const uint4 weightsDwords = weights0.storage.Load4(weightsOffset);
                accumulator[f] = dot4add_i8packed(inputDwords.x, weightsDwords.x, accumulator[f]);
                accumulator[f] = dot4add_i8packed(inputDwords.y, weightsDwords.y, accumulator[f]);
                accumulator[f] = dot4add_i8packed(inputDwords.z, weightsDwords.z, accumulator[f]);
                accumulator[f] = dot4add_i8packed(inputDwords.w, weightsDwords.w, accumulator[f]);
            }
        }
#else
    // First convolution
    [unroll]
    for (uint ky = 0; ky < weights0.logicalSize.y; ++ky)
        [unroll]
        for (uint kx = 0; kx < weights0.logicalSize.x; ++kx)
        {
            if (!ValidPosition(input, int3(piBase.xy + int2(kx, ky), 0)))
                continue;

            int8_t4_packed vs[16/4];
            uint inputIndex = 0;
            [unroll]
            for (uint c = 0; c != weights0.logicalSize.z; c += 16)
            {
                const uint inputOffset = input.OffsetOf(piBase + int3(kx, ky, c));
                const uint4 inputDwords = input.storage.Load4(inputOffset);
                vs[inputIndex++] = inputDwords.x;
                vs[inputIndex++] = inputDwords.y;
                vs[inputIndex++] = inputDwords.z;
                vs[inputIndex++] = inputDwords.w;
            }

            [unroll]
            for (uint f = 0; f != numFeatures; ++f)
            {
                inputIndex = 0;
                [unroll]
                for (uint c = 0; c != weights0.logicalSize.z; c += 16)
                {
                    const uint weightsOffset = weights0.OffsetOf(uint4(kx, ky, c, f));
                    const uint4 weightsDwords = weights0.storage.Load4(weightsOffset);

                    accumulator[f] = dot4add_i8packed(vs[inputIndex++], weightsDwords.x, accumulator[f]);
                    accumulator[f] = dot4add_i8packed(vs[inputIndex++], weightsDwords.y, accumulator[f]);
                    accumulator[f] = dot4add_i8packed(vs[inputIndex++], weightsDwords.z, accumulator[f]);
                    accumulator[f] = dot4add_i8packed(vs[inputIndex++], weightsDwords.w, accumulator[f]);
                }
            }
        }
#endif

    int8_t4_packed conv_result[16/4];
    float inputscale = input.quantizationScale * weights0.quantizationScale;
    [unroll]
    for (uint f = 0; f != numFeatures; f+=4)
    {
        const float4 conv_accumulator = float4(accumulator[f], accumulator[f+1], accumulator[f+2], accumulator[f+3]);
        const int4 quantized_vs = round((conv_accumulator * inputscale + Load4hA(bias0, f)) * rcprOutputScale0);
        conv_result[f/4] = pack_clamp_s8(quantized_vs);
    }

#if APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT
    int conv3_output[16];
    [unroll]
    for (int i = 0; i < 16; ++i) { conv3_output[i] = 0; }

    // Second Convolution + ReLU + Third Convolution
    //
    // The generated post shader used to materialize all 32 hidden accumulators
    // and all eight packed ReLU vectors before starting the C32->C16 contract.
    // That is exact, but it expands the hottest post-CNB live range.  The
    // optimized DP4A lane keeps the exact quantized ReLU boundary, then
    // immediately consumes each four-hidden-channel block into the final C16
    // contract accumulators.  The int accumulation order for each output channel
    // remains h=0..31, matching the generated kernel.
    inputscale = inputScale1 * weights1.quantizationScale;
    [unroll]
    for (uint h4 = 0; h4 < 32; h4 += 4)
    {
        int hidden_accumulator[4];
        [unroll]
        for (uint h = 0; h < 4; ++h)
        {
            const uint hiddenChannel = h4 + h;
            const uint weightsOffset1 = weights1.OffsetOf(uint4(0,0,0,hiddenChannel));
            const uint4 weightsDwords1 = weights1.storage.Load4(weightsOffset1);

            int hidden = 0;
            hidden = dot4add_i8packed(conv_result[0], weightsDwords1.x, hidden);
            hidden = dot4add_i8packed(conv_result[1], weightsDwords1.y, hidden);
            hidden = dot4add_i8packed(conv_result[2], weightsDwords1.z, hidden);
            hidden = dot4add_i8packed(conv_result[3], weightsDwords1.w, hidden);
            hidden_accumulator[h] = hidden;
        }

        const float4 vs = float4(hidden_accumulator[0], hidden_accumulator[1], hidden_accumulator[2], hidden_accumulator[3]);
        const int4 quantized_vs = round((vs * inputscale + Load4hA(bias1, h4)) * rcprOutputScale1);
        #if FFX_FSR4_EXPERIMENT_UNSIGNED_RELU
        const int8_t4_packed relu_packed = PackUnsignedReluActivationU8(quantized_vs);
        #else
        const int8_t4_packed relu_packed = pack_s8(ReluClampS8(quantized_vs));
        #endif

        [unroll]
        for (uint i = 0; i < 16; i++)
        {
            const uint weightsOffset2 = weights2.OffsetOf(uint4(0,0,h4,i));
            const uint weightsDword2 = weights2.storage.Load1(weightsOffset2);
            conv3_output[i] = dot4add_i8packed(relu_packed, weightsDword2, conv3_output[i]);
        }
    }
#else
    // Second Convolution + Relu
    [unroll]
    for (uint i = 0; i < 32; i++)
    {
        const uint weightsOffset = weights1.OffsetOf(uint4(0,0,0,i));
        const uint4 weightsDwords = weights1.storage.Load4(weightsOffset);

        relu_output[i] = dot4add_i8packed(conv_result[0], weightsDwords.x, relu_output[i]);
        relu_output[i] = dot4add_i8packed(conv_result[1], weightsDwords.y, relu_output[i]);
        relu_output[i] = dot4add_i8packed(conv_result[2], weightsDwords.z, relu_output[i]);
        relu_output[i] = dot4add_i8packed(conv_result[3], weightsDwords.w, relu_output[i]);
    }

    int8_t4_packed relu_result[32/4];
    inputscale = inputScale1 * weights1.quantizationScale;
    [unroll]
    for (uint i = 0; i < 32; i+=4)
    {
        const float4 vs = float4(relu_output[i], relu_output[i+1], relu_output[i+2], relu_output[i+3]);
        const int4 quantized_vs = round((vs * inputscale + Load4hA(bias1, i)) * rcprOutputScale1);
        #if FFX_FSR4_EXPERIMENT_UNSIGNED_RELU
        relu_result[i/4] = PackUnsignedReluActivationU8(quantized_vs);
        #else
        relu_result[i/4] = pack_s8(ReluClampS8(quantized_vs));
        #endif
    }

    int conv3_output[16];
    [unroll]
    for (int i = 0; i < 16; ++i) { conv3_output[i] = 0; }

    // Third Convolution
    [unroll]
    for (uint i = 0; i < 16; i++)
    {
        uint weightsOffset = weights2.OffsetOf(uint4(0,0,0,i));
        uint4 weightsDwords = weights2.storage.Load4(weightsOffset);

        conv3_output[i] = dot4add_i8packed(relu_result[0], weightsDwords.x, conv3_output[i]);
        conv3_output[i] = dot4add_i8packed(relu_result[1], weightsDwords.y, conv3_output[i]);
        conv3_output[i] = dot4add_i8packed(relu_result[2], weightsDwords.z, conv3_output[i]);
        conv3_output[i] = dot4add_i8packed(relu_result[3], weightsDwords.w, conv3_output[i]);

        weightsOffset = weights2.OffsetOf(uint4(0,0,16,i));
        weightsDwords = weights2.storage.Load4(weightsOffset);

        conv3_output[i] = dot4add_i8packed(relu_result[4], weightsDwords.x, conv3_output[i]);
        conv3_output[i] = dot4add_i8packed(relu_result[5], weightsDwords.y, conv3_output[i]);
        conv3_output[i] = dot4add_i8packed(relu_result[6], weightsDwords.z, conv3_output[i]);
        conv3_output[i] = dot4add_i8packed(relu_result[7], weightsDwords.w, conv3_output[i]);
    }
#endif

    // Add
    const uint4 inputDwords = input.storage.Load4(input.OffsetOf(int3(poBase.xy, 0)));

    uint4 packedVss;
    inputscale = weights2.quantizationScale * inputScale2;
    const float outputRcprScale = 1.0f / cnbOutputScale;
    [unroll]
    for (uint i = 0; i < 16; i+=4)
    {
        const float4 vs = float4(conv3_output[i], conv3_output[i+1], conv3_output[i+2], conv3_output[i+3]);
        float4 result = vs * inputscale + Load4hA(bias2, i);
        result += unpack_s8s32(inputDwords[i/4]) * input.quantizationScale;

        packedVss[i/4] = pack_clamp_s8(int4(round(result * outputRcprScale)));
    }

    // CT2D
    const float ct2d_input_quantizationScale = cnbOutputScale * ct2d_weights.quantizationScale;
    const uint2 tidBase = dispatchThreadID.xy * 2;

    const uint4 biasDwords = ct2d_bias.storage.Load4(0);

    const float exposureValue = LoadExposureValue();
    const bool shouldConvertColorspace = !rcas_enabled;

    int2 modelFilterCacheBase;
#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT
    apusrPreloadModelFilterInputTile(exposureValue, numThreads, groupThreadID, groupID, modelFilterCacheBase);
#else
    float3 modelFilterInputCache[4][4];
    load_model_filter_input_cache(tidBase, exposureValue, modelFilterCacheBase, modelFilterInputCache);
#endif

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const uint2 pk = uint2(i & 1, i >> 1);
        const uint2 tid = tidBase + pk;

#if APUSR_FSR4_I8_POST_PAIRED_CT2D
        half4 model_parameters_h;
        half4 recurrent = 0;
        ct2d_iteration_4_pair_0_4(pk, packedVss, ct2d_weights, ct2d_input_quantizationScale, model_parameters_h, recurrent);
        recurrent += Unpack4h(biasDwords.zw);
        recurrent = half4(0.5f * (1.0f + tanh(0.5f * recurrent)));
        rw_recurrent_0[tid] = recurrent; // 4k tid

        const float4 model_parameters = float4(model_parameters_h + Unpack4h(biasDwords.xy));
#else
        // Recurrent Channels
        half4 recurrent = ct2d_iteration_4(4, pk, packedVss, ct2d_weights, ct2d_input_quantizationScale) + Unpack4h(biasDwords.zw);
        recurrent = half4(0.5f * (1.0f + tanh(0.5f * recurrent)));
        rw_recurrent_0[tid] = recurrent; // 4k tid

        const float4 model_parameters = ct2d_iteration_4(0, pk, packedVss, ct2d_weights, ct2d_input_quantizationScale) + Unpack4h(biasDwords.xy);
#endif

#if APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT
        float3 model_color = apply_model_filter_from_shared_input(tid.x, tid.y, model_parameters, modelFilterCacheBase, exposureValue); // 4k tid
#else
        float3 model_color = apply_model_filter(tid.x, tid.y, model_parameters, modelFilterCacheBase, modelFilterInputCache); // 4k tid
#endif

#if FFX_DEBUG_VISUALIZE
        {
            float blending_factor = (1.0f / (1.0f + exp(-model_parameters.w)));
            blending_factor = 1.0f - blending_factor;
            DebugWritePredictedBlendFactor(tid, blending_factor);
        }
#endif

        // Model color (in model color space) is retained for history.
        rw_history_color[tid] = model_color; // 4k tid

        // "Final" color is converted to input color space unless RCAS is enabled
        // in which case RCAS will convert it.
        float3 final_color = model_color;

        if (shouldConvertColorspace)
            final_color.rgb = RemoveMuLaw(final_color.rgb) / exposureValue;

        final_color = clamp(final_color, 0, 64000); // Don't overflow storage (half) type.

        rw_mlsr_output_color[tid] = final_color; // 4k tid

    }

}

RWByteAddressBuffer buffer_fused_quantized_NHWC_output;
