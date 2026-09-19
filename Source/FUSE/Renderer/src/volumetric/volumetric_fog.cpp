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
    normalizeSampleCoords(coords);
}

bool FroxelGridLayout::tryClampSampleCoords(FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    clampSampleCoords(coords, desc);
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
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = ScreenMappingRejectReason::InvalidCamera;
        return false;
    }
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        outReason = ScreenMappingRejectReason::DepthOutOfRange;
        return false;
    }

    mapScreenDepthToSampleCoordsImpl(screenX, screenY, viewDepth, desc, camera, outCoords);
    outReason = ScreenMappingRejectReason::None;
    return true;
}

bool FroxelGridLayout::mapScreenDepthToSampleCoords(f32 screenX,
                                                    f32 screenY,
                                                    f32 viewDepth,
                                                    const FroxelGridDesc& desc,
                                                    const FroxelCameraDesc& camera,
                                                    FroxelSampleCoords& outCoords) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, outCoords, reason);
}

bool FroxelGridLayout::tryMapScreenDepthToFroxelIndex(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const FroxelGridDesc& desc,
                                                      const FroxelCameraDesc& camera,
                                                      u32& outFroxelIndex,
                                                      ScreenMappingRejectReason& outReason) {
    FroxelSampleCoords coords{};
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
        outFroxelIndex = 0u;
        return false;
    }

    outFroxelIndex = froxelIndex(coords.tileX0, coords.tileY0, coords.sliceZ0, desc);
    outReason = ScreenMappingRejectReason::None;
    return true;
}

bool FroxelGridLayout::mapScreenDepthToFroxelIndex(f32 screenX,
                                                   f32 screenY,
                                                   f32 viewDepth,
                                                   const FroxelGridDesc& desc,
                                                   const FroxelCameraDesc& camera,
                                                   u32& outFroxelIndex) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, outFroxelIndex, reason);
}

bool FroxelGridLayout::wouldClampSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    return isSampleCoordsOutOfRange(coords, desc) || !isValidSampleCoords(coords, desc);
}

bool FroxelGridLayout::tryPreflightSampleCoords(const FroxelSampleCoords& coords,
                                                const FroxelGridDesc& desc,
                                                SampleCoordRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = SampleCoordRejectReason::EmptyGrid;
        return false;
    }

    if (isValidSampleCoords(coords, desc)) {
        outReason = SampleCoordRejectReason::None;
        return true;
    }

    if (!areSampleCoordsInBounds(coords, desc)) {
        const bool indicesInRange =
            coords.tileX0 <= desc.tilesX - 1u && coords.tileY0 <= desc.tilesY - 1u &&
            coords.sliceZ0 <= desc.slicesZ - 1u && coords.tileX1 <= desc.tilesX - 1u &&
            coords.tileY1 <= desc.tilesY - 1u && coords.sliceZ1 <= desc.slicesZ - 1u;
        if (indicesInRange) {
            outReason = SampleCoordRejectReason::InvalidWeights;
            return true;
        }

        outReason = SampleCoordRejectReason::OutOfBounds;
        return false;
    }

    outReason = SampleCoordRejectReason::OutOfBounds;
    return false;
}

bool FroxelGridLayout::canPreflightSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return tryPreflightSampleCoords(coords, desc, reason);
}

bool FroxelGridLayout::wouldSkipSampleCoordPreflight(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return !tryPreflightSampleCoords(coords, desc, reason);
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
    ScreenMappingRejectReason reject = ScreenMappingRejectReason::None;
    FroxelSampleCoords coords{};
    const bool mapped = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
    if (outCoords != nullptr) {
        *outCoords = coords;
    }
    if (reason != nullptr) {
        *reason = reject;
    }
    return mapped && !screenMappingRejectReasonIsBlocking(reject);
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
    }
    return "unknown";
}

bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason) {
    return reason != ScreenMappingRejectReason::None;
}

const char* sampleCoordRejectReasonLabel(SampleCoordRejectReason reason) {
    switch (reason) {
    case SampleCoordRejectReason::None:
        return "none";
    case SampleCoordRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
    case SampleCoordRejectReason::InvalidWeights:
        return "invalid_weights";
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
    case FroxelTrilinearSampleRejectReason::ClampableWeights:
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
    switch (reason) {
    case GridDensityRejectReason::None:
        return "none";
    case GridDensityRejectReason::EmptyDesc:
        return "empty_desc";
    case GridDensityRejectReason::UndersizedStorage:
        return "undersized_storage";
    case GridDensityRejectReason::DescMismatch:
        return "desc_mismatch";
    case GridDensityRejectReason::DensityCountMismatch:
        return "density_count_mismatch";
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
    return true;
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
    case DensityLookupRejectReason::IndexOutOfRange:
        return "index_out_of_range";
    }
    return "unknown";
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

const char* froxelPopulateRejectReasonLabel(FroxelPopulateRejectReason reason) {
    switch (reason) {
    case FroxelPopulateRejectReason::None:
        return "none";
    case FroxelPopulateRejectReason::EmptyDesc:
        return "empty_desc";
    case FroxelPopulateRejectReason::InvalidCamera:
        return "invalid_camera";
    case FroxelPopulateRejectReason::ZeroDensity:
        return "zero_density";
    case FroxelPopulateRejectReason::ZeroMarchSteps:
        return "zero_march_steps";
    }
    return "unknown";
}

bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason) {
    return reason != FroxelPopulateRejectReason::None;
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

bool canLookupAtCoord(const FroxelDensityGrid& grid,
                      const FroxelGridDesc& desc,
                      u32 /*tileX*/,
                      u32 /*tileY*/,
                      u32 /*sliceZ*/) {
    return isDensityGridAccessible(grid, desc);
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
    const DensityLookupRejectReason reject = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !densityLookupRejectReasonIsBlocking(reject);
}

bool tryCanLookupAtIndex(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 index,
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
    if (FroxelGridLayout::isFroxelIndexOutOfRange(index, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;
    }
    return true;
}

bool tryCanLookupAtCoord(const FroxelDensityGrid& grid,
                         const FroxelGridDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        return false;
    }

    outReason = DensityLookupRejectReason::None;
    if (FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc)) {
        outReason = DensityLookupRejectReason::IndexOutOfRange;
    }
    return true;
}

bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return !tryCanLookupAtIndex(grid, desc, 0u, reason);
}

bool wouldSkipDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return !tryCanLookupAtIndex(grid, desc, index, reason);
}

bool wouldSkipDensityLookupAtCoord(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return !tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
}

bool wouldClampDensityLookupIndex(u32 index, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && FroxelGridLayout::isFroxelIndexOutOfRange(index, desc);
}

bool wouldClampDensityLookupCoord(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::isEmptyGrid(desc) && FroxelGridLayout::isCoordOutOfRange(tileX, tileY, sliceZ, desc);
}

bool canSampleAtCoords(const FroxelDensityGrid& grid,
                       const FroxelGridDesc& desc,
                       const FroxelSampleCoords& /*coords*/) {
    return canLookupAtIndex(grid, desc, 0u);
}

bool tryCanSampleAtCoords(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
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
            break;
        case DensityLookupRejectReason::IndexOutOfRange:
        case DensityLookupRejectReason::None:
            outReason = SampleCoordRejectReason::None;
            break;
        }
        return false;
    }

    return FroxelGridLayout::tryPreflightSampleCoords(coords, desc, outReason);
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
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
    return reason;
}

bool preflightTrilinearSample(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelSampleCoords& coords,
                              FroxelTrilinearSampleRejectReason* reason) {
    const FroxelTrilinearSampleRejectReason reject = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !froxelTrilinearSampleRejectReasonIsBlocking(reject);
}

bool tryCanTrilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                   const FroxelGridDesc& desc,
                                   const FroxelSampleCoords& coords,
                                   FroxelTrilinearSampleRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
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

    SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, sampleReason)) {
        outReason = FroxelTrilinearSampleRejectReason::InvalidSampleCoords;
        return false;
    }

    if (sampleReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelTrilinearSampleRejectReason::ClampableWeights;
        return true;
    }

    outReason = FroxelTrilinearSampleRejectReason::None;
    return true;
}

bool wouldSkipDensityTrilinearSample(const FroxelDensityGrid& grid,
                                     const FroxelGridDesc& desc,
                                     const FroxelSampleCoords& coords) {
    FroxelTrilinearSampleRejectReason reason = FroxelTrilinearSampleRejectReason::None;
    return !tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);
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
    if (grid.density.size() > clampedDesc.froxelCount()) {
        outReason = GridDensityRejectReason::DescMismatch;
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
    return true;
}

bool tryWriteDensityAtIndex(FroxelDensityGrid& grid,
                            const FroxelGridDesc& desc,
                            u32 index,
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

    return writeDensityAtIndex(grid, desc, index, value);
}

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity) {
    DensityLookupRejectReason reason = DensityLookupRejectReason::None;
    return trySampleDensityAtCoord(grid, desc, tileX, tileY, sliceZ, outDensity, reason);
}

bool trySampleDensityAtCoord(const FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ,
                             f32& outDensity,
                             DensityLookupRejectReason& outReason) {
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {
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
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {
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
                               f32& outDensity) {
    SampleCoordRejectReason reason = SampleCoordRejectReason::None;
    return trySampleDensityTrilinear(grid, desc, coords, outDensity, reason);
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity,
                               SampleCoordRejectReason& outReason) {
    if (!tryCanSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    return true;
}

bool trySampleDensityTrilinear(const FroxelDensityGrid& grid,
                               const FroxelGridDesc& desc,
                               const FroxelSampleCoords& coords,
                               f32& outDensity,
                               FroxelTrilinearSampleRejectReason& outReason) {
    if (!tryCanTrilinearSampleAtCoords(grid, desc, coords, outReason)) {
        outDensity = 0.f;
        return false;
    }

    outDensity = sampleDensityTrilinear(grid, desc, coords);
    return true;
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
                              f32& outDensity) {
    ScreenMappingRejectReason reason = ScreenMappingRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, reason);
}

bool trySampleDensityAtScreen(const FroxelDensityGrid& grid,
                              const FroxelGridDesc& desc,
                              const FroxelCameraDesc& camera,
                              f32 screenX,
                              f32 screenY,
                              f32 viewDepth,
                              f32& outDensity,
                              ScreenMappingRejectReason& outReason) {
    DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    if (!tryCanLookupAtIndex(grid, desc, 0u, lookupReason)) {
        outDensity = 0.f;
        outReason = ScreenMappingRejectReason::EmptyGrid;
        return false;
    }

    FroxelSampleCoords coords{};
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(
            screenX, screenY, viewDepth, desc, camera, coords, outReason)) {
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

bool shouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                              const FroxelCameraDesc& /*camera*/,
                              const VolumetricFogParams& params) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    return clampedDesc.froxelCount() == 0u || params.density <= 0.f || params.march_steps == 0u;
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

bool tryCanPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                   const FroxelCameraDesc& camera,
                                   const VolumetricFogParams& params,
                                   FroxelPopulateRejectReason& outReason) {
    const FroxelGridDesc clampedDesc = FroxelGridDesc::clampCounts(desc);
    if (clampedDesc.froxelCount() == 0u) {
        outReason = FroxelPopulateRejectReason::EmptyDesc;
        return false;
    }
    if (params.density <= 0.f) {
        outReason = FroxelPopulateRejectReason::ZeroDensity;
        return false;
    }
    if (params.march_steps == 0u) {
        outReason = FroxelPopulateRejectReason::ZeroMarchSteps;
        return false;
    }
    if (camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        outReason = FroxelPopulateRejectReason::InvalidCamera;
        return false;
    }

    outReason = FroxelPopulateRejectReason::None;
    return true;
}

bool canPopulateFromAnalyticFog(const FroxelGridDesc& desc,
                                const FroxelCameraDesc& camera,
                                const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
}

bool wouldSkipFroxelPopulate(const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params) {
    FroxelPopulateRejectReason reason = FroxelPopulateRejectReason::None;
    return !tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
}

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

// --- deepen additive from deepen-froxel-density-guards-1bcd ---
const char* densityGridRejectReasonLabel(DensityGridRejectReason reason) {
    case DensityGridRejectReason::None:
    case DensityGridRejectReason::EmptyGridDesc:
    case DensityGridRejectReason::DescMismatch:
    case DensityGridRejectReason::CountPartitionMismatch:
bool tryValidateDensityCounts(const FroxelDensityGrid& grid,
                            DensityGridRejectReason& outReason,
        outReason = DensityGridRejectReason::None;
        outReason = DensityGridRejectReason::EmptyGridDesc;
        outReason = DensityGridRejectReason::DescMismatch;
        outReason = DensityGridRejectReason::CountPartitionMismatch;
    DensityGridRejectReason reason = DensityGridRejectReason::None;
    return tryValidateDensityCounts(grid, desc, reason, epsilon);

// --- deepen additive from deepen-froxel-density-guards-6d29 ---
const char* froxelDensityRejectReasonLabel(FroxelDensityRejectReason reason) {
    case FroxelDensityRejectReason::None:
    case FroxelDensityRejectReason::EmptyStorage:
    case FroxelDensityRejectReason::DescMismatch:
bool tryWriteDensityAtIndex(FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index, f32 value) {
                            FroxelDensityRejectReason& outReason,
        outReason = FroxelDensityRejectReason::None;
        outReason = FroxelDensityRejectReason::EmptyStorage;
        outReason = FroxelDensityRejectReason::DescMismatch;
    FroxelDensityRejectReason reason = FroxelDensityRejectReason::None;

// --- deepen additive from deepen-b511-froxel-guards-8f53 ---
    return tryClampSampleCoords(coords, desc, reason);
        outReason = SampleCoordRejectReason::DepthOutOfRange;
    case SampleCoordRejectReason::OutOfRange:
    case SampleCoordRejectReason::DepthOutOfRange:
    return tryCanSampleAtCoords(grid, desc, coords, reason);
        outReason = SampleCoordRejectReason::OutOfRange;
    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, outReason)) {

// --- deepen additive from deepen-b511-froxel-density-guards-ca9c ---
bool FroxelGridLayout::tryAreSampleCoordsInBounds(const FroxelSampleCoords& coords,
        outReason = SampleCoordRejectReason::InvalidDepth;
    case SampleCoordRejectReason::InvalidDepth:
    return tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc),
    if (FroxelGridLayout::tryAreSampleCoordsInBounds(coords, desc, reason)) {
    return FroxelGridLayout::tryClampSampleCoords(clampProbe, desc);
bool tryValidateGridDensityForDesc(const FroxelDensityGrid& grid,
        outReason = GridDensityRejectReason::EmptyDesc;
    return tryValidateGridDensity(grid, desc, outReason, epsilon);
    SampleCoordRejectReason coordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords,
        if (coordReason == SampleCoordRejectReason::EmptyGrid) {

// --- deepen additive from deepen-froxel-volumetrics-a5a0 ---
    case DensityLookupRejectReason::SampleCoordsOutOfRange:
bool tryCanLookupAtIndexBounds(const FroxelDensityGrid& grid,
bool tryCanLookupAtSampleCoords(const FroxelDensityGrid& grid,
        outReason = DensityLookupRejectReason::SampleCoordsOutOfRange;
FroxelGridPreflight preflightFroxelDensityGrid(const FroxelDensityGrid& grid,
    FroxelGridPreflight result{};
FroxelPopulatePreflight preflightPopulateFroxelGrid(const FroxelGridDesc& desc, const VolumetricFogParams& params) {
    FroxelPopulatePreflight result{};
bool tryValidateGridDensityStrict(const FroxelDensityGrid& grid,
bool trySampleDensityAtIndexBounds(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndexBounds(grid, desc, index, outReason)) {
bool trySampleDensityBilinearAtCoords(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtSampleCoords(grid, desc, coords, outReason)) {
bool trySampleDensityTrilinearAtCoords(const FroxelDensityGrid& grid,

// --- deepen additive from deepen-b511-froxel-guards-0e8b ---
    case GridDensityRejectReason::EmptyGrid:
        outReason = GridDensityRejectReason::EmptyGrid;
bool tryValidateSampleCoords(const FroxelSampleCoords& coords,

// --- deepen additive from froxel-volumetric-guards-000f ---
const char* froxelCameraRejectReasonLabel(FroxelCameraRejectReason reason) {
    case FroxelCameraRejectReason::None:
    case FroxelCameraRejectReason::InvalidNearPlane:
    case FroxelCameraRejectReason::InvalidFarPlane:
    case FroxelCameraRejectReason::InvertedDepthRange:
    FroxelCameraRejectReason reason = FroxelCameraRejectReason::None;
    return tryValidateCamera(camera, reason);
bool FroxelSliceLayout::tryValidateCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason) {
        outReason = FroxelCameraRejectReason::InvalidNearPlane;
        outReason = FroxelCameraRejectReason::InvalidFarPlane;
        outReason = FroxelCameraRejectReason::InvertedDepthRange;
    outReason = FroxelCameraRejectReason::None;
const char* froxelGridRejectReasonLabel(FroxelGridRejectReason reason) {
    case FroxelGridRejectReason::None:
    case FroxelGridRejectReason::EmptyTilesX:
    case FroxelGridRejectReason::EmptyTilesY:
    case FroxelGridRejectReason::EmptySlicesZ:
    case SampleCoordRejectReason::TileOutOfRange:
    case SampleCoordRejectReason::WeightOutOfRange:
const char* froxelScreenMappingRejectReasonLabel(FroxelScreenMappingRejectReason reason) {
    case FroxelScreenMappingRejectReason::None:
    case FroxelScreenMappingRejectReason::EmptyGrid:
    case FroxelScreenMappingRejectReason::InvalidCamera:
    case FroxelScreenMappingRejectReason::DepthBelowNear:
    case FroxelScreenMappingRejectReason::DepthAboveFar:
bool FroxelGridLayout::tryValidateSampleCoords(const FroxelSampleCoords& coords,
        outReason = SampleCoordRejectReason::TileOutOfRange;
        outReason = SampleCoordRejectReason::WeightOutOfRange;
                                                       FroxelScreenMappingRejectReason& outReason) {
        outReason = FroxelScreenMappingRejectReason::EmptyGrid;
    FroxelCameraRejectReason cameraReason = FroxelCameraRejectReason::None;
    if (!FroxelSliceLayout::tryValidateCamera(camera, cameraReason)) {
        outReason = FroxelScreenMappingRejectReason::InvalidCamera;
        outReason = FroxelScreenMappingRejectReason::DepthBelowNear;
        outReason = FroxelScreenMappingRejectReason::DepthAboveFar;
    outReason = FroxelScreenMappingRejectReason::None;
bool tryValidateFroxelGridDesc(const FroxelGridDesc& desc, FroxelGridRejectReason& outReason) {
        outReason = FroxelGridRejectReason::EmptyTilesX;
        outReason = FroxelGridRejectReason::EmptyTilesY;
        outReason = FroxelGridRejectReason::EmptySlicesZ;
    outReason = FroxelGridRejectReason::None;
    return tryCanLookupAtIndexInRange(grid, desc, index, reason);
bool tryCanLookupAtIndexInRange(const FroxelDensityGrid& grid,
bool trySampleDensityBilinearInBounds(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtIndexInRange(grid, desc, 0u, outReason)) {
    if (!FroxelGridLayout::tryValidateSampleCoords(coords, desc, coordReason)) {
bool trySampleDensityTrilinearInBounds(const FroxelDensityGrid& grid,
                                FroxelGridRejectReason& outGridReason,
                                FroxelCameraRejectReason& outCameraReason) {
    outGridReason = FroxelGridRejectReason::None;
    outCameraReason = FroxelCameraRejectReason::None;
    if (!tryValidateFroxelGridDesc(desc, outGridReason)) {
    if (!FroxelSliceLayout::tryValidateCamera(camera, outCameraReason)) {

// --- deepen additive from deepen-b511-froxel-guards-98ed ---
bool FroxelGridLayout::tryCanSampleAtCoords(const FroxelSampleCoords& coords,
        outReason = SampleCoordRejectReason::InvertedCorners;
    case SampleCoordRejectReason::InvertedCorners:
                          DensityLookupRejectReason& outLookupReason,
                          SampleCoordRejectReason& outCoordReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outLookupReason)) {
        outCoordReason = SampleCoordRejectReason::None;
    if (!FroxelGridLayout::tryCanSampleAtCoords(coords, desc, outCoordReason)) {
        outLookupReason = DensityLookupRejectReason::None;

// --- deepen additive from deepen-b511-froxel-guards-7caf ---
        outReason = SampleCoordRejectReason::DepthBelowNear;
        outReason = SampleCoordRejectReason::DepthAboveFar;
    case SampleCoordRejectReason::DepthBelowNear:
    case SampleCoordRejectReason::DepthAboveFar:
    return tryCanSampleAtCoords(grid, desc, coords, lookupReason, coordReason);
        outLookupReason = DensityLookupRejectReason::EmptyGrid;
        outCoordReason = SampleCoordRejectReason::EmptyGrid;
        outCoordReason = SampleCoordRejectReason::OutOfBounds;
    if (!tryCanSampleAtCoords(grid, desc, coords, outLookupReason, outCoordReason)) {

// --- deepen additive from deepen-froxel-volumetric-guards-1251 ---
        outReason = SampleCoordRejectReason::OutOfRangeTile;
        outReason = SampleCoordRejectReason::OutOfRangeWeight;
        outReason = SampleCoordRejectReason::InvalidCamera;
    outReason = mapped ? SampleCoordRejectReason::None : SampleCoordRejectReason::DepthOutOfRange;
    case SampleCoordRejectReason::OutOfRangeTile:
    case SampleCoordRejectReason::OutOfRangeWeight:
    case SampleCoordRejectReason::InvalidCamera:
    case DensityLookupRejectReason::SampleCoordRejected:
    case DensityLookupRejectReason::ScreenMappingFailed:
    if (!FroxelGridLayout::tryAreSampleCoordsInBounds(coords, desc, coordReason)) {
        outReason = DensityLookupRejectReason::SampleCoordRejected;
        outReason = DensityLookupRejectReason::ScreenMappingFailed;

// --- deepen additive from deepen-b511-froxel-guards-1cb0 ---
const char* sampleCoordBoundsRejectReasonLabel(SampleCoordBoundsRejectReason reason) {
    case SampleCoordBoundsRejectReason::None:
    case SampleCoordBoundsRejectReason::EmptyGrid:
    case SampleCoordBoundsRejectReason::OutOfBoundsTile:
    case SampleCoordBoundsRejectReason::OutOfBoundsWeight:
                             SampleCoordBoundsRejectReason& outReason) {
        outReason = SampleCoordBoundsRejectReason::EmptyGrid;
        outReason = SampleCoordBoundsRejectReason::OutOfBoundsTile;
        outReason = SampleCoordBoundsRejectReason::OutOfBoundsWeight;
    outReason = SampleCoordBoundsRejectReason::None;
                              SampleCoordRejectReason& outSampleReason) {
        outSampleReason = SampleCoordRejectReason::None;

// --- deepen additive from deepen-froxel-volumetric-guards-8201 ---
bool tryMapScreenDepthToSampleCoords(f32 screenX,
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, outCoordReason)) {

// --- deepen additive from deepen-b511-froxel-preflight-guards-73d5 ---
    case FroxelPopulateRejectReason::EmptyGrid:
            outReason = indicesInRange ? SampleCoordRejectReason::InvalidWeights
                                       : SampleCoordRejectReason::OutOfBounds;
SampleCoordRejectReason classifyFroxelSampleReject(const FroxelDensityGrid& grid,
bool wouldSkipFroxelSample(const FroxelDensityGrid& grid,
                           SampleCoordRejectReason* outReason) {
    const SampleCoordRejectReason reason = classifyFroxelSampleReject(grid, desc, coords);
                *outReason = SampleCoordRejectReason::EmptyGrid;
                *outReason = SampleCoordRejectReason::OutOfBounds;
                *outReason = SampleCoordRejectReason::None;
        outReason = lookupReason == DensityLookupRejectReason::EmptyGrid ? ScreenMappingRejectReason::EmptyGrid
                                                                         : ScreenMappingRejectReason::None;
    return !tryCanPopulateFromAnalyticFog(desc, params, reason);
        outReason = FroxelPopulateRejectReason::EmptyGrid;

// --- deepen additive from deepen-froxel-volumetric-guards-d18a ---
        outReason = SampleCoordRejectReason::UnorderedCorners;
bool FroxelGridLayout::tryPreflightNonEmptyGrid(const FroxelGridDesc& desc, GridDensityRejectReason& outReason) {
    return tryValidateSampleCoords(coords, desc, reason);
    case SampleCoordRejectReason::UnorderedCorners:
bool tryPreflightDensityGridAccess(const FroxelDensityGrid& grid,
                                   GridDensityRejectReason& outReason) {
    if (!FroxelGridLayout::tryPreflightNonEmptyGrid(desc, outReason)) {
    return tryValidateGridDensityForDesc(grid, desc, reason, epsilon);

// --- deepen additive from deepen-b511-froxel-guards-2580 ---
bool tryValidateFroxelIndex(u32 index, const FroxelGridDesc& desc, DensityLookupRejectReason& outReason) {
    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, outReason)) {

// --- deepen additive from deepen-froxel-volumetrics-b511-ecd6 ---
bool preflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return tryCanPopulateFromAnalyticFog(desc, camera, params, outReason);
    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, reason)) {

// --- deepen additive from deepen-froxel-volumetric-guards-a3b2 ---
const char* populateRejectReasonLabel(PopulateRejectReason reason) {
    case PopulateRejectReason::None:
    case PopulateRejectReason::EmptyGrid:
    case PopulateRejectReason::InvalidCamera:
    case PopulateRejectReason::ZeroDensity:
    case PopulateRejectReason::ZeroMarchSteps:
    return tryCanSampleAtCoordsStrict(grid, desc, coords, reason);
bool tryCanSampleAtCoordsStrict(const FroxelDensityGrid& grid,
    if (outReason != SampleCoordRejectReason::None) {
    ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
                              ScreenMappingRejectReason& outMapReason,
                              DensityLookupRejectReason& outLookupReason) {
        outMapReason = ScreenMappingRejectReason::None;
    PopulateRejectReason reason = PopulateRejectReason::None;
        outReason = PopulateRejectReason::EmptyGrid;
        outReason = PopulateRejectReason::InvalidCamera;
        outReason = PopulateRejectReason::ZeroDensity;
        outReason = PopulateRejectReason::ZeroMarchSteps;
    outReason = PopulateRejectReason::None;

// --- deepen additive from deepen-froxel-b511-guards-9658 ---
        outReason = SampleCoordRejectReason::InvalidCorners;
    case SampleCoordRejectReason::InvalidCorners:

// --- deepen additive from deepen-b511-froxel-guards-62b9 ---
    ScreenMappingRejectReason screenReason = ScreenMappingRejectReason::None;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, screenReason);
                              ScreenMappingRejectReason& outScreenReason) {
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, outScreenReason,
                              ScreenMappingRejectReason& outScreenReason,
        outScreenReason = ScreenMappingRejectReason::None;
        outSampleReason = lookupReason == DensityLookupRejectReason::EmptyGrid ? SampleCoordRejectReason::EmptyGrid
    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, outSampleReason)) {

// --- deepen additive from deepen-froxel-volumetrics-b511-4bac ---
    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, mapReason);
                              ScreenMappingRejectReason& outMapReason) {
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, outMapReason,
            outMapReason = ScreenMappingRejectReason::EmptyGrid;
        outSampleReason = SampleCoordRejectReason::OutOfBounds;
    return trySampleDensityTrilinear(grid, desc, coords, outDensity, outSampleReason);

// --- deepen additive from deepen-froxel-volumetrics-b511-037c ---
bool FroxelGridLayout::tryPreflightTileCoords(u32 tileX,
bool FroxelGridLayout::canPreflightTileCoords(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return tryPreflightTileCoords(tileX, tileY, sliceZ, desc, reason);
    if (!tryCanSampleAtCoords(grid, desc, coords, sampleReason)) {

// --- deepen additive from deepen-froxel-b511-guards-e86c ---
    if (!tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc),
        if (lookupReason == DensityLookupRejectReason::EmptyGrid) {

// --- deepen additive from deepen-froxel-volumetrics-b511-425b ---
    case DensityLookupRejectReason::CoordOutOfRange:
    if (!tryCanLookupAtIndex(grid, desc, FroxelGridLayout::froxelIndexClamped(tileX, tileY, sliceZ, desc), outReason)) {
        outReason = DensityLookupRejectReason::CoordOutOfRange;

// --- deepen additive from deepen-b511-froxel-guards-b0a8 ---
    return tryCanSampleAtCoords(grid, desc, coords, reason) &&
           reason == SampleCoordRejectReason::None &&
    if (reason == SampleCoordRejectReason::EmptyGrid || reason == SampleCoordRejectReason::OutOfBounds) {
        outReason = lookupReason == DensityLookupRejectReason::EmptyGrid ||
                            lookupReason == DensityLookupRejectReason::EmptyStorage
    const bool canFill = tryCanPopulateFromAnalyticFog(desc, camera, params, reason);

// --- deepen additive from deepen-b511-froxel-guards-c46a ---
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, mapReason,

// --- deepen additive from deepen-froxel-preflight-guards-4be4 ---
bool wouldClampCoordLookup(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    if (!tryCanLookupAtCoord(grid, desc, coords.tileX0, coords.tileY0, coords.sliceZ0, lookupReason)) {

// --- deepen additive from deepen-froxel-b511-guards-9ea0 ---
                                               FroxelSampleCoordsRejectReason& outReason) {
        outReason = FroxelSampleCoordsRejectReason::EmptyGrid;
        outReason = indicesInRange ? FroxelSampleCoordsRejectReason::OutOfRangeWeights
                                   : FroxelSampleCoordsRejectReason::OutOfRangeIndices;
        outReason = FroxelSampleCoordsRejectReason::UnorderedCorners;
    outReason = FroxelSampleCoordsRejectReason::None;
const char* froxelSampleCoordsRejectReasonLabel(FroxelSampleCoordsRejectReason reason) {
    case FroxelSampleCoordsRejectReason::None:
    case FroxelSampleCoordsRejectReason::EmptyGrid:
    case FroxelSampleCoordsRejectReason::OutOfRangeIndices:
    case FroxelSampleCoordsRejectReason::OutOfRangeWeights:
    case FroxelSampleCoordsRejectReason::UnorderedCorners:
    case FroxelTrilinearSampleRejectReason::NotSampleable:
    case FroxelTrilinearSampleRejectReason::EmptyStorage:
    case FroxelTrilinearSampleRejectReason::UndersizedStorage:
    case FroxelTrilinearSampleRejectReason::DescMismatch:
bool tryValidateDensityLookupIndex(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::NotSampleable;
        outReason = FroxelTrilinearSampleRejectReason::EmptyStorage;
        outReason = FroxelTrilinearSampleRejectReason::UndersizedStorage;
        outReason = FroxelTrilinearSampleRejectReason::DescMismatch;

// --- deepen additive from deepen-froxel-volumetrics-b511-53be ---
    case FroxelTrilinearSampleRejectReason::LookupFailed:
bool tryCanSampleDensityTrilinear(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::LookupFailed;
    return trySampleDensityAtScreen(grid, desc, camera, screenX, screenY, viewDepth, outDensity, lookupReason,
        outLookupReason = DensityLookupRejectReason::ScreenMappingFailed;

// --- deepen additive from deepen-b511-froxel-preflights-8549 ---
    SampleCoordRejectReason localReason = SampleCoordRejectReason::None;
    const bool ok = tryPreflightSampleCoords(coords, desc, localReason);
bool FroxelGridLayout::shouldSkipSampleCoordPreflight(const FroxelGridDesc& desc) {
    case FroxelTrilinearSampleRejectReason::OutOfBoundsCoords:
    DensityLookupRejectReason localReason = DensityLookupRejectReason::None;
    const bool ok = tryCanLookupAtIndex(grid, desc, index, localReason);
bool tryPreflightDensityLookup(const FroxelDensityGrid& grid,
    return tryCanLookupAtIndex(grid, desc, index, outReason);
    return tryCanLookupAtIndex(grid, desc, index, reason) && reason == DensityLookupRejectReason::None;
bool canPreflightTrilinearSample(const FroxelDensityGrid& grid,
    return tryPreflightTrilinearSample(grid, desc, coords, reason);
bool tryPreflightTrilinearSample(const FroxelDensityGrid& grid,
            outReason = FroxelTrilinearSampleRejectReason::OutOfBoundsCoords;
    outReason = sampleReason == SampleCoordRejectReason::InvalidWeights
                    ? FroxelTrilinearSampleRejectReason::InvalidSampleCoords
                    : FroxelTrilinearSampleRejectReason::None;
    FroxelPopulateRejectReason localReason = FroxelPopulateRejectReason::None;
    const bool ok = tryCanPopulateFromAnalyticFog(desc, camera, params, localReason);
bool tryPreflightFroxelPopulate(const FroxelGridDesc& desc,

// --- deepen additive from deepen-froxel-volumetrics-b511-c280 ---
    case FroxelTrilinearSampleRejectReason::ScreenMappingFailed:
bool tryCanSampleTrilinear(const FroxelDensityGrid& grid,
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, lookupReason)) {
    if (!tryCanSampleTrilinear(grid, desc, coords, outReason)) {
        outReason = FroxelTrilinearSampleRejectReason::ScreenMappingFailed;

// --- deepen additive from deepen-froxel-volumetrics-b511-527f ---
    return tryCanSampleDensityTrilinear(grid, desc, coords, reason);
    if (!FroxelGridLayout::tryPreflightSampleCoords(coords, desc, coordReason)) {
    if (!tryCanSampleDensityTrilinear(grid, desc, coords, outReason)) {

// --- deepen additive from deepen-froxel-volumetrics-b511-da24 ---
    case FroxelTrilinearSampleRejectReason::NotAccessible:
DensityLookupRejectReason classifyFroxelDensityLookupReject(const FroxelDensityGrid& grid,
    froxel_util::tryCanLookupAtIndex(grid, desc, index, reason);
SampleCoordRejectReason classifyFroxelSampleCoordReject(const FroxelSampleCoords& coords,
    FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);
    froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, reason);
ScreenMappingRejectReason classifyFroxelScreenMappingReject(f32 screenX,
    FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);
bool wouldSkipFroxelDensityLookup(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason reject = classifyFroxelDensityLookupReject(grid, desc, 0u);
    return reject != DensityLookupRejectReason::None;
bool tryCanSampleTrilinearAtCoords(const FroxelDensityGrid& grid,
        outReason = FroxelTrilinearSampleRejectReason::NotAccessible;
bool wouldSkipFroxelTrilinearSample(const FroxelDensityGrid& grid,
    return reject != FroxelTrilinearSampleRejectReason::None;
    if (!tryCanSampleTrilinearAtCoords(grid, desc, coords, outReason)) {
    return reject != FroxelPopulateRejectReason::None;

// --- deepen additive from deepen-b511-froxel-guards-874b ---
bool FroxelGridLayout::tryNormalizeAndPreflightSampleCoords(FroxelSampleCoords& coords,
    return tryPreflightSampleCoords(coords, desc, outReason);
    case FroxelTrilinearSampleRejectReason::ClampRequired:
bool tryPreflightStrictDensityLookupAtIndex(const FroxelDensityGrid& grid,
    if (outReason == DensityLookupRejectReason::IndexOutOfRange) {
bool tryPreflightStrictDensityLookupAtCoord(const FroxelDensityGrid& grid,
bool wouldRejectDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
    return !tryPreflightStrictDensityLookupAtIndex(grid, desc, index, reason);
bool wouldRejectDensityLookupAtCoord(const FroxelDensityGrid& grid,
    return !tryPreflightStrictDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return tryCanSampleTrilinear(grid, desc, coords, reason);
    if (coordReason == SampleCoordRejectReason::InvalidWeights ||
        outReason = FroxelTrilinearSampleRejectReason::ClampRequired;
bool wouldClampTrilinearSample(const FroxelDensityGrid& grid,
bool tryShouldSkipFroxelPopulate(const FroxelGridDesc& desc,

// --- deepen additive from deepen-froxel-volumetrics-b511-8b99 ---
    case FroxelTrilinearSampleRejectReason::HardOutOfBounds:
bool tryPreflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
bool tryPreflightDensityLookupAtCoord(const FroxelDensityGrid& grid,
    return tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason);
        outReason = FroxelTrilinearSampleRejectReason::HardOutOfBounds;
    return tryCanTrilinearSampleAtCoords(grid, desc, coords, outReason);
                                      FroxelPopulateRejectReason* outReason) {

// --- deepen additive from deepen-b511-froxel-guards-f8af ---
    return preflightSampleAtCoords(grid, desc, coords);
bool preflightSampleAtCoords(const FroxelDensityGrid& grid,
    return preflightSampleAtCoords(grid, desc, coords, reason);
    return tryCanSampleAtCoords(grid, desc, coords, outReason);
bool wouldClampSampleAtCoords(const FroxelDensityGrid& grid,
    return preflightSampleAtCoords(grid, desc, coords, outReason);
bool preflightDensityLookupAtIndex(const FroxelDensityGrid& grid,
    return preflightDensityLookupAtIndex(grid, desc, index, reason);
    return preflightDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);
    return preflightPopulateFromAnalyticFog(desc, camera, params);
    return preflightPopulateFromAnalyticFog(desc, camera, params, reason);

// --- deepen additive from deepen-b511-froxel-preflights-49c6 ---
bool FroxelGridLayout::wouldSkipSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    case FroxelTrilinearSampleRejectReason::CoordsOutOfRange:
    case FroxelTrilinearSampleRejectReason::InvalidWeights:
        outReason = FroxelTrilinearSampleRejectReason::CoordsOutOfRange;
    if (coordReason == SampleCoordRejectReason::InvalidWeights) {
        outReason = FroxelTrilinearSampleRejectReason::InvalidWeights;
bool wouldSkipTrilinearSample(const FroxelDensityGrid& grid,
    return !tryPreflightTrilinearSample(grid, desc, coords, reason);
    if (!tryPreflightTrilinearSample(grid, desc, coords, outReason)) {
bool wouldSkipAnalyticPopulate(const FroxelGridDesc& desc,

// --- deepen additive from deepen-froxel-volumetrics-b511-2131 ---
    return tryPreflightSampleCoords(coords, desc, reason) &&
           reason == SampleCoordRejectReason::InvalidWeights;
const char* densityTrilinearSampleRejectReasonLabel(DensityTrilinearSampleRejectReason reason) {
    case DensityTrilinearSampleRejectReason::None:
    case DensityTrilinearSampleRejectReason::EmptyGrid:
    case DensityTrilinearSampleRejectReason::DescMismatch:
    case DensityTrilinearSampleRejectReason::EmptyStorage:
    case DensityTrilinearSampleRejectReason::InvalidSampleCoords:
    case DensityTrilinearSampleRejectReason::ClampableWeights:
    DensityTrilinearSampleRejectReason reason = DensityTrilinearSampleRejectReason::None;
                                   DensityTrilinearSampleRejectReason& outReason) {
        outReason = DensityTrilinearSampleRejectReason::EmptyGrid;
        outReason = DensityTrilinearSampleRejectReason::EmptyStorage;
        outReason = DensityTrilinearSampleRejectReason::DescMismatch;
        outReason = DensityTrilinearSampleRejectReason::InvalidSampleCoords;
        outReason = DensityTrilinearSampleRejectReason::ClampableWeights;
    outReason = DensityTrilinearSampleRejectReason::None;
bool wouldClampDensityTrilinearSample(const FroxelDensityGrid& grid,
bool wouldPopulateAllocateOnly(const FroxelGridDesc& desc,

// --- deepen additive from deepen-froxel-volumetrics-b511-e35c ---
bool FroxelGridLayout::tryPreflightTrilinearSampleCoords(const FroxelSampleCoords& coords,
        outReason = TrilinearSampleRejectReason::EmptyGrid;
            outReason = TrilinearSampleRejectReason::OutOfBounds;
        outReason = TrilinearSampleRejectReason::InvalidWeights;
        outReason = TrilinearSampleRejectReason::InvalidSampleCoords;
    outReason = TrilinearSampleRejectReason::None;
const char* trilinearSampleRejectReasonLabel(TrilinearSampleRejectReason reason) {
    case TrilinearSampleRejectReason::None:
    case TrilinearSampleRejectReason::EmptyGrid:
    case TrilinearSampleRejectReason::EmptyStorage:
    case TrilinearSampleRejectReason::DescMismatch:
    case TrilinearSampleRejectReason::InvalidSampleCoords:
    case TrilinearSampleRejectReason::OutOfBounds:
    case TrilinearSampleRejectReason::InvalidWeights:
bool preflightDensityLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
        outReason = TrilinearSampleRejectReason::EmptyStorage;
        outReason = TrilinearSampleRejectReason::DescMismatch;
    return FroxelGridLayout::tryPreflightTrilinearSampleCoords(coords, desc, outReason);
bool tryPreflightPopulate(const FroxelGridDesc& desc,

// --- deepen additive from deepen-b511-froxel-guards-c9a6 ---
bool wouldSkipDensityLookupAtIndex(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 index) {
        outReason = sampleReason == SampleCoordRejectReason::EmptyGrid
                        ? FroxelTrilinearSampleRejectReason::EmptyGrid
                        : FroxelTrilinearSampleRejectReason::HardOutOfBounds;

// --- deepen additive from deepen-b511-froxel-guards-5875 ---
    froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, coords, reason);

// --- deepen additive from deepen-froxel-volumetrics-b511-4fcc ---
bool wouldClampTrilinearSampleCoords(const FroxelDensityGrid& grid,
bool canPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,
bool tryPreflightPopulateFromAnalyticFog(const FroxelGridDesc& desc,

// --- deepen additive from deepen-froxel-volumetrics-b511-a69f ---
bool tryCanLookupForDensitySample(const FroxelDensityGrid& grid,
    return tryCanLookupAtIndex(grid, desc, 0u, outReason);
bool wouldRejectSampleCoords(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return !FroxelGridLayout::tryPreflightSampleCoords(coords, desc, reason);
bool wouldPopulateAllocateWithoutFill(const FroxelGridDesc& desc,
bool tryPreflightPopulateAllocation(const FroxelGridDesc& desc, FroxelPopulateRejectReason& outReason) {

// --- deepen additive from deepen-froxel-b511-guards-2eab ---
    if (!tryPreflightSampleCoords(coords, desc, outReason)) {
    case FroxelTrilinearSampleRejectReason::GridInaccessible:
bool wouldClampTrilinearSample(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
        outReason = FroxelTrilinearSampleRejectReason::GridInaccessible;
        outReason = coordReason == SampleCoordRejectReason::EmptyGrid
                        : FroxelTrilinearSampleRejectReason::InvalidSampleCoords;

// --- deepen additive from deepen-froxel-volumetrics-b511-a361 ---
    outReason = classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (outReason != ScreenMappingRejectReason::None) {
        return SampleCoordRejectReason::EmptyGrid;
        return SampleCoordRejectReason::None;
            return SampleCoordRejectReason::InvalidWeights;
        return SampleCoordRejectReason::OutOfBounds;
    outReason = classifySampleCoordReject(coords, desc);
    if (outReason == SampleCoordRejectReason::EmptyGrid ||
        outReason == SampleCoordRejectReason::OutOfBounds) {
    return preflightSampleCoords(coords, desc);
    SampleCoordRejectReason reject = SampleCoordRejectReason::None;
    const bool ok = tryPreflightSampleCoords(coords, desc, reject);
ScreenMappingRejectReason classifyScreenMappingReject(f32 /*screenX*/,
        return ScreenMappingRejectReason::EmptyGrid;
        return ScreenMappingRejectReason::InvalidCamera;
        return ScreenMappingRejectReason::DepthOutOfRange;
    return ScreenMappingRejectReason::None;
bool preflightScreenDepthMapping(f32 screenX,
    const ScreenMappingRejectReason reject =
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    return reject == ScreenMappingRejectReason::None;
        return FroxelPopulateRejectReason::EmptyDesc;
        return FroxelPopulateRejectReason::ZeroDensity;
        return FroxelPopulateRejectReason::ZeroMarchSteps;
        return FroxelPopulateRejectReason::InvalidCamera;
    return FroxelPopulateRejectReason::None;
    return reject == FroxelPopulateRejectReason::None;
                                FroxelPopulateRejectReason& reason) {
    return preflightFroxelPopulate(desc, camera, params, &reason);
        return DensityLookupRejectReason::EmptyGrid;
        return DensityLookupRejectReason::EmptyStorage;
        return DensityLookupRejectReason::DescMismatch;
        return DensityLookupRejectReason::IndexOutOfRange;
    return DensityLookupRejectReason::None;
DensityLookupRejectReason classifyDensityLookupCoordReject(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason baseReject = classifyDensityLookupReject(grid, desc, 0u);
    if (baseReject != DensityLookupRejectReason::None &&
        baseReject != DensityLookupRejectReason::IndexOutOfRange) {
    outReason = classifyDensityLookupReject(grid, desc, index);
    return outReason != DensityLookupRejectReason::EmptyGrid &&
           outReason != DensityLookupRejectReason::EmptyStorage &&
           outReason != DensityLookupRejectReason::DescMismatch;
    outReason = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    return reject != DensityLookupRejectReason::EmptyGrid &&
           reject != DensityLookupRejectReason::EmptyStorage &&
           reject != DensityLookupRejectReason::DescMismatch;
                                      DensityLookupRejectReason& reason) {
    return preflightDensityLookupAtIndex(grid, desc, index, &reason);
    const DensityLookupRejectReason reject = classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
    return preflightDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ, &reason);
SampleCoordRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason lookupReject = classifyDensityLookupReject(grid, desc, 0u);
    return FroxelGridLayout::classifySampleCoordReject(coords, desc);
    outReason = classifyTrilinearSampleReject(grid, desc, coords);
    const bool ok = tryCanSampleAtCoords(grid, desc, coords, reject);
                                 SampleCoordRejectReason& reason) {
    return preflightTrilinearSample(grid, desc, coords, &reason);
    return !preflightTrilinearSample(grid, desc, coords);
    return tryPreflightFroxelPopulate(desc, camera, params, outReason);
    return preflightFroxelPopulate(desc, camera, params);

// --- deepen additive from deepen-froxel-volumetric-guards-b511-453e ---
    return tryPreflightTrilinearSample(grid, desc, coords, outReason);
    FroxelTrilinearSampleRejectReason trilinearReason = FroxelTrilinearSampleRejectReason::None;
    if (!tryPreflightTrilinearSample(grid, desc, coords, trilinearReason)) {
    if (trilinearReason == FroxelTrilinearSampleRejectReason::InvalidSampleCoords) {
bool tryLookupDensityFromScreen(const FroxelDensityGrid& grid,

// --- deepen additive from deepen-b511-froxel-guards-13b8 ---
bool tryCanLookupAtIndexStrict(const FroxelDensityGrid& grid,
    return tryCanLookupAtIndexStrict(grid, desc, index, reason);
bool tryCanLookupAtCoordStrict(const FroxelDensityGrid& grid,
    return tryCanLookupAtCoordStrict(grid, desc, tileX, tileY, sliceZ, reason);
    return FroxelGridLayout::canPreflightSampleCoords(coords, desc);
bool tryPreflightTrilinearDensitySample(const FroxelDensityGrid& grid,
bool canPreflightTrilinearDensitySample(const FroxelDensityGrid& grid,
    return tryPreflightTrilinearDensitySample(grid, desc, coords, reason);
    return !canPreflightTrilinearDensitySample(grid, desc, coords);
bool wouldClampTrilinearDensitySample(const FroxelDensityGrid& grid,
    if (!tryPreflightTrilinearDensitySample(grid, desc, coords, outReason)) {
    if (!trySampleDensityTrilinear(grid, desc, coords, outDensity, sampleReason)) {
bool tryValidatePopulateResult(const FroxelDensityGrid& grid,
    FroxelPopulateRejectReason populateReason = FroxelPopulateRejectReason::None;
    if (!tryCanPopulateFromAnalyticFog(desc, camera, params, populateReason)) {

// --- deepen additive from deepen-b511-froxel-preflights-e22d ---
    const bool ok = tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, localReason);
    const bool ok = tryCanSampleAtCoords(grid, desc, coords, localReason);

// --- deepen additive from deepen-b511-froxel-guards-ca59 ---
    return tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);
        outReason = sampleReason == SampleCoordRejectReason::OutOfBounds
                        ? FroxelTrilinearSampleRejectReason::HardOutOfBounds
    return !tryCanSampleTrilinearAtCoords(grid, desc, coords, reason);

// --- deepen additive from deepen-b511-froxel-guards-1123 ---
SampleCoordRejectReason FroxelGridLayout::classifySampleCoordsReject(const FroxelSampleCoords& coords,
ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingReject(f32 /*screenX*/,
bool FroxelGridLayout::preflightScreenDepthToSampleCoords(f32 screenX,
                                                          ScreenMappingRejectReason* outReason) {
    if (screenMappingRejectReasonIsBlocking(reject)) {
    if (screenMappingRejectReasonIsBlocking(outReason)) {
    outReason = classifySampleCoordsReject(coords, desc);
    return !sampleCoordRejectReasonIsBlocking(outReason);
    return reason != GridDensityRejectReason::None;
DensityLookupRejectReason classifyDensityLookupRejectAtIndex(const FroxelDensityGrid& grid,
        return FroxelTrilinearSampleRejectReason::EmptyGrid;
        return FroxelTrilinearSampleRejectReason::InaccessibleGrid;
    switch (FroxelGridLayout::classifySampleCoordsReject(coords, desc)) {
        return FroxelTrilinearSampleRejectReason::None;
        return FroxelTrilinearSampleRejectReason::ClampableWeights;
        return FroxelTrilinearSampleRejectReason::InvalidSampleCoords;
bool tryCanSampleAtTrilinear(const FroxelDensityGrid& grid,
    outReason = classifyFroxelTrilinearSampleReject(grid, desc, coords);
    return !froxelTrilinearSampleRejectReasonIsBlocking(outReason);
bool preflightFroxelTrilinearSample(const FroxelDensityGrid& grid,
                                    FroxelTrilinearSampleRejectReason* outReason) {
    if (froxelTrilinearSampleRejectReasonIsBlocking(reject)) {
                          GridDensityRejectReason* outReason,

// --- deepen additive from deepen-froxel-preflight-guards-cfa4 ---
        return ScreenMappingRejectReason::NonFiniteScreenCoords;
        return ScreenMappingRejectReason::NonFiniteDepth;
        return ScreenMappingRejectReason::ScreenCoordsOutOfRange;
bool FroxelGridLayout::preflightScreenDepthToFroxelIndex(f32 screenX,
    if (!preflightScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, &coords, reason)) {
bool FroxelGridLayout::wouldSkipScreenDepthMapping(f32 screenX,
    return !preflightScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera);
    case ScreenMappingRejectReason::ScreenCoordsOutOfRange:
    case ScreenMappingRejectReason::NonFiniteScreenCoords:
    case ScreenMappingRejectReason::NonFiniteDepth:
DensityLookupRejectReason classifyDensityLookupIndexReject(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason reject = classifyDensityLookupIndexReject(grid, desc, index);
    return !preflightDensityLookupAtIndex(grid, desc, index);
    return !preflightDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);
SampleCoordRejectReason classifyDensitySampleCoordReject(const FroxelDensityGrid& grid,
bool preflightDensitySampleAtCoords(const FroxelDensityGrid& grid,
    const SampleCoordRejectReason reject = classifyDensitySampleCoordReject(grid, desc, coords);
bool wouldSkipDensitySampleAtCoords(const FroxelDensityGrid& grid,
    return !preflightDensitySampleAtCoords(grid, desc, coords);
bool wouldSkipGridDensityValidation(const FroxelDensityGrid& grid,
    return !preflightGridDensity(grid, desc, nullptr, epsilon);
bool preflightTrilinearDensitySample(const FroxelDensityGrid& grid,
    return preflightDensitySampleAtCoords(grid, desc, coords, reason);
bool wouldSkipTrilinearDensitySample(const FroxelDensityGrid& grid,
    return wouldSkipDensitySampleAtCoords(grid, desc, coords);
bool preflightScreenDensitySample(const FroxelDensityGrid& grid,
            *reason = ScreenMappingRejectReason::EmptyGrid;
    return FroxelGridLayout::preflightScreenDepthToSampleCoords(
bool wouldSkipScreenDensitySample(const FroxelDensityGrid& grid,
    return !preflightScreenDensitySample(grid, desc, camera, screenX, screenY, viewDepth);
bool wouldSkipPopulateFromAnalyticFog(const FroxelGridDesc& desc,
    return !preflightPopulateFromAnalyticFog(desc, camera, params);

// --- deepen additive from deepen-b511-froxel-guards-700f ---
bool FroxelGridLayout::preflightScreenDepthMapping(f32 screenX,
    return !screenMappingRejectReasonIsBlocking(reject);
    return reason != FroxelTrilinearSampleRejectReason::None;
bool wouldSkipFroxelGrid(const FroxelGridDesc& desc) {
bool wouldSkipFroxelLookup(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
bool wouldSkipFroxelMarch(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, f32 epsilon) {
    return !tryValidateGridDensity(grid, desc, reason, epsilon);
bool tryCanTrilinearSample(const FroxelDensityGrid& grid,
FroxelTrilinearSampleRejectReason classifyTrilinearSampleReject(const FroxelDensityGrid& grid,
    tryCanTrilinearSample(grid, desc, coords, reason);
    const FroxelTrilinearSampleRejectReason reject = classifyTrilinearSampleReject(grid, desc, coords);
    return !tryCanTrilinearSample(grid, desc, coords, reason);
ScreenMappingRejectReason classifyScreenDensitySampleReject(const FroxelDensityGrid& grid,
    FroxelGridLayout::tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, mapReason);
        classifyScreenDensitySampleReject(grid, desc, camera, screenX, screenY, viewDepth);
    return !preflightScreenDensitySample(grid, desc, camera, screenX, screenY, viewDepth, &reason);

// --- deepen additive from deepen-b511-froxel-guards-9fe1 ---
    return tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason);
    return tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reason);

// --- deepen additive from deepen-froxel-volumetrics-b511-2f19 ---
    ScreenMappingRejectReason rejectReason = ScreenMappingRejectReason::None;
        tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason);
        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, rejectReason);
    GridDensityRejectReason rejectReason = GridDensityRejectReason::None;
    const bool ok = tryValidateGridDensity(grid, desc, rejectReason, epsilon);
DensityLookupRejectReason classifyDensityLookupAtIndex(const FroxelDensityGrid& grid,
DensityLookupRejectReason classifyDensityLookupAtCoord(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason rejectReason = classifyDensityLookupAtIndex(grid, desc, index);
    return !densityLookupRejectReasonIsBlocking(rejectReason);
    const DensityLookupRejectReason rejectReason = classifyDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);
    FroxelPopulateRejectReason rejectReason = FroxelPopulateRejectReason::None;
    const bool ok = tryCanPopulateFromAnalyticFog(desc, camera, params, rejectReason);
    SampleCoordRejectReason rejectReason = SampleCoordRejectReason::None;
    const bool ok = tryCanSampleAtCoords(grid, desc, coords, rejectReason);
bool preflightDensityAtScreen(const FroxelDensityGrid& grid,
    const bool ok = FroxelGridLayout::tryMapScreenDepthToSampleCoords(

// --- deepen additive from deepen-froxel-b511-guards-2df3 ---
bool FroxelGridLayout::preflightFroxelSampleCoords(const FroxelSampleCoords& coords,
bool FroxelGridLayout::preflightFroxelScreenDepth(f32 screenX,
    case GridDensityRejectReason::NonFiniteDensity:
    case FroxelCameraRejectReason::InvalidPlanes:
    case FroxelCameraRejectReason::ZeroScreenDimensions:
bool froxelCameraRejectReasonIsBlocking(FroxelCameraRejectReason reason) {
    return reason != FroxelCameraRejectReason::None;
bool tryValidateFroxelCamera(const FroxelCameraDesc& camera, FroxelCameraRejectReason& outReason) {
        outReason = FroxelCameraRejectReason::InvalidPlanes;
        outReason = FroxelCameraRejectReason::ZeroScreenDimensions;
    return tryValidateFroxelCamera(camera, reason);
            return FroxelTrilinearSampleRejectReason::EmptyStorage;
            return FroxelTrilinearSampleRejectReason::DescMismatch;
        return GridDensityRejectReason::EmptyDesc;
        return GridDensityRejectReason::UndersizedStorage;
        return GridDensityRejectReason::DescMismatch;
        return GridDensityRejectReason::NonFiniteDensity;
        return GridDensityRejectReason::DensityCountMismatch;
    return GridDensityRejectReason::None;

// --- deepen additive from deepen-froxel-volumetrics-b511-da54 ---
ScreenMappingRejectReason FroxelGridLayout::classifyScreenDepthMappingReject(f32 screenX,
        classifyScreenDepthMappingReject(screenX, screenY, viewDepth, desc, camera);
        classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);

// --- deepen additive from deepen-froxel-b511-guards-8718 ---
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reason)) {
        return screenMappingRejectReasonIsBlocking(reason);
bool FroxelGridLayout::wouldSkipScreenDepthToFroxelIndex(f32 screenX,
    if (!tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reason)) {
    return reason == SampleCoordRejectReason::EmptyGrid || reason == SampleCoordRejectReason::OutOfBounds;
const char* froxelBilinearSampleRejectReasonLabel(FroxelBilinearSampleRejectReason reason) {
    case FroxelBilinearSampleRejectReason::None:
    case FroxelBilinearSampleRejectReason::EmptyGrid:
    case FroxelBilinearSampleRejectReason::InaccessibleGrid:
    case FroxelBilinearSampleRejectReason::InvalidSampleCoords:
    case FroxelBilinearSampleRejectReason::ClampableWeights:
bool froxelBilinearSampleRejectReasonIsBlocking(FroxelBilinearSampleRejectReason reason) {
    return reason == FroxelBilinearSampleRejectReason::EmptyGrid ||
           reason == FroxelBilinearSampleRejectReason::InaccessibleGrid ||
           reason == FroxelBilinearSampleRejectReason::InvalidSampleCoords;
    return reason == FroxelTrilinearSampleRejectReason::EmptyGrid ||
           reason == FroxelTrilinearSampleRejectReason::InaccessibleGrid ||
           reason == FroxelTrilinearSampleRejectReason::InvalidSampleCoords;
    return reason == GridDensityRejectReason::UndersizedStorage ||
           reason == GridDensityRejectReason::DescMismatch ||
           reason == GridDensityRejectReason::DensityCountMismatch;
    return reason == DensityLookupRejectReason::EmptyGrid ||
           reason == DensityLookupRejectReason::DescMismatch ||
           reason == DensityLookupRejectReason::EmptyStorage;
    FroxelBilinearSampleRejectReason reason = FroxelBilinearSampleRejectReason::None;
    return tryCanBilinearSampleAtCoords(grid, desc, coords, reason);
bool tryCanBilinearSampleAtCoords(const FroxelDensityGrid& grid,
                                  FroxelBilinearSampleRejectReason& outReason) {
            outReason = FroxelBilinearSampleRejectReason::EmptyGrid;
            outReason = FroxelBilinearSampleRejectReason::InaccessibleGrid;
        outReason = FroxelBilinearSampleRejectReason::InvalidSampleCoords;
        outReason = FroxelBilinearSampleRejectReason::ClampableWeights;
    outReason = FroxelBilinearSampleRejectReason::None;
bool wouldSkipDensityBilinearSample(const FroxelDensityGrid& grid,
    return !tryCanBilinearSampleAtCoords(grid, desc, coords, reason);
    if (!tryCanBilinearSampleAtCoords(grid, desc, coords, outReason)) {

// --- deepen additive from deepen-b511-froxel-classify-preflight-49bd ---
bool FroxelGridLayout::wouldSkipScreenMapping(f32 screenX,
    return !preflightScreenDepthMapping(screenX, screenY, viewDepth, desc, camera);
bool preflightDensityTrilinearSample(const FroxelDensityGrid& grid,
    return !tryCanSampleAtCoords(grid, desc, coords, reason);
FroxelPopulateRejectReason classifyPopulateReject(const FroxelGridDesc& desc,
    const FroxelPopulateRejectReason reject = classifyPopulateReject(desc, camera, params);

// --- deepen additive from deepen-froxel-volumetrics-b511-2bd2 ---
    const SampleCoordRejectReason reject = classifySampleCoordsReject(coords, desc);
    if (!tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason)) {
        *reason = ScreenMappingRejectReason::None;
    if (!tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, rejectReason)) {
ScreenMappingRejectReason classifyScreenMappingReject(f32 screenX,
DensityLookupRejectReason classifyDensityLookupAtCoordReject(const FroxelDensityGrid& grid,
        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);
GridDensityRejectReason classifyGridDensityReject(const FroxelDensityGrid& grid, const FroxelGridDesc& desc) {
    tryValidateGridDensity(grid, desc, reason);
                          GridDensityRejectReason* reason) {
    const GridDensityRejectReason reject = classifyGridDensityReject(grid, desc);

// --- deepen additive from froxel-classify-isblocking-guards-e388 ---
SampleCoordRejectReason FroxelGridLayout::classifyFroxelSampleCoordReject(const FroxelSampleCoords& coords,
    const SampleCoordRejectReason reject = classifyFroxelSampleCoordReject(coords, desc);
    const DensityLookupRejectReason reject = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
SampleCoordRejectReason classifyFroxelSampleCoordReject(const FroxelDensityGrid& grid,
    const SampleCoordRejectReason reject = classifyFroxelSampleCoordReject(grid, desc, coords);

// --- deepen additive from deepen-froxel-volumetrics-b511-3f3e ---
    return reason != DensityLookupRejectReason::None &&
           reason != DensityLookupRejectReason::IndexOutOfRange;
    const SampleCoordRejectReason sampleReason = FroxelGridLayout::classifySampleCoordReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReason)) {
    outReason = classifyGridDensityReject(grid, desc, epsilon);
    return !gridDensityRejectReasonIsBlocking(outReason);
    outReason = classifyFroxelPopulateReject(desc, camera, params);
    return !froxelPopulateRejectReasonIsBlocking(outReason);

// --- deepen additive from deepen-froxel-classify-isblocking-34dc ---
    if (densityLookupRejectReasonIsBlocking(baseReject)) {
    return !densityLookupRejectReasonIsBlocking(outReason);
    outReason = classifyDensityLookupReject(grid, desc, tileX, tileY, sliceZ);
    if (densityLookupRejectReasonIsBlocking(lookupReject)) {
    const SampleCoordRejectReason sampleReject = FroxelGridLayout::classifySampleCoordsReject(coords, desc);
    if (sampleCoordRejectReasonIsBlocking(sampleReject)) {
    if (sampleReject == SampleCoordRejectReason::InvalidWeights) {
bool preflightPopulate(const FroxelGridDesc& desc,
    outReason = classifyPopulateReject(desc, camera, params);

// --- deepen additive from deepen-froxel-volumetrics-b511-1e4c ---
    return sampleCoordRejectReasonIsBlocking(classifySampleCoordReject(coords, desc));
    const DensityLookupRejectReason indexReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(indexReason)) {
    outReason = classifyDensityLookupRejectAtCoord(grid, desc, tileX, tileY, sliceZ);
    const DensityLookupRejectReason lookupReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(lookupReason)) {

// --- deepen additive from froxel-volumetric-b511-deepen-b4e6 ---
    return mapped && !screenMappingRejectReasonIsBlocking(rejectReason);

// --- deepen additive from deepen-froxel-volumetrics-b511-a55a ---
bool preflightScreenMapping(f32 screenX,
SampleCoordRejectReason classifySampleCoordsRejectImpl(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
    return classifySampleCoordsRejectImpl(coords, desc);
    return FroxelGridLayout::classifySampleCoordsReject(coords, desc);
bool preflightFroxelSample(const FroxelDensityGrid& grid,
    const SampleCoordRejectReason reject = classifyFroxelSampleReject(grid, desc, coords);
    outReason = classifyFroxelSampleReject(grid, desc, coords);
    const SampleCoordRejectReason sampleReason = FroxelGridLayout::classifySampleCoordsReject(coords, desc);
    const DensityLookupRejectReason reject = classifyDensityLookupAtIndex(grid, desc, index);
    const DensityLookupRejectReason reject = classifyDensityLookupAtCoord(grid, desc, tileX, tileY, sliceZ);

// --- deepen additive from deepen-b511-froxel-guards-7b26 ---
bool sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason reason);
    const bool ok = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, reject);
SampleCoordRejectReason classifyDensitySampleReject(const FroxelDensityGrid& grid,
    const SampleCoordRejectReason reject = classifyDensitySampleReject(grid, desc, coords);
    const ScreenMappingRejectReason mapReject =
        FroxelGridLayout::classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
    if (screenMappingRejectReasonIsBlocking(mapReject)) {
    if (!preflightDensityLookup(grid, desc, 0u, &lookupReason)) {

// --- deepen additive from deepen-b511-froxel-classify-guards-10ab ---
bool preflightScreenMappingReady(f32 screenX,
bool FroxelGridLayout::preflightSampleCoordsReady(const FroxelSampleCoords& coords,
bool preflightDensityLookupReady(const FroxelDensityGrid& grid,
bool preflightDensityLookupAtCoordReady(const FroxelDensityGrid& grid,
bool preflightFroxelTrilinearSampleReady(const FroxelDensityGrid& grid,
bool preflightFroxelPopulateReady(const FroxelGridDesc& desc,

// --- deepen additive from deepen-b511-froxel-volumetrics-5ada ---
    if (reject == ScreenMappingRejectReason::None) {

// --- deepen additive from deepen-b511-froxel-guards-d9de ---
bool wouldSkipScreenMapping(f32 screenX,
        classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera));
SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords, const FroxelGridDesc& desc) {
bool preflightSampleCoords(const FroxelSampleCoords& coords,
FroxelBilinearSampleRejectReason classifyFroxelBilinearSampleReject(const FroxelDensityGrid& grid,
    return froxelBilinearSampleRejectReasonIsBlocking(
        classifyFroxelBilinearSampleReject(grid, desc, coords));
    froxel_util::tryValidateGridDensity(grid, desc, reason, epsilon);
    froxel_util::tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, reason);

// --- deepen additive from deepen-froxel-volumetrics-b511-114a ---
ScreenMappingRejectReason classifyScreenDepthMappingReject(f32 screenX,

// --- deepen additive from deepen-froxel-volumetrics-b511-be15 ---
    if (!tryCanLookupAtIndex(grid, desc, 0u, reason)) {
bool preflightDensityLookupCoord(const FroxelDensityGrid& grid,
    return densityLookupRejectReasonIsBlocking(classifyDensityLookupReject(grid, desc, 0u));
    return densityLookupRejectReasonIsBlocking(classifyDensityLookupReject(grid, desc, index));
    return densityLookupRejectReasonIsBlocking(
        classifyDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ));
    return froxelTrilinearSampleRejectReasonIsBlocking(classifyFroxelTrilinearSampleReject(grid, desc, coords));
    return froxelPopulateRejectReasonIsBlocking(classifyFroxelPopulateReject(desc, camera, params));

// --- deepen additive from deepen-froxel-volumetrics-b511-c27f ---
    ScreenMappingRejectReason reject = classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);

// --- deepen additive from deepen-b511-classify-guards-05f2 ---
    outReason = classifyFroxelSampleCoordsReject(coords, desc);
    return sampleCoordRejectReasonIsBlocking(classifyFroxelSampleCoordsReject(coords, desc));
SampleCoordRejectReason FroxelGridLayout::classifyFroxelSampleCoordsReject(const FroxelSampleCoords& coords,
    const SampleCoordRejectReason reject = classifyFroxelSampleCoordsReject(coords, desc);
    outReason = classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);
        classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ));
        FroxelGridLayout::classifyFroxelSampleCoordsReject(coords, desc);

// --- deepen additive from deepen-froxel-volumetrics-b511-997a ---
ScreenMappingRejectReason FroxelGridLayout::classifyScreenDepthToSampleCoordsReject(
    const SampleCoordRejectReason sampleReject = FroxelGridLayout::classifySampleCoordReject(coords, desc);

// --- deepen additive from deepen-froxel-volumetrics-b511-9af2 ---
    outReason = classifyScreenDepthMappingReject(screenX, screenY, viewDepth, desc, camera);
    const DensityLookupRejectReason baseReason = classifyDensityLookupReject(grid, desc, 0u);
    if (densityLookupRejectReasonIsBlocking(baseReason)) {

// --- deepen additive from froxel-volumetric-b511-deepen-ea42 ---
ScreenMappingRejectReason FroxelGridLayout::classifyFroxelScreenMappingReject(f32 screenX,
bool FroxelGridLayout::preflightFroxelScreenMapping(f32 screenX,
        classifyFroxelScreenMappingReject(screenX, screenY, viewDepth, desc, camera);
DensityLookupRejectReason classifyFroxelDensityLookupCoordReject(const FroxelDensityGrid& grid,
bool preflightFroxelDensityLookup(const FroxelDensityGrid& grid,
    const DensityLookupRejectReason reject = classifyFroxelDensityLookupReject(grid, desc, index);
bool preflightFroxelDensityLookupAtCoord(const FroxelDensityGrid& grid,
        classifyFroxelDensityLookupCoordReject(grid, desc, tileX, tileY, sliceZ);
GridDensityRejectReason classifyFroxelGridDensityReject(const FroxelDensityGrid& grid,
bool preflightFroxelGridDensity(const FroxelDensityGrid& grid,
    const GridDensityRejectReason reject = classifyFroxelGridDensityReject(grid, desc, epsilon);

// --- deepen additive from deepen-froxel-volumetrics-b511-45a3 ---
bool preflightScreenDepthToSampleCoords(f32 screenX,
    return ok && !screenMappingRejectReasonIsBlocking(rejectReason);
bool preflightScreenDepthToFroxelIndex(f32 screenX,
    const bool ok = FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
    const bool ok = tryPreflightSampleCoords(coords, desc, rejectReason);
    return ok && !sampleCoordRejectReasonIsBlocking(rejectReason);

// --- deepen additive from deepen-froxel-volumetrics-b511-c843 ---
    if (outCoords != nullptr && !screenMappingRejectReasonIsBlocking(reject)) {
    const bool mapped = preflightScreenDepthToSampleCoords(

// --- deepen additive from deepen-b511-froxel-classify-preflight-9310 ---
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, unused, reason);
        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reject);

// --- deepen additive from b511-froxel-classify-preflight-a761 ---
    return !preflightSampleCoords(coords, desc);
    return !preflightFroxelPopulate(desc, camera, params);

// --- deepen additive from deepen-froxel-volumetrics-b511-6328 ---
    const ScreenMappingRejectReason reject = classifyScreenMappingReject(screenX, screenY, viewDepth, desc, camera);

// --- deepen additive from deepen-b511-classify-preflight-d368 ---
    return !preflightFroxelTrilinearSample(grid, desc, coords);

// --- deepen additive from deepen-b511-froxel-classify-preflight-2ecc ---
    if (!tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, froxelIndex, reject)) {

// --- deepen additive from deepen-b511-froxel-guards-713a ---
bool preflightFroxelSampleAtCoords(const FroxelDensityGrid& grid,

// --- deepen additive from deepen-b511-froxel-guards-80e2 ---
bool FroxelGridLayout::preflightScreenMappingReady(f32 screenX,
bool preflightGridDensityReady(const FroxelDensityGrid& grid,

// --- deepen additive from deepen-froxel-volumetrics-b511-d9ce ---
bool FroxelGridLayout::preflightMapScreenDepthToSampleCoords(f32 screenX,
ScreenMappingRejectReason FroxelGridLayout::classifyScreenMappingFroxelIndexReject(f32 screenX,
bool FroxelGridLayout::preflightMapScreenDepthToFroxelIndex(f32 screenX,
bool preflightTrilinearSampleAtCoords(const FroxelDensityGrid& grid,

// --- deepen additive from deepen-froxel-volumetrics-b511-5991 ---
    const bool ok = tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, coords, rejectReason);

// --- deepen additive from deepen-b511-froxel-preflight-ce10 ---
    tryMapScreenDepthToSampleCoords(screenX, screenY, viewDepth, desc, camera, dummy, reason);
    const DensityLookupRejectReason reject = classifyDensityLookupAtCoordReject(grid, desc, tileX, tileY, sliceZ);

// --- deepen additive from deepen-froxel-volumetrics-b511-034f ---
    return !screenMappingRejectReasonIsBlocking(rejectReason);

// --- deepen additive from froxel-volumetric-b511-deepen-5b86 ---
    return preflightScreenMapping(screenX, screenY, viewDepth, desc, camera, reason);

// --- deepen additive from deepen-froxel-volumetrics-b511-81fc ---
bool screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason reason);
bool froxelTrilinearSampleRejectReasonIsBlocking(FroxelTrilinearSampleRejectReason reason);
bool gridDensityRejectReasonIsBlocking(GridDensityRejectReason reason);
bool densityLookupRejectReasonIsBlocking(DensityLookupRejectReason reason);
bool froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason reason);

// --- deepen additive from deepen-froxel-volumetrics-b511-f48a ---
        tryMapScreenDepthToFroxelIndex(screenX, screenY, viewDepth, desc, camera, index, mapReason);

// --- deepen additive from b511-froxel-volumetrics-deepen-6169 ---
bool preflightTrilinearSampleReady(const FroxelDensityGrid& grid,
bool preflightPopulateReady(const FroxelGridDesc& desc,

// --- deepen additive from froxel-volumetric-b511-deepen-8c6b ---
SampleCoordRejectReason classifySampleCoordReject(const FroxelSampleCoords& coords, const FroxelGridDesc& desc);
bool preflightSampleCoordsReady(const FroxelSampleCoords& coords,

// --- deepen additive from deepen-froxel-volumetric-guards-3b64 ---
    const bool ok = !screenMappingRejectReasonIsBlocking(reject);

// --- deepen additive from deepen-froxel-volumetric-guards-b511-4019 ---
    if (!preflightScreenMapping(screenX, screenY, viewDepth, desc, camera, &coords, reason)) {

// --- deepen additive from deepen-froxel-volumetric-guards-83db ---
bool preflightDensityLookupCoordReady(const FroxelDensityGrid& grid,

// --- deepen additive from deepen-froxel-volumetrics-b511-c943 ---
SampleCoordRejectReason classifyFroxelSampleAtCoordsReject(const FroxelDensityGrid& grid,
    const SampleCoordRejectReason reject = classifyFroxelSampleAtCoordsReject(grid, desc, coords);
