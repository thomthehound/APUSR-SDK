// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
//
// APUSR DP4A-optimized Stage1 shader, dispatch-preserving tiled pass1 candidate.
//
// This shader keeps the imported/generated pass1 runtime dispatch contract:
//
//   [numthreads(64, 2, 1)]
//   groupId.x selects a 64-pixel horizontal strip
//   groupId.y selects two model-space rows
//
// The previous 16x8 prototype version used a nicer conceptual tile, but it
// was wrong for the existing runtime because the dispatch planner still issued
// the original generated pass1 grid.  This version is intentionally less
// ambitious: it preserves the pass1 ABI exactly and only changes the local data
// movement.  Each group stages a 66x4xC16 halo strip in groupshared memory and
// emits the same 64x2xC16 output tile as the reference ConvNextBlock.  The
// current product shader uses vectorized C16-pixel LDS preload plus an interior
// no-bounds-check path, then streams the C16->C32->C16 pointwise MLP
// through four-hidden-channel blocks to shorten temporary live ranges.  The
// math path and stage boundary remain unchanged.
//
// The optimized product front-C16 path now promotes Stage0 to a padded C16
// carrier.  Pass0 writes active Stage0 at logical origin (1,1), clears the
// halo/dispatch-tail cells, and shifts Stage1 output to paddedStage0Bytes.
// Pass1 can therefore preload its 66x4 input strip branchlessly.

#define APUSR_FSR4_I8_PASS1_TILED_EXACT_AVAILABLE 1
#define APUSR_FSR4_I8_TILED_FUSION_STAGE0_SEMANTIC_CHANGE_REQUIRED 1
#define APUSR_FSR4_I8_TILED_FUSION_EXACT_STAGE_BOUNDARY 1
#define APUSR_FSR4_I8_TILED_FUSION_USES_GROUPSHARED_C16_HALO 1
#define APUSR_FSR4_I8_TILED_FUSION_PRESERVES_REFERENCE_DISPATCH 1

#if !defined(APUSR_FSR4_I8_PRESET_NATIVE) && \
    !defined(APUSR_FSR4_I8_PRESET_QUALITY) && \
    !defined(APUSR_FSR4_I8_PRESET_BALANCED) && \
    !defined(APUSR_FSR4_I8_PRESET_PERFORMANCE) && \
    !defined(APUSR_FSR4_I8_PRESET_DRS) && \
    !defined(APUSR_FSR4_I8_PRESET_ULTRAPERF)
#error "Define exactly one APUSR_FSR4_I8_PRESET_* macro."
#endif

#if !defined(APUSR_FSR4_I8_CAPACITY_1080) && \
    !defined(APUSR_FSR4_I8_CAPACITY_2160) && \
    !defined(APUSR_FSR4_I8_CAPACITY_4320)
#error "Define exactly one APUSR_FSR4_I8_CAPACITY_* macro."
#endif

// Pull in the generated pass1 resources/constants and ML2Code helper types.
// This also emits the original generated pass1 entry point into the translation
// unit.  The DP4A-optimized pipeline compiles/dispatches the entry below instead.
#define MLSR_PASS_1 1
#if defined(APUSR_FSR4_I8_PRESET_NATIVE) && defined(APUSR_FSR4_I8_CAPACITY_1080)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/native/passes_1080.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_NATIVE) && defined(APUSR_FSR4_I8_CAPACITY_2160)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/native/passes_2160.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_NATIVE) && defined(APUSR_FSR4_I8_CAPACITY_4320)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/native/passes_4320.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_QUALITY) && defined(APUSR_FSR4_I8_CAPACITY_1080)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/quality/passes_1080.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_QUALITY) && defined(APUSR_FSR4_I8_CAPACITY_2160)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/quality/passes_2160.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_QUALITY) && defined(APUSR_FSR4_I8_CAPACITY_4320)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/quality/passes_4320.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_BALANCED) && defined(APUSR_FSR4_I8_CAPACITY_1080)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/passes_1080.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_BALANCED) && defined(APUSR_FSR4_I8_CAPACITY_2160)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/passes_2160.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_BALANCED) && defined(APUSR_FSR4_I8_CAPACITY_4320)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/passes_4320.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_PERFORMANCE) && defined(APUSR_FSR4_I8_CAPACITY_1080)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/performance/passes_1080.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_PERFORMANCE) && defined(APUSR_FSR4_I8_CAPACITY_2160)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/performance/passes_2160.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_PERFORMANCE) && defined(APUSR_FSR4_I8_CAPACITY_4320)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/performance/passes_4320.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_DRS) && defined(APUSR_FSR4_I8_CAPACITY_1080)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/drs/passes_1080.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_DRS) && defined(APUSR_FSR4_I8_CAPACITY_2160)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/drs/passes_2160.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_DRS) && defined(APUSR_FSR4_I8_CAPACITY_4320)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/drs/passes_4320.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_ULTRAPERF) && defined(APUSR_FSR4_I8_CAPACITY_1080)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/ultraperf/passes_1080.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_ULTRAPERF) && defined(APUSR_FSR4_I8_CAPACITY_2160)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/ultraperf/passes_2160.hlsl"
#elif defined(APUSR_FSR4_I8_PRESET_ULTRAPERF) && defined(APUSR_FSR4_I8_CAPACITY_4320)
#include "../../../models/fsr4_int8/reference/fsr4_4_0_2_int8/ultraperf/passes_4320.hlsl"
#else
#error "Unsupported APUSR FSR4 INT8 preset/capacity combination."
#endif
#undef MLSR_PASS_1

#if defined(APUSR_FSR4_I8_CAPACITY_1080)
static const uint APUSR_FSR4_PASS1_WIDTH = 960;
static const uint APUSR_FSR4_PASS1_HEIGHT = 540;
#elif defined(APUSR_FSR4_I8_CAPACITY_2160)
static const uint APUSR_FSR4_PASS1_WIDTH = 1920;
static const uint APUSR_FSR4_PASS1_HEIGHT = 1080;
#elif defined(APUSR_FSR4_I8_CAPACITY_4320)
static const uint APUSR_FSR4_PASS1_WIDTH = 3840;
static const uint APUSR_FSR4_PASS1_HEIGHT = 2160;
#endif

#if defined(APUSR_FSR4_I8_PRESET_NATIVE)
static const float APUSR_FSR4_PASS1_INPUT_SCALE = 0.014608594588935375;
static const float APUSR_FSR4_PASS1_OUTPUT_SCALE = 0.027550771832466125;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0 = 30.251555794070015;
static const float APUSR_FSR4_PASS1_INPUT_SCALE1 = 0.03305615112185478;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1 = 31.02819042735687;
static const float APUSR_FSR4_PASS1_INPUT_SCALE2 = 0.032228756695985794;
static const float APUSR_FSR4_PASS1_WEIGHT0_SCALE = 0.006728010717779398;
static const float APUSR_FSR4_PASS1_WEIGHT1_SCALE = 0.006839262321591377;
static const float APUSR_FSR4_PASS1_WEIGHT2_SCALE = 0.0068810670636594296;
#elif defined(APUSR_FSR4_I8_PRESET_QUALITY)
static const float APUSR_FSR4_PASS1_INPUT_SCALE = 0.014884727075695992;
static const float APUSR_FSR4_PASS1_OUTPUT_SCALE = 0.01933070458471775;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0 = 19.41786337677181;
static const float APUSR_FSR4_PASS1_INPUT_SCALE1 = 0.05149897187948227;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1 = 39.03760807273706;
static const float APUSR_FSR4_PASS1_INPUT_SCALE2 = 0.025616323575377464;
static const float APUSR_FSR4_PASS1_WEIGHT0_SCALE = 0.005331969354301691;
static const float APUSR_FSR4_PASS1_WEIGHT1_SCALE = 0.005319580901414156;
static const float APUSR_FSR4_PASS1_WEIGHT2_SCALE = 0.005279477220028639;
#elif defined(APUSR_FSR4_I8_PRESET_BALANCED)
static const float APUSR_FSR4_PASS1_INPUT_SCALE = 0.012475206516683102;
static const float APUSR_FSR4_PASS1_OUTPUT_SCALE = 0.024305112659931183;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0 = 29.098199195959484;
static const float APUSR_FSR4_PASS1_INPUT_SCALE1 = 0.034366387873888016;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1 = 46.476467167734775;
static const float APUSR_FSR4_PASS1_INPUT_SCALE2 = 0.021516265347599983;
static const float APUSR_FSR4_PASS1_WEIGHT0_SCALE = 0.005744654219597578;
static const float APUSR_FSR4_PASS1_WEIGHT1_SCALE = 0.005624094046652317;
static const float APUSR_FSR4_PASS1_WEIGHT2_SCALE = 0.005617608781903982;
#elif defined(APUSR_FSR4_I8_PRESET_PERFORMANCE)
static const float APUSR_FSR4_PASS1_INPUT_SCALE = 0.014811458997428417;
static const float APUSR_FSR4_PASS1_OUTPUT_SCALE = 0.01813533715903759;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0 = 39.691229419086014;
static const float APUSR_FSR4_PASS1_INPUT_SCALE1 = 0.025194482877850533;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1 = 49.80184507790761;
static const float APUSR_FSR4_PASS1_INPUT_SCALE2 = 0.02007957734167576;
static const float APUSR_FSR4_PASS1_WEIGHT0_SCALE = 0.0055527822114527225;
static const float APUSR_FSR4_PASS1_WEIGHT1_SCALE = 0.00551386596634984;
static const float APUSR_FSR4_PASS1_WEIGHT2_SCALE = 0.00546378456056118;
#elif defined(APUSR_FSR4_I8_PRESET_DRS)
static const float APUSR_FSR4_PASS1_INPUT_SCALE = 0.013469061814248562;
static const float APUSR_FSR4_PASS1_OUTPUT_SCALE = 0.021159790456295013;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0 = 25.53226708664425;
static const float APUSR_FSR4_PASS1_INPUT_SCALE1 = 0.039166126400232315;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1 = 42.37851232232464;
static const float APUSR_FSR4_PASS1_INPUT_SCALE2 = 0.023596864193677902;
static const float APUSR_FSR4_PASS1_WEIGHT0_SCALE = 0.005910119973123074;
static const float APUSR_FSR4_PASS1_WEIGHT1_SCALE = 0.005874319467693567;
static const float APUSR_FSR4_PASS1_WEIGHT2_SCALE = 0.005870396271348;
#elif defined(APUSR_FSR4_I8_PRESET_ULTRAPERF)
static const float APUSR_FSR4_PASS1_INPUT_SCALE = 0.011951033025979996;
static const float APUSR_FSR4_PASS1_OUTPUT_SCALE = 0.016850261017680168;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0 = 41.959295949110285;
static const float APUSR_FSR4_PASS1_INPUT_SCALE1 = 0.023832621052861214;
static const float APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1 = 53.0461190299257;
static const float APUSR_FSR4_PASS1_INPUT_SCALE2 = 0.018851520493626595;
static const float APUSR_FSR4_PASS1_WEIGHT0_SCALE = 0.005296443123370409;
static const float APUSR_FSR4_PASS1_WEIGHT1_SCALE = 0.00530614610761404;
static const float APUSR_FSR4_PASS1_WEIGHT2_SCALE = 0.005339715164154768;
#endif

static const uint APUSR_FSR4_C16 = 16;
static const uint APUSR_FSR4_C16_DWORDS = 4;
static const uint APUSR_FSR4_PASS1_TILE_W = 64;
static const uint APUSR_FSR4_PASS1_TILE_H = 2;
static const uint APUSR_FSR4_PASS1_HALO = 1;
static const uint APUSR_FSR4_PASS1_LOAD_W = APUSR_FSR4_PASS1_TILE_W + 2 * APUSR_FSR4_PASS1_HALO;
static const uint APUSR_FSR4_PASS1_LOAD_H = APUSR_FSR4_PASS1_TILE_H + 2 * APUSR_FSR4_PASS1_HALO;
static const uint APUSR_FSR4_PASS1_LOAD_PIXELS = APUSR_FSR4_PASS1_LOAD_W * APUSR_FSR4_PASS1_LOAD_H;
static const uint APUSR_FSR4_PASS1_INPUT_OFFSET_BYTES = 0;
static const uint APUSR_FSR4_PASS1_PADDED_STAGE0_WIDTH = APUSR_FSR4_PASS1_WIDTH + 2;
static const uint APUSR_FSR4_PASS1_PADDED_STAGE0_HEIGHT = APUSR_FSR4_PASS1_HEIGHT + 2;
static const uint APUSR_FSR4_PASS1_PADDED_STAGE0_BYTES = APUSR_FSR4_PASS1_PADDED_STAGE0_WIDTH * APUSR_FSR4_PASS1_PADDED_STAGE0_HEIGHT * APUSR_FSR4_C16;
static const uint APUSR_FSR4_PASS1_OUTPUT_OFFSET_BYTES = APUSR_FSR4_PASS1_PADDED_STAGE0_BYTES;
static const uint APUSR_FSR4_PASS1_INPUT_ROW_BYTES = APUSR_FSR4_PASS1_PADDED_STAGE0_WIDTH * APUSR_FSR4_C16;
static const uint APUSR_FSR4_PASS1_OUTPUT_ROW_BYTES = APUSR_FSR4_PASS1_WIDTH * APUSR_FSR4_C16;

// 66x4 pixels, each pixel is C16 = four packed int8 dwords.  This is close to
// the old 16x8 prototype's LDS footprint while preserving the generated pass1
// dispatch and covering exactly the same pixels as reference.  The preload loop
// uses one 16-byte Load4 per C16 pixel instead of four scalar dword loads.
groupshared uint apusr_pass1_c16_halo_tile[APUSR_FSR4_PASS1_LOAD_W * APUSR_FSR4_PASS1_LOAD_H * APUSR_FSR4_C16_DWORDS];


cbuffer MLSR_Optimized_Constants : register(b0)
{
    float2 apusr_cb_inv_size;
    float2 apusr_cb_scale;
    float2 apusr_cb_inv_scale;
    float2 apusr_cb_jitter;
    float2 apusr_cb_mv_scale;
    float2 apusr_cb_tex_size;
    float2 apusr_cb_max_render_size;
    float2 apusr_cb_motion_vector_jitter_cancellation;

    uint width;
    uint height;
    uint apusr_cb_reset;
    uint apusr_cb_width_lr;

    uint apusr_cb_height_lr;
    float apusr_cb_pre_exposure;
    float apusr_cb_previous_pre_exposure;
    uint apusr_cb_rcas_enabled;

    float apusr_cb_rcas_sharpness;
    uint apusr_cb_use_default_exposure;
};

uint apusrPass1TileOffset(uint x, uint y, uint c4)
{
    return ((y * APUSR_FSR4_PASS1_LOAD_W + x) * APUSR_FSR4_C16_DWORDS) + c4;
}

uint apusrPass1GlobalC16Offset(uint x, uint y, uint c4)
{
    return APUSR_FSR4_PASS1_INPUT_OFFSET_BYTES + y * APUSR_FSR4_PASS1_INPUT_ROW_BYTES + x * APUSR_FSR4_C16 + c4 * 4;
}

uint apusrPass1OutputC16Offset(uint x, uint y)
{
    return APUSR_FSR4_PASS1_OUTPUT_OFFSET_BYTES + y * APUSR_FSR4_PASS1_OUTPUT_ROW_BYTES + x * APUSR_FSR4_C16;
}

uint apusrPass1LoadSharedC16(uint localX, uint localY, uint c4)
{
    return apusr_pass1_c16_halo_tile[apusrPass1TileOffset(localX, localY, c4)];
}

[numthreads(APUSR_FSR4_PASS1_TILE_W, APUSR_FSR4_PASS1_TILE_H, 1)]
void apusr_fsr4_i8_pass1_tiled_exact(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    const uint2 tileOrigin = groupId.xy * uint2(APUSR_FSR4_PASS1_TILE_W, APUSR_FSR4_PASS1_TILE_H);
    const uint flatThread = groupThreadId.y * APUSR_FSR4_PASS1_TILE_W + groupThreadId.x;
    const uint threadCount = APUSR_FSR4_PASS1_TILE_W * APUSR_FSR4_PASS1_TILE_H;
    const uint activeWidth = width >> 1u;
    const uint activeHeight = height >> 1u;

    // Padded Stage0 contract: active C16 data is stored at physical origin
    // (1,1), and pass0 clears the top/bottom/left/right/tail cells touched by
    // this preload.  The common path and every active edge path therefore use
    // identical branchless Load4s.  Invalid output lanes still participate in
    // the preload/barrier, then return below.
    [unroll]
    for (uint ly = 0; ly < APUSR_FSR4_PASS1_LOAD_H; ++ly) {
        for (uint lx = flatThread; lx < APUSR_FSR4_PASS1_LOAD_W; lx += threadCount) {
            const uint gx = tileOrigin.x + lx;
            const uint gy = tileOrigin.y + ly;
            const uint4 packed = ScratchBuffer.Load4(apusrPass1GlobalC16Offset(gx, gy, 0));
            const uint sharedBase = (ly * APUSR_FSR4_PASS1_LOAD_W + lx) * APUSR_FSR4_C16_DWORDS;
            apusr_pass1_c16_halo_tile[sharedBase + 0] = packed.x;
            apusr_pass1_c16_halo_tile[sharedBase + 1] = packed.y;
            apusr_pass1_c16_halo_tile[sharedBase + 2] = packed.z;
            apusr_pass1_c16_halo_tile[sharedBase + 3] = packed.w;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint ox = dispatchThreadId.x;
    const uint oy = dispatchThreadId.y;
    if (ox >= activeWidth || oy >= activeHeight) {
        return;
    }

    const ConstantBufferStorage<576> storageW0 = { embedded__encoder2_ResidualBlock_0_body_conv_dw_weight_quant_export_handler_QuantizeLinear_output_0_dwords };
    const QuantizedTensor4i8_NHWC<ConstantBufferStorage<576> > weights0 = {
        uint4(3, 3, 16, 16),
        uint4(0, 0, 0, 0),
        uint4(3, 3, 16, 16),
        uint4(3, 3, 16, 16),
        uint4(16, 48, 1, 144),
        uint4(0, 0, 0, 0),
        uint4(0, 0, 0, 0),
        0,
        APUSR_FSR4_PASS1_WEIGHT0_SCALE,
        storageW0 };

    const ConstantBufferStorage<8> storageB0 = { embedded_encoder2_ResidualBlock_0_body_conv_dw_bias_dwords };
    const Tensor1h<ConstantBufferStorage<8> > bias0 = {
        16,
        0,
        16,
        16,
        2,
        0,
        0,
        0,
        storageB0 };

    const ConstantBufferStorage<128> storageW1 = { embedded__encoder2_ResidualBlock_0_body_conv_pw_expand_weight_quant_export_handler_QuantizeLinear_output_0_dwords };
    const QuantizedTensor4i8_NHWC<ConstantBufferStorage<128> > weights1 = {
        uint4(1, 1, 16, 32),
        uint4(0, 0, 0, 0),
        uint4(1, 1, 16, 32),
        uint4(1, 1, 16, 32),
        uint4(16, 16, 1, 16),
        uint4(0, 0, 0, 0),
        uint4(0, 0, 0, 0),
        0,
        APUSR_FSR4_PASS1_WEIGHT1_SCALE,
        storageW1 };

    const ConstantBufferStorage<16> storageB1 = { embedded_encoder2_ResidualBlock_0_body_conv_pw_expand_bias_dwords };
    const Tensor1h<ConstantBufferStorage<16> > bias1 = {
        32,
        0,
        32,
        32,
        2,
        0,
        0,
        0,
        storageB1 };

    const ConstantBufferStorage<128> storageW2 = { embedded__encoder2_ResidualBlock_0_body_conv_pw_contract_weight_quant_export_handler_QuantizeLinear_output_0_dwords };
    const QuantizedTensor4i8_NHWC<ConstantBufferStorage<128> > weights2 = {
        uint4(1, 1, 32, 16),
        uint4(0, 0, 0, 0),
        uint4(1, 1, 32, 16),
        uint4(1, 1, 32, 16),
        uint4(32, 32, 1, 32),
        uint4(0, 0, 0, 0),
        uint4(0, 0, 0, 0),
        0,
        APUSR_FSR4_PASS1_WEIGHT2_SCALE,
        storageW2 };

    const ConstantBufferStorage<8> storageB2 = { embedded_encoder2_ResidualBlock_0_body_conv_pw_contract_bias_dwords };
    const Tensor1h<ConstantBufferStorage<8> > bias2 = {
        16,
        0,
        16,
        16,
        2,
        0,
        0,
        0,
        storageB2 };

    int accumulator[16];
    [unroll]
    for (uint f0 = 0; f0 < 16; ++f0) {
        accumulator[f0] = 0;
    }

    [unroll]
    for (uint ky = 0; ky < 3; ++ky) {
        [unroll]
        for (uint kx = 0; kx < 3; ++kx) {
            const uint4 inputDwords = uint4(
                apusrPass1LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 0),
                apusrPass1LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 1),
                apusrPass1LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 2),
                apusrPass1LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 3));

            [unroll]
            for (uint f = 0; f < 16; ++f) {
                const uint weightsOffset = weights0.OffsetOf(uint4(kx, ky, 0, f));
                const uint4 weightsDwords = weights0.storage.Load4(weightsOffset);
                accumulator[f] = dot4add_i8packed(inputDwords.x, weightsDwords.x, accumulator[f]);
                accumulator[f] = dot4add_i8packed(inputDwords.y, weightsDwords.y, accumulator[f]);
                accumulator[f] = dot4add_i8packed(inputDwords.z, weightsDwords.z, accumulator[f]);
                accumulator[f] = dot4add_i8packed(inputDwords.w, weightsDwords.w, accumulator[f]);
            }
        }
    }

    int8_t4_packed convResult[4];
    const float inputScale0 = APUSR_FSR4_PASS1_INPUT_SCALE * weights0.quantizationScale;
    [unroll]
    for (uint f4 = 0; f4 < 16; f4 += 4) {
        const float4 acc = float4(accumulator[f4], accumulator[f4 + 1], accumulator[f4 + 2], accumulator[f4 + 3]);
        const int4 quantized = round((acc * inputScale0 + Load4hA(bias0, f4)) * APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE0);
        convResult[f4 / 4] = pack_clamp_s8(quantized);
    }

    int conv3Output[16];
    [unroll]
    for (uint i2 = 0; i2 < 16; ++i2) {
        conv3Output[i2] = 0;
    }

    // Stream the C16->C32->C16 pointwise MLP in 4-hidden-channel blocks.
    // The v2 shader materialized all 32 hidden-channel accumulators and all
    // eight packed ReLU vectors before starting the C32->C16 contract.  That is
    // exact, but it keeps a large temporary live range in the hottest part of
    // the shader.  This keeps the exact quantized ReLU boundary, then consumes
    // each packed four-channel hidden block immediately into the final C16
    // contract accumulators.  Arithmetic and weights are unchanged; only hidden
    // activation lifetime is shortened.
    const float inputScale1 = APUSR_FSR4_PASS1_INPUT_SCALE1 * weights1.quantizationScale;
    [unroll]
    for (uint h4 = 0; h4 < 32; h4 += 4) {
        int hiddenAccum[4];
        [unroll]
        for (uint h = 0; h < 4; ++h) {
            const uint hiddenChannel = h4 + h;
            const uint weightsOffset1 = weights1.OffsetOf(uint4(0, 0, 0, hiddenChannel));
            const uint4 weightsDwords1 = weights1.storage.Load4(weightsOffset1);
            int acc1 = 0;
            acc1 = dot4add_i8packed(convResult[0], weightsDwords1.x, acc1);
            acc1 = dot4add_i8packed(convResult[1], weightsDwords1.y, acc1);
            acc1 = dot4add_i8packed(convResult[2], weightsDwords1.z, acc1);
            acc1 = dot4add_i8packed(convResult[3], weightsDwords1.w, acc1);
            hiddenAccum[h] = acc1;
        }

        const float4 hiddenFloat = float4(hiddenAccum[0], hiddenAccum[1], hiddenAccum[2], hiddenAccum[3]);
        const int4 quantizedHidden = round((hiddenFloat * inputScale1 + Load4hA(bias1, h4)) * APUSR_FSR4_PASS1_RCPR_OUTPUT_SCALE1);
        #if FFX_FSR4_EXPERIMENT_UNSIGNED_RELU
        const int8_t4_packed hiddenPacked = PackUnsignedReluActivationU8(quantizedHidden);
        #else
        const int8_t4_packed hiddenPacked = pack_s8(ReluClampS8(quantizedHidden));
        #endif

        [unroll]
        for (uint o = 0; o < 16; ++o) {
            const uint weightsOffset2 = weights2.OffsetOf(uint4(0, 0, h4, o));
            const uint weightsDword2 = weights2.storage.Load1(weightsOffset2);
            conv3Output[o] = dot4add_i8packed(hiddenPacked, weightsDword2, conv3Output[o]);
        }
    }

    const uint4 residualInput = uint4(
        apusrPass1LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 0),
        apusrPass1LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 1),
        apusrPass1LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 2),
        apusrPass1LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 3));

    uint4 storeDwords;
    const float inputScale2 = weights2.quantizationScale * APUSR_FSR4_PASS1_INPUT_SCALE2;
    const float outputRcprScale = 1.0 / APUSR_FSR4_PASS1_OUTPUT_SCALE;
    [unroll]
    for (uint s4 = 0; s4 < 16; s4 += 4) {
        const float4 acc = float4(conv3Output[s4], conv3Output[s4 + 1], conv3Output[s4 + 2], conv3Output[s4 + 3]);
        float4 result = acc * inputScale2 + Load4hA(bias2, s4);
        result += unpack_s8s32(residualInput[s4 / 4]) * APUSR_FSR4_PASS1_INPUT_SCALE;
        storeDwords[s4 / 4] = pack_clamp_s8(int4(round(result * outputRcprScale)));
    }

    ScratchBuffer.Store4(apusrPass1OutputC16Offset(ox, oy), storeDwords);
}
