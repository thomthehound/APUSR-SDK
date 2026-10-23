// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_frontend.h>

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
    const apusr::FrontendCompatibilityDesc native = apusr::makeNativeApusrDx12Frontend();
    require(apusr::validateFrontendCompatibilityDesc(native).succeeded(), "native frontend profile must validate");
    require(!native.masqueradesAsExternalUpscaler, "native frontend must not masquerade as a third-party upscaler");
    require(native.ingressContract.protocol == apusr::UpscalerProtocol::NativeApusr, "native frontend must declare native ingress");
    require(!apusr::frontendNeedsOriginLedger(native), "native frontend must not require third-party origin tracking");

    const apusr::FrontendCompatibilityDesc ffx = apusr::makeFfxFsr4Dx12DropInFrontend();
    require(apusr::validateFrontendCompatibilityDesc(ffx).succeeded(), "FFX/FSR4 drop-in profile must validate");
    require(ffx.masqueradesAsExternalUpscaler, "FFX/FSR4 profile must explicitly declare the compatibility masquerade");
    require(ffx.ingressContract.protocol == apusr::UpscalerProtocol::FidelityFxFsr4, "FFX/FSR4 profile must declare FSR4 ingress");
    require(ffx.ingressContract.hotPathMayCallThirdPartyRuntime == false, "FFX/FSR4 profile must not call FFX in the hot path");
    require(ffx.usesThirdPartyModelArtifacts, "FFX/FSR4 profile must declare third-party model artifact use");
    require(ffx.usesThirdPartyApiHooks, "FFX/FSR4 profile must declare third-party API hook use");
    require(apusr::frontendNeedsOriginLedger(ffx), "FFX/FSR4 profile must require origin tracking");
    require(ffx.deployment.outputBaseName == "amd_fidelityfx_dx12", "FFX/FSR4 DX12 output basename should match the observed FidelityFX loader drop-in target");
    require(!ffx.deployment.candidateDropInNames.empty(), "FFX/FSR4 profile must declare a drop-in DLL name");

    const apusr::FrontendCompatibilityDesc ngx = apusr::makeNgxDlssDx12DropInFrontend();
    require(apusr::validateFrontendCompatibilityDesc(ngx).succeeded(), "NGX/DLSS drop-in profile must validate");
    require(ngx.masqueradesAsExternalUpscaler, "NGX/DLSS profile must explicitly declare the compatibility masquerade");
    require(ngx.abi == apusr::FrontendAbi::NvngxDlss, "NGX/DLSS profile must declare NVNGX ABI");
    require(ngx.ingressContract.protocol == apusr::UpscalerProtocol::Dlss, "NGX/DLSS profile must declare DLSS ingress");
    require(ngx.ingressContract.integrationMode == apusr::ProtocolIntegrationMode::CompatibilityMasquerade, "NGX/DLSS profile must be a compatibility masquerade");
    require(ngx.usesThirdPartyModelArtifacts, "NGX/DLSS profile must declare third-party model artifact use");
    require(ngx.usesThirdPartyApiHooks, "NGX/DLSS profile must declare third-party API hook use");
    require(apusr::frontendNeedsOriginLedger(ngx), "NGX/DLSS profile must require origin tracking");
    require(ngx.deployment.outputBaseName == "nvngx_dlss", "NGX/DLSS output basename should match the drop-in target");
    require(!ngx.deployment.candidateDropInNames.empty(), "NGX/DLSS profile must declare a drop-in DLL name");

    std::cout << "APUSR frontend smoke: native, FFX/FSR4, and NGX/DLSS profiles validated\n";
    return 0;
}
