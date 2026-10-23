// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>


using ApusrFramegenResult = std::int32_t;
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_OK = 0;
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INVALID_POINTER = static_cast<std::int32_t>(0x80000000u);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INVALID_ALIGNMENT = static_cast<std::int32_t>(0x80000001u);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INVALID_SIZE = static_cast<std::int32_t>(0x80000002u);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INVALID_PATH = static_cast<std::int32_t>(0x80000004u);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY = static_cast<std::int32_t>(0x80000007u);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE = static_cast<std::int32_t>(0x80000008u);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT = static_cast<std::int32_t>(0x8000000au);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE = static_cast<std::int32_t>(0x8000000bu);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_NULL_DEVICE = static_cast<std::int32_t>(0x8000000cu);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_BACKEND_API = static_cast<std::int32_t>(0x8000000du);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INSUFFICIENT_MEMORY = static_cast<std::int32_t>(0x8000000eu);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_INVALID_VERSION = static_cast<std::int32_t>(0x8000000fu);
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_ACCESS_DENIED = static_cast<std::int32_t>(0x80000010u);
// Internal real-time scheduling result: optional work is still in flight.
inline constexpr ApusrFramegenResult APUSR_FRAMEGEN_ERROR_BUSY = static_cast<std::int32_t>(0x80000011u);

using ApusrFramegenDevice = void*;
using ApusrFramegenCommandList = void*;
using ApusrFramegenVersion = std::uint32_t;

struct ApusrFramegenDimensions2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ApusrFramegenFloatCoords2D {
    float x = 0.0f;
    float y = 0.0f;
};

struct ApusrFramegenRect2D {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

enum ApusrFramegenBackbufferTransferFunction : std::uint32_t {
    APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB = 0,
    APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_PQ,
    APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SCRGB,
};

struct ApusrFramegenMemoryUsage {
    std::uint64_t totalUsageInBytes = 0;
    std::uint64_t aliasableUsageInBytes = 0;
};

enum ApusrFramegenShaderModel : std::uint32_t {
    APUSR_FRAMEGEN_SHADER_MODEL_5_1 = 0,
    APUSR_FRAMEGEN_SHADER_MODEL_6_0,
    APUSR_FRAMEGEN_SHADER_MODEL_6_1,
    APUSR_FRAMEGEN_SHADER_MODEL_6_2,
    APUSR_FRAMEGEN_SHADER_MODEL_6_3,
    APUSR_FRAMEGEN_SHADER_MODEL_6_4,
    APUSR_FRAMEGEN_SHADER_MODEL_6_5,
    APUSR_FRAMEGEN_SHADER_MODEL_6_6,
    APUSR_FRAMEGEN_SHADER_MODEL_6_7,
};

struct ApusrFramegenDeviceCapabilities {
    ApusrFramegenShaderModel maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_5_1;
    std::uint32_t waveLaneCountMin = 0;
    std::uint32_t waveLaneCountMax = 0;
    bool fp16Supported = false;
};
