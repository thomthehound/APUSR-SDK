// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_host_capture.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <unknwn.h>
#include <windows.h>
#endif

namespace apusr::frontends {
namespace {

struct HostQueueRecord {
    GraphicsApi api = GraphicsApi::Dx12;
    NativeHandle device;
    NativeHandle queue;
    NativeHandle swapChain;
    HostCaptureQueueKind queueKind = HostCaptureQueueKind::Unknown;
    HostCaptureSourceKind source = HostCaptureSourceKind::ExplicitNativeConfig;
    std::uint32_t nodeMask = 0;
    std::uint64_t firstSeenFrame = 0;
    std::uint64_t lastSeenFrame = 0;
    std::uint64_t serial = 0;
};

struct ApiHandleKey {
    GraphicsApi api = GraphicsApi::Dx12;
    std::uintptr_t handle = 0;

    [[nodiscard]] bool operator==(const ApiHandleKey& other) const
    {
        return api == other.api && handle == other.handle;
    }
};

struct ApiHandleKeyHasher {
    [[nodiscard]] std::size_t operator()(const ApiHandleKey& key) const noexcept
    {
        return (static_cast<std::size_t>(key.handle) >> 4U) ^
               (static_cast<std::size_t>(key.handle) << 7U) ^
               static_cast<std::size_t>(key.api);
    }
};

struct HostCaptureState {
    std::mutex mutex;
    std::vector<HostQueueRecord> queues;
    std::unordered_map<ApiHandleKey, std::vector<std::size_t>, ApiHandleKeyHasher> queuesByDevice;
    std::unordered_map<ApiHandleKey, std::vector<std::size_t>, ApiHandleKeyHasher> queuesBySwapChain;
    HostCaptureStats stats;
    std::uint64_t nextSerial = 1;
};

HostCaptureState& state()
{
    static HostCaptureState capture;
    return capture;
}

std::uintptr_t pointerKey(const NativeHandle handle)
{
    return reinterpret_cast<std::uintptr_t>(handle.value);
}

ApiHandleKey keyFor(const GraphicsApi api, const NativeHandle handle)
{
    return ApiHandleKey{.api = api, .handle = pointerKey(handle)};
}

void addNativeRef(const GraphicsApi api, const NativeHandle handle)
{
#if defined(_WIN32)
    if (api == GraphicsApi::Dx12 && handle.value != nullptr) {
        static_cast<IUnknown*>(handle.value)->AddRef();
    }
#else
    (void)api;
    (void)handle;
#endif
}

void releaseNativeRef(const GraphicsApi api, const NativeHandle handle)
{
#if defined(_WIN32)
    if (api == GraphicsApi::Dx12 && handle.value != nullptr) {
        static_cast<IUnknown*>(handle.value)->Release();
    }
#else
    (void)api;
    (void)handle;
#endif
}

std::uint32_t sourcePriority(const HostCaptureSourceKind source)
{
    switch (source) {
    case HostCaptureSourceKind::ExplicitNativeConfig:
        return 100;
    case HostCaptureSourceKind::NgxParameterBag:
        return 90;
    case HostCaptureSourceKind::FfxFrameGenerationSwapchain:
        return 80;
    case HostCaptureSourceKind::DxgiSwapChain:
        return 70;
    case HostCaptureSourceKind::D3d11On12WrappedQueue:
    case HostCaptureSourceKind::VulkanDeviceQueue:
        return 65;
    case HostCaptureSourceKind::D3d12CreateCommandQueue:
        return 60;
    case HostCaptureSourceKind::D3d11ImmediateContext:
    case HostCaptureSourceKind::VulkanQueueSubmit:
        return 55;
    case HostCaptureSourceKind::D3d12ExecuteCommandLists:
        return 50;
    }
    return 0;
}

std::uint32_t kindPriority(const HostCaptureQueueKind kind, const HostCaptureQueueKind preferred)
{
    if (kind == preferred) {
        return 30;
    }
    if (preferred == HostCaptureQueueKind::Direct && kind == HostCaptureQueueKind::Unknown) {
        return 10;
    }
    return 0;
}

bool betterRecord(
    const HostQueueRecord& candidate,
    const HostQueueRecord& incumbent,
    const HostCaptureQueueKind preferredKind)
{
    const std::uint32_t candidateKind = kindPriority(candidate.queueKind, preferredKind);
    const std::uint32_t incumbentKind = kindPriority(incumbent.queueKind, preferredKind);
    if (candidateKind != incumbentKind) {
        return candidateKind > incumbentKind;
    }

    const std::uint32_t candidateSource = sourcePriority(candidate.source);
    const std::uint32_t incumbentSource = sourcePriority(incumbent.source);
    if (candidateSource != incumbentSource) {
        return candidateSource > incumbentSource;
    }

    if (candidate.lastSeenFrame != incumbent.lastSeenFrame) {
        return candidate.lastSeenFrame > incumbent.lastSeenFrame;
    }

    return candidate.serial > incumbent.serial;
}

void appendIndex(
    std::unordered_map<ApiHandleKey, std::vector<std::size_t>, ApiHandleKeyHasher>& index,
    const ApiHandleKey key,
    const std::size_t recordIndex)
{
    if (key.handle == 0) {
        return;
    }
    auto& records = index[key];
    if (std::find(records.begin(), records.end(), recordIndex) == records.end()) {
        records.push_back(recordIndex);
    }
}

void removeIndex(
    std::unordered_map<ApiHandleKey, std::vector<std::size_t>, ApiHandleKeyHasher>& index,
    const ApiHandleKey key,
    const std::size_t recordIndex)
{
    if (key.handle == 0) {
        return;
    }
    auto existing = index.find(key);
    if (existing == index.end()) {
        return;
    }
    auto& records = existing->second;
    records.erase(std::remove(records.begin(), records.end(), recordIndex), records.end());
    if (records.empty()) {
        index.erase(existing);
    }
}

bool matchesResolvedLookup(const HostQueueRecord& candidate, const GraphicsApi api) noexcept
{
    return candidate.api == api && candidate.queue.value != nullptr;
}

bool matchesResolvedLookup(const HostQueueRecord& candidate, const GraphicsApi api, const NativeHandle swapChain) noexcept
{
    return matchesResolvedLookup(candidate, api) && candidate.swapChain.value == swapChain.value;
}


HostCaptureResolvedQueue resolvedFromRecord(const HostQueueRecord& record)
{
    return HostCaptureResolvedQueue{
        .api = record.api,
        .device = record.device,
        .queue = record.queue,
        .swapChain = record.swapChain,
        .queueKind = record.queueKind,
        .source = record.source,
        .nodeMask = record.nodeMask,
        .available = true,
    };
}

template <typename Predicate>
HostCaptureResolvedQueue resolveFromIndicesLocked(
    HostCaptureState& capture,
    const GraphicsApi api,
    const std::vector<std::size_t>& indices,
    const HostCaptureQueueKind preferredKind,
    Predicate&& predicate)
{
    HostCaptureResolvedQueue resolved;
    resolved.api = api;
    const HostQueueRecord* best = nullptr;
    for (const std::size_t index : indices) {
        if (index >= capture.queues.size()) {
            continue;
        }
        const HostQueueRecord& candidate = capture.queues[index];
        if (!predicate(candidate)) {
            continue;
        }
        if (best == nullptr || betterRecord(candidate, *best, preferredKind)) {
            best = &candidate;
        }
    }
    if (best != nullptr) {
        resolved = resolvedFromRecord(*best);
    }
    return resolved;
}

} // namespace

Status registerHostQueue(const HostCaptureQueueRegistrationDesc& desc)
{
    if (desc.device.value == nullptr) {
        return Status::invalidArgument("HostCapture queue registration requires a device");
    }
    if (desc.queue.value == nullptr) {
        return Status::invalidArgument("HostCapture queue registration requires a command queue");
    }

    HostCaptureState& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);

    capture.stats.registrations += 1;
    capture.stats.lastDevice = pointerKey(desc.device);
    capture.stats.lastQueue = pointerKey(desc.queue);
    capture.stats.lastSwapChain = pointerKey(desc.swapChain);
    capture.stats.lastSource = desc.source;

    for (std::size_t index = 0; index < capture.queues.size(); ++index) {
        HostQueueRecord& record = capture.queues[index];
        if (record.api == desc.api &&
            record.device.value == desc.device.value &&
            record.queue.value == desc.queue.value) {
            record.lastSeenFrame = desc.frameIndex;
            if (desc.queueKind != HostCaptureQueueKind::Unknown) {
                record.queueKind = desc.queueKind;
            }
            if (sourcePriority(desc.source) >= sourcePriority(record.source)) {
                record.source = desc.source;
            }
            record.nodeMask = desc.nodeMask;
            capture.stats.duplicateRegistrations += 1;

            if (desc.swapChain.value != nullptr && record.swapChain.value != desc.swapChain.value) {
                const NativeHandle oldSwapChain = record.swapChain;
                addNativeRef(desc.api, desc.swapChain);
                removeIndex(capture.queuesBySwapChain, keyFor(record.api, oldSwapChain), index);
                record.swapChain = desc.swapChain;
                appendIndex(capture.queuesBySwapChain, keyFor(desc.api, desc.swapChain), index);
                releaseNativeRef(record.api, oldSwapChain);
                capture.stats.swapChainAssociations += 1;
                capture.stats.queueRecordUpdates += 1;
            }
            return Status::ok();
        }
    }

    const std::size_t recordIndex = capture.queues.size();
    addNativeRef(desc.api, desc.device);
    addNativeRef(desc.api, desc.queue);
    addNativeRef(desc.api, desc.swapChain);
    capture.queues.push_back(HostQueueRecord{
        .api = desc.api,
        .device = desc.device,
        .queue = desc.queue,
        .swapChain = desc.swapChain,
        .queueKind = desc.queueKind,
        .source = desc.source,
        .nodeMask = desc.nodeMask,
        .firstSeenFrame = desc.frameIndex,
        .lastSeenFrame = desc.frameIndex,
        .serial = capture.nextSerial++,
    });
    appendIndex(capture.queuesByDevice, keyFor(desc.api, desc.device), recordIndex);
    appendIndex(capture.queuesBySwapChain, keyFor(desc.api, desc.swapChain), recordIndex);
    capture.stats.queueCandidates = static_cast<std::uint64_t>(capture.queues.size());
    if (desc.swapChain.value != nullptr) {
        capture.stats.swapChainAssociations += 1;
    }
    return Status::ok();
}

HostCaptureResolvedQueue resolveHostQueueForSwapChain(
    const GraphicsApi api,
    const NativeHandle swapChain,
    const HostCaptureQueueKind preferredKind)
{
    HostCaptureState& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    auto existing = capture.queuesBySwapChain.find(keyFor(api, swapChain));
    if (swapChain.value == nullptr || existing == capture.queuesBySwapChain.end()) {
        capture.stats.misses += 1;
        HostCaptureResolvedQueue resolved;
        resolved.api = api;
        resolved.swapChain = swapChain;
        return resolved;
    }

    HostCaptureResolvedQueue resolved = resolveFromIndicesLocked(
        capture,
        api,
        existing->second,
        preferredKind,
        [api, swapChain](const HostQueueRecord& candidate) { return matchesResolvedLookup(candidate, api, swapChain); });
    if (!resolved.available) {
        capture.stats.misses += 1;
        resolved.swapChain = swapChain;
        return resolved;
    }
    resolved.swapChain = swapChain;
    capture.stats.resolvesBySwapChain += 1;
    return resolved;
}

HostCaptureResolvedQueue resolveHostQueueForDevice(
    const GraphicsApi api,
    const NativeHandle device,
    const HostCaptureQueueKind preferredKind)
{
    HostCaptureState& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    auto existing = capture.queuesByDevice.find(keyFor(api, device));
    if (device.value == nullptr || existing == capture.queuesByDevice.end()) {
        capture.stats.misses += 1;
        HostCaptureResolvedQueue resolved;
        resolved.api = api;
        resolved.device = device;
        return resolved;
    }

    HostCaptureResolvedQueue resolved = resolveFromIndicesLocked(
        capture,
        api,
        existing->second,
        preferredKind,
        [api, device](const HostQueueRecord& candidate) {
            return candidate.api == api && candidate.device.value == device.value && candidate.queue.value != nullptr;
        });
    if (!resolved.available) {
        capture.stats.misses += 1;
        resolved.device = device;
        return resolved;
    }
    capture.stats.resolvesByDevice += 1;
    return resolved;
}

void clearHostCapture()
{
    HostCaptureState& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    for (const HostQueueRecord& record : capture.queues) {
        releaseNativeRef(record.api, record.swapChain);
        releaseNativeRef(record.api, record.queue);
        releaseNativeRef(record.api, record.device);
    }
    capture.queues.clear();
    capture.queuesByDevice.clear();
    capture.queuesBySwapChain.clear();
    capture.stats = HostCaptureStats{};
    capture.nextSerial = 1;
}

HostCaptureStats hostCaptureStats()
{
    HostCaptureState& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.stats;
}

const char* hostCaptureSourceName(const HostCaptureSourceKind source)
{
    switch (source) {
    case HostCaptureSourceKind::ExplicitNativeConfig:
        return "explicit-native-config";
    case HostCaptureSourceKind::FfxFrameGenerationSwapchain:
        return "ffx-frame-generation-swapchain";
    case HostCaptureSourceKind::DxgiSwapChain:
        return "dxgi-swapchain";
    case HostCaptureSourceKind::D3d12CreateCommandQueue:
        return "d3d12-create-command-queue";
    case HostCaptureSourceKind::D3d12ExecuteCommandLists:
        return "d3d12-execute-command-lists";
    case HostCaptureSourceKind::NgxParameterBag:
        return "ngx-parameter-bag";
    case HostCaptureSourceKind::D3d11ImmediateContext:
        return "d3d11-immediate-context";
    case HostCaptureSourceKind::D3d11On12WrappedQueue:
        return "d3d11on12-wrapped-queue";
    case HostCaptureSourceKind::VulkanDeviceQueue:
        return "vulkan-device-queue";
    case HostCaptureSourceKind::VulkanQueueSubmit:
        return "vulkan-queue-submit";
    }
    return "unknown";
}

const char* hostCaptureQueueKindName(const HostCaptureQueueKind kind)
{
    switch (kind) {
    case HostCaptureQueueKind::Unknown:
        return "unknown";
    case HostCaptureQueueKind::Direct:
        return "direct";
    case HostCaptureQueueKind::Compute:
        return "compute";
    case HostCaptureQueueKind::Copy:
        return "copy";
    }
    return "unknown";
}

} // namespace apusr::frontends
