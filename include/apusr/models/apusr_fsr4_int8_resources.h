// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/models/apusr_fsr4_int8_graph.h>
#include <apusr/apusr_graphics.h>

#include <cstdint>
#include <vector>

namespace apusr::models::fsr4_int8 {

enum class ResourceKind : std::uint32_t {
    Texture2D,
    Buffer,
};

enum class ResourceLifetime : std::uint32_t {
    External,
    Persistent,
    FrameTransient,
    Upload,
};

enum class ResourceInitialContents : std::uint32_t {
    ExternalProvided,
    Uninitialized,
    Zero,
    DefaultExposureOneOne,
    InitializerBytes,
};

enum class ResourceResizePolicy : std::uint32_t {
    ExternalProvided,
    MatchMaxRenderSize,
    MatchAlignedMaxUpscaleSize,
    FixedExtent,
    CapacityByteSize,
    InitializerByteSize,
};

enum class ResourceId : std::uint32_t {
    Null = 0,
    InputColor,
    InputMotionVectors,
    InputDepth,
    InputExposure,
    AutoExposure,
    SpdAtomicCount,
    SpdMip5,
    Luma0,
    Luma1,
    MlsrOutput,
    RcasOutput,
    FinalOutput,
    Recurrent,
    History,
    HistoryReprojected,
    RcasTemp,
    InternalDefaultExposure,
    BufferNhwcInputs,
    BufferFusedQuantizedNhwcOutput,
    InitializerBuffer,
    InitializerStagingBuffer,
    InitializerUploadBuffer,
    ScratchBuffer,
    DebugInformation,
    ResultColor,
};

struct ResourceDesc {
    ResourceId id = ResourceId::Null;
    std::uint32_t ffxIdentifier = 0;
    const char* name = nullptr;
    ResourceKind kind = ResourceKind::Texture2D;
    ResourceLifetime lifetime = ResourceLifetime::Persistent;
    SurfaceFormat format = SurfaceFormat::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t byteSize = 0;
    bool srv = false;
    bool uav = false;
    bool renderTargetCompatible = false;
    bool optional = false;
    bool temporal = false;
    ResourceInitialContents initialContents = ResourceInitialContents::Uninitialized;
    ResourceResizePolicy resizePolicy = ResourceResizePolicy::FixedExtent;
};

struct ResourceAliasDesc {
    ResourceId id = ResourceId::Null;
    std::uint32_t ffxIdentifier = 0;
    const char* name = nullptr;
    ResourceId defaultTarget = ResourceId::Null;
    ResourceId rcasEnabledTarget = ResourceId::Null;
    bool srv = false;
    bool uav = false;
};

struct ResourcePlanDesc {
    RenderSize maxRenderSize;
    DisplaySize maxUpscaleSize;
    bool includeExternalResources = true;
    bool includeDebugInformation = false;
};

struct ResourcePlan {
    ResourcePlanDesc desc;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    RenderSize maxRenderSize;
    DisplaySize logicalMaxUpscaleSize;
    DisplaySize alignedMaxUpscaleSize;
    std::uint64_t initializerByteSize = 0;
    std::uint64_t scratchByteSize = 0;
    std::vector<ResourceDesc> resources;
    std::vector<ResourceAliasDesc> aliases;
};

[[nodiscard]] const char* resourceIdName(ResourceId id);
[[nodiscard]] const char* resourceKindName(ResourceKind kind);
[[nodiscard]] const char* resourceLifetimeName(ResourceLifetime lifetime);
[[nodiscard]] const char* resourceInitialContentsName(ResourceInitialContents initialContents);
[[nodiscard]] const char* resourceResizePolicyName(ResourceResizePolicy resizePolicy);
[[nodiscard]] std::uint32_t ffxResourceIdentifier(ResourceId id);
[[nodiscard]] std::uint64_t scratchByteSize(FrameCapacity frameCapacity);
[[nodiscard]] std::uint64_t maxInitializerByteSize();
[[nodiscard]] ResourcePlan makeReferenceResourcePlan(const ResourcePlanDesc& desc);
[[nodiscard]] const ResourceDesc* findResource(const ResourcePlan& plan, ResourceId id);
[[nodiscard]] const ResourceAliasDesc* findAlias(const ResourcePlan& plan, ResourceId id);

} // namespace apusr::models::fsr4_int8
