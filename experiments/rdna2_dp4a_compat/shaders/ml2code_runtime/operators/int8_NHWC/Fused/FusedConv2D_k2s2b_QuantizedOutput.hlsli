// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
#pragma once

// APUSR-owned experimental replacement for the imported ML2Code k2/s2 INT8
// convolution used by FSR4 passes 3 and 6. It is retained only in the RDNA2
// compatibility quarantine; the imported reference runtime remains untouched.
//
// APUSR_FSR4_I8_DP4A_SCHEDULE_DEPTH4 bounds each signed packed-DOT4
// dependency chain at four operations, matching the strongest scheduling clue
// from later FSR4 INT8 implementations without multiplying the long-lived
// per-output accumulator set.
//
// APUSR_FSR4_I8_DP4A_SCHEDULE_DECHAINED makes every packed DOT4 independent
// and reduces the results with ordinary i32 adds. It is intentionally an
// experimental comparison point, not a product fallback or scalar emulation.

#include "ml2code_runtime/tensor_float16.hlsli"
#include "ml2code_runtime/tensor_int8.hlsli"
#include "ml2code_runtime/tensor_int32.hlsli"

#if defined(APUSR_FSR4_I8_DP4A_SCHEDULE_DEPTH4) == defined(APUSR_FSR4_I8_DP4A_SCHEDULE_DECHAINED)
#error "Select exactly one APUSR pass3/pass6 DP4A scheduling experiment."
#endif

inline int ApusrDp4aDepth4(
    const uint4 weightsDwords,
    const int8_t4_packed v0,
    const int8_t4_packed v1,
    const int8_t4_packed v2,
    const int8_t4_packed v3)
{
    int partial = 0;
    partial = dot4add_i8packed(weightsDwords.x, v0, partial);
    partial = dot4add_i8packed(weightsDwords.y, v1, partial);
    partial = dot4add_i8packed(weightsDwords.z, v2, partial);
    partial = dot4add_i8packed(weightsDwords.w, v3, partial);
    return partial;
}

inline int ApusrDp4aDechained4(
    const uint4 weightsDwords,
    const int8_t4_packed v0,
    const int8_t4_packed v1,
    const int8_t4_packed v2,
    const int8_t4_packed v3)
{
    const int d0 = dot4add_i8packed(weightsDwords.x, v0, 0);
    const int d1 = dot4add_i8packed(weightsDwords.y, v1, 0);
    const int d2 = dot4add_i8packed(weightsDwords.z, v2, 0);
    const int d3 = dot4add_i8packed(weightsDwords.w, v3, 0);
    return (d0 + d1) + (d2 + d3);
}

inline int ApusrDp4aPartial4(
    const uint4 weightsDwords,
    const int8_t4_packed v0,
    const int8_t4_packed v1,
    const int8_t4_packed v2,
    const int8_t4_packed v3)
{
#if defined(APUSR_FSR4_I8_DP4A_SCHEDULE_DEPTH4)
    return ApusrDp4aDepth4(weightsDwords, v0, v1, v2, v3);
#else
    return ApusrDp4aDechained4(weightsDwords, v0, v1, v2, v3);
#endif
}

template<typename SI, typename SW, typename SB, typename SO>
void ApusrConv_16_32(
    const float quantFactor0,
    const float quantFactor1,
    const QuantizedTensor3i8_NHWC<SI> input,
    const QuantizedTensor4i8_NHWC<SW> weights,
    const Tensor1h<SB> bias,
    const Tensor3i8_NHWC<SO> output,
    const int3 piBase,
    const int3 poBase,
    const uint numFeatures)
{
    int accumulator[32];
    [unroll]
    for (uint f = 0; f != 32; f += 4)
    {
        accumulator[f] = 0;
        accumulator[f + 1] = 0;
        accumulator[f + 2] = 0;
        accumulator[f + 3] = 0;
    }

    [unroll]
    for (uint ky = 0; ky < weights.logicalSize.y; ++ky)
    {
        [unroll]
        for (uint kx = 0; kx < weights.logicalSize.x; ++kx)
        {
            const int3 inputOffset = input.OffsetOf(piBase + int3(kx, ky, 0));
            const uint4 inputDwords = input.storage.Load4(inputOffset.x);

            [unroll]
            for (uint f = 0; f < numFeatures; ++f)
            {
                const uint4 weightsOffset = weights.OffsetOf(uint4(kx, ky, 0, f));
                const uint4 weightsDwords = weights.storage.Load4(weightsOffset.x);
                const int partial = ApusrDp4aPartial4(
                    weightsDwords,
                    inputDwords.x,
                    inputDwords.y,
                    inputDwords.z,
                    inputDwords.w);
                accumulator[f] += partial;
            }
        }
    }

    uint storeIndex = output.OffsetOf(poBase);
    uint4 storeDwords;
    [unroll]
    for (uint f = 0; f < numFeatures / 2; f += 4)
    {
        const int4 acc = int4(accumulator[f], accumulator[f + 1], accumulator[f + 2], accumulator[f + 3]);
        const int16_t4 result = int16_t4(round((acc * weights.quantizationScale * input.quantizationScale + Load4hA(bias, f)) * (1.0 / quantFactor0)));
        storeDwords[f / 4] = pack_clamp_s8(result);
    }
    output.storage.Store4(storeIndex, storeDwords);

    [unroll]
    for (uint f = numFeatures / 2; f < numFeatures; f += 4)
    {
        const int4 acc = int4(accumulator[f], accumulator[f + 1], accumulator[f + 2], accumulator[f + 3]);
        const int16_t4 result = int16_t4(round((acc * weights.quantizationScale * input.quantizationScale + Load4hA(bias, f)) * (1.0 / quantFactor1)));
        storeDwords[(f - numFeatures / 2) / 4] = pack_clamp_s8(result);
    }
    storeIndex = output.OffsetOf(poBase + int3(0, 0, 16));
    output.storage.Store4(storeIndex, storeDwords);
}

template<typename SI, typename SW, typename SB, typename SO>
void ApusrConv_32_64(
    const float quantFactor0,
    const float quantFactor1,
    const QuantizedTensor3i8_NHWC<SI> input,
    const QuantizedTensor4i8_NHWC<SW> weights,
    const Tensor1h<SB> bias,
    const Tensor3i8_NHWC<SO> output,
    const int3 piBase,
    const int3 poBase,
    const uint numFeatures)
{
    int accumulator[64];
    [unroll]
    for (uint f = 0; f < 64; f += 4)
    {
        accumulator[f] = 0;
        accumulator[f + 1] = 0;
        accumulator[f + 2] = 0;
        accumulator[f + 3] = 0;
    }

    [unroll]
    for (uint ky = 0; ky < weights.logicalSize.y; ++ky)
    {
        [unroll]
        for (uint kx = 0; kx < weights.logicalSize.x; ++kx)
        {
            int3 inputOffset = input.OffsetOf(piBase + int3(kx, ky, 0));
            const uint4 inputDwords0 = input.storage.Load4(inputOffset.x);
            inputOffset = input.OffsetOf(piBase + int3(kx, ky, 16));
            const uint4 inputDwords1 = input.storage.Load4(inputOffset.x);

            [unroll]
            for (uint f = 0; f < numFeatures; ++f)
            {
                uint4 weightsOffset = weights.OffsetOf(uint4(kx, ky, 0, f));
                const uint4 weightsDwords0 = weights.storage.Load4(weightsOffset.x);
                weightsOffset = weights.OffsetOf(uint4(kx, ky, 16, f));
                const uint4 weightsDwords1 = weights.storage.Load4(weightsOffset.x);

                const int partial0 = ApusrDp4aPartial4(
                    weightsDwords0,
                    inputDwords0.x,
                    inputDwords0.y,
                    inputDwords0.z,
                    inputDwords0.w);
                const int partial1 = ApusrDp4aPartial4(
                    weightsDwords1,
                    inputDwords1.x,
                    inputDwords1.y,
                    inputDwords1.z,
                    inputDwords1.w);
                accumulator[f] += partial0 + partial1;
            }
        }
    }

    uint4 storeDwords;
    [unroll]
    for (uint f = 0; f < numFeatures / 2 / 2; f += 4)
    {
        const int4 acc = int4(accumulator[f], accumulator[f + 1], accumulator[f + 2], accumulator[f + 3]);
        const int16_t4 result = int16_t4(round((acc * weights.quantizationScale * input.quantizationScale + Load4hA(bias, f)) * (1.0 / quantFactor0)));
        storeDwords[f / 4] = pack_clamp_s8(result);
    }
    uint storeIndex = output.OffsetOf(poBase);
    output.storage.Store4(storeIndex, storeDwords);

    [unroll]
    for (uint f = numFeatures / 2 / 2; f < numFeatures / 2; f += 4)
    {
        const int4 acc = int4(accumulator[f], accumulator[f + 1], accumulator[f + 2], accumulator[f + 3]);
        const int16_t4 result = int16_t4(round((acc * weights.quantizationScale * input.quantizationScale + Load4hA(bias, f)) * (1.0 / quantFactor0)));
        storeDwords[(f - numFeatures / 2 / 2) / 4] = pack_clamp_s8(result);
    }
    storeIndex = output.OffsetOf(poBase + int3(0, 0, 16));
    output.storage.Store4(storeIndex, storeDwords);

    [unroll]
    for (uint f = numFeatures / 2; f < numFeatures - 16; f += 4)
    {
        const int4 acc = int4(accumulator[f], accumulator[f + 1], accumulator[f + 2], accumulator[f + 3]);
        const int16_t4 result = int16_t4(round((acc * weights.quantizationScale * input.quantizationScale + Load4hA(bias, f)) * (1.0 / quantFactor1)));
        storeDwords[(f - numFeatures / 2) / 4] = pack_clamp_s8(result);
    }
    storeIndex = output.OffsetOf(poBase + int3(0, 0, 32));
    output.storage.Store4(storeIndex, storeDwords);

    [unroll]
    for (uint f = numFeatures - 16; f < numFeatures; f += 4)
    {
        const int4 acc = int4(accumulator[f], accumulator[f + 1], accumulator[f + 2], accumulator[f + 3]);
        const int16_t4 result = int16_t4(round((acc * weights.quantizationScale * input.quantizationScale + Load4hA(bias, f)) * (1.0 / quantFactor1)));
        storeDwords[(f - (numFeatures - 16)) / 4] = pack_clamp_s8(result);
    }
    storeIndex = output.OffsetOf(poBase + int3(0, 0, 48));
    output.storage.Store4(storeIndex, storeDwords);
}

template<typename SI, typename SW, typename SO, typename BIAS>
void FusedConv2D_k2s2b_QuantizedOutput(
    const float quantFactor0,
    const float quantFactor1,
    const QuantizedTensor3i8_NHWC<SI> input,
    const QuantizedTensor4i8_NHWC<SW> weights,
    const BIAS bias,
    const Tensor3i8_NHWC<SO> output,
    const ComputeShaderParams computeShaderParams)
{
    const uint3 workPerTick = uint3(1, 1, output.threadGroupSliceSize.z);
    const uint3 perThreadWork = SplitWork(output.threadGroupSliceSize, computeShaderParams.numThreads, workPerTick);
    const int3 workOffset = output.threadGroupSliceStart + computeShaderParams.groupThreadID * perThreadWork;
    const uint2 convStrides = uint2(2, 2);
    const uint numFeatures = weights.logicalSize.w;

    for (uint y = 0; y < perThreadWork.y; ++y)
    {
        for (uint x = 0; x < perThreadWork.x; ++x)
        {
            const int3 poBase = workOffset + int3(x, y, 0);
            if (!ValidPosition(output, poBase))
                continue;

            const int3 piBase = int3(poBase.xy * convStrides, 0);
            if (!IsAligned(numFeatures, 4))
                continue;

            if (numFeatures == 32 && input.logicalSize.z == 16)
                ApusrConv_16_32(quantFactor0, quantFactor1, input, weights, bias, output, piBase, poBase, numFeatures);
            else if (numFeatures == 64 && input.logicalSize.z == 32)
                ApusrConv_32_64(quantFactor0, quantFactor1, input, weights, bias, output, piBase, poBase, numFeatures);
        }
    }
}
