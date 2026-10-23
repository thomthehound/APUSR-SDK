// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_protocol.h>

#include <cassert>

namespace {

void validateContract(const apusr::ProtocolIngressContract& contract, apusr::UpscalerProtocol protocol)
{
    assert(contract.protocol == protocol);
    assert(apusr::validateProtocolIngressContract(contract).succeeded());
    assert(apusr::protocolIngressFeedsCoreFrame(contract));
    assert(!contract.hotPathMayCallThirdPartyRuntime);
}

} // namespace

int main()
{
    validateContract(apusr::makeNativeApusrDx12IngressContract(), apusr::UpscalerProtocol::NativeApusr);
    validateContract(apusr::makeFidelityFxFsr4Dx12IngressContract(), apusr::UpscalerProtocol::FidelityFxFsr4);
    validateContract(apusr::makeDlssDx12TranslationIngressContract(), apusr::UpscalerProtocol::Dlss);
    validateContract(apusr::makeXessDx12TranslationIngressContract(), apusr::UpscalerProtocol::Xess);

    return 0;
}
