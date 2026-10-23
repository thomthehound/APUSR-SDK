// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>

namespace apusr::frontends {

struct EmbeddedFsr4Dx12ResidentParameterArena {
    const char* modelPermutation = nullptr;
    const char* parameterProfile = nullptr;
    const std::uint8_t* data = nullptr;
    std::size_t byteSize = 0;

    [[nodiscard]] constexpr bool available() const
    {
        return modelPermutation != nullptr && modelPermutation[0] != '\0' &&
            parameterProfile != nullptr && parameterProfile[0] != '\0' &&
            data != nullptr && byteSize != 0;
    }
};

struct EmbeddedFsr4Dx12Xclbin {
    const std::uint8_t* data = nullptr;
    std::size_t byteSize = 0;
    const std::uint8_t* instructionData = nullptr;
    std::size_t instructionByteSize = 0;
    const std::uint8_t* parameterLoadInstructionData = nullptr;
    std::size_t parameterLoadInstructionByteSize = 0;
    const std::uint8_t* residentParameterArenaData = nullptr;
    std::size_t residentParameterArenaByteSize = 0;
    const EmbeddedFsr4Dx12ResidentParameterArena* residentParameterArenas = nullptr;
    std::size_t residentParameterArenaCount = 0;
    const char* kernelName = nullptr;

    [[nodiscard]] constexpr bool available() const
    {
        return data != nullptr && byteSize != 0 && kernelName != nullptr && kernelName[0] != '\0';
    }

    [[nodiscard]] constexpr bool hasInstructions() const
    {
        return instructionData != nullptr && instructionByteSize != 0;
    }

    [[nodiscard]] constexpr bool hasParameterLoadInstructions() const
    {
        return parameterLoadInstructionData != nullptr && parameterLoadInstructionByteSize != 0;
    }

    [[nodiscard]] constexpr bool hasResidentParameterArena() const
    {
        return (residentParameterArenaData != nullptr && residentParameterArenaByteSize != 0) ||
            (residentParameterArenas != nullptr && residentParameterArenaCount != 0);
    }

    [[nodiscard]] constexpr bool hasResidentParameterArenaSet() const
    {
        return residentParameterArenas != nullptr && residentParameterArenaCount != 0;
    }
};

[[nodiscard]] EmbeddedFsr4Dx12Xclbin embeddedFsr4Dx12Xclbin();

} // namespace apusr::frontends
