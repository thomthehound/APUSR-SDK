// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_tokens.h>

#include <cstdint>
#include <string>
#include <vector>

namespace apusr {

enum class ModelFamily : std::uint32_t {
    Fsr4Int8,
    Experimental,
};

enum class TokenTensorClass : std::uint32_t {
    Boundary,
    Stream,
    SkipDelay,
    ElasticInFlight,
    Parameter,
};

enum class EdgeTransportPolicy : std::uint32_t {
    DirectStream,
    BufferedDmaDelay,
    LayoutDma,
    ExternalSpillDebug,
};

enum class StageCadence : std::uint32_t {
    OneToOne,
    Downsample2x,
    Upsample2x,
};

struct SegmentStageDesc {
    std::uint32_t logicalStage = 0;
    std::string name;
    std::string op;
    StageCadence cadence = StageCadence::OneToOne;
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    std::uint32_t inputHeightDivisor = 1;
    std::uint32_t outputHeightDivisor = 1;
    std::uint32_t spatialKernelWidth = 1;
    std::uint32_t spatialKernelHeight = 1;
    EdgeTransportPolicy outputTransport = EdgeTransportPolicy::DirectStream;
    std::string producesSkip;
    std::string consumesSkip;
};

struct SkipDelayDesc {
    std::string name;
    std::uint32_t producerLogicalStage = 0;
    std::uint32_t consumerLogicalStage = 0;
    std::uint32_t channels = 0;
    std::uint32_t widthDivisor = 1;
    std::uint32_t heightDivisor = 1;
    EdgeTransportPolicy transport = EdgeTransportPolicy::BufferedDmaDelay;
    std::uint32_t recommendedLiveRows = 0;
};

struct SegmentManifest {
    std::string name;
    ModelFamily modelFamily = ModelFamily::Fsr4Int8;
    std::uint32_t firstLogicalStage = 0;
    std::uint32_t lastLogicalStage = 0;
    TokenBoundary inputBoundary;
    TokenBoundary outputBoundary;
    TokenStreamPlan streamPlan;
    std::vector<SegmentStageDesc> stages;
    std::vector<SkipDelayDesc> skipDelays;
};

struct RuntimeManifest {
    std::string name;
    std::vector<SegmentManifest> segments;
};

[[nodiscard]] Status validateSegmentManifest(const SegmentManifest& manifest);
[[nodiscard]] std::uint32_t maxSpatialKernelHeight(const SegmentManifest& manifest);
[[nodiscard]] std::uint32_t maxSpatialKernelWidth(const SegmentManifest& manifest);
[[nodiscard]] bool hasBufferedSkipDelay(const SegmentManifest& manifest);
[[nodiscard]] const char* edgeTransportPolicyName(EdgeTransportPolicy policy);
[[nodiscard]] const char* stageCadenceName(StageCadence cadence);

} // namespace apusr
