// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_runtime.h>

#include <apusr/adapters/apusr_fsr4_boundary.h>
#include <apusr/kernels/apusr_middle_core_resident_abi.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    apusr::frontends::Fsr4Dx12PackagedRuntimeConfig packaged{};
    apusr::Status status = apusr::frontends::loadFsr4Dx12PackagedRuntimeConfig(packaged);
    require(status.succeeded(), "packaged runtime config loading should not fail");
    if (packaged.available) {
        require(!packaged.xclbinData.empty(), "available packaged config should carry embedded xclbin bytes");
        require(!packaged.kernelName.empty(), "available packaged config should carry the fixed kernel name");
        require(packaged.xrtDeviceIndex == 0, "packaged runtime should use AMD/XRT device 0");
        if (packaged.modelEpochParameterDmaBytes != 0) {
            require(packaged.residentParameterArenas.size() == 18, "product middle-core package should embed the full 18-arena AM020 resident parameter set");
            require(packaged.skipSpillArenaBytes == apusr::kernels::middle_core::kStage1To12SkipSpillArenaBytes, "product middle-core package should expose the combined SkipA/SkipB arena size");
            require(packaged.kernelAbi.skipSpillBufferArgIndex == apusr::kernels::middle_core::kSkipSpillBufferArgIndex, "product middle-core package should bind the skip arena at arg6");
        }
    }

    apusr::frontends::Fsr4Dx12PackagedRuntimeConfig native{};
    native.available = true;
    native.xclbinData = std::vector<char>{'x', 'c', 'l', 'b', 'i', 'n'};
    native.kernelName = "apusr_identity";
    native.xrtDeviceIndex = 0;
    native.tokenRingDepth = 6;
    native.tokenBandRows = apusr::kDefaultTokenBandRows;


    native.residentParameterArenas.push_back(apusr::frontends::Fsr4Dx12ResidentParameterArenaConfig{
        .modelPermutation = "quality__2160",
        .parameterProfile = "am020_4x4_base",
        .data = std::vector<char>{'q'},
    });
    native.residentParameterArenas.push_back(apusr::frontends::Fsr4Dx12ResidentParameterArenaConfig{
        .modelPermutation = "performance__2160",
        .parameterProfile = "am020_4x4_base",
        .data = std::vector<char>{'p'},
    });
    native.modelEpochParameterDmaBytes = 1;
    native.skipSpillArenaBytes = 4096;
    native.kernelAbi.skipSpillBufferArgIndex = 6;

    const apusr::adapters::Fsr4RealBoundaryContract selectedBoundary = apusr::adapters::makeFsr4RealBoundaryContract(
        apusr::RenderSize{.width = 1920, .height = 1080},
        apusr::DisplaySize{.width = 3840, .height = 2160},
        apusr::DisplaySize{.width = 3840, .height = 2160},
        apusr::UpscalerFeatureFlags{},
        apusr::adapters::NpuTargetFamily::Npu2Aie2p);
    apusr::frontends::Fsr4Dx12PackagedRuntimeConfig selected{};
    status = apusr::frontends::selectFsr4Dx12PackagedRuntimeConfig(native, selectedBoundary, selected);
    require(status.succeeded(), "packaged config should select the matching resident arena");
    require(selected.residentParameterArenaData == std::vector<char>{'p'}, "selected resident arena should match the normalized model permutation");
    require(selected.residentParameterModelPermutation == "performance__2160", "selected resident arena should report its model permutation");
    require(selected.residentParameterProfile == "am020_4x4_base", "selected resident arena should report the AM020 product profile");

    void* queue = reinterpret_cast<void*>(0x1234);
    const apusr::frontends::Fsr4Dx12RuntimeConfigureDesc desc =
        apusr::frontends::makeFsr4Dx12RuntimeConfigureDesc(native, queue);
    require(desc.header.type == apusr::frontends::kConfigureDescTypeFsr4Dx12Runtime, "runtime desc type should be APUSR-specific");
    require(desc.commandQueue == queue, "runtime desc should carry the resolved queue");
    require(desc.xclbinPath == nullptr, "packaged native mode should not require a filesystem xclbin path");
    require(desc.xclbinData == native.xclbinData, "runtime desc should carry embedded xclbin bytes");
    require(std::string(desc.kernelName) == native.kernelName, "runtime desc should carry the fixed kernel name");
    require(desc.xrtDeviceIndex == 0, "runtime desc should target AMD/XRT device 0");
    require(desc.xrtOpcode == native.xrtOpcode, "runtime desc should carry the selected XRT opcode");
    require(desc.skipSpillArenaBytes == native.skipSpillArenaBytes, "runtime desc should carry the combined SkipA/SkipB arena size");
    require(desc.kernelAbi.skipSpillBufferArgIndex == native.kernelAbi.skipSpillBufferArgIndex, "runtime desc should carry the skip arena ABI arg");

    std::cout << "APUSR FSR4 drop-in config smoke: packaged native-runtime config validated\n";
    return 0;
}
