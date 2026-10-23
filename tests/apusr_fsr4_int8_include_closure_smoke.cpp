// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

namespace {

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool resolveInclude(
    const std::filesystem::path& includingFile,
    const std::vector<std::filesystem::path>& includeRoots,
    const std::string& include,
    std::filesystem::path& outPath)
{
    const std::filesystem::path local = includingFile.parent_path() / include;
    if (std::filesystem::exists(local)) {
        outPath = std::filesystem::weakly_canonical(local);
        return true;
    }
    for (const auto& root : includeRoots) {
        const std::filesystem::path candidate = root / include;
        if (std::filesystem::exists(candidate)) {
            outPath = std::filesystem::weakly_canonical(candidate);
            return true;
        }
    }
    return false;
}

void walkIncludes(
    const std::filesystem::path& path,
    const std::vector<std::filesystem::path>& includeRoots,
    std::set<std::filesystem::path>& visited)
{
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path);
    if (!visited.insert(canonical).second) {
        return;
    }
    const std::string text = readText(canonical);
    const std::regex includeRegex(R"(^\s*#\s*include\s+\"([^\"]+)\")");
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch match;
        if (!std::regex_search(line, match, includeRegex)) {
            continue;
        }
        const std::string include = match[1].str();
        // This smoke test verifies the DX12/HLSL closure. ffx_core.h also names
        // CPU/GLSL backend headers behind preprocessor guards that are not part
        // of APUSR's imported HLSL reference runner closure.
        if (include == "ffx_core_cpu.h" || include == "ffx_core_glsl.h") {
            continue;
        }
        std::filesystem::path included;
        assert(resolveInclude(canonical, includeRoots, include, included));
        walkIncludes(included, includeRoots, visited);
    }
}

} // namespace

int main()
{
    using namespace apusr::models::fsr4_int8;

    const std::filesystem::path repoRoot = APUSR_REPO_ROOT;
    const ReferenceArtifactPaths paths = referenceArtifactPaths(Preset::Balanced, FrameCapacity::R2160);
    std::vector<std::filesystem::path> includeRoots;
    includeRoots.reserve(paths.includeRoots.size());
    for (const std::string& includeRoot : paths.includeRoots) {
        includeRoots.push_back(repoRoot / includeRoot);
        assert(std::filesystem::exists(includeRoots.back()));
    }

    std::set<std::filesystem::path> visited;
    walkIncludes(repoRoot / paths.preShader, includeRoots, visited);
    walkIncludes(repoRoot / paths.passesShader, includeRoots, visited);
    walkIncludes(repoRoot / paths.postShader, includeRoots, visited);
    walkIncludes(repoRoot / paths.rcasShader, includeRoots, visited);
    walkIncludes(repoRoot / paths.autoExposureShader, includeRoots, visited);
    walkIncludes(repoRoot / paths.debugViewShader, includeRoots, visited);

    assert(visited.size() > 20);
    assert(std::filesystem::exists(repoRoot / paths.includeRoot / "ml2code_runtime/tensor_int8.hlsli"));
    assert(std::filesystem::exists(repoRoot / paths.includeRoot / "fsr4/pre_common.hlsli"));
    assert(std::filesystem::exists(repoRoot / paths.includeRoot / "AmdExtD3D/AmdExtD3DShaderIntrinsicsMatrixOps.hlsl"));
    return 0;
}
