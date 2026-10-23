#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118.h"
#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590.h"

typedef union ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationKey;

typedef struct ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo {
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
} ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_IndirectionTable[] = {
    1,
    0,
    1,
    0,
    1,
    0,
    1,
    0,
};

static const ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_PermutationInfo[] = {
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_CBVResourceSpaces, 3, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_SamplerResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_SamplerResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_SamplerResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_5fd66f961d825b0b4aadeef6abfe3118_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_CBVResourceSpaces, 3, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_SamplerResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_SamplerResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_SamplerResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_16bit_c1b73ac3a43628a7c9438c1c4c299590_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
};

