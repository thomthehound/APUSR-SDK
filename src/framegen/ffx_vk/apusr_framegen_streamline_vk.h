// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_session_vk.h"

#include <cstdint>

// Streamline-shaped Vulkan inputs normalized before they enter the FI/OF
// session.  This header intentionally does not include Streamline: the frontend
// owns ABI decoding, while this adapter owns the graphics-resource contract.
struct ApusrFramegenStreamlineVulkanResource {
    bool tagged = false;
    void* image = nullptr;
    void* imageView = nullptr;
    std::uint32_t nativeFormat = 0;
    std::uint32_t nativeLayout = 0xffffffffu;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
    std::uint32_t arrayLayers = 1;
    std::uint32_t nativeUsage = 0;
    std::uint32_t extentLeft = 0;
    std::uint32_t extentTop = 0;
    std::uint32_t extentWidth = 0;
    std::uint32_t extentHeight = 0;
    std::uint32_t aspectMask = 0;
    std::uint32_t baseMipLevel = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t layerCount = 0;
};

struct ApusrFramegenStreamlineConstants {
    bool valid = false;
    ApusrFramegenFloatCoords2D jitterOffset{};
    ApusrFramegenFloatCoords2D motionVectorScale{1.0f, 1.0f};
    bool reset = false;
    bool depthInverted = false;
    bool motionVectorsJittered = false;
    bool motionVectorsDilated = false;
    bool orthographicProjection = false;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFov = 0.0f;
    float cameraAspectRatio = 0.0f;
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
};

struct ApusrFramegenStreamlineVulkanInputs {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    void* commandBuffer = nullptr;
    std::uint64_t frameId = 0;
    float frameTimeDelta = 0.0f;
    bool hdr = false;
    ApusrFramegenBackbufferTransferFunction backbufferTransferFunction =
        APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    float minMaxLuminance[2]{};

    // The backbuffer contract may carry only an extent. The live swapchain image
    // is supplied separately as presentColor by the present adapter.
    ApusrFramegenStreamlineVulkanResource backbufferContract{};
    ApusrFramegenStreamlineVulkanResource depth{};
    ApusrFramegenStreamlineVulkanResource motionVectors{};
    ApusrFramegenStreamlineVulkanResource presentColor{};
    ApusrFramegenStreamlineVulkanResource hudlessColor{};
    ApusrFramegenStreamlineVulkanResource distortionField{};
    ApusrFramegenStreamlineVulkanResource generatedOutput{};
    ApusrFramegenStreamlineConstants constants{};
};

// Owns the metadata to which frameDescription.backendMetadata points. Keep this
// object alive until apusrFramegenDispatchVulkanSession returns.
struct ApusrFramegenAdaptedStreamlineVulkanFrame {
    ApusrFramegenVulkanSessionDescription sessionDescription{};
    ApusrFramegenVulkanFrameDescription frameDescription{};
    ApusrFramegenVulkanExternalResourceMetadata depthMetadata{};
    ApusrFramegenVulkanExternalResourceMetadata motionVectorMetadata{};
    ApusrFramegenVulkanExternalResourceMetadata presentColorMetadata{};
    ApusrFramegenVulkanExternalResourceMetadata hudlessColorMetadata{};
    ApusrFramegenVulkanExternalResourceMetadata distortionMetadata{};
    ApusrFramegenVulkanExternalResourceMetadata outputMetadata{};
};

ApusrFramegenResult apusrFramegenAdaptStreamlineVulkanFrame(
    const ApusrFramegenStreamlineVulkanInputs& inputs,
    ApusrFramegenAdaptedStreamlineVulkanFrame* outFrame) noexcept;
