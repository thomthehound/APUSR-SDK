// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_frontend.h>

#include <string>
#include <utility>

namespace apusr {

FrontendCompatibilityDesc makeNativeApusrDx12Frontend()
{
    return FrontendCompatibilityDesc{
        .persona = FrontendPersona::NativeApusr,
        .abi = FrontendAbi::NativeApusr,
        .ingressContract = makeNativeApusrDx12IngressContract(),
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Experimental,
        .ownership = FrontendOwnership::OwnsUpscalerSchedule,
        .masqueradesAsExternalUpscaler = false,
        .usesThirdPartyModelArtifacts = false,
        .usesThirdPartyApiHooks = false,
        .advertisedName = "APUSR",
        .implementationName = "APUSR native DX12 frontend",
        .exports = {
            FrontendExportDesc{.name = "apusrCreateSession", .role = "Create a native APUSR pipeline session"},
            FrontendExportDesc{.name = "apusrDestroySession", .role = "Destroy a native APUSR pipeline session"},
            FrontendExportDesc{.name = "apusrSubmitFrame", .role = "Submit one graphics-pipeline frame into the token stream"},
        },
        .deployment = FrontendDeploymentDesc{
            .targetName = "apusr_dx12",
            .outputBaseName = "apusr_dx12",
            .candidateDropInNames = {},
            .packaging = FrontendPackaging::SharedLibrary,
        },
    };
}

FrontendCompatibilityDesc makeFfxFsr4Dx12DropInFrontend()
{
    return FrontendCompatibilityDesc{
        .persona = FrontendPersona::FidelityFxFsr4,
        .abi = FrontendAbi::FidelityFxProvider,
        .ingressContract = makeFidelityFxFsr4Dx12IngressContract(),
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .ownership = FrontendOwnership::OwnsUpscalerSchedule,
        .masqueradesAsExternalUpscaler = true,
        .usesThirdPartyModelArtifacts = true,
        .usesThirdPartyApiHooks = true,
        .advertisedName = "AMD FidelityFX FSR4 compatible upscaler",
        .implementationName = "APUSR FSR4 INT8 DX12 frontend",
        .exports = {
            FrontendExportDesc{.name = "ffxCreateContext", .role = "FidelityFX context creation entry point"},
            FrontendExportDesc{.name = "ffxDestroyContext", .role = "FidelityFX context destruction entry point"},
            FrontendExportDesc{.name = "ffxConfigure", .role = "FidelityFX context configuration entry point"},
            FrontendExportDesc{.name = "ffxQuery", .role = "FidelityFX query entry point"},
            FrontendExportDesc{.name = "ffxDispatch", .role = "FidelityFX dispatch entry point"},
        },
        .deployment = FrontendDeploymentDesc{
            .targetName = "apusr_fsr4_dx12",
            .outputBaseName = "amd_fidelityfx_dx12",
            .candidateDropInNames = {"amd_fidelityfx_dx12.dll"},
            .packaging = FrontendPackaging::DropInSharedLibrary,
        },
    };
}

FrontendCompatibilityDesc makeNgxDlssDx12DropInFrontend()
{
    return FrontendCompatibilityDesc{
        .persona = FrontendPersona::NgxDlss,
        .abi = FrontendAbi::NvngxDlss,
        .ingressContract = makeDlssDx12TranslationIngressContract(),
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .ownership = FrontendOwnership::OwnsUpscalerSchedule,
        .masqueradesAsExternalUpscaler = true,
        .usesThirdPartyModelArtifacts = true,
        .usesThirdPartyApiHooks = true,
        .advertisedName = "NVIDIA DLSS-compatible upscaler",
        .implementationName = "APUSR NGX/DLSS DX12 translation frontend",
        .exports = {
            FrontendExportDesc{.name = "NVSDK_NGX_D3D12_Init", .role = "Initialize NGX/DLSS-compatible DX12 ingress"},
            FrontendExportDesc{.name = "NVSDK_NGX_D3D12_CreateFeature", .role = "Create a DLSS-shaped translation feature"},
            FrontendExportDesc{.name = "NVSDK_NGX_D3D12_EvaluateFeature", .role = "Normalize DLSS parameters and record APUSR backend work"},
            FrontendExportDesc{.name = "NVSDK_NGX_D3D12_ReleaseFeature", .role = "Destroy a DLSS-shaped translation feature"},
            FrontendExportDesc{.name = "NVSDK_NGX_D3D12_Shutdown", .role = "Tear down NGX/DLSS-compatible frontend state"},
        },
        .deployment = FrontendDeploymentDesc{
            .targetName = "apusr_ngx_dlss_dx12",
            .outputBaseName = "nvngx_dlss",
            .candidateDropInNames = {"nvngx_dlss.dll"},
            .packaging = FrontendPackaging::DropInSharedLibrary,
        },
    };
}

Status validateFrontendCompatibilityDesc(const FrontendCompatibilityDesc& desc)
{
    if (desc.advertisedName.empty()) {
        return Status::invalidArgument("frontend advertised name must not be empty");
    }
    if (desc.implementationName.empty()) {
        return Status::invalidArgument("frontend implementation name must not be empty");
    }
    if (desc.deployment.targetName.empty()) {
        return Status::invalidArgument("frontend deployment target name must not be empty");
    }
    if (desc.deployment.outputBaseName.empty()) {
        return Status::invalidArgument("frontend deployment output base name must not be empty");
    }
    if (desc.exports.empty()) {
        return Status::invalidArgument("frontend must declare at least one exported entry point");
    }
    for (const FrontendExportDesc& exportDesc : desc.exports) {
        if (exportDesc.name.empty()) {
            return Status::invalidArgument("frontend export name must not be empty");
        }
        if (exportDesc.role.empty()) {
            return Status::invalidArgument("frontend export role must not be empty");
        }
    }
    if (desc.deployment.packaging == FrontendPackaging::DropInSharedLibrary && desc.deployment.candidateDropInNames.empty()) {
        return Status::invalidArgument("drop-in frontends must declare at least one candidate DLL name");
    }
    if (desc.masqueradesAsExternalUpscaler && !desc.usesThirdPartyApiHooks) {
        return Status::invalidArgument("masquerade frontends must declare third-party API hook usage");
    }
    const Status ingressStatus = validateProtocolIngressContract(desc.ingressContract);
    if (!ingressStatus.succeeded()) {
        return ingressStatus;
    }
    if (desc.graphicsApi != desc.ingressContract.graphicsApi) {
        return Status::invalidArgument("frontend graphics API must match the protocol ingress contract");
    }
    if (desc.ownership != FrontendOwnership::OwnsUpscalerSchedule || !desc.ingressContract.ownsUpscalerSchedule) {
        return Status::invalidArgument("compatibility frontends must feed APUSR's own scheduler, not an external upscaler schedule");
    }
    if (desc.ingressContract.hotPathMayCallThirdPartyRuntime) {
        return Status::invalidArgument("frontend ingress must not call third-party upscaler runtimes in the hot path");
    }
    if (desc.persona == FrontendPersona::FidelityFxFsr4 && desc.modelFamily != ModelFamily::Fsr4Int8) {
        return Status::invalidArgument("FidelityFX FSR4 frontend must target the FSR4 INT8 model family");
    }
    if (desc.persona == FrontendPersona::FidelityFxFsr4 && desc.ingressContract.protocol != UpscalerProtocol::FidelityFxFsr4) {
        return Status::invalidArgument("FidelityFX FSR4 frontend must declare the FSR4 ingress protocol");
    }
    if (desc.persona == FrontendPersona::NgxDlss && desc.modelFamily != ModelFamily::Fsr4Int8) {
        return Status::invalidArgument("NGX/DLSS frontend must target the FSR4 INT8 model family");
    }
    if (desc.persona == FrontendPersona::NgxDlss && desc.ingressContract.protocol != UpscalerProtocol::Dlss) {
        return Status::invalidArgument("NGX/DLSS frontend must declare the DLSS ingress protocol");
    }
    if (desc.persona == FrontendPersona::NgxDlss && desc.abi != FrontendAbi::NvngxDlss) {
        return Status::invalidArgument("NGX/DLSS frontend must declare the NVNGX/DLSS ABI");
    }
    return Status::ok();
}

bool frontendNeedsOriginLedger(const FrontendCompatibilityDesc& desc)
{
    return desc.masqueradesAsExternalUpscaler || desc.usesThirdPartyModelArtifacts || desc.usesThirdPartyApiHooks;
}

const char* frontendPersonaName(const FrontendPersona persona)
{
    switch (persona) {
    case FrontendPersona::NativeApusr:
        return "native-apusr";
    case FrontendPersona::FidelityFxFsr4:
        return "fidelityfx-fsr4";
    case FrontendPersona::NgxDlss:
        return "ngx-dlss";
    }
    return "unknown";
}

const char* frontendAbiName(const FrontendAbi abi)
{
    switch (abi) {
    case FrontendAbi::NativeApusr:
        return "native-apusr";
    case FrontendAbi::FidelityFxProvider:
        return "fidelityfx-provider";
    case FrontendAbi::NvngxDlss:
        return "nvngx-dlss";
    }
    return "unknown";
}

const char* frontendPackagingName(const FrontendPackaging packaging)
{
    switch (packaging) {
    case FrontendPackaging::StaticLibrary:
        return "static-library";
    case FrontendPackaging::SharedLibrary:
        return "shared-library";
    case FrontendPackaging::DropInSharedLibrary:
        return "drop-in-shared-library";
    }
    return "unknown";
}

const char* frontendOwnershipName(const FrontendOwnership ownership)
{
    switch (ownership) {
    case FrontendOwnership::OwnsUpscalerSchedule:
        return "owns-upscaler-schedule";
    case FrontendOwnership::CompatibilityAdapterOnly:
        return "compatibility-adapter-only";
    }
    return "unknown";
}

} // namespace apusr
