// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_scratch_layout.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

int main()
{
    using namespace apusr;
    using namespace apusr::models::fsr4_int8;

    const FrontC16ScratchCarrierDesc r1080 = makeFrontC16ScratchCarrierDesc(FrameCapacity::R1080);
    assert(r1080.capacityDisplaySize.width == 1920);
    assert(r1080.capacityDisplaySize.height == 1080);
    assert(r1080.denseC16Size.width == 960);
    assert(r1080.denseC16Size.height == 540);
    assert(r1080.denseStage0Bytes == 8'294'400ull);
    assert(r1080.paddedStage0Bytes == 8'342'464ull);
    assert(r1080.paddedStage0ExtraBytes == 48'064ull);
    assert(r1080.currentPass1OutputOffsetBytes == r1080.denseStage0Bytes);
    assert(r1080.paddedPass1OutputOffsetBytes == r1080.paddedStage0Bytes);
    assert(r1080.generatedMaxScratchEndBytes == 20'736'000ull);
    assert(r1080.declaredScratchBytes == 20'880'256ull);
    assert(r1080.declaredReserveBytes == 144'256ull);
    assert(r1080.paddedLayoutReserveBytes == 96'192ull);
    assert(r1080.paddedStage0FitsDeclaredScratch);

    const FrontC16ScratchCarrierDesc r2160 = makeFrontC16ScratchCarrierDesc(FrameCapacity::R2160);
    assert(r2160.capacityDisplaySize.width == 3840);
    assert(r2160.capacityDisplaySize.height == 2160);
    assert(r2160.denseC16Size.width == 1920);
    assert(r2160.denseC16Size.height == 1080);
    assert(r2160.denseStage0Bytes == 33'177'600ull);
    assert(r2160.paddedStage0Bytes == 33'273'664ull);
    assert(r2160.paddedStage0ExtraBytes == 96'064ull);
    assert(r2160.generatedMaxScratchEndBytes == 82'944'000ull);
    assert(r2160.declaredScratchBytes == 83'232'256ull);
    assert(r2160.declaredReserveBytes == 288'256ull);
    assert(r2160.paddedLayoutMaxScratchEndBytes == 83'040'064ull);
    assert(r2160.paddedLayoutReserveBytes == 192'192ull);
    assert(r2160.paddedStage0FitsDeclaredScratch);

    const FrontC16ScratchCarrierDesc r4320 = makeFrontC16ScratchCarrierDesc(FrameCapacity::R4320);
    assert(r4320.denseC16Size.width == 3840);
    assert(r4320.denseC16Size.height == 2160);
    assert(r4320.denseStage0Bytes == 132'710'400ull);
    assert(r4320.paddedStage0ExtraBytes == 192'064ull);
    assert(r4320.generatedMaxScratchEndBytes == 331'776'000ull);
    assert(r4320.declaredScratchBytes == 332'352'256ull);
    assert(r4320.declaredReserveBytes == 576'256ull);
    assert(r4320.paddedLayoutReserveBytes == 384'192ull);
    assert(frontC16PaddedStage0CarrierFitsCurrentScratch(FrameCapacity::R1080));
    assert(frontC16PaddedStage0CarrierFitsCurrentScratch(FrameCapacity::R2160));
    assert(frontC16PaddedStage0CarrierFitsCurrentScratch(FrameCapacity::R4320));


    const FrontC16ScratchLayoutContractDesc dense2160 = makeFrontC16ScratchLayoutContract(
        FrameCapacity::R2160,
        ScratchLayoutVariant::DenseStage0Current);
    assert(dense2160.variant == ScratchLayoutVariant::DenseStage0Current);
    assert(dense2160.stage0StorageSize.width == 1920);
    assert(dense2160.stage0StorageSize.height == 1080);
    assert(dense2160.stage0LogicalOriginX == 0);
    assert(dense2160.stage0LogicalOriginY == 0);
    assert(dense2160.stage0RowStrideBytes == 30'720ull);
    assert(dense2160.stage1OutputOffsetBytes == 33'177'600ull);
    assert(dense2160.maxUsedScratchBytes == 82'944'000ull);
    assert(dense2160.reserveBytes == 288'256ull);
    assert(dense2160.fitsDeclaredScratch);
    assert(dense2160.regionsAreContiguous);
    assert(dense2160.regionsAreNonOverlapping);
    assert(!dense2160.stage0OriginHasOnePixelHalo);
    assert(dense2160.regions.size() == 3);
    assert(dense2160.regions[0].offsetBytes == 0);
    assert(dense2160.regions[0].byteSize == dense2160.stage1OutputOffsetBytes);
    assert(dense2160.regions[1].offsetBytes == dense2160.stage1OutputOffsetBytes);
    assert(dense2160.regions[1].endBytes == dense2160.maxUsedScratchBytes);
    assert(dense2160.regions[2].offsetBytes == dense2160.maxUsedScratchBytes);
    assert(dense2160.regions[2].endBytes == dense2160.declaredScratchBytes);

    const FrontC16ScratchLayoutContractDesc padded2160 = makeFrontC16ScratchLayoutContract(
        FrameCapacity::R2160,
        ScratchLayoutVariant::PaddedStage0Candidate);
    assert(padded2160.variant == ScratchLayoutVariant::PaddedStage0Candidate);
    assert(padded2160.stage0StorageSize.width == 1922);
    assert(padded2160.stage0StorageSize.height == 1082);
    assert(padded2160.stage0LogicalOriginX == 1);
    assert(padded2160.stage0LogicalOriginY == 1);
    assert(padded2160.stage0RowStrideBytes == 30'752ull);
    assert(padded2160.stage1OutputOffsetBytes == 33'273'664ull);
    assert(padded2160.maxUsedScratchBytes == 83'040'064ull);
    assert(padded2160.reserveBytes == 192'192ull);
    assert(padded2160.extraBytesVsDenseCurrent == 96'064ull);
    assert(padded2160.fitsDeclaredScratch);
    assert(padded2160.regionsAreContiguous);
    assert(padded2160.regionsAreNonOverlapping);
    assert(padded2160.stage0OriginHasOnePixelHalo);
    assert(padded2160.regions.size() == 3);
    assert(padded2160.regions[0].owner == ScratchRegionOwner::SharedUmaCandidate);
    assert(padded2160.regions[0].role == ScratchRegionRole::Stage0C16Carrier);
    assert(padded2160.regions[1].mayAliasAfterBarrier);
    assert(padded2160.regions[2].byteSize == padded2160.reserveBytes);
    assert(scratchLayoutVariantName(ScratchLayoutVariant::PaddedStage0Candidate) == std::string("padded-stage0-candidate"));
    assert(scratchRegionRoleName(ScratchRegionRole::DeclaredReserve) == std::string("declared-reserve"));
    assert(scratchRegionOwnerName(ScratchRegionOwner::SharedUmaCandidate) == std::string("shared-uma-candidate"));

    const FrontC16PassTilingDesc ultrawide = makeFrontC16PassTilingDesc(DisplaySize{.width = 3440, .height = 1440});
    assert(ultrawide.alignedDisplaySize.width == 3440);
    assert(ultrawide.alignedDisplaySize.height == 1440);
    assert(ultrawide.frameCapacity == FrameCapacity::R2160);
    assert(ultrawide.activeC16Size.width == 1720);
    assert(ultrawide.activeC16Size.height == 720);
    assert(ultrawide.capacityC16Size.width == 1920);
    assert(ultrawide.capacityC16Size.height == 1080);
    assert(ultrawide.groupsX == 27);
    assert(ultrawide.groupsY == 360);
    assert(ultrawide.lastGroupValidPixels == 56);
    assert(ultrawide.lastGroupPaddingPixels == 8);
    assert(!ultrawide.activeWidthIsIntegralTile);
    assert(!ultrawide.groupsXIsEven);

    const FrontC16PassTilingDesc full2160 = makeFrontC16PassTilingDesc(DisplaySize{.width = 3840, .height = 2160});
    assert(full2160.activeC16Size.width == 1920);
    assert(full2160.groupsX == 30);
    assert(full2160.lastGroupValidPixels == 64);
    assert(full2160.lastGroupPaddingPixels == 0);
    assert(full2160.activeWidthIsIntegralTile);
    assert(full2160.groupsXIsEven);

    const FrontC16PassTilingDesc odd = makeFrontC16PassTilingDesc(DisplaySize{.width = 1921, .height = 1079});
    assert(odd.alignedDisplaySize.width == 1928);
    assert(odd.alignedDisplaySize.height == 1080);
    assert(odd.activeC16Size.width == 964);
    assert(odd.groupsX == 16);
    assert(odd.lastGroupValidPixels == 4);
    assert(odd.lastGroupPaddingPixels == 60);
    assert(!odd.activeWidthIsIntegralTile);
    assert(odd.groupsXIsEven);

    const MiddlePassTilingDesc ultrawideStage3 = makeMiddlePassTilingDesc(3, DisplaySize{.width = 3440, .height = 1440});
    assert(ultrawideStage3.validStage);
    assert(ultrawideStage3.tensorSize.width == 860);
    assert(ultrawideStage3.tensorSize.height == 360);
    assert(ultrawideStage3.tileWidth == 64);
    assert(ultrawideStage3.tileHeight == 1);
    assert(ultrawideStage3.groupsX == 14);
    assert(ultrawideStage3.groupsY == 360);
    assert(ultrawideStage3.lastGroupValidPixelsX == 28);
    assert(ultrawideStage3.lastGroupPaddingPixelsX == 36);
    assert(!ultrawideStage3.activeWidthIsIntegralTile);
    assert(ultrawideStage3.activeHeightIsIntegralTile);
    assert(ultrawideStage3.groupsXIsEven);

    const MiddlePassTilingDesc ultrawideStage9 = makeMiddlePassTilingDesc(9, DisplaySize{.width = 3440, .height = 1440});
    assert(ultrawideStage9.validStage);
    assert(ultrawideStage9.tensorSize.width == 430);
    assert(ultrawideStage9.tensorSize.height == 180);
    assert(ultrawideStage9.tileWidth == 8);
    assert(ultrawideStage9.tileHeight == 8);
    assert(ultrawideStage9.groupsX == 54);
    assert(ultrawideStage9.groupsY == 23);
    assert(ultrawideStage9.lastGroupValidPixelsX == 6);
    assert(ultrawideStage9.lastGroupValidPixelsY == 4);
    assert(ultrawideStage9.lastGroupPaddingPixelsX == 2);
    assert(ultrawideStage9.lastGroupPaddingPixelsY == 4);

    const FrontC16PassTilingDesc superUltrawide = makeFrontC16PassTilingDesc(DisplaySize{.width = 5120, .height = 1440});
    assert(superUltrawide.frameCapacity == FrameCapacity::R4320);
    assert(superUltrawide.activeC16Size.width == 2560);
    assert(superUltrawide.activeC16Size.height == 720);
    assert(superUltrawide.groupsX == 40);
    assert(superUltrawide.activeWidthIsIntegralTile);

    const MiddlePassTilingDesc surround = makeMiddlePassTilingDesc(1, DisplaySize{.width = 7680, .height = 1440});
    assert(surround.validStage);
    assert(surround.frameCapacity == FrameCapacity::R4320);
    assert(surround.tensorSize.width == 3840);
    assert(surround.groupsX == 60);
    assert(surround.tileHeight == 2);
    assert(surround.groupsY == 360);
    assert(surround.activeWidthIsIntegralTile);

    const MiddlePassTilingDesc portraitStage1 = makeMiddlePassTilingDesc(1, DisplaySize{.width = 2160, .height = 3840});
    assert(portraitStage1.validStage);
    assert(portraitStage1.frameCapacity == FrameCapacity::R4320);
    assert(portraitStage1.tensorSize.width == 1080);
    assert(portraitStage1.tensorSize.height == 1920);
    assert(portraitStage1.groupsX == 17);
    assert(portraitStage1.tileHeight == 2);
    assert(portraitStage1.groupsY == 960);
    assert(portraitStage1.lastGroupValidPixelsX == 56);
    assert(portraitStage1.lastGroupPaddingPixelsX == 8);

    const MiddlePassTilingDesc invalidStage = makeMiddlePassTilingDesc(0, DisplaySize{.width = 3440, .height = 1440});
    assert(!invalidStage.validStage);

    std::cout << "APUSR FSR4 INT8 scratch layout smoke: weird-aspect tiling and padded Stage0 carrier budget validated\n";
    return 0;
}
