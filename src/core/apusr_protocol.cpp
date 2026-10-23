// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_protocol.h>

#include <string>

namespace apusr {

namespace {

[[nodiscard]] ProtocolSurfaceRequirement requiredSurface(
    const ProtocolSurfaceSemantic semantic,
    std::string externalName,
    const bool feedsCoreFrame = true)
{
    return ProtocolSurfaceRequirement{
        .semantic = semantic,
        .required = true,
        .feedsCoreFrame = feedsCoreFrame,
        .externalName = std::move(externalName),
    };
}

[[nodiscard]] ProtocolSurfaceRequirement optionalSurface(
    const ProtocolSurfaceSemantic semantic,
    std::string externalName,
    const bool feedsCoreFrame = false)
{
    return ProtocolSurfaceRequirement{
        .semantic = semantic,
        .required = false,
        .feedsCoreFrame = feedsCoreFrame,
        .externalName = std::move(externalName),
    };
}

} // namespace

ProtocolIngressContract makeNativeApusrDx12IngressContract()
{
    return ProtocolIngressContract{
        .protocol = UpscalerProtocol::NativeApusr,
        .abi = ProtocolAbi::NativeApusr,
        .integrationMode = ProtocolIntegrationMode::Native,
        .graphicsApi = GraphicsApi::Dx12,
        .surfaces = {
            requiredSurface(ProtocolSurfaceSemantic::Color, "color"),
            requiredSurface(ProtocolSurfaceSemantic::MotionVectors, "motion_vectors"),
            optionalSurface(ProtocolSurfaceSemantic::Depth, "depth", true),
            optionalSurface(ProtocolSurfaceSemantic::Exposure, "exposure", true),
            requiredSurface(ProtocolSurfaceSemantic::Output, "output"),
        },
        .ownsUpscalerSchedule = true,
        .hotPathMayCallThirdPartyRuntime = false,
        .protocolSpecificDataMustBeNormalizedAtIngress = true,
        .notes = "Native callers provide APUSR frame inputs directly; no external upscaler ABI defines the core runtime.",
    };
}

ProtocolIngressContract makeFidelityFxFsr4Dx12IngressContract()
{
    return ProtocolIngressContract{
        .protocol = UpscalerProtocol::FidelityFxFsr4,
        .abi = ProtocolAbi::FidelityFxProvider,
        .integrationMode = ProtocolIntegrationMode::CompatibilityMasquerade,
        .graphicsApi = GraphicsApi::Dx12,
        .surfaces = {
            requiredSurface(ProtocolSurfaceSemantic::Color, "ffxDispatchDescUpscale::color"),
            requiredSurface(ProtocolSurfaceSemantic::MotionVectors, "ffxDispatchDescUpscale::motionVectors"),
            optionalSurface(ProtocolSurfaceSemantic::Depth, "ffxDispatchDescUpscale::depth", true),
            optionalSurface(ProtocolSurfaceSemantic::Exposure, "ffxDispatchDescUpscale::exposure", true),
            optionalSurface(ProtocolSurfaceSemantic::ReactiveMask, "ffxDispatchDescUpscale::reactive", false),
            optionalSurface(ProtocolSurfaceSemantic::TransparencyMask, "ffxDispatchDescUpscale::transparencyAndComposition", false),
            requiredSurface(ProtocolSurfaceSemantic::Output, "ffxDispatchDescUpscale::output"),
        },
        .ownsUpscalerSchedule = true,
        .hotPathMayCallThirdPartyRuntime = false,
        .protocolSpecificDataMustBeNormalizedAtIngress = true,
        .notes = "FSR4 is the first drop-in ABI. The frontend consumes the ABI, normalizes inputs, and then leaves the hot path to APUSR.",
    };
}

ProtocolIngressContract makeDlssDx12TranslationIngressContract()
{
    return ProtocolIngressContract{
        .protocol = UpscalerProtocol::Dlss,
        .abi = ProtocolAbi::NvngxDlss,
        .integrationMode = ProtocolIntegrationMode::CompatibilityMasquerade,
        .graphicsApi = GraphicsApi::Dx12,
        .surfaces = {
            requiredSurface(ProtocolSurfaceSemantic::Color, "DLSS input color"),
            requiredSurface(ProtocolSurfaceSemantic::MotionVectors, "DLSS motion vectors"),
            optionalSurface(ProtocolSurfaceSemantic::Depth, "DLSS depth", true),
            optionalSurface(ProtocolSurfaceSemantic::Exposure, "DLSS exposure", true),
            requiredSurface(ProtocolSurfaceSemantic::Output, "DLSS output"),
        },
        .ownsUpscalerSchedule = true,
        .hotPathMayCallThirdPartyRuntime = false,
        .protocolSpecificDataMustBeNormalizedAtIngress = true,
        .notes = "Future DLSS-style ingress should translate descriptors into APUSR frame data, not run a DLSS implementation.",
    };
}

ProtocolIngressContract makeXessDx12TranslationIngressContract()
{
    return ProtocolIngressContract{
        .protocol = UpscalerProtocol::Xess,
        .abi = ProtocolAbi::IntelXess,
        .integrationMode = ProtocolIntegrationMode::TranslationAdapter,
        .graphicsApi = GraphicsApi::Dx12,
        .surfaces = {
            requiredSurface(ProtocolSurfaceSemantic::Color, "XeSS input color"),
            requiredSurface(ProtocolSurfaceSemantic::MotionVectors, "XeSS motion vectors"),
            optionalSurface(ProtocolSurfaceSemantic::Depth, "XeSS depth", true),
            optionalSurface(ProtocolSurfaceSemantic::Exposure, "XeSS exposure", true),
            requiredSurface(ProtocolSurfaceSemantic::Output, "XeSS output"),
        },
        .ownsUpscalerSchedule = true,
        .hotPathMayCallThirdPartyRuntime = false,
        .protocolSpecificDataMustBeNormalizedAtIngress = true,
        .notes = "Future XeSS-style ingress should be a descriptor translation layer over APUSR, not an XeSS-shaped core.",
    };
}

Status validateProtocolIngressContract(const ProtocolIngressContract& contract)
{
    if (contract.surfaces.empty()) {
        return Status::invalidArgument("protocol ingress contract must declare at least one surface");
    }
    if (!contract.ownsUpscalerSchedule) {
        return Status::invalidArgument("APUSR frontends must own the upscaler schedule");
    }
    if (contract.hotPathMayCallThirdPartyRuntime) {
        return Status::invalidArgument("protocol frontends must not call third-party upscaler runtimes in the hot path");
    }
    if (!contract.protocolSpecificDataMustBeNormalizedAtIngress) {
        return Status::invalidArgument("protocol-specific descriptor semantics must be normalized at the frontend boundary");
    }

    bool hasColor = false;
    bool hasMotionVectors = false;
    bool hasOutput = false;
    for (const ProtocolSurfaceRequirement& surface : contract.surfaces) {
        if (surface.externalName.empty()) {
            return Status::invalidArgument("protocol surface requirement must name the external source");
        }
        if (surface.required && !surface.feedsCoreFrame && surface.semantic != ProtocolSurfaceSemantic::Output) {
            return Status::invalidArgument("required protocol surfaces must feed the APUSR frame unless they are explicit outputs");
        }
        hasColor = hasColor || (surface.required && surface.semantic == ProtocolSurfaceSemantic::Color);
        hasMotionVectors = hasMotionVectors || (surface.required && surface.semantic == ProtocolSurfaceSemantic::MotionVectors);
        hasOutput = hasOutput || (surface.required && surface.semantic == ProtocolSurfaceSemantic::Output);
    }

    if (!hasColor) {
        return Status::invalidArgument("protocol ingress contract must require color input");
    }
    if (!hasMotionVectors) {
        return Status::invalidArgument("protocol ingress contract must require motion-vector input");
    }
    if (!hasOutput) {
        return Status::invalidArgument("protocol ingress contract must require output surface");
    }

    switch (contract.protocol) {
    case UpscalerProtocol::NativeApusr:
        if (contract.abi != ProtocolAbi::NativeApusr || contract.integrationMode != ProtocolIntegrationMode::Native) {
            return Status::invalidArgument("native APUSR ingress must use the native ABI and integration mode");
        }
        break;
    case UpscalerProtocol::FidelityFxFsr4:
        if (contract.abi != ProtocolAbi::FidelityFxProvider || contract.integrationMode != ProtocolIntegrationMode::CompatibilityMasquerade) {
            return Status::invalidArgument("FSR4 ingress must use the FidelityFX provider ABI and compatibility masquerade mode");
        }
        break;
    case UpscalerProtocol::Dlss:
        if (contract.abi != ProtocolAbi::NvngxDlss || contract.integrationMode != ProtocolIntegrationMode::CompatibilityMasquerade) {
            return Status::invalidArgument("DLSS ingress must use the NVNGX ABI and compatibility masquerade mode");
        }
        break;
    case UpscalerProtocol::Xess:
        if (contract.abi != ProtocolAbi::IntelXess || contract.integrationMode != ProtocolIntegrationMode::TranslationAdapter) {
            return Status::invalidArgument("XeSS ingress must use the Intel XeSS ABI and translation-adapter mode");
        }
        break;
    }

    return Status::ok();
}

bool protocolIngressFeedsCoreFrame(const ProtocolIngressContract& contract)
{
    bool hasCoreInput = false;
    bool hasOutput = false;
    for (const ProtocolSurfaceRequirement& surface : contract.surfaces) {
        hasCoreInput = hasCoreInput || (surface.required && surface.feedsCoreFrame && surface.semantic != ProtocolSurfaceSemantic::Output);
        hasOutput = hasOutput || (surface.required && surface.semantic == ProtocolSurfaceSemantic::Output);
    }
    return hasCoreInput && hasOutput;
}

const char* upscalerProtocolName(const UpscalerProtocol protocol)
{
    switch (protocol) {
    case UpscalerProtocol::NativeApusr:
        return "native-apusr";
    case UpscalerProtocol::FidelityFxFsr4:
        return "fidelityfx-fsr4";
    case UpscalerProtocol::Dlss:
        return "dlss";
    case UpscalerProtocol::Xess:
        return "xess";
    }
    return "unknown";
}

const char* protocolAbiName(const ProtocolAbi abi)
{
    switch (abi) {
    case ProtocolAbi::NativeApusr:
        return "native-apusr";
    case ProtocolAbi::FidelityFxProvider:
        return "fidelityfx-provider";
    case ProtocolAbi::NvngxDlss:
        return "nvngx-dlss";
    case ProtocolAbi::IntelXess:
        return "intel-xess";
    }
    return "unknown";
}

const char* protocolIntegrationModeName(const ProtocolIntegrationMode mode)
{
    switch (mode) {
    case ProtocolIntegrationMode::Native:
        return "native";
    case ProtocolIntegrationMode::CompatibilityMasquerade:
        return "compatibility-masquerade";
    case ProtocolIntegrationMode::TranslationAdapter:
        return "translation-adapter";
    }
    return "unknown";
}

const char* protocolSurfaceSemanticName(const ProtocolSurfaceSemantic semantic)
{
    switch (semantic) {
    case ProtocolSurfaceSemantic::Color:
        return "color";
    case ProtocolSurfaceSemantic::Depth:
        return "depth";
    case ProtocolSurfaceSemantic::MotionVectors:
        return "motion-vectors";
    case ProtocolSurfaceSemantic::Exposure:
        return "exposure";
    case ProtocolSurfaceSemantic::ReactiveMask:
        return "reactive-mask";
    case ProtocolSurfaceSemantic::TransparencyMask:
        return "transparency-mask";
    case ProtocolSurfaceSemantic::Output:
        return "output";
    }
    return "unknown";
}

} // namespace apusr
