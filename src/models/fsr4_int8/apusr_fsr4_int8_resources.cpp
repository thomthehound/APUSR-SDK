// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_resources.h>

namespace apusr::models::fsr4_int8 {
namespace {

constexpr std::uint32_t roundUpDiv(const std::uint32_t value, const std::uint32_t divisor)
{
    return divisor == 0 ? 0 : (value + divisor - 1) / divisor;
}

constexpr std::uint32_t alignUp(const std::uint32_t value, const std::uint32_t alignment)
{
    return alignment == 0 ? value : roundUpDiv(value, alignment) * alignment;
}

void pushExternal(ResourcePlan& plan, const ResourceId id, const char* name, const SurfaceFormat format, const std::uint32_t width, const std::uint32_t height)
{
    plan.resources.push_back(ResourceDesc{
        .id = id,
        .ffxIdentifier = ffxResourceIdentifier(id),
        .name = name,
        .kind = ResourceKind::Texture2D,
        .lifetime = ResourceLifetime::External,
        .format = format,
        .width = width,
        .height = height,
        .byteSize = 0,
        .srv = id != ResourceId::FinalOutput,
        .uav = id == ResourceId::FinalOutput,
        .renderTargetCompatible = id == ResourceId::FinalOutput,
        .optional = id == ResourceId::InputExposure,
        .initialContents = ResourceInitialContents::ExternalProvided,
        .resizePolicy = ResourceResizePolicy::ExternalProvided,
    });
}

void pushTexture(ResourcePlan& plan,
    const ResourceId id,
    const char* name,
    const SurfaceFormat format,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool srv,
    const bool uav,
    const bool renderTargetCompatible,
    const bool temporal = false,
    const bool optional = false,
    const ResourceInitialContents initialContents = ResourceInitialContents::Uninitialized,
    const ResourceResizePolicy resizePolicy = ResourceResizePolicy::FixedExtent)
{
    plan.resources.push_back(ResourceDesc{
        .id = id,
        .ffxIdentifier = ffxResourceIdentifier(id),
        .name = name,
        .kind = ResourceKind::Texture2D,
        .lifetime = ResourceLifetime::Persistent,
        .format = format,
        .width = width,
        .height = height,
        .byteSize = 0,
        .srv = srv,
        .uav = uav,
        .renderTargetCompatible = renderTargetCompatible,
        .optional = optional,
        .temporal = temporal,
        .initialContents = initialContents,
        .resizePolicy = resizePolicy,
    });
}

void pushBuffer(ResourcePlan& plan,
    const ResourceId id,
    const char* name,
    const std::uint64_t byteSize,
    const ResourceLifetime lifetime,
    const bool srv,
    const bool uav,
    const ResourceInitialContents initialContents = ResourceInitialContents::Uninitialized,
    const ResourceResizePolicy resizePolicy = ResourceResizePolicy::CapacityByteSize)
{
    plan.resources.push_back(ResourceDesc{
        .id = id,
        .ffxIdentifier = ffxResourceIdentifier(id),
        .name = name,
        .kind = ResourceKind::Buffer,
        .lifetime = lifetime,
        .format = SurfaceFormat::Unknown,
        .byteSize = byteSize,
        .srv = srv,
        .uav = uav,
        .initialContents = initialContents,
        .resizePolicy = resizePolicy,
    });
}

} // namespace

const char* resourceIdName(const ResourceId id)
{
    switch (id) {
    case ResourceId::Null: return "null";
    case ResourceId::InputColor: return "input-color";
    case ResourceId::InputMotionVectors: return "input-motion-vectors";
    case ResourceId::InputDepth: return "input-depth";
    case ResourceId::InputExposure: return "input-exposure";
    case ResourceId::AutoExposure: return "auto-exposure";
    case ResourceId::SpdAtomicCount: return "spd-atomic-count";
    case ResourceId::SpdMip5: return "spd-mip5";
    case ResourceId::Luma0: return "luma-0";
    case ResourceId::Luma1: return "luma-1";
    case ResourceId::MlsrOutput: return "mlsr-output";
    case ResourceId::RcasOutput: return "rcas-output";
    case ResourceId::FinalOutput: return "final-output";
    case ResourceId::Recurrent: return "recurrent";
    case ResourceId::History: return "history";
    case ResourceId::HistoryReprojected: return "history-reprojected";
    case ResourceId::RcasTemp: return "rcas-temp";
    case ResourceId::InternalDefaultExposure: return "internal-default-exposure";
    case ResourceId::BufferNhwcInputs: return "buffer-nhwc-inputs";
    case ResourceId::BufferFusedQuantizedNhwcOutput: return "buffer-fused-quantized-nhwc-output";
    case ResourceId::InitializerBuffer: return "initializer-buffer";
    case ResourceId::InitializerStagingBuffer: return "initializer-staging-buffer";
    case ResourceId::InitializerUploadBuffer: return "initializer-upload-buffer";
    case ResourceId::ScratchBuffer: return "scratch-buffer";
    case ResourceId::DebugInformation: return "debug-information";
    case ResourceId::ResultColor: return "result-color";
    }
    return "unknown";
}

const char* resourceKindName(const ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Texture2D: return "texture2d";
    case ResourceKind::Buffer: return "buffer";
    }
    return "unknown";
}

const char* resourceLifetimeName(const ResourceLifetime lifetime)
{
    switch (lifetime) {
    case ResourceLifetime::External: return "external";
    case ResourceLifetime::Persistent: return "persistent";
    case ResourceLifetime::FrameTransient: return "frame-transient";
    case ResourceLifetime::Upload: return "upload";
    }
    return "unknown";
}

const char* resourceInitialContentsName(const ResourceInitialContents initialContents)
{
    switch (initialContents) {
    case ResourceInitialContents::ExternalProvided: return "external-provided";
    case ResourceInitialContents::Uninitialized: return "uninitialized";
    case ResourceInitialContents::Zero: return "zero";
    case ResourceInitialContents::DefaultExposureOneOne: return "default-exposure-one-one";
    case ResourceInitialContents::InitializerBytes: return "initializer-bytes";
    }
    return "unknown";
}

const char* resourceResizePolicyName(const ResourceResizePolicy resizePolicy)
{
    switch (resizePolicy) {
    case ResourceResizePolicy::ExternalProvided: return "external-provided";
    case ResourceResizePolicy::MatchMaxRenderSize: return "match-max-render-size";
    case ResourceResizePolicy::MatchAlignedMaxUpscaleSize: return "match-aligned-max-upscale-size";
    case ResourceResizePolicy::FixedExtent: return "fixed-extent";
    case ResourceResizePolicy::CapacityByteSize: return "capacity-byte-size";
    case ResourceResizePolicy::InitializerByteSize: return "initializer-byte-size";
    }
    return "unknown";
}

std::uint32_t ffxResourceIdentifier(const ResourceId id)
{
    // Values mirror ffx_fsr4upscaler_resources.h so shader reflection can be
    // patched into APUSR-owned descriptors without pulling FFX backend structs
    // above the backend layer.
    switch (id) {
    case ResourceId::Null: return 0;
    case ResourceId::InputColor: return 1;
    case ResourceId::InputMotionVectors: return 2;
    case ResourceId::InputDepth: return 3;
    case ResourceId::InputExposure: return 4;
    case ResourceId::AutoExposure: return 5;
    case ResourceId::SpdAtomicCount: return 6;
    case ResourceId::SpdMip5: return 7;
    case ResourceId::Luma0: return 8;
    case ResourceId::Luma1: return 9;
    case ResourceId::MlsrOutput: return 10;
    case ResourceId::RcasOutput: return 11;
    case ResourceId::FinalOutput: return 12;
    case ResourceId::Recurrent: return 13;
    case ResourceId::History: return 14;
    case ResourceId::HistoryReprojected: return 15;
    case ResourceId::RcasTemp: return 16;
    case ResourceId::InternalDefaultExposure: return 17;
    case ResourceId::BufferNhwcInputs: return 18;
    case ResourceId::BufferFusedQuantizedNhwcOutput: return 19;
    case ResourceId::InitializerBuffer: return 20;
    case ResourceId::InitializerStagingBuffer: return 21;
    case ResourceId::InitializerUploadBuffer: return 21;
    case ResourceId::ScratchBuffer: return 22;
    case ResourceId::DebugInformation: return 23;
    case ResourceId::ResultColor: return 24;
    }
    return 0;
}

std::uint64_t scratchByteSize(const FrameCapacity frameCapacity)
{
    switch (frameCapacity) {
    case FrameCapacity::R1080: return 20880256ull;
    case FrameCapacity::R2160: return 83232256ull;
    case FrameCapacity::R4320: return 332352256ull;
    }
    return 83232256ull;
}

std::uint64_t maxInitializerByteSize()
{
    return 89216ull;
}

ResourcePlan makeReferenceResourcePlan(const ResourcePlanDesc& desc)
{
    ResourcePlan plan{};
    plan.desc = desc;
    plan.maxRenderSize = desc.maxRenderSize;
    plan.logicalMaxUpscaleSize = desc.maxUpscaleSize;
    plan.alignedMaxUpscaleSize = DisplaySize{.width = alignUp(desc.maxUpscaleSize.width, 8u), .height = alignUp(desc.maxUpscaleSize.height, 8u)};
    plan.frameCapacity = frameCapacityForDisplay(plan.alignedMaxUpscaleSize);
    plan.initializerByteSize = maxInitializerByteSize();
    plan.scratchByteSize = scratchByteSize(plan.frameCapacity);

    const std::uint32_t renderWidth = desc.maxRenderSize.width;
    const std::uint32_t renderHeight = desc.maxRenderSize.height;
    const std::uint32_t displayWidth = plan.alignedMaxUpscaleSize.width;
    const std::uint32_t displayHeight = plan.alignedMaxUpscaleSize.height;

    if (desc.includeExternalResources) {
        pushExternal(plan, ResourceId::InputColor, "r_input_color", SurfaceFormat::Rgba16Float, renderWidth, renderHeight);
        pushExternal(plan, ResourceId::InputDepth, "r_depth", SurfaceFormat::R32Float, renderWidth, renderHeight);
        pushExternal(plan, ResourceId::InputMotionVectors, "r_velocity", SurfaceFormat::Rg16Float, renderWidth, renderHeight);
        pushExternal(plan, ResourceId::InputExposure, "r_input_exposure", SurfaceFormat::R32Float, 1, 1);
        pushExternal(plan, ResourceId::FinalOutput, "rw_final_output_color", SurfaceFormat::Rgba16Float, displayWidth, displayHeight);
    }

    pushTexture(plan, ResourceId::Recurrent, "FSR4UPSCALER_Recurrent", SurfaceFormat::Rgba8Unorm, displayWidth, displayHeight, true, true, true, true, false, ResourceInitialContents::Uninitialized, ResourceResizePolicy::MatchAlignedMaxUpscaleSize);
    pushTexture(plan, ResourceId::History, "FSR4UPSCALER_History", SurfaceFormat::Rgba16Float, displayWidth, displayHeight, true, true, true, true, false, ResourceInitialContents::Uninitialized, ResourceResizePolicy::MatchAlignedMaxUpscaleSize);
    pushTexture(plan, ResourceId::HistoryReprojected, "FSR4UPSCALER_HistoryReprojected", SurfaceFormat::Rgba16Float, displayWidth, displayHeight, true, true, true, true, false, ResourceInitialContents::Uninitialized, ResourceResizePolicy::MatchAlignedMaxUpscaleSize);
    pushTexture(plan, ResourceId::RcasTemp, "FSR4UPSCALER_RcasIntermediary", SurfaceFormat::Rgba16Float, displayWidth, displayHeight, true, true, true, false, false, ResourceInitialContents::Uninitialized, ResourceResizePolicy::MatchAlignedMaxUpscaleSize);
    pushTexture(plan, ResourceId::AutoExposure, "FSR4UPSCALER_AutoExposure", SurfaceFormat::R32Float, 2, 1, true, true, true, true, false, ResourceInitialContents::Uninitialized, ResourceResizePolicy::FixedExtent);
    pushTexture(plan, ResourceId::InternalDefaultExposure, "FSR4UPSCALER_DefaultExposure", SurfaceFormat::Rg32Float, 1, 1, true, false, false, false, false, ResourceInitialContents::DefaultExposureOneOne, ResourceResizePolicy::FixedExtent);
    pushTexture(plan, ResourceId::SpdAtomicCount, "FSR4UPSCALER_SpdAtomicCounter", SurfaceFormat::R32Uint, 1, 1, false, true, false, false, false, ResourceInitialContents::Zero, ResourceResizePolicy::FixedExtent);
    pushTexture(plan, ResourceId::SpdMip5, "FSR4UPSCALER_Luma_Mip_5", SurfaceFormat::R32Float, roundUpDiv(renderWidth, 16), roundUpDiv(renderHeight, 16), true, true, true, false, false, ResourceInitialContents::Uninitialized, ResourceResizePolicy::MatchMaxRenderSize);
    if (desc.includeDebugInformation) {
        pushTexture(plan, ResourceId::DebugInformation, "FSR4UPSCALER_DebugInformation", SurfaceFormat::R16Unorm, displayWidth, displayHeight, true, true, true, false, true, ResourceInitialContents::Uninitialized, ResourceResizePolicy::MatchAlignedMaxUpscaleSize);
    }

    pushBuffer(plan, ResourceId::ScratchBuffer, "FSR4UPSCALER_ScratchBuffer", plan.scratchByteSize, ResourceLifetime::Persistent, true, true, ResourceInitialContents::Uninitialized, ResourceResizePolicy::CapacityByteSize);
    pushBuffer(plan, ResourceId::InitializerBuffer, "FSR4UPSCALER_InitializerBuffer", plan.initializerByteSize, ResourceLifetime::Persistent, true, false, ResourceInitialContents::InitializerBytes, ResourceResizePolicy::InitializerByteSize);
    pushBuffer(plan, ResourceId::InitializerUploadBuffer, "FSR4UPSCALER_InitializerBuffer_Upload", plan.initializerByteSize, ResourceLifetime::Upload, true, false, ResourceInitialContents::InitializerBytes, ResourceResizePolicy::InitializerByteSize);

    // These are binding identities in the FFX graph, not separate allocations.
    // The scheduler resolves them to concrete resources at dispatch time.
    plan.aliases.push_back(ResourceAliasDesc{
        .id = ResourceId::MlsrOutput,
        .ffxIdentifier = ffxResourceIdentifier(ResourceId::MlsrOutput),
        .name = "rw_mlsr_output_color",
        .defaultTarget = ResourceId::FinalOutput,
        .rcasEnabledTarget = ResourceId::RcasTemp,
        .uav = true,
    });
    plan.aliases.push_back(ResourceAliasDesc{
        .id = ResourceId::RcasOutput,
        .ffxIdentifier = ffxResourceIdentifier(ResourceId::RcasOutput),
        .name = "rw_rcas_output",
        .defaultTarget = ResourceId::FinalOutput,
        .rcasEnabledTarget = ResourceId::FinalOutput,
        .srv = true,
        .uav = true,
    });
    plan.aliases.push_back(ResourceAliasDesc{
        .id = ResourceId::ResultColor,
        .ffxIdentifier = ffxResourceIdentifier(ResourceId::ResultColor),
        .name = "r_result_color",
        .defaultTarget = ResourceId::FinalOutput,
        .rcasEnabledTarget = ResourceId::FinalOutput,
        .srv = true,
    });
    return plan;
}

const ResourceDesc* findResource(const ResourcePlan& plan, const ResourceId id)
{
    for (const ResourceDesc& resource : plan.resources) {
        if (resource.id == id) {
            return &resource;
        }
    }
    return nullptr;
}

const ResourceAliasDesc* findAlias(const ResourcePlan& plan, const ResourceId id)
{
    for (const ResourceAliasDesc& alias : plan.aliases) {
        if (alias.id == id) {
            return &alias;
        }
    }
    return nullptr;
}

} // namespace apusr::models::fsr4_int8
