// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
//
// APUSR DP4A-optimized Stage2 shader, dispatch-preserving tiled pass2 candidate.
//
// This shader keeps the imported/generated pass2 runtime dispatch contract:
//
//   [numthreads(64, 2, 1)]
//   groupId.x selects a 64-pixel horizontal strip
//   groupId.y selects two model-space rows
//
// This version is deliberately pass2-native, not fused.  Each group stages a
// 66x4xC16 halo strip in groupshared memory and emits the same 64x2xC16
// output tile as the reference second ConvNextBlock.  This is the fair
// preparation step before any later pass1->2 fusion: pass2 gets its own
// locality-friendly kernel shape first, while the graph and scratch boundary
// stay unchanged.
//
// This pass consumes the existing dense NHWC C16 pass1 scratch tensor and
// writes the existing pass2/stage2 scratch tensor.  It deliberately preserves
// the generated pass2 dispatch and scratch ping-pong offsets; no pre/stage0
// semantic change is involved.

#define APUSR_FSR4_I8_PASS2_TILED_EXACT_AVAILABLE 1
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

// Pull in the generated pass2 resources/constants and ML2Code helper types.
// This also emits the original generated pass2 entry point into the translation
// unit.  The experiment compiles/dispatches the entry below instead.
#define MLSR_PASS_2 1
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
#undef MLSR_PASS_2

#if defined(APUSR_FSR4_I8_CAPACITY_1080)
static const uint APUSR_FSR4_PASS2_WIDTH = 960;
static const uint APUSR_FSR4_PASS2_HEIGHT = 540;
#elif defined(APUSR_FSR4_I8_CAPACITY_2160)
static const uint APUSR_FSR4_PASS2_WIDTH = 1920;
static const uint APUSR_FSR4_PASS2_HEIGHT = 1080;
#elif defined(APUSR_FSR4_I8_CAPACITY_4320)
static const uint APUSR_FSR4_PASS2_WIDTH = 3840;
static const uint APUSR_FSR4_PASS2_HEIGHT = 2160;
#endif

#if defined(APUSR_FSR4_I8_PRESET_NATIVE)
static const float APUSR_FSR4_PASS2_INPUT_SCALE = 0.027550771832466125;
static const float APUSR_FSR4_PASS2_OUTPUT_SCALE = 0.030525196343660355;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0 = 29.826718543190584;
static const float APUSR_FSR4_PASS2_INPUT_SCALE1 = 0.033526986837387085;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1 = 52.48575011421084;
static const float APUSR_FSR4_PASS2_INPUT_SCALE2 = 0.019052790477871895;
static const float APUSR_FSR4_PASS2_WEIGHT0_SCALE = 0.005887479055672884;
static const float APUSR_FSR4_PASS2_WEIGHT1_SCALE = 0.005944373551756144;
static const float APUSR_FSR4_PASS2_WEIGHT2_SCALE = 0.005935767199844122;
#elif defined(APUSR_FSR4_I8_PRESET_QUALITY)
static const float APUSR_FSR4_PASS2_INPUT_SCALE = 0.01933070458471775;
static const float APUSR_FSR4_PASS2_OUTPUT_SCALE = 0.02335178479552269;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0 = 29.379312231835634;
static const float APUSR_FSR4_PASS2_INPUT_SCALE1 = 0.03403755649924278;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1 = 52.675386653772975;
static const float APUSR_FSR4_PASS2_INPUT_SCALE2 = 0.018984198570251465;
static const float APUSR_FSR4_PASS2_WEIGHT0_SCALE = 0.005748785100877285;
static const float APUSR_FSR4_PASS2_WEIGHT1_SCALE = 0.005573894362896681;
static const float APUSR_FSR4_PASS2_WEIGHT2_SCALE = 0.005647121462970972;
#elif defined(APUSR_FSR4_I8_PRESET_BALANCED)
static const float APUSR_FSR4_PASS2_INPUT_SCALE = 0.024305112659931183;
static const float APUSR_FSR4_PASS2_OUTPUT_SCALE = 0.021448098123073578;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0 = 25.335617768115522;
static const float APUSR_FSR4_PASS2_INPUT_SCALE1 = 0.039470124989748;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1 = 46.476326348146195;
static const float APUSR_FSR4_PASS2_INPUT_SCALE2 = 0.021516330540180206;
static const float APUSR_FSR4_PASS2_WEIGHT0_SCALE = 0.0054812952876091;
static const float APUSR_FSR4_PASS2_WEIGHT1_SCALE = 0.005539849866181612;
static const float APUSR_FSR4_PASS2_WEIGHT2_SCALE = 0.005547042936086655;
#elif defined(APUSR_FSR4_I8_PRESET_PERFORMANCE)
static const float APUSR_FSR4_PASS2_INPUT_SCALE = 0.01813533715903759;
static const float APUSR_FSR4_PASS2_OUTPUT_SCALE = 0.02086760848760605;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0 = 28.133633036428556;
static const float APUSR_FSR4_PASS2_INPUT_SCALE1 = 0.03554464504122734;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1 = 43.15351289350375;
static const float APUSR_FSR4_PASS2_INPUT_SCALE2 = 0.02317308448255062;
static const float APUSR_FSR4_PASS2_WEIGHT0_SCALE = 0.007696075364947319;
static const float APUSR_FSR4_PASS2_WEIGHT1_SCALE = 0.007589734625071287;
static const float APUSR_FSR4_PASS2_WEIGHT2_SCALE = 0.00756900105625391;
#elif defined(APUSR_FSR4_I8_PRESET_DRS)
static const float APUSR_FSR4_PASS2_INPUT_SCALE = 0.021159790456295013;
static const float APUSR_FSR4_PASS2_OUTPUT_SCALE = 0.019636496901512146;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0 = 23.354849554254486;
static const float APUSR_FSR4_PASS2_INPUT_SCALE1 = 0.04281765967607498;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1 = 43.57405429885922;
static const float APUSR_FSR4_PASS2_INPUT_SCALE2 = 0.02294943667948246;
static const float APUSR_FSR4_PASS2_WEIGHT0_SCALE = 0.006511245854198933;
static const float APUSR_FSR4_PASS2_WEIGHT1_SCALE = 0.0065826671198010445;
static const float APUSR_FSR4_PASS2_WEIGHT2_SCALE = 0.006587487179785967;
#elif defined(APUSR_FSR4_I8_PRESET_ULTRAPERF)
static const float APUSR_FSR4_PASS2_INPUT_SCALE = 0.016850261017680168;
static const float APUSR_FSR4_PASS2_OUTPUT_SCALE = 0.015938660129904747;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0 = 31.89079297554309;
static const float APUSR_FSR4_PASS2_INPUT_SCALE1 = 0.03135701268911362;
static const float APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1 = 68.7875599112978;
static const float APUSR_FSR4_PASS2_INPUT_SCALE2 = 0.014537512324750423;
static const float APUSR_FSR4_PASS2_WEIGHT0_SCALE = 0.00646417448297143;
static const float APUSR_FSR4_PASS2_WEIGHT1_SCALE = 0.00672558881342411;
static const float APUSR_FSR4_PASS2_WEIGHT2_SCALE = 0.006663109641522169;
#endif

static const uint APUSR_FSR4_C16 = 16;
static const uint APUSR_FSR4_C16_DWORDS = 4;
static const uint APUSR_FSR4_PASS2_TILE_W = 64;
static const uint APUSR_FSR4_PASS2_TILE_H = 2;
static const uint APUSR_FSR4_PASS2_HALO = 1;
static const uint APUSR_FSR4_PASS2_LOAD_W = APUSR_FSR4_PASS2_TILE_W + 2 * APUSR_FSR4_PASS2_HALO;
static const uint APUSR_FSR4_PASS2_LOAD_H = APUSR_FSR4_PASS2_TILE_H + 2 * APUSR_FSR4_PASS2_HALO;
static const uint APUSR_FSR4_PASS2_FRAME_BYTES = APUSR_FSR4_PASS2_WIDTH * APUSR_FSR4_PASS2_HEIGHT * APUSR_FSR4_C16;
static const uint APUSR_FSR4_PASS2_PADDED_STAGE0_WIDTH = APUSR_FSR4_PASS2_WIDTH + 2;
static const uint APUSR_FSR4_PASS2_PADDED_STAGE0_HEIGHT = APUSR_FSR4_PASS2_HEIGHT + 2;
static const uint APUSR_FSR4_PASS2_PADDED_STAGE0_BYTES = APUSR_FSR4_PASS2_PADDED_STAGE0_WIDTH * APUSR_FSR4_PASS2_PADDED_STAGE0_HEIGHT * APUSR_FSR4_C16;
static const uint APUSR_FSR4_PASS2_INPUT_OFFSET_BYTES = APUSR_FSR4_PASS2_PADDED_STAGE0_BYTES;
static const uint APUSR_FSR4_PASS2_OUTPUT_OFFSET_BYTES = 0;
static const uint APUSR_FSR4_PASS2_ROW_BYTES = APUSR_FSR4_PASS2_WIDTH * APUSR_FSR4_C16;

// 66x4 pixels, each pixel is C16 = four packed int8 dwords.  This is close to
// the old 16x8 prototype's LDS footprint while preserving the generated pass2
// dispatch and covering exactly the same pixels as reference.
groupshared uint apusr_pass2_c16_halo_tile[APUSR_FSR4_PASS2_LOAD_W * APUSR_FSR4_PASS2_LOAD_H * APUSR_FSR4_C16_DWORDS];


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

uint apusrPass2TileOffset(uint x, uint y, uint c4)
{
    return ((y * APUSR_FSR4_PASS2_LOAD_W + x) * APUSR_FSR4_C16_DWORDS) + c4;
}

uint apusrPass2GlobalC16Offset(uint x, uint y, uint c4)
{
    return APUSR_FSR4_PASS2_INPUT_OFFSET_BYTES + y * APUSR_FSR4_PASS2_ROW_BYTES + x * APUSR_FSR4_C16 + c4 * 4;
}

uint apusrPass2OutputC16Offset(uint x, uint y)
{
    return APUSR_FSR4_PASS2_OUTPUT_OFFSET_BYTES + y * APUSR_FSR4_PASS2_ROW_BYTES + x * APUSR_FSR4_C16;
}

uint apusrPass2LoadSharedC16(uint localX, uint localY, uint c4)
{
    return apusr_pass2_c16_halo_tile[apusrPass2TileOffset(localX, localY, c4)];
}

[numthreads(APUSR_FSR4_PASS2_TILE_W, APUSR_FSR4_PASS2_TILE_H, 1)]
void apusr_fsr4_i8_pass2_tiled_exact(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    const uint2 tileOrigin = groupId.xy * uint2(APUSR_FSR4_PASS2_TILE_W, APUSR_FSR4_PASS2_TILE_H);
    const int2 haloOrigin = int2(tileOrigin) - int2(APUSR_FSR4_PASS2_HALO, APUSR_FSR4_PASS2_HALO);
    const uint flatThread = groupThreadId.y * APUSR_FSR4_PASS2_TILE_W + groupThreadId.x;
    const uint threadCount = APUSR_FSR4_PASS2_TILE_W * APUSR_FSR4_PASS2_TILE_H;
    const uint activeWidth = width >> 1u;
    const uint activeHeight = height >> 1u;
    const bool interiorLoadTile =
        haloOrigin.x >= 0 &&
        haloOrigin.y >= 0 &&
        haloOrigin.x + int(APUSR_FSR4_PASS2_LOAD_W) <= int(activeWidth) &&
        haloOrigin.y + int(APUSR_FSR4_PASS2_LOAD_H) <= int(activeHeight);

    // Load one full C16 pixel into LDS at a time with a 16-byte Load4.
    // Keep the successful vectorized preload, but make the loop row-major
    // instead of flattening 66x4 through division/modulo.  Interior groups also
    // split onto a true no-bounds-check path, so the common case avoids both
    // per-pixel bounds branches and per-pixel /66/%66 arithmetic.
    if (interiorLoadTile) {
        [unroll]
        for (uint ly = 0; ly < APUSR_FSR4_PASS2_LOAD_H; ++ly) {
            for (uint lx = flatThread; lx < APUSR_FSR4_PASS2_LOAD_W; lx += threadCount) {
                const uint gx = tileOrigin.x + lx - APUSR_FSR4_PASS2_HALO;
                const uint gy = tileOrigin.y + ly - APUSR_FSR4_PASS2_HALO;
                const uint4 packed = ScratchBuffer.Load4(apusrPass2GlobalC16Offset(gx, gy, 0));
                const uint sharedBase = (ly * APUSR_FSR4_PASS2_LOAD_W + lx) * APUSR_FSR4_C16_DWORDS;
                apusr_pass2_c16_halo_tile[sharedBase + 0] = packed.x;
                apusr_pass2_c16_halo_tile[sharedBase + 1] = packed.y;
                apusr_pass2_c16_halo_tile[sharedBase + 2] = packed.z;
                apusr_pass2_c16_halo_tile[sharedBase + 3] = packed.w;
            }
        }
    } else {
        // Active-display edge groups include the normal top/left/bottom edges
        // plus partial final strips from odd, portrait, ultrawide, and surround
        // display modes.  Scratch rows are capacity-strided, but semantic loads
        // outside the active C16 tensor must be zero.  Compute the valid lx range
        // once per row instead of branching on every halo texel.
        [unroll]
        for (uint ly = 0; ly < APUSR_FSR4_PASS2_LOAD_H; ++ly) {
            const int gy = int(tileOrigin.y) + int(ly) - int(APUSR_FSR4_PASS2_HALO);
            if (gy < 0 || gy >= int(activeHeight)) {
                for (uint lx = flatThread; lx < APUSR_FSR4_PASS2_LOAD_W; lx += threadCount) {
                    const uint sharedBase = (ly * APUSR_FSR4_PASS2_LOAD_W + lx) * APUSR_FSR4_C16_DWORDS;
                    apusr_pass2_c16_halo_tile[sharedBase + 0] = 0u;
                    apusr_pass2_c16_halo_tile[sharedBase + 1] = 0u;
                    apusr_pass2_c16_halo_tile[sharedBase + 2] = 0u;
                    apusr_pass2_c16_halo_tile[sharedBase + 3] = 0u;
                }
                continue;
            }

            const int gxBase = int(tileOrigin.x) - int(APUSR_FSR4_PASS2_HALO);
            const uint firstRealLx = uint(max(0, -gxBase));
            const uint endRealLx = uint(min(int(APUSR_FSR4_PASS2_LOAD_W), int(activeWidth) - gxBase));

            for (uint lx = flatThread; lx < firstRealLx; lx += threadCount) {
                const uint sharedBase = (ly * APUSR_FSR4_PASS2_LOAD_W + lx) * APUSR_FSR4_C16_DWORDS;
                apusr_pass2_c16_halo_tile[sharedBase + 0] = 0u;
                apusr_pass2_c16_halo_tile[sharedBase + 1] = 0u;
                apusr_pass2_c16_halo_tile[sharedBase + 2] = 0u;
                apusr_pass2_c16_halo_tile[sharedBase + 3] = 0u;
            }

            for (uint lx = firstRealLx + flatThread; lx < endRealLx; lx += threadCount) {
                const uint gx = tileOrigin.x + lx - APUSR_FSR4_PASS2_HALO;
                const uint4 packed = ScratchBuffer.Load4(apusrPass2GlobalC16Offset(gx, uint(gy), 0));
                const uint sharedBase = (ly * APUSR_FSR4_PASS2_LOAD_W + lx) * APUSR_FSR4_C16_DWORDS;
                apusr_pass2_c16_halo_tile[sharedBase + 0] = packed.x;
                apusr_pass2_c16_halo_tile[sharedBase + 1] = packed.y;
                apusr_pass2_c16_halo_tile[sharedBase + 2] = packed.z;
                apusr_pass2_c16_halo_tile[sharedBase + 3] = packed.w;
            }

            for (uint lx = endRealLx + flatThread; lx < APUSR_FSR4_PASS2_LOAD_W; lx += threadCount) {
                const uint sharedBase = (ly * APUSR_FSR4_PASS2_LOAD_W + lx) * APUSR_FSR4_C16_DWORDS;
                apusr_pass2_c16_halo_tile[sharedBase + 0] = 0u;
                apusr_pass2_c16_halo_tile[sharedBase + 1] = 0u;
                apusr_pass2_c16_halo_tile[sharedBase + 2] = 0u;
                apusr_pass2_c16_halo_tile[sharedBase + 3] = 0u;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    const uint ox = dispatchThreadId.x;
    const uint oy = dispatchThreadId.y;
    if (ox >= activeWidth || oy >= activeHeight) {
        return;
    }

    const ConstantBufferStorage<576> storageW0 = { embedded__encoder2_ResidualBlock_1_body_conv_dw_weight_quant_export_handler_QuantizeLinear_output_0_dwords };
    const QuantizedTensor4i8_NHWC<ConstantBufferStorage<576> > weights0 = {
        uint4(3, 3, 16, 16),
        uint4(0, 0, 0, 0),
        uint4(3, 3, 16, 16),
        uint4(3, 3, 16, 16),
        uint4(16, 48, 1, 144),
        uint4(0, 0, 0, 0),
        uint4(0, 0, 0, 0),
        0,
        APUSR_FSR4_PASS2_WEIGHT0_SCALE,
        storageW0 };

    const ConstantBufferStorage<8> storageB0 = { embedded_encoder2_ResidualBlock_1_body_conv_dw_bias_dwords };
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

    const ConstantBufferStorage<128> storageW1 = { embedded__encoder2_ResidualBlock_1_body_conv_pw_expand_weight_quant_export_handler_QuantizeLinear_output_0_dwords };
    const QuantizedTensor4i8_NHWC<ConstantBufferStorage<128> > weights1 = {
        uint4(1, 1, 16, 32),
        uint4(0, 0, 0, 0),
        uint4(1, 1, 16, 32),
        uint4(1, 1, 16, 32),
        uint4(16, 16, 1, 16),
        uint4(0, 0, 0, 0),
        uint4(0, 0, 0, 0),
        0,
        APUSR_FSR4_PASS2_WEIGHT1_SCALE,
        storageW1 };

    const ConstantBufferStorage<16> storageB1 = { embedded_encoder2_ResidualBlock_1_body_conv_pw_expand_bias_dwords };
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

    const ConstantBufferStorage<128> storageW2 = { embedded__encoder2_ResidualBlock_1_body_conv_pw_contract_weight_quant_export_handler_QuantizeLinear_output_0_dwords };
    const QuantizedTensor4i8_NHWC<ConstantBufferStorage<128> > weights2 = {
        uint4(1, 1, 32, 16),
        uint4(0, 0, 0, 0),
        uint4(1, 1, 32, 16),
        uint4(1, 1, 32, 16),
        uint4(32, 32, 1, 32),
        uint4(0, 0, 0, 0),
        uint4(0, 0, 0, 0),
        0,
        APUSR_FSR4_PASS2_WEIGHT2_SCALE,
        storageW2 };

    const ConstantBufferStorage<8> storageB2 = { embedded_encoder2_ResidualBlock_1_body_conv_pw_contract_bias_dwords };
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
                apusrPass2LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 0),
                apusrPass2LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 1),
                apusrPass2LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 2),
                apusrPass2LoadSharedC16(groupThreadId.x + kx, groupThreadId.y + ky, 3));

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
    const float inputScale0 = APUSR_FSR4_PASS2_INPUT_SCALE * weights0.quantizationScale;
    [unroll]
    for (uint f4 = 0; f4 < 16; f4 += 4) {
        const float4 acc = float4(accumulator[f4], accumulator[f4 + 1], accumulator[f4 + 2], accumulator[f4 + 3]);
        const int4 quantized = round((acc * inputScale0 + Load4hA(bias0, f4)) * APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE0);
        convResult[f4 / 4] = pack_clamp_s8(quantized);
    }

    int conv3Output[16];
    [unroll]
    for (uint i2 = 0; i2 < 16; ++i2) {
        conv3Output[i2] = 0;
    }

    // Stream the C16->C32->C16 pointwise MLP in 4-hidden-channel blocks.
    // Pass2 is the stage we want to own before attempting pass1->2 fusion; keep
    // its live state low from the start so a future fused producer patch does
    // not inherit the old materialized reluOutput[32]/reluResult[8] footprint.
    // The quantized ReLU boundary is still exact and local to each hidden block.
    const float inputScale1 = APUSR_FSR4_PASS2_INPUT_SCALE1 * weights1.quantizationScale;
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
        const int4 quantizedHidden = round((hiddenFloat * inputScale1 + Load4hA(bias1, h4)) * APUSR_FSR4_PASS2_RCPR_OUTPUT_SCALE1);
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
        apusrPass2LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 0),
        apusrPass2LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 1),
        apusrPass2LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 2),
        apusrPass2LoadSharedC16(groupThreadId.x + 1, groupThreadId.y + 1, 3));

    uint4 storeDwords;
    const float inputScale2 = weights2.quantizationScale * APUSR_FSR4_PASS2_INPUT_SCALE2;
    const float outputRcprScale = 1.0 / APUSR_FSR4_PASS2_OUTPUT_SCALE;
    [unroll]
    for (uint s4 = 0; s4 < 16; s4 += 4) {
        const float4 acc = float4(conv3Output[s4], conv3Output[s4 + 1], conv3Output[s4 + 2], conv3Output[s4 + 3]);
        float4 result = acc * inputScale2 + Load4hA(bias2, s4);
        result += unpack_s8s32(residualInput[s4 / 4]) * APUSR_FSR4_PASS2_INPUT_SCALE;
        storeDwords[s4 / 4] = pack_clamp_s8(int4(round(result * outputRcprScale)));
    }

    ScratchBuffer.Store4(apusrPass2OutputC16Offset(ox, oy), storeDwords);
}
