// Copyright(C) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.
#pragma once

#if defined(APUSR_VULKAN_VECTOR_LOADS) && APUSR_VULKAN_VECTOR_LOADS
// DXC lowers ByteAddressBuffer.Load4 to four scalar OpAccessChain/OpLoad pairs
// in SPIR-V.  These aliases let call sites with a proven 16-byte alignment use
// one vector storage-buffer load instead.  Vulkan storage-buffer descriptors do
// not encode the shader-side element type, so the runtime binds the same buffers
// at these alias slots.  Ordinary Load4 retains the full 4-byte-aligned HLSL
// contract; only Load4Aligned may use the vector view.
StructuredBuffer<uint4> ApusrMl2cInitializerVector : register(t31);
RWStructuredBuffer<uint4> ApusrMl2cScratchVector : register(u31);
#endif

struct BufferStorage
{
    ByteAddressBuffer buffer;

    uint Load1(uint byteOffset) { return buffer.Load(byteOffset); }
    uint2 Load2(uint byteOffset) { return buffer.Load2(byteOffset); }
    uint3 Load3(uint byteOffset) { return buffer.Load3(byteOffset); }
    uint4 Load4(uint byteOffset) { return buffer.Load4(byteOffset); }
    uint4 Load4Aligned(uint byteOffset) {
#if defined(APUSR_VULKAN_VECTOR_LOADS) && APUSR_VULKAN_VECTOR_LOADS
        return ApusrMl2cInitializerVector[byteOffset >> 4];
#else
        return buffer.Load4(byteOffset);
#endif
    }
};

template<uint N>
struct ConstantBufferStorage
{
    uint dwords[N];

    uint Load1(uint byteOffset) {
        const uint dwordOffset = byteOffset >> 2;
        return dwords[dwordOffset];
    }
    uint2 Load2(uint byteOffset) {
        const uint dwordOffset = byteOffset >> 2;
        return uint2(dwords[dwordOffset], dwords[dwordOffset+1]);
    }
    uint3 Load3(uint byteOffset) {
        const uint dwordOffset = byteOffset >> 2;
        return uint3(dwords[dwordOffset], dwords[dwordOffset+1], dwords[dwordOffset+2]);
    }
    uint4 Load4(uint byteOffset) {
        const uint dwordOffset = byteOffset >> 2;
        return uint4(dwords[dwordOffset], dwords[dwordOffset+1], dwords[dwordOffset+2], dwords[dwordOffset+3]);
    }
    uint4 Load4Aligned(uint byteOffset) { return Load4(byteOffset); }
};

struct RWBufferStorage
{
    RWByteAddressBuffer buffer;

    void Store1(uint byteOffset, uint v) { buffer.Store(byteOffset, v); }
    void Store2(uint byteOffset, uint2 v) { buffer.Store2(byteOffset, v); }
    void Store3(uint byteOffset, uint3 v) { buffer.Store3(byteOffset, v); }
    void Store4(uint byteOffset, uint4 v) { buffer.Store4(byteOffset, v); }
    void Store4Aligned(uint byteOffset, uint4 v) {
#if defined(APUSR_VULKAN_VECTOR_LOADS) && APUSR_VULKAN_VECTOR_LOADS
        ApusrMl2cScratchVector[byteOffset >> 4] = v;
#else
        buffer.Store4(byteOffset, v);
#endif
    }

    uint Load1(uint byteOffset) { return buffer.Load(byteOffset); }
    uint2 Load2(uint byteOffset) { return buffer.Load2(byteOffset); }
    uint3 Load3(uint byteOffset) { return buffer.Load3(byteOffset); }
    uint4 Load4(uint byteOffset) { return buffer.Load4(byteOffset); }
    uint4 Load4Aligned(uint byteOffset) {
#if defined(APUSR_VULKAN_VECTOR_LOADS) && APUSR_VULKAN_VECTOR_LOADS
        return ApusrMl2cScratchVector[byteOffset >> 4];
#else
        return buffer.Load4(byteOffset);
#endif
    }
};

#ifndef ML2C_GROUPSHARED_SIZE
#define ML2C_GROUPSHARED_SIZE 32*1024
#endif

groupshared uint ml2c_ldsScratch[ML2C_GROUPSHARED_SIZE/4];

struct LDSStorage
{
    uint ldsDWordOffset;

    uint Load1(uint byteOffset) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        return ml2c_ldsScratch[dwordOffset];
    }
    uint2 Load2(uint byteOffset) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        return uint2(ml2c_ldsScratch[dwordOffset], ml2c_ldsScratch[dwordOffset + 1]);
    }
    uint3 Load3(uint byteOffset) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        return uint3(ml2c_ldsScratch[dwordOffset], ml2c_ldsScratch[dwordOffset + 1], ml2c_ldsScratch[dwordOffset + 2]);
    }
    uint4 Load4(uint byteOffset) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        return uint4(ml2c_ldsScratch[dwordOffset], ml2c_ldsScratch[dwordOffset + 1], ml2c_ldsScratch[dwordOffset + 2], ml2c_ldsScratch[dwordOffset + 3]);
    }
    uint4 Load4Aligned(uint byteOffset) { return Load4(byteOffset); }

    void Store1(uint byteOffset, uint v) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        ml2c_ldsScratch[dwordOffset] = v;
    }
    void Store2(uint byteOffset, uint2 v) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        ml2c_ldsScratch[dwordOffset] = v.x;
        ml2c_ldsScratch[dwordOffset+1] = v.y;
    }
    void Store3(uint byteOffset, uint3 v) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        ml2c_ldsScratch[dwordOffset] = v.x;
        ml2c_ldsScratch[dwordOffset+1] = v.y;
        ml2c_ldsScratch[dwordOffset+2] = v.z;
    }
    void Store4(uint byteOffset, uint4 v) {
        const uint dwordOffset = ldsDWordOffset + (byteOffset >> 2);
        ml2c_ldsScratch[dwordOffset] = v.x;
        ml2c_ldsScratch[dwordOffset + 1] = v.y;
        ml2c_ldsScratch[dwordOffset + 2] = v.z;
        ml2c_ldsScratch[dwordOffset + 3] = v.w;
    }
};
