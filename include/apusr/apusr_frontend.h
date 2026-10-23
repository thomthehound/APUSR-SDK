// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>
#include <apusr/apusr_manifest.h>
#include <apusr/apusr_protocol.h>

#include <cstdint>
#include <string>
#include <vector>

namespace apusr {

enum class FrontendPersona : std::uint32_t {
    NativeApusr,
    FidelityFxFsr4,
    NgxDlss,
};

enum class FrontendAbi : std::uint32_t {
    NativeApusr,
    FidelityFxProvider,
    NvngxDlss,
};

enum class FrontendPackaging : std::uint32_t {
    StaticLibrary,
    SharedLibrary,
    DropInSharedLibrary,
};

enum class FrontendOwnership : std::uint32_t {
    OwnsUpscalerSchedule,
    CompatibilityAdapterOnly,
};

struct FrontendExportDesc {
    std::string name;
    std::string role;
};

struct FrontendDeploymentDesc {
    std::string targetName;
    std::string outputBaseName;
    std::vector<std::string> candidateDropInNames;
    FrontendPackaging packaging = FrontendPackaging::SharedLibrary;
};

struct FrontendCompatibilityDesc {
    FrontendPersona persona = FrontendPersona::NativeApusr;
    FrontendAbi abi = FrontendAbi::NativeApusr;
    ProtocolIngressContract ingressContract;
    GraphicsApi graphicsApi = GraphicsApi::Dx12;
    ModelFamily modelFamily = ModelFamily::Fsr4Int8;
    FrontendOwnership ownership = FrontendOwnership::OwnsUpscalerSchedule;
    bool masqueradesAsExternalUpscaler = false;
    bool usesThirdPartyModelArtifacts = false;
    bool usesThirdPartyApiHooks = false;
    std::string advertisedName;
    std::string implementationName;
    std::vector<FrontendExportDesc> exports;
    FrontendDeploymentDesc deployment;
};

[[nodiscard]] FrontendCompatibilityDesc makeNativeApusrDx12Frontend();
[[nodiscard]] FrontendCompatibilityDesc makeFfxFsr4Dx12DropInFrontend();
[[nodiscard]] FrontendCompatibilityDesc makeNgxDlssDx12DropInFrontend();
[[nodiscard]] Status validateFrontendCompatibilityDesc(const FrontendCompatibilityDesc& desc);
[[nodiscard]] bool frontendNeedsOriginLedger(const FrontendCompatibilityDesc& desc);
[[nodiscard]] const char* frontendPersonaName(FrontendPersona persona);
[[nodiscard]] const char* frontendAbiName(FrontendAbi abi);
[[nodiscard]] const char* frontendPackagingName(FrontendPackaging packaging);
[[nodiscard]] const char* frontendOwnershipName(FrontendOwnership ownership);

} // namespace apusr
