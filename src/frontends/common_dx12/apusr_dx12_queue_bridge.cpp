// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_dx12_queue_bridge.h>
#include <apusr/frontends/apusr_host_capture.h>

#include <algorithm>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <unknwn.h>
#include <windows.h>

#include <atomic>
#endif

namespace apusr::frontends {
namespace {

struct QueueBridgeDiscoveryStats {
    std::mutex mutex;
    std::uint64_t installAttempts = 0;
    std::uint64_t hooksInstalled = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t uninstalls = 0;
    std::uint64_t deviceCaptureInstallAttempts = 0;
    std::uint64_t deviceCaptureHooksInstalled = 0;
    std::uint64_t deviceCreateCommandQueueCallbacks = 0;
    std::uint64_t deviceCaptureUninstalls = 0;
};

struct QueueBridgeDiscoveryStatsSnapshot {
    std::uint64_t installAttempts = 0;
    std::uint64_t hooksInstalled = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t uninstalls = 0;
    std::uint64_t deviceCaptureInstallAttempts = 0;
    std::uint64_t deviceCaptureHooksInstalled = 0;
    std::uint64_t deviceCreateCommandQueueCallbacks = 0;
    std::uint64_t deviceCaptureUninstalls = 0;
};

QueueBridgeDiscoveryStats& discoveryStats()
{
    static QueueBridgeDiscoveryStats stats;
    return stats;
}

HostCaptureSourceKind toHostCaptureSource(const Dx12QueueSourceKind source)
{
    switch (source) {
    case Dx12QueueSourceKind::ExplicitNativeConfig:
        return HostCaptureSourceKind::ExplicitNativeConfig;
    case Dx12QueueSourceKind::FfxFrameGenerationSwapchain:
        return HostCaptureSourceKind::FfxFrameGenerationSwapchain;
    case Dx12QueueSourceKind::DxgiSwapChain:
        return HostCaptureSourceKind::DxgiSwapChain;
    case Dx12QueueSourceKind::D3d12CreateCommandQueue:
        return HostCaptureSourceKind::D3d12CreateCommandQueue;
    case Dx12QueueSourceKind::D3d12ExecuteCommandLists:
        return HostCaptureSourceKind::D3d12ExecuteCommandLists;
    }
    return HostCaptureSourceKind::ExplicitNativeConfig;
}

Dx12QueueSourceKind toDx12QueueSource(const HostCaptureSourceKind source)
{
    switch (source) {
    case HostCaptureSourceKind::ExplicitNativeConfig:
        return Dx12QueueSourceKind::ExplicitNativeConfig;
    case HostCaptureSourceKind::FfxFrameGenerationSwapchain:
        return Dx12QueueSourceKind::FfxFrameGenerationSwapchain;
    case HostCaptureSourceKind::DxgiSwapChain:
    case HostCaptureSourceKind::NgxParameterBag:
        return Dx12QueueSourceKind::DxgiSwapChain;
    case HostCaptureSourceKind::D3d12CreateCommandQueue:
        return Dx12QueueSourceKind::D3d12CreateCommandQueue;
    case HostCaptureSourceKind::D3d12ExecuteCommandLists:
        return Dx12QueueSourceKind::D3d12ExecuteCommandLists;
    case HostCaptureSourceKind::D3d11ImmediateContext:
    case HostCaptureSourceKind::D3d11On12WrappedQueue:
    case HostCaptureSourceKind::VulkanDeviceQueue:
    case HostCaptureSourceKind::VulkanQueueSubmit:
        return Dx12QueueSourceKind::ExplicitNativeConfig;
    }
    return Dx12QueueSourceKind::ExplicitNativeConfig;
}

void noteDiscoveryInstallAttempt()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.installAttempts += 1;
}

[[maybe_unused]] void noteDiscoveryHookInstalled()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.hooksInstalled += 1;
}

[[maybe_unused]] void noteDiscoveryCallback()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.callbacks += 1;
}

[[maybe_unused]] void noteDiscoveryUninstall()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.uninstalls += 1;
}

void noteDeviceCaptureInstallAttempt()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.deviceCaptureInstallAttempts += 1;
}

[[maybe_unused]] void noteDeviceCaptureHookInstalled()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.deviceCaptureHooksInstalled += 1;
}

[[maybe_unused]] void noteDeviceCreateCommandQueueCallback()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.deviceCreateCommandQueueCallbacks += 1;
}

[[maybe_unused]] void noteDeviceCaptureUninstall()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.deviceCaptureUninstalls += 1;
}

void clearDiscoveryStats()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.installAttempts = 0;
    stats.hooksInstalled = 0;
    stats.callbacks = 0;
    stats.uninstalls = 0;
    stats.deviceCaptureInstallAttempts = 0;
    stats.deviceCaptureHooksInstalled = 0;
    stats.deviceCreateCommandQueueCallbacks = 0;
    stats.deviceCaptureUninstalls = 0;
}

QueueBridgeDiscoveryStatsSnapshot snapshotDiscoveryStats()
{
    QueueBridgeDiscoveryStats& stats = discoveryStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    return QueueBridgeDiscoveryStatsSnapshot{
        .installAttempts = stats.installAttempts,
        .hooksInstalled = stats.hooksInstalled,
        .callbacks = stats.callbacks,
        .uninstalls = stats.uninstalls,
        .deviceCaptureInstallAttempts = stats.deviceCaptureInstallAttempts,
        .deviceCaptureHooksInstalled = stats.deviceCaptureHooksInstalled,
        .deviceCreateCommandQueueCallbacks = stats.deviceCreateCommandQueueCallbacks,
        .deviceCaptureUninstalls = stats.deviceCaptureUninstalls,
    };
}

#if defined(_WIN32)

using CreateCommandQueueFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

HRESULT STDMETHODCALLTYPE hookedCreateCommandQueue(
    ID3D12Device* device,
    const D3D12_COMMAND_QUEUE_DESC* desc,
    REFIID riid,
    void** outCommandQueue);
void STDMETHODCALLTYPE hookedExecuteCommandLists(
    ID3D12CommandQueue* queue,
    UINT numCommandLists,
    ID3D12CommandList* const* commandLists);

thread_local bool g_suppressCreateCommandQueueCapture = false;

struct ScopedSuppressCreateCommandQueueCapture {
    bool previous = false;

    ScopedSuppressCreateCommandQueueCapture() noexcept
        : previous(g_suppressCreateCommandQueueCapture)
    {
        g_suppressCreateCommandQueueCapture = true;
    }

    ~ScopedSuppressCreateCommandQueueCapture()
    {
        g_suppressCreateCommandQueueCapture = previous;
    }
};
constexpr std::size_t kCreateCommandQueueVtableIndex = 8;
constexpr std::size_t kExecuteCommandListsVtableIndex = 10;

struct PatchedVtableSlot {
    void** slot = nullptr;
    void* original = nullptr;
};

struct CreateCommandQueueHookState {
    std::mutex mutex;
    std::vector<PatchedVtableSlot> slots;
    std::uint32_t installRefs = 0;
};

struct ExecuteCommandListsHookState {
    std::mutex mutex;
    void** patchedSlot = nullptr;
    ExecuteCommandListsFn original = nullptr;
    std::uint32_t installRefs = 0;
    bool installed = false;
};

CreateCommandQueueHookState& createQueueHookState()
{
    static CreateCommandQueueHookState hook;
    return hook;
}

ExecuteCommandListsHookState& executeHookState()
{
    static ExecuteCommandListsHookState hook;
    return hook;
}

void** vtableSlotFor(void* object, const std::size_t index) noexcept
{
    if (object == nullptr) {
        return nullptr;
    }
    auto*** objectVtable = reinterpret_cast<void***>(object);
    if (objectVtable == nullptr || *objectVtable == nullptr) {
        return nullptr;
    }
    return &((*objectVtable)[index]);
}

bool replaceVtableSlot(void** slot, void* replacement, void** outOriginal)
{
    if (slot == nullptr || replacement == nullptr) {
        return false;
    }

    DWORD oldProtect = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
        return false;
    }

    if (outOriginal != nullptr) {
        *outOriginal = *slot;
    }
    *slot = replacement;

    DWORD ignoredProtect = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignoredProtect);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool restoreVtableSlotIfCurrent(void** slot, void* expected, void* original)
{
    if (slot == nullptr || expected == nullptr || original == nullptr || *slot != expected) {
        return false;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
        return false;
    }

    if (*slot == expected) {
        *slot = original;
    }

    DWORD ignoredProtect = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignoredProtect);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return *slot == original;
}

CreateCommandQueueFn originalCreateCommandQueueFor(ID3D12Device* device)
{
    void** const slot = vtableSlotFor(device, kCreateCommandQueueVtableIndex);
    CreateCommandQueueHookState& hook = createQueueHookState();
    std::lock_guard<std::mutex> lock(hook.mutex);
    auto found = std::find_if(hook.slots.begin(), hook.slots.end(), [slot](const PatchedVtableSlot& patched) {
        return patched.slot == slot;
    });
    return found != hook.slots.end() ? reinterpret_cast<CreateCommandQueueFn>(found->original) : nullptr;
}

void registerCreatedDirectQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, void* commandQueueObject)
{
    if (device == nullptr || commandQueueObject == nullptr) {
        return;
    }

    ID3D12CommandQueue* queue = nullptr;
    auto* const unknown = static_cast<IUnknown*>(commandQueueObject);
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr) {
        return;
    }

    const D3D12_COMMAND_QUEUE_DESC actualDesc = queue->GetDesc();
    const D3D12_COMMAND_LIST_TYPE type = desc != nullptr ? desc->Type : actualDesc.Type;
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        (void)registerDx12Queue(Dx12QueueRegistrationDesc{
            .device = NativeHandle{device},
            .queue = NativeHandle{queue},
            .source = Dx12QueueSourceKind::D3d12CreateCommandQueue,
            .nodeMask = actualDesc.NodeMask,
        });
    }
    queue->Release();
}

HRESULT STDMETHODCALLTYPE hookedCreateCommandQueue(
    ID3D12Device* device,
    const D3D12_COMMAND_QUEUE_DESC* desc,
    REFIID riid,
    void** outCommandQueue)
{
    noteDeviceCreateCommandQueueCallback();

    CreateCommandQueueFn original = originalCreateCommandQueueFor(device);
    if (original == nullptr) {
        return E_FAIL;
    }

    const HRESULT hr = original(device, desc, riid, outCommandQueue);
    if (!g_suppressCreateCommandQueueCapture && SUCCEEDED(hr) && outCommandQueue != nullptr && *outCommandQueue != nullptr) {
        registerCreatedDirectQueue(device, desc, *outCommandQueue);
    }
    return hr;
}

Status patchCreateCommandQueueVtable(ID3D12Device* device)
{
    void** const slot = vtableSlotFor(device, kCreateCommandQueueVtableIndex);
    if (slot == nullptr) {
        return Status::runtimeFailure("DX12 device queue capture could not read the device vtable");
    }

    auto* const replacement = reinterpret_cast<void*>(&hookedCreateCommandQueue);
    CreateCommandQueueHookState& hook = createQueueHookState();
    std::lock_guard<std::mutex> lock(hook.mutex);

    auto existing = std::find_if(hook.slots.begin(), hook.slots.end(), [slot](const PatchedVtableSlot& patched) {
        return patched.slot == slot;
    });
    if (existing != hook.slots.end()) {
        ++hook.installRefs;
        return Status::ok();
    }

    void* original = nullptr;
    if (!replaceVtableSlot(slot, replacement, &original)) {
        return Status::runtimeFailure("DX12 device queue capture could not make the CreateCommandQueue vtable slot writable");
    }

    hook.slots.push_back(PatchedVtableSlot{.slot = slot, .original = original});
    ++hook.installRefs;
    return Status::ok();
}

void restoreCreateCommandQueueHooks(const bool force)
{
    CreateCommandQueueHookState& hook = createQueueHookState();
    std::lock_guard<std::mutex> lock(hook.mutex);
    if (!force && hook.installRefs > 1) {
        --hook.installRefs;
        return;
    }

    auto* const replacement = reinterpret_cast<void*>(&hookedCreateCommandQueue);
    for (const PatchedVtableSlot& patched : hook.slots) {
        if (restoreVtableSlotIfCurrent(patched.slot, replacement, patched.original)) {
            noteDeviceCaptureUninstall();
        }
    }
    hook.slots.clear();
    hook.installRefs = 0;
}

void restoreExecuteCommandListsHook(const bool force)
{
    ExecuteCommandListsHookState& hook = executeHookState();
    std::lock_guard<std::mutex> lock(hook.mutex);
    if (!hook.installed || hook.patchedSlot == nullptr || hook.original == nullptr) {
        hook.installRefs = 0;
        return;
    }
    if (!force && hook.installRefs > 1) {
        --hook.installRefs;
        return;
    }

    auto* const replacement = reinterpret_cast<void*>(&hookedExecuteCommandLists);
    if (restoreVtableSlotIfCurrent(hook.patchedSlot, replacement, reinterpret_cast<void*>(hook.original))) {
        noteDiscoveryUninstall();
    }

    hook.patchedSlot = nullptr;
    hook.original = nullptr;
    hook.installRefs = 0;
    hook.installed = false;
}

void tryCaptureQueueFromExecute(ID3D12CommandQueue* queue)
{
    if (queue == nullptr) {
        return;
    }

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    ID3D12Device* device = nullptr;
    const HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || device == nullptr) {
        return;
    }

    const Status status = registerDx12Queue(Dx12QueueRegistrationDesc{
        .device = NativeHandle{device},
        .queue = NativeHandle{queue},
        .source = Dx12QueueSourceKind::D3d12ExecuteCommandLists,
        .nodeMask = desc.NodeMask,
    });
    device->Release();

    if (status.succeeded()) {
        restoreExecuteCommandListsHook(true);
    }
}

void STDMETHODCALLTYPE hookedExecuteCommandLists(
    ID3D12CommandQueue* queue,
    UINT numCommandLists,
    ID3D12CommandList* const* commandLists)
{
    ExecuteCommandListsFn original = nullptr;
    {
        ExecuteCommandListsHookState& hook = executeHookState();
        std::lock_guard<std::mutex> lock(hook.mutex);
        original = hook.original;
    }

    noteDiscoveryCallback();
    tryCaptureQueueFromExecute(queue);

    if (original != nullptr) {
        original(queue, numCommandLists, commandLists);
    }
}

Status patchExecuteCommandListsVtable(ID3D12CommandQueue* queue)
{
    void** const slot = vtableSlotFor(queue, kExecuteCommandListsVtableIndex);
    if (slot == nullptr) {
        return Status::runtimeFailure("DX12 queue discovery could not read the command-queue vtable");
    }

    auto* const replacement = reinterpret_cast<void*>(&hookedExecuteCommandLists);

    ExecuteCommandListsHookState& hook = executeHookState();
    std::lock_guard<std::mutex> lock(hook.mutex);
    if (hook.installed) {
        ++hook.installRefs;
        return Status::ok();
    }

    void* original = nullptr;
    if (!replaceVtableSlot(slot, replacement, &original)) {
        return Status::runtimeFailure("DX12 queue discovery could not make the ExecuteCommandLists vtable slot writable");
    }

    hook.patchedSlot = slot;
    hook.original = reinterpret_cast<ExecuteCommandListsFn>(original);
    hook.installRefs = 1;
    hook.installed = true;
    return Status::ok();
}

#endif

} // namespace

Status registerDx12Queue(const Dx12QueueRegistrationDesc& desc)
{
    return registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = desc.device,
        .queue = desc.queue,
        .swapChain = desc.swapChain,
        .queueKind = HostCaptureQueueKind::Direct,
        .source = toHostCaptureSource(desc.source),
        .nodeMask = desc.nodeMask,
        .frameIndex = desc.frameIndex,
    });
}

Dx12ResolvedQueue resolveDx12Queue(const NativeHandle device)
{
    const HostCaptureResolvedQueue resolved =
        resolveHostQueueForDevice(GraphicsApi::Dx12, device, HostCaptureQueueKind::Direct);
    if (!resolved.available) {
        return Dx12ResolvedQueue{.device = device, .queue = NativeHandle{}, .swapChain = NativeHandle{}};
    }
    return Dx12ResolvedQueue{
        .device = resolved.device,
        .queue = resolved.queue,
        .swapChain = resolved.swapChain,
        .source = toDx12QueueSource(resolved.source),
        .available = true,
    };
}

Status installDx12DeviceQueueCapture(const NativeHandle device)
{
    noteDeviceCaptureInstallAttempt();
#if defined(_WIN32)
    if (device.value == nullptr) {
        return Status::invalidArgument("DX12 device queue capture requires a D3D12 device");
    }
    Status status = patchCreateCommandQueueVtable(static_cast<ID3D12Device*>(device.value));
    if (status.succeeded()) {
        noteDeviceCaptureHookInstalled();
    }
    return status;
#else
    (void)device;
    return Status::unsupportedBackend("DX12 device queue capture hooks are Windows-only");
#endif
}

void uninstallDx12DeviceQueueCapture()
{
#if defined(_WIN32)
    restoreCreateCommandQueueHooks(false);
#endif
}

Status installDx12QueueDiscovery(const NativeHandle device)
{
    noteDiscoveryInstallAttempt();
#if defined(_WIN32)
    if (device.value == nullptr) {
        return Status::invalidArgument("DX12 queue discovery requires a D3D12 device");
    }

    auto* const d3d12Device = static_cast<ID3D12Device*>(device.value);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    ID3D12CommandQueue* probeQueue = nullptr;
    ScopedSuppressCreateCommandQueueCapture suppressProbeQueueCapture;
    const HRESULT hr = d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&probeQueue));
    if (FAILED(hr) || probeQueue == nullptr) {
        return Status::runtimeFailure("DX12 queue discovery could not create a temporary direct command queue");
    }

    Status status = patchExecuteCommandListsVtable(probeQueue);
    probeQueue->Release();
    if (status.succeeded()) {
        noteDiscoveryHookInstalled();
    }
    return status;
#else
    (void)device;
    return Status::unsupportedBackend("DX12 queue discovery hooks are Windows-only");
#endif
}

void uninstallDx12QueueDiscovery()
{
#if defined(_WIN32)
    restoreExecuteCommandListsHook(false);
#endif
}

void clearDx12QueueBridge()
{
#if defined(_WIN32)
    restoreExecuteCommandListsHook(true);
    restoreCreateCommandQueueHooks(true);
#else
    uninstallDx12QueueDiscovery();
    uninstallDx12DeviceQueueCapture();
#endif
    clearHostCapture();
    clearDiscoveryStats();
}

Dx12QueueBridgeStats dx12QueueBridgeStats()
{
    const HostCaptureStats hostStats = hostCaptureStats();
    const QueueBridgeDiscoveryStatsSnapshot discovery = snapshotDiscoveryStats();
    return Dx12QueueBridgeStats{
        .registrations = hostStats.registrations,
        .replacements = hostStats.queueRecordUpdates,
        .resolves = hostStats.resolvesByDevice + hostStats.resolvesBySwapChain,
        .misses = hostStats.misses,
        .discoveryInstallAttempts = discovery.installAttempts,
        .discoveryHooksInstalled = discovery.hooksInstalled,
        .discoveryCallbacks = discovery.callbacks,
        .discoveryUninstalls = discovery.uninstalls,
        .deviceCaptureInstallAttempts = discovery.deviceCaptureInstallAttempts,
        .deviceCaptureHooksInstalled = discovery.deviceCaptureHooksInstalled,
        .deviceCreateCommandQueueCallbacks = discovery.deviceCreateCommandQueueCallbacks,
        .deviceCaptureUninstalls = discovery.deviceCaptureUninstalls,
        .lastDevice = hostStats.lastDevice,
        .lastQueue = hostStats.lastQueue,
        .lastSource = toDx12QueueSource(hostStats.lastSource),
    };
}

const char* dx12QueueSourceName(const Dx12QueueSourceKind source)
{
    switch (source) {
    case Dx12QueueSourceKind::ExplicitNativeConfig:
        return "explicit-native-config";
    case Dx12QueueSourceKind::FfxFrameGenerationSwapchain:
        return "ffx-frame-generation-swapchain";
    case Dx12QueueSourceKind::DxgiSwapChain:
        return "dxgi-swapchain";
    case Dx12QueueSourceKind::D3d12CreateCommandQueue:
        return "d3d12-create-command-queue";
    case Dx12QueueSourceKind::D3d12ExecuteCommandLists:
        return "d3d12-execute-command-lists";
    }
    return "unknown";
}

} // namespace apusr::frontends
