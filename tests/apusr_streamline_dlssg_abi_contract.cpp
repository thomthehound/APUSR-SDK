// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_streamline_core_abi.h"
#include "apusr_streamline_dlssg_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

struct StructType {
    std::uint32_t data1{};
    std::uint16_t data2{};
    std::uint16_t data3{};
    std::uint8_t data4[8]{};
};

struct BaseStructure {
    BaseStructure* next{};
    StructType structType{};
    std::size_t structVersion{1u};
};

enum class Mode : std::uint32_t { Off = 0, On = 1, Auto = 2, Dynamic = 3 };
enum class QueueMode : std::uint32_t { BlockPresentingQueue = 0, BlockNoClientQueues = 1 };

struct Options {
    BaseStructure* next{};
    StructType structType{};
    std::size_t structVersion{1u};
    Mode mode = Mode::Off;
    std::uint32_t numFramesToGenerate = 1u;
    std::uint32_t flags{};
    std::uint32_t dynamicResWidth{};
    std::uint32_t dynamicResHeight{};
    std::uint32_t numBackBuffers{};
    std::uint32_t mvecDepthWidth{};
    std::uint32_t mvecDepthHeight{};
    std::uint32_t colorWidth{};
    std::uint32_t colorHeight{};
    std::uint32_t colorBufferFormat{};
    std::uint32_t mvecBufferFormat{};
    std::uint32_t depthBufferFormat{};
    std::uint32_t hudLessBufferFormat{};
    std::uint32_t uiBufferFormat{};
    void* onErrorCallback{};
    char bReserved15 = 2;
    QueueMode queueParallelismMode = QueueMode::BlockPresentingQueue;
    char enableUserInterfaceRecomposition{};
    float dynamicTargetFrameRate{};
};

struct State {
    BaseStructure* next{};
    StructType structType{};
    std::size_t structVersion{1u};
    std::uint64_t estimatedVRAMUsageInBytes{};
    std::uint32_t status{};
    std::uint32_t minWidthOrHeight{};
    std::uint32_t numFramesActuallyPresented{};
    std::uint32_t numFramesToGenerateMax{};
    char bReserved4{};
    char bIsVsyncSupportAvailable{};
    void* inputsProcessingCompletionFence{};
    std::uint64_t lastPresentInputsProcessingCompletionFenceValue{};
    char bIsDynamicMFGSupported{};
};

struct TestFrameToken final : apusr::streamline::core_abi::FrameToken {
    std::uint32_t counter{};
    operator std::uint32_t() const override { return counter; }
};

struct Requirements {
    BaseStructure* next{};
    StructType structType{};
    std::size_t structVersion{1u};
    std::uint32_t vkNumComputeQueuesRequired{};
    std::uint32_t vkNumGraphicsQueuesRequired{};
    std::uint32_t vkNumOpticalFlowQueuesRequired{};
};

static_assert(sizeof(BaseStructure) == 32);
static_assert(offsetof(Options, onErrorCallback) == 96);
static_assert(offsetof(Options, bReserved15) == 104);
static_assert(sizeof(Options) == 120);
static_assert(offsetof(State, numFramesToGenerateMax) == 52);
static_assert(sizeof(State) == 88);

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    TestFrameToken frame{};
    frame.structType = apusr::streamline::core_abi::kFrameTokenStructType;
    frame.structVersion = 1u;
    frame.counter = 0x12345678u;
    std::uint64_t frameIndex = 0u;
    require(apusr::streamline::core_abi::frameIndex(&frame, frameIndex) && frameIndex == frame.counter,
            "Streamline FrameToken ABI did not resolve the token's public frame index");
    frame.structType.data1 ^= 1u;
    require(!apusr::streamline::core_abi::frameIndex(&frame, frameIndex),
            "Streamline FrameToken ABI accepted an object with the wrong struct type");

    Requirements requirements{};
    requirements.structVersion = 1u;
    requirements.vkNumOpticalFlowQueuesRequired = 0xa5a5a5a5u;
    apusr::streamline::dlssg_abi::writeLegacyVulkanQueueRoles(requirements);
    require(requirements.vkNumGraphicsQueuesRequired == 1u &&
                requirements.vkNumComputeQueuesRequired == 1u &&
                requirements.vkNumOpticalFlowQueuesRequired == 0xa5a5a5a5u,
            "DLSS-G v1 requirements writer crossed into the v2 optical-flow queue field");
    requirements.structVersion = 2u;
    apusr::streamline::dlssg_abi::writeLegacyVulkanQueueRoles(requirements);
    require(requirements.vkNumOpticalFlowQueuesRequired == 1u,
            "DLSS-G v2 requirements did not request the legacy optical-flow queue role");

    Options input{};
    input.structVersion = 1u;
    input.mode = Mode::On;
    input.numFramesToGenerate = 1u;
    input.colorWidth = 3440u;
    input.colorHeight = 1440u;
    input.bReserved15 = 1;
    input.queueParallelismMode = QueueMode::BlockNoClientQueues;
    input.enableUserInterfaceRecomposition = 1;
    input.dynamicTargetFrameRate = 240.0f;

    const Options legacy = apusr::streamline::dlssg_abi::captureOptions(input);
    require(legacy.mode == Mode::On && legacy.colorWidth == 3440u && legacy.colorHeight == 1440u,
            "DLSS-G v1 options lost fields from the original ABI");
    require(legacy.bReserved15 == 2,
            "DLSS-G v1 capture read the v2 tail");
    require(legacy.queueParallelismMode == QueueMode::BlockPresentingQueue,
            "DLSS-G v1 capture read the v3 queue-parallelism tail");
    require(legacy.enableUserInterfaceRecomposition == 0,
            "DLSS-G v1 capture read the v4 UI-recomposition tail");
    require(legacy.dynamicTargetFrameRate == 0.0f,
            "DLSS-G v1 capture read the v5 dynamic-frame-rate tail");

    input.structVersion = 3u;
    const Options v3 = apusr::streamline::dlssg_abi::captureOptions(input);
    require(v3.bReserved15 == 1 && v3.queueParallelismMode == QueueMode::BlockNoClientQueues,
            "DLSS-G v3 capture failed to consume its declared ABI tail");
    require(v3.enableUserInterfaceRecomposition == 0 && v3.dynamicTargetFrameRate == 0.0f,
            "DLSS-G v3 capture crossed into a newer ABI tail");

    input.structVersion = 5u;
    const Options v5 = apusr::streamline::dlssg_abi::captureOptions(input);
    require(v5.enableUserInterfaceRecomposition == 1 && v5.dynamicTargetFrameRate == 240.0f,
            "DLSS-G v5 capture failed to consume its declared ABI tail");

    State state{};
    state.structVersion = 1u;
    state.numFramesToGenerateMax = 0xa5a5a5a5u;
    state.bReserved4 = static_cast<char>(0x5a);
    state.bIsVsyncSupportAvailable = static_cast<char>(0x5b);
    state.inputsProcessingCompletionFence = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x11111111u));
    state.lastPresentInputsProcessingCompletionFenceValue = 0xa5a5a5a5a5a5a5a5ull;
    state.bIsDynamicMFGSupported = static_cast<char>(0x5c);

    apusr::streamline::dlssg_abi::StateValues values{};
    values.status = 0x1234u;
    values.minWidthOrHeight = 720u;
    values.numFramesActuallyPresented = 9u;
    values.numFramesToGenerateMax = 1u;
    values.bIsVsyncSupportAvailable = 1;
    values.inputsProcessingCompletionFence = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x12345678u));
    values.lastPresentInputsProcessingCompletionFenceValue = 77u;
    values.bIsDynamicMFGSupported = 1;
    apusr::streamline::dlssg_abi::writeState(state, values);

    require(state.status == 0x1234u && state.minWidthOrHeight == 720u && state.numFramesActuallyPresented == 9u,
            "DLSS-G v1 state did not receive the original ABI fields");
    require(state.numFramesToGenerateMax == 0xa5a5a5a5u &&
                static_cast<unsigned char>(state.bReserved4) == 0x5au &&
                static_cast<unsigned char>(state.bIsVsyncSupportAvailable) == 0x5bu &&
                state.inputsProcessingCompletionFence == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x11111111u)) &&
                state.lastPresentInputsProcessingCompletionFenceValue == 0xa5a5a5a5a5a5a5a5ull &&
                static_cast<unsigned char>(state.bIsDynamicMFGSupported) == 0x5cu,
            "DLSS-G v1 state writer touched fields beyond the caller's structure version");

    state = {};
    state.structVersion = 2u;
    state.bIsDynamicMFGSupported = static_cast<char>(0x5c);
    apusr::streamline::dlssg_abi::writeState(state, values);
    require(state.numFramesToGenerateMax == 1u && state.bIsVsyncSupportAvailable == 1 &&
                state.inputsProcessingCompletionFence == values.inputsProcessingCompletionFence &&
                state.lastPresentInputsProcessingCompletionFenceValue == 77u,
            "DLSS-G v2 state did not receive the MFG/fence ABI tail");
    require(static_cast<unsigned char>(state.bIsDynamicMFGSupported) == 0x5cu,
            "DLSS-G v2 state writer touched the v4 Dynamic-MFG field");

    state.structVersion = 4u;
    apusr::streamline::dlssg_abi::writeState(state, values);
    require(state.bIsDynamicMFGSupported == 1,
            "DLSS-G v4 state did not receive its Dynamic-MFG field");

    std::cout << "APUSR Streamline ABI contract: FrameToken index, legacy DLSS-G queue roles, option reads, and version-bounded state writes validated\n";
    return 0;
}
