// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_pipeline.h>

#include <utility>

namespace apusr {

PipelineSession::PipelineSession() = default;
PipelineSession::~PipelineSession() = default;
PipelineSession::PipelineSession(PipelineSession&&) noexcept = default;
PipelineSession& PipelineSession::operator=(PipelineSession&&) noexcept = default;

Status PipelineSession::configure(RuntimeConfig config)
{
    return runtime_.configure(std::move(config));
}

Status PipelineSession::switchModelEpochQuiescent(const RuntimeModelEpochSwitchDesc& desc)
{
    return runtime_.switchModelEpochQuiescent(desc);
}

Status PipelineSession::submitFrame(const PipelineFrameDesc& frame)
{
    return runtime_.executeFrame(makeRuntimeFrameDesc(frame));
}

bool PipelineSession::configured() const
{
    return runtime_.configured();
}

bool PipelineSession::lastSubmitProducedOutput() const
{
    return runtime_.lastFrameProducedOutput();
}

const DiagnosticTileOverlayDesc& PipelineSession::lastDiagnosticTileOverlay() const
{
    return runtime_.lastDiagnosticTileOverlay();
}

NpuSubmissionStats PipelineSession::npuSubmissionStats() const
{
    return runtime_.npuSubmissionStats();
}

FrameDesc makeRuntimeFrameDesc(const PipelineFrameDesc& frame)
{
    return FrameDesc{
        .frameIndex = frame.frameIndex,
        .renderSize = frame.renderSize,
        .displaySize = frame.displaySize,
        .jitter = frame.jitter,
        .previousJitter = frame.previousJitter,
        .camera = frame.camera,
        .options = frame.options,
        .surfaces = frame.surfaces,
        .borrowedCommandList = frame.borrowedCommandList,
    };
}

} // namespace apusr
