// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/adapters/apusr_ffx_fsr4_adapter.h>
#include <apusr/apusr_runtime.h>
#include <apusr/apusr_tokens.h>

#include <cstdint>
#include <iostream>

int main()
{
    const apusr::DisplaySize modelOutputSize{1920, 1080};
    const apusr::TokenShape boundaryShape = apusr::makeFsr4MiddleBoundaryShapeFromModelOutput(modelOutputSize);
    const std::uint64_t bytes = apusr::tokenSlabBytes(boundaryShape);

    if (boundaryShape.width != 960 || boundaryShape.height != 540 || boundaryShape.channels != 16) {
        std::cerr << "unexpected FSR4 middle boundary shape\n";
        return 1;
    }

    if (bytes == 0 || bytes % apusr::kTokenBytes != 0) {
        std::cerr << "unexpected token slab byte count\n";
        return 1;
    }

    const apusr::SegmentManifest manifest = apusr::adapters::makeFsr4MiddleCoreManifestForActiveOutput(
        modelOutputSize);
    if (manifest.firstLogicalStage != 1 || manifest.lastLogicalStage != 12) {
        std::cerr << "unexpected FSR4 middle segment\n";
        return 1;
    }
    if (manifest.inputBoundary.shape.layout != apusr::TokenLayout::RasterNhwcC16 ||
        manifest.outputBoundary.shape.layout != apusr::TokenLayout::CellM4x8C16) {
        std::cerr << "FSR4 middle boundary must use row-major C16 ingress and m4x8 C16 egress; the frame scheduler derives the zero-guarded ingress slot\n";
        return 1;
    }

    std::cout << "APUSR smoke: " << boundaryShape.width << "x" << boundaryShape.height
              << "x" << boundaryShape.channels << ", " << bytes << " bytes\n";
    return 0;
}
