#include <fuse/renderer/lighting/clustered.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

BufferDesc makeStorageBuffer(usize size, const char* name) {
    BufferDesc desc{};
    desc.size = size;
    desc.usage = BufferUsage::Storage;
    desc.memoryUsage = MemoryUsage::GpuOnly;
    desc.cudaInterop = true;
    desc.name = name;
    return desc;
}

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

} // namespace

void ClusterGridSoA::allocate(const ClusterDesc& desc) {
    const ClusterDesc clampedDesc = ClusterDesc::clampCounts(desc);
    const u32 clusterCount = clampedDesc.clusterCount();
    aabbs.assign(clusterCount, ClusterAABB{});
    grid.assign(clusterCount, ClusterGridEntry{});
    lightList.clear();
}

void ClusterGridSoA::clear() {
    aabbs.clear();
    grid.clear();
    lightList.clear();
}

bool ClusterGridSoA::matchesDesc(const ClusterDesc& desc) const {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    return aabbs.size() == clusterCount && grid.size() == clusterCount;
}

ClusterDesc ClusterDesc::clampCounts(const ClusterDesc& raw) {
    ClusterDesc out = raw;
    if (out.tilesX > kMaxTilesX) {
        out.tilesX = kMaxTilesX;
    }
    if (out.tilesY > kMaxTilesY) {
        out.tilesY = kMaxTilesY;
    }
    if (out.slicesZ > kMaxSlicesZ) {
        out.slicesZ = kMaxSlicesZ;
    }
    if (out.maxLightsPerCluster > kMaxLightsPerCluster) {
        out.maxLightsPerCluster = kMaxLightsPerCluster;
    }
    return out;
}

const char* gridPopulationRejectReasonLabel(GridPopulationRejectReason reason) {
    switch (reason) {
    case GridPopulationRejectReason::None:
        return "none";
    case GridPopulationRejectReason::EmptyGrid:
        return "empty_grid";
    case GridPopulationRejectReason::DescMismatch:
        return "desc_mismatch";
    case GridPopulationRejectReason::UndersizedGrid:
        return "undersized_grid";
    case GridPopulationRejectReason::PopulationMismatch:
        return "population_mismatch";
    case GridPopulationRejectReason::NonContiguousOffsets:
        return "non_contiguous_offsets";
    }
    return "unknown";
}

const char* clusterLookupRejectReasonLabel(ClusterLookupRejectReason reason) {
    switch (reason) {
    case ClusterLookupRejectReason::None:
        return "none";
    case ClusterLookupRejectReason::EmptyGrid:
        return "empty_grid";
    case ClusterLookupRejectReason::DescMismatch:
        return "desc_mismatch";
    case ClusterLookupRejectReason::EmptyStorage:
        return "empty_storage";
    case ClusterLookupRejectReason::ScreenMappingFailed:
        return "screen_mapping_failed";
    }
    return "unknown";
}

bool cluster_util::gridMatchesDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    if (clusterCount == 0u) {
        return grid.grid.empty();
    }
    return grid.grid.size() == clusterCount;
}

bool cluster_util::isGridAccessible(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    return !ClusterGridLayout::isEmptyGrid(desc) && gridMatchesDesc(grid, desc);
}

bool cluster_util::isLightGridAccessible(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    return !grid.isEmpty() && !ClusterGridLayout::isEmptyGrid(desc) && gridMatchesDesc(grid, desc);
}

bool cluster_util::shouldSkipClusterLookup(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    return !isGridAccessible(grid, desc);
}

bool cluster_util::shouldSkipClusterCull(const ClusterDesc& desc) {
    return ClusterGridLayout::isEmptyGrid(desc);
}

bool cluster_util::canLookupAtIndex(const ClusterGridSoA& grid, const ClusterDesc& desc, u32 /*index*/) {
    return isGridAccessible(grid, desc);
}

bool cluster_util::canLookupAtCoord(const ClusterGridSoA& grid,
                                     const ClusterDesc& desc,
                                     u32 /*tileX*/,
                                     u32 /*tileY*/,
                                     u32 /*sliceZ*/) {
    return isGridAccessible(grid, desc);
}

bool cluster_util::tryCanLookupAtIndex(const ClusterGridSoA& grid,
                                        const ClusterDesc& desc,
                                        u32 /*index*/,
                                        ClusterLookupRejectReason& outReason) {
    if (ClusterGridLayout::isEmptyGrid(desc)) {
        outReason = ClusterLookupRejectReason::EmptyGrid;
        return false;
    }
    if (grid.grid.empty()) {
        outReason = ClusterLookupRejectReason::EmptyStorage;
        return false;
    }
    if (!gridMatchesDesc(grid, desc)) {
        outReason = ClusterLookupRejectReason::DescMismatch;
        return false;
    }

    outReason = ClusterLookupRejectReason::None;
    return true;
}

bool cluster_util::tryCanLookupAtCoord(const ClusterGridSoA& grid,
                                        const ClusterDesc& desc,
                                        u32 tileX,
                                        u32 tileY,
                                        u32 sliceZ,
                                        ClusterLookupRejectReason& outReason) {
    return tryCanLookupAtIndex(grid, desc, ClusterGridLayout::clusterIndexClamped(tileX, tileY, sliceZ, desc),
                               outReason);
}

u32 cluster_util::clusterLightCountAtIndex(const ClusterGridSoA& grid, const ClusterDesc& desc, u32 index) {
    if (ClusterGridLayout::isEmptyGrid(desc) || !gridMatchesDesc(grid, desc)) {
        return 0u;
    }

    const u32 clampedIndex = ClusterGridLayout::clampClusterIndex(index, desc);
    return clusterLightCount(grid, clampedIndex);
}

bool cluster_util::tryAssignLight(std::vector<u32>& clusterLights, u32 lightIdx, u32 maxLightsPerCluster) {
    if (maxLightsPerCluster > 0u && clusterLights.size() >= maxLightsPerCluster) {
        return false;
    }
    clusterLights.push_back(lightIdx);
    return true;
}

u32 cluster_util::assignLights(std::vector<u32>& clusterLights,
                                const std::vector<u32>& candidates,
                                u32 maxLightsPerCluster) {
    u32 dropped = 0u;
    for (u32 lightIdx : candidates) {
        if (!tryAssignLight(clusterLights, lightIdx, maxLightsPerCluster)) {
            ++dropped;
        }
    }
    return dropped;
}

bool cluster_util::assignLightToCluster(std::vector<std::vector<u32>>& perClusterLights,
                                         u32 clusterIdx,
                                         u32 lightIdx,
                                         u32 maxLightsPerCluster) {
    if (clusterIdx >= perClusterLights.size()) {
        return false;
    }
    return tryAssignLight(perClusterLights[clusterIdx], lightIdx, maxLightsPerCluster);
}

u32 cluster_util::lookupClusterLights(const ClusterGridSoA& grid, u32 clusterIdx, std::vector<u32>& outLights) {
    outLights.clear();
    if (clusterIdx >= grid.grid.size()) {
        return 0u;
    }

    const ClusterGridEntry& entry = grid.grid[clusterIdx];
    if (entry.count == 0u) {
        return 0u;
    }

    const u32 endOffset = entry.offset + entry.count;
    if (endOffset > grid.lightList.size()) {
        return 0u;
    }

    outLights.reserve(entry.count);
    for (u32 offset = entry.offset; offset < endOffset; ++offset) {
        outLights.push_back(grid.lightList[offset]);
    }
    return entry.count;
}

u32 cluster_util::lookupClusterLightsAtIndex(const ClusterGridSoA& grid,
                                              const ClusterDesc& desc,
                                              u32 index,
                                              std::vector<u32>& outLights) {
    if (!canLookupAtIndex(grid, desc, index)) {
        outLights.clear();
        return 0u;
    }

    const u32 clampedIndex = ClusterGridLayout::clampClusterIndex(index, desc);
    return lookupClusterLights(grid, clampedIndex, outLights);
}

u32 cluster_util::lookupClusterLightsAtCoord(const ClusterGridSoA& grid,
                                              const ClusterDesc& desc,
                                              u32 tileX,
                                              u32 tileY,
                                              u32 sliceZ,
                                              std::vector<u32>& outLights) {
    if (!canLookupAtCoord(grid, desc, tileX, tileY, sliceZ)) {
        outLights.clear();
        return 0u;
    }

    const u32 index = ClusterGridLayout::clusterIndexClamped(tileX, tileY, sliceZ, desc);
    return lookupClusterLights(grid, index, outLights);
}

u32 cluster_util::clusterLightCountAtCoord(const ClusterGridSoA& grid,
                                              const ClusterDesc& desc,
                                              u32 tileX,
                                              u32 tileY,
                                              u32 sliceZ) {
    if (!canLookupAtCoord(grid, desc, tileX, tileY, sliceZ)) {
        return 0u;
    }

    const u32 index = ClusterGridLayout::clusterIndexClamped(tileX, tileY, sliceZ, desc);
    return clusterLightCount(grid, index);
}

bool cluster_util::tryLookupClusterLightsAtIndex(const ClusterGridSoA& grid,
                                                  const ClusterDesc& desc,
                                                  u32 index,
                                                  std::vector<u32>& outLights,
                                                  u32& outCount) {
    ClusterLookupRejectReason reason = ClusterLookupRejectReason::None;
    return tryLookupClusterLightsAtIndex(grid, desc, index, outLights, outCount, reason);
}

bool cluster_util::tryLookupClusterLightsAtIndex(const ClusterGridSoA& grid,
                                                  const ClusterDesc& desc,
                                                  u32 index,
                                                  std::vector<u32>& outLights,
                                                  u32& outCount,
                                                  ClusterLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, index, outReason)) {
        outLights.clear();
        outCount = 0u;
        return false;
    }

    outCount = lookupClusterLightsAtIndex(grid, desc, index, outLights);
    outReason = ClusterLookupRejectReason::None;
    return true;
}

bool cluster_util::tryLookupClusterLightsAtCoord(const ClusterGridSoA& grid,
                                                  const ClusterDesc& desc,
                                                  u32 tileX,
                                                  u32 tileY,
                                                  u32 sliceZ,
                                                  std::vector<u32>& outLights,
                                                  u32& outCount) {
    ClusterLookupRejectReason reason = ClusterLookupRejectReason::None;
    return tryLookupClusterLightsAtCoord(grid, desc, tileX, tileY, sliceZ, outLights, outCount, reason);
}

bool cluster_util::tryLookupClusterLightsAtCoord(const ClusterGridSoA& grid,
                                                  const ClusterDesc& desc,
                                                  u32 tileX,
                                                  u32 tileY,
                                                  u32 sliceZ,
                                                  std::vector<u32>& outLights,
                                                  u32& outCount,
                                                  ClusterLookupRejectReason& outReason) {
    if (!tryCanLookupAtCoord(grid, desc, tileX, tileY, sliceZ, outReason)) {
        outLights.clear();
        outCount = 0u;
        return false;
    }

    outCount = lookupClusterLightsAtCoord(grid, desc, tileX, tileY, sliceZ, outLights);
    outReason = ClusterLookupRejectReason::None;
    return true;
}

bool cluster_util::tryLookupClusterLightsFromScreen(const ClusterGridSoA& grid,
                                                     const ClusterDesc& desc,
                                                     const ClusterCameraDesc& camera,
                                                     f32 screenX,
                                                     f32 screenY,
                                                     f32 viewDepth,
                                                     std::vector<u32>& outLights,
                                                     u32& outCount) {
    ClusterLookupRejectReason reason = ClusterLookupRejectReason::None;
    return tryLookupClusterLightsFromScreen(grid, desc, camera, screenX, screenY, viewDepth, outLights, outCount,
                                            reason);
}

bool cluster_util::tryLookupClusterLightsFromScreen(const ClusterGridSoA& grid,
                                                     const ClusterDesc& desc,
                                                     const ClusterCameraDesc& camera,
                                                     f32 screenX,
                                                     f32 screenY,
                                                     f32 viewDepth,
                                                     std::vector<u32>& outLights,
                                                     u32& outCount,
                                                     ClusterLookupRejectReason& outReason) {
    if (!tryCanLookupAtIndex(grid, desc, 0u, outReason)) {
        outLights.clear();
        outCount = 0u;
        return false;
    }

    u32 clusterIndex = 0u;
    if (!ClusterGridLayout::mapScreenDepthToClusterIndex(screenX, screenY, viewDepth, desc, camera, clusterIndex)) {
        outLights.clear();
        outCount = 0u;
        outReason = ClusterLookupRejectReason::ScreenMappingFailed;
        return false;
    }

    return tryLookupClusterLightsAtIndex(grid, desc, clusterIndex, outLights, outCount, outReason);
}

u32 cluster_util::clusterLightCount(const ClusterGridSoA& grid, u32 clusterIdx) {
    if (clusterIdx >= grid.grid.size()) {
        return 0u;
    }
    return grid.grid[clusterIdx].count;
}

u32 cluster_util::countAssignedLights(const ClusterGridSoA& grid, u32 clusterCount) {
    if (grid.grid.size() < clusterCount) {
        return 0u;
    }

    u32 total = 0u;
    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        total += grid.grid[clusterIdx].count;
    }
    return total;
}

u32 cluster_util::countNonEmptyClusters(const ClusterGridSoA& grid, u32 clusterCount) {
    if (clusterCount == 0u || grid.grid.size() < clusterCount) {
        return 0u;
    }

    u32 nonEmpty = 0u;
    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        if (grid.grid[clusterIdx].count > 0u) {
            ++nonEmpty;
        }
    }
    return nonEmpty;
}

u32 cluster_util::countEmptyClusters(const ClusterGridSoA& grid, u32 clusterCount) {
    if (clusterCount == 0u) {
        return 0u;
    }
    if (grid.grid.size() < clusterCount) {
        return clusterCount;
    }

    u32 empty = 0u;
    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        if (grid.grid[clusterIdx].count == 0u) {
            ++empty;
        }
    }
    return empty;
}

u32 cluster_util::countClustersAtCapacity(const ClusterGridSoA& grid, u32 clusterCount, u32 maxLightsPerCluster) {
    if (clusterCount == 0u || maxLightsPerCluster == 0u || grid.grid.size() < clusterCount) {
        return 0u;
    }

    u32 atCapacity = 0u;
    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        if (grid.grid[clusterIdx].count >= maxLightsPerCluster) {
            ++atCapacity;
        }
    }
    return atCapacity;
}

bool cluster_util::validatePopulationCounts(const ClusterGridSoA& grid, u32 clusterCount) {
    if (clusterCount == 0u) {
        return true;
    }
    if (grid.grid.size() < clusterCount) {
        return false;
    }
    if (countNonEmptyClusters(grid, clusterCount) + countEmptyClusters(grid, clusterCount) != clusterCount) {
        return false;
    }
    return countAssignedLights(grid, clusterCount) == grid.lightList.size();
}

bool cluster_util::tryValidateGridPopulation(const ClusterGridSoA& grid,
                                              u32 clusterCount,
                                              GridPopulationRejectReason& outReason) {
    if (clusterCount == 0u) {
        outReason = GridPopulationRejectReason::None;
        return true;
    }
    if (grid.grid.size() < clusterCount) {
        outReason = GridPopulationRejectReason::UndersizedGrid;
        return false;
    }
    if (countNonEmptyClusters(grid, clusterCount) + countEmptyClusters(grid, clusterCount) != clusterCount ||
        countAssignedLights(grid, clusterCount) != grid.lightList.size()) {
        outReason = GridPopulationRejectReason::PopulationMismatch;
        return false;
    }
    if (!ClusterLightGridLayout::validateContiguousOffsets(grid, clusterCount)) {
        outReason = GridPopulationRejectReason::NonContiguousOffsets;
        return false;
    }

    outReason = GridPopulationRejectReason::None;
    return true;
}

bool cluster_util::validateGridPopulation(const ClusterGridSoA& grid, u32 clusterCount) {
    GridPopulationRejectReason reason = GridPopulationRejectReason::None;
    return tryValidateGridPopulation(grid, clusterCount, reason);
}

bool cluster_util::validateGridPopulationForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    GridPopulationRejectReason reason = GridPopulationRejectReason::None;
    return tryValidateGridPopulationForDesc(grid, desc, reason);
}

bool cluster_util::tryValidateGridPopulationForDesc(const ClusterGridSoA& grid,
                                                     const ClusterDesc& desc,
                                                     GridPopulationRejectReason& outReason) {
    const ClusterDesc clampedDesc = ClusterDesc::clampCounts(desc);
    const u32 clusterCount = clampedDesc.clusterCount();

    if (clusterCount == 0u) {
        outReason = GridPopulationRejectReason::None;
        return true;
    }
    if (ClusterGridLayout::isEmptyGrid(desc)) {
        outReason = GridPopulationRejectReason::EmptyGrid;
        return false;
    }
    if (!gridMatchesDesc(grid, desc)) {
        outReason = GridPopulationRejectReason::DescMismatch;
        return false;
    }

    return tryValidateGridPopulation(grid, clusterCount, outReason);
}

u32 cluster_util::countAssignedLightsForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    if (!gridMatchesDesc(grid, desc)) {
        return 0u;
    }
    return countAssignedLights(grid, ClusterDesc::clampCounts(desc).clusterCount());
}

bool cluster_util::hasAssignedLights(const ClusterGridSoA& grid, u32 clusterCount) {
    return countNonEmptyClusters(grid, clusterCount) > 0u;
}

bool cluster_util::isPopulationFullyEmpty(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    if (!isGridAccessible(grid, desc)) {
        return false;
    }
    return !hasAssignedLights(grid, ClusterDesc::clampCounts(desc).clusterCount());
}

u32 cluster_util::countNonEmptyClustersForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    if (!gridMatchesDesc(grid, desc)) {
        return 0u;
    }
    return countNonEmptyClusters(grid, ClusterDesc::clampCounts(desc).clusterCount());
}

u32 cluster_util::countEmptyClustersForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    if (!gridMatchesDesc(grid, desc)) {
        return clusterCount;
    }
    return countEmptyClusters(grid, clusterCount);
}

bool cluster_util::validatePopulationCountsForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    if (!gridMatchesDesc(grid, desc)) {
        return false;
    }
    return validatePopulationCounts(grid, ClusterDesc::clampCounts(desc).clusterCount());
}

bool ClusterLightGridLayout::canRebuildLightGrid(const ClusterDesc& desc, u32 clusterCount) {
    const u32 expectedCount = ClusterDesc::clampCounts(desc).clusterCount();
    if (clusterCount == 0u) {
        return true;
    }
    if (ClusterGridLayout::isEmptyGrid(desc)) {
        return false;
    }
    return clusterCount == expectedCount;
}

bool ClusterLightGridLayout::canRebuildLightGridForDesc(const ClusterDesc& desc) {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    return canRebuildLightGrid(desc, clusterCount);
}

bool ClusterGridLayout::isEmptyGrid(const ClusterDesc& desc) {
    return desc.tilesX == 0u || desc.tilesY == 0u || desc.slicesZ == 0u;
}

u32 ClusterGridLayout::clusterIndex(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc) {
    return (tileY * desc.tilesX + tileX) * desc.slicesZ + sliceZ;
}

u32 ClusterGridLayout::clusterIndexClamped(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc) {
    return clusterIndex(clampTileX(tileX, desc), clampTileY(tileY, desc), clampSliceZ(sliceZ, desc), desc);
}

void ClusterGridLayout::decodeClusterIndex(u32 index, const ClusterDesc& desc, u32& tileX, u32& tileY, u32& sliceZ) {
    if (isEmptyGrid(desc)) {
        tileX = 0u;
        tileY = 0u;
        sliceZ = 0u;
        return;
    }

    const u32 clampedIndex = clampClusterIndex(index, desc);
    sliceZ = clampedIndex % desc.slicesZ;
    const u32 tileSlice = clampedIndex / desc.slicesZ;
    tileX = tileSlice % desc.tilesX;
    tileY = tileSlice / desc.tilesX;
}

bool ClusterGridLayout::isValidClusterIndex(u32 index, const ClusterDesc& desc) {
    return index < desc.clusterCount();
}

bool ClusterGridLayout::isClusterIndexOutOfRange(u32 index, const ClusterDesc& desc) {
    const u32 count = desc.clusterCount();
    return count == 0u || index >= count;
}

u32 ClusterGridLayout::clampClusterIndex(u32 index, const ClusterDesc& desc) {
    const u32 clusterCount = desc.clusterCount();
    if (clusterCount == 0u) {
        return 0u;
    }
    return std::min(index, clusterCount - 1u);
}

u32 ClusterGridLayout::clampTileX(u32 tileX, const ClusterDesc& desc) {
    if (desc.tilesX == 0u) {
        return 0u;
    }
    return std::min(tileX, desc.tilesX - 1u);
}

u32 ClusterGridLayout::clampTileY(u32 tileY, const ClusterDesc& desc) {
    if (desc.tilesY == 0u) {
        return 0u;
    }
    return std::min(tileY, desc.tilesY - 1u);
}

u32 ClusterGridLayout::clampSliceZ(u32 sliceZ, const ClusterDesc& desc) {
    if (desc.slicesZ == 0u) {
        return 0u;
    }
    return std::min(sliceZ, desc.slicesZ - 1u);
}

u32 ClusterGridLayout::maxClusterIndex(const ClusterDesc& desc) {
    return desc.maxClusterIndex();
}

bool ClusterGridLayout::isAtMaxClusterIndex(u32 index, const ClusterDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    return index == maxClusterIndex(desc);
}

bool ClusterGridLayout::tryClampClusterIndex(u32 index, const ClusterDesc& desc, u32& outIndex) {
    if (isEmptyGrid(desc)) {
        outIndex = 0u;
        return false;
    }

    outIndex = clampClusterIndex(index, desc);
    return true;
}

bool ClusterGridLayout::mapScreenDepthToClusterIndex(f32 screenX,
                                                     f32 screenY,
                                                     f32 viewDepth,
                                                     const ClusterDesc& desc,
                                                     const ClusterCameraDesc& camera,
                                                     u32& outClusterIndex) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return false;
    }

    const u32 tileX = static_cast<u32>(clamp01(screenX) * static_cast<f32>(desc.tilesX));
    const u32 tileY = static_cast<u32>(clamp01(screenY) * static_cast<f32>(desc.tilesY));
    const u32 sliceZ = ClusterSliceLayout::computeSliceZFromDepth(viewDepth, desc, camera);
    outClusterIndex = clusterIndexClamped(tileX, tileY, sliceZ, desc);
    return true;
}

f32 ClusterSliceLayout::computeSliceNearZ(u32 sliceZ, const ClusterDesc& desc, const ClusterCameraDesc& camera) {
    if (sliceZ >= desc.slicesZ) {
        return camera.farPlane;
    }

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 sliceT0 = static_cast<f32>(sliceZ) / static_cast<f32>(desc.slicesZ);
    return camera.nearPlane * std::pow(depthRatio, sliceT0);
}

f32 ClusterSliceLayout::computeSliceFarZ(u32 sliceZ, const ClusterDesc& desc, const ClusterCameraDesc& camera) {
    if (sliceZ >= desc.slicesZ) {
        return camera.farPlane;
    }

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 sliceT1 = static_cast<f32>(sliceZ + 1u) / static_cast<f32>(desc.slicesZ);
    return camera.nearPlane * std::pow(depthRatio, sliceT1);
}

u32 ClusterSliceLayout::computeSliceZFromDepth(f32 viewDepth, const ClusterDesc& desc, const ClusterCameraDesc& camera) {
    if (desc.slicesZ == 0u || camera.nearPlane <= 0.f || camera.farPlane <= camera.nearPlane) {
        return 0u;
    }

    const f32 clampedDepth = std::clamp(viewDepth, camera.nearPlane, camera.farPlane);
    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 logDepth = std::log(clampedDepth / camera.nearPlane) / std::log(depthRatio);
    const u32 sliceZ = static_cast<u32>(logDepth * static_cast<f32>(desc.slicesZ));
    return ClusterGridLayout::clampSliceZ(sliceZ, desc);
}

u32 ClusterLightGridLayout::rebuildLightGridForDesc(ClusterGridSoA& grid,
                                                     const ClusterDesc& desc,
                                                     const std::vector<std::vector<u32>>& perClusterLights,
                                                     u32 maxLightsPerCluster) {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    return rebuildLightGrid(grid, clusterCount, perClusterLights, maxLightsPerCluster);
}

u32 ClusterLightGridLayout::rebuildLightGrid(ClusterGridSoA& grid,
                                              u32 clusterCount,
                                              const std::vector<std::vector<u32>>& perClusterLights,
                                              u32 maxLightsPerCluster) {
    grid.lightList.clear();
    grid.grid.clear();
    if (clusterCount == 0u) {
        return 0u;
    }

    grid.grid.assign(clusterCount, ClusterGridEntry{});

    u32 lightsDropped = 0u;
    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        ClusterGridEntry& entry = grid.grid[clusterIdx];
        entry.offset = static_cast<u32>(grid.lightList.size());

        if (clusterIdx < perClusterLights.size()) {
            u32 assigned = 0u;
            for (u32 lightIdx : perClusterLights[clusterIdx]) {
                if (maxLightsPerCluster > 0u && assigned >= maxLightsPerCluster) {
                    ++lightsDropped;
                    continue;
                }
                grid.lightList.push_back(lightIdx);
                ++entry.count;
                ++assigned;
            }
        }
    }

    return lightsDropped;
}

bool ClusterLightGridLayout::validateContiguousOffsetsForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    return validateContiguousOffsets(grid, clusterCount);
}

bool ClusterLightGridLayout::validateContiguousOffsets(const ClusterGridSoA& grid, u32 clusterCount) {
    if (clusterCount == 0u) {
        return true;
    }
    if (grid.grid.size() < clusterCount) {
        return false;
    }

    u32 expectedOffset = 0;
    u32 totalLights = 0;
    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        const ClusterGridEntry& entry = grid.grid[clusterIdx];
        if (entry.offset != expectedOffset) {
            return false;
        }
        expectedOffset += entry.count;
        totalLights += entry.count;
    }

    return totalLights == grid.lightList.size();
}

void cluster_math::viewBasis(const ClusterCameraDesc& camera,
                             fuse::math::Vec3& outRight,
                             fuse::math::Vec3& outUp,
                             fuse::math::Vec3& outBack) {
    fuse::math::Vec3 forward = camera.forward.normalized();
    if (forward.dot(forward) == 0.f) {
        forward = {0.f, 0.f, -1.f};
    }
    fuse::math::Vec3 right = fuse::math::cross(forward, camera.up).normalized();
    if (right.dot(right) == 0.f) {
        // Up hint parallel to forward: pick any perpendicular axis.
        const fuse::math::Vec3 fallback =
            std::fabs(forward.y) < 0.99f ? fuse::math::Vec3{0.f, 1.f, 0.f} : fuse::math::Vec3{1.f, 0.f, 0.f};
        right = fuse::math::cross(forward, fallback).normalized();
    }
    outRight = right;
    outUp = fuse::math::cross(right, forward);
    outBack = forward * -1.f;
}

fuse::math::Vec3 cluster_math::worldToView(const ClusterCameraDesc& camera, const fuse::math::Vec3& world) {
    fuse::math::Vec3 right;
    fuse::math::Vec3 up;
    fuse::math::Vec3 back;
    viewBasis(camera, right, up, back);
    const fuse::math::Vec3 rel = world - camera.position;
    return {rel.dot(right), rel.dot(up), rel.dot(back)};
}

fuse::math::Vec3 cluster_math::viewToWorld(const ClusterCameraDesc& camera, const fuse::math::Vec3& view) {
    fuse::math::Vec3 right;
    fuse::math::Vec3 up;
    fuse::math::Vec3 back;
    viewBasis(camera, right, up, back);
    return camera.position + right * view.x + up * view.y + back * view.z;
}

f32 cluster_math::viewDepthFromDeviceDepth(f32 deviceDepth, const ClusterCameraDesc& camera) {
    if (camera.reversedZ) {
        // Infinite-far reversed-Z: ndc = near / d; 0 is the cleared (sky) value.
        return deviceDepth > 0.f ? camera.nearPlane / deviceDepth : 0.f;
    }
    // Standard [0,1]: ndc = far (d - near) / (d (far - near)); 1 is the cleared value.
    if (deviceDepth >= 1.f || camera.farPlane <= camera.nearPlane) {
        return 0.f;
    }
    const f32 n = camera.nearPlane;
    const f32 f = camera.farPlane;
    return (n * f) / (f - deviceDepth * (f - n));
}

f32 cluster_math::deviceDepthFromViewDepth(f32 viewDepth, const ClusterCameraDesc& camera) {
    if (viewDepth <= 0.f) {
        return camera.reversedZ ? 0.f : 1.f;
    }
    if (camera.reversedZ) {
        return camera.nearPlane / viewDepth;
    }
    const f32 n = camera.nearPlane;
    const f32 f = camera.farPlane;
    return (f * (viewDepth - n)) / (viewDepth * (f - n));
}

fuse::math::Vec3 cluster_math::viewPositionFromScreen(f32 screenX,
                                                      f32 screenY,
                                                      f32 viewDepth,
                                                      const ClusterCameraDesc& camera) {
    const f32 tanY = std::tan(camera.fovYRadians * 0.5f);
    const f32 tanX = tanY * camera.aspect();
    const f32 ndcX = screenX * 2.f - 1.f;
    const f32 ndcY = 1.f - screenY * 2.f;
    return {ndcX * tanX * viewDepth, ndcY * tanY * viewDepth, -viewDepth};
}

ClusterAABB cluster_math::buildClusterAabb(u32 tileX,
                                           u32 tileY,
                                           u32 sliceZ,
                                           const ClusterDesc& desc,
                                           const ClusterCameraDesc& camera) {
    ClusterAABB aabb{};
    if (desc.tilesX == 0u || desc.tilesY == 0u || desc.slicesZ == 0u) {
        return aabb;
    }

    const f32 tanY = std::tan(camera.fovYRadians * 0.5f);
    const f32 tanX = tanY * camera.aspect();
    const f32 depths[2] = {ClusterSliceLayout::computeSliceNearZ(sliceZ, desc, camera),
                           ClusterSliceLayout::computeSliceFarZ(sliceZ, desc, camera)};
    // Tile 0 is the top row (screenY = 0 -> ndc y = +1), matching mapScreenDepthToClusterIndex.
    const f32 ndcX[2] = {(static_cast<f32>(tileX) / static_cast<f32>(desc.tilesX)) * 2.f - 1.f,
                         (static_cast<f32>(tileX + 1u) / static_cast<f32>(desc.tilesX)) * 2.f - 1.f};
    const f32 ndcY[2] = {1.f - (static_cast<f32>(tileY + 1u) / static_cast<f32>(desc.tilesY)) * 2.f,
                         1.f - (static_cast<f32>(tileY) / static_cast<f32>(desc.tilesY)) * 2.f};

    // The cell is the convex hull of its 8 corners, so their bounds are the tight AABB.
    bool first = true;
    for (f32 depth : depths) {
        for (f32 nx : ndcX) {
            for (f32 ny : ndcY) {
                const fuse::math::Vec3 corner{nx * tanX * depth, ny * tanY * depth, -depth};
                if (first) {
                    aabb.minP = corner;
                    aabb.maxP = corner;
                    first = false;
                    continue;
                }
                aabb.minP = {std::min(aabb.minP.x, corner.x), std::min(aabb.minP.y, corner.y),
                             std::min(aabb.minP.z, corner.z)};
                aabb.maxP = {std::max(aabb.maxP.x, corner.x), std::max(aabb.maxP.y, corner.y),
                             std::max(aabb.maxP.z, corner.z)};
            }
        }
    }
    return aabb;
}

void cluster_math::buildClusterAabbs(const ClusterDesc& desc,
                                     const ClusterCameraDesc& camera,
                                     std::vector<ClusterAABB>& outAabbs) {
    outAabbs.assign(desc.clusterCount(), ClusterAABB{});
    for (u32 sliceZ = 0; sliceZ < desc.slicesZ; ++sliceZ) {
        for (u32 tileY = 0; tileY < desc.tilesY; ++tileY) {
            for (u32 tileX = 0; tileX < desc.tilesX; ++tileX) {
                outAabbs[ClusterGridLayout::clusterIndex(tileX, tileY, sliceZ, desc)] =
                    buildClusterAabb(tileX, tileY, sliceZ, desc, camera);
            }
        }
    }
}

bool cluster_math::sphereIntersectsAabb(const fuse::math::Vec3& center, f32 radius, const ClusterAABB& aabb) {
    if (!(radius >= 0.f) || !std::isfinite(radius)) {
        return false;
    }
    const f32 closestX = std::clamp(center.x, aabb.minP.x, aabb.maxP.x);
    const f32 closestY = std::clamp(center.y, aabb.minP.y, aabb.maxP.y);
    const f32 closestZ = std::clamp(center.z, aabb.minP.z, aabb.maxP.z);

    const f32 dx = center.x - closestX;
    const f32 dy = center.y - closestY;
    const f32 dz = center.z - closestZ;
    return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
}

ClusterCullResult cluster_math::cullLightsToClusters(const ClusterDesc& desc,
                                                     const ClusterCameraDesc& camera,
                                                     const std::vector<ClusterAABB>& aabbs,
                                                     const std::vector<PointLightInput>& pointLights,
                                                     const std::vector<SpotLightInput>& spotLights,
                                                     std::vector<std::vector<u32>>& outPerClusterLights) {
    ClusterCullResult result{};
    const u32 clusterCount = desc.clusterCount();
    outPerClusterLights.resize(clusterCount);
    for (std::vector<u32>& lights : outPerClusterLights) {
        lights.clear();
    }
    if (clusterCount == 0u || aabbs.size() < clusterCount) {
        return result;
    }

    std::vector<u8> overflowed(clusterCount, 0u);
    const u32 pointCount = static_cast<u32>(pointLights.size());
    const u32 totalCount = pointCount + static_cast<u32>(spotLights.size());
    const bool usePrefilter = desc.tilesX <= 32u && desc.tilesY <= 32u; // Row/column bit masks.

    // Light-major loop keeps every cluster list in ascending light index (same order a
    // cluster-major brute force produces). Only depth slices the sphere can reach are visited;
    // the slice window is widened by one on each side so float rounding in the log/pow slice
    // mapping can never skip a cluster — the exact sphere/AABB test decides membership.
    for (u32 lightIdx = 0; lightIdx < totalCount; ++lightIdx) {
        fuse::math::Vec3 worldPos;
        f32 radius = 0.f;
        if (lightIdx < pointCount) {
            worldPos = pointLights[lightIdx].position;
            radius = pointLights[lightIdx].radius;
        } else {
            // Spot lights are bounded by their range sphere (conservative superset of the cone).
            worldPos = spotLights[lightIdx - pointCount].position;
            radius = spotLights[lightIdx - pointCount].radius;
        }
        // A light with no range (falloff is identically zero) occupies no cluster.
        if (!(radius > 0.f) || !std::isfinite(radius) || !std::isfinite(worldPos.x) ||
            !std::isfinite(worldPos.y) || !std::isfinite(worldPos.z)) {
            continue;
        }

        const fuse::math::Vec3 center = worldToView(camera, worldPos);
        const f32 depth = -center.z;
        const f32 minDepth = std::clamp(depth - radius, camera.nearPlane, camera.farPlane);
        const f32 maxDepth = std::clamp(depth + radius, camera.nearPlane, camera.farPlane);
        u32 sliceLo = ClusterSliceLayout::computeSliceZFromDepth(minDepth, desc, camera);
        u32 sliceHi = ClusterSliceLayout::computeSliceZFromDepth(maxDepth, desc, camera);
        sliceLo = sliceLo > 0u ? sliceLo - 1u : 0u;
        sliceHi = std::min(sliceHi + 1u, desc.slicesZ - 1u);

        const f32 radiusSq = radius * radius;
        for (u32 sliceZ = sliceLo; sliceZ <= sliceHi; ++sliceZ) {
            // Per-axis prefilter: a tile's x bounds depend only on (tileX, slice) and its y bounds
            // only on (tileY, slice), so reject whole columns/rows whose single-axis separation
            // already exceeds the radius. Same arithmetic as the exact test, so it never rejects a
            // cluster the exact test would accept.
            u32 rowMask = usePrefilter ? 0u : ~0u;
            for (u32 tileY = 0; usePrefilter && tileY < desc.tilesY; ++tileY) {
                const ClusterAABB& box = aabbs[ClusterGridLayout::clusterIndex(0u, tileY, sliceZ, desc)];
                const f32 dy = center.y - std::clamp(center.y, box.minP.y, box.maxP.y);
                if (dy * dy <= radiusSq) {
                    rowMask |= 1u << tileY;
                }
            }
            u32 columnMask = usePrefilter ? 0u : ~0u;
            for (u32 tileX = 0; usePrefilter && tileX < desc.tilesX; ++tileX) {
                const ClusterAABB& box = aabbs[ClusterGridLayout::clusterIndex(tileX, 0u, sliceZ, desc)];
                const f32 dx = center.x - std::clamp(center.x, box.minP.x, box.maxP.x);
                if (dx * dx <= radiusSq) {
                    columnMask |= 1u << tileX;
                }
            }
            if (rowMask == 0u || columnMask == 0u) {
                continue;
            }

            for (u32 tileY = 0; tileY < desc.tilesY; ++tileY) {
                if (usePrefilter && (rowMask & (1u << tileY)) == 0u) {
                    continue;
                }
                for (u32 tileX = 0; tileX < desc.tilesX; ++tileX) {
                    if (usePrefilter && (columnMask & (1u << tileX)) == 0u) {
                        continue;
                    }
                    const u32 clusterIdx = ClusterGridLayout::clusterIndex(tileX, tileY, sliceZ, desc);
                    if (!sphereIntersectsAabb(center, radius, aabbs[clusterIdx])) {
                        continue;
                    }
                    if (cluster_util::tryAssignLight(outPerClusterLights[clusterIdx], lightIdx,
                                                     desc.maxLightsPerCluster)) {
                        ++result.lightsCulled;
                    } else {
                        ++result.lightsDroppedOverflow;
                        overflowed[clusterIdx] = 1u;
                    }
                }
            }
        }
    }

    for (u8 flag : overflowed) {
        result.clustersAtCapacity += flag;
    }
    return result;
}

void ClusteredLightCuller::init(const ClusterDesc& desc, ResourceManager& resources) {
    destroy();
    m_desc = ClusterDesc::clampCounts(desc);
    m_resources = &resources;

    const u32 clusterCount = m_desc.clusterCount();
    m_buffers.clusterCount = clusterCount;

    if (clusterCount == 0u) {
        m_gridSoA.allocate(m_desc);
        m_stats.ready = true;
        return;
    }

    m_buffers.clusterAabbs =
        resources.createBuffer(makeStorageBuffer(static_cast<usize>(clusterCount) * sizeof(ClusterAABB),
                                                 "cluster_aabbs"));
    m_buffers.lightGrid =
        resources.createBuffer(makeStorageBuffer(static_cast<usize>(clusterCount) * sizeof(ClusterGridEntry),
                                                 "cluster_light_grid"));
    m_buffers.lightList = resources.createBuffer(
        makeStorageBuffer(static_cast<usize>(clusterCount) * m_desc.maxLightsPerCluster * sizeof(u32),
                          "cluster_light_list"));
    m_buffers.lightSsbo =
        resources.createBuffer(makeStorageBuffer(static_cast<usize>(m_desc.maxLightsPerCluster) * sizeof(GPUPointLight),
                                                 "cluster_light_ssbo"));

    if (!m_buffers.clusterAabbs.isValid() || !m_buffers.lightGrid.isValid() ||
        !m_buffers.lightList.isValid() || !m_buffers.lightSsbo.isValid()) {
        destroy();
        return;
    }

    m_gridSoA.allocate(m_desc);
    m_stats.ready = true;
}

void ClusteredLightCuller::destroy() {
    if (m_resources != nullptr) {
        if (m_buffers.clusterAabbs.isValid()) {
            m_resources->destroyBuffer(m_buffers.clusterAabbs);
        }
        if (m_buffers.lightGrid.isValid()) {
            m_resources->destroyBuffer(m_buffers.lightGrid);
        }
        if (m_buffers.lightList.isValid()) {
            m_resources->destroyBuffer(m_buffers.lightList);
        }
        if (m_buffers.lightSsbo.isValid()) {
            m_resources->destroyBuffer(m_buffers.lightSsbo);
        }
    }

    m_desc = {};
    m_buffers = {};
    m_gridSoA = {};
    m_stats = {};
    m_resources = nullptr;
}

void ClusteredLightCuller::updateClusters(const ClusterCameraDesc& camera) {
    if (!m_stats.ready) {
        return;
    }

    const u32 clusterCount = m_desc.clusterCount();
    if (ClusterGridLayout::isEmptyGrid(m_desc) || clusterCount == 0u) {
        m_gridSoA.clear();
        m_stats.clustersBuilt = 0;
        return;
    }

    if (m_gridSoA.aabbs.size() != clusterCount || m_gridSoA.grid.size() != clusterCount) {
        m_gridSoA.allocate(m_desc);
    }

    cluster_math::buildClusterAabbs(m_desc, camera, m_gridSoA.aabbs);
    m_stats.clustersBuilt = clusterCount;
}

bool ClusteredLightCuller::sphereIntersectsAabb(const fuse::math::Vec3& center,
                                                 f32 radius,
                                                 const ClusterAABB& aabb) const {
    return cluster_math::sphereIntersectsAabb(center, radius, aabb);
}

void ClusteredLightCuller::cullLights(const std::vector<PointLightInput>& pointLights,
                                      const std::vector<SpotLightInput>& spotLights,
                                      const ClusterCameraDesc& camera) {
    if (!m_stats.ready) {
        return;
    }

    updateClusters(camera);

    m_stats.clustersAtCapacity = 0;
    m_stats.lightsDroppedOverflow = 0;

    if (cluster_util::shouldSkipClusterCull(m_desc) || m_desc.clusterCount() == 0u) {
        m_pendingClusterLights.clear();
        rebuildLightGrid();
        m_stats.lightsCulled = 0;
        m_stats.lightListEntries = 0;
        return;
    }

    const ClusterCullResult result = cluster_math::cullLightsToClusters(
        m_desc, camera, m_gridSoA.aabbs, pointLights, spotLights, m_pendingClusterLights);
    rebuildLightGrid();

    m_stats.lightsCulled = result.lightsCulled;
    m_stats.lightListEntries = static_cast<u32>(m_gridSoA.lightList.size());
    m_stats.clustersAtCapacity = result.clustersAtCapacity;
    m_stats.lightsDroppedOverflow = result.lightsDroppedOverflow;
}

void ClusteredLightCuller::rebuildLightGrid() {
    if (!m_stats.ready) {
        return;
    }

    ClusterLightGridLayout::rebuildLightGrid(m_gridSoA,
                                             m_desc.clusterCount(),
                                             m_pendingClusterLights,
                                             m_desc.maxLightsPerCluster);
}

void ClusteredLightCuller::recordCullPass(CommandBufferRecorder& recorder,
                                          const ClusterCameraDesc& camera,
                                          const std::vector<PointLightInput>& pointLights,
                                          const std::vector<SpotLightInput>& spotLights) {
    if (!m_stats.ready || cluster_util::shouldSkipClusterCull(m_desc)) {
        return;
    }

    recorder.beginPass("clustered_light_cull");
    cullLights(pointLights, spotLights, camera);
    recorder.endPass();
    ++m_stats.cullPassCount;
}

} // namespace fuse::renderer
