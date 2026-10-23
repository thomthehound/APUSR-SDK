// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_host_capture.h>

#include <cstdint>
#include <iostream>

#if defined(_WIN32)
#include <unknwn.h>

class MockUnknown final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** outObject) override
    {
        if (outObject == nullptr) {
            return E_POINTER;
        }
        *outObject = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        return --references_;
    }

private:
    ULONG references_ = 1;
};
#endif

namespace {

void* fakeHandle(const std::uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

#if defined(_WIN32)
void* queueHandle(MockUnknown& object)
{
    return static_cast<IUnknown*>(&object);
}
#else
void* queueHandle(const std::uintptr_t value)
{
    return fakeHandle(value);
}
#endif

} // namespace

int main()
{
    using namespace apusr;
    using namespace apusr::frontends;

    clearHostCapture();

    const HostCaptureResolvedQueue missing = resolveHostQueueForDevice(
        GraphicsApi::Dx12,
        NativeHandle{fakeHandle(0x1000)},
        HostCaptureQueueKind::Direct);
    if (missing.available) {
        std::cerr << "unexpected host queue before registration\n";
        return 1;
    }

#if defined(_WIN32)
    MockUnknown deviceObject;
    MockUnknown swapChainObject;
    MockUnknown replacementSwapChainObject;
    MockUnknown secondDeviceObject;
    MockUnknown lowPriorityQueueObject;
    MockUnknown explicitQueueObject;
    MockUnknown presentQueueObject;
    MockUnknown computeQueueObject;
    MockUnknown directQueueObject;
    void* device = queueHandle(deviceObject);
    void* swapChain = queueHandle(swapChainObject);
    void* replacementSwapChain = queueHandle(replacementSwapChainObject);
    void* secondDevice = queueHandle(secondDeviceObject);
    void* lowPriorityQueue = queueHandle(lowPriorityQueueObject);
    void* explicitQueue = queueHandle(explicitQueueObject);
    void* presentQueue = queueHandle(presentQueueObject);
    void* computeQueue = queueHandle(computeQueueObject);
    void* directQueue = queueHandle(directQueueObject);
#else
    void* device = fakeHandle(0x1111);
    void* swapChain = fakeHandle(0x2222);
    void* replacementSwapChain = fakeHandle(0x2A2A);
    void* secondDevice = fakeHandle(0x3333);
    void* lowPriorityQueue = queueHandle(0x2000);
    void* explicitQueue = queueHandle(0x3000);
    void* presentQueue = queueHandle(0x4000);
    void* computeQueue = queueHandle(0x5000);
    void* directQueue = queueHandle(0x6000);
#endif

    Status status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{device},
        .queue = NativeHandle{lowPriorityQueue},
        .queueKind = HostCaptureQueueKind::Direct,
        .source = HostCaptureSourceKind::D3d12ExecuteCommandLists,
        .frameIndex = 1,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }

    status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{device},
        .queue = NativeHandle{explicitQueue},
        .queueKind = HostCaptureQueueKind::Direct,
        .source = HostCaptureSourceKind::ExplicitNativeConfig,
        .frameIndex = 0,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }

    status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{device},
        .queue = NativeHandle{presentQueue},
        .swapChain = NativeHandle{swapChain},
        .queueKind = HostCaptureQueueKind::Direct,
        .source = HostCaptureSourceKind::NgxParameterBag,
        .frameIndex = 2,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }

    const HostCaptureResolvedQueue byDevice = resolveHostQueueForDevice(
        GraphicsApi::Dx12,
        NativeHandle{device},
        HostCaptureQueueKind::Direct);
    if (!byDevice.available || byDevice.queue.value != explicitQueue ||
        byDevice.source != HostCaptureSourceKind::ExplicitNativeConfig) {
        std::cerr << "device resolve should prefer explicit direct queue\n";
        return 1;
    }

    const HostCaptureResolvedQueue bySwapChain = resolveHostQueueForSwapChain(
        GraphicsApi::Dx12,
        NativeHandle{swapChain},
        HostCaptureQueueKind::Direct);
    if (!bySwapChain.available || bySwapChain.queue.value != presentQueue ||
        bySwapChain.source != HostCaptureSourceKind::NgxParameterBag) {
        std::cerr << "swapchain resolve should return the queue associated with that swapchain\n";
        return 1;
    }

    status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{secondDevice},
        .queue = NativeHandle{computeQueue},
        .queueKind = HostCaptureQueueKind::Compute,
        .source = HostCaptureSourceKind::ExplicitNativeConfig,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }
    status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{secondDevice},
        .queue = NativeHandle{directQueue},
        .queueKind = HostCaptureQueueKind::Direct,
        .source = HostCaptureSourceKind::DxgiSwapChain,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }

    const HostCaptureResolvedQueue preferredDirect = resolveHostQueueForDevice(
        GraphicsApi::Dx12,
        NativeHandle{secondDevice},
        HostCaptureQueueKind::Direct);
    if (!preferredDirect.available || preferredDirect.queue.value != directQueue) {
        std::cerr << "direct queue preference should beat a higher-source compute queue\n";
        return 1;
    }

    status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{device},
        .queue = NativeHandle{explicitQueue},
        .queueKind = HostCaptureQueueKind::Direct,
        .source = HostCaptureSourceKind::D3d12ExecuteCommandLists,
        .frameIndex = 9,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }
    const HostCaptureResolvedQueue afterLowPriorityDuplicate = resolveHostQueueForDevice(
        GraphicsApi::Dx12,
        NativeHandle{device},
        HostCaptureQueueKind::Direct);
    if (!afterLowPriorityDuplicate.available || afterLowPriorityDuplicate.queue.value != explicitQueue ||
        afterLowPriorityDuplicate.source != HostCaptureSourceKind::ExplicitNativeConfig) {
        std::cerr << "duplicate low-priority observations must not downgrade stronger queue source evidence\n";
        return 1;
    }

    status = registerHostQueue(HostCaptureQueueRegistrationDesc{
        .api = GraphicsApi::Dx12,
        .device = NativeHandle{device},
        .queue = NativeHandle{presentQueue},
        .swapChain = NativeHandle{replacementSwapChain},
        .queueKind = HostCaptureQueueKind::Direct,
        .source = HostCaptureSourceKind::NgxParameterBag,
        .frameIndex = 10,
    });
    if (!status.succeeded()) {
        std::cerr << status.message << '\n';
        return 1;
    }

    const HostCaptureResolvedQueue staleSwapChain = resolveHostQueueForSwapChain(
        GraphicsApi::Dx12,
        NativeHandle{swapChain},
        HostCaptureQueueKind::Direct);
    if (staleSwapChain.available) {
        std::cerr << "stale swapchain index must not resolve after the queue record moves to another swapchain\n";
        return 1;
    }

    const HostCaptureResolvedQueue replacementBySwapChain = resolveHostQueueForSwapChain(
        GraphicsApi::Dx12,
        NativeHandle{replacementSwapChain},
        HostCaptureQueueKind::Direct);
    if (!replacementBySwapChain.available || replacementBySwapChain.queue.value != presentQueue ||
        replacementBySwapChain.swapChain.value != replacementSwapChain) {
        std::cerr << "replacement swapchain should resolve to the moved queue record\n";
        return 1;
    }

    const HostCaptureStats stats = hostCaptureStats();
    if (stats.registrations != 7 || stats.duplicateRegistrations != 2 || stats.queueCandidates != 5 ||
        stats.queueRecordUpdates != 1 || stats.swapChainAssociations != 2 || stats.resolvesByDevice != 3 ||
        stats.resolvesBySwapChain != 2 || stats.misses != 2) {
        std::cerr << "unexpected HostCapture stats\n";
        return 1;
    }

    clearHostCapture();
    std::cout << "APUSR HostCapture smoke: multi-queue and swapchain-aware queue resolution validated\n";
    return 0;
}
