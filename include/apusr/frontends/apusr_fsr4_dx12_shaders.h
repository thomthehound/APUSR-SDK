// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/backends/apusr_gfx_dx12.h>
#include <apusr/apusr_common.h>

#include <memory>

namespace apusr::frontends {

// Owns the native DX12 export/import shader state for the first FSR4-compatible
// frontend. The FFX ABI can reach these callbacks, but the implementation is
// APUSR-owned and records the GPU/NPU token boundary directly.
class Fsr4Dx12TokenShaderRecorder {
public:
    Fsr4Dx12TokenShaderRecorder();
    ~Fsr4Dx12TokenShaderRecorder();

    Fsr4Dx12TokenShaderRecorder(const Fsr4Dx12TokenShaderRecorder&) = delete;
    Fsr4Dx12TokenShaderRecorder& operator=(const Fsr4Dx12TokenShaderRecorder&) = delete;
    Fsr4Dx12TokenShaderRecorder(Fsr4Dx12TokenShaderRecorder&&) noexcept;
    Fsr4Dx12TokenShaderRecorder& operator=(Fsr4Dx12TokenShaderRecorder&&) noexcept;

    [[nodiscard]] Status initialize(NativeHandle device);
    [[nodiscard]] Status recordExport(backends::Dx12RecordContext& context, const ExportPassDesc& desc);
    [[nodiscard]] Status recordImport(backends::Dx12RecordContext& context, const ImportPassDesc& desc);

    [[nodiscard]] static Status recordExportCallback(
        backends::Dx12RecordContext& context,
        const ExportPassDesc& desc,
        void* userData);
    [[nodiscard]] static Status recordImportCallback(
        backends::Dx12RecordContext& context,
        const ImportPassDesc& desc,
        void* userData);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apusr::frontends
