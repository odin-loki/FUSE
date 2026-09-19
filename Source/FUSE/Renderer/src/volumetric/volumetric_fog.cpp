#include <fuse/renderer/volumetric/volumetric_fog.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

bool isValidFroxelCamera(const FroxelCameraDesc& camera) {
    return camera.nearPlane > 0.f && camera.farPlane > camera.nearPlane;
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
}

bool FroxelSliceLayout::isCameraValid(const FroxelCameraDesc& camera) {
    FroxelCameraRejectReason reason = FroxelCameraRejectReason::None;
    return tryValidateCamera(camera, reason);
}

bool FroxelSliceLayout::tryValidateCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason) {
    if (camera.nearPlane <= 0.f) {
        outReason = FroxelCameraRejectReason::InvalidNearPlane;
        return false;
    }
    if (camera.farPlane <= 0.f) {
        outReason = FroxelCameraRejectReason::InvalidFarPlane;
        return false;
    }
    if (camera.farPlane <= camera.nearPlane) {
        outReason = FroxelCameraRejectReason::InvertedDepthRange;
        return false;
    }

    outReason = FroxelCameraRejectReason::None;
    return true;
}

bool FroxelSliceLayout::isSliceIndexOutOfRange(u32 sliceZ, const FroxelGridDesc& desc) {
    return desc.slicesZ == 0u || sliceZ >= desc.slicesZ;
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

bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (!areSampleCoordsInBounds(coords, desc)) {
        return false;
    }

    return coords.tileX0 <= coords.tileX1 && coords.tileY0 <= coords.tileY1 && coords.sliceZ0 <= coords.sliceZ1;
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
        return false;
    }

    if (!mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords)) {
        outReason = SampleCoordRejectReason::DepthOutOfRange;
        return false;
    }

    outReason = SampleCoordRejectReason::None;
    return true;
}

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords,
                                            const FroxelGridDesc& desc,
                                            SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;

    clampSampleCoords(coords, desc);
    outReason = SampleCoordRejectReason::None;
    return true;





bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {

    return coords.tileX0 < desc.tilesX && coords.tileY0 < desc.tilesY && coords.sliceZ0 < desc.slicesZ &&
           coords.tileX1 < desc.tilesX && coords.tileY1 < desc.tilesY && coords.sliceZ1 < desc.slicesZ &&
           coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;


    return true;
}

bool FroxelGridLayout::canMapScreenDepth(f32 viewDepth, const FroxelCameraDesc& camera) {
    return viewDepth >= camera.nearPlane && viewDepth <= camera.farPlane;
}

bool FroxelGridLayout::tryAreSampleCoordsInBounds(const FroxelSampleCoords& coords,
                                                  const FroxelGridDesc& desc,
                                                  SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }
    if (!areSampleCoordsInBounds(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfBounds;
        return false;
    }

    outReason = SampleCoordRejectReason::None;
    return true;
}

void FroxelGridLayout::normalizeFroxelSampleCoords(FroxelSampleCoords& coords) {
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

bool FroxelGridLayout::isValidFroxelSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
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
        return false;
    }

    return coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;
}

bool FroxelGridLayout::buildFroxelSampleCoords(f32 screenX,
                                               f32 screenY,
                                               f32 viewDepth,
                                               const FroxelGridDesc& desc,
                                               const FroxelCameraDesc& camera,
                                               FroxelSampleCoords& outCoords) {
    return mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords);
}

void FroxelGridLayout::normalizeSampleCoords(FroxelSampleCoords& coords) {
    coords.tx = clamp01(coords.tx);
    coords.ty = clamp01(coords.ty);
    coords.tz = clamp01(coords.tz);
}

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
    }
    return "unknown";
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::TileOutOfRange:
        return "tile_out_of_range";
    case SampleCoordRejectReason::WeightOutOfRange:
        return "weight_out_of_range";
    }
    return "unknown";
}

const char* froxelScreenMappingRejectReasonLabel(FroxelScreenMappingRejectReason reason) {
    switch (reason) {
    case FroxelScreenMappingRejectReason::None:
        return "none";
    case FroxelScreenMappingRejectReason::EmptyGrid:
        return "empty_grid";
    case FroxelScreenMappingRejectReason::InvalidCamera:
        return "invalid_camera";
    case FroxelScreenMappingRejectReason::DepthBelowNear:
        return "depth_below_near";
    case FroxelScreenMappingRejectReason::DepthAboveFar:
        return "depth_above_far";
    }
    return "unknown";
}

bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,
                                               const FroxelGridDesc& desc,
                                               SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }

    const u32 maxTileX = desc.tilesX - 1u;
    const u32 maxTileY = desc.tilesY - 1u;
    const u32 maxSliceZ = desc.slicesZ - 1u;
    if (coords.tileX0 > maxTileX || coords.tileY0 > maxTileY || coords.sliceZ0 > maxSliceZ ||
        coords.tileX1 > maxTileX || coords.tileY1 > maxTileY || coords.sliceZ1 > maxSliceZ) {
        outReason = SampleCoordRejectReason::TileOutOfRange;
        return false;
    }
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = SampleCoordRejectReason::WeightOutOfRange;
        return false;
    }

    outReason = SampleCoordRejectReason::None;
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
        return false;
    }
    if (viewDepth < camera.nearPlane) {
        outReason = FroxelScreenMappingRejectReason::DepthBelowNear;
        return false;
    }
    if (viewDepth > camera.farPlane) {
        outReason = FroxelScreenMappingRejectReason::DepthAboveFar;
        return false;
    }

    if (!mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords)) {
        outReason = FroxelScreenMappingRejectReason::EmptyGrid;
        return false;
    }

    outReason = FroxelScreenMappingRejectReason::None;
    return true;
}

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera,
                                                      u32& outFroxelIndex,
                                                      FroxelScreenMappingRejectReason& outReason) {
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
        return false;
    }

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    outReason = FroxelScreenMappingRejectReason::None;
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

bool FroxelGridLayout::isValidSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        return false;
    }

    return areSampleCoordsInBounds(coords, desc);
}

bool FroxelGridLayout::tryCanSampleAtCoords(const FroxelSampleCoords& coords,
                                            const FroxelGridDesc& desc,
                                            SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }
    if (coords.tileX0 > coords.tileX1 || coords.tileY0 > coords.tileY1 || coords.sliceZ0 > coords.sliceZ1) {
        outReason = SampleCoordRejectReason::InvertedCorners;
        return false;
    }
    if (!areSampleCoordsInBounds(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfRange;
        return false;
    }

    outReason = SampleCoordRejectReason::None;
    return true;
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

bool FroxelGridLayout::wouldClampSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;

    return isSampleCoordsOutOfRange(coords, desc) || !isValidSampleCoords(coords, desc);

bool FroxelGridLayout::tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                                const FroxelGridDesc& desc,
                                                SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;

    if (isValidSampleCoords(coords, desc)) {
        outReason = SampleCoordRejectReason::None;
        return true;

    if (!areSampleCoordsInBounds(coords, desc)) {
        const bool indicesInRange =
            coords.tileX0 <= desc.tilesX - 1u && coords.tileY0 <= desc.tilesY - 1u &&
            coords.sliceZ0 <= desc.slicesZ - 1u && coords.tileX1 <= desc.tilesX - 1u &&
            coords.tileY1 <= desc.tilesY - 1u && coords.sliceZ1 <= desc.slicesZ - 1u;
        if (indicesInRange) {
            outReason = SampleCoordRejectReason::InvalidWeights;

        outReason = SampleCoordRejectReason::OutOfBounds;


bool FroxelGridLayout::canPreflightSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryPreflightSampleCoords(coords, desc, reason);

bool FroxelGridLayout::wouldSkipSampleCoordPreflight(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return !tryPreflightSampleCoords(coords, desc, reason);

SampleCoordRejectReason FroxelGridLayout::classifySampleCoordReject(const FroxelSampleCoords& coords,
                                                                    const FroxelGridDesc& desc) {
    tryPreflightSampleCoords(coords, desc, reason);
    return reason;

bool FroxelGridLayout::preflightSampleCoords(const FroxelSampleCoords& coords,
                                             SampleCoordRejectReason* reason) {
    const SampleCoordRejectReason reject = classifySampleCoordReject(coords, desc);
    if (reason != nullptr) {
        *reason = reject;
    return !sampleCoordRejectReasonIsBlocking(reject);

ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 screenX,
                                                                        f32 screenY,
                                                                        f32 viewDepth,
                                                                        const FroxelCameraDesc& camera) {
    FroxelSampleCoords coords{};
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);

bool FroxelGridLayout::preflightScreenMapping(f32 screenX,
                                              const FroxelCameraDesc& camera,
                                              FroxelSampleCoords* outCoords,
                                              ScreenMappingRejectReason* reason) {
    ScreenMappingRejectReason reject = ScreenMappingRejectReason::None;
    const bool mapped = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (outCoords != nullptr) {
        *outCoords = coords;
    return mapped && !screenMappingRejectReasonIsBlocking(reject);

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

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    case SampleCoordRejectReason::None:
    case SampleCoordRejectReason::EmptyGrid:
    case SampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
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
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
        return "clampable_weights";
    }
    return "unknown";

bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason) {
        return false;
        return true;

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
    }
    return "unknown";
}

bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason) {
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

const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason) {
    case FroxelPopulateRejectReason::None:
    case FroxelPopulateRejectReason::EmptyDesc:
        return "empty_desc";
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

    switch (reason) {
        return "none";
        return "empty_grid";
    case SampleCoordRejectReason::OutOfRange:
        return "out_of_range";
    case DensityLookupRejectReason::SampleCoordsOutOfRange:
        return "sample_coords_out_of_range";
    }

    }
    return "unknown";
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::OutOfRange:
        return "out_of_range";
    case SampleCoordRejectReason::InvertedCorners:
        return "inverted_corners";
    }
    return "unknown";
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::DepthBelowNear:
        return "depth_below_near";
    case SampleCoordRejectReason::DepthAboveFar:
        return "depth_above_far";
    case SampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
    }
    return "unknown";
}

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

bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         u32 /*index*/,
                         DensityLookupRejectReason& outReason) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = DensityLookupRejectReason::EmptyGrid;
        return false;
    if (grid.isEmpty()) {
        outReason = DensityLookupRejectReason::EmptyStorage;
    if (!gridMatchesDesc(grid, desc)) {
        outReason = DensityLookupRejectReason::DescMismatch;

    outReason = DensityLookupRejectReason::None;
    if (FroxelGridLayout::isFroxelIndexOutOfRange(index, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;
    return true;

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

bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelSampleCoords& /*coords*/) {
    return canLookupAtIndex(grid, desc, 0u);

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords,
                          SampleCoordRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        switch (lookupReason) {
        case DensityLookupRejectReason::EmptyGrid:
            outReason = SampleCoordRejectReason::EmptyGrid;
            break;
        case DensityLookupRejectReason::EmptyStorage:
        case DensityLookupRejectReason::DescMismatch:
            outReason = SampleCoordRejectReason::OutOfBounds;
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = SampleCoordRejectReason::None;

    return FroxelGridLayout::tryPreflightSampleCoords(coords, desc, outReason);

bool canTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

FroxelTrilinearSampleRejectReason classifyFroxelTrilinearSampleReject(const FroxelDensityGrid& grid,
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   FroxelTrilinearSampleRejectReason& outReason) {
            outReason = FroxelTrilinearSampleRejectReason::EmptyGrid;
            outReason = FroxelTrilinearSampleRejectReason::InaccessibleGrid;

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        outReason = FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelTrilinearSampleRejectReason::ClampableWeights;

    outReason = FroxelTrilinearSampleRejectReason::None;

bool wouldSkipDensityTrilinearSample(const FroxelDensityGrid& grid,
    return !tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

bool canAccessDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
    return !FroxelGridLayout::isEmptyGrid(desc) && gridMatchesDesc(grid, desc);

bool canSampleDensityAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
bool canSampleAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {

bool canSampleAtCoord(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc) {
    }
        return false;


bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryCanLookupAtIndex(grid, desc, index, reason);
}

bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
    return tryCanLookupAtIndex(grid, desc, index, reason);

                         u32 /*index*/,
                         FroxelLookupRejectReason& outReason) {
        outReason = FroxelLookupRejectReason::EmptyGrid;
        outReason = FroxelLookupRejectReason::EmptyStorage;
        outReason = FroxelLookupRejectReason::DescMismatch;

    outReason = FroxelLookupRejectReason::None;
bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        return false;
    }
    if (grid.isEmpty()) {
    if (!gridMatchesDesc(grid, desc)) {

    return true;
bool isEmptyGridForSampling(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return FroxelGridLayout::isEmptyGrid(desc) || grid.isEmpty();

bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelSampleCoords& coords) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryCanSampleAtCoords(grid, desc, coords, reason);

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelSampleCoords& coords,
                          SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;
    if (FroxelGridLayout::isSampleCoordsOutOfRange(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfRange;

    outReason = SampleCoordRejectReason::None;

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
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryCanLookupAtIndexInRange(grid, desc, index, reason);

bool tryCanLookupAtIndexInRange(const FroxelDensityGrid& grid,

    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
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
        outReason = GridDensityRejectReason::DescMismatch;
        return false;
    }
        outReason = GridDensityRejectReason::DensityCountMismatch;
        return false;
    if (!validateDensityCounts(grid, epsilon)) {
        outReason = GridDensityRejectReason::DensityCountMismatch;


bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             u32 index,
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtIndex(grid, desc, index, outDensity, reason);

                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
}

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        outReason = GridDensityRejectReason::None;
        return true;
    }
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = GridDensityRejectReason::EmptyDesc;
        return false;
    }
    if (!gridMatchesDesc(grid, desc)) {
        if (grid.density.size() < clampedDesc.froxelCount()) {
            outReason = GridDensityRejectReason::UndersizedStorage;
        } else {
            outReason = GridDensityRejectReason::DescMismatch;
        }
        return false;
    }

    return tryValidateGridDensity(grid, desc, outReason, epsilon);
}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             FroxelLookupRejectReason& outReason) {
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
    return trySampleDensityAtIndex(grid, desc, index, outDensity, reason);
}

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        outDensity = 0.f;

    outDensity = sampleDensityAtIndex(grid, desc, index);
    outReason = FroxelLookupRejectReason::None;
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool tryValidateGridDensityStrict(const FroxelDensityGrid& grid,
                                  const FroxelGridDesc& desc,
                                  GridDensityRejectReason& outReason,
                                  f32 epsilon) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = GridDensityRejectReason::EmptyDesc;
        return false;
    }
    return tryValidateGridDensity(grid, desc, outReason, epsilon);

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    const u32 froxelCount = clampedDesc.froxelCount();

    if (froxelCount == 0u) {
        outReason = GridDensityRejectReason::None;
        return true;
        outReason = GridDensityRejectReason::EmptyGrid;
    if (!gridMatchesDesc(grid, desc)) {
        outReason = GridDensityRejectReason::DescMismatch;


bool canPopulateFroxelGrid(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    if (shouldSkipFroxelGrid(desc)) {
    if (params.density <= 0.f || params.march_steps == 0u) {

bool tryValidateSampleCoords(const FroxelSampleCoords& coords,
                             SampleCoordRejectReason& outReason) {
        outReason = SampleCoordRejectReason::EmptyGrid;
    if (FroxelGridLayout::isSampleCoordsOutOfRange(coords, desc)) {
        outReason = SampleCoordRejectReason::OutOfRange;

    outReason = SampleCoordRejectReason::None;

bool trySampleDensityAtIndex(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 index,
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtIndex(grid, desc, index, outDensity, reason);
}

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            f32 value) {
    return tryWriteDensityAtIndex(grid, desc, index, value, reason);

                            f32 value,

    return writeDensityAtIndex(grid, desc, index, value);
    FroxelLookupRejectReason reason = FroxelLookupRejectReason::None;
}

bool trySampleDensityAtIndexBounds(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 index,
                                   f32& outDensity,
                                   DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndexBounds(grid, desc, index, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = grid.density[index];
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    const u32 froxelCount = clampedDesc.froxelCount();

    if (froxelCount == 0u) {
        outReason = GridDensityRejectReason::None;
        return true;
    }
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = GridDensityRejectReason::EmptyDesc;
        return false;
    }
    if (!gridMatchesDesc(grid, desc)) {
        outReason = GridDensityRejectReason::UndersizedStorage;
        return false;
    }

    return tryValidateGridDensity(grid, desc, outReason, epsilon);
}

bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   GridDensityRejectReason& outReason,
                                   f32 epsilon) {
    return tryValidateGridDensity(grid, desc, outReason, epsilon);
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
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return tryWriteDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value, reason);
}

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
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
                            DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        return false;

    outReason = DensityLookupRejectReason::None;
    return wrote;
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
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityBilinear(grid, desc, coords, outDensity, reason);
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
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    outLookupReason = DensityLookupRejectReason::None;
    outCoordReason = SampleCoordRejectReason::None;
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
}

bool trySampleDensityTrilinearAtCoords(const FroxelDensityGrid& grid,
                                       const FroxelGridDesc& desc,
                                       const FroxelSampleCoords& coords,
                                       f32& outDensity,
                                       DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtSampleCoords(grid, desc, coords, outReason)) {
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
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityBilinearInBounds(const FroxelDensityGrid& grid,
                                      const FroxelGridDesc& desc,
                                      const FroxelSampleCoords& coords,
                                      f32& outDensity,
                                      DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndexInRange(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;
    }

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryValidateSampleCoords(coords, desc, coordReason)) {
        outDensity = 0.f;
        outReason = DensityLookupRejectReason::SampleCoordsOutOfRange;
        return false;
    }

    outDensity = sampleDensityBilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityTrilinearInBounds(const FroxelDensityGrid& grid,
                                       const FroxelGridDesc& desc,
                                       const FroxelSampleCoords& coords,
                                       f32& outDensity,
                                       DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndexInRange(grid, desc, 0u, outReason)) {
        outDensity = 0.f;
        return false;
    }

    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryValidateSampleCoords(coords, desc, coordReason)) {
        outDensity = 0.f;
        outReason = DensityLookupRejectReason::SampleCoordsOutOfRange;
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
                               DensityLookupRejectReason& outLookupReason,
                               SampleCoordRejectReason& outCoordReason) {
    if (!tryCanSampleAtCoords(grid, desc, coords, outLookupReason, outCoordReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outLookupReason = DensityLookupRejectReason::None;
    outCoordReason = SampleCoordRejectReason::None;
    return true;
}

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
    }

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords,
                                                           coordReason)) {
        outDensity = 0.f;
        if (coordReason == SampleCoordRejectReason::EmptyGrid) {
            outReason = DensityLookupRejectReason::EmptyGrid;
        } else {
            outReason = DensityLookupRejectReason::None;
        }
        return false;
    }

    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity)) {
        outReason = FroxelSampleRejectReason::InaccessibleGrid;

    outReason = FroxelSampleRejectReason::None;
    return true;

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, reason);

                              f32& outDensity,
                              ScreenMappingRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        outReason = ScreenMappingRejectReason::EmptyGrid;

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(

    outDensity = sampleDensityTrilinear(grid, desc, coords);
            screenX, screenY, viewDepth, desc, camera, coords, outCoordReason)) {
        outLookupReason = FroxelLookupRejectReason::None;

    outCoordReason = FroxelSampleCoordRejectReason::None;
    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, outReason)) {
        return false;
    }

    outReason = DensityLookupRejectReason::None;
    return true;

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, reason);

bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const VolumetricFogParams& params) {
    if (!canPopulateFroxelGrid(desc)) {

    populateFromAnalyticFog(grid, desc, camera, params);
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
    if (!FroxelGridLayout::mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords)) {
        outDensity = 0.f;
        if (FroxelGridLayout::isEmptyGrid(desc)) {
            outReason = DensityLookupRejectReason::EmptyGrid;
        }
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelGridDesc& desc,
                       const FroxelSampleCoords& /*coords*/) {
    return isDensityGridAccessible(grid, desc);
}

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords,
                          DensityLookupRejectReason& outLookupReason,
                          SampleCoordRejectReason& outCoordReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outLookupReason)) {
        outCoordReason = SampleCoordRejectReason::None;
        return false;
    }

    if (!FroxelGridLayout::tryCanSampleAtCoords(coords, desc, outCoordReason)) {
        outLookupReason = DensityLookupRejectReason::None;
        return false;
    }

    outLookupReason = DensityLookupRejectReason::None;
    outCoordReason = SampleCoordRejectReason::None;
    return true;
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              DensityLookupRejectReason& outLookupReason,
                              SampleCoordRejectReason& outCoordReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outLookupReason)) {
        outDensity = 0.f;
        outCoordReason = SampleCoordRejectReason::None;
        return false;
    }

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, outCoordReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outLookupReason = DensityLookupRejectReason::None;
    outCoordReason = SampleCoordRejectReason::None;
    return true;
}

void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    grid.allocate(clampedDesc);
    if (FroxelGridLayout::isEmptyGrid(clampedDesc) || params.density <= 0.f || params.march_steps == 0u) {
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
                              const FroxelCameraDesc& /*camera*/,
                              const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    return clampedDesc.froxelCount() == 0u || params.density <= 0.f || params.march_steps == 0u;
}

FroxelPopulateRejectReason classifyFroxelPopulateReject(const FroxelGridDesc& desc,
                                                        const FroxelCameraDesc& camera,
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
    return reason;

bool preflightFroxelPopulate(const FroxelGridDesc& desc,
                             const VolumetricFogParams& params,
                             FroxelPopulateRejectReason* reason) {
    const FroxelPopulateRejectReason reject = classifyFroxelPopulateReject(desc, camera, params);
    if (reason != nullptr) {
        *reason = reject;
    return !froxelPopulateRejectReasonIsBlocking(reject);

bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   FroxelPopulateRejectReason& outReason) {
    if (clampedDesc.froxelCount() == 0u) {
        outReason = FroxelPopulateRejectReason::EmptyDesc;
        return false;
    if (params.density <= 0.f) {
        outReason = FroxelPopulateRejectReason::ZeroDensity;
    if (params.march_steps == 0u) {
        outReason = FroxelPopulateRejectReason::ZeroMarchSteps;
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = FroxelPopulateRejectReason::InvalidCamera;

    outReason = FroxelPopulateRejectReason::None;
    return true;

bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
    return !tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
    return tryPopulateFromAnalyticFog(grid, desc, camera, params, reason);

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

} // namespace froxel_util

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
