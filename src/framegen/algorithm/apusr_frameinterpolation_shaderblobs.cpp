// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "ffx_frameinterpolation_shaderblobs.h"
#include "apusr_framegen_dxil.h"

#include <cstring>

ApusrFramegenResult frameInterpolationGetPermutationBlobByIndex(
    ApusrFramegenPass passId,
    uint32_t permutationOptions,
    ApusrFramegenShaderBlob* outBlob)
{
    if (!outBlob || passId >= APUSR_FRAMEINTERPOLATION_PASS_COUNT) {
        if (outBlob) {
            std::memset(outBlob, 0, sizeof(*outBlob));
        }
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    if (!apusr::framegen::dxil::findShaderBlob(
            APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION,
            static_cast<std::uint8_t>(passId),
            permutationOptions,
            outBlob)) {
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult frameInterpolationIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    isWave64 = (permutationOptions & (1u << 3)) != 0;
    return APUSR_FRAMEGEN_OK;
}
