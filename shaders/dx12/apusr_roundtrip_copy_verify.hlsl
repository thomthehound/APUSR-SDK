// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

struct ApusrRoundTripCopyVerifyConstants {
    uint expectedByteOffsetLow;
    uint expectedByteOffsetHigh;
    uint actualByteOffsetLow;
    uint actualByteOffsetHigh;
    uint byteSizeLow;
    uint byteSizeHigh;
    uint wordCount;
    uint pad0;
};

ConstantBuffer<ApusrRoundTripCopyVerifyConstants> ApusrVerify : register(b0);
ByteAddressBuffer ApusrExpectedTokens : register(t0);
ByteAddressBuffer ApusrActualTokens : register(t1);
RWByteAddressBuffer ApusrDiagnostics : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint wordIndex = dispatchThreadID.x;
    if (wordIndex >= ApusrVerify.wordCount) {
        return;
    }

    const uint relativeByteOffset = wordIndex * 4u;
    const uint expectedByteOffset = ApusrVerify.expectedByteOffsetLow + relativeByteOffset;
    const uint actualByteOffset = ApusrVerify.actualByteOffsetLow + relativeByteOffset;
    uint expected = ApusrExpectedTokens.Load(expectedByteOffset);
    uint actual = ApusrActualTokens.Load(actualByteOffset);

    const uint remainingBytes = ApusrVerify.byteSizeLow - relativeByteOffset;
    if (remainingBytes < 4u) {
        const uint mask = (1u << (remainingBytes * 8u)) - 1u;
        expected &= mask;
        actual &= mask;
    }

    if (expected != actual) {
        uint ignored;
        ApusrDiagnostics.InterlockedAdd(0u, 1u, ignored);
        ApusrDiagnostics.InterlockedMin(4u, relativeByteOffset, ignored);
        ApusrDiagnostics.InterlockedXor(8u, expected, ignored);
        ApusrDiagnostics.InterlockedXor(12u, actual, ignored);
    }
}
