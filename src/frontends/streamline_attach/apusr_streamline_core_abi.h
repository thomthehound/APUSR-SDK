// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace apusr::streamline::core_abi {

struct StructType {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

struct BaseStructure {
    BaseStructure* next;
    StructType structType;
    std::size_t structVersion;
};

// Streamline deliberately makes FrameToken expose its frame index through a
// virtual conversion operator. The token object can originate in sl.common or
// another Streamline module; its identity therefore must not depend on seeing
// the corresponding slGetNewFrameToken call in this proxy.
struct FrameToken : BaseStructure {
    virtual operator std::uint32_t() const = 0;
};

constexpr StructType kFrameTokenStructType{
    0x830a0f35u, 0xdb84u, 0x4171u, {0xa8u, 0x04u, 0x59u, 0xb2u, 0x06u, 0x49u, 0x9bu, 0x18u}};

[[nodiscard]] inline bool structTypeEquals(const StructType& lhs, const StructType& rhs) noexcept
{
    return std::memcmp(&lhs, &rhs, sizeof(StructType)) == 0;
}

[[nodiscard]] inline bool frameIndex(const void* token, std::uint64_t& index) noexcept
{
    if (token == nullptr)
        return false;

    const auto* frame = static_cast<const FrameToken*>(token);
    if (frame->structVersion < 1u || !structTypeEquals(frame->structType, kFrameTokenStructType))
        return false;

    index = static_cast<std::uint32_t>(*frame);
    return true;
}

static_assert(sizeof(StructType) == 16, "Streamline StructType ABI drift");
static_assert(sizeof(BaseStructure) == 32, "Streamline BaseStructure ABI drift");
static_assert(sizeof(FrameToken) == 40, "Streamline x64 FrameToken ABI drift");

} // namespace apusr::streamline::core_abi
