// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#undef NDEBUG

#include <apusr/backends/apusr_fsr4_reference_dx12.h>
#include <apusr/generated/apusr_fsr4_reference_dx12_shader_blobs.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

int main()
{
    using namespace apusr;
    using namespace apusr::backends;
    using namespace apusr::models::fsr4_int8;

    Dx12Fsr4ReferenceCreateDesc create{};
    create.repoRoot = APUSR_REPO_ROOT;
    create.runner.maxRenderSize = RenderSize{.width = 2560, .height = 1440};
    create.runner.maxUpscaleSize = DisplaySize{.width = 3440, .height = 1440};
    create.runner.features.displayResolutionMotionVectors = false;
    create.runner.features.motionVectorJitterCancellation = false;
    create.runner.features.depthInverted = true;
    create.runner.features.autoExposure = true;
    create.runner.initialPreset = Preset::Balanced;

    Dx12Fsr4ReferenceFrameDesc frame{};
    frame.runner.frame.frameIndex = 7;
    frame.runner.frame.renderSize = RenderSize{.width = 2024, .height = 847};
    frame.runner.frame.displaySize = DisplaySize{.width = 3440, .height = 1440};
    frame.runner.frame.jitter = Jitter{.x = 0.25f, .y = -0.125f};
    frame.runner.frame.options.motionVectorScale = MotionVectorScale{.x = -2024.0f, .y = 847.0f};
    frame.runner.frame.options.features = create.runner.features;
    frame.runner.frame.options.hasDepth = true;
    frame.runner.frame.options.hasExposure = false;
    frame.runner.frame.options.preExposure = 1.0f;
    frame.runner.frame.options.sharpeningRequested = true;
    frame.runner.frame.options.sharpness = 0.35f;
    frame.runner.residentModelPreset = Preset::Balanced;

    Dx12Fsr4ReferenceBackendPlan plan{};
    const Status status = makeDx12Fsr4ReferenceBackendPlan(create, frame, plan);
    assert(status.succeeded());
    assert(plan.framePlan.preset == Preset::Balanced);
    assert(plan.framePlan.frameCapacity == FrameCapacity::R2160);
    assert(plan.initializerBytes > 0);
    assert(plan.resourcePlan.scratchByteSize == scratchByteSize(FrameCapacity::R2160));
    assert(!plan.shaders.empty());
    assert(!plan.descriptorBindings.empty());
    assert(plan.srvCount == 19);
    assert(plan.uavCount == 12);
    assert(plan.cbvCount == 2);
    assert(plan.samplerCount == 1);
    assert(plan.maxSrvRegister == 18);
    assert(plan.maxUavRegister == 11);
    assert(plan.maxCbvRegister == 1);
    assert(plan.maxSamplerRegister == 0);
    const bool hasEmbeddedProductTable = apusr::backends::fsr4_reference_dx12_shader_blobs::shaderBlobCount() > 0;
    assert(plan.availableShaderBlobCount == (hasEmbeddedProductTable ? plan.shaders.size() : 0u));
    assert(plan.missingShaderBlobCount == (hasEmbeddedProductTable ? 0u : plan.shaders.size()));
    assert(plan.shaderProvider.embeddedShaderBlobCount == (hasEmbeddedProductTable ? plan.shaders.size() : 0u));
    assert(plan.shaderProvider.packagedShaderBlobCount == 0);
    assert(plan.shaderProvider.runtimeCompileCandidateCount == 0);
    assert(plan.shaderProvider.missingShaderBlobCount == (hasEmbeddedProductTable ? 0u : plan.shaders.size()));
    assert(plan.framePlan.permutationKey == 458u);
    assert(dx12Fsr4ReferenceShaderCacheKey(Preset::Balanced, FrameCapacity::R2160, 458u, "fsr4_pre") == "balanced/2160/p458/fsr4_pre");
    assert(dx12Fsr4ReferencePackagedShaderBlobPath("cache", Preset::Balanced, FrameCapacity::R2160, 458u, "fsr4_pre") == "cache/balanced/2160/p458/fsr4_pre.dxil");

    assert(plan.rootSignature.rootParameters.size() == 4);
    assert(plan.rootSignature.staticSamplers.size() == 1);
    assert(plan.rootSignature.uavTableRootParameter == 0);
    assert(plan.rootSignature.srvTableRootParameter == 1);
    assert(plan.rootSignature.fsr4ConstantsRootParameter == 2);
    assert(plan.rootSignature.spdConstantsRootParameter == 2);
    assert(plan.rootSignature.rcasConstantsRootParameter == 2);
    assert(plan.rootSignature.passWeightsRootParameter == 3);
    assert(plan.rootSignature.descriptorHeap.srvBaseOffset == 0);
    assert(plan.rootSignature.descriptorHeap.uavBaseOffset == plan.srvCount);
    assert(plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors == plan.srvCount + plan.uavCount);
    assert(plan.rootSignature.rootParameters[0].kind == Dx12Fsr4ReferenceRootParameterKind::UavDescriptorTable);
    assert(plan.rootSignature.rootParameters[0].descriptorCount == plan.uavCount);
    assert(plan.rootSignature.rootParameters[1].kind == Dx12Fsr4ReferenceRootParameterKind::SrvDescriptorTable);
    assert(plan.rootSignature.rootParameters[1].descriptorCount == plan.srvCount);
    assert(plan.rootSignature.rootParameters[2].kind == Dx12Fsr4ReferenceRootParameterKind::RootCbv);
    assert(plan.rootSignature.rootParameters[2].baseShaderRegister == 0);
    assert(plan.rootSignature.rootParameters[3].kind == Dx12Fsr4ReferenceRootParameterKind::RootCbv);
    assert(plan.rootSignature.rootParameters[3].baseShaderRegister == 1);
    assert(std::string(plan.rootSignature.staticSamplers[0].shaderName) == "g_history_sampler");

    assert(plan.dispatchRecordPlan.size() == plan.framePlan.dispatches.size());
    assert(!plan.descriptorPatchPlan.empty());
    assert(!plan.resourceUsagePlan.empty());
    assert(plan.resourceRealizationPlan.externalBorrowedCount == 5);
    assert(plan.resourceRealizationPlan.internalOwnedCount >= 8);
    assert(plan.resourceRealizationPlan.uploadOwnedCount == 1);
    assert(plan.resourceRealizationPlan.aliasCount == 3);
    assert(plan.resourceRealizationPlan.missingRequiredExternalCount == 4);
    assert(plan.resourceRealizationPlan.missingOptionalExternalCount == 1);
    assert(plan.resourceRealizationPlan.needsInitializerUpload);
    assert(plan.resourceRealizationPlan.needsDefaultExposureUpload);
    assert(plan.resourceRealizationPlan.persistentByteSize >= plan.resourcePlan.scratchByteSize);
    assert(plan.descriptorRealizationPlan.heapRequired);
    assert(plan.descriptorRealizationPlan.shaderVisibleDescriptorCount >= plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors);
    assert((plan.descriptorRealizationPlan.shaderVisibleDescriptorCount % plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors) == 0);
    assert(plan.descriptorRealizationPlan.shaderVisibleDescriptorCount > plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors);
    assert(plan.descriptorRealizationPlan.activeSrvPatchCount > 0);
    assert(plan.descriptorRealizationPlan.activeUavPatchCount > 0);
    assert(plan.descriptorRealizationPlan.uniqueShaderVisibleSlotCount > 0);
    assert(plan.descriptorRealizationPlan.missingRequiredResourceDescriptorCount >= plan.resourceRealizationPlan.missingRequiredExternalCount);
    assert(plan.constantUploadPlan.slotCount == 4);
    assert(plan.constantUploadPlan.perFrameByteSize == 1792);
    assert(plan.constantUploadPlan.hasPassWeights);
    assert(plan.constantUploadPlan.hasSpdConstants);
    assert(plan.constantUploadPlan.hasRcasConstants);
    assert(plan.constantUploadPlan.slots[0].role == ReferenceConstantBufferRole::Fsr4Upscaler);
    assert(plan.constantUploadPlan.slots[0].byteOffset == 0);
    assert(plan.constantUploadPlan.slots[0].byteSize == sizeof(OptimizedConstants));
    assert(plan.constantUploadPlan.slots[1].role == ReferenceConstantBufferRole::SpdAutoExposure);
    assert(plan.constantUploadPlan.slots[1].byteOffset == 256);
    assert(plan.constantUploadPlan.slots[2].role == ReferenceConstantBufferRole::Rcas);
    assert(plan.constantUploadPlan.slots[2].byteOffset == 512);
    assert(plan.constantUploadPlan.slots[3].role == ReferenceConstantBufferRole::PassWeights);
    assert(plan.constantUploadPlan.slots[3].byteOffset == 768);
    assert(plan.constantUploadPlan.slots[3].byteSize == sizeof(Pass0WeightsConstants));
    bool sawRealizedHistory = false;
    bool sawBorrowedInputColor = false;
    bool sawAliasMlsrOutput = false;
    for (const Dx12Fsr4ReferenceRealizedResourceDesc& resource : plan.resourceRealizationPlan.resources) {
        if (resource.id == ResourceId::History) {
            sawRealizedHistory = true;
            assert(resource.ownership == Dx12Fsr4ReferenceResourceOwnership::InternalOwned);
            assert(resource.width == plan.resourcePlan.alignedMaxUpscaleSize.width);
            assert(resource.height == plan.resourcePlan.alignedMaxUpscaleSize.height);
        }
        if (resource.id == ResourceId::InputColor) {
            sawBorrowedInputColor = true;
            assert(resource.ownership == Dx12Fsr4ReferenceResourceOwnership::ExternalBorrowed);
            assert(resource.requiredForSelectedFrame);
            assert(!resource.validHandle);
        }
        if (resource.id == ResourceId::MlsrOutput) {
            sawAliasMlsrOutput = true;
            assert(resource.ownership == Dx12Fsr4ReferenceResourceOwnership::Alias);
            assert(resource.resolvedResource == ResourceId::RcasTemp);
        }
    }
    assert(sawRealizedHistory);
    assert(sawBorrowedInputColor);
    assert(sawAliasMlsrOutput);
    bool sawPreRecord = false;
    bool sawPreInputColorPatch = false;
    bool sawPostMlsrAliasToRcasTemp = false;
    std::uint32_t preScratchU11HeapOffset = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t rcasOutputU11HeapOffset = std::numeric_limits<std::uint32_t>::max();
    for (const Dx12Fsr4ReferenceDispatchRecordPlan& record : plan.dispatchRecordPlan) {
        if (std::string(record.dispatchName) == "fsr4_pre") {
            sawPreRecord = true;
            assert(record.primaryConstants == ReferenceConstantBufferRole::Fsr4Upscaler);
            assert(record.primaryConstantsRootParameter == plan.rootSignature.fsr4ConstantsRootParameter);
            assert(record.passWeightsRootParameter == plan.rootSignature.passWeightsRootParameter);
            assert(record.constantBuffers.size() == 2);
            assert(record.constantBuffers[0].role == ReferenceConstantBufferRole::Fsr4Upscaler);
            assert(record.constantBuffers[0].rootParameterIndex == plan.rootSignature.fsr4ConstantsRootParameter);
            assert(record.constantBuffers[1].role == ReferenceConstantBufferRole::PassWeights);
            assert(record.constantBuffers[1].rootParameterIndex == plan.rootSignature.passWeightsRootParameter);
            assert(record.srvBindingCount == 6);
            assert(record.uavBindingCount == 2);
            assert(record.cbvBindingCount == 2);
            assert(record.samplerBindingCount == 1);
            assert(record.pipelineCacheKey == "balanced/2160/p458/fsr4_pre");
            assert(record.srvDescriptorTableBase != std::numeric_limits<std::uint32_t>::max());
            assert(record.uavDescriptorTableBase != std::numeric_limits<std::uint32_t>::max());
            assert(!record.resourceTransitions.empty());
            bool sawPreScratchTransition = false;
            bool sawPreInputTransition = false;
            for (const Dx12Fsr4ReferenceDispatchResourceTransitionDesc& transition : record.resourceTransitions) {
                if (transition.resource == ResourceId::ScratchBuffer) {
                    sawPreScratchTransition = true;
                    assert(transition.descriptorSpace == Dx12Fsr4ReferenceDescriptorSpace::Uav);
                    assert(!transition.optional);
                }
                if (transition.resource == ResourceId::InputColor) {
                    sawPreInputTransition = true;
                    assert(transition.descriptorSpace == Dx12Fsr4ReferenceDescriptorSpace::Srv);
                    assert(!transition.optional);
                }
            }
            assert(sawPreScratchTransition);
            assert(sawPreInputTransition);
        }
    }
    for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
        if (std::string(patch.dispatchName) == "fsr4_pre" && patch.resource == ResourceId::InputColor &&
            patch.space == Dx12Fsr4ReferenceDescriptorSpace::Srv) {
            sawPreInputColorPatch = true;
            assert(patch.registerIndex == 3);
            assert(patch.rootParameterIndex == plan.rootSignature.srvTableRootParameter);
            assert(patch.descriptorTableOffset == 3);
            assert((patch.shaderVisibleHeapOffset % plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors) ==
                   plan.rootSignature.descriptorHeap.srvBaseOffset + 3);
            assert(patch.resolvedResource == ResourceId::InputColor);
        }
        if (std::string(patch.dispatchName) == "fsr4_post" && patch.resource == ResourceId::MlsrOutput &&
            patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav) {
            sawPostMlsrAliasToRcasTemp = true;
            assert(patch.resolvedResource == ResourceId::RcasTemp);
            assert(patch.rootParameterIndex == plan.rootSignature.uavTableRootParameter);
            assert((patch.shaderVisibleHeapOffset % plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors) ==
                   plan.rootSignature.descriptorHeap.uavBaseOffset + patch.descriptorTableOffset);
        }
        if (std::string(patch.dispatchName) == "fsr4_pre" && patch.resource == ResourceId::ScratchBuffer &&
            patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav && patch.registerIndex == 11) {
            preScratchU11HeapOffset = patch.shaderVisibleHeapOffset;
        }
        if (std::string(patch.dispatchName) == "rcas" && patch.resource == ResourceId::RcasOutput &&
            patch.space == Dx12Fsr4ReferenceDescriptorSpace::Uav && patch.registerIndex == 11) {
            rcasOutputU11HeapOffset = patch.shaderVisibleHeapOffset;
        }
    }
    bool sawUsagePostMlsr = false;
    for (const ReferenceResourceUsageDesc& usage : plan.resourceUsagePlan) {
        if (usage.resource == ResourceId::MlsrOutput && usage.passKind == PassKind::ModelPost) {
            sawUsagePostMlsr = true;
            assert(usage.access == ReferenceResourceAccess::Write);
        }
    }
    assert(sawUsagePostMlsr);

    assert(sawPreRecord);
    assert(sawPreInputColorPatch);
    assert(sawPostMlsrAliasToRcasTemp);
    assert(preScratchU11HeapOffset != std::numeric_limits<std::uint32_t>::max());
    assert(rcasOutputU11HeapOffset != std::numeric_limits<std::uint32_t>::max());
    assert(preScratchU11HeapOffset != rcasOutputU11HeapOffset);
    assert((preScratchU11HeapOffset % plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors) ==
           (rcasOutputU11HeapOffset % plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors));

    Dx12Fsr4ReferenceCreateDesc noAutoCreate = create;
    noAutoCreate.runner.features.autoExposure = false;
    Dx12Fsr4ReferenceFrameDesc noAutoFrame = frame;
    noAutoFrame.runner.frame.options.features = noAutoCreate.runner.features;
    noAutoFrame.runner.frame.options.hasExposure = false;
    Dx12Fsr4ReferenceBackendPlan noAutoPlan{};
    assert(makeDx12Fsr4ReferenceBackendPlan(noAutoCreate, noAutoFrame, noAutoPlan).succeeded());
    assert(!noAutoPlan.framePlan.permutation.autoExposure);
    assert(noAutoPlan.framePlan.constants.useDefaultExposure == 1);
    bool noAutoSawSpd = false;
    for (const ReferenceDispatchDesc& dispatch : noAutoPlan.framePlan.dispatches) {
        if (dispatch.kind == PassKind::SpdAutoExposure) {
            noAutoSawSpd = true;
        }
    }
    assert(!noAutoSawSpd);
    bool noAutoSawPreInputExposure = false;
    bool noAutoSawPostInputExposure = false;
    bool noAutoSawRcasInputExposure = false;
    bool noAutoSawAutoExposure = false;
    for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : noAutoPlan.descriptorPatchPlan) {
        if (patch.resource == ResourceId::AutoExposure) {
            noAutoSawAutoExposure = true;
        }
        if (patch.resource == ResourceId::InputExposure) {
            assert(patch.registerIndex == 6);
            assert(patch.resolvedResource == ResourceId::InternalDefaultExposure);
            if (std::string(patch.dispatchName) == "fsr4_pre") {
                noAutoSawPreInputExposure = true;
            }
            if (std::string(patch.dispatchName) == "fsr4_post") {
                noAutoSawPostInputExposure = true;
            }
            if (std::string(patch.dispatchName) == "rcas") {
                noAutoSawRcasInputExposure = true;
            }
        }
    }
    assert(!noAutoSawAutoExposure);
    assert(noAutoSawPreInputExposure);
    assert(noAutoSawPostInputExposure);
    assert(noAutoSawRcasInputExposure);

    std::vector<Dx12Fsr4ReferenceShaderReflectionDesc> plannedReflection;
    for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
        Dx12Fsr4ReferenceShaderReflectionDesc reflected{};
        reflected.dispatchName = dispatch.name;
        reflected.passKind = dispatch.kind;
        reflected.logicalStage = dispatch.logicalStage;
        for (const Dx12Fsr4ReferenceDescriptorPatchDesc& patch : plan.descriptorPatchPlan) {
            if (patch.dispatchName != dispatch.name) {
                continue;
            }
            reflected.registers.push_back(Dx12Fsr4ReferenceShaderRegisterUse{
                .passKind = dispatch.kind,
                .space = patch.space,
                .registerIndex = patch.registerIndex,
                .registerSpace = patch.registerSpace,
                .shaderName = patch.shaderName,
                .required = true,
            });
        }
        plannedReflection.push_back(std::move(reflected));
    }
    Dx12Fsr4ReferenceShaderReflectionValidationReport reflectionReport{};
    assert(validateDx12Fsr4ReferenceShaderReflectionCoverage(plan, plannedReflection, &reflectionReport).succeeded());
    assert(reflectionReport.inspectedShaderCount == plannedReflection.size());
    assert(reflectionReport.missingBindingCount == 0);
    assert(reflectionReport.missingRootSignatureCoverageCount == 0);

    std::vector<Dx12Fsr4ReferenceShaderReflectionDesc> badReflection = plannedReflection;
    badReflection.front().registers.push_back(Dx12Fsr4ReferenceShaderRegisterUse{
        .passKind = badReflection.front().passKind,
        .space = Dx12Fsr4ReferenceDescriptorSpace::Srv,
        .registerIndex = 42,
        .registerSpace = 0,
        .shaderName = "r_missing_reflection_test",
        .required = true,
    });
    Dx12Fsr4ReferenceShaderReflectionValidationReport badReflectionReport{};
    const Status badReflectionStatus = validateDx12Fsr4ReferenceShaderReflectionCoverage(plan, badReflection, &badReflectionReport);
    assert(badReflectionStatus.code == StatusCode::InvalidArgument);
    assert(badReflectionReport.missingBindingCount == 1);
    assert(badReflectionReport.missingRootSignatureCoverageCount == 1);

    const auto writeReflectionManifest = [&](const std::filesystem::path& path,
                                             const std::vector<Dx12Fsr4ReferenceShaderReflectionDesc>& reflection,
                                             std::uint32_t permutationKey) {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        out << "{\n";
        out << "  \"schema\": \"apusr.fsr4_reference.shader_resource_manifest.v1\",\n";
        out << "  \"targetApi\": \"dx12\",\n";
        out << "  \"compiler\": \"smoke\",\n";
        out << "  \"targetProfile\": \"cs_6_6\",\n";
        out << "  \"entries\": [\n";
        for (std::size_t i = 0; i < reflection.size(); ++i) {
            const Dx12Fsr4ReferenceShaderReflectionDesc& shader = reflection[i];
            out << "    {\n";
            out << "      \"preset\": \"balanced\",\n";
            out << "      \"capacity\": \"2160\",\n";
            out << "      \"permutationKey\": " << permutationKey << ",\n";
            out << "      \"dispatch\": \"" << shader.dispatchName << "\",\n";
            out << "      \"passKind\": \"" << passKindName(shader.passKind) << "\",\n";
            out << "      \"logicalStage\": " << shader.logicalStage << ",\n";
            out << "      \"entryPoint\": \"main\",\n";
            out << "      \"source\": \"smoke\",\n";
            out << "      \"resourceCount\": " << shader.registers.size() << ",\n";
            out << "      \"resources\": [\n";
            for (std::size_t j = 0; j < shader.registers.size(); ++j) {
                const Dx12Fsr4ReferenceShaderRegisterUse& reg = shader.registers[j];
                out << "        {\"name\": \"" << (reg.shaderName != nullptr ? reg.shaderName : "unnamed")
                    << "\", \"space\": \"" << dx12Fsr4ReferenceDescriptorSpaceName(reg.space)
                    << "\", \"register\": " << reg.registerIndex
                    << ", \"registerSpace\": " << reg.registerSpace
                    << ", \"type\": \"smoke\"}";
                if (j + 1u != shader.registers.size()) {
                    out << ",";
                }
                out << "\n";
            }
            out << "      ]\n";
            out << "    }";
            if (i + 1u != reflection.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << "  ]\n";
        out << "}\n";
    };

    const std::filesystem::path manifestPath = std::filesystem::temp_directory_path() / "apusr_fsr4_reference_dx12_reflection_manifest_smoke.json";
    writeReflectionManifest(manifestPath, plannedReflection, plan.framePlan.permutationKey);
    Dx12Fsr4ReferenceShaderReflectionManifest manifest{};
    assert(loadDx12Fsr4ReferenceShaderReflectionManifest(manifestPath.generic_string(), manifest).succeeded());
    assert(manifest.entryCount == plannedReflection.size());
    assert(manifest.registerCount == reflectionReport.inspectedRegisterCount);
    const Dx12Fsr4ReferenceShaderReflectionManifestSelection selection =
        selectDx12Fsr4ReferenceShaderReflectionManifestEntries(manifest, plan.framePlan);
    assert(selection.selectedEntryCount == plannedReflection.size());
    assert(selection.shaders.size() == plannedReflection.size());
    Dx12Fsr4ReferenceShaderReflectionValidationReport manifestReport{};
    assert(validateDx12Fsr4ReferenceShaderReflectionManifestCoverage(plan, manifest, true, &manifestReport).succeeded());
    assert(manifestReport.missingManifestDispatchCount == 0);
    assert(manifestReport.inspectedRegisterCount == reflectionReport.inspectedRegisterCount);

    const std::filesystem::path incompleteManifestPath = std::filesystem::temp_directory_path() / "apusr_fsr4_reference_dx12_reflection_manifest_incomplete_smoke.json";
    writeReflectionManifest(incompleteManifestPath, std::vector<Dx12Fsr4ReferenceShaderReflectionDesc>{plannedReflection.front()}, plan.framePlan.permutationKey);
    Dx12Fsr4ReferenceShaderReflectionManifest incompleteManifest{};
    assert(loadDx12Fsr4ReferenceShaderReflectionManifest(incompleteManifestPath.generic_string(), incompleteManifest).succeeded());
    Dx12Fsr4ReferenceShaderReflectionValidationReport incompleteManifestReport{};
    const Status incompleteManifestStatus = validateDx12Fsr4ReferenceShaderReflectionManifestCoverage(
        plan, incompleteManifest, true, &incompleteManifestReport);
    assert(incompleteManifestStatus.code == StatusCode::InvalidArgument);
    assert(incompleteManifestReport.missingManifestDispatchCount == plan.framePlan.dispatches.size() - 1u);

    const std::filesystem::path wrongPermutationManifestPath = std::filesystem::temp_directory_path() / "apusr_fsr4_reference_dx12_reflection_manifest_wrong_perm_smoke.json";
    writeReflectionManifest(wrongPermutationManifestPath, plannedReflection, plan.framePlan.permutationKey + 1u);
    Dx12Fsr4ReferenceShaderReflectionManifest wrongPermutationManifest{};
    assert(loadDx12Fsr4ReferenceShaderReflectionManifest(wrongPermutationManifestPath.generic_string(), wrongPermutationManifest).succeeded());
    const Dx12Fsr4ReferenceShaderReflectionManifestSelection wrongPermutationSelection =
        selectDx12Fsr4ReferenceShaderReflectionManifestEntries(wrongPermutationManifest, plan.framePlan);
    assert(wrongPermutationSelection.selectedEntryCount == 0);

    Dx12Fsr4ReferenceCreateDesc manifestCreate = create;
    manifestCreate.reflectionManifestPath = manifestPath.generic_string();
    manifestCreate.enableReflectionManifestValidation = true;
    manifestCreate.requireCompleteReflectionCoverage = true;
    auto manifestRuntime = createDx12Fsr4ReferenceRuntime();
    assert(manifestRuntime != nullptr);
    assert(manifestRuntime->initialize(manifestCreate).succeeded());
    Dx12Fsr4ReferenceBackendPlan manifestRuntimePlan{};
    assert(manifestRuntime->planFrame(frame, manifestRuntimePlan).succeeded());
    assert(manifestRuntimePlan.framePlan.permutationKey == plan.framePlan.permutationKey);
    std::filesystem::remove(manifestPath);
    std::filesystem::remove(incompleteManifestPath);
    std::filesystem::remove(wrongPermutationManifestPath);

#ifdef APUSR_REFLECTION_MANIFEST_PATH
    const std::filesystem::path generatedManifestPath = APUSR_REFLECTION_MANIFEST_PATH;
    if (std::filesystem::exists(generatedManifestPath)) {
        Dx12Fsr4ReferenceShaderReflectionManifest generatedManifest{};
        assert(loadDx12Fsr4ReferenceShaderReflectionManifest(generatedManifestPath.generic_string(), generatedManifest).succeeded());
        assert(generatedManifest.entryCount > 0);
        const Dx12Fsr4ReferenceShaderReflectionManifestSelection generatedSelection =
            selectDx12Fsr4ReferenceShaderReflectionManifestEntries(generatedManifest, plan.framePlan);
        assert(generatedSelection.selectedEntryCount > 0);
        Dx12Fsr4ReferenceShaderReflectionValidationReport generatedManifestReport{};
        assert(validateDx12Fsr4ReferenceShaderReflectionManifestCoverage(
            plan, generatedManifest, false, &generatedManifestReport).succeeded());
        assert(generatedManifestReport.inspectedRegisterCount > 0);
        Dx12Fsr4ReferenceCreateDesc generatedManifestCreate = create;
        generatedManifestCreate.reflectionManifestPath = generatedManifestPath.generic_string();
        generatedManifestCreate.requireCompleteReflectionCoverage = false;
        auto generatedManifestRuntime = createDx12Fsr4ReferenceRuntime();
        assert(generatedManifestRuntime != nullptr);
        assert(generatedManifestRuntime->initialize(generatedManifestCreate).succeeded());
        Dx12Fsr4ReferenceBackendPlan generatedManifestRuntimePlan{};
        assert(generatedManifestRuntime->planFrame(frame, generatedManifestRuntimePlan).succeeded());
    }
#endif

    bool sawPre = false;
    bool sawPass1 = false;
    bool sawPost = false;
    for (const Dx12Fsr4ReferenceShaderBuildDesc& shader : plan.shaders) {
        assert(shader.targetProfile == "cs_6_6");
        assert(shader.hlslVersion == "2021");
        assert(shader.enable16BitTypes);
        if (hasEmbeddedProductTable) {
            const auto* blob = apusr::backends::fsr4_reference_dx12_shader_blobs::findShaderBlob(
                plan.framePlan.preset,
                plan.framePlan.frameCapacity,
                plan.framePlan.permutationKey,
                shader.shader.name);
            assert(blob != nullptr);
            assert(blob->data != nullptr);
            assert(blob->size > 0);
        }
        const auto hasDefine = [&](const std::string& define) {
            return std::find(shader.shader.defines.begin(), shader.shader.defines.end(), define) != shader.shader.defines.end();
        };
        assert(hasDefine("FFX_GPU=1"));
        assert(hasDefine("FFX_HLSL=1"));
        assert(hasDefine("FFX_HLSL_SM=66"));
        for (const std::string& define : shader.shader.defines) {
            assert(define.find("FFX_FSR4_OPTION_") == std::string::npos);
        }
        if (shader.shader.kind == PassKind::ModelPre ||
            shader.shader.kind == PassKind::ModelMiddle ||
            shader.shader.kind == PassKind::ModelPost) {
            assert(hasDefine("FFX_HALF=1"));
        } else {
            assert(!hasDefine("FFX_HALF=1"));
        }
        if (shader.shader.kind == PassKind::ModelPre) {
            sawPre = true;
            assert(shader.shader.entryPoint == "main");
            assert(hasDefine("FFX_MLSR_DEPTH_INVERTED=1"));
            assert(hasDefine("FFX_MLSR_LOW_RES_MV=1"));
            assert(hasDefine("FFX_MLSR_AUTOEXPOSURE_ENABLED=1"));
            assert(hasDefine("FFX_MLSR_COLORSPACE=0"));
        }
        if (shader.shader.kind == PassKind::ModelMiddle && shader.shader.logicalStage == 1) {
            sawPass1 = true;
            assert(shader.shader.entryPoint == "fsr4_model_v07_i8_pass1");
            assert(hasDefine("MLSR_PASS_1=1"));
        }
        if (shader.shader.kind == PassKind::ModelPost) {
            sawPost = true;
            assert(shader.shader.entryPoint == "main");
            assert(hasDefine("FFX_DEBUG_VISUALIZE=0"));
        }
    }
    assert(sawPre);
    assert(sawPass1);
    assert(sawPost);

    bool sawSrvInputColor = false;
    bool sawUavScratch = false;
    bool sawCbvPassWeights = false;
    bool sawStrictMiddleConstants = false;
    std::uint32_t middleScratchBindingCount = 0;
    std::uint32_t middleInitializerBindingCount = 0;
    std::size_t exactBindingCount = 0;
    for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
        exactBindingCount += dispatch.bindings.size();
    }
    assert(plan.descriptorBindings.size() == exactBindingCount);
    for (const Dx12Fsr4ReferenceDescriptorBinding& binding : plan.descriptorBindings) {
        if (binding.resource == ResourceId::InputColor && binding.space == Dx12Fsr4ReferenceDescriptorSpace::Srv &&
            std::string(binding.dispatchName) == "fsr4_pre") {
            sawSrvInputColor = true;
            assert(binding.passKind == PassKind::ModelPre);
            assert(binding.registerIndex == 3);
            assert(binding.access == ReferenceResourceAccess::Read);
        }
        if (binding.resource == ResourceId::ScratchBuffer && binding.space == Dx12Fsr4ReferenceDescriptorSpace::Uav &&
            binding.passKind == PassKind::ModelMiddle) {
            sawUavScratch = true;
            ++middleScratchBindingCount;
            assert(binding.registerIndex == 1);
            assert(binding.access == ReferenceResourceAccess::ReadWrite);
        }
        if (binding.resource == ResourceId::InitializerBuffer && binding.passKind == PassKind::ModelMiddle) {
            ++middleInitializerBindingCount;
            assert(binding.space == Dx12Fsr4ReferenceDescriptorSpace::Srv);
            assert(binding.logicalStage >= 6 && binding.logicalStage <= 9);
        }
        if (binding.constantBuffer == ReferenceConstantBufferRole::PassWeights && binding.space == Dx12Fsr4ReferenceDescriptorSpace::Cbv) {
            sawCbvPassWeights = true;
            assert(binding.passKind == PassKind::ModelPre);
            assert(binding.registerIndex == 1);
        }
        if (binding.constantBuffer == ReferenceConstantBufferRole::Fsr4Upscaler &&
            binding.passKind == PassKind::ModelMiddle && binding.space == Dx12Fsr4ReferenceDescriptorSpace::Cbv) {
            sawStrictMiddleConstants = true;
        }
    }
    assert(sawSrvInputColor);
    assert(sawUavScratch);
    assert(sawCbvPassWeights);
    assert(!sawStrictMiddleConstants);
    assert(middleScratchBindingCount == 12);
    assert(middleInitializerBindingCount == 4);

    auto runtime = createDx12Fsr4ReferenceRuntime();
    assert(runtime != nullptr);
    assert(runtime->initialize(create).succeeded());
    Dx12Fsr4ReferenceBackendPlan runtimePlan{};
    assert(runtime->planFrame(frame, runtimePlan).succeeded());
    assert(runtimePlan.availableShaderBlobCount == (hasEmbeddedProductTable ? runtimePlan.shaders.size() : 0u));
    assert(runtimePlan.missingShaderBlobCount == (hasEmbeddedProductTable ? 0u : runtimePlan.shaders.size()));

    // The runtime keeps the expensive structural plan resident between frames.
    // Per-frame values must still exactly track a freshly constructed plan.
    Dx12Fsr4ReferenceFrameDesc dynamicFrame = frame;
    dynamicFrame.runner.frame.frameIndex = frame.runner.frame.frameIndex + 1u;
    dynamicFrame.runner.frame.jitter = Jitter{.x = -0.375f, .y = 0.1875f};
    dynamicFrame.runner.previousJitter[0] = 0.125f;
    dynamicFrame.runner.previousJitter[1] = -0.25f;
    dynamicFrame.runner.frame.options.preExposure = 1.25f;
    dynamicFrame.runner.previousPreExposure = 0.75f;
    dynamicFrame.runner.frame.options.sharpness = 0.62f;
    dynamicFrame.runner.forceReset = true;
    // Cache hits refresh borrowed handles and missing-resource accounting in
    // place; they must not retain the handles from the frame that built the
    // structural plan. These are opaque planner-only sentinels on this path.
    dynamicFrame.externalResources.inputColor = NativeHandle{reinterpret_cast<void*>(0x1000)};
    dynamicFrame.externalResources.inputDepth = NativeHandle{reinterpret_cast<void*>(0x2000)};
    dynamicFrame.externalResources.inputMotionVectors = NativeHandle{reinterpret_cast<void*>(0x3000)};
    dynamicFrame.externalResources.output = NativeHandle{reinterpret_cast<void*>(0x4000)};
    Dx12Fsr4ReferenceBackendPlan dynamicExpected{};
    Dx12Fsr4ReferenceBackendPlan dynamicRuntime{};
    assert(makeDx12Fsr4ReferenceBackendPlan(create, dynamicFrame, dynamicExpected).succeeded());
    assert(runtime->planFrame(dynamicFrame, dynamicRuntime).succeeded());
    // The 256-byte constant-buffer alignment includes unused tail padding.
    // Compare the shader-visible payload, not the padding object representation.
    constexpr std::size_t optimizedPayloadBytes =
        offsetof(OptimizedConstants, useDefaultExposure) + sizeof(std::uint32_t);
    constexpr std::size_t spdPayloadBytes =
        offsetof(AutoExposureSpdConstants, padding) + sizeof(float);
    constexpr std::size_t rcasPayloadBytes =
        offsetof(RcasConstants, outputHeight) + sizeof(std::uint32_t);
    static_assert(optimizedPayloadBytes == 104);
    static_assert(spdPayloadBytes == 32);
    static_assert(rcasPayloadBytes == 32);
    assert(std::memcmp(&dynamicRuntime.framePlan.constants, &dynamicExpected.framePlan.constants, optimizedPayloadBytes) == 0);
    assert(std::memcmp(&dynamicRuntime.framePlan.spdConstants, &dynamicExpected.framePlan.spdConstants, spdPayloadBytes) == 0);
    assert(std::memcmp(&dynamicRuntime.framePlan.rcasConstants, &dynamicExpected.framePlan.rcasConstants, rcasPayloadBytes) == 0);
    assert(dynamicRuntime.framePlan.dispatches.size() == dynamicExpected.framePlan.dispatches.size());
    assert(dynamicRuntime.descriptorPatchPlan.size() == dynamicExpected.descriptorPatchPlan.size());
    for (std::size_t i = 0; i < dynamicRuntime.descriptorPatchPlan.size(); ++i) {
        assert(dynamicRuntime.descriptorPatchPlan[i].resolvedResource == dynamicExpected.descriptorPatchPlan[i].resolvedResource);
    }
    assert(dynamicRuntime.resourceRealizationPlan.missingRequiredExternalCount ==
        dynamicExpected.resourceRealizationPlan.missingRequiredExternalCount);
    assert(dynamicRuntime.resourceRealizationPlan.missingOptionalExternalCount ==
        dynamicExpected.resourceRealizationPlan.missingOptionalExternalCount);
    assert(dynamicRuntime.descriptorRealizationPlan.missingRequiredResourceDescriptorCount ==
        dynamicExpected.descriptorRealizationPlan.missingRequiredResourceDescriptorCount);
    assert(dynamicRuntime.resourceRealizationPlan.resources.size() == dynamicExpected.resourceRealizationPlan.resources.size());
    for (std::size_t i = 0; i < dynamicRuntime.resourceRealizationPlan.resources.size(); ++i) {
        const Dx12Fsr4ReferenceRealizedResourceDesc& actual = dynamicRuntime.resourceRealizationPlan.resources[i];
        const Dx12Fsr4ReferenceRealizedResourceDesc& expected = dynamicExpected.resourceRealizationPlan.resources[i];
        assert(actual.id == expected.id);
        assert(actual.resolvedResource == expected.resolvedResource);
        assert(actual.ownership == expected.ownership);
        assert(actual.requiredForSelectedFrame == expected.requiredForSelectedFrame);
        assert(actual.validHandle == expected.validHandle);
        assert(actual.nativeResource.value == expected.nativeResource.value);
    }

    // Exposure source changes alter descriptor resolution and therefore must
    // invalidate the structural cache rather than retaining the default-exposure alias.
    Dx12Fsr4ReferenceFrameDesc exposureFrame = dynamicFrame;
    exposureFrame.runner.frame.options.hasExposure = true;
    Dx12Fsr4ReferenceBackendPlan exposureExpected{};
    Dx12Fsr4ReferenceBackendPlan exposureRuntime{};
    assert(makeDx12Fsr4ReferenceBackendPlan(create, exposureFrame, exposureExpected).succeeded());
    assert(runtime->planFrame(exposureFrame, exposureRuntime).succeeded());
    assert(exposureRuntime.descriptorPatchPlan.size() == exposureExpected.descriptorPatchPlan.size());
    for (std::size_t i = 0; i < exposureRuntime.descriptorPatchPlan.size(); ++i) {
        assert(exposureRuntime.descriptorPatchPlan[i].resolvedResource == exposureExpected.descriptorPatchPlan[i].resolvedResource);
    }

    // Cache hits must preserve the public planner's input-validation contract.
    // A zero display size would otherwise normalize to maxUpscaleSize and could
    // accidentally alias the preceding valid cache key.
    Dx12Fsr4ReferenceFrameDesc invalidCachedFrame = exposureFrame;
    invalidCachedFrame.runner.frame.displaySize = DisplaySize{};
    Dx12Fsr4ReferenceBackendPlan invalidCachedPlan{};
    assert(!runtime->planFrame(invalidCachedFrame, invalidCachedPlan).succeeded());

    // Crossing the SPD 64-pixel dispatch boundary changes structural geometry
    // and must rebuild rather than retaining the prior dispatch grid.
    Dx12Fsr4ReferenceFrameDesc resizedFrame = exposureFrame;
    resizedFrame.runner.frame.renderSize.width = 1664u;
    resizedFrame.runner.frame.options.motionVectorScale.x = -static_cast<float>(resizedFrame.runner.frame.renderSize.width);
    Dx12Fsr4ReferenceBackendPlan resizedExpected{};
    Dx12Fsr4ReferenceBackendPlan resizedRuntime{};
    assert(makeDx12Fsr4ReferenceBackendPlan(create, resizedFrame, resizedExpected).succeeded());
    assert(runtime->planFrame(resizedFrame, resizedRuntime).succeeded());
    assert(resizedRuntime.framePlan.dispatches.size() == resizedExpected.framePlan.dispatches.size());
    for (std::size_t i = 0; i < resizedRuntime.framePlan.dispatches.size(); ++i) {
        assert(resizedRuntime.framePlan.dispatches[i].grid.groupsX == resizedExpected.framePlan.dispatches[i].grid.groupsX);
        assert(resizedRuntime.framePlan.dispatches[i].grid.groupsY == resizedExpected.framePlan.dispatches[i].grid.groupsY);
        assert(resizedRuntime.framePlan.dispatches[i].grid.groupsZ == resizedExpected.framePlan.dispatches[i].grid.groupsZ);
    }

    Dx12Fsr4ReferenceCreateDesc requiredPackagedPass1Create = create;
    requiredPackagedPass1Create.shaderProvider.packagedCacheRoot =
        (std::filesystem::temp_directory_path() / "apusr_missing_required_pass1_cache").generic_string();
    requiredPackagedPass1Create.shaderProvider.requiredPackagedDispatches.push_back("fsr4_pass_1");
    Dx12Fsr4ReferenceBackendPlan requiredPackagedPass1Plan{};
    assert(makeDx12Fsr4ReferenceBackendPlan(requiredPackagedPass1Create, frame, requiredPackagedPass1Plan).succeeded());
    assert(requiredPackagedPass1Plan.shaderProvider.missingShaderBlobCount == (hasEmbeddedProductTable ? 1u : runtimePlan.shaders.size()));
    assert(requiredPackagedPass1Plan.shaderProvider.embeddedShaderBlobCount == (hasEmbeddedProductTable ? runtimePlan.shaders.size() - 1u : 0u));
    bool sawRequiredPass1Miss = false;
    for (const Dx12Fsr4ReferenceShaderResolutionDesc& shader : requiredPackagedPass1Plan.shaderProvider.shaders) {
        if (shader.dispatchName != nullptr && std::string(shader.dispatchName) == "fsr4_pass_1") {
            sawRequiredPass1Miss = shader.requiredPackaged && !shader.available &&
                shader.provider == Dx12Fsr4ReferenceShaderProviderKind::PackagedCache;
        }
    }
    assert(sawRequiredPass1Miss);

    Dx12Fsr4ReferenceFrameDesc nonDefaultFrame = frame;
    nonDefaultFrame.runner.frame.options.features.colorSpacePq = true;
    Dx12Fsr4ReferenceBackendPlan nonDefaultPlan{};
    assert(makeDx12Fsr4ReferenceBackendPlan(create, nonDefaultFrame, nonDefaultPlan).succeeded());
    assert(nonDefaultPlan.framePlan.permutationKey != plan.framePlan.permutationKey);
    assert(nonDefaultPlan.availableShaderBlobCount == (hasEmbeddedProductTable ? nonDefaultPlan.shaders.size() : 0u));
    assert(nonDefaultPlan.missingShaderBlobCount == (hasEmbeddedProductTable ? 0u : nonDefaultPlan.shaders.size()));
    assert(nonDefaultPlan.shaderProvider.missingShaderBlobCount == (hasEmbeddedProductTable ? 0u : nonDefaultPlan.shaders.size()));
    const Status nonDefaultRecordStatus = runtime->recordFrame(nonDefaultFrame);
    assert(nonDefaultRecordStatus.code == (hasEmbeddedProductTable ? StatusCode::RuntimeFailure : StatusCode::InvalidArgument));

    Dx12Fsr4ReferenceCreateDesc noCacheCreate = create;
    noCacheCreate.shaderProvider.enableEmbeddedCache = false;
    noCacheCreate.shaderProvider.enablePackagedCache = false;
    Dx12Fsr4ReferenceBackendPlan noCachePlan{};
    assert(makeDx12Fsr4ReferenceBackendPlan(noCacheCreate, frame, noCachePlan).succeeded());
    assert(noCachePlan.availableShaderBlobCount == 0);
    assert(noCachePlan.missingShaderBlobCount == noCachePlan.shaders.size());

    Dx12Fsr4ReferenceCreateDesc devCompileCreate = noCacheCreate;
    devCompileCreate.shaderProvider.allowRuntimeCompilation = true;
    devCompileCreate.shaderProvider.requireCompletePrecompiledCache = false;
    Dx12Fsr4ReferenceBackendPlan devCompilePlan{};
    assert(makeDx12Fsr4ReferenceBackendPlan(devCompileCreate, frame, devCompilePlan).succeeded());
    assert(devCompilePlan.availableShaderBlobCount == 0);
    assert(devCompilePlan.shaderProvider.runtimeCompileCandidateCount == devCompilePlan.shaders.size());
    assert(devCompilePlan.missingShaderBlobCount == devCompilePlan.shaders.size());

    const std::filesystem::path packagedRoot = std::filesystem::temp_directory_path() / "apusr_fsr4_reference_shader_cache_smoke";
    std::filesystem::remove_all(packagedRoot);
    const std::filesystem::path prePath = packagedRoot / "balanced" / "2160" / "p458" / "fsr4_pre.dxil";
    std::filesystem::create_directories(prePath.parent_path());
    {
        const unsigned char fakeDxil[] = {'D', 'X', 'B', 'C'};
        std::ofstream out(prePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(fakeDxil), static_cast<std::streamsize>(sizeof(fakeDxil)));
    }
    Dx12Fsr4ReferenceCreateDesc packagedOnlyCreate = create;
    packagedOnlyCreate.shaderProvider.enableEmbeddedCache = false;
    packagedOnlyCreate.shaderProvider.packagedCacheRoot = packagedRoot.generic_string();
    Dx12Fsr4ReferenceBackendPlan packagedOnlyPlan{};
    assert(makeDx12Fsr4ReferenceBackendPlan(packagedOnlyCreate, frame, packagedOnlyPlan).succeeded());
    assert(packagedOnlyPlan.shaderProvider.packagedShaderBlobCount == 1);
    assert(packagedOnlyPlan.shaderProvider.embeddedShaderBlobCount == 0);
    assert(packagedOnlyPlan.shaderProvider.missingShaderBlobCount == packagedOnlyPlan.shaders.size() - 1u);
    std::filesystem::remove_all(packagedRoot);

    const Status recordStatus = runtime->recordFrame(frame);
    assert(recordStatus.code == (hasEmbeddedProductTable ? StatusCode::RuntimeFailure : StatusCode::InvalidArgument));
    if (hasEmbeddedProductTable) {
        assert(recordStatus.message.find("requires a native command list") != std::string::npos);
    }

    std::cout << "apusr_fsr4_reference_dx12_plan_smoke ok shaders=" << plan.shaders.size()
              << " srv=" << plan.srvCount
              << " uav=" << plan.uavCount
              << " cbv=" << plan.cbvCount
              << " rootParams=" << plan.rootSignature.rootParameters.size()
              << " heapDescriptors=" << plan.rootSignature.descriptorHeap.totalCbvSrvUavDescriptors
              << " shaderBlobs=" << plan.availableShaderBlobCount
              << " embeddedProductTable=" << (hasEmbeddedProductTable ? 1 : 0)
              << " initializerBytes=" << plan.initializerBytes << "\n";
    return 0;
}
