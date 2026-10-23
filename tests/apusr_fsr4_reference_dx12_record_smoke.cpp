// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_fsr4_reference_dx12.h>

#include <iostream>

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool failed(const char* what, const HRESULT hr)
{
    if (SUCCEEDED(hr)) {
        return false;
    }
    std::cerr << what << " failed HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
    return true;
}

bool failedStatus(const char* what, const apusr::Status& status)
{
    if (status.succeeded()) {
        return false;
    }
    std::cerr << what << " failed status=" << apusr::statusCodeName(status.code)
              << " message='" << status.message << "'\n";
    return true;
}


bool tryEnableDebugLayer()
{
    // This is a structural smoke test, not a product path.  Enabling the debug
    // layer when available gives actionable PSO/root-signature diagnostics
    // without putting any overhead into the runtime hot path.
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        return false;
    }
    debug->EnableDebugLayer();
    return true;
}

const char* shaderModelName(const D3D_SHADER_MODEL model)
{
    // D3D_SHADER_MODEL enum identifiers above the SDK's known range are not
    // always present as named constants, so keep this smoke compatible with the
    // Windows SDK packaged with the user's VS installation by comparing the
    // encoded numeric values directly (0x66 == shader model 6.6, etc.).
    switch (static_cast<unsigned>(model)) {
    case 0x68u: return "6.8";
    case 0x67u: return "6.7";
    case 0x66u: return "6.6";
    case 0x65u: return "6.5";
    case 0x64u: return "6.4";
    case 0x63u: return "6.3";
    case 0x62u: return "6.2";
    case 0x61u: return "6.1";
    case 0x60u: return "6.0";
    case 0x51u: return "5.1";
    default: return "unknown";
    }
}

const wchar_t* shaderModelNameWide(const D3D_SHADER_MODEL model)
{
    switch (static_cast<unsigned>(model)) {
    case 0x68u: return L"6.8";
    case 0x67u: return L"6.7";
    case 0x66u: return L"6.6";
    case 0x65u: return L"6.5";
    case 0x64u: return L"6.4";
    case 0x63u: return L"6.3";
    case 0x62u: return L"6.2";
    case 0x61u: return L"6.1";
    case 0x60u: return L"6.0";
    case 0x51u: return L"5.1";
    default: return L"unknown";
    }
}

D3D_SHADER_MODEL queryHighestShaderModel(ID3D12Device* device)
{
    if (device == nullptr) {
        return static_cast<D3D_SHADER_MODEL>(0x51u);
    }

    // Ask from newest to oldest without requiring the SDK to define named enum
    // constants for every SM version.  Unsupported requests can return failure;
    // the next lower request keeps the test robust on older drivers/SDKs.
    const unsigned candidates[] = {0x68u, 0x67u, 0x66u, 0x65u, 0x64u, 0x63u, 0x62u, 0x61u, 0x60u, 0x51u};
    for (const unsigned candidate : candidates) {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
        shaderModel.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(candidate);
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))) {
            return shaderModel.HighestShaderModel;
        }
    }
    return static_cast<D3D_SHADER_MODEL>(0x51u);
}

bool supportsRequiredShaderModel(ID3D12Device* device)
{
    return static_cast<unsigned>(queryHighestShaderModel(device)) >= 0x66u;
}

std::wstring adapterName(IDXGIAdapter1* adapter)
{
    DXGI_ADAPTER_DESC1 desc{};
    if (adapter != nullptr && SUCCEEDED(adapter->GetDesc1(&desc))) {
        return desc.Description;
    }
    return L"<unknown adapter>";
}

bool isSoftwareAdapter(IDXGIAdapter1* adapter)
{
    DXGI_ADAPTER_DESC1 desc{};
    if (adapter == nullptr || FAILED(adapter->GetDesc1(&desc))) {
        return false;
    }
    // Some hosted Windows images enumerate Microsoft Basic Render Driver with
    // the software flag unset. It is still a CPU adapter, not product hardware.
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414u;
}

void dumpD3D12Messages(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> queue;
    if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        return;
    }

    const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    const UINT64 first = count > 16 ? count - 16 : 0;
    for (UINT64 index = first; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &length)) || length == 0) {
            continue;
        }
        std::vector<std::uint8_t> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(index, message, &length)) || message->pDescription == nullptr) {
            continue;
        }
        std::cerr << "D3D12 message[" << index << "] severity=" << static_cast<unsigned>(message->Severity)
                  << " id=" << static_cast<unsigned>(message->ID)
                  << ": " << message->pDescription << "\n";
    }
}

bool createDevice(ComPtr<ID3D12Device>& outDevice)
{
    outDevice.Reset();

    UINT factoryFlags = 0;
    const bool debugLayerEnabled = tryEnableDebugLayer();
    if (debugLayerEnabled) {
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    ComPtr<IDXGIFactory6> factory;
    if (failed("CreateDXGIFactory2", CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    // Do not rely on D3D12CreateDevice(nullptr) here.  On multi-adapter systems
    // that can pick an adapter that is valid D3D12 but not useful for FSR4.  The
    // reference embedded model shaders are SM 6.6, so the smoke explicitly picks
    // a hardware adapter that advertises that capability.
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !adapter || isSoftwareAdapter(adapter.Get())) {
            continue;
        }

        ComPtr<ID3D12Device> candidate;
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate));
        if (FAILED(hr) || !candidate) {
            continue;
        }
        const D3D_SHADER_MODEL highestShaderModel = queryHighestShaderModel(candidate.Get());
        std::wcerr << L"DX12 record smoke candidate adapter: " << adapterName(adapter.Get())
                   << L" shaderModel=" << shaderModelNameWide(highestShaderModel) << L"\n";
        if (!supportsRequiredShaderModel(candidate.Get())) {
            continue;
        }
        outDevice = std::move(candidate);
        return true;
    }

    // WARP can be useful in a VM, but executing the full reference shader chain
    // in software is not a good default product smoke.  Keep it opt-in so this
    // test does not become a surprise long-running CPU job on machines without
    // a native D3D12 GPU.
    if (std::getenv("APUSR_DX12_RECORD_SMOKE_ALLOW_WARP") == nullptr) {
        return false;
    }

    ComPtr<IDXGIAdapter> warp;
    if (failed("IDXGIFactory::EnumWarpAdapter", factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        return false;
    }
    HRESULT hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&outDevice));
    if (failed("D3D12CreateDevice(WARP)", hr)) {
        return false;
    }
    const D3D_SHADER_MODEL highestShaderModel = queryHighestShaderModel(outDevice.Get());
    std::cerr << "DX12 record smoke WARP shaderModel=" << shaderModelName(highestShaderModel) << "\n";
    if (!supportsRequiredShaderModel(outDevice.Get())) {
        outDevice.Reset();
        return false;
    }
    return true;
}

D3D12_RESOURCE_DESC textureDesc(const DXGI_FORMAT format,
    const std::uint32_t width,
    const std::uint32_t height,
    const D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

bool createTexture(ID3D12Device* device,
    const DXGI_FORMAT format,
    const std::uint32_t width,
    const std::uint32_t height,
    const D3D12_RESOURCE_FLAGS flags,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    outResource.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    const D3D12_RESOURCE_DESC desc = textureDesc(format, width, height, flags);
    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (failed("ID3D12Device::CreateCommittedResource(test texture)", hr)) {
        return false;
    }
    outResource->SetName(debugName);
    return true;
}

} // namespace

int main()
{
    using namespace apusr;
    using namespace apusr::backends;
    using namespace apusr::models::fsr4_int8;

    ComPtr<ID3D12Device> device;
    if (!createDevice(device)) {
        std::cerr << "No D3D12-capable device or WARP fallback was available; skipping DX12 record smoke.\n";
        return 77;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    ComPtr<ID3D12CommandQueue> queue;
    if (failed("ID3D12Device::CreateCommandQueue", device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) {
        return 1;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    if (failed("ID3D12Device::CreateCommandAllocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
        return 1;
    }

    ComPtr<ID3D12GraphicsCommandList> commandList;
    if (failed("ID3D12Device::CreateCommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)))) {
        return 1;
    }

    // Keep this ratio in the Balanced band so the smoke targets a stable
    // Balanced/R2160/p458 structural case.  A 2.0x ratio would correctly select
    // the Performance model and would be the wrong target for this recorder
    // smoke.
    constexpr std::uint32_t renderWidth = 2024;
    constexpr std::uint32_t renderHeight = 847;
    constexpr std::uint32_t displayWidth = 3440;
    constexpr std::uint32_t displayHeight = 1440;

    ComPtr<ID3D12Resource> inputColor;
    ComPtr<ID3D12Resource> inputDepth;
    ComPtr<ID3D12Resource> inputMotionVectors;
    ComPtr<ID3D12Resource> output;
    if (!createTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_NONE, L"APUSR test input color", inputColor) ||
        !createTexture(device.Get(), DXGI_FORMAT_R32_FLOAT, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_NONE, L"APUSR test depth", inputDepth) ||
        !createTexture(device.Get(), DXGI_FORMAT_R16G16_FLOAT, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_NONE, L"APUSR test motion vectors", inputMotionVectors) ||
        !createTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, displayWidth, displayHeight, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"APUSR test output", output)) {
        return 1;
    }

    Dx12Fsr4ReferenceCreateDesc create{};
    create.device = NativeHandle{device.Get()};
    create.repoRoot = APUSR_REPO_ROOT;
    create.runner.maxRenderSize = RenderSize{.width = renderWidth, .height = renderHeight};
    create.runner.maxUpscaleSize = DisplaySize{.width = displayWidth, .height = displayHeight};
    create.runner.features.displayResolutionMotionVectors = false;
    create.runner.features.motionVectorJitterCancellation = false;
    create.runner.features.depthInverted = true;
    create.runner.features.autoExposure = true;
    create.runner.initialPreset = Preset::Balanced;
    create.shaderProvider.enablePackagedCache = false;
    create.shaderProvider.enableEmbeddedCache = true;
    create.shaderProvider.allowRuntimeCompilation = false;
    create.shaderProvider.requireCompletePrecompiledCache = true;
    create.enableReflectionManifestValidation = false;
    create.requireCompleteReflectionCoverage = false;

    std::unique_ptr<Dx12Fsr4ReferenceRuntime> runtime = createDx12Fsr4ReferenceRuntime();
    if (!runtime) {
        std::cerr << "createDx12Fsr4ReferenceRuntime returned null\n";
        return 1;
    }
    if (failedStatus("Dx12Fsr4ReferenceRuntime::initialize", runtime->initialize(create))) {
        return 1;
    }

    Dx12Fsr4ReferenceFrameDesc frame{};
    frame.commandList = NativeHandle{commandList.Get()};
    frame.runner.frame.frameIndex = 1;
    frame.runner.frame.renderSize = RenderSize{.width = renderWidth, .height = renderHeight};
    frame.runner.frame.displaySize = DisplaySize{.width = displayWidth, .height = displayHeight};
    frame.runner.frame.jitter = Jitter{.x = 0.25f, .y = -0.125f};
    frame.runner.frame.options.motionVectorScale = MotionVectorScale{.x = -static_cast<float>(renderWidth), .y = static_cast<float>(renderHeight)};
    frame.runner.frame.options.features = create.runner.features;
    frame.runner.frame.options.hasDepth = true;
    frame.runner.frame.options.hasExposure = false;
    frame.runner.frame.options.preExposure = 1.0f;
    frame.runner.frame.options.sharpeningRequested = true;
    frame.runner.frame.options.sharpness = 0.3f;
    frame.runner.frame.options.resetHistory = true;
    frame.runner.frame.camera.nearPlane = 0.1f;
    frame.runner.frame.camera.farPlane = 1000.0f;
    frame.runner.frame.camera.verticalFovRadians = 1.04719755f;
    frame.runner.residentModelPreset = Preset::Balanced;
    frame.externalResources.inputColor = NativeHandle{inputColor.Get()};
    frame.externalResources.inputDepth = NativeHandle{inputDepth.Get()};
    frame.externalResources.inputMotionVectors = NativeHandle{inputMotionVectors.Get()};
    frame.externalResources.output = NativeHandle{output.Get()};
    frame.externalResources.inputColorState = static_cast<std::uint64_t>(D3D12_RESOURCE_STATE_COMMON);
    frame.externalResources.inputDepthState = static_cast<std::uint64_t>(D3D12_RESOURCE_STATE_COMMON);
    frame.externalResources.inputMotionVectorsState = static_cast<std::uint64_t>(D3D12_RESOURCE_STATE_COMMON);
    frame.externalResources.outputState = static_cast<std::uint64_t>(D3D12_RESOURCE_STATE_COMMON);
    frame.externalResources.restoreExternalResourceStatesAfterDispatch = false;

    Dx12Fsr4ReferenceBackendPlan plan{};
    if (failedStatus("Dx12Fsr4ReferenceRuntime::planFrame", runtime->planFrame(frame, plan))) {
        return 1;
    }
    if (plan.framePlan.frameCapacity != FrameCapacity::R2160 || plan.framePlan.permutationKey != 458u) {
        std::cerr << "unexpected embedded-cache smoke key capacity=" << frameCapacityName(plan.framePlan.frameCapacity)
                  << " permutation=" << plan.framePlan.permutationKey << "\n";
        return 1;
    }
    if (plan.resourceRealizationPlan.missingRequiredExternalCount != 0 || plan.descriptorRealizationPlan.missingRequiredResourceDescriptorCount != 0) {
        std::cerr << "record smoke unexpectedly has missing required resources: external="
                  << plan.resourceRealizationPlan.missingRequiredExternalCount
                  << " descriptors=" << plan.descriptorRealizationPlan.missingRequiredResourceDescriptorCount << "\n";
        return 1;
    }

    const Status recordStatus = runtime->recordFrame(frame);
    if (failedStatus("Dx12Fsr4ReferenceRuntime::recordFrame", recordStatus)) {
        std::cerr << "DX12 record smoke device shaderModel=" << shaderModelName(queryHighestShaderModel(device.Get())) << "\n";
        dumpD3D12Messages(device.Get());
        return 1;
    }
    if (failed("ID3D12GraphicsCommandList::Close", commandList->Close())) {
        return 1;
    }

    ID3D12CommandList* lists[] = {commandList.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (failed("ID3D12Device::CreateFence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return 1;
    }
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr) {
        std::cerr << "CreateEventW failed\n";
        return 1;
    }
    if (failed("ID3D12CommandQueue::Signal", queue->Signal(fence.Get(), 1))) {
        CloseHandle(eventHandle);
        return 1;
    }
    if (fence->GetCompletedValue() < 1) {
        if (failed("ID3D12Fence::SetEventOnCompletion", fence->SetEventOnCompletion(1, eventHandle))) {
            CloseHandle(eventHandle);
            return 1;
        }
        WaitForSingleObject(eventHandle, INFINITE);
    }
    CloseHandle(eventHandle);

    if (device->GetDeviceRemovedReason() != S_OK) {
        failed("ID3D12Device::GetDeviceRemovedReason", device->GetDeviceRemovedReason());
        return 1;
    }

    std::cout << "apusr_fsr4_reference_dx12_record_smoke ok dispatches=" << plan.dispatchRecordPlan.size()
              << " descriptors=" << plan.descriptorRealizationPlan.shaderVisibleDescriptorCount
              << " constants=" << plan.constantUploadPlan.perFrameByteSize << "\n";
    return 0;
}

#else
int main()
{
    std::cout << "apusr_fsr4_reference_dx12_record_smoke skipped: native DX12 is not enabled for this platform.\n";
    return 77;
}
#endif
