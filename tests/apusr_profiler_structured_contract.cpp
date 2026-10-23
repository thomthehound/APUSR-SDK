// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_profiler.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace {

void setEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main()
{
#if !APUSR_ENABLE_PROFILER
    return 0;
#else
    namespace fs = std::filesystem;
    const fs::path output = fs::current_path() / "profiler-structured-contract-output";
    std::error_code ec;
    fs::remove_all(output, ec);
    fs::create_directories(output, ec);
    if (ec) {
        std::cerr << "failed to create profiler contract output directory\n";
        return 1;
    }

    setEnvironment("APUSR_PROFILE", "full");
    setEnvironment("APUSR_PROFILE_CPU", "1");
    setEnvironment("APUSR_PROFILE_GPU", "1");
    setEnvironment("APUSR_PROFILE_RING_MIB", "4");
    setEnvironment("APUSR_PROFILE_DIR", output.string().c_str());
    setEnvironment("APUSR_PROFILE_SESSION", "structured-contract");

    const auto stream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::FrameGeneration,
        .name = "profiler.contract",
        .capabilities = apusr::profile::StreamCapabilityFrames |
            apusr::profile::StreamCapabilityGpuIntervals |
            apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityErrors |
            apusr::profile::StreamCapabilityQueueEvents |
            apusr::profile::StreamCapabilityDecisions |
            apusr::profile::StreamCapabilityStateTransitions |
            apusr::profile::StreamCapabilityResourceEvents |
            apusr::profile::StreamCapabilityFlows |
            apusr::profile::StreamCapabilityThreadInfo |
            apusr::profile::StreamCapabilityWaitIntervals |
            apusr::profile::StreamCapabilityMetadata |
            apusr::profile::StreamCapabilityGpuWorkloadInfo,
    });
    if (!stream) {
        std::cerr << "profiler stream registration failed\n";
        return 2;
    }

    // A second logical stream deliberately reuses the same semantic name and
    // local resource ID. Offline resource accounting must still keep the two
    // ledgers separate by stream identity.
    const auto resourcePeerStream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::FrameGeneration,
        .name = "profiler.contract",
        .capabilities = apusr::profile::StreamCapabilityResourceEvents,
    });
    if (!resourcePeerStream) {
        std::cerr << "profiler peer stream registration failed\n";
        return 23;
    }

    // Same backend/component, distinct frame-phase names. The human report
    // must never average these into one misleading Frame Generation cost.
    const auto computePhaseStream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::FrameGeneration,
        .name = "profiler.contract.compute",
        .capabilities = apusr::profile::StreamCapabilityFrames,
    });
    const auto preparePhaseStream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::FrameGeneration,
        .name = "profiler.contract.prepare",
        .capabilities = apusr::profile::StreamCapabilityFrames,
    });
    if (!computePhaseStream || !preparePhaseStream) {
        std::cerr << "profiler phase stream registration failed\n";
        return 27;
    }

    const auto pipelineActivityStream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::GraphicsInterop,
        .name = "vulkan.pipeline_activity",
        .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
            apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityMetadata,
    });
    const auto reflexStream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Dx12,
        .component = apusr::profile::Component::Frontend,
        .name = "streamline.reflex",
        .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
            apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityStateTransitions,
    });
    if (!pipelineActivityStream || !reflexStream) {
        std::cerr << "optimization profiler stream registration failed\n";
        return 26;
    }

    apusr::profile::recordDecision({
        .stream = stream,
        .frameId = 17,
        .correlationId = 101,
        .name = "framegen.source_disposition",
        .reason = "history_gap",
        .outcome = apusr::profile::DecisionOutcome::Warmup,
        .value0 = 17,
        .value1 = 16,
        .flags = apusr::profile::DecisionTriggerBurst,
    });
    apusr::profile::recordStateTransition({
        .stream = stream,
        .frameId = 17,
        .name = "framegen.enabled",
        .reason = "contract",
        .previousValue = 0,
        .nextValue = 1,
        .subject = 7,
        .epoch = 3,
    });
    apusr::profile::recordResourceEvent({
        .stream = stream,
        .frameId = 17,
        .name = "framegen.effect_resource",
        .detail = "image",
        .operation = apusr::profile::ResourceOperation::Allocate,
        .resourceId = 0x1234,
        .nativeHandle = 0x5678,
        .bytes = 4096,
        .alignment = 256,
        .heapOrMemoryType = 2,
    });
    apusr::profile::recordResourceEvent({
        .stream = resourcePeerStream,
        .frameId = 17,
        .name = "framegen.effect_resource",
        .detail = "peer-image",
        .operation = apusr::profile::ResourceOperation::Allocate,
        .resourceId = 0x1234,
        .nativeHandle = 0x9abc,
        .bytes = 8192,
        .alignment = 256,
        .heapOrMemoryType = 3,
    });

    apusr::profile::recordFlowEvent({
        .stream = stream,
        .frameId = 17,
        .occurrenceId = 1701,
        .flowId = 55,
        .parentFlowId = 11,
        .name = "runtime.execute_frame",
        .detail = "begin",
        .phase = apusr::profile::FlowPhase::Begin,
        .value0 = 99,
    });

    apusr::profile::recordFlowEvent({
        .stream = stream,
        .frameId = 17,
        .occurrenceId = 1701,
        .flowId = 55,
        .parentFlowId = 11,
        .name = "runtime.execute_frame",
        .detail = "end",
        .phase = apusr::profile::FlowPhase::End,
        .value0 = 1234,
    });
    apusr::profile::recordThreadInfo({
        .stream = stream,
        .name = "Profiler Contract Worker",
        .role = "test.worker",
        .priority = 2,
        .affinityMask = 0x3,
    });
    apusr::profile::recordQueueEvent({
        .stream = stream,
        .frameId = 17,
        .sampleSequence = 55,
        .name = "profiler.contract.signal",
        .nativeQueue = 0xdef,
        .syncObject = 0xabc,
        .syncValue = 9,
        .operation = apusr::profile::QueueOperation::Signal,
    });
    const std::uint64_t waitStart = apusr::profile::cpuTick();
    const std::uint64_t waitEnd = waitStart + (apusr::profile::cpuFrequency() / 1000u);
    apusr::profile::recordWaitInterval({
        .stream = stream,
        .frameId = 17,
        .correlationId = 55,
        .name = "profiler.contract.wait",
        .detail = "fence_ready",
        .kind = apusr::profile::WaitKind::GpuFence,
        .result = apusr::profile::WaitResult::Satisfied,
        .startCpuTick = waitStart,
        .endCpuTick = waitEnd,
        .object = 0xabc,
        .value = 9,
        .timeoutNs = 5'000'000,
        .observedValue = 9,
    });
    apusr::profile::recordMetadata({
        .stream = stream,
        .frameId = 17,
        .name = "profiler.contract.metadata",
        .detail = "contract-value",
        .value0 = 42,
        .value1 = 7,
    });
    apusr::profile::recordGpuWorkloadInfo({
        .stream = stream,
        .name = "profiler.contract.kernel",
        .detail = "spirv.compute",
        .workloadId = 0x44,
        .nativePipeline = 0x9988,
        .codeHash = 0x12345678abcdef00ull,
        .codeBytes = 8192,
        .sharedMemoryBytes = 2048,
        .threadsX = 8,
        .threadsY = 8,
        .threadsZ = 1,
        .subgroupSize = 64,
        .flags = 3,
    });
    // Different display-observation groups intentionally expose different
    // optional columns in display_timing.csv. The reporter must union those
    // schemas rather than assuming the first row defines every field.
    apusr::profile::recordCounter({
        .stream = stream,
        .frameId = 18,
        .name = "display.dxgi_sync_qpc",
        .value = 123456,
        .value2 = 18,
    });
    apusr::profile::recordCounter({
        .stream = stream,
        .frameId = 19,
        .name = "display.present_to_sync_ns",
        .value = 2'000'000,
        .value2 = 1'000'000,
    });

    apusr::profile::recordFrameSample({
        .stream = computePhaseStream,
        .frameId = 21,
        .sampleSequence = 56,
        .configurationHash = 0xabcdef,
        .renderWidth = 1280,
        .renderHeight = 720,
        .displayWidth = 1920,
        .displayHeight = 1080,
        .gpuDurationNs = 4'000'000,
        .gpuBeginTick = 10'000,
        .gpuEndTick = 14'000,
        .gpuFrequency = 1'000'000,
    });
    apusr::profile::recordFrameSample({
        .stream = preparePhaseStream,
        .frameId = 21,
        .sampleSequence = 56,
        .configurationHash = 0xabcdef,
        .renderWidth = 1280,
        .renderHeight = 720,
        .displayWidth = 1920,
        .displayHeight = 1080,
        .gpuDurationNs = 1'000'000,
        .gpuBeginTick = 20'000,
        .gpuEndTick = 21'000,
        .gpuFrequency = 1'000'000,
    });

    apusr::profile::recordFrameSample({
        .stream = stream,
        .frameId = 17,
        .sampleSequence = 55,
        .burstSequence = 1,
        .configurationHash = 0xabcdef,
        .burstFrame = 0,
        .renderWidth = 1280,
        .renderHeight = 720,
        .displayWidth = 1920,
        .displayHeight = 1080,
        .gpuBeginTick = 1000,
        .gpuEndTick = 2000,
        .gpuFrequency = 0,
    });
    apusr::profile::recordGpuInterval({
        .stream = stream,
        .frameId = 17,
        .sampleSequence = 55,
        .name = "profiler.contract.invalid_frequency",
        .gpuBeginTick = 1100,
        .gpuExecutionBeginTick = 1200,
        .gpuEndTick = 1900,
        .gpuFrequency = 0,
    });
    apusr::profile::recordResourceEvent({
        .stream = stream,
        .frameId = 17,
        .name = "framegen.effect_resource",
        .detail = "image",
        .operation = apusr::profile::ResourceOperation::Free,
        .resourceId = 0x1234,
        .nativeHandle = 0x5678,
        .bytes = 4096,
        .alignment = 256,
        .heapOrMemoryType = 2,
    });

    apusr::profile::recordResourceEvent({
        .stream = resourcePeerStream,
        .frameId = 17,
        .name = "framegen.effect_resource",
        .detail = "peer-image",
        .operation = apusr::profile::ResourceOperation::Free,
        .resourceId = 0x1234,
        .nativeHandle = 0x9abc,
        .bytes = 8192,
        .alignment = 256,
        .heapOrMemoryType = 3,
    });

    // Synthetic cold-path Vulkan driver activity validates the optimization
    // attribution report without requiring a Windows/Vulkan host in CI.
    apusr::profile::recordMetadata({
        .stream = pipelineActivityStream,
        .frameId = 200,
        .name = "vulkan.pipeline.callsite_module",
        .detail = "contract-host.exe",
        .value0 = 0x777,
    });
    const std::uint64_t pipelineTick = apusr::profile::cpuTick();
    const std::uint64_t oneMs = apusr::profile::cpuFrequency() / 1000u;
    apusr::profile::recordCpuInterval({
        .stream = pipelineActivityStream,
        .frameId = 200,
        .startCpuTick = pipelineTick,
        .endCpuTick = pipelineTick + oneMs,
        .name = "vk.create_shader_module",
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 200,
        .name = "vulkan.shader_module.create",
        .value = 4096,
        .value2 = 0x12345678abcdef00ull,
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 200,
        .name = "vulkan.shader_module.result",
        .value = 0,
        .value2 = 0x888,
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 200,
        .name = "vulkan.pipeline.callsite",
        .value = 0x777,
    });
    apusr::profile::recordCpuInterval({
        .stream = pipelineActivityStream,
        .frameId = 201,
        .startCpuTick = pipelineTick + 2u * oneMs,
        .endCpuTick = pipelineTick + 5u * oneMs,
        .name = "vk.create_compute_pipelines",
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 201,
        .name = "vulkan.pipeline.compute.create",
        .value = 2,
        .value2 = 0,
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 201,
        .name = "vulkan.pipeline.compute.result",
        .value = 0,
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 201,
        .name = "vulkan.pipeline.callsite",
        .value = 0x777,
    });
    apusr::profile::recordCpuInterval({
        .stream = pipelineActivityStream,
        .frameId = 202,
        .startCpuTick = pipelineTick + 6u * oneMs,
        .endCpuTick = pipelineTick + 7u * oneMs,
        .name = "vk.create_pipeline_cache",
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 202,
        .name = "vulkan.pipeline_cache.create",
        .value = 1024,
        .value2 = 0x999,
        .flags = 1,
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 202,
        .name = "vulkan.pipeline_cache.result",
        .value = 0,
    });
    apusr::profile::recordCounter({
        .stream = pipelineActivityStream,
        .frameId = 202,
        .name = "vulkan.pipeline.callsite",
        .value = 0x777,
    });

    apusr::profile::recordStateTransition({
        .stream = reflexStream,
        .frameId = 300,
        .name = "streamline.reflex.mode",
        .reason = "contract",
        .previousValue = 0,
        .nextValue = 1,
    });
    const std::uint64_t reflexTick = apusr::profile::cpuTick();
    apusr::profile::recordCpuInterval({
        .stream = reflexStream,
        .frameId = 300,
        .startCpuTick = reflexTick,
        .endCpuTick = reflexTick + 2u * oneMs,
        .name = "reflex.d3d12.update",
    });

    // Error-path flushing is intentionally rate limited in production, but the
    // first error also gives this contract test a cross-platform durability
    // boundary before it opens the mapped trace through a second file handle.
    apusr::profile::recordError(
        stream, 17, "profiler.contract.flush", apusr::StatusCode::RuntimeFailure,
        "structured profiler contract flush");

    fs::path trace;
    for (const auto& entry : fs::directory_iterator(output)) {
        if (entry.path().extension() == ".aprof") {
            trace = entry.path();
            break;
        }
    }
    if (trace.empty()) {
        std::cerr << "profiler did not create an .aprof trace\n";
        return 3;
    }

    constexpr std::size_t kHeaderBytes = 4096;
    constexpr std::size_t kRecordBytes = 256;
    constexpr std::size_t kTypeOffset = 136;
    std::ifstream input(trace, std::ios::binary);
    if (!input) {
        std::cerr << "failed to reopen profiler trace\n";
        return 4;
    }
    input.seekg(static_cast<std::streamoff>(kHeaderBytes));
    std::set<std::uint32_t> types;
    std::array<std::byte, kRecordBytes> record{};
    for (unsigned i = 0; i < 64 && input.read(reinterpret_cast<char*>(record.data()), record.size()); ++i) {
        std::uint32_t type = 0u;
        std::memcpy(&type, record.data() + kTypeOffset, sizeof(type));
        if (type != 0u)
            types.insert(type);
    }

    for (const std::uint32_t required : {3u, 4u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u}) {
        if (!types.contains(required)) {
            std::cerr << "structured profiler record type " << required << " was not persisted\n";
            return 5;
        }
    }
    return 0;
#endif
}
