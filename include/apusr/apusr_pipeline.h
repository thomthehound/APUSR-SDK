// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_runtime.h>

namespace apusr {

struct PipelineFrameDesc {
    std::uint64_t frameIndex = 0;
    RenderSize renderSize;
    DisplaySize displaySize;
    Jitter jitter;
    Jitter previousJitter;
    CameraState camera;
    UpscalerFrameOptions options;
    FrameSurfaces surfaces;
    NativeHandle borrowedCommandList;
};

class PipelineSession {
public:
    PipelineSession();
    ~PipelineSession();

    PipelineSession(const PipelineSession&) = delete;
    PipelineSession& operator=(const PipelineSession&) = delete;
    PipelineSession(PipelineSession&&) noexcept;
    PipelineSession& operator=(PipelineSession&&) noexcept;

    [[nodiscard]] Status configure(RuntimeConfig config);
    [[nodiscard]] Status switchModelEpochQuiescent(const RuntimeModelEpochSwitchDesc& desc);
    [[nodiscard]] Status submitFrame(const PipelineFrameDesc& frame);
    [[nodiscard]] bool configured() const;
    [[nodiscard]] bool lastSubmitProducedOutput() const;
    [[nodiscard]] const DiagnosticTileOverlayDesc& lastDiagnosticTileOverlay() const;
    [[nodiscard]] NpuSubmissionStats npuSubmissionStats() const;

private:
    RuntimeContext runtime_;
};

[[nodiscard]] FrameDesc makeRuntimeFrameDesc(const PipelineFrameDesc& frame);

} // namespace apusr
