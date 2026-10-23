#include "ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e.h"
#include "ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9.h"

typedef union ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_PermutationKey;

typedef struct ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_PermutationInfo {
    const uint32_t       blobSize;
    const unsigned char* blobData;


    const uint32_t  numConstantBuffers;
    const char**    constantBufferNames;
    const uint32_t* constantBufferBindings;
    const uint32_t* constantBufferCounts;
    const uint32_t* constantBufferSpaces;

    const uint32_t  numSRVTextures;
    const char**    srvTextureNames;
    const uint32_t* srvTextureBindings;
    const uint32_t* srvTextureCounts;
    const uint32_t* srvTextureSpaces;

    const uint32_t  numUAVTextures;
    const char**    uavTextureNames;
    const uint32_t* uavTextureBindings;
    const uint32_t* uavTextureCounts;
    const uint32_t* uavTextureSpaces;

    const uint32_t  numSRVBuffers;
    const char**    srvBufferNames;
    const uint32_t* srvBufferBindings;
    const uint32_t* srvBufferCounts;
    const uint32_t* srvBufferSpaces;

    const uint32_t  numUAVBuffers;
    const char**    uavBufferNames;
    const uint32_t* uavBufferBindings;
    const uint32_t* uavBufferCounts;
    const uint32_t* uavBufferSpaces;

    const uint32_t  numSamplers;
    const char**    samplerNames;
    const uint32_t* samplerBindings;
    const uint32_t* samplerCounts;
    const uint32_t* samplerSpaces;

    const uint32_t  numRTAccelerationStructures;
    const char**    rtAccelerationStructureNames;
    const uint32_t* rtAccelerationStructureBindings;
    const uint32_t* rtAccelerationStructureCounts;
    const uint32_t* rtAccelerationStructureSpaces;
} ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_IndirectionTable[] = {
    0,
    1,
    0,
    1,
    0,
    1,
    0,
    1,
};

static const ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_PermutationInfo g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_PermutationInfo[] = {
    { g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_size, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_data, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_CBVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_CBVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_CBVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_CBVResourceSpaces, 7, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureSRVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureSRVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureSRVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureUAVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureUAVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureUAVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_SamplerResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_SamplerResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_SamplerResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_12f6c6a14f47a3f06c123f55472af97e_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
    { g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_size, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_data, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_CBVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_CBVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_CBVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_CBVResourceSpaces, 7, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureSRVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureSRVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureSRVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureUAVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureUAVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureUAVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_SamplerResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_SamplerResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_SamplerResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_16bit_5111bec4ab6a344b8c634762a919e7c9_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
};

