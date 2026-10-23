// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstddef>
#include <cstdint>

#if defined(__AIEARCH__) || defined(__AIE_ARCH__)
#define APUSR_AIE_COMPILE 1
#include <aie_api/aie.hpp>
#else
#define APUSR_AIE_COMPILE 0
#endif

namespace apusr::aie_middle_core {
namespace {

constexpr int kInputTileWidth = 16;
constexpr int kInputTileHeight = 8;
constexpr int kOutputTileWidth = 32;
constexpr int kOutputTileHeight = 16;
constexpr int kInputChannels = 64;
constexpr int kOutputChannels = 8;
constexpr int kPixelsPerGroup = 4;

constexpr int kMmulM = 4;
constexpr int kMmulK = 8;
constexpr int kMmulN = 8;
constexpr int kOutputElements = kMmulM * kMmulN;
constexpr int kActivationElements = kMmulM * kMmulK;
constexpr int kWeightElements = kMmulK * kMmulN;

constexpr int kLowGroups = (kInputTileWidth * kInputTileHeight) / kPixelsPerGroup;
constexpr int kInputChannelGroups = kInputChannels / kMmulK;
constexpr int kCt2dPhases = 4;

constexpr std::size_t kParamHeaderBytes = 4 * sizeof(std::int32_t);
constexpr std::size_t kCt2dWeightsBytes = kCt2dPhases * kInputChannelGroups * kWeightElements;
constexpr std::size_t kCt2dBiasBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kCt2dMultiplierBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kSkipMultiplierBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kSkipShiftBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kSkipZeroPointBytes = kOutputChannels * sizeof(std::int32_t);

constexpr std::size_t kCt2dWeightsOffset = kParamHeaderBytes;
constexpr std::size_t kCt2dBiasOffset = kCt2dWeightsOffset + kCt2dWeightsBytes;
constexpr std::size_t kCt2dMultiplierOffset = kCt2dBiasOffset + kCt2dBiasBytes;
constexpr std::size_t kSkipMultiplierOffset = kCt2dMultiplierOffset + kCt2dMultiplierBytes;
constexpr std::size_t kSkipShiftOffset = kSkipMultiplierOffset + kSkipMultiplierBytes;
constexpr std::size_t kSkipZeroPointOffset = kSkipShiftOffset + kSkipShiftBytes;
constexpr std::size_t kParamBytes = kSkipZeroPointOffset + kSkipZeroPointBytes;

static_assert(kParamBytes == 2224, "CT2D32 C8-chunk parameter payload must stay compact");

[[nodiscard]] inline std::int32_t signed_srs_scalar(const std::int64_t value, const std::int32_t right_shift)
{
    if (right_shift <= 0) {
        return static_cast<std::int32_t>(value << (-right_shift));
    }
    const std::int64_t rounding = std::int64_t{1} << (right_shift - 1);
    if (value >= 0) {
        return static_cast<std::int32_t>((value + rounding) >> right_shift);
    }
    return -static_cast<std::int32_t>(((-value) + rounding) >> right_shift);
}

[[nodiscard]] inline std::int8_t saturate_i8_scalar(const std::int32_t value)
{
    return static_cast<std::int8_t>(value > 127 ? 127 : (value < -128 ? -128 : value));
}

[[nodiscard]] inline const std::int32_t* as_i32_ptr(const std::int8_t* base, const std::size_t byte_offset)
{
    return reinterpret_cast<const std::int32_t*>(base + byte_offset);
}

inline void gather_input_c64_block(
    const std::int8_t* input_tile_16x8_c64,
    const int low_y,
    const int low_x_base,
    const int channel_group,
    std::int8_t* activation_m8)
{
    const int channel_base = channel_group * kMmulK;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int input_x = low_x_base + pixel;
        const std::int8_t* input_pixel = input_tile_16x8_c64 + ((low_y * kInputTileWidth + input_x) * kInputChannels) + channel_base;
        for (int lane = 0; lane < kMmulK; ++lane) {
            activation_m8[pixel * kMmulK + lane] = input_pixel[lane];
        }
    }
}

inline void fill_repeated_i32_tile(const std::int32_t* compact_c8, std::int32_t* tiled_m8)
{
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        for (int lane = 0; lane < kMmulN; ++lane) {
            tiled_m8[pixel * kMmulN + lane] = compact_c8[lane];
        }
    }
}

inline void gather_skip_c8_i32_tile(
    const std::int8_t* skip_tile_32x16_c8,
    const std::int32_t* multiplier_c8,
    const std::int32_t* shift_c8,
    const std::int32_t* zero_point_c8,
    const int low_y,
    const int low_x_base,
    const int phase,
    std::int32_t* skip_m8)
{
    const int phase_y = phase >> 1;
    const int phase_x = phase & 1;
    const int output_y = low_y * 2 + phase_y;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int output_x = (low_x_base + pixel) * 2 + phase_x;
        const std::int8_t* skip_pixel = skip_tile_32x16_c8 + ((output_y * kOutputTileWidth + output_x) * kOutputChannels);
        for (int lane = 0; lane < kMmulN; ++lane) {
            const std::int64_t centered = static_cast<std::int64_t>(skip_pixel[lane] - zero_point_c8[lane]);
            skip_m8[pixel * kMmulN + lane] = signed_srs_scalar(centered * multiplier_c8[lane], shift_c8[lane]);
        }
    }
}

inline void scatter_output_c8_block(
    std::int8_t* output_tile_32x16_c8,
    const int low_y,
    const int low_x_base,
    const int phase,
    const std::int8_t* block_m8)
{
    const int phase_y = phase >> 1;
    const int phase_x = phase & 1;
    const int output_y = low_y * 2 + phase_y;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int output_x = (low_x_base + pixel) * 2 + phase_x;
        std::int8_t* output_pixel = output_tile_32x16_c8 + ((output_y * kOutputTileWidth + output_x) * kOutputChannels);
        for (int lane = 0; lane < kMmulN; ++lane) {
            output_pixel[lane] = block_m8[pixel * kMmulN + lane];
        }
    }
}

} // namespace

extern "C" void apusr_middle_core_ct2d32_ochunk_add_tile_i8(
    const std::int8_t* input_tile_16x8_c64,
    const std::int8_t* skip_tile_32x16_c8,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_32x16_c8)
{
    const std::int32_t* header = as_i32_ptr(param_blob, 0);
    const std::int32_t ct2d_right_shift = header[0];

    const std::int8_t* ct2d_weights = param_blob + kCt2dWeightsOffset;
    const std::int32_t* ct2d_bias = as_i32_ptr(param_blob, kCt2dBiasOffset);
    const std::int32_t* ct2d_multiplier = as_i32_ptr(param_blob, kCt2dMultiplierOffset);
    const std::int32_t* skip_multiplier = as_i32_ptr(param_blob, kSkipMultiplierOffset);
    const std::int32_t* skip_shift = as_i32_ptr(param_blob, kSkipShiftOffset);
    const std::int32_t* skip_zero_point = as_i32_ptr(param_blob, kSkipZeroPointOffset);

#if APUSR_AIE_COMPILE
    using MmulS8S8 = aie::mmul<kMmulM, kMmulK, kMmulN, int8, int8>;
    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);
#endif

    for (int group = 0; group < kLowGroups; ++group) {
        const int low_y = group / (kInputTileWidth / kPixelsPerGroup);
        const int low_x_base = (group % (kInputTileWidth / kPixelsPerGroup)) * kPixelsPerGroup;

        alignas(32) std::int8_t activation_block[kActivationElements]{};
        alignas(32) std::int32_t bias_block[kOutputElements]{};
        alignas(32) std::int32_t multiplier_block[kOutputElements]{};
        alignas(32) std::int32_t skip_block[kOutputElements]{};
        alignas(32) std::int8_t result_block[kOutputElements]{};

        for (int phase = 0; phase < kCt2dPhases; ++phase) {
#if APUSR_AIE_COMPILE
            MmulS8S8 accumulator = aie::zeros<acc32, kOutputElements>();
#else
            std::int32_t scalar_accumulator[kOutputElements]{};
#endif
            for (int channel_group = 0; channel_group < kInputChannelGroups; ++channel_group) {
                gather_input_c64_block(input_tile_16x8_c64, low_y, low_x_base, channel_group, activation_block);
                const std::size_t weight_offset =
                    (static_cast<std::size_t>(phase) * kInputChannelGroups + channel_group) * kWeightElements;
#if APUSR_AIE_COMPILE
                const aie::vector<int8, kActivationElements> activation_vector = aie::load_v<kActivationElements>(activation_block);
                const aie::vector<int8, kWeightElements> weight_vector = aie::load_v<kWeightElements>(ct2d_weights + weight_offset);
                accumulator.mac(activation_vector, weight_vector);
#else
                const std::int8_t* weight_block = ct2d_weights + weight_offset;
                for (int pixel = 0; pixel < kMmulM; ++pixel) {
                    for (int oc = 0; oc < kMmulN; ++oc) {
                        for (int ic = 0; ic < kMmulK; ++ic) {
                            scalar_accumulator[pixel * kMmulN + oc] +=
                                static_cast<std::int32_t>(activation_block[pixel * kMmulK + ic]) * weight_block[ic * kMmulN + oc];
                        }
                    }
                }
#endif
            }

            fill_repeated_i32_tile(ct2d_bias, bias_block);
            fill_repeated_i32_tile(ct2d_multiplier, multiplier_block);
            gather_skip_c8_i32_tile(skip_tile_32x16_c8, skip_multiplier, skip_shift, skip_zero_point, low_y, low_x_base, phase, skip_block);
#if APUSR_AIE_COMPILE
            const auto dot_accumulator = accumulator.to_accum();
            const aie::vector<int32, kOutputElements> bias_vector = aie::load_v<kOutputElements>(bias_block);
            const aie::vector<int32, kOutputElements> skip_vector = aie::load_v<kOutputElements>(skip_block);
            const aie::vector<int32, kOutputElements> multiplier_vector = aie::load_v<kOutputElements>(multiplier_block);
            auto biased_accumulator = aie::add(dot_accumulator, bias_vector);
            biased_accumulator = aie::add(biased_accumulator, skip_vector);
            const aie::vector<int32, kOutputElements> biased_vector = aie::to_vector<int32>(biased_accumulator, 0);
            const auto scaled_accumulator = aie::mul<acc64>(biased_vector, multiplier_vector);
            const aie::vector<int8, kOutputElements> result_vector = aie::to_vector<int8>(scaled_accumulator, ct2d_right_shift);
            aie::store_v(result_block, result_vector);
#else
            for (int lane = 0; lane < kOutputElements; ++lane) {
                const std::int64_t scaled =
                    static_cast<std::int64_t>(scalar_accumulator[lane] + bias_block[lane] + skip_block[lane]) * multiplier_block[lane];
                result_block[lane] = saturate_i8_scalar(signed_srs_scalar(scaled, ct2d_right_shift));
            }
#endif
            scatter_output_c8_block(output_tile_32x16_c8, low_y, low_x_base, phase, result_block);
        }
    }
}

} // namespace apusr::aie_middle_core
