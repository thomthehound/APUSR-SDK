// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
// Auto-generated from models/fsr4_int8/reference/fsr4_4_0_2_int8/*/pre.hlsl.

#pragma once

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <cstddef>
#include <cstdint>

namespace apusr::models::fsr4_int8::embedded_pass0_weights {

struct Pass0WeightsBlob {
    Preset preset = Preset::Balanced;
    const char* presetName = nullptr;
    const std::uint32_t* dwords = nullptr;
    std::size_t dwordCount = 0;
    const char* sha256 = nullptr;

    [[nodiscard]] constexpr bool available() const noexcept { return dwords != nullptr && dwordCount == 256; }
};

[[nodiscard]] const Pass0WeightsBlob* find(Preset preset) noexcept;

} // namespace apusr::models::fsr4_int8::embedded_pass0_weights
