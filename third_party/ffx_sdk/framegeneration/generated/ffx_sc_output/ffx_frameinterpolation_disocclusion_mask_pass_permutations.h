#include "ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11.h"
#include "ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea.h"

typedef union ffx_frameinterpolation_disocclusion_mask_pass_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_disocclusion_mask_pass_PermutationKey;

typedef struct ffx_frameinterpolation_disocclusion_mask_pass_PermutationInfo {
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
} ffx_frameinterpolation_disocclusion_mask_pass_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_disocclusion_mask_pass_IndirectionTable[] = {
    1,
    0,
    1,
    0,
    1,
    0,
    1,
    0,
};

static const ffx_frameinterpolation_disocclusion_mask_pass_PermutationInfo g_ffx_frameinterpolation_disocclusion_mask_pass_PermutationInfo[] = {
    { g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_size, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_data, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_CBVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_CBVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_CBVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_CBVResourceSpaces, 7, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureSRVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureSRVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureSRVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureUAVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureUAVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureUAVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_SamplerResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_SamplerResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_SamplerResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_f352436cf2bd542c77fef11a5dbe5e11_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
    { g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_size, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_data, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_CBVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_CBVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_CBVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_CBVResourceSpaces, 7, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureSRVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureSRVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureSRVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureSRVResourceSpaces, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureUAVResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureUAVResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureUAVResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_TextureUAVResourceSpaces, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_SamplerResourceNames, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_SamplerResourceBindings, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_SamplerResourceCounts, g_ffx_frameinterpolation_disocclusion_mask_pass_63e38b3fb92813ba70053958f93c8bea_SamplerResourceSpaces, 0, 0, 0, 0, 0, },
};

