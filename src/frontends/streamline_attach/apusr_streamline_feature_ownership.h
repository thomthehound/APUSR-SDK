#pragma once

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstdint>

namespace apusr::streamline::feature_ownership {

inline constexpr std::uint32_t kDlss = 0u;
inline constexpr std::uint32_t kReflex = 3u;
inline constexpr std::uint32_t kPcl = 4u;
inline constexpr std::uint32_t kDlssG = 1000u;
inline constexpr std::uint32_t kDlssRr = 1001u;

// Stock Streamline remains the Vulkan DLSS-SR semantic owner, but APUSR owns
// the cross-vendor Reflex and DLSS-G implementations. Those NVIDIA plugins
// must not be initialized beside APUSR on the same Vulkan presentation path.
// DLSS-RR is not implemented and likewise must not enter stock's NVIDIA-only
// startup graph. PCL is retained as an optional backing marker/statistics path.
[[nodiscard]] constexpr bool excludeFromStockVulkanInitialization(
    const std::uint32_t feature) noexcept
{
    return feature == kReflex || feature == kDlssG || feature == kDlssRr;
}

static_assert(!excludeFromStockVulkanInitialization(kDlss));
static_assert(!excludeFromStockVulkanInitialization(kPcl));
static_assert(excludeFromStockVulkanInitialization(kReflex));
static_assert(excludeFromStockVulkanInitialization(kDlssG));
static_assert(excludeFromStockVulkanInitialization(kDlssRr));

} // namespace apusr::streamline::feature_ownership
