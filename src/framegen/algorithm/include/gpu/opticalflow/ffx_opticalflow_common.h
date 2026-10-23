// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#if !defined(FFX_OPTICALFLOW_COMMON_H)
#define FFX_OPTICALFLOW_COMMON_H

#if defined(FFX_GPU)

#define SCD_OUTPUT_SCENE_CHANGE_SLOT            0
#define SCD_OUTPUT_HISTORY_BITS_SLOT            1
#define SCD_OUTPUT_COMPLETED_WORKGROUPS_SLOT    2


#define ffxClamp(x, a, b) (ffxMax(a, ffxMin(b, x)))


FfxUInt32 GetPackedLuma(FfxInt32 width, FfxInt32 x, FfxUInt32 luma0, FfxUInt32 luma1, FfxUInt32 luma2, FfxUInt32 luma3)
{
    FfxUInt32 packedLuma = luma0 | (luma1 << 8) | (luma2 << 16) | (luma3 << 24);

    if (x < 0)
    {
        FfxUInt32 outOfScreenFiller = packedLuma & 0xffu;
        if (x <= -1)
            packedLuma = (packedLuma << 8) | outOfScreenFiller;
        if (x <= -2)
            packedLuma = (packedLuma << 8) | outOfScreenFiller;
        if (x <= -3)
            packedLuma = (packedLuma << 8) | outOfScreenFiller;
    }
    else if (x > width - 4)
    {
        FfxUInt32 outOfScreenFiller = packedLuma & 0xff000000u;
        if (x >= width - 3)
            packedLuma = (packedLuma >> 8) | outOfScreenFiller;
        if (x >= width - 2)
            packedLuma = (packedLuma >> 8) | outOfScreenFiller;
        if (x >= width - 1)
            packedLuma = (packedLuma >> 8) | outOfScreenFiller;
    }
    return packedLuma;
}

#if defined(APUSR_OPTICALFLOW_VULKAN_PACKED_SAD) && APUSR_OPTICALFLOW_VULKAN_PACKED_SAD
// Standard SPIR-V has no MSAD4 equivalent. Use a SWAR byte-wise absolute
// difference followed by native packed UDOT instead of scalarizing sixteen
// byte extracts/subtracts for every QSad. The compare is performed per byte
// without cross-byte borrow: low 7-bit subtraction is biased with each lane's
// high bit, while differing original high bits select the unsigned ordering.
FfxUInt32 ApusrPackedByteAbsDiff(FfxUInt32 a, FfxUInt32 b)
{
    const FfxUInt32 kHigh = 0x80808080u;
    const FfxUInt32 kLow = 0x7f7f7f7fu;

    // Compare all four unsigned byte lanes without allowing a borrow to cross
    // a lane boundary.  If the high bits differ they alone determine the
    // ordering; otherwise the biased low-seven-bit subtraction does.  Keep the
    // original XOR so the selected max/min can be formed with one conditional
    // swap rather than two pairs of AND/OR selects.  QSad executes this helper
    // 64 times per search thread, so removing even a few scalar integer ops
    // here has unusually high leverage on Vulkan optical flow.
    const FfxUInt32 xorValue = a ^ b;
    const FfxUInt32 aHigh = a & kHigh;
    const FfxUInt32 highDiff = xorValue & kHigh;
    const FfxUInt32 lowGe = ((((a & kLow) | kHigh) - (b & kLow)) & kHigh);
    const FfxUInt32 geBit = lowGe ^ (highDiff & (lowGe ^ aHigh));
    const FfxUInt32 geMask = (geBit >> 7) * 0xffu;
    const FfxUInt32 swap = xorValue & geMask;
    const FfxUInt32 maxValue = b ^ swap;
    const FfxUInt32 minValue = a ^ swap;
    return maxValue - minValue;
}
#endif

FfxUInt32 Sad(FfxUInt32 a, FfxUInt32 b)
{
#if defined(APUSR_OPTICALFLOW_VULKAN_PACKED_SAD) && APUSR_OPTICALFLOW_VULKAN_PACKED_SAD
    return dot4add_u8packed(ApusrPackedByteAbsDiff(a, b), 0x01010101u, 0u);
#elif FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1
    return msad4(a, FfxUInt32x2(b, 0), FfxUInt32x4(0, 0, 0, 0)).x;
#else
    return abs(FfxInt32((a >> 0) & 0xffu) - FfxInt32((b >> 0) & 0xffu)) +
        abs(FfxInt32((a >> 8) & 0xffu) - FfxInt32((b >> 8) & 0xffu)) +
        abs(FfxInt32((a >> 16) & 0xffu) - FfxInt32((b >> 16) & 0xffu)) +
        abs(FfxInt32((a >> 24) & 0xffu) - FfxInt32((b >> 24) & 0xffu));
#endif
}

FfxUInt32x4 QSad(FfxUInt32 a0, FfxUInt32 a1, FfxUInt32 b)
{
#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1 && !(defined(APUSR_OPTICALFLOW_VULKAN_PACKED_SAD) && APUSR_OPTICALFLOW_VULKAN_PACKED_SAD)
    return msad4(b, FfxUInt32x2(a0, a1), FfxUInt32x4(0, 0, 0, 0));
#else
    FfxUInt32x4 sad;
    sad.x = Sad(a0, b);

    a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
    a1 >>= 8;
    sad.y = Sad(a0, b);

    a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
    a1 >>= 8;
    sad.z = Sad(a0, b);

    a0 = (a0 >> 8) | ((a1 & 0xffu) << 24);
    sad.w = Sad(a0, b);
    return sad;
#endif
}

#endif  // #if defined(FFX_GPU)

#endif //!defined(FFX_OPTICALFLOW_COMMON_H)
