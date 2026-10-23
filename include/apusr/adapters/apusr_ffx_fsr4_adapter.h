// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_manifest.h>
#include <apusr/apusr_runtime.h>
#include <apusr/apusr_upscaler_common.h>

#include <vector>

namespace apusr::adapters {

struct Fsr4AdapterConfig {
    // The runtime arena is sized from the output capacity, not from the
    // low-resolution render input. FSR4 model alignment happens below.
    RenderSize maxRenderSize;
    DisplaySize maxActiveOutputSize;
    std::uint32_t tokenRingDepth = 6;
    std::uint32_t tokenBandRows = kDefaultTokenBandRows;
    VendorPolicy vendorPolicy = VendorPolicy::PerformanceFirst;
};

[[nodiscard]] std::vector<SegmentStageDesc> makeFsr4MiddleCoreStages();
[[nodiscard]] std::vector<SkipDelayDesc> makeFsr4MiddleCoreSkipDelays();
[[nodiscard]] std::uint32_t estimateFsr4MiddleCoreLookaheadRows();
[[nodiscard]] SegmentManifest makeFsr4MiddleCoreManifestForActiveOutput(
    DisplaySize activeOutputSize,
    std::uint32_t tokenBandRows = kDefaultTokenBandRows);
[[nodiscard]] RuntimeManifest makeFsr4RuntimeManifestForActiveOutput(
    DisplaySize activeOutputSize,
    std::uint32_t tokenBandRows = kDefaultTokenBandRows);
[[nodiscard]] RuntimeConfig makeFsr4RuntimeConfig(
    const Fsr4AdapterConfig& adapterConfig,
    GraphicsBackendPtr graphicsBackend,
    NpuBackendPtr npuBackend);

} // namespace apusr::adapters
