#include <fuse/renderer/volumetric/volumetric_fog.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason);
bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason);
bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason);
bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason);
bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason);
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

namespace {

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

bool isValidFroxelCamera(const FroxelCameraDesc& camera) {
    return camera.nearPlane > 0.f && camera.farPlane > camera.nearPlane;
bool isScreenCoordOutOfRange(f32 screenX, f32 screenY) {
    return screenX < 0.f || screenX > 1.f || screenY < 0.f || screenY > 1.f;
}

f32 froxelDensityAt(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 tileX, u32 tileY, u32 sliceZ) {
    if (grid.density.empty() || desc.tilesX == 0u || desc.tilesY == 0u || desc.slicesZ == 0u) {
        return 0.f;
    }

    const u32 index = FroxelGridLayout::froxelIndex(FroxelGridLayout::clampTileX(tileX, desc),
                                                    FroxelGridLayout::clampTileY(tileY, desc),
                                                    FroxelGridLayout::clampSliceZ(sliceZ, desc),
                                                    desc);
    if (index >= grid.density.size()) {
        return 0.f;
    }
    return grid.density[index];
}

bool mapScreenDepthToSampleCoordsImpl(f32 screenX,
                                      f32 screenY,
                                      f32 viewDepth,
                                      const FroxelGridDesc& desc,
                                      const FroxelCameraDesc& camera,
                                      FroxelSampleCoords& outCoords) {
    const f32 continuousTileX = clamp01(screenX) * static_cast<f32>(desc.tilesX);
    const f32 continuousTileY = clamp01(screenY) * static_cast<f32>(desc.tilesY);
    outCoords.tileX0 = FroxelGridLayout::clampTileX(static_cast<u32>(continuousTileX), desc);
    outCoords.tileY0 = FroxelGridLayout::clampTileY(static_cast<u32>(continuousTileY), desc);
    outCoords.tileX1 = std::min(outCoords.tileX0 + 1u, desc.tilesX - 1u);
    outCoords.tileY1 = std::min(outCoords.tileY0 + 1u, desc.tilesY - 1u);
    outCoords.tx = continuousTileX - static_cast<f32>(outCoords.tileX0);
    outCoords.ty = continuousTileY - static_cast<f32>(outCoords.tileY0);

    outCoords.sliceZ0 = FroxelSliceLayout::computeSliceZFromDepth(viewDepth, desc, camera);
    outCoords.sliceZ1 = std::min(outCoords.sliceZ0 + 1u, desc.slicesZ - 1u);
    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 logDepth = std::log(std::clamp(viewDepth, camera.nearPlane, camera.farPlane) / camera.nearPlane) /
                         std::log(depthRatio);
    const f32 continuousSlice = logDepth * static_cast<f32>(desc.slicesZ);
    outCoords.tz = continuousSlice - static_cast<f32>(outCoords.sliceZ0);
    return true;
}

} // namespace

void FroxelDensityGrid::allocate(const FroxelGridDesc& desc) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    density.assign(clampedDesc.froxelCount(), 0.f);
}

void FroxelDensityGrid::clear() {
    density.clear();
}

bool FroxelDensityGrid::matchesDesc(const FroxelGridDesc& desc) const {
    return density.size() == FroxelGridDesc::clampCounts(desc).froxelCount();
}

FroxelGridDesc FroxelGridDesc::clampCounts(const FroxelGridDesc& raw) {
    FroxelGridDesc out = raw;
    if (out.tilesX > kMaxTilesX) {
        out.tilesX = kMaxTilesX;
    }
    if (out.tilesY > kMaxTilesY) {
        out.tilesY = kMaxTilesY;
    }
    if (out.slicesZ > kMaxSlicesZ) {
        out.slicesZ = kMaxSlicesZ;
    }
    return out;
}

const char* froxelCameraRejectReasonLabel(FroxelCameraRejectReason reason) {
    switch (reason) {
    case FroxelCameraRejectReason::None:
        return "none";
    case FroxelCameraRejectReason::InvalidNearPlane:
        return "invalid_near_plane";
    case FroxelCameraRejectReason::InvalidFarPlane:
        return "invalid_far_plane";
    case FroxelCameraRejectReason::InvertedDepthRange:
        return "inverted_depth_range";
    }
    return "unknown";

bool FroxelSliceLayout::isCameraValid(const FroxelCameraDesc& camera) {
    FroxelCameraRejectReason reason = FroxelCameraRejectReason::None;
    return tryValidateCamera(camera, reason);

bool FroxelSliceLayout::tryValidateCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason) {
    if (camera.nearPlane <= 0.f) {
        outReason = FroxelCameraRejectReason::InvalidNearPlane;
        return false;
    if (camera.farPlane <= 0.f) {
        outReason = FroxelCameraRejectReason::InvalidFarPlane;
    if (camera.farPlane <= camera.nearPlane) {
        outReason = FroxelCameraRejectReason::InvertedDepthRange;

    outReason = FroxelCameraRejectReason::None;
    return true;

bool FroxelSliceLayout::isSliceIndexOutOfRange(u32 sliceZ, const FroxelGridDesc& desc) {
    return desc.slicesZ == 0u || sliceZ >= desc.slicesZ;
bool FroxelGridDesc::wouldClampCounts(const FroxelGridDesc& raw) {
    return raw.tilesX > kMaxTilesX || raw.tilesY > kMaxTilesY || raw.slicesZ > kMaxSlicesZ;
}

f32 FroxelSliceLayout::computeSliceNearZ(u32 sliceZ, const FroxelGridDesc& desc, const FroxelCameraDesc& camera) {
    if (sliceZ >= desc.slicesZ) {
        return camera.farPlane;
    }

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 sliceT0 = static_cast<f32>(sliceZ) / static_cast<f32>(desc.slicesZ);
    return camera.nearPlane * std::pow(depthRatio, sliceT0);
}

f32 FroxelSliceLayout::computeSliceFarZ(u32 sliceZ, const FroxelGridDesc& desc, const FroxelCameraDesc& camera) {
    if (sliceZ >= desc.slicesZ) {
        return camera.farPlane;
    }

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 sliceT1 = static_cast<f32>(sliceZ + 1u) / static_cast<f32>(desc.slicesZ);
    return camera.nearPlane * std::pow(depthRatio, sliceT1);
}

u32 FroxelSliceLayout::computeSliceZFromDepth(f32 viewDepth,
                                              const FroxelGridDesc& desc,
                                              const FroxelCameraDesc& camera) {
    if (desc.slicesZ == 0u || camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return 0u;
    }

    const f32 clampedDepth = std::clamp(viewDepth, camera.nearPlane, camera.farPlane);
    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 logDepth = std::log(clampedDepth / camera.nearPlane) / std::log(depthRatio);
    const u32 sliceZ = static_cast<u32>(logDepth * static_cast<f32>(desc.slicesZ));
    return FroxelGridLayout::clampSliceZ(sliceZ, desc);
}

bool FroxelGridLayout::isEmptyGrid(const FroxelGridDesc& desc) {
    return desc.tilesX == 0u || desc.tilesY == 0u || desc.slicesZ == 0u;
}

u32 FroxelGridLayout::froxelIndex(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return (tileY * desc.tilesX + tileX) * desc.slicesZ + sliceZ;
}

u32 FroxelGridLayout::froxelIndexClamped(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return froxelIndex(clampTileX(tileX, desc), clampTileY(tileY, desc), clampSliceZ(sliceZ, desc), desc);
}

void FroxelGridLayout::decodeFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& tileX, u32& tileY, u32& sliceZ) {
    if (isEmptyGrid(desc)) {
        tileX = 0u;
        tileY = 0u;
        sliceZ = 0u;
        return;
    }

    const u32 clampedIndex = clampFroxelIndex(index, desc);
    sliceZ = clampedIndex % desc.slicesZ;
    const u32 tileSlice = clampedIndex / desc.slicesZ;
    tileX = tileSlice % desc.tilesX;
    tileY = tileSlice / desc.tilesX;
}

bool FroxelGridLayout::isValidFroxelIndex(u32 index, const FroxelGridDesc& desc) {
    return index < desc.froxelCount();
}

bool FroxelGridLayout::isFroxelIndexOutOfRange(u32 index, const FroxelGridDesc& desc) {
    const u32 count = desc.froxelCount();
    return count == 0u || index >= count;
}

bool FroxelGridLayout::isCoordOutOfRange(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
bool FroxelGridLayout::isFroxelCoordOutOfRange(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return true;
    }
    return tileX >= desc.tilesX || tileY >= desc.tilesY || sliceZ >= desc.slicesZ;
}

u32 FroxelGridLayout::clampFroxelIndex(u32 index, const FroxelGridDesc& desc) {
    const u32 count = desc.froxelCount();
    if (count == 0u) {
        return 0u;
    }
    return std::min(index, count - 1u);
}

bool FroxelGridLayout::areSampleCoordsInBounds(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ) {
        return false;
    }
    if (coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
        return false;
    }
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        return false;
    }
    return true;
}

bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                               const FroxelGridDesc& desc,
                                               SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }

    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        outReason = SampleCoordRejectReason::UnorderedCorners;

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ ||
        coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
        outReason = SampleCoordRejectReason::OutOfBounds;

    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = SampleCoordRejectReason::InvalidWeights;

    outReason = SampleCoordRejectReason::None;
    return true;

bool FroxelGridLayout::tryPreflightNonEmptyGrid(const FroxelGridDesc& desc, GridDensityRejectReason& outReason) {
        outReason = GridDensityRejectReason::EmptyDesc;

    outReason = GridDensityRejectReason::None;

bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryValidateSampleCoords(coords, desc, reason);
bool FroxelGridLayout::areSampleCoordsOrdered(const FroxelSampleCoords& coords) {
    return coords.tileX0 <= coords.tileX1 && coords.tileY0 <= coords.tileY1 && coords.sliceZ0 <= coords.sliceZ1;

    if (!areSampleCoordsInBounds(coords, desc)) {

    return areSampleCoordsOrdered(coords);
}

void FroxelGridLayout::normalizeSampleCoords(FroxelSampleCoords& coords) {
    if (coords.tileX0 > coords.tileX1) {
        std::swap(coords.tileX0, coords.tileX1);
        coords.tx = 1.f - coords.tx;
    }
    if (coords.tileY0 > coords.tileY1) {
        std::swap(coords.tileY0, coords.tileY1);
        coords.ty = 1.f - coords.ty;
    }
    if (coords.sliceZ0 > coords.sliceZ1) {
        std::swap(coords.sliceZ0, coords.sliceZ1);
        coords.tz = 1.f - coords.tz;
    }

    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);
}

bool FroxelGridLayout::isSampleCoordsOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return true;
    }
    return !areSampleCoordsInBounds(coords, desc);
}

bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
bool FroxelGridLayout::tryAreSampleCoordsInBounds(const FroxelSampleCoords& coords,
                                                  const FroxelGridDesc& desc,
                                                  SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ) {
        outReason = SampleCoordRejectReason::OutOfRangeTile;
    if (coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = SampleCoordRejectReason::OutOfRangeWeight;

    outReason = SampleCoordRejectReason::None;
    return true;
bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                               FroxelSampleCoordsRejectReason& outReason) {
        outReason = FroxelSampleCoordsRejectReason::EmptyGrid;

    if (!areSampleCoordsInBounds(coords, desc)) {
        const bool indicesInRange = coords.tileX0 <= maxTileX && coords.tileY0 <= maxTileY &&
                                    coords.sliceZ0 <= maxSliceZ && coords.tileX1 <= maxTileX &&
                                    coords.tileY1 <= maxTileY && coords.sliceZ1 <= maxSliceZ;
        outReason = indicesInRange ? FroxelSampleCoordsRejectReason::OutOfRangeWeights
                                   : FroxelSampleCoordsRejectReason::OutOfRangeIndices;

    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        outReason = FroxelSampleCoordsRejectReason::UnorderedCorners;

    outReason = FroxelSampleCoordsRejectReason::None;

void FroxelGridLayout::clampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    if (!areSampleCoordsInBounds(coords, desc)) {
        return false;
    }
    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        return false;
    }
    return true;
}

void FroxelGridLayout::normalizeSampleCoords(FroxelSampleCoords& coords) {
    if (coords.tileX0 > coords.tileX1) {
        std::swap(coords.tileX0, coords.tileX1);
        coords.tx = 1.f - coords.tx;
    }
    if (coords.tileY0 > coords.tileY1) {
        std::swap(coords.tileY0, coords.tileY1);
        coords.ty = 1.f - coords.ty;
    }
    if (coords.sliceZ0 > coords.sliceZ1) {
        std::swap(coords.sliceZ0, coords.sliceZ1);
        coords.tz = 1.f - coords.tz;
    }

    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);
}

void FroxelGridLayout::clampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (!tryClampSampleCoords(coords, desc)) {
        return;
    }
}

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    coords.tileX0 = clampTileX(coords.tileX0, desc);
    coords.tileY0 = clampTileY(coords.tileY0, desc);
    coords.sliceZ0 = clampSliceZ(coords.sliceZ0, desc);
    coords.tileX1 = std::min(clampTileX(coords.tileX1, desc), desc.tilesX - 1u);
    coords.tileY1 = std::min(clampTileY(coords.tileY1, desc), desc.tilesY - 1u);
    coords.sliceZ1 = std::min(clampSliceZ(coords.sliceZ1, desc), desc.slicesZ - 1u);
    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);
    normalizeSampleCoords(coords);
}

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryClampSampleCoords(coords, desc, reason);
}

bool FroxelGridLayout::tryMapScreenDepthToSampleCoords(f32 screenX,
                                                       f32 screenY,
                                                       f32 viewDepth,
                                                       const FroxelGridDesc& desc,
                                                       const FroxelCameraDesc& camera,
                                                       FroxelSampleCoords& outCoords,
                                                       SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        outReason = SampleCoordRejectReason::DepthOutOfRange;

    if (!mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords)) {

    outReason = SampleCoordRejectReason::None;
    return true;

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords,

    clampSampleCoords(coords, desc);





bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    return coords.tileX0 < desc.tilesX && coords.tileY0 < desc.tilesY && coords.sliceZ0 < desc.slicesZ &&
           coords.tileX1 < desc.tilesX && coords.tileY1 < desc.tilesY && coords.sliceZ1 < desc.slicesZ &&
           coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;



bool FroxelGridLayout::canMapScreenDepth(f32 viewDepth, const FroxelCameraDesc& camera) {
    return viewDepth >= camera.nearPlane && viewDepth <= camera.farPlane;

bool FroxelGridLayout::tryAreSampleCoordsInBounds(const FroxelSampleCoords& coords,
    if (!areSampleCoordsInBounds(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfBounds;


void FroxelGridLayout::normalizeFroxelSampleCoords(FroxelSampleCoords& coords) {
    if (coords.tileX0 > coords.tileX1) {
        std::swap(coords.tileX0, coords.tileX1);
        coords.tx = 1.f - coords.tx;
    if (coords.tileY0 > coords.tileY1) {
        std::swap(coords.tileY0, coords.tileY1);
        coords.ty = 1.f - coords.ty;
    if (coords.sliceZ0 > coords.sliceZ1) {
        std::swap(coords.sliceZ0, coords.sliceZ1);
        coords.tz = 1.f - coords.tz;

    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);

bool FroxelGridLayout::isValidFroxelSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
        return false;
    }

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;

    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!inRange(coords.tileX0, maxTileX) || !inRange(coords.tileX1, maxTileX) ||
        !inRange(coords.tileY0, maxTileY) || !inRange(coords.tileY1, maxTileY) ||
        !inRange(coords.sliceZ0, maxSliceZ) || !inRange(coords.sliceZ1, maxSliceZ)) {
        return false;
    }
    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {

    return coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;

bool FroxelGridLayout::buildFroxelSampleCoords(f32 screenX,
                                               f32 screenY,
                                               f32 viewDepth,
                                               const FroxelGridDesc& desc,
                                               const FroxelCameraDesc& camera,
                                               FroxelSampleCoords& outCoords) {
    return mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords);

void FroxelGridLayout::normalizeSampleCoords(FroxelSampleCoords& coords) {
    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);

const char* froxelGridRejectReasonLabel(FroxelGridRejectReason reason) {
    switch (reason) {
    case FroxelGridRejectReason::None:
        return "none";
    case FroxelGridRejectReason::EmptyTilesX:
        return "empty_tiles_x";
    case FroxelGridRejectReason::EmptyTilesY:
        return "empty_tiles_y";
    case FroxelGridRejectReason::EmptySlicesZ:
        return "empty_slices_z";
    return "unknown";

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::TileOutOfRange:
        return "tile_out_of_range";
    case SampleCoordRejectReason::WeightOutOfRange:
        return "weight_out_of_range";

const char* froxelScreenMappingRejectReasonLabel(FroxelScreenMappingRejectReason reason) {
    case FroxelScreenMappingRejectReason::None:
    case FroxelScreenMappingRejectReason::EmptyGrid:
    case FroxelScreenMappingRejectReason::InvalidCamera:
        return "invalid_camera";
    case FroxelScreenMappingRejectReason::DepthBelowNear:
        return "depth_below_near";
    case FroxelScreenMappingRejectReason::DepthAboveFar:
        return "depth_above_far";

bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                               SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;

    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ ||
        coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
        outReason = SampleCoordRejectReason::TileOutOfRange;
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = SampleCoordRejectReason::WeightOutOfRange;

    clampSampleCoords(coords, desc);
    outReason = SampleCoordRejectReason::None;
    return true;
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordsReject(const FroxelSampleCoords& coords,
                                                                     const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return SampleCoordRejectReason::EmptyGrid;

    if (isValidSampleCoords(coords, desc)) {
        return SampleCoordRejectReason::None;

    if (!areSampleCoordsInBounds(coords, desc)) {
        const bool indicesInRange =
            coords.tileX0 <= desc.tilesX - 1u && coords.tileY0 <= desc.tilesY - 1u &&
            coords.sliceZ0 <= desc.slicesZ - 1u && coords.tileX1 <= desc.tilesX - 1u &&
            coords.tileY1 <= desc.tilesY - 1u && coords.sliceZ1 <= desc.slicesZ - 1u;
        if (indicesInRange) {
            return SampleCoordRejectReason::InvalidWeights;

        return SampleCoordRejectReason::OutOfBounds;


ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 /*screenX*/,
                                                                      f32 /*screenY*/,
                                                                      f32 viewDepth,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelCameraDesc& camera) {
        return ScreenMappingRejectReason::EmptyGrid;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return ScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return ScreenMappingRejectReason::DepthOutOfRange;

    return ScreenMappingRejectReason::None;

bool FroxelGridLayout::preflightScreenDepthToSampleCoords(f32 screenX,
                                                          f32 screenY,
                                                          const FroxelCameraDesc& camera,
                                                          FroxelSampleCoords* outCoords,
                                                          ScreenMappingRejectReason* outReason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (outReason != nullptr) {
        *outReason = reject;
    if (screenMappingRejectReasonIsBlocking(reject)) {
        return false;

    FroxelSampleCoords coords{};
    mapScreenDepthToSampleCoordsImpl(screenX, screenY, viewDepth, desc, camera, coords);
    if (outCoords != nullptr) {
        *outCoords = coords;
    return true;
}

bool FroxelGridLayout::tryMapScreenDepthToSampleCoords(f32 screenX,
                                                       f32 screenY,
                                                       f32 viewDepth,
                                                       const FroxelGridDesc& desc,
                                                       const FroxelCameraDesc& camera,
                                                       FroxelSampleCoords& outCoords,
                                                       FroxelScreenMappingRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = FroxelScreenMappingRejectReason::EmptyGrid;
        return false;
    }

    FroxelCameraRejectReason cameraReason = FroxelCameraRejectReason::None;
    if (!FroxelSliceLayout::tryValidateCamera(camera, cameraReason)) {
        outReason = FroxelScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane) {
        outReason = FroxelScreenMappingRejectReason::DepthBelowNear;
    if (viewDepth > camera.farPlane) {
        outReason = FroxelScreenMappingRejectReason::DepthAboveFar;

    if (!mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords)) {

    outReason = FroxelScreenMappingRejectReason::None;
    return true;

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      u32& outFroxelIndex,
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);

void FroxelGridLayout::normalizeSampleCoords(FroxelSampleCoords& coords) {
    if (coords.tileX0 > coords.tileX1) {
        std::swap(coords.tileX0, coords.tileX1);
        coords.tx = 1.f - coords.tx;
    if (coords.tileY0 > coords.tileY1) {
        std::swap(coords.tileY0, coords.tileY1);
        coords.ty = 1.f - coords.ty;
    if (coords.sliceZ0 > coords.sliceZ1) {
        std::swap(coords.sliceZ0, coords.sliceZ1);
        coords.tz = 1.f - coords.tz;

    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);

bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {

    return areSampleCoordsInBounds(coords, desc);

bool FroxelGridLayout::tryCanSampleAtCoords(const FroxelSampleCoords& coords,
                                            SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        outReason = SampleCoordRejectReason::InvertedCorners;
    if (!areSampleCoordsInBounds(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfRange;

    outReason = SampleCoordRejectReason::None;


bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords,
    if (!tryPreflightSampleCoords(coords, desc, outReason)) {

    clampSampleCoords(coords, desc);
}

u32 FroxelGridLayout::clampTileX(u32 tileX, const FroxelGridDesc& desc) {
    if (desc.tilesX == 0u) {
        return 0u;
    }
    return std::min(tileX, desc.tilesX - 1u);
}

u32 FroxelGridLayout::clampTileY(u32 tileY, const FroxelGridDesc& desc) {
    if (desc.tilesY == 0u) {
        return 0u;
    }
    return std::min(tileY, desc.tilesY - 1u);
}

u32 FroxelGridLayout::clampSliceZ(u32 sliceZ, const FroxelGridDesc& desc) {
    if (desc.slicesZ == 0u) {
        return 0u;
    }
    return std::min(sliceZ, desc.slicesZ - 1u);
}

bool FroxelGridLayout::isTileCoordOutOfRange(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return true;
    }
    return tileX >= desc.tilesX || tileY >= desc.tilesY || sliceZ >= desc.slicesZ;
}

u32 FroxelGridLayout::maxFroxelIndex(const FroxelGridDesc& desc) {
    return desc.maxFroxelIndex();
}

bool FroxelGridLayout::isAtMaxFroxelIndex(u32 index, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    return index == maxFroxelIndex(desc);
}

bool FroxelGridLayout::tryClampFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& outIndex) {
    if (isEmptyGrid(desc)) {
        outIndex = 0u;
        return false;
    }

    outIndex = clampFroxelIndex(index, desc);
    return true;
}

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                      f32 screenY,
                                                                      f32 viewDepth,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelCameraDesc& camera) {
    if (isEmptyGrid(desc)) {
        return ScreenMappingRejectReason::EmptyGrid;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return ScreenMappingRejectReason::InvalidCamera;
    }
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return ScreenMappingRejectReason::DepthOutOfRange;
    }

    return ScreenMappingRejectReason::None;
}

bool FroxelGridLayout::preflightScreenMapping(f32 screenX,
                                              f32 screenY,
                                              f32 viewDepth,
                                              const FroxelGridDesc& desc,
                                              const FroxelCameraDesc& camera,
                                              ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !screenMappingRejectReasonIsBlocking(reject);
}

bool FroxelGridLayout::tryMapScreenDepthToSampleCoords(f32 screenX,
                                                       f32 screenY,
                                                       f32 viewDepth,
                                                       const FroxelGridDesc& desc,
                                                       const FroxelCameraDesc& camera,
                                                       FroxelSampleCoords& outCoords,
                                                       ScreenMappingRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = ScreenMappingRejectReason::EmptyGrid;
        return false;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = ScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        outReason = ScreenMappingRejectReason::DepthOutOfRange;
    outReason = classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (screenMappingRejectReasonIsBlocking(outReason)) {
    }

    mapScreenDepthToSampleCoordsImpl(screenX, screenY, viewDepth, desc, camera, outCoords);
    outReason = ScreenMappingRejectReason::None;
bool FroxelGridLayout::areSampleCoordsInBounds(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ) {
    if (coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
    if (outReason != ScreenMappingRejectReason::None) {




    outReason = classifyScreenDepthMappingReject(screenX, screenY, viewDepth, desc, camera);



    return true;

bool FroxelGridLayout::isSampleCoordsOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return !areSampleCoordsInBounds(coords, desc);

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    clampSampleCoords(coords, desc);
}

bool FroxelGridLayout::mapScreenDepthToSampleCoords(f32 screenX,
                                                    f32 screenY,
                                                    f32 viewDepth,
                                                    const FroxelGridDesc& desc,
                                                    const FroxelCameraDesc& camera,
                                                    FroxelSampleCoords& outCoords) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords, reason);

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      u32& outFroxelIndex,
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
        outFroxelIndex = 0u;
    FroxelSampleRejectReason reason = FroxelSampleRejectReason::None;

bool FroxelGridLayout::tryMapScreenDepthToSampleCoords(f32 screenX,
                                                       FroxelSampleCoords& outCoords,
                                                       FroxelSampleRejectReason& outReason) {
        outReason = FroxelSampleRejectReason::EmptyGrid;
        outReason = FroxelSampleRejectReason::InvalidCamera;
                                                       SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;
                                                       FroxelSampleCoordRejectReason& outReason) {
        outReason = FroxelSampleCoordRejectReason::EmptyGrid;
    if (!isValidFroxelCamera(camera)) {
        outReason = FroxelSampleCoordRejectReason::InvalidCamera;
        outReason = FroxelSampleRejectReason::DepthOutOfRange;
        outReason = SampleCoordRejectReason::DepthOutOfRange;
        outReason = FroxelSampleCoordRejectReason::DepthOutOfRange;
    if (!canMapScreenDepth(viewDepth, camera)) {
        outReason = SampleCoordRejectReason::InvalidDepth;
    if (viewDepth < camera.nearPlane) {
        outReason = SampleCoordRejectReason::DepthBelowNear;
    if (viewDepth > camera.farPlane) {
        outReason = SampleCoordRejectReason::DepthAboveFar;
        return false;
    }

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    outReason = ScreenMappingRejectReason::None;
    const f32 continuousTileX = clamp01(screenX) * static_cast<f32>(desc.tilesX);
    const f32 continuousTileY = clamp01(screenY) * static_cast<f32>(desc.tilesY);
    outCoords.tileX0 = clampTileX(static_cast<u32>(continuousTileX), desc);
    outCoords.tileY0 = clampTileY(static_cast<u32>(continuousTileY), desc);
    outCoords.tileX1 = std::min(outCoords.tileX0 + 1u, desc.tilesX - 1u);
    outCoords.tileY1 = std::min(outCoords.tileY0 + 1u, desc.tilesY - 1u);
    outCoords.tx = continuousTileX - static_cast<f32>(outCoords.tileX0);
    outCoords.ty = continuousTileY - static_cast<f32>(outCoords.tileY0);

    outCoords.sliceZ0 = FroxelSliceLayout::computeSliceZFromDepth(viewDepth, desc, camera);
    outCoords.sliceZ1 = std::min(outCoords.sliceZ0 + 1u, desc.slicesZ - 1u);
    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 logDepth = std::log(std::clamp(viewDepth, camera.nearPlane, camera.farPlane) / camera.nearPlane) /
                         std::log(depthRatio);
    const f32 continuousSlice = logDepth * static_cast<f32>(desc.slicesZ);
    outCoords.tz = continuousSlice - static_cast<f32>(outCoords.sliceZ0);
    outReason = FroxelSampleRejectReason::None;
    outReason = SampleCoordRejectReason::None;
    outReason = FroxelSampleCoordRejectReason::None;
    return true;
}

bool FroxelGridLayout::mapScreenDepthToSampleCoords(f32 screenX,
                                                    f32 screenY,
                                                    f32 viewDepth,
                                                    const FroxelGridDesc& desc,
                                                    const FroxelCameraDesc& camera,
                                                    FroxelSampleCoords& outCoords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords, reason);

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      u32& outFroxelIndex,
                                                      SampleCoordRejectReason& outReason) {
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
        outFroxelIndex = 0u;
        return false;

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    return true;
}

bool FroxelGridLayout::mapScreenDepthToSampleCoords(f32 screenX,
                                                    f32 screenY,
                                                    f32 viewDepth,
                                                    const FroxelGridDesc& desc,
                                                    const FroxelCameraDesc& camera,
                                                    FroxelSampleCoords& outCoords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords, reason);

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      u32& outFroxelIndex,
                                                      SampleCoordRejectReason& outReason) {
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
        outFroxelIndex = 0u;
        return false;

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    return true;
}

    FroxelSampleCoordRejectReason reason = FroxelSampleCoordRejectReason::None;

                                                      FroxelSampleCoordRejectReason& outReason) {



bool FroxelGridLayout::tryMapScreenDepthToSampleCoords(f32 screenX,
                                                       FroxelSampleCoords& outCoords,
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
    if (viewDepth < camera.nearPlane) {
        outReason = SampleCoordRejectReason::DepthBelowNear;
    if (viewDepth > camera.farPlane) {
        outReason = SampleCoordRejectReason::DepthAboveFar;

    if (!mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords)) {
        outReason = SampleCoordRejectReason::OutOfBounds;

    outReason = SampleCoordRejectReason::None;

bool FroxelGridLayout::mapScreenDepthToFroxelIndex(f32 screenX,
                                                   f32 screenY,
                                                   f32 viewDepth,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelCameraDesc& camera,
                                                   u32& outFroxelIndex) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, outFroxelIndex, reason);
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
bool FroxelGridLayout::tryMapScreenDepthToSampleCoords(f32 screenX,
                                                       f32 screenY,
                                                       f32 viewDepth,
                                                       const FroxelGridDesc& desc,
                                                       const FroxelCameraDesc& camera,
                                                       FroxelSampleCoords& outCoords,
                                                       SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = SampleCoordRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        outReason = SampleCoordRejectReason::DepthOutOfRange;

    const bool mapped = mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords);
    outReason = mapped ? SampleCoordRejectReason::None : SampleCoordRejectReason::DepthOutOfRange;
    return mapped;

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      u32& outFroxelIndex,
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    outReason = SampleCoordRejectReason::None;
    return true;

    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::DepthBelowNear:
        return "depth_below_near";
    case SampleCoordRejectReason::DepthAboveFar:
        return "depth_above_far";
    return "unknown";

const char* sampleCoordBoundsRejectReasonLabel(SampleCoordBoundsRejectReason reason) {
    case SampleCoordBoundsRejectReason::None:
    case SampleCoordBoundsRejectReason::EmptyGrid:
    case SampleCoordBoundsRejectReason::OutOfBoundsTile:
        return "out_of_bounds_tile";
    case SampleCoordBoundsRejectReason::OutOfBoundsWeight:
        return "out_of_bounds_weight";

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::DepthOutOfRange:
        return "depth_out_of_range";
    }
    return "unknown";
}

bool tryMapScreenDepthToSampleCoords(f32 screenX,
                                     f32 screenY,
                                     f32 viewDepth,
                                     const FroxelGridDesc& desc,
                                     const FroxelCameraDesc& camera,
                                     FroxelSampleCoords& outCoords,
                                     SampleCoordRejectReason& outReason) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        outReason = SampleCoordRejectReason::DepthOutOfRange;
        return false;
    }

    if (!FroxelGridLayout::mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords)) {
        outReason = SampleCoordRejectReason::DepthOutOfRange;
        return false;
    }

    outReason = SampleCoordRejectReason::None;
    return true;
}

const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
    case SampleCoordRejectReason::InvalidDepth:
        return "invalid_depth";
    return "unknown";
}

bool FroxelGridLayout::wouldClampScreenCoords(f32 screenX, f32 screenY) {
    return screenX < 0.f || screenX > 1.f || screenY < 0.f || screenY > 1.f;
}

bool FroxelGridLayout::canMapScreenDepthToSampleCoords(f32 screenX,
                                                       f32 screenY,
                                                       f32 viewDepth,
                                                       const FroxelGridDesc& desc,
                                                       const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    return tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
}

bool FroxelGridLayout::canMapScreenDepthToFroxelIndex(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    u32 froxelIndex = 0u;
    return tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reason);
}

bool FroxelGridLayout::wouldClampSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;

    return isSampleCoordsOutOfRange(coords, desc) || !isValidSampleCoords(coords, desc);

bool FroxelGridLayout::isSampleCoordsClampable(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryPreflightSampleCoords(coords, desc, reason) &&
           reason == SampleCoordRejectReason::InvalidWeights;
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             const FroxelGridDesc& desc,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);

SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason);

bool FroxelGridLayout::tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                                SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;

    if (isValidSampleCoords(coords, desc)) {
        outReason = SampleCoordRejectReason::None;
        return true;

    const bool indicesInRange =
        coords.tileX0 <= desc.tilesX - 1u && coords.tileY0 <= desc.tilesY - 1u &&
        coords.sliceZ0 <= desc.slicesZ - 1u && coords.tileX1 <= desc.tilesX - 1u &&
        coords.tileY1 <= desc.tilesY - 1u && coords.sliceZ1 <= desc.slicesZ - 1u;
    const bool cornersOrdered =
        coords.tileX0 <= coords.tileX1 && coords.tileY0 <= coords.tileY1 && coords.sliceZ0 <= coords.sliceZ1;
    const bool weightsInRange = coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f &&
                                coords.tz >= 0.f && coords.tz <= 1.f;

    if (indicesInRange && !cornersOrdered && weightsInRange) {
        outReason = SampleCoordRejectReason::InvalidCorners;
    if (isEmptyGrid(desc)) {
        return SampleCoordRejectReason::EmptyGrid;
    outReason = classifySampleCoordReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(outReason);


SampleCoordRejectReason FroxelGridLayout::classifySampleCoordsReject(const FroxelSampleCoords& coords,


        return SampleCoordRejectReason::None;


















bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    return reason != ScreenMappingRejectReason::None;

ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:



        reason = SampleCoordRejectReason::EmptyGrid;










    if (!areSampleCoordsInBounds(coords, desc)) {
        if (indicesInRange) {
            outReason = SampleCoordRejectReason::InvalidWeights;

        outReason = SampleCoordRejectReason::OutOfBounds;

    if (indicesInRange && !weightsInRange) {

            return SampleCoordRejectReason::InvalidWeights;

        return SampleCoordRejectReason::OutOfBounds;






bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordsReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);




bool FroxelGridLayout::preflightFroxelSampleCoords(const FroxelSampleCoords& coords,
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);






                                             const FroxelGridDesc& desc,





bool FroxelGridLayout::tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                                SampleCoordRejectReason& outReason) {


ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    outReason = classifySampleCoordsReject(coords, desc);


bool FroxelGridLayout::preflightScreenMapping(f32 screenX,




        return ScreenMappingRejectReason::EmptyGrid;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return ScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return ScreenMappingRejectReason::DepthOutOfRange;

    return ScreenMappingRejectReason::None;

                                              const FroxelCameraDesc& camera,
                                              ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    return !screenMappingRejectReasonIsBlocking(reject);


ScreenMappingRejectReason FroxelGridLayout::classifyScreenDepthMappingReject(f32 screenX,

bool FroxelGridLayout::preflightFroxelScreenDepth(f32 screenX,

bool FroxelGridLayout::preflightScreenDepthMapping(f32 screenX,
        classifyScreenDepthMappingReject(screenX, screenY, viewDepth, desc, camera);





                                              FroxelSampleCoords* outCoords,






    ScreenMappingRejectReason reject = ScreenMappingRejectReason::None;
    const bool ok = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (outCoords != nullptr) {
        *outCoords = coords;
    return ok;






    const bool mapped =
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (outCoords != nullptr && mapped) {















        }



bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,

                                                                       f32 screenY,
                                                                       f32 viewDepth,
                                                                       const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    return reason;

bool FroxelGridLayout::preflightScreenMappingReady(f32 screenX,



    outReason = classifySampleCoordReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(outReason);






}

                                              f32 screenY,
                                              f32 viewDepth,
    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    const bool ok = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
    if (reason != nullptr) {
        *reason = rejectReason;
}

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);

                                                 f32 screenY,
                                                 f32 viewDepth,
    ScreenMappingRejectReason reject = ScreenMappingRejectReason::None;
    const bool mapped =
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (outCoords != nullptr) {
        *outCoords = coords;
    return mapped && !screenMappingRejectReasonIsBlocking(reject);

bool FroxelGridLayout::tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                                SampleCoordRejectReason& outReason) {
    outReason = SampleCoordRejectReason::UnorderedCorners;

bool FroxelGridLayout::preflightScreenDepthToSampleCoords(f32 screenX,
    const bool ok = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    return ok;










    outReason = classifyFroxelSampleCoordsReject(coords, desc);
        }



    outReason = classifySampleCoordReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(outReason);


ScreenMappingRejectReason FroxelGridLayout::classifyScreenDepthToSampleCoordsReject(
    f32 screenX,



    if (isEmptyGrid(desc)) {
        return ScreenMappingRejectReason::EmptyGrid;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return ScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return ScreenMappingRejectReason::DepthOutOfRange;

    return ScreenMappingRejectReason::None;











    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason)) {
            *reason = rejectReason;

        *reason = ScreenMappingRejectReason::None;
    return true;




    if (reason != nullptr) {
        *reason = reject;

bool FroxelGridLayout::canPreflightSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryPreflightSampleCoords(coords, desc, reason);

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                            const FroxelGridDesc& desc,
                                            SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
    return preflightSampleCoords(coords, desc);

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                        f32 screenY,
                                                                        f32 viewDepth,
                                                                        const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    return reason;

bool FroxelGridLayout::preflightScreenDepthMapping(f32 screenX,
                                                   const FroxelCameraDesc& camera,
                                                   ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    return !screenMappingRejectReasonIsBlocking(reject);
}

bool FroxelGridLayout::wouldSkipSampleCoordPreflight(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return !tryPreflightSampleCoords(coords, desc, reason);

    tryPreflightSampleCoords(coords, desc, reason);





    return preflightSampleCoords(coords, desc);



                                              FroxelSampleCoords* outCoords,
    ScreenMappingRejectReason reject = ScreenMappingRejectReason::None;
    const bool mapped = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (outCoords != nullptr) {
        *outCoords = coords;
    return mapped && !screenMappingRejectReasonIsBlocking(reject);

bool FroxelGridLayout::wouldClampTileCoords(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {

    return tileX >= desc.tilesX || tileY >= desc.tilesY || sliceZ >= desc.slicesZ;

bool FroxelGridLayout::tryPreflightTileCoords(u32 tileX,
                                              u32 tileY,
                                              u32 sliceZ,

    if (wouldClampTileCoords(tileX, tileY, sliceZ, desc)) {

    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        outReason = SampleCoordRejectReason::UnorderedCorners;


bool FroxelGridLayout::canPreflightTileCoords(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return tryPreflightTileCoords(tileX, tileY, sliceZ, desc, reason);

    SampleCoordRejectReason localReason = SampleCoordRejectReason::None;
    const bool ok = tryPreflightSampleCoords(coords, desc, localReason);
        *reason = localReason;
    return ok;

bool FroxelGridLayout::shouldSkipSampleCoordPreflight(const FroxelGridDesc& desc) {
    return isEmptyGrid(desc);

bool FroxelGridLayout::sampleCoordsReady(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return !isEmptyGrid(desc) && isValidSampleCoords(coords, desc) && !wouldClampSampleCoords(coords, desc);

bool FroxelGridLayout::tryNormalizeAndPreflightSampleCoords(FroxelSampleCoords& coords,
    normalizeSampleCoords(coords);
    return tryPreflightSampleCoords(coords, desc, outReason);

const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
        return "none";
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelTrilinearSampleRejectReason::ClampRequired:
        return "clamp_required";
    return "unknown";

bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,

bool FroxelGridLayout::wouldSkipSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

bool FroxelGridLayout::isTrilinearSampleCoordsValid(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (!isValidSampleCoords(coords, desc)) {

    return coords.sliceZ0 <= coords.sliceZ1;

bool FroxelGridLayout::tryPreflightTrilinearSampleCoords(const FroxelSampleCoords& coords,
                                                         TrilinearSampleRejectReason& outReason) {
        outReason = TrilinearSampleRejectReason::EmptyGrid;

        if (!indicesInRange) {
            outReason = TrilinearSampleRejectReason::OutOfBounds;

        outReason = TrilinearSampleRejectReason::InvalidWeights;

    if (coords.sliceZ0 > coords.sliceZ1 || coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1) {
        outReason = TrilinearSampleRejectReason::InvalidSampleCoords;

    outReason = TrilinearSampleRejectReason::None;

bool FroxelGridLayout::wouldClampTrilinearSlice(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    if (coords.sliceZ0 > coords.sliceZ1) {
    if (coords.tz < 0.f || coords.tz > 1.f) {
    if (coords.sliceZ0 > desc.slicesZ - 1u || coords.sliceZ1 > desc.slicesZ - 1u) {





    if (outReason == SampleCoordRejectReason::EmptyGrid ||
        outReason == SampleCoordRejectReason::OutOfBounds) {


    SampleCoordRejectReason reject = SampleCoordRejectReason::None;
    const bool ok = tryPreflightSampleCoords(coords, desc, reject);

bool FroxelGridLayout::isTrilinearSliceOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.sliceZ0 > maxSliceZ || coords.sliceZ1 > maxSliceZ) {
    return coords.tz < 0.f || coords.tz > 1.f;


    return isTrilinearSliceOutOfRange(coords, desc) || coords.sliceZ0 > coords.sliceZ1;





        return ScreenMappingRejectReason::EmptyGrid;
    if (!std::isfinite(screenX) || !std::isfinite(screenY)) {
        return ScreenMappingRejectReason::NonFiniteScreenCoords;
    if (!std::isfinite(viewDepth)) {
        return ScreenMappingRejectReason::NonFiniteDepth;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return ScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return ScreenMappingRejectReason::DepthOutOfRange;
    if (isScreenCoordOutOfRange(screenX, screenY)) {
        return ScreenMappingRejectReason::ScreenCoordsOutOfRange;

    return ScreenMappingRejectReason::None;

bool FroxelGridLayout::preflightScreenDepthToSampleCoords(f32 screenX,
    if (screenMappingRejectReasonIsBlocking(reject)) {

    mapScreenDepthToSampleCoordsImpl(screenX, screenY, viewDepth, desc, camera, coords);

bool FroxelGridLayout::preflightScreenDepthToFroxelIndex(f32 screenX,
                                                         u32* outFroxelIndex,
    if (!preflightScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, &coords, reason)) {

    if (outFroxelIndex != nullptr) {
        *outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);

bool FroxelGridLayout::wouldSkipScreenDepthMapping(f32 screenX,
    return !preflightScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera);


    SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);



    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason)) {
        return screenMappingRejectReasonIsBlocking(reason);

bool FroxelGridLayout::wouldSkipScreenDepthToFroxelIndex(f32 screenX,
    u32 froxelIndex = 0u;
    if (!tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reason)) {



    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason)) {
            *reason = rejectReason;
    return sampleCoordRejectReasonIsBlocking(classifySampleCoordReject(coords, desc));



        *reason = ScreenMappingRejectReason::None;

    if (!tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, rejectReason)) {

        *outFroxelIndex = froxelIndex;












        mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, *outCoords);




    const bool mapped =
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
    return mapped && !screenMappingRejectReasonIsBlocking(rejectReason);

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return reason;
}

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             const FroxelGridDesc& desc,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                       f32 screenY,
                                                                       f32 viewDepth,
                                                                       const FroxelCameraDesc& camera) {

    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);

bool FroxelGridLayout::preflightMapScreenDepthToSampleCoords(f32 screenX,
                                                             const FroxelCameraDesc& camera,
                                                             ScreenMappingRejectReason* reason) {
        return false;

    return !screenMappingRejectReasonIsBlocking(rejectReason);




bool FroxelGridLayout::preflightScreenDepthMapping(f32 screenX,
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    return !screenMappingRejectReasonIsBlocking(reject);


    return sampleCoordRejectReasonIsBlocking(classifyFroxelSampleCoordsReject(coords, desc));





SampleCoordRejectReason FroxelGridLayout::classifyFroxelSampleCoordsReject(const FroxelSampleCoords& coords,

bool FroxelGridLayout::preflightFroxelSampleCoords(const FroxelSampleCoords& coords,
    const SampleCoordRejectReason reject = classifyFroxelSampleCoordsReject(coords, desc);

ScreenMappingRejectReason FroxelGridLayout::classifyFroxelScreenMappingReject(f32 screenX,

bool FroxelGridLayout::preflightFroxelScreenMapping(f32 screenX,
        classifyFroxelScreenMappingReject(screenX, screenY, viewDepth, desc, camera);

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordsReject(const FroxelSampleCoords& coords,

    const SampleCoordRejectReason reject = classifySampleCoordsReject(coords, desc);


bool FroxelGridLayout::preflightScreenMapping(f32 screenX,









    return !preflightSampleCoords(coords, desc);




    const bool ok =
    return true;
    return ok && !screenMappingRejectReasonIsBlocking(rejectReason);

        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, rejectReason);



bool FroxelGridLayout::preflightScreenMappingReady(f32 screenX,

bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,

bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    return reason != ScreenMappingRejectReason::None;

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    case FroxelTrilinearSampleRejectReason::ClampableWeights:

bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason) {
    return reason != GridDensityRejectReason::None;

bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason) {
    return reason != FroxelPopulateRejectReason::None;

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:




        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (mapped && outCoords != nullptr) {

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingFroxelIndexReject(f32 screenX,
    tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reason);

bool FroxelGridLayout::preflightMapScreenDepthToFroxelIndex(f32 screenX,
        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reject);
    if (mapped && outFroxelIndex != nullptr) {






















    return preflightScreenMapping(screenX, screenY, viewDepth, desc, camera, reason);


    if (isEmptyGrid(desc)) {



        mapScreenDepthToSampleCoordsImpl(screenX, screenY, viewDepth, desc, camera, *outCoords);






    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (reason != nullptr) {
        *reason = reject;
    }
    if (screenMappingRejectReasonIsBlocking(reject)) {
        return false;
    }

    if (outCoords != nullptr) {
        FroxelSampleCoords coords{};
        ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, mapReason);
        *outCoords = coords;
    }
    return true;

bool FroxelGridLayout::preflightScreenDepthToFroxelIndex(f32 screenX,
                                                         f32 screenY,
                                                         f32 viewDepth,
                                                         const FroxelGridDesc& desc,
                                                         const FroxelCameraDesc& camera,
                                                         u32* outFroxelIndex,
                                                         ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (reason != nullptr) {
        *reason = reject;
    if (screenMappingRejectReasonIsBlocking(reject)) {
        return false;

    if (outFroxelIndex != nullptr) {
        u32 index = 0u;
        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, index, mapReason);
        *outFroxelIndex = index;

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                  const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;

bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,
                                                  SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(reject);

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                        const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords unused{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, unused, reason);

bool FroxelGridLayout::preflightScreenMappingReady(f32 screenX,
    return !screenMappingRejectReasonIsBlocking(reject);
    mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords);
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                        f32 screenY,
                                                                        f32 viewDepth,
                                                                        const FroxelGridDesc& desc,
                                                                        const FroxelCameraDesc& camera) {
    FroxelSampleCoords coords{};
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    return reason;
}

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             const FroxelGridDesc& desc,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

bool FroxelGridLayout::preflightScreenMapping(f32 screenX,
                                              f32 screenY,
                                              f32 viewDepth,
                                              const FroxelGridDesc& desc,
                                              const FroxelCameraDesc& camera,
                                              FroxelSampleCoords* outCoords,
                                              ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (reason != nullptr) {
        *reason = reject;
    }
    const bool ok = !screenMappingRejectReasonIsBlocking(reject);
    if (ok && outCoords != nullptr) {
        FroxelSampleCoords coords{};
        ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, mapReason);
        *outCoords = coords;
    }
    return ok;
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,
                                                  const FroxelGridDesc& desc,
                                                  SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                        f32 screenY,
                                                                        f32 viewDepth,
                                                                        const FroxelGridDesc& desc,
                                                                        const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    return reason;
}

bool FroxelGridLayout::preflightScreenMappingReady(f32 screenX,
                                                   f32 screenY,
                                                   f32 viewDepth,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelCameraDesc& camera,
                                                   ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !screenMappingRejectReasonIsBlocking(reject);
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             const FroxelGridDesc& desc,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                        f32 screenY,
                                                                        f32 viewDepth,
                                                                        const FroxelGridDesc& desc,
                                                                        const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    return reason;
}

bool FroxelGridLayout::preflightScreenMapping(f32 screenX,
                                              f32 screenY,
                                              f32 viewDepth,
                                              const FroxelGridDesc& desc,
                                              const FroxelCameraDesc& camera,
                                              FroxelSampleCoords* outCoords,
                                              ScreenMappingRejectReason* reason) {
    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    const bool mapped =
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
    if (outCoords != nullptr) {
        *outCoords = coords;
    }
    if (reason != nullptr) {
        *reason = rejectReason;
    }
    return mapped;
}

bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    switch (reason) {
    case ScreenMappingRejectReason::None:
        return false;
    case ScreenMappingRejectReason::EmptyGrid:
    case ScreenMappingRejectReason::InvalidCamera:
    case ScreenMappingRejectReason::DepthOutOfRange:
        return true;
    }
    return true;
}

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,
                                                  const FroxelGridDesc& desc,
                                                  SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

const char* screenMappingRejectReasonLabel(ScreenMappingRejectReason reason) {
    switch (reason) {
    case ScreenMappingRejectReason::None:
        return "none";
    case ScreenMappingRejectReason::EmptyGrid:
        return "empty_grid";
    case ScreenMappingRejectReason::InvalidCamera:
        return "invalid_camera";
    case ScreenMappingRejectReason::DepthOutOfRange:
        return "depth_out_of_range";
    return "unknown";

bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    return reason != ScreenMappingRejectReason::None;

const char* froxelSampleCoordsRejectReasonLabel(FroxelSampleCoordsRejectReason reason) {
    switch (reason) {
    case FroxelSampleCoordsRejectReason::None:
        return "none";
    case FroxelSampleCoordsRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelSampleCoordsRejectReason::OutOfRangeIndices:
        return "out_of_range_indices";
    case FroxelSampleCoordsRejectReason::OutOfRangeWeights:
        return "out_of_range_weights";
    case FroxelSampleCoordsRejectReason::UnorderedCorners:
        return "unordered_corners";
    case ScreenMappingRejectReason::ScreenCoordsOutOfRange:
        return "screen_coords_out_of_range";
    case ScreenMappingRejectReason::NonFiniteScreenCoords:
        return "non_finite_screen_coords";
    case ScreenMappingRejectReason::NonFiniteDepth:
        return "non_finite_depth";
    }
    return "unknown";
}

ScreenMappingRejectReason classifyScreenMappingReject(f32 /*screenX*/,
                                                     f32 /*screenY*/,
                                                     f32 viewDepth,
                                                     const FroxelGridDesc& desc,
                                                     const FroxelCameraDesc& camera) {
bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    return reason != ScreenMappingRejectReason::None;
}

    if (FroxelGridLayout::isEmptyGrid(desc)) {
        return ScreenMappingRejectReason::EmptyGrid;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return ScreenMappingRejectReason::InvalidCamera;
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return ScreenMappingRejectReason::DepthOutOfRange;
    return ScreenMappingRejectReason::None;

bool preflightScreenDepthMapping(f32 screenX,
                                 f32 screenY,
bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    return reason != ScreenMappingRejectReason::None;

ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,
                                                      f32 viewDepth,
                                                      const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    return reason;

bool preflightScreenMappingReady(f32 screenX,
}

                                 f32 screenY,
                                 f32 viewDepth,
                                 const FroxelGridDesc& desc,
                                 const FroxelCameraDesc& camera,
                                 ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (reason != nullptr) {
        *reason = reject;
    return reject == ScreenMappingRejectReason::None;




ScreenMappingRejectReason classifyScreenDepthMappingReject(f32 screenX,



bool preflightScreenMapping(f32 screenX,
    return !screenMappingRejectReasonIsBlocking(reject);


bool wouldSkipScreenMapping(f32 screenX,
    return screenMappingRejectReasonIsBlocking(
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera));


    switch (reason) {
    case ScreenMappingRejectReason::None:
        return false;
    case ScreenMappingRejectReason::EmptyGrid:
    case ScreenMappingRejectReason::InvalidCamera:
    case ScreenMappingRejectReason::DepthOutOfRange:
        return true;

bool preflightScreenDepthToSampleCoords(f32 screenX,
                                        FroxelSampleCoords* out_coords,
    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    const bool ok = FroxelGridLayout::tryMapScreenDepthToSampleCoords(
        screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
    if (out_coords != nullptr) {
        *out_coords = coords;
        *reason = rejectReason;
    return ok && !screenMappingRejectReasonIsBlocking(rejectReason);

bool preflightScreenDepthToFroxelIndex(f32 screenX,
                                       u32* out_froxel_index,
    u32 froxelIndex = 0u;
    const bool ok = FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
        screenX, screenY, viewDepth, desc, camera, froxelIndex, rejectReason);
    if (out_froxel_index != nullptr) {
        *out_froxel_index = froxelIndex;

    const ScreenMappingRejectReason reject = classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);


bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
    }
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case ScreenMappingRejectReason::None:
    case ScreenMappingRejectReason::ScreenCoordsOutOfRange:
        return false;
    case ScreenMappingRejectReason::EmptyGrid:
    case ScreenMappingRejectReason::InvalidCamera:
    case ScreenMappingRejectReason::DepthOutOfRange:
    case ScreenMappingRejectReason::NonFiniteScreenCoords:
    case ScreenMappingRejectReason::NonFiniteDepth:
        return true;
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::UnorderedCorners:
        return "unordered_corners";
    case SampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
    case SampleCoordRejectReason::InvalidCorners:
        return "invalid_corners";
    case SampleCoordRejectReason::InvalidWeights:
        return "invalid_weights";

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    FroxelSampleRejectReason reason = FroxelSampleRejectReason::None;

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      u32& outFroxelIndex,
                                                      FroxelSampleRejectReason& outReason) {
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    outReason = FroxelSampleRejectReason::None;
    FroxelSampleCoordRejectReason reason = FroxelSampleCoordRejectReason::None;
    case SampleCoordRejectReason::UnorderedCorners:
        return "unordered_corners";
    }
    return "unknown";

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

bool preflightSampleCoords(const FroxelSampleCoords& coords,
                           const FroxelGridDesc& desc,
                           SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

const char* froxelBilinearSampleRejectReasonLabel(FroxelBilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelBilinearSampleRejectReason::None:
        return "none";
    case FroxelBilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelBilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelBilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelBilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    }
    return "unknown";
}

bool froxelBilinearSampleRejectReasonIsBlocking(FroxelBilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelBilinearSampleRejectReason::None:
    case FroxelBilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelBilinearSampleRejectReason::EmptyGrid:
    case FroxelBilinearSampleRejectReason::InaccessibleGrid:
    case FroxelBilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        return SampleCoordRejectReason::EmptyGrid;
    }

    if (FroxelGridLayout::isValidSampleCoords(coords, desc)) {
        return SampleCoordRejectReason::None;
    }

    if (!FroxelGridLayout::areSampleCoordsInBounds(coords, desc)) {
        const bool indicesInRange =
            coords.tileX0 <= desc.tilesX - 1u && coords.tileY0 <= desc.tilesY - 1u &&
            coords.sliceZ0 <= desc.slicesZ - 1u && coords.tileX1 <= desc.tilesX - 1u &&
            coords.tileY1 <= desc.tilesY - 1u && coords.sliceZ1 <= desc.slicesZ - 1u;
        if (indicesInRange) {
            return SampleCoordRejectReason::InvalidWeights;
        }

        return SampleCoordRejectReason::OutOfBounds;
    }

    return SampleCoordRejectReason::OutOfBounds;
}

bool preflightSampleCoordsReady(const FroxelSampleCoords& coords,
                                const FroxelGridDesc& desc,
                                SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);
    return reason;
}

bool preflightSampleCoordsReady(const FroxelSampleCoords& coords,
                                const FroxelGridDesc& desc,
                                SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
        return "none";
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelTrilinearSampleRejectReason::ScreenMappingFailed:
        return "screen_mapping_failed";
    }
    return "unknown";
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return reason == SampleCoordRejectReason::EmptyGrid || reason == SampleCoordRejectReason::OutOfBounds;

const char* froxelBilinearSampleRejectReasonLabel(FroxelBilinearSampleRejectReason reason) {
    case FroxelBilinearSampleRejectReason::None:
        return "none";
    case FroxelBilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelBilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelBilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelBilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    return "unknown";

bool froxelBilinearSampleRejectReasonIsBlocking(FroxelBilinearSampleRejectReason reason) {
    return reason == FroxelBilinearSampleRejectReason::EmptyGrid ||
           reason == FroxelBilinearSampleRejectReason::InaccessibleGrid ||
           reason == FroxelBilinearSampleRejectReason::InvalidSampleCoords;
    return true;
    }
}

const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
        return "none";
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelTrilinearSampleRejectReason::NotAccessible:
        return "not_accessible";
    case FroxelTrilinearSampleRejectReason::DescMismatch:
        return "desc_mismatch";
    case FroxelTrilinearSampleRejectReason::EmptyStorage:
        return "empty_storage";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelTrilinearSampleRejectReason::HardOutOfBounds:
        return "hard_out_of_bounds";
    }
    return "unknown";
const char* trilinearSampleRejectReasonLabel(TrilinearSampleRejectReason reason) {
    case TrilinearSampleRejectReason::None:
    case TrilinearSampleRejectReason::EmptyGrid:
    case TrilinearSampleRejectReason::EmptyStorage:
        return "empty_storage";
    case TrilinearSampleRejectReason::DescMismatch:
        return "desc_mismatch";
    case TrilinearSampleRejectReason::InvalidSampleCoords:
    case TrilinearSampleRejectReason::OutOfBounds:
        return "out_of_bounds";
    case TrilinearSampleRejectReason::InvalidWeights:
        return "invalid_weights";
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelTrilinearSampleRejectReason::InvalidWeights:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    case FroxelTrilinearSampleRejectReason::GridInaccessible:
        return "grid_inaccessible";
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::InvalidWeights:
        return false;
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return true;
    }
    return true;
}

const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
        return "none";
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    case FroxelTrilinearSampleRejectReason::ScreenMappingFailed:
        return "screen_mapping_failed";
    }
    return "unknown";

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
        return false;
        return true;

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    return reason == FroxelTrilinearSampleRejectReason::EmptyGrid ||
           reason == FroxelTrilinearSampleRejectReason::InaccessibleGrid ||
           reason == FroxelTrilinearSampleRejectReason::InvalidSampleCoords;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

const char* froxelBilinearSampleRejectReasonLabel(FroxelBilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelBilinearSampleRejectReason::None:
        return "none";
    case FroxelBilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelBilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelBilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelBilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    }
    return "unknown";
}

const char* froxelScreenSampleRejectReasonLabel(FroxelScreenSampleRejectReason reason) {
bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
const char* froxelBilinearSampleRejectReasonLabel(FroxelBilinearSampleRejectReason reason) {
    case FroxelBilinearSampleRejectReason::None:
        return "none";
    case FroxelBilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelBilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelBilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelBilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    return "unknown";
bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason) {
    case GridDensityRejectReason::None:
    case GridDensityRejectReason::EmptyDesc:
    case GridDensityRejectReason::UndersizedStorage:
    case GridDensityRejectReason::DescMismatch:
    case GridDensityRejectReason::DensityCountMismatch:
    return true;
}

const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason) {
    switch (reason) {
    case FroxelScreenSampleRejectReason::None:
        return "none";
    case FroxelScreenSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelScreenSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelScreenSampleRejectReason::ScreenMappingFailed:
        return "screen_mapping_failed";
    case FroxelScreenSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelScreenSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    }
    return "unknown";
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return false;
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return true;
    }
    return true;
}

const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason) {
    case GridDensityRejectReason::None:
    case GridDensityRejectReason::EmptyDesc:
        return "empty_desc";
    case GridDensityRejectReason::EmptyGrid:
        return "empty_grid";
    case GridDensityRejectReason::DescMismatch:
        return "desc_mismatch";
    case GridDensityRejectReason::UndersizedStorage:
        return "undersized_storage";
    case GridDensityRejectReason::DescMismatch:
        return "desc_mismatch";
    case GridDensityRejectReason::DensityCountMismatch:
        return "density_count_mismatch";
const char* densityGridRejectReasonLabel(DensityGridRejectReason reason) {
    case DensityGridRejectReason::None:
    case DensityGridRejectReason::EmptyGridDesc:
        return "empty_grid_desc";
    case DensityGridRejectReason::DescMismatch:
    case DensityGridRejectReason::CountPartitionMismatch:
        return "count_partition_mismatch";
const char* froxelDensityRejectReasonLabel(FroxelDensityRejectReason reason) {
    case FroxelDensityRejectReason::None:
    case FroxelDensityRejectReason::EmptyStorage:
        return "empty_storage";
    case FroxelDensityRejectReason::DescMismatch:
    case GridDensityRejectReason::NonFiniteDensity:
        return "non_finite_density";
    }
    return "unknown";
}

bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason) {
const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::OutOfRangeTile:
        return "out_of_range_tile";
    case SampleCoordRejectReason::OutOfRangeWeight:
        return "out_of_range_weight";
    case SampleCoordRejectReason::DepthOutOfRange:
        return "depth_out_of_range";
    case SampleCoordRejectReason::InvalidCamera:
        return "invalid_camera";
    }
    return "unknown";
const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::NotSampleable:
        return "not_sampleable";
    case FroxelTrilinearSampleRejectReason::EmptyStorage:
        return "empty_storage";
    case FroxelTrilinearSampleRejectReason::UndersizedStorage:
        return "undersized_storage";
    case FroxelTrilinearSampleRejectReason::DescMismatch:
        return "desc_mismatch";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    return reason != GridDensityRejectReason::None;
    case GridDensityRejectReason::None:
    case GridDensityRejectReason::EmptyDesc:
        return false;
    case GridDensityRejectReason::UndersizedStorage:
    case GridDensityRejectReason::DescMismatch:
    case GridDensityRejectReason::DensityCountMismatch:
        return true;
    return reason == GridDensityRejectReason::UndersizedStorage ||
           reason == GridDensityRejectReason::DescMismatch ||
           reason == GridDensityRejectReason::DensityCountMismatch;
bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  const FroxelGridDesc& desc,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    froxel_util::tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;

bool preflightGridDensityReady(const FroxelDensityGrid& grid,
                               GridDensityRejectReason* reason,
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    return !gridDensityRejectReasonIsBlocking(reject);

const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason) {
    switch (reason) {
    case GridDensityRejectReason::None:
    case GridDensityRejectReason::EmptyDesc:
        return false;
    case GridDensityRejectReason::UndersizedStorage:
    case GridDensityRejectReason::DescMismatch:
    case GridDensityRejectReason::DensityCountMismatch:
        return true;
    }

const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason) {
    case DensityLookupRejectReason::None:
        return "none";
    case DensityLookupRejectReason::EmptyGrid:
        return "empty_grid";
    case DensityLookupRejectReason::DescMismatch:
        return "desc_mismatch";
    case DensityLookupRejectReason::EmptyStorage:
        return "empty_storage";
    case DensityLookupRejectReason::IndexOutOfRange:
        return "index_out_of_range";
    return "unknown";

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    switch (reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
        return false;
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:
        return true;
    }
    return true;
}

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    switch (reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
        return false;
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:
        return true;
    }
    return true;
}

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    switch (reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
        return false;
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:
        return true;
    }
    return true;
}

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    switch (reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
        return false;
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:
        return true;
    }
    return true;
}

const char* froxelCameraRejectReasonLabel(FroxelCameraRejectReason reason) {
    switch (reason) {
    case FroxelCameraRejectReason::None:
        return "none";
    case FroxelCameraRejectReason::InvalidPlanes:
        return "invalid_planes";
    case FroxelCameraRejectReason::ZeroScreenDimensions:
        return "zero_screen_dimensions";
    }
    return "unknown";
}

bool froxelCameraRejectReasonIsBlocking(FroxelCameraRejectReason reason) {
    return reason != FroxelCameraRejectReason::None;
}

bool tryValidateFroxelCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason) {
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = FroxelCameraRejectReason::InvalidPlanes;
        return false;
    if (camera.screenWidth == 0u || camera.screenHeight == 0u) {
        outReason = FroxelCameraRejectReason::ZeroScreenDimensions;

    outReason = FroxelCameraRejectReason::None;
    return true;

bool isValidFroxelCamera(const FroxelCameraDesc& camera) {
    FroxelCameraRejectReason reason = FroxelCameraRejectReason::None;
    return tryValidateFroxelCamera(camera, reason);

const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    switch (reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:

    return reason == DensityLookupRejectReason::EmptyGrid ||
           reason == DensityLookupRejectReason::DescMismatch ||
           reason == DensityLookupRejectReason::EmptyStorage;

    return reason != DensityLookupRejectReason::None &&
           reason != DensityLookupRejectReason::IndexOutOfRange;

bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason) {
    return reason != FroxelPopulateRejectReason::None;

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    froxel_util::tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                               u32 tileX,
                                                               u32 tileY,
                                                               u32 sliceZ) {
    froxel_util::tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool preflightDensityLookupReady(const FroxelDensityGrid& grid,
                                 u32 index,
                                 DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return !densityLookupRejectReasonIsBlocking(reject);

bool preflightDensityLookupAtCoordReady(const FroxelDensityGrid& grid,
                                        u32 sliceZ,
    const DensityLookupRejectReason reject =
        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool preflightTrilinearSampleReady(const FroxelDensityGrid& grid,
                                   const FroxelSampleCoords& coords,
                                   FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
    case FroxelPopulateRejectReason::None:
    case FroxelPopulateRejectReason::EmptyDesc:
    case FroxelPopulateRejectReason::InvalidCamera:
    case FroxelPopulateRejectReason::ZeroDensity:
    case FroxelPopulateRejectReason::ZeroMarchSteps:
        return false;
        return true;
    }

const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
        return "none";
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelTrilinearSampleRejectReason::EmptyStorage:
        return "empty_storage";
    case FroxelTrilinearSampleRejectReason::DescMismatch:
        return "desc_mismatch";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    }
    return "unknown";
}

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
    return reason != FroxelTrilinearSampleRejectReason::None;
}

const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason) {
    case FroxelPopulateRejectReason::None:
    case FroxelPopulateRejectReason::EmptyDesc:
        return "empty_desc";
    }

    switch (reason) {
        return "none";
    case FroxelPopulateRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelPopulateRejectReason::InvalidCamera:
        return "invalid_camera";
    case FroxelPopulateRejectReason::ZeroDensity:
        return "zero_density";
    case FroxelPopulateRejectReason::ZeroMarchSteps:
        return "zero_march_steps";

bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason) {
    return reason != FroxelPopulateRejectReason::None;
const char* froxelLookupRejectReasonLabel(FroxelLookupRejectReason reason) {
    case FroxelLookupRejectReason::None:
    case FroxelLookupRejectReason::EmptyGrid:
    case FroxelLookupRejectReason::DescMismatch:
    case FroxelLookupRejectReason::EmptyStorage:

const char* froxelSampleRejectReasonLabel(FroxelSampleRejectReason reason) {
    case FroxelSampleRejectReason::None:
    case FroxelSampleRejectReason::EmptyGrid:
    case FroxelSampleRejectReason::DepthOutOfRange:
        return "depth_out_of_range";
    case FroxelSampleRejectReason::InvalidCamera:
    case FroxelSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::DepthOutOfRange:

const char* froxelSampleCoordRejectReasonLabel(FroxelSampleCoordRejectReason reason) {
    case FroxelSampleCoordRejectReason::None:
    case FroxelSampleCoordRejectReason::EmptyGrid:
    case FroxelSampleCoordRejectReason::DepthOutOfRange:
    case FroxelSampleCoordRejectReason::InvalidCamera:

    case SampleCoordRejectReason::OutOfRange:
        return "out_of_range";
    case DensityLookupRejectReason::SampleCoordsOutOfRange:
        return "sample_coords_out_of_range";


    case SampleCoordRejectReason::InvertedCorners:
        return "inverted_corners";

    case SampleCoordRejectReason::DepthBelowNear:
        return "depth_below_near";
    case SampleCoordRejectReason::DepthAboveFar:
        return "depth_above_far";
    case SampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
    case DensityLookupRejectReason::SampleCoordRejected:
        return "sample_coord_rejected";
    case DensityLookupRejectReason::ScreenMappingFailed:
        return "screen_mapping_failed";

    case DensityLookupRejectReason::CoordOutOfRange:
        return "coord_out_of_range";

const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
    case FroxelTrilinearSampleRejectReason::None:
    case FroxelTrilinearSampleRejectReason::LookupFailed:
        return "lookup_failed";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";

    case FroxelTrilinearSampleRejectReason::EmptyGrid:
    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
    case FroxelTrilinearSampleRejectReason::OutOfBoundsCoords:
        return "out_of_bounds_coords";

    case FroxelTrilinearSampleRejectReason::ScreenMappingFailed:

    case FroxelTrilinearSampleRejectReason::DescMismatch:
        return "desc_mismatch";
    case FroxelTrilinearSampleRejectReason::EmptyStorage:
        return "empty_storage";
    case FroxelTrilinearSampleRejectReason::CoordsOutOfRange:
        return "coords_out_of_range";
    case FroxelTrilinearSampleRejectReason::InvalidWeights:
        return "invalid_weights";

const char* densityTrilinearSampleRejectReasonLabel(DensityTrilinearSampleRejectReason reason) {
    case DensityTrilinearSampleRejectReason::None:
    case DensityTrilinearSampleRejectReason::EmptyGrid:
    case DensityTrilinearSampleRejectReason::DescMismatch:
    case DensityTrilinearSampleRejectReason::EmptyStorage:
    case DensityTrilinearSampleRejectReason::InvalidSampleCoords:
    case DensityTrilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";

    case FroxelTrilinearSampleRejectReason::NotSampleable:
        return "not_sampleable";
    }
    return "unknown";
}

bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason) {
    return reason != FroxelPopulateRejectReason::None;
}

const char* froxelTrilinearSampleRejectReasonLabel(FroxelTrilinearSampleRejectReason reason) {
    switch (reason) {
    case FroxelTrilinearSampleRejectReason::None:
        return "none";
    case FroxelTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelTrilinearSampleRejectReason::NotAccessible:
        return "not_accessible";
    case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case FroxelTrilinearSampleRejectReason::HardOutOfBounds:
        return "hard_out_of_bounds";
    return "unknown";

bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason) {
    case DensityLookupRejectReason::None:
    case DensityLookupRejectReason::IndexOutOfRange:
        return false;
    case DensityLookupRejectReason::EmptyGrid:
    case DensityLookupRejectReason::DescMismatch:
    case DensityLookupRejectReason::EmptyStorage:
        return true;

    case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
        return "inaccessible_grid";
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {

const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason) {
    case FroxelPopulateRejectReason::None:
    case FroxelPopulateRejectReason::EmptyGrid:
    case FroxelPopulateRejectReason::ZeroDensity:
        return "zero_density";
    case FroxelPopulateRejectReason::ZeroMarchSteps:
        return "zero_march_steps";
    case FroxelPopulateRejectReason::InvalidCamera:
        return "invalid_camera";
const char* populateRejectReasonLabel(PopulateRejectReason reason) {
    case PopulateRejectReason::None:
    case PopulateRejectReason::EmptyGrid:
    case PopulateRejectReason::InvalidCamera:
    case PopulateRejectReason::ZeroDensity:
    case PopulateRejectReason::ZeroMarchSteps:

bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                               const FroxelGridDesc& desc,
                                               SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;

    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        outReason = SampleCoordRejectReason::UnorderedCorners;

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ ||
        coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
        outReason = SampleCoordRejectReason::OutOfBounds;

    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = SampleCoordRejectReason::InvalidWeights;

    outReason = SampleCoordRejectReason::None;
    case FroxelTrilinearSampleRejectReason::UndersizedStorage:
        return "undersized_storage";

DensityLookupRejectReason classifyFroxelDensityLookupReject(const FroxelDensityGrid& grid,
                                                            u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    froxel_util::tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;

SampleCoordRejectReason classifyFroxelSampleCoordReject(const FroxelSampleCoords& coords,
                                                        const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

ScreenMappingRejectReason classifyFroxelScreenMappingReject(f32 screenX,
                                                              f32 screenY,
                                                              f32 viewDepth,
                                                              const FroxelCameraDesc& camera) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    return FroxelPopulateRejectReason::None;

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
}

                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    return reject == FroxelPopulateRejectReason::None;

bool tryPreflightFroxelPopulate(const FroxelGridDesc& desc,
                                FroxelPopulateRejectReason& reason) {
    return preflightFroxelPopulate(desc, camera, params, &reason);

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        return DensityLookupRejectReason::EmptyGrid;
    if (grid.isEmpty()) {
        return DensityLookupRejectReason::EmptyStorage;
    if (!froxel_util::gridMatchesDesc(grid, desc)) {
        return DensityLookupRejectReason::DescMismatch;
    if (FroxelGridLayout::isFroxelIndexOutOfRange(index, desc)) {
        return DensityLookupRejectReason::IndexOutOfRange;
    return DensityLookupRejectReason::None;

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           u32 tileX,
                                                           u32 tileY,
                                                           u32 sliceZ) {
    const DensityLookupRejectReason baseReject = classifyDensityLookupReject(grid, desc, 0u);
    if (baseReject != DensityLookupRejectReason::None &&
        baseReject != DensityLookupRejectReason::IndexOutOfRange) {
        return baseReject;
    if (FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc)) {

    return reason != FroxelTrilinearSampleRejectReason::None;
SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
        return SampleCoordRejectReason::EmptyGrid;

    if (isValidSampleCoords(coords, desc)) {
        return SampleCoordRejectReason::None;

    if (!areSampleCoordsInBounds(coords, desc)) {
namespace {

SampleCoordRejectReason classifySampleCoordsRejectImpl(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    if (FroxelGridLayout::isValidSampleCoords(coords, desc)) {

    if (!FroxelGridLayout::areSampleCoordsInBounds(coords, desc)) {
SampleCoordRejectReason FroxelGridLayout::classifyFroxelSampleCoordsReject(const FroxelSampleCoords& coords,


        const bool indicesInRange =
            coords.tileX0 <= desc.tilesX - 1u && coords.tileY0 <= desc.tilesY - 1u &&
            coords.sliceZ0 <= desc.slicesZ - 1u && coords.tileX1 <= desc.tilesX - 1u &&
            coords.tileY1 <= desc.tilesY - 1u && coords.sliceZ1 <= desc.slicesZ - 1u;
        if (indicesInRange) {
            return SampleCoordRejectReason::InvalidWeights;

        return SampleCoordRejectReason::OutOfBounds;


bool FroxelGridLayout::tryPreflightSampleCoords(const FroxelSampleCoords& coords,
    outReason = classifySampleCoordReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(outReason);

bool FroxelGridLayout::canPreflightSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return tryPreflightSampleCoords(coords, desc, reason);

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);

bool FroxelGridLayout::preflightScreenDepthToSampleCoords(f32 screenX,
                                                          FroxelSampleCoords* outCoords,
                                                          ScreenMappingRejectReason* reason) {
    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    const bool ok =
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
    if (outCoords != nullptr) {
        *outCoords = coords;
        *reason = rejectReason;
    return ok;

bool FroxelGridLayout::preflightScreenDepthToFroxelIndex(f32 screenX,
                                                         u32* outFroxelIndex,
    u32 froxelIndex = 0u;
        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, rejectReason);
    if (outFroxelIndex != nullptr) {
        *outFroxelIndex = froxelIndex;
    tryPreflightSampleCoords(coords, desc, reason);

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(reject);


bool FroxelGridLayout::preflightScreenDepthMapping(f32 screenX,
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    return !screenMappingRejectReasonIsBlocking(reject);

bool FroxelGridLayout::wouldSkipScreenMapping(f32 screenX,
    return !preflightScreenDepthMapping(screenX, screenY, viewDepth, desc, camera);



bool FroxelGridLayout::preflightScreenMapping(f32 screenX,




SampleCoordRejectReason FroxelGridLayout::classifyFroxelSampleCoordReject(const FroxelSampleCoords& coords,

    const SampleCoordRejectReason reject = classifyFroxelSampleCoordReject(coords, desc);





SampleCoordRejectReason FroxelGridLayout::classifySampleCoordsReject(const FroxelSampleCoords& coords,


    const SampleCoordRejectReason reject = classifySampleCoordsReject(coords, desc);




} // namespace

    return classifySampleCoordsRejectImpl(coords, desc);


    outReason = classifySampleCoordsReject(coords, desc);


bool FroxelGridLayout::wouldSkipSampleCoordPreflight(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return !tryPreflightSampleCoords(coords, desc, reason);








ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,

bool preflightScreenMappingReady(f32 screenX,


bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,



    if (reject == ScreenMappingRejectReason::None) {
        mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords);


    ScreenMappingRejectReason reject = classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
        mapScreenDepthToSampleCoordsImpl(screenX, screenY, viewDepth, desc, camera, coords);




ScreenMappingRejectReason FroxelGridLayout::classifyScreenDepthMappingReject(f32 screenX,

bool FroxelGridLayout::preflightFroxelSampleCoords(const FroxelSampleCoords& coords,
    const SampleCoordRejectReason reject = classifyFroxelSampleCoordsReject(coords, desc);




    SampleCoordRejectReason rejectReason = SampleCoordRejectReason::None;
    const bool ok = tryPreflightSampleCoords(coords, desc, rejectReason);
    return ok && !sampleCoordRejectReasonIsBlocking(rejectReason);



    if (outCoords != nullptr && !screenMappingRejectReasonIsBlocking(reject)) {
        ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, mapReason);

    const bool mapped = preflightScreenDepthToSampleCoords(
        screenX, screenY, viewDepth, desc, camera, &coords, reason);
    if (mapped && outFroxelIndex != nullptr) {
        *outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    return mapped;


    FroxelSampleCoords unused{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, unused, reason);

    ScreenMappingRejectReason reject = ScreenMappingRejectReason::None;
    const bool mapped =
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    return mapped && !screenMappingRejectReasonIsBlocking(reject);

        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reject);

    return !froxelPopulateRejectReasonIsBlocking(reject);




    if (!tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reject)) {
            *outFroxelIndex = 0u;

        *reason = ScreenMappingRejectReason::None;
    FroxelSampleCoords dummy{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, dummy, reason);

    if (outCoords != nullptr && mapped) {





    if (screenMappingRejectReasonIsBlocking(reject)) {


    if (!preflightScreenMapping(screenX, screenY, viewDepth, desc, camera, &coords, reason)) {


bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,





namespace froxel_util {

f32 lerpDensity(f32 a, f32 b, f32 t) {
    if (a == b) {
        return a;
    }
    return a + (b - a) * clamp01(t);
}

bool gridMatchesDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (FroxelGridLayout::isEmptyGrid(clampedDesc)) {
        return grid.density.empty();
    }
    return grid.density.size() == clampedDesc.froxelCount();
}

bool canAccessDensityGrid(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    if (grid.isEmpty() || FroxelGridLayout::isEmptyGrid(desc)) {
        return false;
    }
    return gridMatchesDesc(grid, desc);
}

bool isDensityGridAccessible(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !grid.isEmpty() && !FroxelGridLayout::isEmptyGrid(desc) && grid.matchesDesc(desc);
}

bool shouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !isDensityGridAccessible(grid, desc);
}

bool wouldSkipFroxelDensityLookup(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyFroxelDensityLookupReject(grid, desc, 0u);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject != DensityLookupRejectReason::None;
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return !tryCanLookupAtIndex(grid, desc, 0u, reason);

bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ) {
    return !tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
bool shouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return shouldSkipFroxelLookup(grid, desc);

bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   u32 index,
                                   DensityLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc, index, outReason);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason);
bool wouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return wouldSkipDensityLookup(grid, desc);
}

bool shouldSkipFroxelGrid(const FroxelGridDesc& desc) {
    return FroxelGridLayout::isEmptyGrid(desc);

bool tryValidateFroxelGridDesc(const FroxelGridDesc& desc, FroxelGridRejectReason& outReason) {
    if (desc.tilesX == 0u) {
        outReason = FroxelGridRejectReason::EmptyTilesX;
        return false;
    }
    if (desc.tilesY == 0u) {
        outReason = FroxelGridRejectReason::EmptyTilesY;
        return false;
    }
    if (desc.slicesZ == 0u) {
        outReason = FroxelGridRejectReason::EmptySlicesZ;
        return false;
    }

    outReason = FroxelGridRejectReason::None;
    return true;
}

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              const VolumetricFogParams& params) {
    if (shouldSkipFroxelGrid(desc)) {
        return true;
    }
    if (!FroxelSliceLayout::isCameraValid(camera)) {
        return true;
    }
    return params.density <= 0.f || params.march_steps == 0u;
}

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    return shouldSkipFroxelGrid(desc) || params.density <= 0.f || params.march_steps == 0u;
}

bool canAllocateFroxelGrid(const FroxelGridDesc& desc) {
    return FroxelGridDesc::clampCounts(desc).froxelCount() > 0u;
}

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc) {
    return !canAllocateFroxelGrid(desc);
}

bool canSampleFroxelGrid(const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc);
}

u32 requiredFroxelCount(const FroxelGridDesc& desc) {
    if (!canSampleFroxelGrid(desc)) {
        return 0u;
    }
    return FroxelGridDesc::clampCounts(desc).froxelCount();
}

bool isDensitySizedForGrid(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    const u32 required = requiredFroxelCount(desc);
    if (required == 0u) {
        return true;
    }
    return grid.density.size() >= required;
}

u32 densityEntriesMissing(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    const u32 required = requiredFroxelCount(desc);
    if (required == 0u || grid.density.size() >= required) {
        return 0u;
    }
    return required - static_cast<u32>(grid.density.size());
}

bool wouldSkipFroxelGrid(const FroxelGridDesc& desc) {
    return shouldSkipFroxelGrid(desc);
}

bool wouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return shouldSkipFroxelLookup(grid, desc);
}

bool wouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    return shouldSkipFroxelMarch(grid, desc, epsilon);
}

bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params) {
    return shouldSkipFroxelPopulate(desc, camera, params);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 tileX,
                                                      u32 tileY,
                                                      u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject =
        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 tileX,
                                                      u32 tileY,
                                                      u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

SampleCoordRejectReason classifyFroxelSampleCoordReject(const FroxelDensityGrid& grid,
                                                        const FroxelGridDesc& desc,
                                                        const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryCanSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightDensitySampleAtCoords(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleCoordReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                        const FroxelGridDesc& desc,
                                                                        const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,
                                     const FroxelGridDesc& desc,
                                     const FroxelSampleCoords& coords,
                                     FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  const FroxelGridDesc& desc,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;
}

bool preflightGridDensity(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          GridDensityRejectReason* reason,
                          f32 epsilon) {
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !gridDensityRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

bool wouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    return shouldSkipFroxelMarch(grid, desc, epsilon);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject =
        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
    return isDensityGridAccessible(grid, desc);

bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      const FroxelGridDesc& desc,
                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return !densityLookupRejectReasonIsBlocking(reject);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
    const DensityLookupRejectReason reject = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);

bool preflightDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return canLookupAtIndex(grid, desc, 0u);
}

bool tryPreflightDensityLookup(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               u32 index,
                               DensityLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc, index, outReason);
}

bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      const FroxelGridDesc& desc,
                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {
    return isDensityGridAccessible(grid, desc);
}

bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      const FroxelGridDesc& desc,
                      u32 tileX,
                      u32 tileY,
                      u32 sliceZ) {
    return canLookupAtIndex(grid, desc,
                            FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc));
}

                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {
    return canLookupAtIndex(grid, desc, 0u);

    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);




    return isDensityGridAccessible(grid, desc);

    return canLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc));



bool tryValidateDensityLookupIndex(const FroxelDensityGrid& grid,
                                   u32 index,
                                   DensityLookupRejectReason& outReason) {
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index) {
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,

                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return !densityLookupRejectReasonIsBlocking(reject);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
    const DensityLookupRejectReason reject =
        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,

bool preflightDensityLookup(const FroxelDensityGrid& grid,

        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);



bool preflightDensityLookupCoord(const FroxelDensityGrid& grid,






    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);



        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);



    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);

bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = DensityLookupRejectReason::EmptyGrid;
        return false;
    if (grid.isEmpty()) {
        outReason = DensityLookupRejectReason::EmptyStorage;
    if (grid.density.size() < requiredFroxelCount(desc)) {
        outReason = DensityLookupRejectReason::DescMismatch;
    if (!gridMatchesDesc(grid, desc)) {
    if (FroxelGridLayout::isFroxelIndexOutOfRange(index, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;

    outReason = DensityLookupRejectReason::None;
    return true;

                         u32 /*index*/,


bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {

    if (FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc)) {

bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !tryCanLookupAtIndex(grid, desc, 0u, reason);

bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    return !tryCanLookupAtIndex(grid, desc, index, reason);

bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
    return !tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool wouldClampDensityLookupIndex(u32 index, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && FroxelGridLayout::isFroxelIndexOutOfRange(index, desc);

bool wouldClampDensityLookupCoord(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc);

bool tryPreflightDensityGridAccess(const FroxelDensityGrid& grid,
                                   GridDensityRejectReason& outReason) {
    if (!FroxelGridLayout::tryPreflightNonEmptyGrid(desc, outReason)) {
    if (!isDensityGridAccessible(grid, desc)) {
            outReason = GridDensityRejectReason::UndersizedStorage;
        } else if (!gridMatchesDesc(grid, desc)) {
            outReason = GridDensityRejectReason::DescMismatch;
        } else {

    outReason = GridDensityRejectReason::None;

bool tryValidateFroxelIndex(u32 index, const FroxelGridDesc& desc, DensityLookupRejectReason& outReason) {
    if (!FroxelGridLayout::isValidFroxelIndex(index, desc)) {

    outReason = classifyDensityLookupReject(grid, desc, index);
    return outReason != DensityLookupRejectReason::EmptyGrid &&
           outReason != DensityLookupRejectReason::EmptyStorage &&
           outReason != DensityLookupRejectReason::DescMismatch;

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    return FroxelGridLayout::isEmptyGrid(desc) || params.density <= 0.f || params.march_steps == 0u;
bool areSampleCoordsReady(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return FroxelGridLayout::isValidSampleCoords(coords, desc);

DensityLookupRejectReason classifyDensityLookupRejectAtIndex(const FroxelDensityGrid& grid,
        return DensityLookupRejectReason::EmptyGrid;
        return DensityLookupRejectReason::EmptyStorage;
        return DensityLookupRejectReason::DescMismatch;
        return DensityLookupRejectReason::IndexOutOfRange;
    return DensityLookupRejectReason::None;

    const DensityLookupRejectReason baseReject = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(baseReject)) {
        return baseReject;




    const DensityLookupRejectReason indexReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(indexReason)) {
        return indexReason;



DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason lookupReject = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(lookupReject)) {
        return lookupReject;








    return !densityLookupRejectReasonIsBlocking(outReason);












    const DensityLookupRejectReason reject = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);












bool preflightDensityLookupReady(const FroxelDensityGrid& grid,



DensityLookupRejectReason classifyDensityLookupAtIndex(const FroxelDensityGrid& grid,

    const DensityLookupRejectReason reject = classifyDensityLookupAtIndex(grid, desc, index);

    const DensityLookupRejectReason baseReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(baseReason)) {
        return baseReason;



}

bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason) {
    outReason = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    return outReason != DensityLookupRejectReason::EmptyGrid &&
           outReason != DensityLookupRejectReason::EmptyStorage &&
           outReason != DensityLookupRejectReason::DescMismatch;
}

    outReason = DensityLookupRejectReason::None;
    if (FroxelGridLayout::isTileCoordOutOfRange(tileX, tileY, sliceZ, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 index,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return reject != DensityLookupRejectReason::EmptyGrid &&
           reject != DensityLookupRejectReason::EmptyStorage &&
           reject != DensityLookupRejectReason::DescMismatch;

bool tryPreflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                      DensityLookupRejectReason& reason) {
    return preflightDensityLookupAtIndex(grid, desc, index, &reason);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);

bool tryPreflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
    return preflightDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ, &reason);

bool wouldClampDensityLookupCoord(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return FroxelGridLayout::isTileCoordOutOfRange(tileX, tileY, sliceZ, desc);
bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
                         DensityLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc,
                               FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc), outReason);
    const u32 index = FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc);
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        return false;

    if (wouldClampDensityLookupCoord(tileX, tileY, sliceZ, desc)) {
    if (!tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc), outReason)) {

    if (tileX >= desc.tilesX || tileY >= desc.tilesY || sliceZ >= desc.slicesZ) {
        outReason = DensityLookupRejectReason::CoordOutOfRange;
    return true;

    return tryCanLookupAtIndex(grid,
                               desc,
                               FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc),
                               outReason);
bool preflightDensityLookup(const FroxelDensityGrid& grid,
    DensityLookupRejectReason localReason = DensityLookupRejectReason::None;
    const bool ok = tryCanLookupAtIndex(grid, desc, index, localReason);
        *reason = localReason;
    return ok;

bool tryPreflightDensityLookup(const FroxelDensityGrid& grid,
    return tryCanLookupAtIndex(grid, desc, index, outReason);

bool shouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
    return shouldSkipFroxelLookup(grid, desc);

bool densityLookupReady(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
    outReason = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
    return !densityLookupRejectReasonIsBlocking(outReason);

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           u32 sliceZ) {
    const DensityLookupRejectReason baseReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(baseReason)) {
        return baseReason;
    if (FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc)) {
        return DensityLookupRejectReason::IndexOutOfRange;

    return DensityLookupRejectReason::None;

    return !densityLookupRejectReasonIsBlocking(reject);

    outReason = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
}

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           u32 tileX,
                                                           u32 tileY,
DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
DensityLookupRejectReason classifyDensityLookupAtCoord(const FroxelDensityGrid& grid,
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return !densityLookupRejectReasonIsBlocking(reject);

    const DensityLookupRejectReason reject =
        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
                                                             u32 tileX,
                                                             u32 tileY,
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    }

DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
                                                             u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
DensityLookupRejectReason classifyFroxelDensityLookupReject(const FroxelDensityGrid& grid,
                                                            u32 index) {
    tryCanLookupAtIndex(grid, desc, index, reason);

DensityLookupRejectReason classifyFroxelDensityLookupCoordReject(const FroxelDensityGrid& grid,

bool preflightFroxelDensityLookup(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason reject = classifyFroxelDensityLookupReject(grid, desc, index);

bool preflightFroxelDensityLookupAtCoord(const FroxelDensityGrid& grid,
        classifyFroxelDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,


bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,







DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,




    const DensityLookupRejectReason reject = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);
    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    const DensityLookupRejectReason reject = classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);
bool preflightDensityLookupAtCoordReady(const FroxelDensityGrid& grid,


bool preflightDensityLookupReady(const FroxelDensityGrid& grid,

    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);

        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);


bool preflightDensityLookupCoordReady(const FroxelDensityGrid& grid,
        classifyDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);

bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryCanLookupAtIndex(grid, desc, index, reason) && reason == DensityLookupRejectReason::None;
bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    return !tryCanLookupAtIndex(grid, desc, index, reason);

bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ) {
bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !tryCanLookupAtIndex(grid, desc, 0u, reason);

bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {

    return !tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
bool tryCanLookupForDensitySample(const FroxelDensityGrid& grid,
    return tryCanLookupAtIndex(grid, desc, 0u, outReason);

bool isDensityLookupIndexInRange(u32 index, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && !FroxelGridLayout::isFroxelIndexOutOfRange(index, desc);

bool tryCanLookupAtIndexStrict(const FroxelDensityGrid& grid,

    if (outReason == DensityLookupRejectReason::IndexOutOfRange) {

    outReason = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
    return !densityLookupRejectReasonIsBlocking(outReason);

bool canLookupAtIndexStrict(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index) {
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;

    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

    return !densityLookupRejectReasonIsBlocking(reject);

    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);


DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,


    const DensityLookupRejectReason reject =
        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);




        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);


DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,


        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);





    return tryCanLookupAtIndexStrict(grid, desc, index, reason);

bool tryCanLookupAtCoordStrict(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {



bool canLookupAtCoordStrict(const FroxelDensityGrid& grid,
    return tryCanLookupAtCoordStrict(grid, desc, tileX, tileY, sliceZ, reason);

    const bool ok = tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, localReason);

bool wouldClampDensityLookupIndex(u32 index, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && FroxelGridLayout::isFroxelIndexOutOfRange(index, desc);

bool canLookupAtCoord(const FroxelDensityGrid& grid,
    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = DensityLookupRejectReason::EmptyGrid;
    if (grid.isEmpty()) {
        outReason = DensityLookupRejectReason::EmptyStorage;
    if (!gridMatchesDesc(grid, desc)) {
        outReason = DensityLookupRejectReason::DescMismatch;



    return tileX > desc.tilesX - 1u || tileY > desc.tilesY - 1u || sliceZ > desc.slicesZ - 1u;
    return tileX >= desc.tilesX || tileY >= desc.tilesY || sliceZ >= desc.slicesZ;
                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {
    return isDensityGridAccessible(grid, desc);

bool wouldClampCoordLookup(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    return tileX > maxTileX || tileY > maxTileY || sliceZ > maxSliceZ;

bool tryPreflightStrictDensityLookupAtIndex(const FroxelDensityGrid& grid,

    outReason = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);

bool tryPreflightStrictDensityLookupAtCoord(const FroxelDensityGrid& grid,


bool wouldRejectDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
DensityLookupRejectReason classifyDensityLookupAtIndex(const FroxelDensityGrid& grid,

DensityLookupRejectReason classifyDensityLookupAtCoord(const FroxelDensityGrid& grid,

    const DensityLookupRejectReason reject = classifyDensityLookupAtIndex(grid, desc, index);

    const DensityLookupRejectReason reject = classifyDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);






DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,


        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);



bool preflightDensityLookupReady(const FroxelDensityGrid& grid,

bool preflightDensityLookupAtCoordReady(const FroxelDensityGrid& grid,












    const DensityLookupRejectReason reject = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);

    return !tryPreflightStrictDensityLookupAtIndex(grid, desc, index, reason);

bool wouldRejectDensityLookupAtCoord(const FroxelDensityGrid& grid,
    return !tryPreflightStrictDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool wouldRejectDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                     u32 /*index*/) {
    return !isDensityGridAccessible(grid, desc);



    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason);


bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid,


    if (!tryCanLookupAtIndex(grid, desc, 0u, reason)) {

    if (FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc)) {
        return DensityLookupRejectReason::IndexOutOfRange;

    return DensityLookupRejectReason::None;


bool preflightDensityLookupCoord(const FroxelDensityGrid& grid,

    return densityLookupRejectReasonIsBlocking(classifyDensityLookupReject(grid, desc, 0u));

    outReason = classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);


    return densityLookupRejectReasonIsBlocking(classifyDensityLookupReject(grid, desc, index));
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                               const FroxelGridDesc& desc,
                                                               u32 tileX,
                                                               u32 tileY,
                                                               u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject =
        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ) {
    return densityLookupRejectReasonIsBlocking(
        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ));
        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ));
}

DensityLookupRejectReason classifyDensityLookupIndexReject(const FroxelDensityGrid& grid,
                                                           const FroxelGridDesc& desc,
                                                           u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
}

                                                      u32 tileX,
                                                      u32 tileY,
                                                      u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool preflightDensityLookup(const FroxelDensityGrid& grid,



DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,






DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,





                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return !densityLookupRejectReasonIsBlocking(reject);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 sliceZ,
    const DensityLookupRejectReason reject =
        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
    }

                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   DensityLookupRejectReason* reason) {
    if (reason != nullptr) {
        *reason = reject;

        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);



        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);

    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);


bool wouldClampDensityLookupIndex(u32 index, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && FroxelGridLayout::isFroxelIndexOutOfRange(index, desc);
}

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           const FroxelGridDesc& desc,
                                                           u32 tileX,
                                                           u32 tileY,
                                                           u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 index,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupIndexReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    return !preflightDensityLookupAtIndex(grid, desc, index);
}

bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ) {
    return !preflightDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
                                                               const FroxelGridDesc& desc,
                                                               u32 tileX,
                                                               u32 tileY,
                                                               u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject =
        classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 index,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelGridDesc& desc,
                       const FroxelSampleCoords& coords) {
    return preflightSampleAtCoords(grid, desc, coords);
    if (!canLookupAtIndex(grid, desc, 0u)) {
        return false;
    }

    return FroxelGridLayout::canPreflightSampleCoords(coords, desc);
}

bool preflightSampleAtCoords(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return preflightSampleAtCoords(grid, desc, coords, reason);
bool canBilinearSampleAtCoords(const FroxelDensityGrid& grid,
    FroxelBilinearSampleRejectReason reason = FroxelBilinearSampleRejectReason::None;
    return tryCanBilinearSampleAtCoords(grid, desc, coords, reason);
}

bool tryCanBilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                  const FroxelSampleCoords& coords,
                                  FroxelBilinearSampleRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = FroxelBilinearSampleRejectReason::EmptyGrid;
            break;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelBilinearSampleRejectReason::InaccessibleGrid;
        return false;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        outReason = FroxelBilinearSampleRejectReason::InvalidSampleCoords;

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelBilinearSampleRejectReason::ClampableWeights;
        return true;

    outReason = FroxelBilinearSampleRejectReason::None;

FroxelBilinearSampleRejectReason classifyFroxelBilinearSampleReject(const FroxelDensityGrid& grid,
    tryCanBilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;

bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
    return froxelBilinearSampleRejectReasonIsBlocking(
        classifyFroxelBilinearSampleReject(grid, desc, coords));
SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
    tryCanSampleAtCoords(grid, desc, coords, reason);

bool preflightFroxelSample(const FroxelDensityGrid& grid,
                           SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          SampleCoordRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = FroxelBilinearSampleRejectReason::EmptyGrid;
            break;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelBilinearSampleRejectReason::InaccessibleGrid;
        return false;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        outReason = FroxelBilinearSampleRejectReason::InvalidSampleCoords;

    if (sampleReason == SampleCoordRejectReason::InvalidWeights ||
        sampleReason == SampleCoordRejectReason::UnorderedCorners) {
        outReason = FroxelBilinearSampleRejectReason::ClampableWeights;
        return true;

    outReason = FroxelBilinearSampleRejectReason::None;

bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
    return !tryCanBilinearSampleAtCoords(grid, desc, coords, reason);

SampleCoordRejectReason classifyDensitySampleReject(const FroxelDensityGrid& grid,
                                                    const FroxelGridDesc& desc,
                                                    const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryCanSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightDensitySampleAtCoords(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyDensitySampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
}

bool tryCanBilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                  const FroxelSampleCoords& coords,
                                  FroxelBilinearSampleRejectReason& outReason) {
bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   FroxelTrilinearSampleRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = FroxelBilinearSampleRejectReason::EmptyGrid;
            break;
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
    const DensityLookupRejectReason lookupReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(lookupReason)) {
            return FroxelTrilinearSampleRejectReason::EmptyGrid;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelBilinearSampleRejectReason::InaccessibleGrid;
        return false;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        outReason = FroxelBilinearSampleRejectReason::InvalidSampleCoords;

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelBilinearSampleRejectReason::ClampableWeights;
        return true;

    outReason = FroxelBilinearSampleRejectReason::None;

bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
    return !tryCanBilinearSampleAtCoords(grid, desc, coords, reason);

SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
            return SampleCoordRejectReason::EmptyGrid;
            return SampleCoordRejectReason::OutOfBounds;
            return SampleCoordRejectReason::None;
        }

    return FroxelGridLayout::classifySampleCoordReject(coords, desc);

bool preflightFroxelSampleAtCoords(const FroxelDensityGrid& grid,
                                   SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);

bool canBilinearSampleAtCoords(const FroxelDensityGrid& grid,
    FroxelBilinearSampleRejectReason reason = FroxelBilinearSampleRejectReason::None;
    return tryCanBilinearSampleAtCoords(grid, desc, coords, reason);

            return FroxelTrilinearSampleRejectReason::InaccessibleGrid;


    if (sampleReason == SampleCoordRejectReason::InvalidWeights ||
        sampleReason == SampleCoordRejectReason::UnorderedCorners) {
        outReason = FroxelTrilinearSampleRejectReason::ClampableWeights;

    const SampleCoordRejectReason sampleReason = FroxelGridLayout::classifySampleCoordReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReason)) {
        return FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

        return FroxelTrilinearSampleRejectReason::ClampableWeights;

    return FroxelTrilinearSampleRejectReason::None;

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   FroxelTrilinearSampleRejectReason& outReason) {
    outReason = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(outReason);

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);


bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

                             SampleCoordRejectReason& outReason) {
    return tryCanSampleAtCoords(grid, desc, coords, outReason);

bool wouldClampSampleAtCoords(const FroxelDensityGrid& grid,
    if (!isDensityGridAccessible(grid, desc)) {

    return FroxelGridLayout::wouldClampSampleCoords(coords, desc);

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
    return preflightSampleAtCoords(grid, desc, coords);

    return preflightSampleAtCoords(grid, desc, coords, outReason);

bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                   u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return preflightDensityLookupAtIndex(grid, desc, index, reason);

                                   u32 index,
                                   DensityLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc, index, outReason);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ) {
    return preflightDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

                                   u32 sliceZ,
    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason);

    if (!tryCanLookupAtIndex(grid, desc,
                             FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc), outReason)) {
    const u32 index = FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc);
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {

    if (wouldClampDensityLookupCoord(tileX, tileY, sliceZ, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;
    return FroxelGridLayout::wouldClampTileCoords(tileX, tileY, sliceZ, desc);
    if (!tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc),
                             outReason)) {

    outReason = DensityLookupRejectReason::None;



    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {


bool wouldClampDensityLookupCoord(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {


    return FroxelGridLayout::isFroxelCoordOutOfRange(tileX, tileY, sliceZ, desc);

    if (wouldClampCoordLookup(tileX, tileY, sliceZ, desc)) {
    } else {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = DensityLookupRejectReason::EmptyGrid;
    if (grid.isEmpty()) {
        outReason = DensityLookupRejectReason::EmptyStorage;
    if (!gridMatchesDesc(grid, desc)) {
        outReason = DensityLookupRejectReason::DescMismatch;

        outReason = DensityLookupRejectReason::CoordOutOfRange;

bool canSampleAtCoords(const FroxelDensityGrid& grid,
    return tryCanSampleAtCoords(grid, desc, coords, reason);

bool tryCanSampleTrilinear(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;

    const DensityLookupRejectReason lookupReject = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(lookupReject)) {
        switch (lookupReject) {
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        outReason = FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

    outReason = FroxelTrilinearSampleRejectReason::None;

bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason) {
        outReason = FroxelPopulateRejectReason::EmptyGrid;
    if (params.density <= 0.f) {
        outReason = FroxelPopulateRejectReason::ZeroDensity;
    if (params.march_steps == 0u) {
        outReason = FroxelPopulateRejectReason::ZeroMarchSteps;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = FroxelPopulateRejectReason::InvalidCamera;

    outReason = FroxelPopulateRejectReason::None;

                       const FroxelSampleCoords& /*coords*/) {
    return canLookupAtIndex(grid, desc, 0u);

SampleCoordRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason lookupReject = classifyDensityLookupReject(grid, desc, 0u);
    switch (lookupReject) {
        return SampleCoordRejectReason::EmptyGrid;
        return SampleCoordRejectReason::OutOfBounds;

    return FroxelGridLayout::classifySampleCoordReject(coords, desc);
            return FroxelTrilinearSampleRejectReason::EmptyGrid;
            return FroxelTrilinearSampleRejectReason::EmptyStorage;
            return FroxelTrilinearSampleRejectReason::DescMismatch;

        return FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

    return FroxelTrilinearSampleRejectReason::None;

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    FroxelTrilinearSampleRejectReason* reason) {
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;

bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,

bool preflightFroxelTrilinearSampleReady(const FroxelDensityGrid& grid,

    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtCoord(grid, desc, coords.tileX0, coords.tileY0, coords.sliceZ0, lookupReason)) {
            outReason = SampleCoordRejectReason::OutOfBounds;
        case DensityLookupRejectReason::ScreenMappingFailed:
            outReason = SampleCoordRejectReason::None;

    return FroxelGridLayout::tryPreflightSampleCoords(coords, desc, outReason);

bool canBilinearSampleAtCoords(const FroxelDensityGrid& grid,
    return tryCanBilinearSampleAtCoords(grid, desc, coords, reason);
            return SampleCoordRejectReason::None;

    return FroxelGridLayout::classifySampleCoordsReject(coords, desc);

bool preflightFroxelSample(const FroxelDensityGrid& grid,
                           SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleReject(grid, desc, coords);
    return !sampleCoordRejectReasonIsBlocking(reject);

    outReason = classifyFroxelSampleReject(grid, desc, coords);
    return !sampleCoordRejectReasonIsBlocking(outReason);

bool tryCanBilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                  FroxelBilinearSampleRejectReason& outReason) {


FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(
    const FroxelDensityGrid& grid,

    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
            outReason = FroxelBilinearSampleRejectReason::EmptyGrid;
FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
    if (densityLookupRejectReasonIsBlocking(lookupReject)) {
            outReason = FroxelBilinearSampleRejectReason::InaccessibleGrid;
            return FroxelTrilinearSampleRejectReason::InaccessibleGrid;

        outReason = FroxelBilinearSampleRejectReason::InvalidSampleCoords;

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelBilinearSampleRejectReason::ClampableWeights;

    outReason = FroxelBilinearSampleRejectReason::None;
    const SampleCoordRejectReason sampleReject = FroxelGridLayout::classifySampleCoordsReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReject)) {

    if (sampleReject == SampleCoordRejectReason::InvalidWeights) {
        return FroxelTrilinearSampleRejectReason::ClampableWeights;


    const FroxelTrilinearSampleRejectReason reject = classifyTrilinearSampleReject(grid, desc, coords);

    outReason = classifyTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(outReason);







        outReason = FroxelTrilinearSampleRejectReason::ClampableWeights;



        }

    const SampleCoordRejectReason sampleReason =
        FroxelGridLayout::classifyFroxelSampleCoordsReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReason)) {

                                     const FroxelGridDesc& desc,
                                     const FroxelSampleCoords& coords,

                                   FroxelTrilinearSampleRejectReason& outReason) {
    outReason = classifyFroxelTrilinearSampleReject(grid, desc, coords);

    const SampleCoordRejectReason sampleReject = FroxelGridLayout::classifySampleCoordReject(coords, desc);





FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                    const FroxelGridDesc& desc,
                                                                    const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,
                                     const FroxelGridDesc& desc,
                                     const FroxelSampleCoords& coords,
                                     FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryCanSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelSampleAtCoords(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   const FroxelSampleCoords& coords,
                                   SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

SampleCoordRejectReason classifyFroxelSampleAtCoordsReject(const FroxelDensityGrid& grid,
                                                           const FroxelGridDesc& desc,
                                                           const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryCanSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelSampleAtCoords(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   const FroxelSampleCoords& coords,
                                   SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleAtCoordsReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
                                const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
}

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelSampleCoords& coords,
                                    FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

bool wouldSkipDensityTrilinearSample(const FroxelDensityGrid& grid,
    return !tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool canAccessDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
    return !FroxelGridLayout::isEmptyGrid(desc) && gridMatchesDesc(grid, desc);

bool canSampleDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
bool canSampleAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {

bool canSampleAtCoord(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc) {


bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryCanLookupAtIndex(grid, desc, index, reason);

    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;

                         u32 /*index*/,
                         FroxelLookupRejectReason& outReason) {
        outReason = FroxelLookupRejectReason::EmptyGrid;
        outReason = FroxelLookupRejectReason::EmptyStorage;
        outReason = FroxelLookupRejectReason::DescMismatch;

    outReason = FroxelLookupRejectReason::None;
bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
    if (FroxelGridLayout::isEmptyGrid(desc)) {
    if (grid.isEmpty()) {
    if (!gridMatchesDesc(grid, desc)) {

bool isEmptyGridForSampling(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return FroxelGridLayout::isEmptyGrid(desc) || grid.isEmpty();

bool canSampleAtCoords(const FroxelDensityGrid& grid,
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryCanSampleAtCoords(grid, desc, coords, reason);

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          SampleCoordRejectReason& outReason) {
    if (FroxelGridLayout::isSampleCoordsOutOfRange(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfRange;


bool shouldSkipDensitySample(const FroxelDensityGrid& grid,
    return !canSampleAtCoords(grid, desc, coords);
bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {
    return canLookupAtIndex(grid, desc, 0u);

bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc),
                               outReason);

bool canSampleFroxelGrid(const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc);

bool shouldSkipFroxelSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return shouldSkipFroxelLookup(grid, desc);

bool isValidDensitySampleRequest(const FroxelDensityGrid& grid,
    if (!isDensityGridAccessible(grid, desc)) {

    if (FroxelGridLayout::tryAreSampleCoordsInBounds(coords, desc, reason)) {

    FroxelSampleCoords clampProbe = coords;
    return FroxelGridLayout::tryClampSampleCoords(clampProbe, desc);

bool canPopulateFroxelGrid(const FroxelGridDesc& desc) {
    return canSampleFroxelGrid(desc);
bool tryCanLookupAtIndexBounds(const FroxelDensityGrid& grid,
                               u32 index,
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
    if (FroxelGridLayout::isFroxelIndexOutOfRange(index, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;

    outReason = DensityLookupRejectReason::None;

bool tryCanLookupAtSampleCoords(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outReason = DensityLookupRejectReason::SampleCoordsOutOfRange;


FroxelGridPreflight preflightFroxelDensityGrid(const FroxelDensityGrid& grid,
                                               f32 epsilon) {
    FroxelGridPreflight result{};
    result.desc_non_empty = !FroxelGridLayout::isEmptyGrid(desc);
    result.storage_allocated = !grid.isEmpty();
    result.storage_matches_desc = gridMatchesDesc(grid, desc);
    result.density_counts_valid = validateDensityCounts(grid, epsilon);
    return result;

FroxelPopulatePreflight preflightPopulateFroxelGrid(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    FroxelPopulatePreflight result{};
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    result.desc_non_empty = clampedDesc.froxelCount() > 0u;
    result.params_enabled = params.density > 0.f && params.march_steps > 0u;
bool canLookupAtIndexInRange(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    return tryCanLookupAtIndexInRange(grid, desc, index, reason);

bool tryCanLookupAtIndexInRange(const FroxelDensityGrid& grid,

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    return tryCanSampleAtCoords(grid, desc, coords, lookupReason, coordReason);

                          DensityLookupRejectReason& outLookupReason,
                          SampleCoordRejectReason& outCoordReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outLookupReason)) {
        outCoordReason = SampleCoordRejectReason::None;
        outLookupReason = DensityLookupRejectReason::EmptyGrid;
        outCoordReason = SampleCoordRejectReason::EmptyGrid;
    if (!FroxelGridLayout::isValidSampleCoords(coords, desc)) {
        outLookupReason = DensityLookupRejectReason::None;
        outCoordReason = SampleCoordRejectReason::OutOfBounds;


bool canSampleValidCoords(const FroxelDensityGrid& grid,
    return canLookupAtIndex(grid, desc, 0u) && FroxelGridLayout::isValidSampleCoords(coords, desc);

SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
    tryCanSampleAtCoords(grid, desc, coords, reason);

bool wouldSkipFroxelSample(const FroxelDensityGrid& grid,
                           SampleCoordRejectReason* outReason) {
    const SampleCoordRejectReason reason = classifyFroxelSampleReject(grid, desc, coords);
    if (outReason != nullptr) {
        *outReason = reason;

                *outReason = SampleCoordRejectReason::EmptyGrid;
                *outReason = SampleCoordRejectReason::OutOfBounds;
                *outReason = SampleCoordRejectReason::None;


bool canSampleAtCoordsStrict(const FroxelDensityGrid& grid,
    return tryCanSampleAtCoordsStrict(grid, desc, coords, reason);

bool tryCanSampleAtCoordsStrict(const FroxelDensityGrid& grid,
    if (!tryCanSampleAtCoords(grid, desc, coords, outReason)) {

    if (outReason != SampleCoordRejectReason::None) {


    return tryCanSampleAtCoords(grid, desc, coords, reason) &&
           reason == SampleCoordRejectReason::None &&
           FroxelGridLayout::isValidSampleCoords(coords, desc);



    if (reason == SampleCoordRejectReason::EmptyGrid || reason == SampleCoordRejectReason::OutOfBounds) {

    if (!canSampleFroxelGrid(desc)) {
        outReason = FroxelTrilinearSampleRejectReason::NotSampleable;
        outReason = FroxelTrilinearSampleRejectReason::EmptyStorage;
    if (!isDensitySizedForGrid(grid, desc)) {
        outReason = FroxelTrilinearSampleRejectReason::UndersizedStorage;
        outReason = FroxelTrilinearSampleRejectReason::DescMismatch;


bool tryCanSampleDensityTrilinear(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::LookupFailed;



bool canSampleDensityTrilinear(const FroxelDensityGrid& grid,
    return tryCanSampleDensityTrilinear(grid, desc, coords, reason);


    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, coordReason)) {


bool tryCanSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::NotAccessible;

    if (grid.density.size() < clampedDesc.froxelCount()) {


bool wouldSkipFroxelTrilinearSample(const FroxelDensityGrid& grid,
    return reject != FroxelTrilinearSampleRejectReason::None;

bool canSampleTrilinear(const FroxelDensityGrid& grid,
    return tryCanSampleTrilinear(grid, desc, coords, reason);

bool tryCanSampleTrilinear(const FroxelDensityGrid& grid,


    if (coordReason == SampleCoordRejectReason::InvalidWeights ||
        FroxelGridLayout::wouldClampSampleCoords(coords, desc)) {
        outReason = FroxelTrilinearSampleRejectReason::ClampRequired;


bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
    return FroxelGridLayout::wouldClampSampleCoords(coords, desc);


        outReason = FroxelTrilinearSampleRejectReason::HardOutOfBounds;


bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, outReason);

    DensityTrilinearSampleRejectReason reason = DensityTrilinearSampleRejectReason::None;

                                   DensityTrilinearSampleRejectReason& outReason) {
        outReason = DensityTrilinearSampleRejectReason::EmptyGrid;
        outReason = DensityTrilinearSampleRejectReason::EmptyStorage;
        outReason = DensityTrilinearSampleRejectReason::DescMismatch;

        outReason = DensityTrilinearSampleRejectReason::InvalidSampleCoords;

        outReason = DensityTrilinearSampleRejectReason::ClampableWeights;

    outReason = DensityTrilinearSampleRejectReason::None;

bool wouldClampDensityTrilinearSample(const FroxelDensityGrid& grid,


                                   TrilinearSampleRejectReason& outReason) {
        outReason = TrilinearSampleRejectReason::EmptyGrid;
        outReason = TrilinearSampleRejectReason::EmptyStorage;
        outReason = TrilinearSampleRejectReason::DescMismatch;

    return FroxelGridLayout::tryPreflightTrilinearSampleCoords(coords, desc, outReason);


    if (outReason == SampleCoordRejectReason::EmptyGrid ||
        outReason == SampleCoordRejectReason::OutOfBounds) {

        outReason = sampleReason == SampleCoordRejectReason::EmptyGrid
                        ? FroxelTrilinearSampleRejectReason::EmptyGrid
                        : FroxelTrilinearSampleRejectReason::HardOutOfBounds;

        outReason = FroxelTrilinearSampleRejectReason::InvalidWeights;





    outReason = classifyFroxelTrilinearSampleReject(grid, desc, coords);

    const DensityLookupRejectReason lookupReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(lookupReason)) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
        classifyTrilinearSampleReject(grid, desc, coords);

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   FroxelTrilinearSampleRejectReason& outReason) {
    return !froxelTrilinearSampleRejectReasonIsBlocking(outReason);

    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            return FroxelTrilinearSampleRejectReason::EmptyGrid;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;

        outReason = FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

        outReason = FroxelTrilinearSampleRejectReason::ClampableWeights;

            return FroxelTrilinearSampleRejectReason::InaccessibleGrid;
        }

    const SampleCoordRejectReason sampleReason = FroxelGridLayout::classifySampleCoordsReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReason)) {
        return FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        return FroxelTrilinearSampleRejectReason::ClampableWeights;

    return FroxelTrilinearSampleRejectReason::None;

    const SampleCoordRejectReason sampleReason = FroxelGridLayout::classifySampleCoordReject(coords, desc);


bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   FroxelTrilinearSampleRejectReason& outReason) {
    outReason = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(outReason);

bool wouldSkipDensityTrilinearSample(const FroxelDensityGrid& grid,
    return !tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool canSampleDensityTrilinear(const FroxelDensityGrid& grid,
    return tryCanSampleDensityTrilinear(grid, desc, coords, reason);

bool tryCanSampleDensityTrilinear(const FroxelDensityGrid& grid,
    if (FroxelGridLayout::isEmptyGrid(desc)) {
    if (grid.isEmpty()) {
        outReason = FroxelTrilinearSampleRejectReason::EmptyStorage;
    if (grid.density.size() < FroxelGridDesc::clampCounts(desc).froxelCount()) {
        outReason = FroxelTrilinearSampleRejectReason::UndersizedStorage;
        outReason = FroxelTrilinearSampleRejectReason::NotSampleable;



bool wouldClampTrilinearSampleCoords(const FroxelDensityGrid& grid,



bool wouldRejectSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return !FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);

bool shouldSkipFroxelTrilinear(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !isDensityGridAccessible(grid, desc);


        } else if (!gridMatchesDesc(grid, desc)) {
            outReason = FroxelTrilinearSampleRejectReason::DescMismatch;
        } else {

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, coordReason)) {




bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return shouldSkipFroxelLookup(grid, desc);

bool wouldClampTrilinearSample(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

bool tryCanSampleTrilinear(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::GridInaccessible;

        outReason = coordReason == SampleCoordRejectReason::EmptyGrid
                        : FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

    if (coordReason == SampleCoordRejectReason::InvalidWeights) {

bool canSampleTrilinear(const FroxelDensityGrid& grid,
    return tryCanSampleTrilinear(grid, desc, coords, reason);
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              SampleCoordRejectReason* reason) {
    SampleCoordRejectReason reject = SampleCoordRejectReason::None;
    const bool ok = tryCanSampleAtCoords(grid, desc, coords, reject);
    return ok;

bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
                                 SampleCoordRejectReason& reason) {
    return preflightTrilinearSample(grid, desc, coords, &reason);

bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid,
    return !preflightTrilinearSample(grid, desc, coords);

















    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);













bool preflightTrilinearSampleReady(const FroxelDensityGrid& grid,

}

bool shouldSkipFroxelTrilinearSample(const FroxelDensityGrid& grid,
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           u32 tileX,
                                                           u32 tileY,
                                                           u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
bool preflightTrilinearSample(const FroxelDensityGrid& grid,
    const FroxelTrilinearSampleRejectReason reject = classifyTrilinearSampleReject(grid, desc, coords);
    }
bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
bool preflightTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
bool preflightFroxelTrilinearSampleReady(const FroxelDensityGrid& grid,
bool preflightTrilinearSampleReady(const FroxelDensityGrid& grid,
GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    return !densityLookupRejectReasonIsBlocking(reject);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);

bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* reason,
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    return !gridDensityRejectReasonIsBlocking(reject);


bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    return !froxelPopulateRejectReasonIsBlocking(reject);

bool wouldSkipDensityTrilinearSample(const FroxelDensityGrid& grid,
                                     const FroxelGridDesc& desc,
                                     const FroxelSampleCoords& coords) {
    return !preflightTrilinearSample(grid, desc, coords);
}

FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                const FroxelGridDesc& desc,
                                                                const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;

bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   const FroxelSampleCoords& coords,
                                   FroxelTrilinearSampleRejectReason& outReason) {
    return tryPreflightTrilinearSample(grid, desc, coords, outReason);

        outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;
        return false;
    if (!gridMatchesDesc(grid, desc)) {

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        switch (sampleReason) {
        case SampleCoordRejectReason::EmptyGrid:
            break;
        case SampleCoordRejectReason::OutOfBounds:
            outReason = FroxelTrilinearSampleRejectReason::HardOutOfBounds;
        case SampleCoordRejectReason::InvalidWeights:
        case SampleCoordRejectReason::None:

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelTrilinearSampleRejectReason::None;
    return true;

bool tryPreflightTrilinearDensitySample(const FroxelDensityGrid& grid,
                                        SampleCoordRejectReason& outReason) {
    return tryCanSampleAtCoords(grid, desc, coords, outReason);

bool canPreflightTrilinearDensitySample(const FroxelDensityGrid& grid,
    return tryPreflightTrilinearDensitySample(grid, desc, coords, reason);

    return !canPreflightTrilinearDensitySample(grid, desc, coords);

bool wouldClampTrilinearDensitySample(const FroxelDensityGrid& grid,
    if (!isDensityGridAccessible(grid, desc)) {

    return FroxelGridLayout::wouldClampSampleCoords(coords, desc);

bool preflightSampleAtCoords(const FroxelDensityGrid& grid,
    SampleCoordRejectReason localReason = SampleCoordRejectReason::None;
    const bool ok = tryCanSampleAtCoords(grid, desc, coords, localReason);
        *reason = localReason;

bool canSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
    return tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);

bool tryCanSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::NotAccessible;

        outReason = sampleReason == SampleCoordRejectReason::OutOfBounds
                        ? FroxelTrilinearSampleRejectReason::HardOutOfBounds
            return FroxelTrilinearSampleRejectReason::InaccessibleGrid;

    const SampleCoordRejectReason sampleReason = FroxelGridLayout::classifySampleCoordReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReason)) {
        return FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

        return FroxelTrilinearSampleRejectReason::ClampableWeights;

    return FroxelTrilinearSampleRejectReason::None;

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                     FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

bool wouldSkipFroxelTrilinearSample(const FroxelDensityGrid& grid,
FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;

    const FroxelTrilinearSampleRejectReason reject = classifyTrilinearSampleReject(grid, desc, coords);


    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);





    return !tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);

        return FroxelTrilinearSampleRejectReason::EmptyGrid;

    switch (FroxelGridLayout::classifySampleCoordsReject(coords, desc)) {

bool tryCanSampleAtTrilinear(const FroxelDensityGrid& grid,
    outReason = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(outReason);

                                    f32* outDensity,
                                    FroxelTrilinearSampleRejectReason* outReason) {
    if (outReason != nullptr) {
        *outReason = reject;
    if (froxelTrilinearSampleRejectReasonIsBlocking(reject)) {
        if (outDensity != nullptr) {
            *outDensity = 0.f;

        *outDensity = sampleDensityTrilinear(grid, desc, coords);


    if (reason != nullptr) {
        *reason = reject;


SampleCoordRejectReason classifyDensitySampleCoordReject(const FroxelDensityGrid& grid,
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            return SampleCoordRejectReason::EmptyGrid;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
            return SampleCoordRejectReason::OutOfBounds;
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            return SampleCoordRejectReason::None;

    return FroxelGridLayout::classifySampleCoordReject(coords, desc);

bool preflightDensitySampleAtCoords(const FroxelDensityGrid& grid,
                                    SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyDensitySampleCoordReject(grid, desc, coords);
    return !sampleCoordRejectReasonIsBlocking(reject);

bool wouldSkipDensitySampleAtCoords(const FroxelDensityGrid& grid,
    return !preflightDensitySampleAtCoords(grid, desc, coords);


bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,

bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return !tryCanSampleAtCoords(grid, desc, coords, reason);







bool preflightTrilinearSample(const FroxelDensityGrid& grid,










    return froxelTrilinearSampleRejectReasonIsBlocking(classifyFroxelTrilinearSampleReject(grid, desc, coords));




    return froxelTrilinearSampleRejectReasonIsBlocking(
        classifyFroxelTrilinearSampleReject(grid, desc, coords));





    return !preflightFroxelTrilinearSample(grid, desc, coords);




}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           const FroxelGridDesc& desc,
                                                           u32 tileX,
                                                           u32 tileY,
                                                           u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    tryCanSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  const FroxelGridDesc& desc,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject =
        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightFroxelSample(const FroxelDensityGrid& grid,
                           const FroxelGridDesc& desc,
                           const FroxelSampleCoords& coords,
                           SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifyFroxelSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !sampleCoordRejectReasonIsBlocking(reject);
}

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

bool preflightGridDensity(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          GridDensityRejectReason* reason,
                          f32 epsilon) {
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !gridDensityRejectReasonIsBlocking(reject);
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                    const FroxelGridDesc& desc,
                                                                    const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    const FroxelGridDesc& desc,
                                    const FroxelSampleCoords& coords,
                                    FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

u32 countNonZeroFroxels(const FroxelDensityGrid& grid, f32 epsilon) {
    if (grid.isEmpty()) {
        return 0u;
    }

    u32 count = 0u;
    for (f32 value : grid.density) {
        if (value > epsilon) {
            ++count;
        }
    }
    return count;
}

u32 countNonZeroFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!gridMatchesDesc(grid, desc)) {
        return 0u;
    }
    return countNonZeroFroxels(grid, epsilon);
}

u32 countEmptyFroxels(const FroxelDensityGrid& grid, f32 epsilon) {
    if (grid.isEmpty()) {
        return 0u;
    }

    u32 count = 0u;
    for (f32 value : grid.density) {
        if (value <= epsilon) {
            ++count;
        }
    }
    return count;
}

u32 countNonZeroFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!isDensityGridAccessible(grid, desc)) {
    if (!gridMatchesDesc(grid, desc)) {
        return 0u;
    }
    return countNonZeroFroxels(grid, epsilon);

u32 countEmptyFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
        return desc.froxelCount();
    const u32 froxelCount = FroxelGridDesc::clampCounts(desc).froxelCount();
        return froxelCount;
        return FroxelGridDesc::clampCounts(desc).froxelCount();
    }
    return countEmptyFroxels(grid, epsilon);
}

bool isDensityGridFullyEmpty(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!isDensityGridAccessible(grid, desc)) {
        return true;
    }
    return !hasNonZeroDensity(grid, epsilon);
bool isDensityFullyEmpty(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
        return false;
    return countNonZeroFroxels(grid, epsilon) == 0u;

bool validateDensityCountsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!gridMatchesDesc(grid, desc)) {
        return FroxelGridLayout::isEmptyGrid(desc);
    return validateDensityCounts(grid, epsilon);
}

bool hasNonZeroDensity(const FroxelDensityGrid& grid, f32 epsilon) {
    return countNonZeroFroxels(grid, epsilon) > 0u;
}

bool shouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!isDensityGridAccessible(grid, desc)) {
        return true;
    }
    return !hasNonZeroDensity(grid, epsilon);
}

f32 sampleDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    if (!isDensityGridAccessible(grid, desc)) {
    if (!canAccessDensityGrid(grid, desc)) {
    if (!canAccessDensityAtIndex(grid, desc, index)) {
        return 0.f;
    }

    const u32 clampedIndex = FroxelGridLayout::clampFroxelIndex(index, desc);
    return grid.density[clampedIndex];
}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity) {
    if (!canSampleDensityAtIndex(grid, desc, index)) {
    if (!canSampleAtIndex(grid, desc, index)) {
        outDensity = 0.f;
        return false;
    }

    const u32 clampedIndex = FroxelGridLayout::clampFroxelIndex(index, desc);
    outDensity = grid.density[clampedIndex];
    return true;
}

f32 sampleDensityAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ) {
    if (!isDensityGridAccessible(grid, desc)) {
        return 0.f;
    }

    const u32 index = FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc);
    return grid.density[index];

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity) {
    if (!canSampleAtCoord(grid, desc)) {
        outDensity = 0.f;
        return false;
    }

    const u32 index = FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc);
    outDensity = grid.density[index];
    return true;
}

bool writeDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value) {
    if (!canAccessDensityGrid(grid, desc)) {
bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             u32 index,
                             f32& outDensity) {
    if (!canAccessDensityAtIndex(grid, desc, index)) {
        outDensity = 0.f;
        return false;

    outDensity = sampleDensityAtIndex(grid, desc, index);
    return true;

        return false;
    }

    const u32 clampedIndex = FroxelGridLayout::clampFroxelIndex(index, desc);
    grid.density[clampedIndex] = value;
    return true;
}

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value) {
    if (!canSampleDensityAtIndex(grid, desc, index)) {
        return false;
    }

    const u32 clampedIndex = FroxelGridLayout::clampFroxelIndex(index, desc);
    grid.density[clampedIndex] = value;
    return true;
}

bool writeDensityAtCoord(FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         f32 value) {
    if (!isDensityGridAccessible(grid, desc)) {
        return false;
    }

    const u32 index = FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc);
    grid.density[index] = value;
    return true;
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            u32 index,
    if (!canAccessDensityAtIndex(grid, desc, index)) {

    return writeDensityAtIndex(grid, desc, index, value);
}

bool validateDensityCounts(const FroxelDensityGrid& grid, f32 epsilon) {
    if (grid.isEmpty()) {
        return true;
    }

    return countNonZeroFroxels(grid, epsilon) + countEmptyFroxels(grid, epsilon) == grid.density.size();
}

bool validateGridDensity(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    return tryValidateGridDensity(grid, desc, reason, epsilon);
}

bool validateGridDensityForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    return validateGridDensity(grid, desc, epsilon);

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  const FroxelGridDesc& desc,
                                                  f32 epsilon) {
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;

bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* reason,
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    return !gridDensityRejectReasonIsBlocking(reject);
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    return tryValidateGridDensityForDesc(grid, desc, reason, epsilon);
}

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   GridDensityRejectReason& outReason,
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        outReason = GridDensityRejectReason::None;
        return true;
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = GridDensityRejectReason::EmptyDesc;
        return false;

    return tryValidateGridDensity(grid, desc, outReason, epsilon);
}

    if (!gridMatchesDesc(grid, desc)) {
        outReason = GridDensityRejectReason::DescMismatch;


bool validateDensityCountsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    return validateDensityCounts(grid, epsilon);

u32 countNonZeroFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
        return 0u;
    return countNonZeroFroxels(grid, epsilon);

u32 countEmptyFroxelsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
        return clampedDesc.froxelCount();
    return countEmptyFroxels(grid, epsilon);

bool isDensityFullyEmpty(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!isDensityGridAccessible(grid, desc)) {
    return !hasNonZeroDensity(grid, epsilon);

    const u32 froxelCount = clampedDesc.froxelCount();

    if (froxelCount == 0u) {
    if (grid.density.size() < froxelCount) {
        outReason = GridDensityRejectReason::UndersizedStorage;
    if (!validateDensityCounts(grid, epsilon)) {
        outReason = GridDensityRejectReason::DensityCountMismatch;

                                   const FroxelGridDesc& desc,
                                   f32 epsilon) {





GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;

bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* reason,
    GridDensityRejectReason rejectReason = GridDensityRejectReason::None;
    const bool ok = tryValidateGridDensity(grid, desc, rejectReason, epsilon);
    if (reason != nullptr) {
        *reason = rejectReason;
    return ok;
GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    tryValidateGridDensity(grid, desc, reason);

                          GridDensityRejectReason* reason) {
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc);
        *reason = reject;
    return !gridDensityRejectReasonIsBlocking(reject);

    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
        return GridDensityRejectReason::None;
    if (grid.density.size() < clampedDesc.froxelCount()) {
        return GridDensityRejectReason::UndersizedStorage;
    if (grid.density.size() > clampedDesc.froxelCount()) {
        return GridDensityRejectReason::DescMismatch;
        return GridDensityRejectReason::DensityCountMismatch;


















bool preflightGridDensityReady(const FroxelDensityGrid& grid,

bool tryValidateGridDensity(const FroxelDensityGrid& grid,
                            GridDensityRejectReason& outReason,
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = GridDensityRejectReason::None;
        return true;
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (grid.density.size() < clampedDesc.froxelCount()) {
        outReason = GridDensityRejectReason::UndersizedStorage;
        return false;
    if (grid.density.size() > clampedDesc.froxelCount()) {
        outReason = GridDensityRejectReason::DescMismatch;
    if (!gridMatchesDesc(grid, desc)) {
    }
        outReason = GridDensityRejectReason::DensityCountMismatch;
    if (!validateDensityCounts(grid, epsilon)) {


bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             u32 index,
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtIndex(grid, desc, index, outDensity, reason);

                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon) {
    if (clampedDesc.froxelCount() == 0u) {
        outReason = GridDensityRejectReason::EmptyDesc;
        } else {

    return tryValidateGridDensity(grid, desc, outReason, epsilon);

                             FroxelLookupRejectReason& outReason) {

    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        outDensity = 0.f;

    outDensity = sampleDensityAtIndex(grid, desc, index);
    outReason = FroxelLookupRejectReason::None;
    outReason = DensityLookupRejectReason::None;

bool tryValidateGridDensityStrict(const FroxelDensityGrid& grid,

    const u32 froxelCount = clampedDesc.froxelCount();

    if (froxelCount == 0u) {
        outReason = GridDensityRejectReason::EmptyGrid;


bool canPopulateFroxelGrid(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    if (shouldSkipFroxelGrid(desc)) {
    if (params.density <= 0.f || params.march_steps == 0u) {

bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
                             SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;
    if (FroxelGridLayout::isSampleCoordsOutOfRange(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfRange;

    outReason = SampleCoordRejectReason::None;


bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            f32 value) {
    return tryWriteDensityAtIndex(grid, desc, index, value, reason);

                            f32 value,

    return writeDensityAtIndex(grid, desc, index, value);

bool trySampleDensityAtIndexBounds(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndexBounds(grid, desc, index, outReason)) {

    outDensity = grid.density[index];


    outReason = classifyGridDensityReject(grid, desc, epsilon);
    return !gridDensityRejectReasonIsBlocking(outReason);

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
        return GridDensityRejectReason::None;
        return GridDensityRejectReason::UndersizedStorage;
        return GridDensityRejectReason::DescMismatch;



        if (!grid.isEmpty()) {




                             SampleCoordBoundsRejectReason& outReason) {
        outReason = SampleCoordBoundsRejectReason::EmptyGrid;

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ ||
        coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
        outReason = SampleCoordBoundsRejectReason::OutOfBoundsTile;
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = SampleCoordBoundsRejectReason::OutOfBoundsWeight;

    outReason = SampleCoordBoundsRejectReason::None;
        return GridDensityRejectReason::DensityCountMismatch;


bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* reason,
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    return !gridDensityRejectReasonIsBlocking(reject);


    if (grid.density.size() < froxelCount) {



bool validateDensityCountsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    if (!isDensityGridAccessible(grid, desc)) {
    return validateDensityCounts(grid, epsilon);
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;

                          GridDensityRejectReason* outReason,
    if (outReason != nullptr) {
        *outReason = reject;


bool wouldSkipGridDensityValidation(const FroxelDensityGrid& grid,
    return !preflightGridDensity(grid, desc, nullptr, epsilon);


    return !tryValidateGridDensity(grid, desc, reason, epsilon);
DensityLookupRejectReason classifyDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                                       u32 index) {
    tryCanLookupAtIndex(grid, desc, index, reason);

DensityLookupRejectReason classifyDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                                       u32 tileX,
                                                       u32 tileY,
                                                       u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
                                     DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason rejectReason = classifyDensityLookupAtIndex(grid, desc, index);
        *reason = rejectReason;
    return !densityLookupRejectReasonIsBlocking(rejectReason);

bool preflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                     u32 sliceZ,
    const DensityLookupRejectReason rejectReason = classifyDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);
        return GridDensityRejectReason::EmptyDesc;

    if (hasNonFiniteDensity(grid)) {
        return GridDensityRejectReason::NonFiniteDensity;



bool isFiniteDensityValue(f32 value) {
    return std::isfinite(value) && value >= 0.f;

f32 sanitizeDensityValue(f32 value) {
    if (!std::isfinite(value) || value < 0.f) {
        return 0.f;
    return value;

u32 countNonFiniteDensities(const FroxelDensityGrid& grid) {
    if (grid.isEmpty()) {
        return 0u;

    u32 count = 0u;
    for (f32 value : grid.density) {
        if (!isFiniteDensityValue(value)) {
            ++count;
    return count;

bool hasNonFiniteDensity(const FroxelDensityGrid& grid) {
    return countNonFiniteDensities(grid) > 0u;

















    const DensityLookupRejectReason reject = classifyDensityLookupAtIndex(grid, desc, index);
    return !densityLookupRejectReasonIsBlocking(reject);

    const DensityLookupRejectReason reject = classifyDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);



DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,

bool preflightDensityLookup(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);

    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);

FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              const FroxelSampleCoords& coords,
                              FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);


























GridDensityRejectReason classifyFroxelGridDensityReject(const FroxelDensityGrid& grid,

bool preflightFroxelGridDensity(const FroxelDensityGrid& grid,
    const GridDensityRejectReason reject = classifyFroxelGridDensityReject(grid, desc, epsilon);


























bool preflightGridDensityReady(const FroxelDensityGrid& grid,





















}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtIndex(grid, desc, index, outDensity, reason);
}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityAtIndex(grid, desc, index);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtIndex(grid, desc, index, outDensity, reason);
}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        outDensity = 0.f;
        return false;

    outDensity = sampleDensityAtIndex(grid, desc, index);
    outReason = DensityLookupRejectReason::None;


    return true;
}

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            FroxelLookupRejectReason& outReason) {
                            f32 value) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryWriteDensityAtIndex(grid, desc, index, value, reason);
}

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value,
                            DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        return false;
    }

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value,
                            DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        return false;

    const bool wrote = writeDensityAtIndex(grid, desc, index, value);
    outReason = FroxelLookupRejectReason::None;
    return wrote;

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
    return trySampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ, outDensity, reason);

    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {

    outDensity = sampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ);

bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
    return tryWriteDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value, reason);


    return writeDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value);
    if (FroxelGridLayout::isEmptyGrid(clampedDesc)) {
        return grid.isEmpty();
    if (grid.isEmpty()) {
    if (!gridMatchesDesc(grid, clampedDesc)) {
    return validateDensityCounts(grid, epsilon);
bool tryValidateDensityCounts(const FroxelDensityGrid& grid,
                            DensityGridRejectReason& outReason,
        outReason = DensityGridRejectReason::None;

        outReason = DensityGridRejectReason::EmptyGridDesc;

        outReason = DensityGridRejectReason::DescMismatch;

        outReason = DensityGridRejectReason::CountPartitionMismatch;


bool validateDensityCountsForDesc(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    DensityGridRejectReason reason = DensityGridRejectReason::None;
    return tryValidateDensityCounts(grid, desc, reason, epsilon);
                            FroxelDensityRejectReason& outReason,
        outReason = FroxelDensityRejectReason::None;
        outReason = FroxelDensityRejectReason::EmptyStorage;
        outReason = FroxelDensityRejectReason::DescMismatch;


    FroxelDensityRejectReason reason = FroxelDensityRejectReason::None;

                             f32& outDensity) {
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
    outReason = DensityLookupRejectReason::None;
    writeDensityAtIndex(grid, desc, index, value);
    return true;
}

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity,
                             FroxelLookupRejectReason& outReason) {
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ, outDensity, reason);
}

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;

    outReason = FroxelLookupRejectReason::None;
    outDensity = sampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ);
    outReason = DensityLookupRejectReason::None;
    return true;

                            f32 value) {

                            f32 value,

    const bool wrote = writeDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value);
    return tryWriteDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value, reason);

                             f32& outDensity,
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, lookupReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryWriteDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value, reason);
}

bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value,
                            DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, lookupReason)) {
        return false;


    }

    const bool wrote = writeDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value);
    outReason = DensityLookupRejectReason::None;
    return wrote;
    writeDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value);
    return true;
}

f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords) {
    if (!isDensityGridAccessible(grid, desc)) {
    if (!canAccessDensityGrid(grid, desc)) {
    f32 density = 0.f;
    if (!trySampleDensityBilinear(grid, desc, coords, density)) {
        return 0.f;
    }
    return density;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity) {
    if (!isDensityGridAccessible(grid, desc)) {
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, reason)) {
        outDensity = 0.f;
        return false;
    }

    FroxelSampleCoords safeCoords = coords;
    FroxelGridLayout::clampSampleCoords(safeCoords, desc);

    const f32 d00 = froxelDensityAt(grid, desc, safeCoords.tileX0, safeCoords.tileY0, safeCoords.sliceZ0);
    const f32 d10 = froxelDensityAt(grid, desc, safeCoords.tileX1, safeCoords.tileY0, safeCoords.sliceZ0);
    const f32 d01 = froxelDensityAt(grid, desc, safeCoords.tileX0, safeCoords.tileY1, safeCoords.sliceZ0);
    const f32 d11 = froxelDensityAt(grid, desc, safeCoords.tileX1, safeCoords.tileY1, safeCoords.sliceZ0);

    const f32 d0 = lerpDensity(d00, d10, safeCoords.tx);
    const f32 d1 = lerpDensity(d01, d11, safeCoords.tx);
    outDensity = lerpDensity(d0, d1, safeCoords.ty);
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return trySampleDensityBilinear(grid, desc, coords, outDensity, reason);

}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              SampleCoordRejectReason& outReason) {
    if (!tryCanSampleAtCoords(grid, desc, coords, outReason)) {
                              DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;

    outDensity = sampleDensityBilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;

                              f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;

                              f32& outDensity,
                              DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              DensityLookupRejectReason& outLookupReason,
                              SampleCoordRejectReason& outCoordReason) {
    if (!tryCanSampleAtCoords(grid, desc, coords, outLookupReason, outCoordReason)) {
        outDensity = 0.f;
        return false;

    outDensity = sampleDensityBilinear(grid, desc, coords);
    outLookupReason = DensityLookupRejectReason::None;
    outCoordReason = SampleCoordRejectReason::None;

                              DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryAreSampleCoordsInBounds(coords, desc, coordReason)) {
        outReason = DensityLookupRejectReason::SampleCoordRejected;

    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              FroxelBilinearSampleRejectReason& outReason) {
    if (!tryCanBilinearSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              FroxelBilinearSampleRejectReason& outReason) {
    if (!tryCanBilinearSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              FroxelBilinearSampleRejectReason& outReason) {
    if (!tryCanBilinearSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              FroxelBilinearSampleRejectReason& outReason) {
    if (!tryCanBilinearSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    return true;
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity,
                              FroxelBilinearSampleRejectReason& outReason) {
    if (!tryCanBilinearSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    return true;
}

f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelGridDesc& desc,
                           const FroxelSampleCoords& coords) {
    if (!isDensityGridAccessible(grid, desc)) {
    if (!canAccessDensityGrid(grid, desc)) {
    f32 density = 0.f;
    if (!trySampleDensityTrilinear(grid, desc, coords, density)) {
        return 0.f;
    }
    return density;
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity) {
    if (!isDensityGridAccessible(grid, desc)) {
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, reason)) {
        outDensity = 0.f;
        return false;
    }

    FroxelSampleCoords safeCoords = coords;
    FroxelGridLayout::clampSampleCoords(safeCoords, desc);

    FroxelSampleCoords slice0 = safeCoords;
    slice0.sliceZ1 = slice0.sliceZ0;
    FroxelSampleCoords slice1 = safeCoords;
    slice1.sliceZ0 = slice1.sliceZ1;

    f32 nearSlice = 0.f;
    f32 farSlice = 0.f;
    if (!trySampleDensityBilinear(grid, desc, slice0, nearSlice) ||
        !trySampleDensityBilinear(grid, desc, slice1, farSlice)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = lerpDensity(nearSlice, farSlice, safeCoords.tz);
    return true;

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              SampleCoordRejectReason* reason) {
    return preflightSampleAtCoords(grid, desc, coords, reason);
}

bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
                                 const FroxelSampleCoords& coords,
                                 SampleCoordRejectReason& outReason) {
    return tryCanSampleAtCoords(grid, desc, coords, outReason);
}

bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords) {
    if (shouldSkipFroxelLookup(grid, desc)) {
        return true;
    }

    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return !FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);
}

bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords) {
    if (!isDensityGridAccessible(grid, desc)) {
        return false;
    }

    return FroxelGridLayout::wouldClampSampleCoords(coords, desc);
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return trySampleDensityTrilinear(grid, desc, coords, outDensity, reason);

                               f32& outDensity,
                               SampleCoordRejectReason& outReason) {
    if (!tryCanSampleAtCoords(grid, desc, coords, outReason)) {
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;

    outDensity = sampleDensityTrilinear(grid, desc, coords);

                               FroxelTrilinearSampleRejectReason& outReason) {
    if (!tryCanTrilinearSampleAtCoords(grid, desc, coords, outReason)) {

    const f32 nearSlice = sampleDensityBilinear(grid, desc, slice0);
    const f32 farSlice = sampleDensityBilinear(grid, desc, slice1);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityTrilinear(grid, desc, coords, outDensity, reason);
}

bool trySampleDensityBilinearAtCoords(const FroxelDensityGrid& grid,
                                      const FroxelGridDesc& desc,
                                      const FroxelSampleCoords& coords,
                                      f32& outDensity,
                                      DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtSampleCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;

bool trySampleDensityTrilinearAtCoords(const FroxelDensityGrid& grid,
bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity,
                               DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;

bool trySampleDensityBilinearInBounds(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndexInRange(grid, desc, 0u, outReason)) {

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryValidateSampleCoords(coords, desc, coordReason)) {
        outReason = DensityLookupRejectReason::SampleCoordsOutOfRange;

    outDensity = sampleDensityBilinear(grid, desc, coords);

bool trySampleDensityTrilinearInBounds(const FroxelDensityGrid& grid,



                               DensityLookupRejectReason& outLookupReason,
                               SampleCoordRejectReason& outCoordReason) {
    if (!tryCanSampleAtCoords(grid, desc, coords, outLookupReason, outCoordReason)) {

    outLookupReason = DensityLookupRejectReason::None;
    outCoordReason = SampleCoordRejectReason::None;


    if (!FroxelGridLayout::tryAreSampleCoordsInBounds(coords, desc, coordReason)) {
        outReason = DensityLookupRejectReason::SampleCoordRejected;

    return true;
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity,
                               SampleCoordRejectReason& outReason) {
    FroxelTrilinearSampleRejectReason trilinearReason = FroxelTrilinearSampleRejectReason::None;
    if (!tryPreflightTrilinearSample(grid, desc, coords, trilinearReason)) {
    if (!tryPreflightTrilinearDensitySample(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        switch (trilinearReason) {
        case FroxelTrilinearSampleRejectReason::EmptyGrid:
            outReason = SampleCoordRejectReason::EmptyGrid;
            break;
        case FroxelTrilinearSampleRejectReason::EmptyStorage:
        case FroxelTrilinearSampleRejectReason::DescMismatch:
            outReason = SampleCoordRejectReason::OutOfBounds;
            break;
        case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
            outReason = SampleCoordRejectReason::InvalidWeights;
            break;
        case FroxelTrilinearSampleRejectReason::HardOutOfBounds:
            outReason = SampleCoordRejectReason::OutOfBounds;
            break;
        case FroxelTrilinearSampleRejectReason::None:
            outReason = SampleCoordRejectReason::None;
            break;
        }
        return false;
    }

    if (trilinearReason == FroxelTrilinearSampleRejectReason::InvalidSampleCoords) {
        outReason = SampleCoordRejectReason::InvalidWeights;
    } else {
        outReason = SampleCoordRejectReason::None;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
                                 const FroxelSampleCoords& coords,
                                 FroxelTrilinearSampleRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;
            break;
        case DensityLookupRejectReason::DescMismatch:
            outReason = FroxelTrilinearSampleRejectReason::DescMismatch;
        case DensityLookupRejectReason::EmptyStorage:
            outReason = FroxelTrilinearSampleRejectReason::EmptyStorage;
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
        }
        return false;

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, coordReason)) {
        outReason = FroxelTrilinearSampleRejectReason::CoordsOutOfRange;

    if (coordReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelTrilinearSampleRejectReason::InvalidWeights;
        return true;

    outReason = FroxelTrilinearSampleRejectReason::None;

bool wouldSkipTrilinearSample(const FroxelDensityGrid& grid,
                              const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    return !tryPreflightTrilinearSample(grid, desc, coords, reason);

bool canPreflightTrilinearSample(const FroxelDensityGrid& grid,
    return tryPreflightTrilinearSample(grid, desc, coords, reason);

    if (FroxelGridLayout::isEmptyGrid(desc)) {
    if (shouldSkipFroxelLookup(grid, desc)) {
        outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        switch (sampleReason) {
        case SampleCoordRejectReason::EmptyGrid:
        case SampleCoordRejectReason::OutOfBounds:
            outReason = FroxelTrilinearSampleRejectReason::OutOfBoundsCoords;
        case SampleCoordRejectReason::InvalidWeights:
        case SampleCoordRejectReason::None:
            outReason = FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

    outReason = sampleReason == SampleCoordRejectReason::InvalidWeights
                    ? FroxelTrilinearSampleRejectReason::InvalidSampleCoords
                    : FroxelTrilinearSampleRejectReason::None;

bool shouldSkipTrilinearSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return shouldSkipFroxelLookup(grid, desc);

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               f32& outDensity,
    if (!tryCanSampleTrilinear(grid, desc, coords, outReason)) {
        outDensity = 0.f;

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    if (!tryCanSampleDensityTrilinear(grid, desc, coords, outReason)) {

    if (!tryCanSampleTrilinearAtCoords(grid, desc, coords, outReason)) {


    if (!tryCanTrilinearSampleAtCoords(grid, desc, coords, outReason)) {

    if (!tryPreflightTrilinearSample(grid, desc, coords, outReason)) {

                               DensityTrilinearSampleRejectReason& outReason) {

                               TrilinearSampleRejectReason& outReason) {







bool preflightTrilinearDensitySample(const FroxelDensityGrid& grid,
                                     SampleCoordRejectReason* reason) {
    return preflightDensitySampleAtCoords(grid, desc, coords, reason);

bool wouldSkipTrilinearDensitySample(const FroxelDensityGrid& grid,
    return wouldSkipDensitySampleAtCoords(grid, desc, coords);
bool tryCanTrilinearSample(const FroxelDensityGrid& grid,
    if (!isDensityGridAccessible(grid, desc)) {



FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
    tryCanTrilinearSample(grid, desc, coords, reason);
    return reason;

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

    return !tryCanTrilinearSample(grid, desc, coords, reason);
bool canSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);

bool tryCanSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
                                   SampleCoordRejectReason& outReason) {
    return tryCanSampleAtCoords(grid, desc, coords, outReason);

bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
    return FroxelGridLayout::wouldClampSampleCoords(coords, desc);

f32 sampleDensityAtScreen(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelCameraDesc& camera,
                          f32 screenX,
                          f32 screenY,
                          f32 viewDepth) {
    if (!isDensityGridAccessible(grid, desc)) {
    if (!canAccessDensityGrid(grid, desc)) {
    f32 density = 0.f;
    FroxelSampleRejectReason reason = FroxelSampleRejectReason::None;
    if (!trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, density, reason)) {
    FroxelLookupRejectReason lookupReason = FroxelLookupRejectReason::None;
    FroxelSampleCoordRejectReason coordReason = FroxelSampleCoordRejectReason::None;
    if (!trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, density, lookupReason, coordReason)) {
        return 0.f;
    }
    return density;
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              FroxelSampleRejectReason& outReason) {
    if (!isDensityGridAccessible(grid, desc)) {
                              f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, reason);
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, mapReason);
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, mapReason,
                                    sampleReason);
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    ScreenMappingRejectReason screenReason = ScreenMappingRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, lookupReason,
                                    screenReason);
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        outReason = FroxelSampleRejectReason::InaccessibleGrid;
                              FroxelLookupRejectReason& outLookupReason,
                              FroxelSampleCoordRejectReason& outCoordReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outLookupReason)) {
        outCoordReason = FroxelSampleCoordRejectReason::None;
                              f32& outDensity) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, reason);
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              SampleCoordRejectReason& outReason) {
    if (isEmptyGridForSampling(grid, desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords,
                                                           coordReason)) {
        if (coordReason == SampleCoordRejectReason::EmptyGrid) {
            outReason = DensityLookupRejectReason::EmptyGrid;
        } else {
            outReason = DensityLookupRejectReason::None;

    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity)) {

    outReason = FroxelSampleRejectReason::None;
    return true;

    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;

                              ScreenMappingRejectReason& outReason) {
    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    return trySampleDensityAtScreen(
        grid, desc, camera, screenX, screenY, viewDepth, outDensity, outReason, sampleReason);
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              ScreenMappingRejectReason& outMapReason,
                              SampleCoordRejectReason& outSampleReason) {
    outMapReason = ScreenMappingRejectReason::None;
    outSampleReason = SampleCoordRejectReason::None;

    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        outReason = ScreenMappingRejectReason::EmptyGrid;


    outDensity = sampleDensityTrilinear(grid, desc, coords);
            screenX, screenY, viewDepth, desc, camera, coords, outCoordReason)) {
        outLookupReason = FroxelLookupRejectReason::None;

    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, outReason)) {


    DensityLookupRejectReason reason = DensityLookupRejectReason::None;

bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const VolumetricFogParams& params) {
    if (!canPopulateFroxelGrid(desc)) {

    populateFromAnalyticFog(grid, desc, camera, params);
    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    return trySampleDensityAtScreen(
        grid, desc, camera, screenX, screenY, viewDepth, outDensity, lookupReason, sampleReason);

                              DensityLookupRejectReason& outLookupReason,
                              SampleCoordRejectReason& outSampleReason) {
        outSampleReason = SampleCoordRejectReason::None;

        outReason = lookupReason == DensityLookupRejectReason::EmptyGrid ? ScreenMappingRejectReason::EmptyGrid
                                                                         : ScreenMappingRejectReason::None;

        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            break;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::None:

    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
        grid, desc, camera, screenX, screenY, viewDepth, outDensity, mapReason, lookupReason);

                              ScreenMappingRejectReason& outMapReason,
                              DensityLookupRejectReason& outLookupReason) {
        outMapReason = ScreenMappingRejectReason::None;

        case DensityLookupRejectReason::IndexOutOfRange:
    ScreenMappingRejectReason screenReason = ScreenMappingRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, screenReason);

                              ScreenMappingRejectReason& outScreenReason) {
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, outScreenReason,
                                    sampleReason);

                              ScreenMappingRejectReason& outScreenReason,
        outScreenReason = ScreenMappingRejectReason::None;
        outSampleReason = lookupReason == DensityLookupRejectReason::EmptyGrid ? SampleCoordRejectReason::EmptyGrid
                                                                             : SampleCoordRejectReason::OutOfBounds;
                              f32& outDensity,
                              ScreenMappingRejectReason& outMapReason) {
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, outMapReason,

            outMapReason = ScreenMappingRejectReason::EmptyGrid;
        outSampleReason = SampleCoordRejectReason::OutOfBounds;
        if (lookupReason == DensityLookupRejectReason::EmptyGrid) {
            outReason = ScreenMappingRejectReason::None;
        case DensityLookupRejectReason::CoordOutOfRange:
        outReason = lookupReason == DensityLookupRejectReason::EmptyGrid ||
                            lookupReason == DensityLookupRejectReason::EmptyStorage
                        ? ScreenMappingRejectReason::EmptyGrid

                              FroxelTrilinearSampleRejectReason& outReason) {
            outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;
            outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;
    if (FroxelGridLayout::isEmptyGrid(desc) || !isDensityGridAccessible(grid, desc)) {
        outDensity = 0.f;
        return false;
    }

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, outSampleReason)) {
            screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords,
                                                           outReason)) {
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
                                                           mapReason)) {
            screenX, screenY, viewDepth, desc, camera, coords, outMapReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = ScreenMappingRejectReason::None;
    return true;

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              FroxelTrilinearSampleRejectReason& outReason) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;
    if (!isDensityGridAccessible(grid, desc)) {
        outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, mapReason)) {
        outReason = FroxelTrilinearSampleRejectReason::ScreenMappingFailed;

    if (!tryCanSampleDensityTrilinear(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    if (!tryCanSampleTrilinear(grid, desc, coords, outReason)) {
            outReason = DensityLookupRejectReason::EmptyGrid;
            screenX, screenY, viewDepth, desc, camera, coords, outMapReason)) {
        outLookupReason = DensityLookupRejectReason::None;
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
                                                           mapReason)) {
        outReason = DensityLookupRejectReason::ScreenMappingFailed;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!tryCanSampleAtCoords(grid, desc, coords, sampleReason)) {
        outReason = DensityLookupRejectReason::SampleCoordRejected;
                                                           outMapReason)) {
        outSampleReason = SampleCoordRejectReason::None;

    outReason = DensityLookupRejectReason::None;
                                                           outScreenReason)) {

    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, outSampleReason)) {

    outScreenReason = ScreenMappingRejectReason::None;
        outMapReason = ScreenMappingRejectReason::None;
        outLookupReason = DensityLookupRejectReason::ScreenMappingFailed;

    outReason = FroxelTrilinearSampleRejectReason::None;

bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelSampleCoords& /*coords*/) {
    return isDensityGridAccessible(grid, desc);

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords,
                          DensityLookupRejectReason& outLookupReason,
                          SampleCoordRejectReason& outCoordReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outLookupReason)) {
        outCoordReason = SampleCoordRejectReason::None;

    if (!FroxelGridLayout::tryCanSampleAtCoords(coords, desc, outCoordReason)) {



            screenX, screenY, viewDepth, desc, camera, coords, outCoordReason)) {


                              DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
            screenX, screenY, viewDepth, desc, camera, coords, coordReason)) {



    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outCoordReason)) {


bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return !tryCanPopulateFromAnalyticFog(desc, params, reason);

bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason) {
    if (shouldSkipFroxelGrid(desc)) {
        outReason = FroxelPopulateRejectReason::EmptyGrid;
    if (params.density <= 0.f) {
        outReason = FroxelPopulateRejectReason::ZeroDensity;
    if (params.march_steps == 0u) {
        outReason = FroxelPopulateRejectReason::ZeroMarchSteps;


bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const VolumetricFogParams& params) {
    return tryPopulateFromAnalyticFog(grid, desc, camera, params, reason);

    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, outReason)) {

    populateFromAnalyticFog(grid, desc, camera, params);
    outReason = FroxelPopulateRejectReason::None;

bool isValidPopulateCamera(const FroxelCameraDesc& camera) {
    return camera.nearPlane > 0.f && camera.farPlane > camera.nearPlane;

bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
    if (!isValidPopulateCamera(camera)) {

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
    return !canPopulateFromAnalyticFog(desc, camera, params);

        outReason = FroxelPopulateRejectReason::InvalidCamera;


bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return tryCanPopulateFromAnalyticFog(desc, camera, params, outReason);

    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, reason)) {


    PopulateRejectReason reason = PopulateRejectReason::None;
    return !tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

    return tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

                                   PopulateRejectReason& outReason) {
    if (FroxelGridLayout::isEmptyGrid(clampedDesc)) {
        outReason = PopulateRejectReason::EmptyGrid;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = PopulateRejectReason::InvalidCamera;
        outReason = PopulateRejectReason::ZeroDensity;
        outReason = PopulateRejectReason::ZeroMarchSteps;

    outReason = PopulateRejectReason::None;


    return canPopulateFromAnalyticFog(desc, camera, params);

bool validatePopulatedDensity(const FroxelDensityGrid& grid,
                              f32 epsilon) {
    if (!validateGridDensity(grid, desc, epsilon)) {

    if (FroxelGridLayout::isEmptyGrid(clampedDesc) || params.density <= 0.f || params.march_steps == 0u) {

    return hasNonZeroDensity(grid, epsilon);
        outSampleReason = SampleCoordRejectReason::OutOfBounds;

    return trySampleDensityTrilinear(grid, desc, coords, outDensity, outSampleReason);

bool tryLookupDensityFromScreen(const FroxelDensityGrid& grid,
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        outReason = lookupReason;


    FroxelTrilinearSampleRejectReason trilinearReason = FroxelTrilinearSampleRejectReason::None;
    if (!tryPreflightTrilinearSample(grid, desc, coords, trilinearReason)) {
        switch (trilinearReason) {
        case FroxelTrilinearSampleRejectReason::EmptyGrid:
            break;
        case FroxelTrilinearSampleRejectReason::DescMismatch:
            outReason = DensityLookupRejectReason::DescMismatch;
        case FroxelTrilinearSampleRejectReason::EmptyStorage:
            outReason = DensityLookupRejectReason::EmptyStorage;
        case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
        case FroxelTrilinearSampleRejectReason::HardOutOfBounds:
        case FroxelTrilinearSampleRejectReason::None:

    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, sampleReason)) {
        outReason = ScreenMappingRejectReason::EmptyGrid;

    return true;
}

bool preflightScreenDensitySample(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  f32 screenX,
                                  f32 screenY,
                                  f32 viewDepth,
                                  ScreenMappingRejectReason* reason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        if (reason != nullptr) {
            *reason = ScreenMappingRejectReason::EmptyGrid;
        }
        return false;
    }

    return FroxelGridLayout::preflightScreenDepthToSampleCoords(
        screenX, screenY, viewDepth, desc, camera, nullptr, reason);
}

bool wouldSkipScreenDensitySample(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  f32 screenX,
                                  f32 screenY,
                                  f32 viewDepth) {
    return !preflightScreenDensitySample(grid, desc, camera, screenX, screenY, viewDepth);
}

ScreenMappingRejectReason classifyScreenDensitySampleReject(const FroxelDensityGrid& grid,
                                                            const FroxelGridDesc& desc,
                                                            const FroxelCameraDesc& camera,
                                                            f32 screenX,
                                                            f32 screenY,
                                                            f32 viewDepth) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        return ScreenMappingRejectReason::EmptyGrid;
    }

    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, mapReason);
    return mapReason;
}

bool preflightScreenDensitySample(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  f32 screenX,
                                  f32 screenY,
                                  f32 viewDepth,
                                  ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason reject =
        classifyScreenDensitySampleReject(grid, desc, camera, screenX, screenY, viewDepth);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !screenMappingRejectReasonIsBlocking(reject);
}

bool wouldSkipScreenDensitySample(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  f32 screenX,
                                  f32 screenY,
                                  f32 viewDepth) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return !preflightScreenDensitySample(grid, desc, camera, screenX, screenY, viewDepth, &reason);
}

bool tryCanSampleDensityAtScreen(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
                                 const FroxelCameraDesc& camera,
                                 f32 screenX,
                                 f32 screenY,
                                 f32 viewDepth,
                                 FroxelScreenSampleRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = FroxelScreenSampleRejectReason::EmptyGrid;
            break;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelScreenSampleRejectReason::InaccessibleGrid;
            break;
        }
        return false;
    }

    FroxelSampleCoords coords{};
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, mapReason)) {
        outReason = FroxelScreenSampleRejectReason::ScreenMappingFailed;
        return false;
    }

    FroxelTrilinearSampleRejectReason trilinearReason = FroxelTrilinearSampleRejectReason::None;
    if (!tryCanTrilinearSampleAtCoords(grid, desc, coords, trilinearReason)) {
        switch (trilinearReason) {
        case FroxelTrilinearSampleRejectReason::EmptyGrid:
            outReason = FroxelScreenSampleRejectReason::EmptyGrid;
            break;
        case FroxelTrilinearSampleRejectReason::InaccessibleGrid:
            outReason = FroxelScreenSampleRejectReason::InaccessibleGrid;
            break;
        case FroxelTrilinearSampleRejectReason::InvalidSampleCoords:
            outReason = FroxelScreenSampleRejectReason::InvalidSampleCoords;
            break;
        case FroxelTrilinearSampleRejectReason::ClampableWeights:
        case FroxelTrilinearSampleRejectReason::None:
            outReason = FroxelScreenSampleRejectReason::InvalidSampleCoords;
            break;
        }
        return false;
    }

    if (trilinearReason == FroxelTrilinearSampleRejectReason::ClampableWeights) {
        outReason = FroxelScreenSampleRejectReason::ClampableWeights;
        return true;
    }

    outReason = FroxelScreenSampleRejectReason::None;
    return true;
}

bool wouldSkipDensityScreenSample(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  f32 screenX,
                                  f32 screenY,
                                  f32 viewDepth) {
    FroxelScreenSampleRejectReason reason = FroxelScreenSampleRejectReason::None;
    return !tryCanSampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, reason);
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              FroxelScreenSampleRejectReason& outReason) {
    if (!tryCanSampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth);
    return true;
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              FroxelTrilinearSampleRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        outDensity = 0.f;
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;
            break;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;
            break;
        }
        return false;
    }

    FroxelSampleCoords coords{};
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, mapReason)) {
        outDensity = 0.f;
        outReason = FroxelTrilinearSampleRejectReason::ScreenMappingFailed;
        return false;
    }

    return trySampleDensityTrilinear(grid, desc, coords, outDensity, outReason);
}

bool preflightDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              ScreenMappingRejectReason* reason) {
    const ScreenMappingRejectReason mapReject =
        FroxelGridLayout::classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (screenMappingRejectReasonIsBlocking(mapReject)) {
        if (reason != nullptr) {
            *reason = mapReject;
        }
        return false;
    }

    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!preflightDensityLookup(grid, desc, 0u, &lookupReason)) {
        if (reason != nullptr) {
            *reason = ScreenMappingRejectReason::EmptyGrid;
        }
        return false;
    }

    if (reason != nullptr) {
        *reason = ScreenMappingRejectReason::None;
    }
    return true;
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;
    }

    FroxelSampleCoords coords{};
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, mapReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    return true;
}

void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    grid.allocate(clampedDesc);
    if (FroxelGridLayout::isEmptyGrid(clampedDesc) || params.density <= 0.f || params.march_steps == 0u) {
    if (shouldSkipFroxelPopulate(clampedDesc, params)) {
        return;
    }

    for (u32 sliceZ = 0; sliceZ < clampedDesc.slicesZ; ++sliceZ) {
        const f32 sliceNear = FroxelSliceLayout::computeSliceNearZ(sliceZ, clampedDesc, camera);
        const f32 sliceFar = FroxelSliceLayout::computeSliceFarZ(sliceZ, clampedDesc, camera);
        const f32 viewDepth = 0.5f * (sliceNear + sliceFar);

        for (u32 tileY = 0; tileY < clampedDesc.tilesY; ++tileY) {
            for (u32 tileX = 0; tileX < clampedDesc.tilesX; ++tileX) {
                const u32 index = FroxelGridLayout::froxelIndex(tileX, tileY, sliceZ, clampedDesc);
                const math::Vec3 world_pos{camera.position.x,
                                           params.base_height + viewDepth * 0.01f,
                                           camera.position.z};
                grid.density[index] = sample_volumetric_fog_density(params, world_pos);
            }
        }
    }
}

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return !tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                                                        const VolumetricFogParams& params) {
}


    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    return FroxelPopulateRejectReason::None;



                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelPopulateRejectReasonIsBlocking(reject);
    return reject != FroxelPopulateRejectReason::None;
bool tryShouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                                 FroxelPopulateRejectReason& outReason) {
    const bool canFill = tryCanPopulateFromAnalyticFog(desc, camera, params, outReason);
    return !canFill;
bool wouldSkipAnalyticPopulate(const FroxelGridDesc& desc,
    return !tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
bool wouldPopulateAllocateOnly(const FroxelGridDesc& desc,
                               const FroxelCameraDesc& /*camera*/,
bool wouldPopulateAllocateWithoutFill(const FroxelGridDesc& desc,
    return canAllocateFroxelGridForPopulate(desc) && shouldSkipFroxelPopulate(desc, camera, params);

bool canAllocateFroxelGridForPopulate(const FroxelGridDesc& desc) {
    return FroxelGridDesc::clampCounts(desc).froxelCount() > 0u;

bool tryPreflightPopulateAllocation(const FroxelGridDesc& desc, FroxelPopulateRejectReason& outReason) {
    if (!canAllocateFroxelGridForPopulate(desc)) {
FroxelPopulateRejectReason classifyPopulateReject(const FroxelGridDesc& desc,

bool preflightPopulate(const FroxelGridDesc& desc,
    const FroxelPopulateRejectReason reject = classifyPopulateReject(desc, camera, params);

bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
        outReason = FroxelPopulateRejectReason::EmptyDesc;
        return false;

    outReason = FroxelPopulateRejectReason::None;
    return true;

        outReason = FroxelPopulateRejectReason::ZeroDensity;
        outReason = FroxelPopulateRejectReason::ZeroMarchSteps;
        outReason = FroxelPopulateRejectReason::InvalidCamera;


    return clampedDesc.froxelCount() > 0u && (params.density <= 0.f || params.march_steps == 0u);



bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return tryCanPopulateFromAnalyticFog(desc, camera, params, reason);


bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
    return tryPopulateFromAnalyticFog(grid, desc, camera, params, reason);

    return !canPopulateFromAnalyticFog(desc, camera, params);

    FroxelPopulateRejectReason localReason = FroxelPopulateRejectReason::None;
    const bool ok = tryCanPopulateFromAnalyticFog(desc, camera, params, localReason);
        *reason = localReason;
    return ok;

bool tryPreflightFroxelPopulate(const FroxelGridDesc& desc,
    return tryCanPopulateFromAnalyticFog(desc, camera, params, outReason);

bool froxelPopulateReady(const FroxelGridDesc& desc,
    return canPopulateFromAnalyticFog(desc, camera, params);
    return preflightPopulateFromAnalyticFog(desc, camera, params);

bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return preflightPopulateFromAnalyticFog(desc, camera, params, reason);

                                      FroxelPopulateRejectReason* outReason) {
    const bool canFill = tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    if (outReason != nullptr) {
        *outReason = reason;
    return canFill;

bool needsPopulateReallocate(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !grid.matchesDesc(desc);

bool tryPreflightPopulate(const FroxelGridDesc& desc,
                          const FroxelDensityGrid& grid,
    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, outReason)) {

    if (!grid.isEmpty() && !gridMatchesDesc(grid, desc)) {


bool canPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,

bool tryPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,

    return tryPreflightFroxelPopulate(desc, camera, params, outReason);

    return preflightFroxelPopulate(desc, camera, params);

bool canPopulateFromAnalyticFogForDesc(const FroxelGridDesc& desc) {
    return clampedDesc.froxelCount() > 0u;

bool tryValidatePopulateResult(const FroxelDensityGrid& grid,
    if (!grid.matchesDesc(clampedDesc)) {

    FroxelPopulateRejectReason populateReason = FroxelPopulateRejectReason::None;
    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, populateReason)) {
        return countNonZeroFroxels(grid) == 0u;

    if (!hasNonZeroDensity(grid)) {
    outReason = classifyFroxelPopulateReject(desc, camera, params);
    return !froxelPopulateRejectReasonIsBlocking(outReason);

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,



DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    return !densityLookupRejectReasonIsBlocking(reject);



bool wouldSkipPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return !preflightPopulateFromAnalyticFog(desc, camera, params);
    FroxelPopulateRejectReason rejectReason = FroxelPopulateRejectReason::None;
    const bool ok = tryCanPopulateFromAnalyticFog(desc, camera, params, rejectReason);
        *reason = rejectReason;
    outReason = classifyPopulateReject(desc, camera, params);












bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,







bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    }
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    }
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    }

    return FroxelPopulateRejectReason::None;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera,
                                                      const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    }
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    }
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    }
    return FroxelPopulateRejectReason::None;
}

bool preflightPopulate(const FroxelGridDesc& desc,
                       const FroxelCameraDesc& camera,
                       const VolumetricFogParams& params,
                       FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    }
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    }
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    }

    return FroxelPopulateRejectReason::None;
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    }
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    }
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    }

    return FroxelPopulateRejectReason::None;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera,
                                                      const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    }
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    }
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    }

    return FroxelPopulateRejectReason::None;
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    }
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    }
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;
    }
    return FroxelPopulateRejectReason::None;
}

bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  const VolumetricFogParams& params,
                                  FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const FroxelCameraDesc& camera,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason) {
    outReason = classifyFroxelPopulateReject(desc, camera, params);
    return !froxelPopulateRejectReasonIsBlocking(outReason);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;

    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return FroxelPopulateRejectReason::EmptyDesc;
    if (params.density <= 0.f) {
        return FroxelPopulateRejectReason::ZeroDensity;
    if (params.march_steps == 0u) {
        return FroxelPopulateRejectReason::ZeroMarchSteps;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return FroxelPopulateRejectReason::InvalidCamera;

    return FroxelPopulateRejectReason::None;

bool preflightFroxelPopulate(const FroxelGridDesc& desc,

                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelPopulateRejectReasonIsBlocking(reject);

















DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);

                                                      u32 tileX,
                                                      u32 tileY,
                                                      u32 sliceZ) {
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);


FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool preflightDensityLookupReady(const FroxelDensityGrid& grid,
                                 u32 index,
                                 DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    return !densityLookupRejectReasonIsBlocking(reject);

                                 u32 sliceZ,
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);

bool preflightGridDensityReady(const FroxelDensityGrid& grid,
                               GridDensityRejectReason* reason,
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    return !gridDensityRejectReasonIsBlocking(reject);

bool preflightFroxelTrilinearSampleReady(const FroxelDensityGrid& grid,
                                         const FroxelSampleCoords& coords,
                                         FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,













bool preflightPopulateReady(const FroxelGridDesc& desc,








}

bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
}

bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                      const VolumetricFogParams& params,
                                      FroxelPopulateRejectReason* reason) {
    FroxelPopulateRejectReason localReason = FroxelPopulateRejectReason::None;
    const bool ok = tryCanPopulateFromAnalyticFog(desc, camera, params, localReason);
    if (reason != nullptr) {
        *reason = localReason;
    return ok;

bool tryPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                         FroxelPopulateRejectReason& outReason) {
    return tryCanPopulateFromAnalyticFog(desc, camera, params, outReason);
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                          const FroxelCameraDesc& camera,
                                                          const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
    return froxelPopulateRejectReasonIsBlocking(classifyFroxelPopulateReject(desc, camera, params));
    return !preflightFroxelPopulate(desc, camera, params);
}


bool preflightFroxelPopulate(const FroxelGridDesc& desc,
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
        *reason = reject;
    return !froxelPopulateRejectReasonIsBlocking(reject);
}


bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
    return froxelPopulateRejectReasonIsBlocking(classifyFroxelPopulateReject(desc, camera, params));

                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    if (reason != nullptr) {
FroxelPopulateRejectReason classifyPopulateReject(const FroxelGridDesc& desc,
                                                  const VolumetricFogParams& params) {

    const FroxelPopulateRejectReason reject = classifyPopulateReject(desc, camera, params);
FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,

}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    if (reason != nullptr) {
        *reason = reject;
    return !froxelPopulateRejectReasonIsBlocking(reject);
    const FroxelPopulateRejectReason reject = classifyPopulateReject(desc, camera, params);
    }
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);

bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return tryPopulateFromAnalyticFog(grid, desc, camera, params, reason);
}

bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params,
                                FroxelPopulateRejectReason& outReason) {
    const bool canFill = tryCanPopulateFromAnalyticFog(desc, camera, params, outReason);
    populateFromAnalyticFog(grid, desc, camera, params);
    return canFill;
                                FroxelGridRejectReason& outGridReason,
                                FroxelCameraRejectReason& outCameraReason) {
    outGridReason = FroxelGridRejectReason::None;
    outCameraReason = FroxelCameraRejectReason::None;

    if (!tryValidateFroxelGridDesc(desc, outGridReason)) {
        return false;
    }
    if (!FroxelSliceLayout::tryValidateCamera(camera, outCameraReason)) {
    if (params.density <= 0.f || params.march_steps == 0u) {

    return true;
}

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon) {
    return tryValidateGridDensity(grid, desc, outReason, epsilon);
}

bool validatePopulateResult(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            const VolumetricFogParams& params,
                            f32 epsilon) {
    if (!validateGridDensityForDesc(grid, desc, epsilon)) {
        return false;
    }

    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        return true;
    }
    if (shouldSkipFroxelPopulate(desc, FroxelCameraDesc{}, params)) {
        return isDensityFullyEmpty(grid, desc, epsilon);
    }
    return countNonZeroFroxelsForDesc(grid, desc, epsilon) == clampedDesc.froxelCount();
}

bool preflightTrilinearDensitySample(const FroxelDensityGrid& grid,
                                     const FroxelGridDesc& desc,
                                     const FroxelSampleCoords& coords,
                                     SampleCoordRejectReason* reason) {
    SampleCoordRejectReason rejectReason = SampleCoordRejectReason::None;
    const bool ok = tryCanSampleAtCoords(grid, desc, coords, rejectReason);
    if (reason != nullptr) {
        *reason = rejectReason;
    }
    return ok;
}

bool preflightDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              ScreenMappingRejectReason* reason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        if (reason != nullptr) {
            *reason = ScreenMappingRejectReason::EmptyGrid;
        }
        return false;
    }

    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    const bool ok = FroxelGridLayout::tryMapScreenDepthToSampleCoords(
        screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
    if (reason != nullptr) {
        *reason = rejectReason;
    }
    return ok;
}

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
                                                           const FroxelGridDesc& desc,
                                                           u32 tileX,
                                                           u32 tileY,
                                                           u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookupReady(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
                                 u32 index,
                                 DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool preflightDensityLookupCoordReady(const FroxelDensityGrid& grid,
                                      const FroxelGridDesc& desc,
                                      u32 tileX,
                                      u32 tileY,
                                      u32 sliceZ,
                                      DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject =
        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                    const FroxelGridDesc& desc,
                                                                    const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelTrilinearSampleReady(const FroxelDensityGrid& grid,
                                           const FroxelGridDesc& desc,
                                           const FroxelSampleCoords& coords,
                                           FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  const FroxelGridDesc& desc,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;
}

bool preflightGridDensityReady(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               GridDensityRejectReason* reason,
                               f32 epsilon) {
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !gridDensityRejectReasonIsBlocking(reject);
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
                                  const VolumetricFogParams& params,
                                  FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelPopulateRejectReasonIsBlocking(reject);
}

} // namespace froxel_util

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightFroxelTrilinearSampleReady(const FroxelDensityGrid& grid,
                                         const FroxelSampleCoords& coords,
                                         FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject =
        classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  const FroxelGridDesc& desc,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    froxel_util::tryValidateGridDensity(grid, desc, reason, epsilon);
    return reason;
}

bool preflightGridDensityReady(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               GridDensityRejectReason* reason,
                               f32 epsilon) {
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !gridDensityRejectReasonIsBlocking(reject);

DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
                                                      const FroxelGridDesc& desc,
                                                      u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    froxel_util::tryCanLookupAtIndex(grid, desc, index, reason);
    return reason;
}

DensityLookupRejectReason classifyDensityLookupRejectAtCoord(const FroxelDensityGrid& grid,
                                                             const FroxelGridDesc& desc,
                                                             u32 tileX,
                                                             u32 tileY,
                                                             u32 sliceZ) {
DensityLookupRejectReason classifyDensityLookupReject(const FroxelDensityGrid& grid,
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    froxel_util::tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return reason;
}

bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, index);
    if (reason != nullptr) {
        *reason = reject;
    return !densityLookupRejectReasonIsBlocking(reject);
bool preflightDensityLookupReady(const FroxelDensityGrid& grid,
    }

bool preflightDensityLookupAtCoordReady(const FroxelDensityGrid& grid,
                                        u32 tileX,
                                        u32 tileY,
                                        u32 sliceZ,
    const DensityLookupRejectReason reject =
        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
                                                        const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;
}

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelPopulateRejectReasonIsBlocking(reject);

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
                                                                      const FroxelGridDesc& desc,
                                                                      const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);


bool preflightDensityLookup(const FroxelDensityGrid& grid,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            DensityLookupRejectReason* reason) {
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);

GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid,
                                                  f32 epsilon) {
    GridDensityRejectReason reason = GridDensityRejectReason::None;
    froxel_util::tryValidateGridDensity(grid, desc, reason, epsilon);

bool preflightGridDensity(const FroxelDensityGrid& grid,
                          GridDensityRejectReason* reason,
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc, epsilon);
    return !gridDensityRejectReasonIsBlocking(reject);
bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,
                                  const FroxelCameraDesc& camera,
    }

namespace {

struct VolumetricFogPassUserData {
    VolumetricFogParams params{};
    VolumetricFogPassStats stats{};
};

void executeVolumetricFogPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* pass = static_cast<VolumetricFogPassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }

    recorder->beginPass("volumetric_fog");
    record_volumetric_fog_pass(pass->params, pass->stats);
    recorder->endPass();
}

VolumetricFogPassUserData g_volumetricFogPasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_volumetricFogAccesses[RenderGraph::kMaxPassesPerFrame * 4]{};
u32 g_volumetricFogPassCount = 0;

} // namespace

f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos) {
    const f32 height_delta = world_pos.y - params.base_height;
    const f32 height_factor = std::exp(-params.height_falloff * std::max(0.f, height_delta));
    return params.density * height_factor;
}

bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats) {
    if (params.march_steps == 0u || params.density <= 0.f) {
        stats.ready = false;
        stats.lastDensity = 0.f;
        return false;
    }

    stats.ready = true;
    stats.lastDensity = sample_volumetric_fog_density(params, {0.f, 0.f, 0.f});
    ++stats.framesRecorded;
    return true;
}

void resetVolumetricFogPassGraphStorage() {
    g_volumetricFogPassCount = 0;
}

void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count) {
    if (g_volumetricFogPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_volumetricFogPassCount++;
    VolumetricFogPassUserData& userData = g_volumetricFogPasses[passIndex];
    userData.params = VolumetricFogParams{};
    userData.stats = {};

    const RGTextureAccess* accesses = nullptr;
    u32 resolvedCount = 0u;
    if (depth_read != nullptr && access_count > 0u) {
        const u32 base = passIndex * 4u;
        for (u32 i = 0; i < access_count; ++i) {
            g_volumetricFogAccesses[base + i] = depth_read[i];
        }
        accesses = &g_volumetricFogAccesses[base];
        resolvedCount = access_count;
    }

    RGPassDesc pass{};
    pass.name = "volumetric_fog";
    pass.execute = executeVolumetricFogPass;
    pass.userData = &userData;
    pass.textureAccesses = accesses;
    pass.textureAccessCount = resolvedCount;
    pass.isCuda = true;
    graph.addPass(pass);
}

} // namespace fuse::renderer
