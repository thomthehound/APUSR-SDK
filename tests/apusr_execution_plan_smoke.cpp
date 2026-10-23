// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_execution_plan.h>

#include <cassert>

int main()
{
    const auto passes = apusr::fsr4Int8ReferenceProviderPasses();
    assert(passes.size() == 14);
    assert(apusr::fsr4Int8ReferenceProviderPassCount() == 14);
    assert(passes.front().providerDispatchIndex == 0);
    assert(passes.front().kind == apusr::UpscalerPassKind::PreBookend);
    assert(passes.back().providerDispatchIndex == 13);
    assert(passes.back().kind == apusr::UpscalerPassKind::PostBookend);
    assert(passes.back().writesTemporalHistory);

    std::uint32_t npuIslandCandidateCount = 0;
    for (const apusr::UpscalerPassDesc& pass : passes) {
        if (pass.acceptsNpuIsland) {
            ++npuIslandCandidateCount;
            assert(pass.kind == apusr::UpscalerPassKind::ModelMiddle);
        }
    }
    assert(npuIslandCandidateCount == 12);

    const apusr::UpscalerPassDesc* pass9 = apusr::findFsr4Int8ReferenceProviderPass("pass9");
    assert(pass9 != nullptr);
    assert(pass9->logicalStageFirst == 9);
    assert(pass9->acceptsNpuIsland);
    assert(apusr::findFsr4Int8ReferenceProviderPass("pass0") == nullptr);

    apusr::UpscalerExecutionPlanDesc reference = apusr::makeFsr4Int8ReferencePlan(
        apusr::UpscalerFrontendPath::FidelityFxFsr4Dx12DropIn,
        apusr::UpscalerLatencyPolicy::FrameDelayed);
    assert(apusr::validateUpscalerExecutionPlanDesc(reference).succeeded());
    assert(reference.usesFrameSlotTransport);
    assert(apusr::upscalerExecutionPathIsGpuOnly(reference.executionPath));

    apusr::UpscalerExecutionPlanDesc tuned = apusr::makeFsr4Int8Dp4aTunedPlan(
        apusr::UpscalerFrontendPath::FidelityFxFsr4Dx12DropIn,
        apusr::UpscalerLatencyPolicy::FrameDelayed);
    assert(apusr::validateUpscalerExecutionPlanDesc(tuned).succeeded());
    assert(tuned.executionPath == apusr::UpscalerExecutionPath::GpuFsr4Int8Dp4aTuned);
    assert(apusr::upscalerExecutionPathIsGpuOnly(tuned.executionPath));
    assert(!tuned.allowNpuIslands);

    apusr::UpscalerExecutionPlanDesc ngxHybrid = apusr::makeFsr4NgxDlssHybridIslandPlan();
    assert(apusr::validateUpscalerExecutionPlanDesc(ngxHybrid).succeeded());
    assert(ngxHybrid.allowNpuIslands);
    assert(apusr::upscalerExecutionPathUsesNpu(ngxHybrid.executionPath));
    assert(apusr::upscalerLatencyPolicyIsInline(ngxHybrid.latencyPolicy));

    apusr::UpscalerExecutionPlanDesc invalidNgxHybrid = ngxHybrid;
    invalidNgxHybrid.latencyPolicy = apusr::UpscalerLatencyPolicy::FrameDelayed;
    assert(!apusr::validateUpscalerExecutionPlanDesc(invalidNgxHybrid).succeeded());

    apusr::UpscalerExecutionPlanDesc invalidGpu = reference;
    invalidGpu.allowNpuIslands = true;
    assert(!apusr::validateUpscalerExecutionPlanDesc(invalidGpu).succeeded());

    return 0;
}
