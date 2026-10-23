// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>

#include <cstdint>
#include <string>
#include <vector>

namespace apusr {

enum class UpscalerProtocol : std::uint32_t {
    NativeApusr,
    FidelityFxFsr4,
    Dlss,
    Xess,
};

enum class ProtocolAbi : std::uint32_t {
    NativeApusr,
    FidelityFxProvider,
    NvngxDlss,
    IntelXess,
};

enum class ProtocolIntegrationMode : std::uint32_t {
    Native,
    CompatibilityMasquerade,
    TranslationAdapter,
};

enum class ProtocolSurfaceSemantic : std::uint32_t {
    Color,
    Depth,
    MotionVectors,
    Exposure,
    ReactiveMask,
    TransparencyMask,
    Output,
};

struct ProtocolSurfaceRequirement {
    ProtocolSurfaceSemantic semantic = ProtocolSurfaceSemantic::Color;
    bool required = true;
    bool feedsCoreFrame = true;
    std::string externalName;
};

struct ProtocolIngressContract {
    UpscalerProtocol protocol = UpscalerProtocol::NativeApusr;
    ProtocolAbi abi = ProtocolAbi::NativeApusr;
    ProtocolIntegrationMode integrationMode = ProtocolIntegrationMode::Native;
    GraphicsApi graphicsApi = GraphicsApi::Dx12;
    std::vector<ProtocolSurfaceRequirement> surfaces;
    bool ownsUpscalerSchedule = true;
    bool hotPathMayCallThirdPartyRuntime = false;
    bool protocolSpecificDataMustBeNormalizedAtIngress = true;
    std::string notes;
};

[[nodiscard]] ProtocolIngressContract makeNativeApusrDx12IngressContract();
[[nodiscard]] ProtocolIngressContract makeFidelityFxFsr4Dx12IngressContract();
[[nodiscard]] ProtocolIngressContract makeDlssDx12TranslationIngressContract();
[[nodiscard]] ProtocolIngressContract makeXessDx12TranslationIngressContract();
[[nodiscard]] Status validateProtocolIngressContract(const ProtocolIngressContract& contract);
[[nodiscard]] bool protocolIngressFeedsCoreFrame(const ProtocolIngressContract& contract);
[[nodiscard]] const char* upscalerProtocolName(UpscalerProtocol protocol);
[[nodiscard]] const char* protocolAbiName(ProtocolAbi abi);
[[nodiscard]] const char* protocolIntegrationModeName(ProtocolIntegrationMode mode);
[[nodiscard]] const char* protocolSurfaceSemanticName(ProtocolSurfaceSemantic semantic);

} // namespace apusr
