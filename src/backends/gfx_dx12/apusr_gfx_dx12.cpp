// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_gfx_dx12.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

namespace apusr::backends {

namespace {

#ifdef _WIN32

Status hresultStatus(const char* operation, const HRESULT hr)
{
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return Status::runtimeFailure(message.str());
}

ID3D12Device* dx12Device(const Dx12BackendConfig& config)
{
    return static_cast<ID3D12Device*>(config.device.value);
}

ID3D12CommandQueue* dx12Queue(const Dx12BackendConfig& config)
{
    return static_cast<ID3D12CommandQueue*>(config.queue.value);
}

struct Dx12TokenBuffer {
    ComPtr<ID3D12Resource> resource;
    void* mappedPointer = nullptr;
    std::uint64_t byteSize = 0;
};

struct Dx12CommandContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> lastSubmitFence;
    std::uint64_t lastSubmitValue = 0;
    CommandPhase phase = CommandPhase::ExportTokens;
};

struct Dx12ImportedFence {
    ComPtr<ID3D12Fence> fence;
};

D3D12_RESOURCE_DESC makeTokenBufferDesc(const std::uint64_t byteSize)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = byteSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc;
}

Status createCpuVisibleUavBuffer(
    ID3D12Device* device,
    const std::uint64_t byteSize,
    ComPtr<ID3D12Resource>& outResource)
{
    if (device == nullptr) {
        return Status::invalidArgument("DX12 token buffer allocation requires a device");
    }

    const D3D12_RESOURCE_DESC desc = makeTokenBufferDesc(byteSize);

    // The GPU/NPU payload path must live in system memory that both agents can
    // access directly. WRITE_BACK and L1 fallbacks make the cache/visibility
    // contract ambiguous and can turn the test into a driver-dependent BAR or
    // CPU-cache exercise. Token arenas are not CPU-hot, so use L0
    // WRITE_COMBINE only and fail if the stack cannot provide it.
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource) {
        return hresultStatus("CreateCommittedResource(CUSTOM L0 WRITE_COMBINE UAV token buffer)", hr);
    }

    return Status::ok();
}

CommandContext makePublicCommandContext(Dx12CommandContext& nativeContext, ID3D12CommandQueue* queue)
{
    return CommandContext{
        .commandList = NativeHandle{nativeContext.commandList.Get()},
        .commandAllocator = NativeHandle{nativeContext.allocator.Get()},
        .commandQueue = NativeHandle{queue},
        .nativeContext = NativeHandle{&nativeContext},
        .phase = nativeContext.phase,
    };
}

Dx12CommandContext* nativeCommandContext(CommandContext& commandContext)
{
    return static_cast<Dx12CommandContext*>(commandContext.nativeContext.value);
}

Dx12CommandContext* nativeCommandContext(const CommandContext& commandContext)
{
    return static_cast<Dx12CommandContext*>(commandContext.nativeContext.value);
}

Status waitForCommandContextRetirement(Dx12CommandContext& nativeContext)
{
    if (!nativeContext.lastSubmitFence || nativeContext.lastSubmitValue == 0) {
        return Status::ok();
    }
    if (nativeContext.lastSubmitFence->GetCompletedValue() >= nativeContext.lastSubmitValue) {
        return Status::ok();
    }

    HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completionEvent == nullptr) {
        return Status::runtimeFailure("CreateEventW(DX12 command-context retirement) failed");
    }

    const HRESULT hr = nativeContext.lastSubmitFence->SetEventOnCompletion(nativeContext.lastSubmitValue, completionEvent);
    if (FAILED(hr)) {
        CloseHandle(completionEvent);
        return hresultStatus("ID3D12Fence::SetEventOnCompletion(command-context retirement)", hr);
    }

    const DWORD waitResult = WaitForSingleObject(completionEvent, INFINITE);
    CloseHandle(completionEvent);
    if (waitResult != WAIT_OBJECT_0) {
        return Status::runtimeFailure("WaitForSingleObject(DX12 command-context retirement) failed");
    }

    return Status::ok();
}

ID3D12Fence* graphicsFence(const TimelineFence& fence)
{
    if (fence.nativeGraphicsFence.value != nullptr) {
        return static_cast<ID3D12Fence*>(fence.nativeGraphicsFence.value);
    }
    return static_cast<ID3D12Fence*>(fence.nativeFence.value);
}

#else

struct Dx12TokenBuffer {};
struct Dx12CommandContext {};
struct Dx12ImportedFence {};

#endif

class Dx12GraphicsBackend final : public GraphicsBackend {
public:
    explicit Dx12GraphicsBackend(Dx12BackendConfig config) : config_(std::move(config)) {}

    [[nodiscard]] GraphicsApi api() const override { return GraphicsApi::Dx12; }

    [[nodiscard]] GraphicsBackendCapabilities capabilities() const override
    {
        return GraphicsBackendCapabilities{
            .sharedBufferHandleKind = SharedBufferHandleKind::HostUserPointer,
            .timelineFenceHandleKind = TimelineFenceHandleKind::Dx12Fence,
            .supportsSharedTokenBuffers = true,
            .supportsExternalTimelineFences = true,
            .supportsQueueSubmit = true,
            .supportsPersistentCommandContexts = true,
        };
    }

    [[nodiscard]] Status createTokenBuffer(const SharedBufferDesc& desc, SharedBuffer& outBuffer) override
    {
        if (config_.device.value == nullptr || config_.queue.value == nullptr) {
            return Status::invalidArgument("DX12 backend requires device and queue handles");
        }
        if (desc.byteSize == 0) {
            return Status::invalidArgument("DX12 token buffer size must be non-zero");
        }

#ifdef _WIN32
        auto tokenBuffer = std::make_unique<Dx12TokenBuffer>();
        tokenBuffer->byteSize = alignUp(desc.byteSize, desc.alignment);

        Status status = createCpuVisibleUavBuffer(dx12Device(config_), tokenBuffer->byteSize, tokenBuffer->resource);
        if (!status.succeeded()) {
            return status;
        }

        if (desc.debugName != nullptr) {
            std::wstring wideName;
            for (const char* cursor = desc.debugName; *cursor != '\0'; ++cursor) {
                wideName.push_back(static_cast<wchar_t>(*cursor));
            }
            tokenBuffer->resource->SetName(wideName.c_str());
        }

        D3D12_RANGE noCpuRead{0, 0};
        HRESULT hr = tokenBuffer->resource->Map(0, &noCpuRead, &tokenBuffer->mappedPointer);
        if (FAILED(hr) || tokenBuffer->mappedPointer == nullptr) {
            return hresultStatus("ID3D12Resource::Map(CPU-visible token buffer)", hr);
        }
        if ((reinterpret_cast<std::uintptr_t>(tokenBuffer->mappedPointer) % desc.alignment) != 0) {
            return Status::runtimeFailure("DX12 token buffer mapping does not satisfy the requested shared-buffer alignment");
        }

        outBuffer.nativeBuffer = NativeHandle{tokenBuffer->mappedPointer};
        outBuffer.nativeAllocation = NativeHandle{tokenBuffer->resource.Get()};
        outBuffer.nativeGraphicsBuffer = NativeHandle{tokenBuffer->resource.Get()};
        outBuffer.handleKind = SharedBufferHandleKind::HostUserPointer;
        outBuffer.byteSize = tokenBuffer->byteSize;
        outBuffer.gpuVirtualAddress = tokenBuffer->resource->GetGPUVirtualAddress();

        tokenBuffers_.push_back(std::move(tokenBuffer));
        return Status::ok();
#else
        (void)outBuffer;
        return Status::notImplemented("DX12 token-buffer allocation is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status importTimelineFence(TimelineFence& fence) override
    {
        if (config_.device.value == nullptr || config_.queue.value == nullptr) {
            return Status::invalidArgument("DX12 backend requires device and queue handles");
        }
        if (fence.handleKind != TimelineFenceHandleKind::Dx12Fence) {
            return Status::invalidArgument("DX12 backend can only import DX12-compatible timeline fences");
        }
        if (fence.nativeSharedHandle.value == nullptr && fence.nativeGraphicsFence.value == nullptr && fence.nativeFence.value == nullptr) {
            return Status::invalidArgument("DX12 timeline fence import requires an exported shared handle or an existing ID3D12Fence");
        }

#ifdef _WIN32
        if (fence.nativeGraphicsFence.value != nullptr) {
            return Status::ok();
        }

        auto importedFence = std::make_unique<Dx12ImportedFence>();
        HRESULT hr = S_OK;
        if (fence.nativeSharedHandle.value != nullptr) {
            hr = dx12Device(config_)->OpenSharedHandle(
                static_cast<HANDLE>(fence.nativeSharedHandle.value),
                IID_PPV_ARGS(&importedFence->fence));
        } else {
            importedFence->fence = static_cast<ID3D12Fence*>(fence.nativeFence.value);
        }
        if (FAILED(hr) || !importedFence->fence) {
            return hresultStatus("ID3D12Device::OpenSharedHandle(XRT timeline fence)", hr);
        }

        fence.nativeGraphicsFence = NativeHandle{importedFence->fence.Get()};
        fence.nativeFence = fence.nativeGraphicsFence;
        importedFences_.push_back(std::move(importedFence));
        return Status::ok();
#else
        return Status::notImplemented("DX12 timeline-fence import is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status createCommandContext(const CommandPhase phase, CommandContext& outContext) override
    {
        if (config_.device.value == nullptr || config_.queue.value == nullptr) {
            return Status::invalidArgument("DX12 backend requires device and queue handles");
        }

#ifdef _WIN32
        auto nativeContext = std::make_unique<Dx12CommandContext>();
        nativeContext->phase = phase;

        HRESULT hr = dx12Device(config_)->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&nativeContext->allocator));
        if (FAILED(hr)) {
            return hresultStatus("ID3D12Device::CreateCommandAllocator", hr);
        }

        hr = dx12Device(config_)->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            nativeContext->allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&nativeContext->commandList));
        if (FAILED(hr)) {
            return hresultStatus("ID3D12Device::CreateCommandList", hr);
        }

        hr = nativeContext->commandList->Close();
        if (FAILED(hr)) {
            return hresultStatus("ID3D12GraphicsCommandList::Close(initial)", hr);
        }

        outContext = makePublicCommandContext(*nativeContext, dx12Queue(config_));
        commandContexts_.push_back(std::move(nativeContext));
        return Status::ok();
#else
        (void)phase;
        (void)outContext;
        return Status::notImplemented("DX12 command context creation is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status resetCommandContext(CommandContext& commandContext) override
    {
#ifdef _WIN32
        Dx12CommandContext* nativeContext = nativeCommandContext(commandContext);
        if (nativeContext == nullptr) {
            return Status::invalidArgument("DX12 command context reset requires a native command context");
        }

        Status retirementStatus = waitForCommandContextRetirement(*nativeContext);
        if (!retirementStatus.succeeded()) {
            return retirementStatus;
        }

        HRESULT hr = nativeContext->allocator->Reset();
        if (FAILED(hr)) {
            return hresultStatus("ID3D12CommandAllocator::Reset", hr);
        }
        hr = nativeContext->commandList->Reset(nativeContext->allocator.Get(), nullptr);
        if (FAILED(hr)) {
            return hresultStatus("ID3D12GraphicsCommandList::Reset", hr);
        }

        commandContext.commandList = NativeHandle{nativeContext->commandList.Get()};
        commandContext.commandQueue = config_.queue;
        commandContext.phase = nativeContext->phase;
        return Status::ok();
#else
        (void)commandContext;
        return Status::notImplemented("DX12 command context reset is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status recordExportPass(CommandContext& commandContext, const ExportPassDesc& desc) override
    {
#ifdef _WIN32
        if (!config_.recordExportPass) {
            return Status::notImplemented("DX12 export pass recorder is not configured");
        }
        Dx12CommandContext* nativeContext = nativeCommandContext(commandContext);
        NativeHandle commandList = commandContext.commandList;
        NativeHandle commandAllocator = commandContext.commandAllocator;
        if (nativeContext != nullptr) {
            commandList = NativeHandle{nativeContext->commandList.Get()};
            commandAllocator = NativeHandle{nativeContext->allocator.Get()};
        }
        if (commandList.value == nullptr) {
            return Status::invalidArgument("DX12 export recording requires a command list");
        }
        Dx12RecordContext recordContext{
            .device = config_.device,
            .queue = config_.queue,
            .commandList = commandList,
            .commandAllocator = commandAllocator,
            .phase = CommandPhase::ExportTokens,
        };
        return config_.recordExportPass(recordContext, desc, config_.recordUserData);
#else
        (void)commandContext;
        (void)desc;
        return Status::notImplemented("DX12 export pass recording is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status recordImportPass(CommandContext& commandContext, const ImportPassDesc& desc) override
    {
#ifdef _WIN32
        if (!config_.recordImportPass) {
            return Status::notImplemented("DX12 import pass recorder is not configured");
        }
        Dx12CommandContext* nativeContext = nativeCommandContext(commandContext);
        NativeHandle commandList = commandContext.commandList;
        NativeHandle commandAllocator = commandContext.commandAllocator;
        if (nativeContext != nullptr) {
            commandList = NativeHandle{nativeContext->commandList.Get()};
            commandAllocator = NativeHandle{nativeContext->allocator.Get()};
        }
        if (commandList.value == nullptr) {
            return Status::invalidArgument("DX12 import recording requires a command list");
        }
        Dx12RecordContext recordContext{
            .device = config_.device,
            .queue = config_.queue,
            .commandList = commandList,
            .commandAllocator = commandAllocator,
            .phase = CommandPhase::ImportTokens,
        };
        return config_.recordImportPass(recordContext, desc, config_.recordUserData);
#else
        (void)commandContext;
        (void)desc;
        return Status::notImplemented("DX12 import pass recording is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status closeCommandContext(CommandContext& commandContext) override
    {
#ifdef _WIN32
        Dx12CommandContext* nativeContext = nativeCommandContext(commandContext);
        if (nativeContext == nullptr) {
            return Status::invalidArgument("DX12 command context close requires a native command context");
        }
        const HRESULT hr = nativeContext->commandList->Close();
        if (FAILED(hr)) {
            return hresultStatus("ID3D12GraphicsCommandList::Close", hr);
        }
        commandContext.commandList = NativeHandle{nativeContext->commandList.Get()};
        commandContext.commandQueue = config_.queue;
        return Status::ok();
#else
        (void)commandContext;
        return Status::notImplemented("DX12 command context close is only available in Windows builds");
#endif
    }

    [[nodiscard]] Status submitGpuWork(const GpuSubmitDesc& desc) override
    {
        if (config_.queue.value == nullptr) {
            return Status::invalidArgument("DX12 GPU submission requires a queue");
        }

#ifdef _WIN32
        ID3D12CommandQueue* queue = dx12Queue(config_);
        for (const TimelineFenceWait& wait : desc.waits) {
            ID3D12Fence* fence = graphicsFence(wait.fence);
            if (fence == nullptr) {
                return Status::invalidArgument("DX12 queue wait requires an imported ID3D12Fence");
            }
            const HRESULT hr = queue->Wait(fence, wait.value);
            if (FAILED(hr)) {
                return hresultStatus("ID3D12CommandQueue::Wait", hr);
            }
        }

        ID3D12CommandList* lists[] = {static_cast<ID3D12CommandList*>(desc.commandContext.commandList.value)};
        if (lists[0] != nullptr) {
            queue->ExecuteCommandLists(1, lists);
        } else if (desc.waits.empty() && desc.signals.empty()) {
            return Status::invalidArgument("DX12 queue submission requires work, waits, or signals");
        }

        ID3D12Fence* lastSubmittedFence = nullptr;
        std::uint64_t lastSubmittedValue = 0;
        for (const TimelineFenceSignal& signal : desc.signals) {
            ID3D12Fence* fence = graphicsFence(signal.fence);
            if (fence == nullptr) {
                return Status::invalidArgument("DX12 queue signal requires an imported ID3D12Fence");
            }
            const HRESULT hr = queue->Signal(fence, signal.value);
            if (FAILED(hr)) {
                return hresultStatus("ID3D12CommandQueue::Signal", hr);
            }
            lastSubmittedFence = fence;
            lastSubmittedValue = signal.value;
        }

        Dx12CommandContext* nativeContext = nativeCommandContext(desc.commandContext);
        if (nativeContext != nullptr && lastSubmittedFence != nullptr) {
            nativeContext->lastSubmitFence = lastSubmittedFence;
            nativeContext->lastSubmitValue = lastSubmittedValue;
        }

        return Status::ok();
#else
        (void)desc;
        return Status::notImplemented("DX12 queue submission is only available in Windows builds");
#endif
    }

private:
    Dx12BackendConfig config_;
    std::vector<std::unique_ptr<Dx12TokenBuffer>> tokenBuffers_;
    std::vector<std::unique_ptr<Dx12CommandContext>> commandContexts_;
    std::vector<std::unique_ptr<Dx12ImportedFence>> importedFences_;
};

} // namespace

GraphicsBackendPtr createDx12GraphicsBackend(const Dx12BackendConfig& config)
{
    return std::make_shared<Dx12GraphicsBackend>(config);
}

} // namespace apusr::backends
