// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/adapters/apusr_fsr4_boundary.h>
#include <apusr/backends/apusr_npu_xrt.h>
#include <apusr/apusr_common.h>
#include <apusr/apusr_runtime.h>
#include <apusr/apusr_tokens.h>

#include <cstdint>
#include <string>
#include <vector>

#include <apusr/compat/apusr_ffx_api_abi.h>

namespace apusr::frontends {

inline constexpr ffxStructType_t kConfigureDescTypeFsr4Dx12Runtime = 0x4150555346535234ull; // "APUSFSR4"

struct Fsr4Dx12ResidentParameterArenaConfig {
    std::string modelPermutation;
    std::string parameterProfile;
    std::vector<char> data;
};

struct Fsr4Dx12RuntimeConfigureDesc {
    ffxConfigureDescHeader header;
    void* commandQueue = nullptr;
    const char* xclbinPath = nullptr;
    std::vector<char> xclbinData;
    std::vector<char> instructionData;
    std::vector<char> parameterLoadInstructionData;
    std::vector<char> residentParameterArenaData;
    std::uint64_t modelEpochParameterDmaBytes = 0;
    std::uint64_t hotPathParameterDmaBytesPerRun = 0;
    std::uint64_t skipSpillArenaBytes = 0;
    const char* kernelName = nullptr;
    std::uint32_t xrtDeviceIndex = 0;
    std::uint32_t tokenRingDepth = 6;
    std::uint32_t tokenBandRows = kDefaultTokenBandRows;
    std::uint32_t flags = 0;
    backends::AmdXrtKernelAbi kernelAbi;
    std::uint32_t xrtOpcode = 3;
};

struct Fsr4Dx12PackagedRuntimeConfig {
    bool available = false;
    std::vector<char> xclbinData;
    std::vector<char> instructionData;
    std::vector<char> parameterLoadInstructionData;
    std::vector<char> residentParameterArenaData;
    std::vector<Fsr4Dx12ResidentParameterArenaConfig> residentParameterArenas;
    std::string residentParameterModelPermutation;
    std::string residentParameterProfile;
    std::uint64_t modelEpochParameterDmaBytes = 0;
    std::uint64_t hotPathParameterDmaBytesPerRun = 0;
    std::uint64_t skipSpillArenaBytes = 0;
    std::string kernelName;
    std::uint32_t xrtDeviceIndex = 0;
    std::uint32_t tokenRingDepth = 6;
    std::uint32_t tokenBandRows = kDefaultTokenBandRows;
    std::uint32_t flags = 0;
    backends::AmdXrtKernelAbi kernelAbi;
    std::uint32_t xrtOpcode = 3;
};

struct FfxFsr4DispatchNormalization {
    FrameDesc frame;
    void* commandList = nullptr;
    bool hasOptionalDepth = false;
    bool hasOptionalExposure = false;
    bool hasReactiveMask = false;
    bool hasTransparencyMask = false;
    bool sharpeningRequested = false;
    float sharpness = 0.0f;
    float frameTimeDeltaMs = 0.0f;
    float preExposure = 1.0f;
    float viewSpaceToMetersFactor = 1.0f;
    std::uint32_t dispatchFlags = 0;
    adapters::Fsr4RealBoundaryContract realBoundary;
};

[[nodiscard]] Fsr4Dx12RuntimeConfigureDesc makeFsr4Dx12RuntimeConfigureDesc(
    const Fsr4Dx12PackagedRuntimeConfig& config,
    void* commandQueue);
[[nodiscard]] Status loadFsr4Dx12PackagedRuntimeConfig(Fsr4Dx12PackagedRuntimeConfig& outConfig);
[[nodiscard]] Status selectFsr4Dx12PackagedRuntimeConfig(
    const Fsr4Dx12PackagedRuntimeConfig& baseConfig,
    const adapters::Fsr4RealBoundaryContract& boundary,
    Fsr4Dx12PackagedRuntimeConfig& outConfig);

[[nodiscard]] UpscalerFeatureFlags decodeFfxUpscaleFeatureFlags(std::uint32_t flags);
[[nodiscard]] SurfaceFormat mapFfxSurfaceFormat(std::uint32_t format);
[[nodiscard]] SurfaceDesc normalizeFfxResource(const FfxApiResource& resource, SurfaceRole role);
[[nodiscard]] Status normalizeFfxFsr4Dispatch(
    const ffxDispatchDescUpscale& dispatch,
    std::uint64_t frameIndex,
    std::uint32_t createFlags,
    FfxFsr4DispatchNormalization& outNormalization,
    DisplaySize maxActiveOutputSize = {});


} // namespace apusr::frontends
