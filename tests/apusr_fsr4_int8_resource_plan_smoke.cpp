// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_resources.h>

#include <cassert>
#include <cstring>

namespace {

const apusr::models::fsr4_int8::ResourceDesc& requireResource(
    const apusr::models::fsr4_int8::ResourcePlan& plan,
    const apusr::models::fsr4_int8::ResourceId id)
{
    const auto* resource = apusr::models::fsr4_int8::findResource(plan, id);
    assert(resource != nullptr);
    return *resource;
}

} // namespace

int main()
{
    using namespace apusr::models::fsr4_int8;

    assert(std::strcmp(resourceIdName(ResourceId::History), "history") == 0);
    assert(std::strcmp(resourceKindName(ResourceKind::Buffer), "buffer") == 0);
    assert(std::strcmp(resourceLifetimeName(ResourceLifetime::Upload), "upload") == 0);
    assert(std::strcmp(resourceInitialContentsName(ResourceInitialContents::DefaultExposureOneOne), "default-exposure-one-one") == 0);
    assert(std::strcmp(resourceResizePolicyName(ResourceResizePolicy::MatchAlignedMaxUpscaleSize), "match-aligned-max-upscale-size") == 0);
    assert(ffxResourceIdentifier(ResourceId::InputDepth) == 3);
    assert(ffxResourceIdentifier(ResourceId::Luma0) == 8);
    assert(ffxResourceIdentifier(ResourceId::Luma1) == 9);
    assert(ffxResourceIdentifier(ResourceId::BufferNhwcInputs) == 18);
    assert(ffxResourceIdentifier(ResourceId::BufferFusedQuantizedNhwcOutput) == 19);
    assert(ffxResourceIdentifier(ResourceId::InitializerBuffer) == 20);
    assert(ffxResourceIdentifier(ResourceId::InitializerStagingBuffer) == 21);
    assert(ffxResourceIdentifier(ResourceId::ScratchBuffer) == 22);
    assert(scratchByteSize(FrameCapacity::R1080) == 20880256ull);
    assert(scratchByteSize(FrameCapacity::R2160) == 83232256ull);
    assert(scratchByteSize(FrameCapacity::R4320) == 332352256ull);
    assert(maxInitializerByteSize() == 89216ull);

    const ResourcePlan plan = makeReferenceResourcePlan(ResourcePlanDesc{
        .maxRenderSize = apusr::RenderSize{.width = 2024, .height = 847},
        .maxUpscaleSize = apusr::DisplaySize{.width = 3440, .height = 1440},
        .includeExternalResources = true,
        .includeDebugInformation = true,
    });
    assert(plan.frameCapacity == FrameCapacity::R2160);
    assert(plan.logicalMaxUpscaleSize.width == 3440);
    assert(plan.alignedMaxUpscaleSize.width == 3440);
    assert(plan.initializerByteSize == 89216ull);
    assert(plan.scratchByteSize == 83232256ull);

    const ResourceDesc& history = requireResource(plan, ResourceId::History);
    assert(history.format == apusr::SurfaceFormat::Rgba16Float);
    assert(history.width == 3440);
    assert(history.height == 1440);
    assert(history.srv && history.uav && history.temporal);
    assert(history.resizePolicy == ResourceResizePolicy::MatchAlignedMaxUpscaleSize);

    const ResourceDesc& recurrent = requireResource(plan, ResourceId::Recurrent);
    assert(recurrent.format == apusr::SurfaceFormat::Rgba8Unorm);
    assert(recurrent.temporal);

    const ResourceDesc& spdMip5 = requireResource(plan, ResourceId::SpdMip5);
    assert(spdMip5.width == 127);
    assert(spdMip5.height == 53);

    const ResourceDesc& inputExposure = requireResource(plan, ResourceId::InputExposure);
    assert(inputExposure.optional);

    const ResourceDesc& defaultExposure = requireResource(plan, ResourceId::InternalDefaultExposure);
    assert(defaultExposure.initialContents == ResourceInitialContents::DefaultExposureOneOne);

    const ResourceDesc& scratch = requireResource(plan, ResourceId::ScratchBuffer);
    assert(scratch.kind == ResourceKind::Buffer);
    assert(scratch.byteSize == 83232256ull);
    assert(scratch.srv && scratch.uav);
    assert(scratch.resizePolicy == ResourceResizePolicy::CapacityByteSize);

    const ResourceDesc& upload = requireResource(plan, ResourceId::InitializerUploadBuffer);
    assert(upload.lifetime == ResourceLifetime::Upload);
    assert(upload.initialContents == ResourceInitialContents::InitializerBytes);
    assert(upload.byteSize == 89216ull);

    const ResourceAliasDesc* mlsr = findAlias(plan, ResourceId::MlsrOutput);
    assert(mlsr != nullptr);
    assert(mlsr->defaultTarget == ResourceId::FinalOutput);
    assert(mlsr->rcasEnabledTarget == ResourceId::RcasTemp);
    assert(mlsr->uav);

    const ResourceAliasDesc* resultColor = findAlias(plan, ResourceId::ResultColor);
    assert(resultColor != nullptr);
    assert(resultColor->defaultTarget == ResourceId::FinalOutput);
    assert(resultColor->srv);

    const ResourceDesc& debug = requireResource(plan, ResourceId::DebugInformation);
    assert(debug.optional);
    assert(debug.format == apusr::SurfaceFormat::R16Unorm);

    return 0;
}
