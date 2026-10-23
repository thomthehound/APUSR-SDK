#include "ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a.h"
#include "ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd.h"

typedef union ffx_frameinterpolation_disocclusion_mask_pass_wave64_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_disocclusion_mask_pass_wave64_PermutationKey;

typedef struct ffx_frameinterpolation_disocclusion_mask_pass_wave64_PermutationInfo {
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
} ffx_frameinterpolation_disocclusion_mask_pass_wave64_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_IndirectionTable[] = {
    1,
    0,
    1,
    0,
    1,
    0,
    1,
    0,
};

static const ffx_frameinterpolation_disocclusion_mask_pass_wave64_PermutationInfo g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_PermutationInfo[] = {
    { g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_size, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_data, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_CBVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_CBVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_CBVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_CBVResourceSpaces, 7, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureSRVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureSRVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureSRVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureUAVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureUAVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureUAVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_SamplerResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_SamplerResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_SamplerResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_b8e22acdb2f4327e16561389033c627a_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
    { g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_size, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_data, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_CBVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_CBVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_CBVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_CBVResourceSpaces, 7, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureSRVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureSRVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureSRVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureUAVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureUAVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureUAVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_SamplerResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_SamplerResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_SamplerResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_wave64_f21f468b373decb848f375ce2d23b1fd_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
};

