// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>

namespace apusr::streamline::dlssg_abi {

constexpr std::size_t kStructVersion1 = 1u;
constexpr std::size_t kStructVersion2 = 2u;
constexpr std::size_t kStructVersion3 = 3u;
constexpr std::size_t kStructVersion4 = 4u;
constexpr std::size_t kStructVersion5 = 5u;

template <typename Requirements>
void writeLegacyVulkanQueueRoles(Requirements& output) noexcept
{
    // Streamline 2.7.x Vulkan integrations can require all three queue-role
    // vectors to be populated before they query DLSS-G state. APUSR needs one
    // queue per role for that host contract; its optical flow remains ordinary
    // Vulkan compute rather than VK_NV_optical_flow.
    output.vkNumGraphicsQueuesRequired = 1u;
    output.vkNumComputeQueuesRequired = 1u;
    if (output.structVersion >= kStructVersion2)
        output.vkNumOpticalFlowQueuesRequired = 1u;
}

template <typename Options>
[[nodiscard]] Options captureOptions(const Options& input) noexcept
{
    Options captured{};
    captured.next = nullptr;
    captured.structType = input.structType;
    captured.structVersion = input.structVersion;

    // DLSSGOptions v1 is the compatibility floor used by pre-MFG Streamline
    // integrations. Every access below this point through onErrorCallback is
    // part of that original ABI.
    captured.mode = input.mode;
    captured.numFramesToGenerate = input.numFramesToGenerate;
    captured.flags = input.flags;
    captured.dynamicResWidth = input.dynamicResWidth;
    captured.dynamicResHeight = input.dynamicResHeight;
    captured.numBackBuffers = input.numBackBuffers;
    captured.mvecDepthWidth = input.mvecDepthWidth;
    captured.mvecDepthHeight = input.mvecDepthHeight;
    captured.colorWidth = input.colorWidth;
    captured.colorHeight = input.colorHeight;
    captured.colorBufferFormat = input.colorBufferFormat;
    captured.mvecBufferFormat = input.mvecBufferFormat;
    captured.depthBufferFormat = input.depthBufferFormat;
    captured.hudLessBufferFormat = input.hudLessBufferFormat;
    captured.uiBufferFormat = input.uiBufferFormat;
    captured.onErrorCallback = input.onErrorCallback;

    if (input.structVersion >= kStructVersion2)
        captured.bReserved15 = input.bReserved15;
    if (input.structVersion >= kStructVersion3)
        captured.queueParallelismMode = input.queueParallelismMode;
    if (input.structVersion >= kStructVersion4)
        captured.enableUserInterfaceRecomposition = input.enableUserInterfaceRecomposition;
    if (input.structVersion >= kStructVersion5)
        captured.dynamicTargetFrameRate = input.dynamicTargetFrameRate;

    return captured;
}

struct StateValues {
    std::uint64_t estimatedVRAMUsageInBytes = 0u;
    std::uint32_t status = 0u;
    std::uint32_t minWidthOrHeight = 0u;
    std::uint32_t numFramesActuallyPresented = 0u;
    std::uint32_t numFramesToGenerateMax = 1u;
    char bReserved4 = 0;
    char bIsVsyncSupportAvailable = 0;
    void* inputsProcessingCompletionFence = nullptr;
    std::uint64_t lastPresentInputsProcessingCompletionFenceValue = 0u;
    char bIsDynamicMFGSupported = 0;
};

template <typename State>
void writeState(State& output, const StateValues& values) noexcept
{
    // DLSSGState v1 ends at numFramesActuallyPresented. Do not touch newer
    // members unless the caller explicitly advertises the corresponding
    // structure version; older games allocate only the older structure size.
    output.estimatedVRAMUsageInBytes = values.estimatedVRAMUsageInBytes;
    output.status = values.status;
    output.minWidthOrHeight = values.minWidthOrHeight;
    output.numFramesActuallyPresented = values.numFramesActuallyPresented;

    if (output.structVersion >= kStructVersion2) {
        output.numFramesToGenerateMax = values.numFramesToGenerateMax;
        output.bReserved4 = values.bReserved4;
        output.bIsVsyncSupportAvailable = values.bIsVsyncSupportAvailable;
        output.inputsProcessingCompletionFence = values.inputsProcessingCompletionFence;
        output.lastPresentInputsProcessingCompletionFenceValue =
            values.lastPresentInputsProcessingCompletionFenceValue;
    }
    if (output.structVersion >= kStructVersion4)
        output.bIsDynamicMFGSupported = values.bIsDynamicMFGSupported;
}

} // namespace apusr::streamline::dlssg_abi
