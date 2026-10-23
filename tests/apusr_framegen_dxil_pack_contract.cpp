// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_dxil.h"

#include <cstdint>
#include <iostream>
#include <set>

namespace {

bool validateBlob(
    ApusrFramegenShaderEffect effect,
    std::uint8_t passId,
    std::uint32_t options,
    std::set<const std::uint8_t*>& uniqueBlobs)
{
    ApusrFramegenShaderBlob blob{};
    if (!apusr::framegen::dxil::findShaderBlob(effect, passId, options, &blob)) {
        std::cerr << "missing shader: effect=" << static_cast<unsigned>(effect)
                  << " pass=" << static_cast<unsigned>(passId)
                  << " options=0x" << std::hex << options << std::dec << '\n';
        return false;
    }
    if (!blob.data || blob.size < 4 || blob.data[0] != 'D' || blob.data[1] != 'X' ||
        blob.data[2] != 'B' || blob.data[3] != 'C') {
        std::cerr << "invalid DXIL container\n";
        return false;
    }
    if (blob.effect != effect || blob.passId != passId || blob.permutationOptions != options) {
        std::cerr << "shader identity round-trip mismatch\n";
        return false;
    }
    uniqueBlobs.insert(blob.data);
    return true;
}

} // namespace

int main()
{
    constexpr std::uint32_t fiJitter = 1u << 1;
    constexpr std::uint32_t fiWave64 = 1u << 3;
    constexpr std::uint32_t fiFp16 = 1u << 4;
    constexpr std::uint32_t ofWave64 = 1u << 0;
    constexpr std::uint32_t ofFp16 = 1u << 1;
    constexpr std::uint32_t ofHdr = 1u << 2;

    std::set<const std::uint8_t*> uniqueBlobs;
    std::size_t logicalCount = 0;

    for (std::uint8_t pass = 0; pass < 11; ++pass) {
        for (std::uint32_t core = 0; core < 8; ++core) {
            for (std::uint32_t wave = 0; wave < 2; ++wave) {
                for (std::uint32_t fp16 = 0; fp16 < 2; ++fp16) {
                    const std::uint32_t options = core | (wave ? fiWave64 : 0u) | (fp16 ? fiFp16 : 0u);
                    if (!validateBlob(APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, pass, options, uniqueBlobs)) {
                        return 1;
                    }
                    ++logicalCount;
                }
            }
        }
    }

    for (std::uint8_t pass = 0; pass < 7; ++pass) {
        for (std::uint32_t hdr = 0; hdr < 2; ++hdr) {
            for (std::uint32_t wave = 0; wave < 2; ++wave) {
                for (std::uint32_t fp16 = 0; fp16 < 2; ++fp16) {
                    const std::uint32_t options = (hdr ? ofHdr : 0u) | (wave ? ofWave64 : 0u) | (fp16 ? ofFp16 : 0u);
                    if (!validateBlob(APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW, pass, options, uniqueBlobs)) {
                        return 1;
                    }
                    ++logicalCount;
                }
            }
        }
    }

    if (logicalCount != 408 || uniqueBlobs.size() != 108) {
        std::cerr << "pack cardinality mismatch: logical=" << logicalCount
                  << " unique=" << uniqueBlobs.size() << '\n';
        return 1;
    }

    // Jitter cancellation is consumed only by reconstruct-and-dilate. Every
    // precision/wave/depth/MV-resolution form of that pass must therefore have
    // distinct jittered DXIL, while later passes must remain deduplicated across
    // the jitter bit.
    for (std::uint32_t options = 0; options < 32; ++options) {
        if (options & fiJitter) {
            continue;
        }
        ApusrFramegenShaderBlob plain{};
        ApusrFramegenShaderBlob jittered{};
        if (!apusr::framegen::dxil::findShaderBlob(
                APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, 0, options, &plain) ||
            !apusr::framegen::dxil::findShaderBlob(
                APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, 0, options | fiJitter, &jittered) ||
            plain.data == jittered.data) {
            std::cerr << "reconstruct-and-dilate jitter permutation is inert: options=0x"
                      << std::hex << options << std::dec << '\n';
            return 1;
        }
        for (std::uint8_t pass = 1; pass < 11; ++pass) {
            if (!apusr::framegen::dxil::findShaderBlob(
                    APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, pass, options, &plain) ||
                !apusr::framegen::dxil::findShaderBlob(
                    APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, pass, options | fiJitter, &jittered) ||
                plain.data != jittered.data) {
                std::cerr << "unrelated FI pass changed across jitter bit: pass="
                          << static_cast<unsigned>(pass) << " options=0x"
                          << std::hex << options << std::dec << '\n';
                return 1;
            }
        }
    }

    ApusrFramegenShaderBlob invalid{};
    invalid.data = reinterpret_cast<const std::uint8_t*>(1);
    if (apusr::framegen::dxil::findShaderBlob(
            APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, 0, 0x80000000u, &invalid)) {
        std::cerr << "invalid permutation unexpectedly resolved\n";
        return 1;
    }
    if (invalid.data != nullptr || invalid.size != 0) {
        std::cerr << "failed lookup did not clear the output blob\n";
        return 1;
    }
    if (apusr::framegen::dxil::findShaderBlob(
            APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION, 0, 0u, nullptr)) {
        std::cerr << "null output unexpectedly accepted\n";
        return 1;
    }

    return 0;
}
