// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_frame_desc.h>
#include <apusr/models/apusr_fsr4_int8_reference_runner.h>
#include <apusr/models/apusr_fsr4_int8_resources.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace apusr::backends {

inline constexpr std::uint32_t kVulkanFsr4SrvBindingBase = 0;
inline constexpr std::uint32_t kVulkanFsr4UavBindingBase = 32;
inline constexpr std::uint32_t kVulkanFsr4CbvBindingBase = 64;
inline constexpr std::uint32_t kVulkanFsr4SamplerBindingBase = 80;
inline constexpr std::uint32_t kVulkanFsr4VectorInitializerSrvRegister = 31;
inline constexpr std::uint32_t kVulkanFsr4VectorScratchUavRegister = 31;

struct VulkanFsr4ReferenceShaderPackView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct VulkanFsr4ReferenceShaderKey {
    models::fsr4_int8::PipelineVariant pipelineVariant = models::fsr4_int8::PipelineVariant::StrictReference;
    models::fsr4_int8::Preset preset = models::fsr4_int8::Preset::Balanced;
    models::fsr4_int8::FrameCapacity frameCapacity = models::fsr4_int8::FrameCapacity::R2160;
    std::uint32_t permutationKey = 0;
    models::fsr4_int8::PassKind passKind = models::fsr4_int8::PassKind::ModelMiddle;
    std::uint32_t logicalStage = 0;
};

struct VulkanFsr4ReferenceShaderBlobView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct VulkanFsr4ReferenceCreateDesc {
    NativeHandle instance;
    NativeHandle physicalDevice;
    NativeHandle device;
    std::uint32_t graphicsQueueFamilyIndex = kFrameInputUnknownQueueFamily;
    std::uint32_t computeQueueFamilyIndex = kFrameInputUnknownQueueFamily;
    std::string repoRoot;
    std::filesystem::path shaderPackPath;
    VulkanFsr4ReferenceShaderPackView shaderPack;
    models::fsr4_int8::ReferenceRunnerCreateDesc runner;
    // Zero preserves the driver-selected subgroup size. 32 and 64 request an
    // exact compute subgroup through Vulkan subgroup-size control.
    std::uint32_t requiredSubgroupSize = 0;
    bool requireInitializerBytes = true;
};

struct VulkanFsr4ReferenceFrameDesc {
    NativeHandle commandBuffer;
    FrameInputDesc frameInput;
    models::fsr4_int8::ReferenceRunnerFrameDesc runner;
};

enum class VulkanFsr4ReferenceDescriptorKind : std::uint32_t {
    Sampler,
    SampledImage,
    StorageImage,
    UniformBuffer,
    StorageBuffer,
};

struct VulkanFsr4ReferenceDescriptorBinding {
    std::uint32_t binding = 0;
    VulkanFsr4ReferenceDescriptorKind kind = VulkanFsr4ReferenceDescriptorKind::SampledImage;
    models::fsr4_int8::ResourceId resource = models::fsr4_int8::ResourceId::Null;
    models::fsr4_int8::ReferenceConstantBufferRole constantBuffer = models::fsr4_int8::ReferenceConstantBufferRole::None;
    models::fsr4_int8::ReferenceResourceAccess access = models::fsr4_int8::ReferenceResourceAccess::Read;
    const char* shaderName = nullptr;
    bool optional = false;
};

struct VulkanFsr4ReferenceDispatchPlan {
    const char* dispatchName = nullptr;
    models::fsr4_int8::PassKind passKind = models::fsr4_int8::PassKind::ModelMiddle;
    std::uint32_t logicalStage = 0;
    DispatchGrid grid;
    VulkanFsr4ReferenceShaderKey shaderKey;
    std::vector<VulkanFsr4ReferenceDescriptorBinding> bindings;
};

struct VulkanFsr4ReferenceRecordPlan {
    models::fsr4_int8::ReferenceFramePlan framePlan;
    models::fsr4_int8::ResourcePlan resourcePlan;
    std::vector<VulkanFsr4ReferenceDispatchPlan> dispatches;
};

[[nodiscard]] VulkanFsr4ReferenceRecordPlan makeVulkanFsr4ReferenceRecordPlan(
    const VulkanFsr4ReferenceCreateDesc& createDesc,
    const VulkanFsr4ReferenceFrameDesc& frameDesc);

[[nodiscard]] Status validateVulkanFsr4ReferenceShaderPack(VulkanFsr4ReferenceShaderPackView pack);
[[nodiscard]] VulkanFsr4ReferenceShaderBlobView findVulkanFsr4ReferenceShaderBlob(
    VulkanFsr4ReferenceShaderPackView pack,
    const VulkanFsr4ReferenceShaderKey& key) noexcept;
[[nodiscard]] const char* vulkanFsr4ReferenceDescriptorKindName(VulkanFsr4ReferenceDescriptorKind kind);

class VulkanFsr4ReferenceRuntime {
public:
    virtual ~VulkanFsr4ReferenceRuntime() = default;
    [[nodiscard]] virtual Status initialize(const VulkanFsr4ReferenceCreateDesc& desc) = 0;
    [[nodiscard]] virtual Status prepareFrameCache(const VulkanFsr4ReferenceFrameDesc& desc) = 0;
    [[nodiscard]] virtual Status recordFrame(const VulkanFsr4ReferenceFrameDesc& desc) = 0;
};

[[nodiscard]] std::unique_ptr<VulkanFsr4ReferenceRuntime> createVulkanFsr4ReferenceRuntime();

} // namespace apusr::backends
