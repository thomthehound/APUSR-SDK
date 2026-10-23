#include "ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea.h"

typedef union ffx_frameinterpolation_pass_wave64_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_pass_wave64_PermutationKey;

typedef struct ffx_frameinterpolation_pass_wave64_PermutationInfo {
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
} ffx_frameinterpolation_pass_wave64_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_pass_wave64_IndirectionTable[] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};

static const ffx_frameinterpolation_pass_wave64_PermutationInfo g_ffx_frameinterpolation_pass_wave64_PermutationInfo[] = {
    { g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_size, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_data, 1, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_CBVResourceNames, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_CBVResourceBindings, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_CBVResourceCounts, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_CBVResourceSpaces, 8, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureSRVResourceNames, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureSRVResourceBindings, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureSRVResourceCounts, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureSRVResourceSpaces, 2, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureUAVResourceNames, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureUAVResourceBindings, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureUAVResourceCounts, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_TextureUAVResourceSpaces, 1, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_BufferSRVResourceNames, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_BufferSRVResourceBindings, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_BufferSRVResourceCounts, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_BufferSRVResourceSpaces, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_SamplerResourceNames, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_SamplerResourceBindings, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_SamplerResourceCounts, g_ffx_frameinterpolation_pass_wave64_d241884b751ae04ede409f4d0242fdea_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
};

