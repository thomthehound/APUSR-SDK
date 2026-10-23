// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_fsr4_reference_vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

extern "C" void apusrFakeVulkanResetCounters();
extern "C" std::uint64_t apusrFakeVulkanDispatchCount();
extern "C" std::uint64_t apusrFakeVulkanPipelineCount();
extern "C" std::uint64_t apusrFakeVulkanRequiredSubgroup32PipelineCount();
extern "C" std::uint64_t apusrFakeVulkanDescriptorWriteCount();
extern "C" std::uint64_t apusrFakeVulkanUpdateBytes();
extern "C" std::uint64_t apusrFakeVulkanClearCount();
extern "C" std::uint64_t apusrFakeVulkanBarrierCount();
extern "C" std::uint64_t apusrFakeVulkanBarrierCallCount();
extern "C" std::uint64_t apusrFakeVulkanComputeBufferBarrierCount();
extern "C" std::uint64_t apusrFakeVulkanInvalidDescriptorLayoutCount();
extern "C" std::uint64_t apusrFakeVulkanInvalidOwnedSharingCount();
extern "C" std::uint64_t apusrFakeVulkanDeviceWaitIdleCount();
extern "C" std::uint64_t apusrFakeVulkanTimestampWriteCount();

namespace {

apusr::NativeHandle handle(std::uintptr_t value)
{
    return apusr::NativeHandle{reinterpret_cast<void*>(value)};
}

std::size_t countOccurrences(const std::string& text, const std::string& needle)
{
    if (needle.empty()) {
        return 0u;
    }
    std::size_t count = 0u;
    for (std::size_t pos = 0u; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++count;
    }
    return count;
}

bool appendSurface(
    apusr::FrameInputDesc& frame,
    apusr::FrameInputSurfaceRole role,
    std::uintptr_t image,
    std::uintptr_t view,
    apusr::SurfaceFormat format,
    apusr::RenderSize size,
    std::uint64_t layout)
{
    apusr::FrameInputSurfaceDesc surface{};
    surface.role = role;
    surface.resource = handle(image);
    surface.view = handle(view);
    surface.format = format;
    surface.size = size;
    surface.nativeFormat = 1;
    surface.nativeAspectMask = 1;
    surface.nativeStateOrLayout = layout;
    const apusr::Status status = apusr::appendFrameInputSurface(frame, surface);
    if (!status.succeeded()) {
        std::cerr << "appendFrameInputSurface failed: " << status.message << "\n";
        return false;
    }
    return true;
}


} // namespace

int main()
{
    using namespace apusr;
    using namespace apusr::backends;
    using namespace apusr::models::fsr4_int8;

    const std::filesystem::path timestampDir = std::filesystem::temp_directory_path() / "apusr_vulkan_timestamp_smoke";
    std::error_code timestampEc;
    std::filesystem::remove_all(timestampDir, timestampEc);
    std::filesystem::create_directories(timestampDir, timestampEc);
#if defined(_WIN32)
    _putenv_s("APUSR_VULKAN_GPU_TIMESTAMPS", "1");
    _putenv_s("APUSR_LOG_DIR", timestampDir.string().c_str());
#else
    setenv("APUSR_VULKAN_GPU_TIMESTAMPS", "1", 1);
    setenv("APUSR_LOG_DIR", timestampDir.string().c_str(), 1);
#endif

    VulkanFsr4ReferenceCreateDesc create{};
    create.instance = handle(0x1000);
    create.physicalDevice = handle(0x2000);
    create.device = handle(0x3000);
    create.graphicsQueueFamilyIndex = 3u;
    create.computeQueueFamilyIndex = 5u;
    create.repoRoot = APUSR_REPO_ROOT;
    create.shaderPackPath = std::filesystem::path(APUSR_REPO_ROOT) / "tests/data/fsr4_vulkan_p459.pack";
    create.shaderPack = {};
    create.runner.pipelineVariant = PipelineVariant::Dp4aOptimized;
    create.runner.maxRenderSize = RenderSize{1920, 1080};
    create.runner.maxUpscaleSize = DisplaySize{3840, 2160};
    create.runner.initialPreset = Preset::Performance;
    create.runner.features.autoExposure = true;
    create.runner.features.depthInverted = true;
    create.runner.allowDebugVisualization = false;
    create.runner.supportsWmma = false;
    create.requiredSubgroupSize = 32u;

    VulkanFsr4ReferenceFrameDesc frame{};
    frame.commandBuffer = handle(0x4000);
    frame.frameInput.command.api = FrameInputApi::Vulkan;
    frame.frameInput.command.device = create.device;
    frame.frameInput.command.commandListOrBuffer = frame.commandBuffer;
    frame.frameInput.command.nativeQueueFamilyIndex = 3;
    frame.frameInput.constants.renderSize = create.runner.maxRenderSize;
    frame.frameInput.constants.displaySize = create.runner.maxUpscaleSize;
    frame.frameInput.constants.motionVectorSize = create.runner.maxRenderSize;
    frame.frameInput.options.qualityMode = UpscalerQualityMode::Performance;
    frame.frameInput.options.features.autoExposure = true;
    frame.frameInput.options.features.depthInverted = true;
    frame.frameInput.options.hasDepth = true;
    frame.frameInput.options.hasExposure = false;
    frame.frameInput.options.sharpeningRequested = true;
    frame.frameInput.options.sharpness = 0.5f;
    frame.frameInput.options.preExposure = 1.0f;

    constexpr std::uint64_t kShaderReadOnly = 5;
    constexpr std::uint64_t kGeneral = 1;
    if (!appendSurface(frame.frameInput, FrameInputSurfaceRole::Color, 0x5000, 0x5100, SurfaceFormat::Rgba16Float, {1920, 1080}, kGeneral) ||
        !appendSurface(frame.frameInput, FrameInputSurfaceRole::Depth, 0x5200, 0x5300, SurfaceFormat::R32Float, {1920, 1080}, kShaderReadOnly) ||
        !appendSurface(frame.frameInput, FrameInputSurfaceRole::MotionVectors, 0x5400, 0x5500, SurfaceFormat::Rg16Float, {1920, 1080}, kShaderReadOnly) ||
        !appendSurface(frame.frameInput, FrameInputSurfaceRole::Output, 0x5600, 0x5700, SurfaceFormat::Rgba16Float, {3840, 2160}, kGeneral)) {
        return 1;
    }

    Status status = makeRuntimeFrameDescFromFrameInput(frame.frameInput, frame.runner.frame);
    if (!status.succeeded()) {
        std::cerr << "makeRuntimeFrameDescFromFrameInput failed: " << status.message << "\n";
        return 1;
    }
    frame.runner.maxUpscaleSize = create.runner.maxUpscaleSize;
    frame.runner.residentModelPreset = Preset::Performance;
    frame.runner.forceReset = true;

    const VulkanFsr4ReferenceRecordPlan plan = makeVulkanFsr4ReferenceRecordPlan(create, frame);
    if (plan.framePlan.permutationKey != 459u || plan.dispatches.size() != 16u) {
        std::cerr << "unexpected Vulkan product slice: permutation=" << plan.framePlan.permutationKey
                  << " dispatches=" << plan.dispatches.size() << "\n";
        return 1;
    }
    apusrFakeVulkanResetCounters();
    VulkanFsr4ReferenceCreateDesc invalidSubgroupCreate = create;
    invalidSubgroupCreate.requiredSubgroupSize = 48u;
    auto invalidSubgroupRuntime = createVulkanFsr4ReferenceRuntime();
    status = invalidSubgroupRuntime->initialize(invalidSubgroupCreate);
    if (status.succeeded()) {
        std::cerr << "Vulkan accepted unsupported required subgroup size 48\n";
        return 1;
    }

    auto runtime = createVulkanFsr4ReferenceRuntime();
    status = runtime->initialize(create);
    if (!status.succeeded()) {
        std::cerr << "Vulkan initialize failed: " << status.message << "\n";
        return 1;
    }
    if (apusrFakeVulkanInvalidOwnedSharingCount() != 0u) {
        std::cerr << "Vulkan owned resources did not use the Streamline graphics/compute sharing contract\n";
        return 1;
    }
    status = runtime->prepareFrameCache(frame);
    if (!status.succeeded()) {
        std::cerr << "Vulkan prepareFrameCache failed: " << status.message << "\n";
        return 1;
    }
    if (apusrFakeVulkanPipelineCount() != 16u ||
        apusrFakeVulkanRequiredSubgroup32PipelineCount() != 16u) {
        std::cerr << "expected 16 cached wave32 compute pipelines, got pipelines="
                  << apusrFakeVulkanPipelineCount()
                  << " wave32=" << apusrFakeVulkanRequiredSubgroup32PipelineCount() << "\n";
        return 1;
    }

    status = runtime->recordFrame(frame);
    if (!status.succeeded()) {
        std::cerr << "Vulkan first recordFrame failed: " << status.message << "\n";
        return 1;
    }
    const std::uint64_t writesAfterFirst = apusrFakeVulkanDescriptorWriteCount();
    const std::uint64_t updatesAfterFirst = apusrFakeVulkanUpdateBytes();
    const std::uint64_t computeBufferBarriersAfterFirst = apusrFakeVulkanComputeBufferBarrierCount();
    const std::uint64_t barrierCallsAfterFirst = apusrFakeVulkanBarrierCallCount();
    const std::uint64_t barrierObjectsAfterFirst = apusrFakeVulkanBarrierCount();
    if (apusrFakeVulkanDispatchCount() != 16u || writesAfterFirst == 0u || updatesAfterFirst < 90000u ||
        apusrFakeVulkanClearCount() != 2u || barrierObjectsAfterFirst == 0u ||
        barrierCallsAfterFirst >= barrierObjectsAfterFirst ||
        apusrFakeVulkanInvalidDescriptorLayoutCount() != 0u) {
        std::cerr << "first Vulkan record did not exercise the complete graph/resources"
                  << " dispatches=" << apusrFakeVulkanDispatchCount()
                  << " descriptorWrites=" << writesAfterFirst
                  << " updateBytes=" << updatesAfterFirst
                  << " clears=" << apusrFakeVulkanClearCount()
                  << " barriers=" << apusrFakeVulkanBarrierCount()
                  << " invalidDescriptorLayouts=" << apusrFakeVulkanInvalidDescriptorLayoutCount()
                  << " barrierCalls=" << barrierCallsAfterFirst << "\n";
        return 1;
    }
    const std::uint64_t timestampWritesAfterFirst = apusrFakeVulkanTimestampWriteCount();
    if (timestampWritesAfterFirst == 0u) {
        std::cerr << "Vulkan GPU timestamp profiler did not sample the first frame\n";
        return 1;
    }

    frame.runner.forceReset = false;
    frame.runner.frame.frameIndex = 1;
    status = runtime->recordFrame(frame);
    if (!status.succeeded()) {
        std::cerr << "Vulkan second recordFrame failed: " << status.message << "\n";
        return 1;
    }
    const std::uint64_t warmBarrierCalls = apusrFakeVulkanBarrierCallCount() - barrierCallsAfterFirst;
    const std::uint64_t warmBarrierObjects = apusrFakeVulkanBarrierCount() - barrierObjectsAfterFirst;
    if (apusrFakeVulkanDispatchCount() != 32u || apusrFakeVulkanPipelineCount() != 16u ||
        apusrFakeVulkanDescriptorWriteCount() != writesAfterFirst ||
        apusrFakeVulkanUpdateBytes() - updatesAfterFirst != 1792u ||
        apusrFakeVulkanClearCount() != 3u ||
        apusrFakeVulkanComputeBufferBarrierCount() <= 2u * computeBufferBarriersAfterFirst ||
        warmBarrierCalls >= warmBarrierObjects) {
        std::cerr << "Vulkan warm-frame cache/reuse contract failed"
                  << " dispatches=" << apusrFakeVulkanDispatchCount()
                  << " pipelines=" << apusrFakeVulkanPipelineCount()
                  << " descriptorWrites=" << apusrFakeVulkanDescriptorWriteCount()
                  << " updateDelta=" << (apusrFakeVulkanUpdateBytes() - updatesAfterFirst)
                  << " clears=" << apusrFakeVulkanClearCount()
                  << " computeBufferBarriers=" << apusrFakeVulkanComputeBufferBarrierCount()
                  << " firstComputeBufferBarriers=" << computeBufferBarriersAfterFirst
                  << " barrierCalls=" << apusrFakeVulkanBarrierCallCount()
                  << " firstBarrierCalls=" << barrierCallsAfterFirst
                  << " warmBarrierCalls=" << warmBarrierCalls
                  << " warmBarrierObjects=" << warmBarrierObjects << "\n";
        return 1;
    }

    const std::uint64_t timestampWritesAfterSecond = apusrFakeVulkanTimestampWriteCount();
    if (timestampWritesAfterSecond <= timestampWritesAfterFirst) {
        std::cerr << "Vulkan GPU timestamp profiler did not continue the three-frame sample burst"
                  << " firstWrites=" << timestampWritesAfterFirst
                  << " secondWrites=" << timestampWritesAfterSecond << "\n";
        return 1;
    }

    frame.runner.frame.frameIndex = 2;
    status = runtime->recordFrame(frame);
    if (!status.succeeded()) {
        std::cerr << "Vulkan third recordFrame failed: " << status.message << "\n";
        return 1;
    }
    const std::uint64_t timestampWritesAfterThird = apusrFakeVulkanTimestampWriteCount();
    if (timestampWritesAfterThird <= timestampWritesAfterSecond) {
        std::cerr << "Vulkan GPU timestamp profiler did not capture the third frame in the burst\n";
        return 1;
    }

    frame.runner.frame.frameIndex = 3;
    status = runtime->recordFrame(frame);
    if (!status.succeeded()) {
        std::cerr << "Vulkan fourth recordFrame failed: " << status.message << "\n";
        return 1;
    }
    if (apusrFakeVulkanTimestampWriteCount() != timestampWritesAfterThird) {
        std::cerr << "Vulkan GPU timestamp profiler exceeded the three-frame burst before the one-second interval"
                  << " thirdWrites=" << timestampWritesAfterThird
                  << " fourthWrites=" << apusrFakeVulkanTimestampWriteCount() << "\n";
        return 1;
    }

    VulkanFsr4ReferenceFrameDesc wrongQueueFamily = frame;
    wrongQueueFamily.frameInput.command.nativeQueueFamilyIndex = 4;
    status = runtime->recordFrame(wrongQueueFamily);
    if (status.succeeded() || apusrFakeVulkanDispatchCount() != 64u) {
        std::cerr << "Vulkan queue-family affinity contract was not enforced\n";
        return 1;
    }

    runtime.reset();
    if (apusrFakeVulkanDeviceWaitIdleCount() != 1u) {
        std::cerr << "Vulkan runtime destruction did not wait for recorded GPU work\n";
        return 1;
    }
    const std::filesystem::path timestampCsv = timestampDir / "apusr_vulkan_gpu_timestamps.csv";
    auto readTimestampCsv = [&]() {
        std::ifstream timestampFile(timestampCsv);
        std::ostringstream timestampText;
        timestampText << timestampFile.rdbuf();
        if (!timestampFile.good() && !timestampFile.eof()) {
            return std::string{};
        }
        return timestampText.str();
    };
    const std::string firstTimestampCsvText = readTimestampCsv();
    if (firstTimestampCsvText.empty()) {
        std::cerr << "Vulkan GPU timestamp profiler did not create its CSV\n";
        return 1;
    }
    if (firstTimestampCsvText.find("timestamp_period_ns") == std::string::npos ||
        firstTimestampCsvText.find("api_version") == std::string::npos ||
        firstTimestampCsvText.find("total_workgroups") == std::string::npos ||
        firstTimestampCsvText.find("color_native_format") == std::string::npos ||
        firstTimestampCsvText.find("queue_family") == std::string::npos ||
        firstTimestampCsvText.find("shader_bytes") == std::string::npos ||
        firstTimestampCsvText.find("barrier_calls") == std::string::npos ||
        firstTimestampCsvText.find("constants_ticks") == std::string::npos ||
        firstTimestampCsvText.find("cpu_frame_interval_us") == std::string::npos ||
        firstTimestampCsvText.find("cpu_total_us") == std::string::npos ||
        firstTimestampCsvText.find(",frame,-1,__frame__,") == std::string::npos ||
        firstTimestampCsvText.find(",dispatch,0,") == std::string::npos ||
        countOccurrences(firstTimestampCsvText, ",frame,-1,__frame__,") != 3u) {
        std::cerr << "Vulkan GPU timestamp CSV did not contain one complete three-frame burst with timing/context metadata\n";
        return 1;
    }

    // A Streamline quality/preset change recreates this runtime. That must append a
    // fresh three-frame burst instead of truncating all samples from earlier modes.
    frame.runner.frame.frameIndex = 4;
    auto recreatedRuntime = createVulkanFsr4ReferenceRuntime();
    status = recreatedRuntime->initialize(create);
    if (!status.succeeded()) {
        std::cerr << "Vulkan recreated runtime initialize failed: " << status.message << "\n";
        return 1;
    }
    status = recreatedRuntime->prepareFrameCache(frame);
    if (!status.succeeded()) {
        std::cerr << "Vulkan recreated runtime prepareFrameCache failed: " << status.message << "\n";
        return 1;
    }
    for (std::uint64_t frameIndex = 4u; frameIndex < 7u; ++frameIndex) {
        frame.runner.frame.frameIndex = frameIndex;
        status = recreatedRuntime->recordFrame(frame);
        if (!status.succeeded()) {
            std::cerr << "Vulkan recreated runtime recordFrame failed: " << status.message << "\n";
            return 1;
        }
    }
    recreatedRuntime.reset();

    const std::string appendedTimestampCsvText = readTimestampCsv();
    if (countOccurrences(appendedTimestampCsvText, "sequence,runtime_sequence,api_version,driver_version,vendor_id,device_id,burst_sequence,") != 1u ||
        countOccurrences(appendedTimestampCsvText, ",frame,-1,__frame__,") != 6u) {
        std::cerr << "Vulkan GPU timestamp CSV did not preserve three-frame bursts across runtime recreation"
                  << " headers=" << countOccurrences(appendedTimestampCsvText, "sequence,runtime_sequence,api_version,driver_version,vendor_id,device_id,burst_sequence,")
                  << " frames=" << countOccurrences(appendedTimestampCsvText, ",frame,-1,__frame__,") << "\n";
        return 1;
    }

    std::cout << "APUSR Vulkan FSR4 recorder smoke passed: mapped shader-pack + graph reuse + batched hazards + Streamline queue-family sharing + GPU timestamps + safe retirement"
              << " barrierCalls=" << apusrFakeVulkanBarrierCallCount()
              << " barriers=" << apusrFakeVulkanBarrierCount() << "\n";
    return 0;
}
