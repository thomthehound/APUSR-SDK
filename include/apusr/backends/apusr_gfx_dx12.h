// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_graphics.h>

#include <memory>

namespace apusr::backends {

struct Dx12RecordContext {
    NativeHandle device;
    NativeHandle queue;
    NativeHandle commandList;
    NativeHandle commandAllocator;
    CommandPhase phase = CommandPhase::ExportTokens;
};

using Dx12RecordExportPassFn = Status (*)(Dx12RecordContext& context, const ExportPassDesc& desc, void* userData);
using Dx12RecordImportPassFn = Status (*)(Dx12RecordContext& context, const ImportPassDesc& desc, void* userData);

struct Dx12BackendConfig {
    NativeHandle device;
    NativeHandle queue;

    // APUSR owns scheduling and queue submission. The frontend supplies the actual
    // export/import shader recording so the graphics backend never pretends to know
    // the final model-boundary shader code.
    Dx12RecordExportPassFn recordExportPass = nullptr;
    Dx12RecordImportPassFn recordImportPass = nullptr;
    void* recordUserData = nullptr;

    bool requireExternalFenceInterop = true;
};

[[nodiscard]] GraphicsBackendPtr createDx12GraphicsBackend(const Dx12BackendConfig& config);

} // namespace apusr::backends
