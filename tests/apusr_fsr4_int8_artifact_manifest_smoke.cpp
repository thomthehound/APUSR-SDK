// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

namespace {

std::uint64_t fileSize(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    assert(!ec);
    return size;
}

void requireFile(const std::filesystem::path& root, const std::string& relativePath, const std::uint64_t minBytes)
{
    const std::filesystem::path path = root / relativePath;
    assert(std::filesystem::exists(path));
    assert(fileSize(path) >= minBytes);
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

int main()
{
    using namespace apusr::models::fsr4_int8;

    const std::filesystem::path repoRoot = APUSR_REPO_ROOT;
    const std::filesystem::path manifestPath = repoRoot / "models/fsr4_int8/reference/fsr4_4_0_2_int8/manifest.json";
    const std::string manifest = readText(manifestPath);
    assert(manifest.find("apusr.fsr4_int8_reference_artifacts.v1") != std::string::npos);
    assert(manifest.find("fsr4_model_v07_i8") != std::string::npos);
    assert(manifest.find("include") != std::string::npos);

    constexpr Preset presets[] = {
        Preset::NativeAA,
        Preset::Quality,
        Preset::Balanced,
        Preset::Performance,
        Preset::UltraPerformance,
        Preset::DynamicResolution,
    };
    constexpr FrameCapacity capacities[] = {
        FrameCapacity::R1080,
        FrameCapacity::R2160,
        FrameCapacity::R4320,
    };

    for (const Preset preset : presets) {
        for (const FrameCapacity capacity : capacities) {
            const ReferenceArtifactPaths paths = referenceArtifactPaths(preset, capacity);
            requireFile(repoRoot, paths.passesShader, 450000);
        }
        const ReferenceArtifactPaths paths = referenceArtifactPaths(preset, FrameCapacity::R2160);
        requireFile(repoRoot, paths.preShader, 7000);
        requireFile(repoRoot, paths.postShader, 40000);
        requireFile(repoRoot, paths.initializers, 80000);
    }

    const ReferenceArtifactPaths balanced = referenceArtifactPaths(Preset::Balanced, FrameCapacity::R2160);
    requireFile(repoRoot, balanced.autoExposureShader, 1000);
    requireFile(repoRoot, balanced.rcasShader, 1000);
    requireFile(repoRoot, balanced.debugViewShader, 1000);
    requireFile(repoRoot, balanced.includeRoot + "/ml2code_runtime/tensor_int8.hlsli", 1000);
    requireFile(repoRoot, balanced.includeRoot + "/ml2code_runtime/operators/int8_NHWC/Fused/FasterNetBlock.hlsli", 1000);
    requireFile(repoRoot, balanced.includeRoot + "/fsr4/pre_common.hlsli", 1000);
    requireFile(repoRoot, balanced.includeRoot + "/fsr4/post_common.hlsli", 1000);
    requireFile(repoRoot, balanced.includeRoot + "/AmdExtD3D/AmdExtD3DShaderIntrinsicsMatrixOps.hlsl", 1000);
    return 0;
}
