// Copyright(C) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.

template<typename T>
T Relu(const T v)
{
    return max(v, 0);
}

template<typename T>
T Sigmoid(const T v)
{
    return 1 / (1 + exp(-v));
}

int4 ReluClampS8(const int4 v)
{
    return clamp(v, 0, 127);
}

#if !defined(FFX_FSR4_EXPERIMENT_UNSIGNED_RELU)
#define FFX_FSR4_EXPERIMENT_UNSIGNED_RELU 0
#endif

int8_t4_packed PackUnsignedReluActivationU8(const int4 v)
{
    return (int8_t4_packed)pack_clamp_u8(v);
}

int8_t4_packed PackUnsignedReluActivationU8(const int16_t4 v)
{
    return PackUnsignedReluActivationU8(int4(v));
}

template<typename T>
T LeakyRelu(const float alpha, const T v)
{
    return v >= 0 ? v : alpha * v;
}

template<typename T>
T SqrSwish(const T v)
{
    return 0.5 * v * (1. + v / sqrt(v * v + 1.));
}