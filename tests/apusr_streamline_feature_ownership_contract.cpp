// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_streamline_feature_ownership.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
    namespace ownership = apusr::streamline::feature_ownership;
    constexpr std::uint32_t unrelatedFeature = 42u;
    constexpr std::array requested{
        ownership::kDlss,
        ownership::kReflex,
        ownership::kPcl,
        ownership::kDlssG,
        ownership::kDlssRr,
        unrelatedFeature,
    };

    std::vector<std::uint32_t> stockFeatures;
    for (const std::uint32_t feature : requested) {
        if (!ownership::excludeFromStockVulkanInitialization(feature)) {
            stockFeatures.push_back(feature);
        }
    }

    const std::vector<std::uint32_t> expected{
        ownership::kDlss,
        ownership::kPcl,
        unrelatedFeature,
    };
    assert(stockFeatures == expected);

    std::cout << "APUSR Streamline feature ownership contract: stock Vulkan retains "
                 "DLSS-SR/PCL/passthrough while APUSR owns Reflex/DLSS-G\n";
    return 0;
}
