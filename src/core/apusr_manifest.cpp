// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_manifest.h>

#include <algorithm>
#include <utility>

namespace apusr {

Status Status::ok()
{
    return Status{};
}

Status Status::invalidArgument(std::string message)
{
    return Status{StatusCode::InvalidArgument, std::move(message)};
}

Status Status::unsupportedBackend(std::string message)
{
    return Status{StatusCode::UnsupportedBackend, std::move(message)};
}

Status Status::missingBackend(std::string message)
{
    return Status{StatusCode::MissingBackend, std::move(message)};
}

Status Status::runtimeFailure(std::string message)
{
    return Status{StatusCode::RuntimeFailure, std::move(message)};
}

Status Status::notImplemented(std::string message)
{
    return Status{StatusCode::NotImplemented, std::move(message)};
}

bool Status::succeeded() const
{
    return code == StatusCode::Ok;
}

const char* statusCodeName(const StatusCode code)
{
    switch (code) {
    case StatusCode::Ok:
        return "Ok";
    case StatusCode::InvalidArgument:
        return "InvalidArgument";
    case StatusCode::UnsupportedBackend:
        return "UnsupportedBackend";
    case StatusCode::MissingBackend:
        return "MissingBackend";
    case StatusCode::RuntimeFailure:
        return "RuntimeFailure";
    case StatusCode::NotImplemented:
        return "NotImplemented";
    }
    return "Unknown";
}

const char* edgeTransportPolicyName(const EdgeTransportPolicy policy)
{
    switch (policy) {
    case EdgeTransportPolicy::DirectStream:
        return "direct_stream";
    case EdgeTransportPolicy::BufferedDmaDelay:
        return "buffered_dma_delay";
    case EdgeTransportPolicy::LayoutDma:
        return "layout_dma";
    case EdgeTransportPolicy::ExternalSpillDebug:
        return "external_spill_debug";
    }
    return "unknown";
}

const char* stageCadenceName(const StageCadence cadence)
{
    switch (cadence) {
    case StageCadence::OneToOne:
        return "one_to_one";
    case StageCadence::Downsample2x:
        return "downsample_2x";
    case StageCadence::Upsample2x:
        return "upsample_2x";
    }
    return "unknown";
}

std::uint32_t maxSpatialKernelHeight(const SegmentManifest& manifest)
{
    std::uint32_t maxHeight = 1;
    for (const SegmentStageDesc& stage : manifest.stages) {
        maxHeight = std::max(maxHeight, stage.spatialKernelHeight);
    }
    return maxHeight;
}

std::uint32_t maxSpatialKernelWidth(const SegmentManifest& manifest)
{
    std::uint32_t maxWidth = 1;
    for (const SegmentStageDesc& stage : manifest.stages) {
        maxWidth = std::max(maxWidth, stage.spatialKernelWidth);
    }
    return maxWidth;
}

bool hasBufferedSkipDelay(const SegmentManifest& manifest)
{
    return std::any_of(manifest.skipDelays.begin(), manifest.skipDelays.end(), [](const SkipDelayDesc& skip) {
        return skip.transport == EdgeTransportPolicy::BufferedDmaDelay;
    });
}

Status validateSegmentManifest(const SegmentManifest& manifest)
{
    if (manifest.name.empty()) {
        return Status::invalidArgument("segment manifest requires a name");
    }
    if (manifest.firstLogicalStage > manifest.lastLogicalStage) {
        return Status::invalidArgument("segment manifest stage range is inverted");
    }
    if (manifest.inputBoundary.byteSize == 0 || manifest.outputBoundary.byteSize == 0) {
        return Status::invalidArgument("segment boundaries must have non-zero byte sizes");
    }
    if (manifest.inputBoundary.shape.width == 0 || manifest.inputBoundary.shape.height == 0 ||
        manifest.inputBoundary.shape.channels == 0) {
        return Status::invalidArgument("input boundary shape is incomplete");
    }
    if (manifest.outputBoundary.shape.width == 0 || manifest.outputBoundary.shape.height == 0 ||
        manifest.outputBoundary.shape.channels == 0) {
        return Status::invalidArgument("output boundary shape is incomplete");
    }
    if (manifest.inputBoundary.shape.width != manifest.outputBoundary.shape.width ||
        manifest.inputBoundary.shape.height != manifest.outputBoundary.shape.height ||
        manifest.inputBoundary.shape.channels != manifest.outputBoundary.shape.channels) {
        return Status::invalidArgument("streaming segments currently require matching input/output token shapes");
    }
    if (manifest.streamPlan.bandRows == 0 || manifest.streamPlan.bandCount == 0) {
        return Status::invalidArgument("segment stream plan requires non-zero token-band rows and band count");
    }
    if (manifest.streamPlan.maxInputBandBytes == 0 || manifest.streamPlan.maxOutputBandBytes == 0) {
        return Status::invalidArgument("segment stream plan requires non-zero token-band slab sizes");
    }
    const std::uint32_t expectedBandCount = tokenBandCount(manifest.outputBoundary.shape, manifest.streamPlan.bandRows);
    if (manifest.streamPlan.bandCount != expectedBandCount) {
        return Status::invalidArgument("segment stream plan band count does not match the output boundary shape");
    }
    if (manifest.streamPlan.mode == TokenStreamMode::ContinuousFrameStream && manifest.streamPlan.inputHaloRows != 0) {
        return Status::invalidArgument("continuous token streams must not duplicate GPU-side halo rows");
    }
    if (manifest.streamPlan.maxSpatialKernelHeight < maxSpatialKernelHeight(manifest) ||
        manifest.streamPlan.maxSpatialKernelWidth < maxSpatialKernelWidth(manifest)) {
        return Status::invalidArgument("segment stream plan understates the stage spatial kernel footprint");
    }
    if (!manifest.stages.empty()) {
        if (manifest.stages.front().logicalStage != manifest.firstLogicalStage ||
            manifest.stages.back().logicalStage != manifest.lastLogicalStage) {
            return Status::invalidArgument("segment stage list does not match the manifest logical-stage range");
        }
        for (const SegmentStageDesc& stage : manifest.stages) {
            if (stage.name.empty() || stage.op.empty()) {
                return Status::invalidArgument("segment stages require names and op identifiers");
            }
            if (stage.inputChannels == 0 || stage.outputChannels == 0) {
                return Status::invalidArgument("segment stages require non-zero channel counts");
            }
            if (stage.spatialKernelWidth == 0 || stage.spatialKernelHeight == 0) {
                return Status::invalidArgument("segment stages require non-zero spatial kernels");
            }
        }
    }
    for (const SkipDelayDesc& skip : manifest.skipDelays) {
        if (skip.name.empty() || skip.channels == 0) {
            return Status::invalidArgument("skip delay descriptors require names and channels");
        }
        if (skip.producerLogicalStage >= skip.consumerLogicalStage) {
            return Status::invalidArgument("skip delay producer must precede its consumer");
        }
        if (skip.transport == EdgeTransportPolicy::ExternalSpillDebug) {
            return Status::invalidArgument("performance manifests must not use external spill skip delays");
        }
    }
    return Status::ok();
}

} // namespace apusr
