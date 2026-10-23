// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/adapters/apusr_fsr4_boundary.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    require(apusr::adapters::fsr4UpscaleRatioForQualityMode(0) == 1.0f, "NativeAA ratio changed");
    require(apusr::adapters::fsr4UpscaleRatioForQualityMode(1) == 1.5f, "Quality ratio changed");
    require(apusr::adapters::fsr4UpscaleRatioForQualityMode(2) == 1.7f, "Balanced ratio changed");
    require(apusr::adapters::fsr4UpscaleRatioForQualityMode(3) == 2.0f, "Performance ratio changed");
    require(apusr::adapters::fsr4UpscaleRatioForQualityMode(4) == 3.0f, "UltraPerformance ratio changed");
    require(apusr::adapters::fsr4RenderExtentFromDisplayExtent(3440, 1.7f) == 2023, "Balanced render-width query changed");

    apusr::UpscalerFeatureFlags features{};
    const apusr::RenderSize qualityRender{
        apusr::adapters::fsr4RenderExtentFromDisplayExtent(2560, 1.5f),
        apusr::adapters::fsr4RenderExtentFromDisplayExtent(1440, 1.5f),
    };
    require(qualityRender.width == 1706 && qualityRender.height == 960, "Quality render-resolution query changed");
    require(
        apusr::adapters::selectFsr4ModelPreset(qualityRender, apusr::DisplaySize{2560, 1440}, features) ==
            apusr::adapters::Fsr4ModelPreset::Quality,
        "integer-rounded 1.5x render extents must remain on the Quality model");

    require(
        apusr::adapters::selectFsr4ModelPreset(apusr::RenderSize{1707, 960}, apusr::DisplaySize{2560, 1440}, features) ==
            apusr::adapters::Fsr4ModelPreset::Quality,
        "a one-pixel Quality extent difference must remain on the Quality model");
    require(
        apusr::adapters::selectFsr4ModelPreset(apusr::RenderSize{1708, 960}, apusr::DisplaySize{2560, 1440}, features) ==
            apusr::adapters::Fsr4ModelPreset::NativeAA,
        "Quality inference must not broaden beyond the one-pixel integer ambiguity");

    const apusr::RenderSize balancedRender{2024, 848};
    const apusr::DisplaySize ultrawideDisplay{3440, 1440};
    require(
        apusr::adapters::selectFsr4ModelPreset(balancedRender, ultrawideDisplay, features) ==
            apusr::adapters::Fsr4ModelPreset::Balanced,
        "3440x1440 balanced dispatch should select the balanced preset");

    features.dynamicResolution = true;
    require(
        apusr::adapters::selectFsr4ModelPreset(balancedRender, ultrawideDisplay, features) ==
            apusr::adapters::Fsr4ModelPreset::DynamicResolution,
        "dynamic-resolution dispatches must stay on the DRS model preset");
    features.dynamicResolution = false;

    const apusr::DisplaySize oddAlignedOutput = apusr::adapters::alignFsr4ModelOutputSize(apusr::DisplaySize{1921, 1079});
    require(oddAlignedOutput.width == 1928 && oddAlignedOutput.height == 1080, "FSR4 model output extent must align to 8 pixels");

    require(
        apusr::adapters::selectFsr4ModelResolution(apusr::DisplaySize{1920, 1080}) ==
            apusr::adapters::Fsr4ModelResolution::R1080,
        "1080 max-upscale size should select the 1080 model family");
    require(
        apusr::adapters::selectFsr4ModelResolution(apusr::DisplaySize{3440, 1440}) ==
            apusr::adapters::Fsr4ModelResolution::R2160,
        "3440x1440 should select the 2160 model family");
    require(
        apusr::adapters::selectFsr4ModelResolution(apusr::DisplaySize{5120, 2880}) ==
            apusr::adapters::Fsr4ModelResolution::R4320,
        "5K should select the 4320 model family");

    const apusr::adapters::Fsr4RealBoundaryContract contract = apusr::adapters::makeFsr4RealBoundaryContract(
        balancedRender,
        ultrawideDisplay,
        ultrawideDisplay,
        features,
        apusr::adapters::NpuTargetFamily::Npu1Aie2,
        0.0124752065f,
        0.0331711769f);
    require(apusr::adapters::validateFsr4RealBoundaryContract(contract).succeeded(), "real boundary contract should validate");
    require(contract.npuTarget == apusr::adapters::NpuTargetFamily::Npu1Aie2, "contract must preserve the selected NPU target family");
    require(contract.logicalWidth == 1720 && contract.logicalHeight == 720, "real boundary should use half-resolution C16 shape");
    require(contract.paddedWidth == 1728, "real boundary should publish the padded C16 width");
    require(contract.rowPitchBytes == 27648, "real boundary row pitch changed");
    require(contract.ingress.shape.channels == 16 && contract.egress.shape.channels == 16, "real boundary must stay C16");
    require(contract.ingress.shape.layout == apusr::TokenLayout::RasterNhwcC16, "real boundary ingress must stay row-major C16");
    require(contract.egress.shape.layout == apusr::TokenLayout::CellM4x8C16, "real boundary egress must be cell-major m4x8 C16");
    require(contract.ingress.producerTensorId == "passes:slice_0", "ingress producer tensor identity changed");
    require(contract.egress.consumerTensorId == "passes:fused_quantized_NHWC__decoder2_ResidualBlock_2_body_input_quantization_act_quant_export_handler_QuantizeLinear_output_0", "egress consumer tensor identity changed");
    require(std::string(apusr::adapters::fsr4ModelPresetBundleName(contract.preset)) == "balanced", "balanced render/display ratio should select the balanced bundle");
    require(std::string(apusr::adapters::npuTargetFamilyName(contract.npuTarget)) == "npu1", "NPU target name changed");

    const apusr::adapters::Fsr4RealBoundaryContract oddContract = apusr::adapters::makeFsr4RealBoundaryContract(
        apusr::RenderSize{1280, 720},
        apusr::DisplaySize{1921, 1079},
        apusr::DisplaySize{1921, 1079},
        features,
        apusr::adapters::NpuTargetFamily::Npu2Aie2p);
    require(oddContract.activeOutputSize.width == 1921 && oddContract.activeOutputSize.height == 1079, "boundary contract should preserve the active output extent");
    require(oddContract.logicalWidth == 964 && oddContract.logicalHeight == 540, "boundary contract should derive C16 shape from the model output extent");
    require(oddContract.paddedWidth == 976, "odd aligned C16 width should still pad to whole 256-byte tokens");
    require(oddContract.rowPitchBytes == 15616, "odd aligned C16 row pitch changed");

    std::cout << "APUSR FSR4 real boundary smoke: preset selection, model resolution, tensor identity, and C16 row contract validated\n";
    return 0;
}
