// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#ifndef APUSR_AUTO_EXPOSURE_HLSLI
#define APUSR_AUTO_EXPOSURE_HLSLI

struct ApusrAutoExposureConstants
{
    uint renderWidth;
    uint renderHeight;
    uint tileColumns;
    uint tileCount;
    float preExposure;
    uint featureFlags;
    uint pad0;
    uint pad1;
};

float ApusrAutoExposureLogLuma(float3 linearColor, float preExposure)
{
    const float3 rgbToY = float3(0.2126f, 0.7152f, 0.0722f);
    const float3 unexposedColor = linearColor / max(preExposure, 1.0e-6f);
    return log(max(dot(rgbToY, unexposedColor), 1.0e-6f));
}

float ApusrExposureFromAverageLogLuma(float averageLogLuma)
{
    // Same camera calibration as the FSR4 SPD auto-exposure pass, simplified to
    // one exponential because this runs once per frame after reduction.
    return rcp(max(9.6f * exp(averageLogLuma), 1.0e-6f));
}

#endif // APUSR_AUTO_EXPOSURE_HLSLI
