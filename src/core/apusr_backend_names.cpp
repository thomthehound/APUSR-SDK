// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_graphics.h>
#include <apusr/apusr_npu.h>

namespace apusr {

const char* sharedBufferHandleKindName(const SharedBufferHandleKind kind)
{
    switch (kind) {
    case SharedBufferHandleKind::None:
        return "None";
    case SharedBufferHandleKind::Dx12Resource:
        return "Dx12Resource";
    case SharedBufferHandleKind::VulkanBuffer:
        return "VulkanBuffer";
    case SharedBufferHandleKind::HostUserPointer:
        return "HostUserPointer";
    }
    return "Unknown";
}

const char* timelineFenceHandleKindName(const TimelineFenceHandleKind kind)
{
    switch (kind) {
    case TimelineFenceHandleKind::None:
        return "None";
    case TimelineFenceHandleKind::Dx12Fence:
        return "Dx12Fence";
    case TimelineFenceHandleKind::VulkanTimelineSemaphore:
        return "VulkanTimelineSemaphore";
    case TimelineFenceHandleKind::HostTimeline:
        return "HostTimeline";
    }
    return "Unknown";
}

const char* npuVendorName(const NpuVendor vendor)
{
    switch (vendor) {
    case NpuVendor::AmdXdna:
        return "AmdXdna";
    case NpuVendor::Intel:
        return "Intel";
    case NpuVendor::Qualcomm:
        return "Qualcomm";
    case NpuVendor::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* npuRuntimeKindName(const NpuRuntimeKind kind)
{
    switch (kind) {
    case NpuRuntimeKind::AmdXrt:
        return "AmdXrt";
    case NpuRuntimeKind::VendorNative:
        return "VendorNative";
    }
    return "Unknown";
}

} // namespace apusr
