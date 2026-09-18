#include <fuse/renderer/volumetric/volumetric_fog.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
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

bool FroxelGridLayout::isSampleCoordsOutOfRange(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return true;
    }
    return !areSampleCoordsInBounds(coords, desc);
}

void FroxelGridLayout::clampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return;
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
}

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    clampSampleCoords(coords, desc);
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
                                                       SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }
    if (!canMapScreenDepth(viewDepth, camera)) {
        outReason = SampleCoordRejectReason::InvalidDepth;
        return false;
    }

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
    outReason = SampleCoordRejectReason::None;
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
}

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera,
                                                      u32& outFroxelIndex,
                                                      SampleCoordRejectReason& outReason) {
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
        outFroxelIndex = 0u;
        return false;
    }

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    return true;
}

bool FroxelGridLayout::mapScreenDepthToFroxelIndex(f32 screenX,
                                                   f32 screenY,
                                                   f32 viewDepth,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelCameraDesc& camera,
                                                   u32& outFroxelIndex) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, outFroxelIndex, reason);
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
    }
    return "unknown";
}

const char* gridDensityRejectReasonLabel(GridDensityRejectReason reason) {
    switch (reason) {
    case GridDensityRejectReason::None:
        return "none";
    case GridDensityRejectReason::EmptyDesc:
        return "empty_desc";
    case GridDensityRejectReason::DescMismatch:
        return "desc_mismatch";
    case GridDensityRejectReason::UndersizedStorage:
        return "undersized_storage";
    case GridDensityRejectReason::DensityCountMismatch:
        return "density_count_mismatch";
    }
    return "unknown";
}

const char* densityLookupRejectReasonLabel(DensityLookupRejectReason reason) {
    switch (reason) {
    case DensityLookupRejectReason::None:
        return "none";
    case DensityLookupRejectReason::EmptyGrid:
        return "empty_grid";
    case DensityLookupRejectReason::DescMismatch:
        return "desc_mismatch";
    case DensityLookupRejectReason::EmptyStorage:
        return "empty_storage";
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
    return grid.matchesDesc(desc);
}

bool isDensityGridAccessible(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !grid.isEmpty() && !FroxelGridLayout::isEmptyGrid(desc) && grid.matchesDesc(desc);
}

bool shouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return !isDensityGridAccessible(grid, desc);
}

bool shouldSkipFroxelGrid(const FroxelGridDesc& desc) {
    return FroxelGridLayout::isEmptyGrid(desc);
}

bool canLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 /*index*/) {
    return isDensityGridAccessible(grid, desc);
}

bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 /*index*/,
                         DensityLookupRejectReason& outReason) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = DensityLookupRejectReason::EmptyGrid;
        return false;
    }
    if (grid.isEmpty()) {
        outReason = DensityLookupRejectReason::EmptyStorage;
        return false;
    }
    if (!gridMatchesDesc(grid, desc)) {
        outReason = DensityLookupRejectReason::DescMismatch;
        return false;
    }

    outReason = DensityLookupRejectReason::None;
    return true;
}

bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      const FroxelGridDesc& desc,
                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {
    return canLookupAtIndex(grid, desc, 0u);
}

bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc),
                               outReason);
}

bool canSampleFroxelGrid(const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc);
}

bool shouldSkipFroxelSample(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    return shouldSkipFroxelLookup(grid, desc);
}

bool isValidDensitySampleRequest(const FroxelDensityGrid& grid,
                                 const FroxelGridDesc& desc,
                                 const FroxelSampleCoords& coords) {
    if (!isDensityGridAccessible(grid, desc)) {
        return false;
    }

    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    if (FroxelGridLayout::tryAreSampleCoordsInBounds(coords, desc, reason)) {
        return true;
    }

    FroxelSampleCoords clampProbe = coords;
    return FroxelGridLayout::tryClampSampleCoords(clampProbe, desc);
}

bool canPopulateFroxelGrid(const FroxelGridDesc& desc) {
    return canSampleFroxelGrid(desc);
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
        return 0.f;
    }

    const u32 clampedIndex = FroxelGridLayout::clampFroxelIndex(index, desc);
    return grid.density[clampedIndex];
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
}

bool writeDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value) {
    if (!isDensityGridAccessible(grid, desc)) {
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
}

bool tryValidateGridDensity(const FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            GridDensityRejectReason& outReason,
                            f32 epsilon) {
    if (FroxelGridLayout::isEmptyGrid(desc)) {
        outReason = GridDensityRejectReason::None;
        return true;
    }
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (grid.density.size() < clampedDesc.froxelCount()) {
        outReason = GridDensityRejectReason::UndersizedStorage;
        return false;
    }
    if (!gridMatchesDesc(grid, desc)) {
        outReason = GridDensityRejectReason::DescMismatch;
        return false;
    }
    if (!validateDensityCounts(grid, epsilon)) {
        outReason = GridDensityRejectReason::DensityCountMismatch;
        return false;
    }

    outReason = GridDensityRejectReason::None;
    return true;
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

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
                            f32 value) {
    if (!canLookupAtIndex(grid, desc, index)) {
        return false;
    }

    return writeDensityAtIndex(grid, desc, index, value);
}

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity) {
    if (!canLookupAtIndex(grid, desc, 0u)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ);
    return true;
}

bool tryWriteDensityAtCoord(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 tileX,
                            u32 tileY,
                            u32 sliceZ,
                            f32 value) {
    if (!canLookupAtIndex(grid, desc, 0u)) {
        return false;
    }

    return writeDensityAtCoord(grid, desc, tileX, tileY, sliceZ, value);
}

f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords) {
    if (!isDensityGridAccessible(grid, desc)) {
        return 0.f;
    }

    FroxelSampleCoords safeCoords = coords;
    FroxelGridLayout::clampSampleCoords(safeCoords, desc);

    const f32 d00 = froxelDensityAt(grid, desc, safeCoords.tileX0, safeCoords.tileY0, safeCoords.sliceZ0);
    const f32 d10 = froxelDensityAt(grid, desc, safeCoords.tileX1, safeCoords.tileY0, safeCoords.sliceZ0);
    const f32 d01 = froxelDensityAt(grid, desc, safeCoords.tileX0, safeCoords.tileY1, safeCoords.sliceZ0);
    const f32 d11 = froxelDensityAt(grid, desc, safeCoords.tileX1, safeCoords.tileY1, safeCoords.sliceZ0);

    const f32 d0 = lerpDensity(d00, d10, safeCoords.tx);
    const f32 d1 = lerpDensity(d01, d11, safeCoords.tx);
    return lerpDensity(d0, d1, safeCoords.ty);
}

bool trySampleDensityBilinear(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
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
                              f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityBilinear(grid, desc, coords, outDensity, reason);
}

f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelGridDesc& desc,
                           const FroxelSampleCoords& coords) {
    if (!isDensityGridAccessible(grid, desc)) {
        return 0.f;
    }

    FroxelSampleCoords safeCoords = coords;
    FroxelGridLayout::clampSampleCoords(safeCoords, desc);

    FroxelSampleCoords slice0 = safeCoords;
    slice0.sliceZ1 = slice0.sliceZ0;
    FroxelSampleCoords slice1 = safeCoords;
    slice1.sliceZ0 = slice1.sliceZ1;

    const f32 nearSlice = sampleDensityBilinear(grid, desc, slice0);
    const f32 farSlice = sampleDensityBilinear(grid, desc, slice1);
    return lerpDensity(nearSlice, farSlice, safeCoords.tz);
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

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityTrilinear(grid, desc, coords, outDensity, reason);
}

f32 sampleDensityAtScreen(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelCameraDesc& camera,
                          f32 screenX,
                          f32 screenY,
                          f32 viewDepth) {
    if (!isDensityGridAccessible(grid, desc)) {
        return 0.f;
    }

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::mapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords)) {
        return 0.f;
    }
    return sampleDensityTrilinear(grid, desc, coords);
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

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    outReason = DensityLookupRejectReason::None;
    return true;
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, reason);
}

bool tryPopulateFromAnalyticFog(FroxelDensityGrid& grid,
                                const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params) {
    if (!canPopulateFroxelGrid(desc)) {
        return false;
    }

    populateFromAnalyticFog(grid, desc, camera, params);
    return true;
}

void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    grid.allocate(clampedDesc);
    if (clampedDesc.froxelCount() == 0u || params.density <= 0.f || params.march_steps == 0u) {
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
