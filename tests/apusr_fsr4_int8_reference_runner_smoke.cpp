// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#undef NDEBUG

#include <apusr/models/apusr_fsr4_int8_reference_runner.h>
#include <apusr/generated/apusr_fsr4_reference_pass0_weights.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

namespace {

bool hasDefine(const std::vector<std::string>& defines, const char* expected)
{
    for (const std::string& define : defines) {
        if (define == expected) {
            return true;
        }
    }
    return false;
}

const apusr::models::fsr4_int8::ReferenceDispatchDesc* findDispatch(
    const apusr::models::fsr4_int8::ReferenceFramePlan& plan,
    const apusr::models::fsr4_int8::PassKind kind,
    const std::uint32_t logicalStage = UINT32_MAX)
{
    for (const auto& dispatch : plan.dispatches) {
        if (dispatch.kind == kind && (logicalStage == UINT32_MAX || dispatch.logicalStage == logicalStage)) {
            return &dispatch;
        }
    }
    return nullptr;
}

const apusr::models::fsr4_int8::ReferenceResourceBindingDesc* findBinding(
    const apusr::models::fsr4_int8::ReferenceDispatchDesc& dispatch,
    const apusr::models::fsr4_int8::ReferenceBindingClass bindingClass,
    const std::uint32_t shaderRegister,
    const apusr::models::fsr4_int8::ResourceId resource = apusr::models::fsr4_int8::ResourceId::Null,
    const apusr::models::fsr4_int8::ReferenceConstantBufferRole constantBuffer = apusr::models::fsr4_int8::ReferenceConstantBufferRole::None)
{
    for (const auto& binding : dispatch.bindings) {
        if (binding.bindingClass == bindingClass && binding.shaderRegister == shaderRegister &&
            (resource == apusr::models::fsr4_int8::ResourceId::Null || binding.resource == resource) &&
            (constantBuffer == apusr::models::fsr4_int8::ReferenceConstantBufferRole::None || binding.constantBuffer == constantBuffer)) {
            return &binding;
        }
    }
    return nullptr;
}

std::size_t bindingCount(
    const apusr::models::fsr4_int8::ReferenceDispatchDesc& dispatch,
    const apusr::models::fsr4_int8::ReferenceBindingClass bindingClass)
{
    return static_cast<std::size_t>(std::count_if(
        dispatch.bindings.begin(), dispatch.bindings.end(),
        [bindingClass](const auto& binding) { return binding.bindingClass == bindingClass; }));
}

} // namespace

int main()
{
    using namespace apusr;
    using namespace apusr::models::fsr4_int8;

    ReferenceRunnerCreateDesc create{};
    create.maxRenderSize = RenderSize{.width = 2560, .height = 1440};
    create.maxUpscaleSize = DisplaySize{.width = 3440, .height = 1440};
    create.features.displayResolutionMotionVectors = false;
    create.features.motionVectorJitterCancellation = true;
    create.features.depthInverted = true;
    create.features.autoExposure = true;
    create.initialPreset = Preset::Balanced;

    ReferenceRunnerFrameDesc frame{};
    frame.frame.frameIndex = 42;
    frame.frame.renderSize = RenderSize{.width = 2024, .height = 847};
    frame.frame.displaySize = DisplaySize{.width = 3440, .height = 1440};
    frame.frame.jitter = Jitter{.x = 0.25f, .y = -0.125f};
    frame.frame.options.motionVectorScale = MotionVectorScale{.x = -2024.0f, .y = 847.0f};
    frame.frame.options.features = create.features;
    frame.frame.options.hasDepth = true;
    frame.frame.options.hasExposure = false;
    frame.frame.options.preExposure = 1.0f;
    frame.frame.options.sharpeningRequested = true;
    frame.frame.options.sharpness = 0.35f;
    frame.residentModelPreset = Preset::Balanced;
    frame.previousJitter[0] = -0.25f;
    frame.previousJitter[1] = 0.125f;

    const ReferenceFramePlan plan = makeReferenceFramePlan(create, frame);
    assert(plan.pipelineVariant == PipelineVariant::StrictReference);
    assert(plan.alignedUpscaleSize.width == 3440);
    assert(plan.alignedUpscaleSize.height == 1440);
    assert(plan.preset == Preset::Balanced);
    assert(plan.frameCapacity == FrameCapacity::R2160);
    assert(plan.permutation.lowResMotionVectors);
    assert(plan.permutation.depthInverted);
    assert(plan.permutation.autoExposure);
    assert(plan.constants.width == 3440);
    assert(plan.constants.height == 1440);
    assert(plan.constants.widthLowRes == 2024);
    assert(plan.constants.heightLowRes == 847);
    assert(plan.constants.rcasEnabled == 1);
    assert(plan.constants.useDefaultExposure == 1);
    assert(plan.spdConstants.mips == 10);
    assert(plan.spdConstants.numWorkGroups == 32 * 14);
    assert(plan.rcasConstants.config[0] == 0x3ecfefc6u);
    assert(plan.rcasConstants.config[1] == 0x367f367fu);
    assert(plan.rcasConstants.config[2] == 0u);
    assert(plan.rcasConstants.config[3] == 0u);
    assert(plan.rcasConstants.outputWidth == 3440u);
    assert(plan.rcasConstants.outputHeight == 1440u);
    assert(!plan.dispatches.empty());

    bool sawSpd = false;
    bool sawPre = false;
    bool sawPost = false;
    bool sawRcas = false;
    std::uint32_t middlePassCount = 0;
    for (const ReferenceDispatchDesc& dispatch : plan.dispatches) {
        if (dispatch.kind == PassKind::SpdAutoExposure) {
            sawSpd = true;
            assert(findBinding(dispatch, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::SpdAutoExposure) != nullptr);
            assert(dispatch.grid.groupsX == 32);
            assert(dispatch.grid.groupsY == 14);
        }
        if (dispatch.kind == PassKind::ModelPre) {
            sawPre = true;
            assert(bindingCount(dispatch, ReferenceBindingClass::ConstantBuffer) == 2);
            assert(findBinding(dispatch, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::Fsr4Upscaler) != nullptr);
            assert(findBinding(dispatch, ReferenceBindingClass::ConstantBuffer, 1, ResourceId::Null, ReferenceConstantBufferRole::PassWeights) != nullptr);
            assert(dispatch.shader.entryPoint == "main");
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_DEPTH_INVERTED=1"));
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_LOW_RES_MV=1"));
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_AUTOEXPOSURE_ENABLED=1"));
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_JITTERED_MOTION_VECTORS=1"));
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_COLORSPACE=0"));
            assert(dispatch.grid.groupsX == 215);
            assert(dispatch.grid.groupsY == 90);
        }
        if (dispatch.kind == PassKind::ModelMiddle) {
            ++middlePassCount;
            assert(dispatch.shader.entryPoint == std::string("fsr4_model_v07_i8_pass") + std::to_string(dispatch.logicalStage));
        }
        if (dispatch.kind == PassKind::ModelPost) {
            sawPost = true;
            assert(dispatch.shader.entryPoint == "main");
            assert(dispatch.grid.groupsX == 215);
            assert(dispatch.grid.groupsY == 90);
        }
        if (dispatch.kind == PassKind::Rcas) {
            sawRcas = true;
            assert(findBinding(dispatch, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::Rcas) != nullptr);
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_AUTOEXPOSURE_ENABLED=1"));
            assert(hasDefine(dispatch.shader.defines, "FFX_MLSR_COLORSPACE=0"));
        }
    }
    assert(sawSpd);
    assert(sawPre);
    assert(sawPost);
    assert(sawRcas);
    assert(middlePassCount == 12);

    ReferenceRunnerFrameDesc edgeFrame = frame;
    edgeFrame.frame.renderSize = RenderSize{.width = 2560, .height = 1440};
    edgeFrame.frame.displaySize = DisplaySize{.width = 3833, .height = 2160};
    edgeFrame.frame.options.resetHistory = false;
    edgeFrame.forceReset = true;
    edgeFrame.residentModelPreset = Preset::Performance;
    edgeFrame.frame.options.qualityMode = UpscalerQualityMode::Performance;
    edgeFrame.frame.options.motionVectorScale = MotionVectorScale{.x = 3833.0f, .y = 2160.0f};
    edgeFrame.previousJitter[0] = edgeFrame.frame.jitter.x;
    edgeFrame.previousJitter[1] = edgeFrame.frame.jitter.y;
    ReferenceRunnerCreateDesc highResMvCreate = create;
    highResMvCreate.features.displayResolutionMotionVectors = true;
    highResMvCreate.features.motionVectorJitterCancellation = true;
    ReferenceFramePlan edgePlan = makeReferenceFramePlan(highResMvCreate, edgeFrame);
    assert(edgePlan.alignedUpscaleSize.width == 3840);
    assert(edgePlan.preset == Preset::Performance);
    assert(edgePlan.constants.reset == 1);
    assert(edgePlan.constants.motionVectorScale[0] == 1.0f);
    assert(edgePlan.constants.motionVectorScale[1] == 1.0f);

    ReferenceRunnerFrameDesc roundedQualityFrame = frame;
    roundedQualityFrame.frame.renderSize = RenderSize{.width = 1707, .height = 960};
    roundedQualityFrame.frame.displaySize = DisplaySize{.width = 2560, .height = 1440};
    roundedQualityFrame.frame.options.qualityMode = UpscalerQualityMode::Auto;
    roundedQualityFrame.residentModelPreset = Preset::NativeAA;
    assert(makeReferenceFramePlan(create, roundedQualityFrame).preset == Preset::Quality);

    // The graph itself is now the binding contract: verify the exact shader
    // interfaces that previously got blurred together by stage-level scopes.
    const ReferenceDispatchDesc* preDispatch = findDispatch(plan, PassKind::ModelPre);
    const ReferenceDispatchDesc* postDispatch = findDispatch(plan, PassKind::ModelPost);
    const ReferenceDispatchDesc* spdDispatch = findDispatch(plan, PassKind::SpdAutoExposure);
    assert(preDispatch != nullptr);
    assert(postDispatch != nullptr);
    assert(spdDispatch != nullptr);
    assert(preDispatch->bindings.size() == 11);
    assert(findBinding(*preDispatch, ReferenceBindingClass::SrvTexture, 3, ResourceId::InputColor) != nullptr);
    assert(findBinding(*preDispatch, ReferenceBindingClass::SrvTexture, 2, ResourceId::InputDepth) != nullptr);
    assert(findBinding(*preDispatch, ReferenceBindingClass::SrvTexture, 17, ResourceId::AutoExposure) != nullptr);
    assert(findBinding(*preDispatch, ReferenceBindingClass::SrvTexture, 6, ResourceId::InputExposure) == nullptr);
    const ReferenceResourceBindingDesc* preScratch = findBinding(
        *preDispatch, ReferenceBindingClass::UavBuffer, 11, ResourceId::ScratchBuffer);
    assert(preScratch != nullptr);
    assert(preScratch->access == ReferenceResourceAccess::Write);
    assert(findBinding(*preDispatch, ReferenceBindingClass::ConstantBuffer, 1, ResourceId::Null, ReferenceConstantBufferRole::PassWeights) != nullptr);

    for (std::uint32_t stage = 1; stage <= 12; ++stage) {
        const ReferenceDispatchDesc* middle = findDispatch(plan, PassKind::ModelMiddle, stage);
        assert(middle != nullptr);
        const ReferenceResourceBindingDesc* scratch = findBinding(
            *middle, ReferenceBindingClass::UavBuffer, 1, ResourceId::ScratchBuffer);
        assert(scratch != nullptr);
        assert(scratch->access == ReferenceResourceAccess::ReadWrite);
        const bool shouldHaveInitializers = stage >= 6 && stage <= 9;
        assert((findBinding(*middle, ReferenceBindingClass::SrvBuffer, 1, ResourceId::InitializerBuffer) != nullptr) == shouldHaveInitializers);
        assert(findBinding(*middle, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::Fsr4Upscaler) == nullptr);
        assert(middle->bindings.size() == (shouldHaveInitializers ? 2u : 1u));
    }

    const ReferenceResourceBindingDesc* postScratch = findBinding(
        *postDispatch, ReferenceBindingClass::UavBuffer, 11, ResourceId::ScratchBuffer);
    assert(postScratch != nullptr);
    assert(postScratch->access == ReferenceResourceAccess::Read);
    assert(findBinding(*postDispatch, ReferenceBindingClass::UavTexture, 12, ResourceId::DebugInformation) == nullptr);

    assert(findBinding(*spdDispatch, ReferenceBindingClass::UavTexture, 0, ResourceId::SpdAtomicCount)->access == ReferenceResourceAccess::ReadWrite);
    assert(findBinding(*spdDispatch, ReferenceBindingClass::UavTexture, 1, ResourceId::SpdMip5)->access == ReferenceResourceAccess::ReadWrite);
    assert(findBinding(*spdDispatch, ReferenceBindingClass::UavTexture, 2, ResourceId::AutoExposure)->access == ReferenceResourceAccess::ReadWrite);

    ReferenceRunnerCreateDesc noAutoBindingCreate = create;
    noAutoBindingCreate.features.autoExposure = false;
    ReferenceRunnerFrameDesc noAutoBindingFrame = frame;
    noAutoBindingFrame.frame.options.features = noAutoBindingCreate.features;
    const ReferenceFramePlan noAutoBindingPlan = makeReferenceFramePlan(noAutoBindingCreate, noAutoBindingFrame);
    const ReferenceDispatchDesc* noAutoPre = findDispatch(noAutoBindingPlan, PassKind::ModelPre);
    assert(noAutoPre != nullptr);
    assert(findBinding(*noAutoPre, ReferenceBindingClass::SrvTexture, 6, ResourceId::InputExposure) != nullptr);
    assert(findBinding(*noAutoPre, ReferenceBindingClass::SrvTexture, 17, ResourceId::AutoExposure) == nullptr);

    ReferenceRunnerCreateDesc debugCreate = create;
    debugCreate.allowDebugVisualization = true;
    ReferenceRunnerFrameDesc nonDebugFrame = frame;
    nonDebugFrame.frame.options.features.debugVisualization = false;
    const ReferenceFramePlan nonDebugPlan = makeReferenceFramePlan(debugCreate, nonDebugFrame);
    assert(!nonDebugPlan.permutation.debugVisualize);
    assert(findDispatch(nonDebugPlan, PassKind::DebugView) == nullptr);
    const ReferenceDispatchDesc* nonDebugPost = findDispatch(nonDebugPlan, PassKind::ModelPost);
    assert(nonDebugPost != nullptr);
    assert(findBinding(*nonDebugPost, ReferenceBindingClass::UavTexture, 12, ResourceId::DebugInformation) == nullptr);

    ReferenceRunnerFrameDesc debugFrame = frame;
    debugFrame.frame.options.features.debugVisualization = true;
    const ReferenceFramePlan debugPlan = makeReferenceFramePlan(debugCreate, debugFrame);
    assert(debugPlan.permutation.debugVisualize);
    const ReferenceDispatchDesc* debugPost = findDispatch(debugPlan, PassKind::ModelPost);
    const ReferenceDispatchDesc* debugView = findDispatch(debugPlan, PassKind::DebugView);
    assert(debugPost != nullptr);
    assert(debugView != nullptr);
    assert(findBinding(*debugPost, ReferenceBindingClass::UavTexture, 12, ResourceId::DebugInformation) != nullptr);
    assert(findBinding(*debugView, ReferenceBindingClass::SrvTexture, 19, ResourceId::DebugInformation) != nullptr);
    assert(findBinding(*debugView, ReferenceBindingClass::SrvTexture, 20, ResourceId::ResultColor) != nullptr);
    assert(findBinding(*debugView, ReferenceBindingClass::UavTexture, 2, ResourceId::MlsrOutput) != nullptr);
    assert(findBinding(*debugView, ReferenceBindingClass::UavTexture, 11, ResourceId::RcasOutput) != nullptr);
    assert(findBinding(*debugView, ReferenceBindingClass::UavTexture, 12, ResourceId::DebugInformation) == nullptr);

    const embedded_pass0_weights::Pass0WeightsBlob* balancedWeights = embedded_pass0_weights::find(Preset::Balanced);
    assert(balancedWeights != nullptr);
    assert(balancedWeights->available());
    assert(balancedWeights->dwords[0] == 0x283cadb9u);
    std::vector<std::uint8_t> initializerBytes;
    const Status initializerStatus = loadReferenceInitializerBytes(APUSR_REPO_ROOT, Preset::Balanced, initializerBytes);
    assert(initializerStatus.succeeded());
    assert(initializerBytes.size() >= sizeof(std::uint32_t));
    const std::uint32_t firstInitializerDword =
        static_cast<std::uint32_t>(initializerBytes[0]) |
        (static_cast<std::uint32_t>(initializerBytes[1]) << 8u) |
        (static_cast<std::uint32_t>(initializerBytes[2]) << 16u) |
        (static_cast<std::uint32_t>(initializerBytes[3]) << 24u);
    assert(firstInitializerDword != balancedWeights->dwords[0]);

    ReferenceRunnerCreateDesc tunedCreate = create;
    tunedCreate.pipelineVariant = PipelineVariant::Dp4aTunedCandidate;
    const ReferenceFramePlan tunedPlan = makeReferenceFramePlan(tunedCreate, frame);
    assert(tunedPlan.pipelineVariant == PipelineVariant::Dp4aTunedCandidate);
    assert(tunedPlan.dispatches.size() == plan.dispatches.size());
    assert(tunedPlan.permutationKey == plan.permutationKey);
    assert(tunedPlan.dispatches.front().shader.shaderPath == plan.dispatches.front().shader.shaderPath);

    ReferenceRunnerCreateDesc optimizedCreate = create;
    optimizedCreate.pipelineVariant = PipelineVariant::Dp4aOptimized;
    const ReferenceFramePlan optimizedPlan = makeReferenceFramePlan(optimizedCreate, frame);
    assert(optimizedPlan.pipelineVariant == PipelineVariant::Dp4aOptimized);
    assert(optimizedPlan.dispatches.size() == plan.dispatches.size());
    assert(optimizedPlan.permutationKey == plan.permutationKey);
    bool sawOptimizedStage1 = false;
    bool sawOptimizedStage2 = false;
    for (const ReferenceDispatchDesc& dispatch : optimizedPlan.dispatches) {
        if (dispatch.kind == PassKind::ModelMiddle && dispatch.logicalStage == 1) {
            sawOptimizedStage1 = true;
            assert(dispatch.grid.groupsX == 27);
            assert(dispatch.grid.groupsY == 360);
        }
        if (dispatch.kind == PassKind::ModelMiddle && dispatch.logicalStage == 2) {
            sawOptimizedStage2 = true;
            assert(dispatch.grid.groupsX == 27);
            assert(dispatch.grid.groupsY == 360);
        }
    }
    assert(sawOptimizedStage1);
    assert(sawOptimizedStage2);
    const ReferenceDispatchDesc* optimizedStage1 = findDispatch(optimizedPlan, PassKind::ModelMiddle, 1);
    const ReferenceDispatchDesc* optimizedStage2 = findDispatch(optimizedPlan, PassKind::ModelMiddle, 2);
    const ReferenceDispatchDesc* optimizedStage3 = findDispatch(optimizedPlan, PassKind::ModelMiddle, 3);
    assert(optimizedStage1 != nullptr && optimizedStage2 != nullptr && optimizedStage3 != nullptr);
    assert(findBinding(*optimizedStage1, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::Fsr4Upscaler) != nullptr);
    assert(findBinding(*optimizedStage2, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::Fsr4Upscaler) != nullptr);
    assert(findBinding(*optimizedStage3, ReferenceBindingClass::ConstantBuffer, 0, ResourceId::Null, ReferenceConstantBufferRole::Fsr4Upscaler) == nullptr);

    const std::vector<ReferenceResourceUsageDesc> usage = makeReferenceResourceUsagePlan(plan);
    assert(!usage.empty());
    bool sawPostMlsrWrite = false;
    for (const ReferenceResourceUsageDesc& item : usage) {
        if (item.resource == ResourceId::MlsrOutput && item.passKind == PassKind::ModelPost) {
            sawPostMlsrWrite = true;
            assert(item.access == ReferenceResourceAccess::Write);
        }
    }
    assert(sawPostMlsrWrite);

    std::vector<std::uint8_t> initializer;
    const Status status = loadReferenceInitializerBytes(APUSR_REPO_ROOT, plan.preset, initializer);
    assert(status.succeeded());
    assert(!initializer.empty());
    assert(initializer.size() <= maxInitializerByteSize());

    std::vector<std::uint8_t> embeddedInitializer;
    const Status embeddedStatus = loadReferenceInitializerBytes("", plan.preset, embeddedInitializer);
    assert(embeddedStatus.succeeded());
    assert(embeddedInitializer == initializer);

    std::cout << "apusr_fsr4_int8_reference_runner_smoke ok dispatches=" << plan.dispatches.size()
              << " initializerBytes=" << initializer.size()
              << " embeddedFallbackBytes=" << embeddedInitializer.size() << "\n";
    return 0;
}
