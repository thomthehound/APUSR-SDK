// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_fsr4_reference_vulkan.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readBinary(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::vector<std::uint32_t> readSpirv(const apusr::backends::VulkanFsr4ReferenceShaderBlobView blob)
{
    if (blob.data == nullptr || blob.size < 20 || (blob.size & 3u) != 0u) {
        return {};
    }
    std::vector<std::uint32_t> words(blob.size / sizeof(std::uint32_t));
    std::memcpy(words.data(), blob.data, blob.size);
    return words;
}

std::string spirvString(const std::uint32_t* words, const std::size_t wordCount)
{
    const char* bytes = reinterpret_cast<const char*>(words);
    const std::size_t byteCount = wordCount * sizeof(std::uint32_t);
    const void* terminator = std::memchr(bytes, '\0', byteCount);
    const std::size_t length = terminator != nullptr
        ? static_cast<const char*>(terminator) - bytes
        : byteCount;
    return std::string(bytes, length);
}

enum class SpirvDescriptorKind : std::uint32_t {
    Sampler,
    SampledImage,
    StorageImage,
    UniformBuffer,
    StorageBuffer,
};

struct SpirvContract {
    std::map<std::uint32_t, SpirvDescriptorKind> bindings;
    std::set<std::uint32_t> descriptorSets;
    std::set<std::string> entryPoints;
    std::set<std::uint32_t> capabilities;
    std::set<std::string> extensions;
};

bool inspectSpirv(const apusr::backends::VulkanFsr4ReferenceShaderBlobView blob, SpirvContract& out)
{
    constexpr std::uint32_t kMagic = 0x07230203u;
    constexpr std::uint32_t kSpirv15 = 0x00010500u;
    constexpr std::uint16_t kOpExtension = 10u;
    constexpr std::uint16_t kOpEntryPoint = 15u;
    constexpr std::uint16_t kOpCapability = 17u;
    constexpr std::uint16_t kOpTypeImage = 25u;
    constexpr std::uint16_t kOpTypeSampler = 26u;
    constexpr std::uint16_t kOpTypeStruct = 30u;
    constexpr std::uint16_t kOpTypePointer = 32u;
    constexpr std::uint16_t kOpVariable = 59u;
    constexpr std::uint16_t kOpDecorate = 71u;
    constexpr std::uint32_t kBindingDecoration = 33u;
    constexpr std::uint32_t kDescriptorSetDecoration = 34u;
    constexpr std::uint32_t kStorageUniformConstant = 0u;
    constexpr std::uint32_t kStorageUniform = 2u;
    constexpr std::uint32_t kStorageBuffer = 12u;

    enum class TypeKind : std::uint32_t { Unknown, Image, Sampler, Struct };
    struct TypeInfo {
        TypeKind kind = TypeKind::Unknown;
        std::uint32_t sampled = 0;
    };
    struct PointerInfo {
        std::uint32_t storageClass = 0;
        std::uint32_t pointeeType = 0;
    };
    struct VariableInfo {
        std::uint32_t pointerType = 0;
        std::uint32_t storageClass = 0;
    };
    struct DecorationInfo {
        bool hasBinding = false;
        std::uint32_t binding = 0;
        bool hasSet = false;
        std::uint32_t descriptorSet = 0;
    };

    const std::vector<std::uint32_t> words = readSpirv(blob);
    if (words.size() < 5 || words[0] != kMagic || words[1] != kSpirv15) {
        return false;
    }

    std::map<std::uint32_t, TypeInfo> types;
    std::map<std::uint32_t, PointerInfo> pointers;
    std::map<std::uint32_t, VariableInfo> variables;
    std::map<std::uint32_t, DecorationInfo> decorations;

    std::size_t cursor = 5;
    while (cursor < words.size()) {
        const std::uint32_t first = words[cursor];
        const std::uint16_t wordCount = static_cast<std::uint16_t>(first >> 16u);
        const std::uint16_t opcode = static_cast<std::uint16_t>(first & 0xffffu);
        if (wordCount == 0 || cursor + wordCount > words.size()) {
            return false;
        }
        if (opcode == kOpExtension && wordCount >= 2) {
            out.extensions.insert(spirvString(words.data() + cursor + 1, wordCount - 1));
        } else if (opcode == kOpEntryPoint && wordCount >= 4) {
            out.entryPoints.insert(spirvString(words.data() + cursor + 3, wordCount - 3));
        } else if (opcode == kOpCapability && wordCount == 2) {
            out.capabilities.insert(words[cursor + 1]);
        } else if (opcode == kOpTypeImage && wordCount >= 9) {
            types[words[cursor + 1]] = TypeInfo{.kind = TypeKind::Image, .sampled = words[cursor + 7]};
        } else if (opcode == kOpTypeSampler && wordCount >= 2) {
            types[words[cursor + 1]] = TypeInfo{.kind = TypeKind::Sampler};
        } else if (opcode == kOpTypeStruct && wordCount >= 2) {
            types[words[cursor + 1]] = TypeInfo{.kind = TypeKind::Struct};
        } else if (opcode == kOpTypePointer && wordCount >= 4) {
            pointers[words[cursor + 1]] = PointerInfo{
                .storageClass = words[cursor + 2],
                .pointeeType = words[cursor + 3],
            };
        } else if (opcode == kOpVariable && wordCount >= 4) {
            variables[words[cursor + 2]] = VariableInfo{
                .pointerType = words[cursor + 1],
                .storageClass = words[cursor + 3],
            };
        } else if (opcode == kOpDecorate && wordCount >= 4) {
            DecorationInfo& decoration = decorations[words[cursor + 1]];
            if (words[cursor + 2] == kBindingDecoration) {
                decoration.hasBinding = true;
                decoration.binding = words[cursor + 3];
            } else if (words[cursor + 2] == kDescriptorSetDecoration) {
                decoration.hasSet = true;
                decoration.descriptorSet = words[cursor + 3];
            }
        }
        cursor += wordCount;
    }
    if (cursor != words.size()) {
        return false;
    }

    for (const auto& [variableId, decoration] : decorations) {
        if (!decoration.hasBinding) {
            continue;
        }
        const auto variableIt = variables.find(variableId);
        if (variableIt == variables.end()) {
            return false;
        }
        const auto pointerIt = pointers.find(variableIt->second.pointerType);
        if (pointerIt == pointers.end()) {
            return false;
        }
        const auto typeIt = types.find(pointerIt->second.pointeeType);
        if (typeIt == types.end()) {
            return false;
        }

        SpirvDescriptorKind kind{};
        if (variableIt->second.storageClass == kStorageUniformConstant && typeIt->second.kind == TypeKind::Sampler) {
            kind = SpirvDescriptorKind::Sampler;
        } else if (variableIt->second.storageClass == kStorageUniformConstant && typeIt->second.kind == TypeKind::Image && typeIt->second.sampled == 1u) {
            kind = SpirvDescriptorKind::SampledImage;
        } else if (variableIt->second.storageClass == kStorageUniformConstant && typeIt->second.kind == TypeKind::Image && typeIt->second.sampled == 2u) {
            kind = SpirvDescriptorKind::StorageImage;
        } else if (variableIt->second.storageClass == kStorageUniform && typeIt->second.kind == TypeKind::Struct) {
            kind = SpirvDescriptorKind::UniformBuffer;
        } else if (variableIt->second.storageClass == kStorageBuffer && typeIt->second.kind == TypeKind::Struct) {
            kind = SpirvDescriptorKind::StorageBuffer;
        } else {
            return false;
        }

        const auto [_, inserted] = out.bindings.emplace(decoration.binding, kind);
        if (!inserted) {
            return false;
        }
        out.descriptorSets.insert(decoration.hasSet ? decoration.descriptorSet : 0u);
    }
    return true;
}

apusr::backends::VulkanFsr4ReferenceDescriptorKind runtimeKind(const SpirvDescriptorKind kind)
{
    using Kind = apusr::backends::VulkanFsr4ReferenceDescriptorKind;
    switch (kind) {
    case SpirvDescriptorKind::Sampler: return Kind::Sampler;
    case SpirvDescriptorKind::SampledImage: return Kind::SampledImage;
    case SpirvDescriptorKind::StorageImage: return Kind::StorageImage;
    case SpirvDescriptorKind::UniformBuffer: return Kind::UniformBuffer;
    case SpirvDescriptorKind::StorageBuffer: return Kind::StorageBuffer;
    }
    return Kind::SampledImage;
}

const char* expectedEntryPoint(
    const apusr::backends::VulkanFsr4ReferenceRecordPlan& plan,
    const char* dispatchName)
{
    for (const apusr::models::fsr4_int8::ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
        if (dispatch.name != nullptr && dispatchName != nullptr && std::strcmp(dispatch.name, dispatchName) == 0) {
            return dispatch.shader.entryPoint.c_str();
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    using namespace apusr;
    using namespace apusr::backends;
    using namespace apusr::models::fsr4_int8;

    const std::vector<std::uint8_t> shaderPackBytes = readBinary(
        std::filesystem::path(APUSR_REPO_ROOT) / "tests/data/fsr4_vulkan_p459.pack");
    VulkanFsr4ReferenceCreateDesc create{};
    create.repoRoot = APUSR_REPO_ROOT;
    create.shaderPack = VulkanFsr4ReferenceShaderPackView{shaderPackBytes.data(), shaderPackBytes.size()};
    create.runner.pipelineVariant = PipelineVariant::Dp4aOptimized;
    create.runner.maxRenderSize = RenderSize{1920, 1080};
    create.runner.maxUpscaleSize = DisplaySize{3840, 2160};
    create.runner.initialPreset = Preset::Performance;
    create.runner.features.autoExposure = true;
    create.runner.features.depthInverted = true;
    create.runner.allowDebugVisualization = false;
    create.runner.supportsWmma = false;

    VulkanFsr4ReferenceFrameDesc frame{};
    frame.runner.maxUpscaleSize = create.runner.maxUpscaleSize;
    frame.runner.residentModelPreset = Preset::Performance;
    frame.runner.frame.renderSize = create.runner.maxRenderSize;
    frame.runner.frame.displaySize = create.runner.maxUpscaleSize;
    frame.runner.frame.options.qualityMode = UpscalerQualityMode::Performance;
    frame.runner.frame.options.features.autoExposure = true;
    frame.runner.frame.options.features.depthInverted = true;
    frame.runner.frame.options.hasDepth = true;
    frame.runner.frame.options.sharpeningRequested = true;
    frame.runner.frame.options.sharpness = 0.5f;

    const VulkanFsr4ReferenceRecordPlan plan = makeVulkanFsr4ReferenceRecordPlan(create, frame);
    if (plan.framePlan.permutationKey != 459u || plan.dispatches.size() != 16u) {
        std::cerr << "unexpected packaged Vulkan slice: permutation=" << plan.framePlan.permutationKey
                  << " dispatches=" << plan.dispatches.size() << "\n";
        return 1;
    }

    const Status packStatus = validateVulkanFsr4ReferenceShaderPack(create.shaderPack);
    if (!packStatus.succeeded()) {
        std::cerr << "invalid Vulkan shader-pack fixture: " << packStatus.message << "\n";
        return 1;
    }
    VulkanFsr4ReferenceShaderKey strictKey = plan.dispatches[2].shaderKey;
    strictKey.pipelineVariant = PipelineVariant::StrictReference;
    if (findVulkanFsr4ReferenceShaderBlob(create.shaderPack, strictKey).data != nullptr) {
        std::cerr << "Vulkan shader pack key failed to separate pipeline implementations\n";
        return 1;
    }

    std::set<std::uint32_t> packageCapabilities;
    std::set<std::string> packageExtensions;
    for (const VulkanFsr4ReferenceDispatchPlan& dispatch : plan.dispatches) {
        const VulkanFsr4ReferenceShaderBlobView blob = findVulkanFsr4ReferenceShaderBlob(create.shaderPack, dispatch.shaderKey);
        SpirvContract spirv{};
        if (!inspectSpirv(blob, spirv)) {
            std::cerr << "invalid or missing SPIR-V-1.5 shader-pack entry for " << dispatch.dispatchName << "\n";
            return 1;
        }
        packageCapabilities.insert(spirv.capabilities.begin(), spirv.capabilities.end());
        packageExtensions.insert(spirv.extensions.begin(), spirv.extensions.end());
        if (spirv.descriptorSets.size() > 1u || (!spirv.descriptorSets.empty() && *spirv.descriptorSets.begin() != 0u)) {
            std::cerr << dispatch.dispatchName << " uses a descriptor set other than set 0\n";
            return 1;
        }
        const char* entryPoint = expectedEntryPoint(plan, dispatch.dispatchName);
        if (entryPoint == nullptr || !spirv.entryPoints.contains(entryPoint)) {
            std::cerr << dispatch.dispatchName << " is missing expected SPIR-V entry point "
                      << (entryPoint != nullptr ? entryPoint : "<null>") << "\n";
            return 1;
        }

        std::map<std::uint32_t, VulkanFsr4ReferenceDescriptorKind> runtimeBindings;
        for (const VulkanFsr4ReferenceDescriptorBinding& binding : dispatch.bindings) {
            const auto [_, inserted] = runtimeBindings.emplace(binding.binding, binding.kind);
            if (!inserted) {
                std::cerr << dispatch.dispatchName << " contains a duplicate recorder binding " << binding.binding << "\n";
                return 1;
            }
        }
        for (const auto& [binding, spirvKind] : spirv.bindings) {
            const auto found = runtimeBindings.find(binding);
            if (found == runtimeBindings.end()) {
                std::cerr << dispatch.dispatchName << " SPIR-V binding " << binding
                          << " is absent from the recorder descriptor plan\n";
                return 1;
            }
            if (found->second != runtimeKind(spirvKind)) {
                std::cerr << dispatch.dispatchName << " SPIR-V binding " << binding
                          << " has a descriptor type that disagrees with the recorder plan\n";
                return 1;
            }
        }
        const bool vectorScratchStage = dispatch.passKind == PassKind::ModelMiddle &&
            (dispatch.logicalStage == 3u || dispatch.logicalStage == 6u || dispatch.logicalStage == 7u ||
             dispatch.logicalStage == 8u || dispatch.logicalStage == 9u);
        const bool vectorInitializerStage = vectorScratchStage && dispatch.logicalStage != 3u;
        const std::uint32_t initializerVectorBinding =
            kVulkanFsr4SrvBindingBase + kVulkanFsr4VectorInitializerSrvRegister;
        const std::uint32_t scratchVectorBinding =
            kVulkanFsr4UavBindingBase + kVulkanFsr4VectorScratchUavRegister;
        if (spirv.bindings.contains(initializerVectorBinding) != vectorInitializerStage ||
            spirv.bindings.contains(scratchVectorBinding) != vectorScratchStage) {
            std::cerr << dispatch.dispatchName
                      << " does not match the audited aligned vector-buffer descriptor contract\n";
            return 1;
        }
    }

    const std::set<std::uint32_t> expectedCapabilities{1u, 9u, 22u, 39u, 49u, 50u, 56u, 68u, 5350u, 6018u, 6019u};
    const std::set<std::string> expectedExtensions{
        "SPV_KHR_compute_shader_derivatives",
        "SPV_KHR_integer_dot_product",
    };
    if (packageCapabilities != expectedCapabilities) {
        std::cerr << "packaged Vulkan SPIR-V capability envelope changed; device negotiation must be reviewed\n";
        return 1;
    }
    if (packageExtensions != expectedExtensions) {
        std::cerr << "packaged Vulkan SPIR-V extension envelope changed; device negotiation must be reviewed\n";
        return 1;
    }

    std::cout << "APUSR Vulkan SPIR-V contract passed: compact p459 shader-pack lookup + 16 bindings/entry points/capability envelope\n";
    return 0;
}
