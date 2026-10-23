// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

// Product-shaped stage11 variant: accepts grouped C8 halos from stage10 directly.

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

constexpr int kInputTileWidth = 18;
constexpr int kInputTileHeight = 10;
constexpr int kLowTileWidth = 16;
constexpr int kLowTileHeight = 8;
constexpr int kOutputTileWidth = 32;
constexpr int kOutputTileHeight = 16;
constexpr int kInputChannels = 32;
constexpr int kChannelChunk = 8;
constexpr int kSpatialChannels = 16;
constexpr int kHiddenChannels = 64;
constexpr int kOutputChannels = 16;
constexpr int kPixelsPerGroup = 4;

constexpr int kMmulM = 4;
constexpr int kMmulK = 8;
constexpr int kMmulN = 8;
constexpr int kOutputElements = kMmulM * kMmulN;
constexpr int kActivationElements = kMmulM * kMmulK;
constexpr int kWeightElements = kMmulK * kMmulN;

constexpr int kLowGroups = (kLowTileWidth * kLowTileHeight) / kPixelsPerGroup;
constexpr int kInputChannelGroups = kInputChannels / kMmulK;
constexpr int kSpatialChannelGroups = kSpatialChannels / kMmulK;
constexpr int kHiddenGroups = kHiddenChannels / kMmulK;
constexpr int kSpatialOutputChunks = kSpatialChannels / kMmulN;
constexpr int kExpandOutputChunks = kHiddenChannels / kMmulN;
constexpr int kContractOutputChunks = kInputChannels / kMmulN;
constexpr int kCt2dOutputChunks = kOutputChannels / kMmulN;
constexpr int kCt2dPhases = 4;

constexpr int kSpatialLocalElements = kContractOutputChunks * kOutputElements;
constexpr int kExpandLocalElements = kExpandOutputChunks * kOutputElements;
constexpr int kFnbLocalElements = kContractOutputChunks * kOutputElements;

constexpr std::size_t kParamHeaderBytes = 4 * sizeof(std::int32_t);
constexpr std::size_t kSpatialWeightsBytes = 9 * kSpatialOutputChunks * kSpatialChannelGroups * kWeightElements;
constexpr std::size_t kSpatialBiasBytes = kSpatialChannels * sizeof(std::int32_t);
constexpr std::size_t kSpatialMultiplierBytes = kSpatialChannels * sizeof(std::int32_t);
constexpr std::size_t kExpandWeightsBytes = kExpandOutputChunks * kInputChannelGroups * kWeightElements;
constexpr std::size_t kExpandBiasBytes = kHiddenChannels * sizeof(std::int32_t);
constexpr std::size_t kExpandMultiplierBytes = kHiddenChannels * sizeof(std::int32_t);
constexpr std::size_t kContractWeightsBytes = kContractOutputChunks * kHiddenGroups * kWeightElements;
constexpr std::size_t kContractBiasBytes = kInputChannels * sizeof(std::int32_t);
constexpr std::size_t kContractMultiplierBytes = kInputChannels * sizeof(std::int32_t);
constexpr std::size_t kResidualMultiplierBytes = kInputChannels * sizeof(std::int32_t);
constexpr std::size_t kResidualShiftBytes = kInputChannels * sizeof(std::int32_t);
constexpr std::size_t kResidualZeroPointBytes = kInputChannels * sizeof(std::int32_t);
constexpr std::size_t kCt2dWeightsBytes = kCt2dPhases * kCt2dOutputChunks * kInputChannelGroups * kWeightElements;
constexpr std::size_t kCt2dBiasBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kCt2dMultiplierBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kSkipMultiplierBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kSkipShiftBytes = kOutputChannels * sizeof(std::int32_t);
constexpr std::size_t kSkipZeroPointBytes = kOutputChannels * sizeof(std::int32_t);

constexpr std::size_t kSpatialWeightsOffset = kParamHeaderBytes;
constexpr std::size_t kSpatialBiasOffset = kSpatialWeightsOffset + kSpatialWeightsBytes;
constexpr std::size_t kSpatialMultiplierOffset = kSpatialBiasOffset + kSpatialBiasBytes;
constexpr std::size_t kExpandWeightsOffset = kSpatialMultiplierOffset + kSpatialMultiplierBytes;
constexpr std::size_t kExpandBiasOffset = kExpandWeightsOffset + kExpandWeightsBytes;
constexpr std::size_t kExpandMultiplierOffset = kExpandBiasOffset + kExpandBiasBytes;
constexpr std::size_t kContractWeightsOffset = kExpandMultiplierOffset + kExpandMultiplierBytes;
constexpr std::size_t kContractBiasOffset = kContractWeightsOffset + kContractWeightsBytes;
constexpr std::size_t kContractMultiplierOffset = kContractBiasOffset + kContractBiasBytes;
constexpr std::size_t kResidualMultiplierOffset = kContractMultiplierOffset + kContractMultiplierBytes;
constexpr std::size_t kResidualShiftOffset = kResidualMultiplierOffset + kResidualMultiplierBytes;
constexpr std::size_t kResidualZeroPointOffset = kResidualShiftOffset + kResidualShiftBytes;
constexpr std::size_t kCt2dWeightsOffset = kResidualZeroPointOffset + kResidualZeroPointBytes;
constexpr std::size_t kCt2dBiasOffset = kCt2dWeightsOffset + kCt2dWeightsBytes;
constexpr std::size_t kCt2dMultiplierOffset = kCt2dBiasOffset + kCt2dBiasBytes;
constexpr std::size_t kSkipMultiplierOffset = kCt2dMultiplierOffset + kCt2dMultiplierBytes;
constexpr std::size_t kSkipShiftOffset = kSkipMultiplierOffset + kSkipMultiplierBytes;
constexpr std::size_t kSkipZeroPointOffset = kSkipShiftOffset + kSkipShiftBytes;

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

[[nodiscard]] inline std::uint8_t saturate_u8_scalar(const std::int32_t value)
{
    return static_cast<std::uint8_t>(value > 255 ? 255 : (value < 0 ? 0 : value));
}

[[nodiscard]] inline const std::int32_t* as_i32_ptr(const std::int8_t* base, const std::size_t byte_offset)
{
    return reinterpret_cast<const std::int32_t*>(base + byte_offset);
}


[[nodiscard]] inline const std::int8_t* select_c8_chunk(
    const std::int8_t* chunk0_18x10_c8,
    const std::int8_t* chunk1_18x10_c8,
    const std::int8_t* chunk2_18x10_c8,
    const std::int8_t* chunk3_18x10_c8,
    const int chunk_index)
{
    switch (chunk_index) {
    case 0:
        return chunk0_18x10_c8;
    case 1:
        return chunk1_18x10_c8;
    case 2:
        return chunk2_18x10_c8;
    default:
        return chunk3_18x10_c8;
    }
}

inline void fill_repeated_i32_tile(const std::int32_t* compact_c, const int channel_base, std::int32_t* tiled_m8)
{
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        for (int lane = 0; lane < kMmulN; ++lane) {
            tiled_m8[pixel * kMmulN + lane] = compact_c[channel_base + lane];
        }
    }
}

inline void gather_spatial_activation_block(
    const std::int8_t* input_chunk0_18x10_c8,
    const std::int8_t* input_chunk1_18x10_c8,
    const std::int8_t* input_chunk2_18x10_c8,
    const std::int8_t* input_chunk3_18x10_c8,
    const int output_y,
    const int output_x_base,
    const int tap_y,
    const int tap_x,
    const int channel_group,
    std::int8_t* activation_m8)
{
    const std::int8_t* input_chunk = select_c8_chunk(
        input_chunk0_18x10_c8,
        input_chunk1_18x10_c8,
        input_chunk2_18x10_c8,
        input_chunk3_18x10_c8,
        channel_group);
    const int input_y = output_y + tap_y;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int input_x = output_x_base + pixel + tap_x;
        const std::int8_t* input_pixel = input_chunk + ((input_y * kInputTileWidth + input_x) * kChannelChunk);
        for (int lane = 0; lane < kMmulK; ++lane) {
            activation_m8[pixel * kMmulK + lane] = input_pixel[lane];
        }
    }
}

inline void gather_center_i8_block(
    const std::int8_t* input_chunk0_18x10_c8,
    const std::int8_t* input_chunk1_18x10_c8,
    const std::int8_t* input_chunk2_18x10_c8,
    const std::int8_t* input_chunk3_18x10_c8,
    const int output_y,
    const int output_x_base,
    const int channel_base,
    std::int8_t* block_m8)
{
    const int chunk_index = channel_base / kChannelChunk;
    const std::int8_t* input_chunk = select_c8_chunk(
        input_chunk0_18x10_c8,
        input_chunk1_18x10_c8,
        input_chunk2_18x10_c8,
        input_chunk3_18x10_c8,
        chunk_index);
    const int input_y = output_y + 1;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int input_x = output_x_base + pixel + 1;
        const std::int8_t* input_pixel = input_chunk + ((input_y * kInputTileWidth + input_x) * kChannelChunk);
        for (int lane = 0; lane < kMmulN; ++lane) {
            block_m8[pixel * kMmulN + lane] = input_pixel[lane];
        }
    }
}

inline void gather_center_residual_i32_tile(
    const std::int8_t* input_chunk0_18x10_c8,
    const std::int8_t* input_chunk1_18x10_c8,
    const std::int8_t* input_chunk2_18x10_c8,
    const std::int8_t* input_chunk3_18x10_c8,
    const std::int32_t* multiplier_c32,
    const std::int32_t* shift_c32,
    const std::int32_t* zero_point_c32,
    const int output_y,
    const int output_x_base,
    const int channel_base,
    std::int32_t* residual_m8)
{
    const int chunk_index = channel_base / kChannelChunk;
    const std::int8_t* input_chunk = select_c8_chunk(
        input_chunk0_18x10_c8,
        input_chunk1_18x10_c8,
        input_chunk2_18x10_c8,
        input_chunk3_18x10_c8,
        chunk_index);
    const int input_y = output_y + 1;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int input_x = output_x_base + pixel + 1;
        const std::int8_t* input_pixel = input_chunk + ((input_y * kInputTileWidth + input_x) * kChannelChunk);
        for (int lane = 0; lane < kMmulN; ++lane) {
            const int channel = channel_base + lane;
            const std::int64_t centered = static_cast<std::int64_t>(input_pixel[lane] - zero_point_c32[channel]);
            residual_m8[pixel * kMmulN + lane] = signed_srs_scalar(centered * multiplier_c32[channel], shift_c32[channel]);
        }
    }
}

inline void gather_skip_i32_tile(
    const std::int8_t* skip_tile,
    const std::int32_t* multiplier_c16,
    const std::int32_t* shift_c16,
    const std::int32_t* zero_point_c16,
    const int low_y,
    const int low_x_base,
    const int phase,
    const int channel_base,
    std::int32_t* skip_m8)
{
    const int phase_y = phase >> 1;
    const int phase_x = phase & 1;
    const int output_y = low_y * 2 + phase_y;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int output_x = (low_x_base + pixel) * 2 + phase_x;
        const std::int8_t* skip_pixel = skip_tile + ((output_y * kOutputTileWidth + output_x) * kOutputChannels);
        for (int lane = 0; lane < kMmulN; ++lane) {
            const int channel = channel_base + lane;
            const std::int64_t centered = static_cast<std::int64_t>(skip_pixel[channel] - zero_point_c16[channel]);
            skip_m8[pixel * kMmulN + lane] = signed_srs_scalar(centered * multiplier_c16[channel], shift_c16[channel]);
        }
    }
}

inline void scatter_ct2d_c8_block(
    std::int8_t* output_tile,
    const int low_y,
    const int low_x_base,
    const int phase,
    const int channel_base,
    const std::int8_t* block_m8)
{
    const int phase_y = phase >> 1;
    const int phase_x = phase & 1;
    const int output_y = low_y * 2 + phase_y;
    for (int pixel = 0; pixel < kPixelsPerGroup; ++pixel) {
        const int output_x = (low_x_base + pixel) * 2 + phase_x;
        std::int8_t* output_pixel = output_tile + ((output_y * kOutputTileWidth + output_x) * kOutputChannels) + channel_base;
        for (int lane = 0; lane < kMmulN; ++lane) {
            output_pixel[lane] = block_m8[pixel * kMmulN + lane];
        }
    }
}

} // namespace

extern "C" void apusr_middle_core_fnb32_c8group_ct2d16_add_tile_i8(
    const std::int8_t* input_chunk0_18x10_c8,
    const std::int8_t* input_chunk1_18x10_c8,
    const std::int8_t* input_chunk2_18x10_c8,
    const std::int8_t* input_chunk3_18x10_c8,
    const std::int8_t* skip_tile_32x16_c16,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_32x16_c16)
{
    const std::int32_t* header = as_i32_ptr(param_blob, 0);
    const std::int32_t spatial_right_shift = header[0];
    const std::int32_t expand_right_shift = header[1];
    const std::int32_t contract_right_shift = header[2];
    const std::int32_t ct2d_right_shift = header[3];

    const std::int8_t* spatial_weights = param_blob + kSpatialWeightsOffset;
    const std::int32_t* spatial_bias = as_i32_ptr(param_blob, kSpatialBiasOffset);
    const std::int32_t* spatial_multiplier = as_i32_ptr(param_blob, kSpatialMultiplierOffset);
    const std::int8_t* expand_weights = param_blob + kExpandWeightsOffset;
    const std::int32_t* expand_bias = as_i32_ptr(param_blob, kExpandBiasOffset);
    const std::int32_t* expand_multiplier = as_i32_ptr(param_blob, kExpandMultiplierOffset);
    const std::int8_t* contract_weights = param_blob + kContractWeightsOffset;
    const std::int32_t* contract_bias = as_i32_ptr(param_blob, kContractBiasOffset);
    const std::int32_t* contract_multiplier = as_i32_ptr(param_blob, kContractMultiplierOffset);
    const std::int32_t* residual_multiplier = as_i32_ptr(param_blob, kResidualMultiplierOffset);
    const std::int32_t* residual_shift = as_i32_ptr(param_blob, kResidualShiftOffset);
    const std::int32_t* residual_zero_point = as_i32_ptr(param_blob, kResidualZeroPointOffset);
    const std::int8_t* ct2d_weights = param_blob + kCt2dWeightsOffset;
    const std::int32_t* ct2d_bias = as_i32_ptr(param_blob, kCt2dBiasOffset);
    const std::int32_t* ct2d_multiplier = as_i32_ptr(param_blob, kCt2dMultiplierOffset);
    const std::int32_t* skip_multiplier = as_i32_ptr(param_blob, kSkipMultiplierOffset);
    const std::int32_t* skip_shift = as_i32_ptr(param_blob, kSkipShiftOffset);
    const std::int32_t* skip_zero_point = as_i32_ptr(param_blob, kSkipZeroPointOffset);

#if APUSR_AIE_COMPILE
    using MmulS8S8 = aie::mmul<kMmulM, kMmulK, kMmulN, int8, int8>;
    using MmulU8S8 = aie::mmul<kMmulM, kMmulK, kMmulN, uint8, int8>;

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);
#endif

    for (int group = 0; group < kLowGroups; ++group) {
        const int low_y = group / (kLowTileWidth / kPixelsPerGroup);
        const int low_x_base = (group % (kLowTileWidth / kPixelsPerGroup)) * kPixelsPerGroup;

        alignas(32) std::int8_t spatial_local[kSpatialLocalElements]{};
        alignas(32) std::uint8_t expand_local[kExpandLocalElements]{};
        alignas(32) std::int8_t fnb_local[kFnbLocalElements]{};
        alignas(32) std::int8_t activation_block[kActivationElements]{};
        alignas(32) std::int32_t bias_block[kOutputElements]{};
        alignas(32) std::int32_t multiplier_block[kOutputElements]{};
        alignas(32) std::int32_t residual_block[kOutputElements]{};
        alignas(32) std::int8_t result_i8_block[kOutputElements]{};

        for (int spatial_chunk = 0; spatial_chunk < kSpatialOutputChunks; ++spatial_chunk) {
#if APUSR_AIE_COMPILE
            MmulS8S8 accumulator = aie::zeros<acc32, kOutputElements>();
#else
            std::int32_t scalar_accumulator[kOutputElements]{};
#endif
            for (int tap = 0; tap < 9; ++tap) {
                const int tap_y = tap / 3;
                const int tap_x = tap % 3;
                for (int channel_group = 0; channel_group < kSpatialChannelGroups; ++channel_group) {
                    gather_spatial_activation_block(
                        input_chunk0_18x10_c8,
                        input_chunk1_18x10_c8,
                        input_chunk2_18x10_c8,
                        input_chunk3_18x10_c8,
                        low_y,
                        low_x_base,
                        tap_y,
                        tap_x,
                        channel_group,
                        activation_block);
                    const std::size_t weight_offset =
                        ((static_cast<std::size_t>(spatial_chunk) * 9 + tap) * kSpatialChannelGroups + channel_group) *
                        kWeightElements;
#if APUSR_AIE_COMPILE
                    const aie::vector<int8, kActivationElements> activation_vector = aie::load_v<kActivationElements>(activation_block);
                    const aie::vector<int8, kWeightElements> weight_vector =
                        aie::load_v<kWeightElements>(spatial_weights + weight_offset);
                    accumulator.mac(activation_vector, weight_vector);
#else
                    const std::int8_t* weight_block = spatial_weights + weight_offset;
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
            }

            const int channel_base = spatial_chunk * kMmulN;
            fill_repeated_i32_tile(spatial_bias, channel_base, bias_block);
            fill_repeated_i32_tile(spatial_multiplier, channel_base, multiplier_block);
#if APUSR_AIE_COMPILE
            const auto dot_accumulator = accumulator.to_accum();
            const aie::vector<int32, kOutputElements> bias_vector = aie::load_v<kOutputElements>(bias_block);
            const aie::vector<int32, kOutputElements> multiplier_vector = aie::load_v<kOutputElements>(multiplier_block);
            const auto biased_accumulator = aie::add(dot_accumulator, bias_vector);
            const aie::vector<int32, kOutputElements> biased_vector = aie::to_vector<int32>(biased_accumulator, 0);
            const auto scaled_accumulator = aie::mul<acc64>(biased_vector, multiplier_vector);
            const aie::vector<int8, kOutputElements> result_vector = aie::to_vector<int8>(scaled_accumulator, spatial_right_shift);
            aie::store_v(spatial_local + spatial_chunk * kOutputElements, result_vector);
#else
            for (int lane = 0; lane < kOutputElements; ++lane) {
                const std::int64_t scaled = static_cast<std::int64_t>(scalar_accumulator[lane] + bias_block[lane]) * multiplier_block[lane];
                spatial_local[spatial_chunk * kOutputElements + lane] = saturate_i8_scalar(signed_srs_scalar(scaled, spatial_right_shift));
            }
#endif
        }

        gather_center_i8_block(
            input_chunk0_18x10_c8,
            input_chunk1_18x10_c8,
            input_chunk2_18x10_c8,
            input_chunk3_18x10_c8,
            low_y,
            low_x_base,
            16,
            spatial_local + 2 * kOutputElements);
        gather_center_i8_block(
            input_chunk0_18x10_c8,
            input_chunk1_18x10_c8,
            input_chunk2_18x10_c8,
            input_chunk3_18x10_c8,
            low_y,
            low_x_base,
            24,
            spatial_local + 3 * kOutputElements);

        for (int hidden_chunk = 0; hidden_chunk < kExpandOutputChunks; ++hidden_chunk) {
#if APUSR_AIE_COMPILE
            MmulS8S8 accumulator = aie::zeros<acc32, kOutputElements>();
#else
            std::int32_t scalar_accumulator[kOutputElements]{};
#endif
            for (int channel_group = 0; channel_group < kInputChannelGroups; ++channel_group) {
                const std::int8_t* activation_group = spatial_local + channel_group * kOutputElements;
                const std::size_t weight_offset =
                    (static_cast<std::size_t>(hidden_chunk) * kInputChannelGroups + channel_group) * kWeightElements;
#if APUSR_AIE_COMPILE
                const aie::vector<int8, kActivationElements> activation_vector = aie::load_v<kActivationElements>(activation_group);
                const aie::vector<int8, kWeightElements> weight_vector = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(activation_vector, weight_vector);
#else
                const std::int8_t* weight_block = expand_weights + weight_offset;
                for (int pixel = 0; pixel < kMmulM; ++pixel) {
                    for (int oc = 0; oc < kMmulN; ++oc) {
                        for (int ic = 0; ic < kMmulK; ++ic) {
                            scalar_accumulator[pixel * kMmulN + oc] +=
                                static_cast<std::int32_t>(activation_group[pixel * kMmulK + ic]) * weight_block[ic * kMmulN + oc];
                        }
                    }
                }
#endif
            }

            const int channel_base = hidden_chunk * kMmulN;
            fill_repeated_i32_tile(expand_bias, channel_base, bias_block);
            fill_repeated_i32_tile(expand_multiplier, channel_base, multiplier_block);
#if APUSR_AIE_COMPILE
            const auto dot_accumulator = accumulator.to_accum();
            const aie::vector<int32, kOutputElements> bias_vector = aie::load_v<kOutputElements>(bias_block);
            const aie::vector<int32, kOutputElements> multiplier_vector = aie::load_v<kOutputElements>(multiplier_block);
            const auto biased_accumulator = aie::add(dot_accumulator, bias_vector);
            const aie::vector<int32, kOutputElements> biased_vector = aie::to_vector<int32>(biased_accumulator, 0);
            const auto scaled_accumulator = aie::mul<acc64>(biased_vector, multiplier_vector);
            const aie::vector<uint8, kOutputElements> result_vector = aie::to_vector<uint8>(scaled_accumulator, expand_right_shift);
            aie::store_v(expand_local + hidden_chunk * kOutputElements, result_vector);
#else
            for (int lane = 0; lane < kOutputElements; ++lane) {
                const std::int64_t scaled = static_cast<std::int64_t>(scalar_accumulator[lane] + bias_block[lane]) * multiplier_block[lane];
                expand_local[hidden_chunk * kOutputElements + lane] = saturate_u8_scalar(signed_srs_scalar(scaled, expand_right_shift));
            }
#endif
        }

        for (int contract_chunk = 0; contract_chunk < kContractOutputChunks; ++contract_chunk) {
#if APUSR_AIE_COMPILE
            MmulU8S8 accumulator = aie::zeros<acc32, kOutputElements>();
#else
            std::int32_t scalar_accumulator[kOutputElements]{};
#endif
            for (int channel_group = 0; channel_group < kHiddenGroups; ++channel_group) {
                const std::uint8_t* activation_group = expand_local + channel_group * kOutputElements;
                const std::size_t weight_offset =
                    (static_cast<std::size_t>(contract_chunk) * kHiddenGroups + channel_group) * kWeightElements;
#if APUSR_AIE_COMPILE
                const aie::vector<uint8, kActivationElements> activation_vector = aie::load_v<kActivationElements>(activation_group);
                const aie::vector<int8, kWeightElements> weight_vector = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(activation_vector, weight_vector);
#else
                const std::int8_t* weight_block = contract_weights + weight_offset;
                for (int pixel = 0; pixel < kMmulM; ++pixel) {
                    for (int oc = 0; oc < kMmulN; ++oc) {
                        for (int ic = 0; ic < kMmulK; ++ic) {
                            scalar_accumulator[pixel * kMmulN + oc] +=
                                static_cast<std::int32_t>(activation_group[pixel * kMmulK + ic]) * weight_block[ic * kMmulN + oc];
                        }
                    }
                }
#endif
            }

            const int channel_base = contract_chunk * kMmulN;
            fill_repeated_i32_tile(contract_bias, channel_base, bias_block);
            fill_repeated_i32_tile(contract_multiplier, channel_base, multiplier_block);
            gather_center_residual_i32_tile(
                input_chunk0_18x10_c8,
                input_chunk1_18x10_c8,
                input_chunk2_18x10_c8,
                input_chunk3_18x10_c8,
                residual_multiplier,
                residual_shift,
                residual_zero_point,
                low_y,
                low_x_base,
                channel_base,
                residual_block);
#if APUSR_AIE_COMPILE
            const auto dot_accumulator = accumulator.to_accum();
            const aie::vector<int32, kOutputElements> bias_vector = aie::load_v<kOutputElements>(bias_block);
            const aie::vector<int32, kOutputElements> residual_vector = aie::load_v<kOutputElements>(residual_block);
            const aie::vector<int32, kOutputElements> multiplier_vector = aie::load_v<kOutputElements>(multiplier_block);
            auto biased_accumulator = aie::add(dot_accumulator, bias_vector);
            biased_accumulator = aie::add(biased_accumulator, residual_vector);
            const aie::vector<int32, kOutputElements> biased_vector = aie::to_vector<int32>(biased_accumulator, 0);
            const auto scaled_accumulator = aie::mul<acc64>(biased_vector, multiplier_vector);
            const aie::vector<int8, kOutputElements> result_vector = aie::to_vector<int8>(scaled_accumulator, contract_right_shift);
            aie::store_v(fnb_local + contract_chunk * kOutputElements, result_vector);
#else
            for (int lane = 0; lane < kOutputElements; ++lane) {
                const std::int64_t scaled =
                    static_cast<std::int64_t>(scalar_accumulator[lane] + bias_block[lane] + residual_block[lane]) * multiplier_block[lane];
                fnb_local[contract_chunk * kOutputElements + lane] = saturate_i8_scalar(signed_srs_scalar(scaled, contract_right_shift));
            }
#endif
        }

        for (int phase = 0; phase < kCt2dPhases; ++phase) {
            for (int output_chunk = 0; output_chunk < kCt2dOutputChunks; ++output_chunk) {
#if APUSR_AIE_COMPILE
                MmulS8S8 accumulator = aie::zeros<acc32, kOutputElements>();
#else
                std::int32_t scalar_accumulator[kOutputElements]{};
#endif
                for (int channel_group = 0; channel_group < kInputChannelGroups; ++channel_group) {
                    const std::int8_t* activation_group = fnb_local + channel_group * kOutputElements;
                    const std::size_t weight_offset =
                        (((static_cast<std::size_t>(phase) * kCt2dOutputChunks + output_chunk) * kInputChannelGroups) + channel_group) *
                        kWeightElements;
#if APUSR_AIE_COMPILE
                    const aie::vector<int8, kActivationElements> activation_vector = aie::load_v<kActivationElements>(activation_group);
                    const aie::vector<int8, kWeightElements> weight_vector = aie::load_v<kWeightElements>(ct2d_weights + weight_offset);
                    accumulator.mac(activation_vector, weight_vector);
#else
                    const std::int8_t* weight_block = ct2d_weights + weight_offset;
                    for (int pixel = 0; pixel < kMmulM; ++pixel) {
                        for (int oc = 0; oc < kMmulN; ++oc) {
                            for (int ic = 0; ic < kMmulK; ++ic) {
                                scalar_accumulator[pixel * kMmulN + oc] +=
                                    static_cast<std::int32_t>(activation_group[pixel * kMmulK + ic]) * weight_block[ic * kMmulN + oc];
                            }
                        }
                    }
#endif
                }

                const int channel_base = output_chunk * kMmulN;
                fill_repeated_i32_tile(ct2d_bias, channel_base, bias_block);
                fill_repeated_i32_tile(ct2d_multiplier, channel_base, multiplier_block);
                gather_skip_i32_tile(
                    skip_tile_32x16_c16,
                    skip_multiplier,
                    skip_shift,
                    skip_zero_point,
                    low_y,
                    low_x_base,
                    phase,
                    channel_base,
                    residual_block);
#if APUSR_AIE_COMPILE
                const auto dot_accumulator = accumulator.to_accum();
                const aie::vector<int32, kOutputElements> bias_vector = aie::load_v<kOutputElements>(bias_block);
                const aie::vector<int32, kOutputElements> skip_vector = aie::load_v<kOutputElements>(residual_block);
                const aie::vector<int32, kOutputElements> multiplier_vector = aie::load_v<kOutputElements>(multiplier_block);
                auto biased_accumulator = aie::add(dot_accumulator, bias_vector);
                biased_accumulator = aie::add(biased_accumulator, skip_vector);
                const aie::vector<int32, kOutputElements> biased_vector = aie::to_vector<int32>(biased_accumulator, 0);
                const auto scaled_accumulator = aie::mul<acc64>(biased_vector, multiplier_vector);
                const aie::vector<int8, kOutputElements> result_vector = aie::to_vector<int8>(scaled_accumulator, ct2d_right_shift);
                aie::store_v(result_i8_block, result_vector);
#else
                for (int lane = 0; lane < kOutputElements; ++lane) {
                    const std::int64_t scaled =
                        static_cast<std::int64_t>(scalar_accumulator[lane] + bias_block[lane] + residual_block[lane]) * multiplier_block[lane];
                    result_i8_block[lane] = saturate_i8_scalar(signed_srs_scalar(scaled, ct2d_right_shift));
                }
#endif
                scatter_ct2d_c8_block(output_tile_32x16_c16, low_y, low_x_base, phase, channel_base, result_i8_block);
            }
        }
    }
}

} // namespace apusr::aie_middle_core
