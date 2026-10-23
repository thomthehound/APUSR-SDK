// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace apusr::backends::vk {

struct SpirvComputeMetadata {
    std::uint32_t localSizeX = 0;
    std::uint32_t localSizeY = 0;
    std::uint32_t localSizeZ = 0;
};

// Extract the literal LocalSize execution mode from a SPIR-V module without
// reflection libraries. This is a pipeline-creation-time profiler helper, not
// a shader validator. LocalSizeId remains intentionally unavailable rather
// than resolving a partial constant-expression language here.
[[nodiscard]] inline bool readSpirvComputeMetadata(
    const void* data,
    const std::size_t byteSize,
    SpirvComputeMetadata& out) noexcept
{
    out = {};
    constexpr std::uint32_t kSpirvMagic = 0x07230203u;
    constexpr std::uint16_t kOpExecutionMode = 16u;
    constexpr std::uint32_t kExecutionModeLocalSize = 17u;
    if (data == nullptr || byteSize < 5u * sizeof(std::uint32_t) || (byteSize & 3u) != 0u) {
        return false;
    }

    auto word = [data](const std::size_t index) noexcept {
        std::uint32_t value = 0;
        std::memcpy(&value, static_cast<const std::byte*>(data) + index * sizeof(value), sizeof(value));
        return value;
    };
    if (word(0u) != kSpirvMagic) {
        return false;
    }

    const std::size_t wordCount = byteSize / sizeof(std::uint32_t);
    for (std::size_t cursor = 5u; cursor < wordCount;) {
        const std::uint32_t instruction = word(cursor);
        const std::uint16_t opcode = static_cast<std::uint16_t>(instruction & 0xffffu);
        const std::uint16_t count = static_cast<std::uint16_t>(instruction >> 16u);
        if (count == 0u || cursor + count > wordCount) {
            return false;
        }
        // OpExecutionMode <entry-point> LocalSize X Y Z
        if (opcode == kOpExecutionMode && count >= 6u && word(cursor + 2u) == kExecutionModeLocalSize) {
            out.localSizeX = word(cursor + 3u);
            out.localSizeY = word(cursor + 4u);
            out.localSizeZ = word(cursor + 5u);
            return out.localSizeX != 0u && out.localSizeY != 0u && out.localSizeZ != 0u;
        }
        cursor += count;
    }
    return false;
}

} // namespace apusr::backends::vk
