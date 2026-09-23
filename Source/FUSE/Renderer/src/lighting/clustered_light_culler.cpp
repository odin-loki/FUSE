// Clustered light culling. The cluster / slice / cull math lives once in
// fuse/renderer/lighting/clustered_kernel.hpp (FUSE_HOST_DEVICE, shared with kernels/clustered_lighting.cu);
// the scalar helpers here delegate to it and the cull runs as kernel::launch()es.

#include <fuse/renderer/lighting/clustered.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/lighting/clustered_kernel.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

#if defined(FUSE_HAS_CUDA)
/// kernels/clustered_lighting.cu: stages the lights + AABBs and runs the bounds + bin + cull bodies on the device.
bool launchClusteredCullCuda(const clustered_kernel::BoundsParams& bounds,
                             const clustered_kernel::BinParams& bin,
                             const clustered_kernel::CullParams& cull,
                             void* stream);
#endif

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

/// Camera with only the position + view basis resolved (worldToView / viewToWorld).
clustered_kernel::CameraView basisCamera(const ClusterCameraDesc& camera) {
    clustered_kernel::CameraView c{};
    c.position = camera.position;
    clustered_kernel::view_basis(camera.forward, camera.up, c.right, c.up, c.back);
    return c;
}

} // namespace

void ClusterCullLists::clear() {
    capacity = 0u;
    bounds.clear();
    sliceLights.clear();
    sliceCounts.clear();
    slots.clear();
    counts.clear();
    dropped.clear();
}

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
    return clustered_kernel::cluster_index(tileX, tileY, sliceZ, desc.tilesX, desc.slicesZ);
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
    return clustered_kernel::map_screen_depth_to_cluster(screenX, screenY, viewDepth, clustered_kernel::make_grid(desc),
                                                         camera.nearPlane, camera.farPlane, outClusterIndex);
}

f32 ClusterSliceLayout::computeSliceNearZ(u32 sliceZ, const ClusterDesc& desc, const ClusterCameraDesc& camera) {
    return clustered_kernel::slice_near_z(sliceZ, desc.slicesZ, camera.nearPlane, camera.farPlane);
}

f32 ClusterSliceLayout::computeSliceFarZ(u32 sliceZ, const ClusterDesc& desc, const ClusterCameraDesc& camera) {
    return clustered_kernel::slice_far_z(sliceZ, desc.slicesZ, camera.nearPlane, camera.farPlane);
}

u32 ClusterSliceLayout::computeSliceZFromDepth(f32 viewDepth, const ClusterDesc& desc, const ClusterCameraDesc& camera) {
    return clustered_kernel::slice_from_depth(viewDepth, desc.slicesZ, camera.nearPlane, camera.farPlane);
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
    clustered_kernel::view_basis(camera.forward, camera.up, outRight, outUp, outBack);
}

fuse::math::Vec3 cluster_math::worldToView(const ClusterCameraDesc& camera, const fuse::math::Vec3& world) {
    return clustered_kernel::world_to_view(basisCamera(camera), world);
}

fuse::math::Vec3 cluster_math::viewToWorld(const ClusterCameraDesc& camera, const fuse::math::Vec3& view) {
    return clustered_kernel::view_to_world(basisCamera(camera), view);
}

f32 cluster_math::viewDepthFromDeviceDepth(f32 deviceDepth, const ClusterCameraDesc& camera) {
    return clustered_kernel::view_depth_from_device_depth(deviceDepth, camera.nearPlane, camera.farPlane,
                                                          camera.reversedZ);
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
    return clustered_kernel::view_position_from_screen(screenX, screenY, viewDepth, tanX, tanY);
}

ClusterAABB cluster_math::buildClusterAabb(u32 tileX,
                                           u32 tileY,
                                           u32 sliceZ,
                                           const ClusterDesc& desc,
                                           const ClusterCameraDesc& camera) {
    const clustered_kernel::CameraView c = clustered_kernel::make_camera(camera);
    return clustered_kernel::build_cluster_aabb(tileX, tileY, sliceZ, clustered_kernel::make_grid(desc), c.near_plane,
                                                c.far_plane, c.tan_x, c.tan_y);
}

void cluster_math::buildClusterAabbs(const ClusterDesc& desc,
                                     const ClusterCameraDesc& camera,
                                     std::vector<ClusterAABB>& outAabbs,
                                     kernel::Backend backend) {
    const u32 clusterCount = desc.clusterCount();
    outAabbs.assign(clusterCount, ClusterAABB{});
    if (clusterCount == 0u) {
        return;
    }
    clustered_kernel::BuildParams params{};
    params.grid = clustered_kernel::make_grid(desc);
    params.camera = clustered_kernel::make_camera(camera);
    params.out_aabbs = {outAabbs.data(), clusterCount};
    kernel::launch(backend, clustered_kernel::make_linear_launch(clustered_kernel::kBuildName, clusterCount),
                   clustered_kernel::BuildKernel{}, params);
}

bool cluster_math::sphereIntersectsAabb(const fuse::math::Vec3& center, f32 radius, const ClusterAABB& aabb) {
    return clustered_kernel::sphere_intersects_aabb(center, radius, aabb);
}

ClusterCullResult cluster_math::cullLightsToClusterLists(const ClusterDesc& desc,
                                                         const ClusterCameraDesc& camera,
                                                         const std::vector<ClusterAABB>& aabbs,
                                                         const std::vector<PointLightInput>& pointLights,
                                                         const std::vector<SpotLightInput>& spotLights,
                                                         ClusterCullLists& outLists,
                                                         kernel::Backend backend) {
    ClusterCullResult result{};
    const u32 clusterCount = desc.clusterCount();
    const u32 pointCount = static_cast<u32>(pointLights.size());
    const u32 totalCount = pointCount + static_cast<u32>(spotLights.size());
    outLists.capacity = desc.maxLightsPerCluster > 0u ? std::min(desc.maxLightsPerCluster, totalCount) : totalCount;
    outLists.counts.assign(clusterCount, 0u);
    outLists.dropped.assign(clusterCount, 0u);
    outLists.slots.resize(static_cast<usize>(clusterCount) * outLists.capacity);
    outLists.bounds.resize(totalCount);
    if (clusterCount == 0u || aabbs.size() < clusterCount || totalCount == 0u) {
        return result;
    }

    const clustered_kernel::GridDims grid = clustered_kernel::make_grid(desc);
    clustered_kernel::BoundsParams bounds{};
    bounds.grid = grid;
    bounds.camera = clustered_kernel::make_camera(camera);
    bounds.point_lights = {pointLights.data(), pointCount};
    bounds.spot_lights = {spotLights.data(), static_cast<u32>(spotLights.size())};
    bounds.out_bounds = {outLists.bounds.data(), totalCount};
    outLists.sliceLights.resize(static_cast<usize>(desc.slicesZ) * totalCount);
    outLists.sliceCounts.assign(desc.slicesZ, 0u);
    clustered_kernel::BinParams bin{};
    bin.bounds = {outLists.bounds.data(), totalCount};
    bin.capacity = totalCount;
    bin.out_lights = {outLists.sliceLights.data(), static_cast<u32>(outLists.sliceLights.size())};
    bin.out_counts = {outLists.sliceCounts.data(), desc.slicesZ};

    clustered_kernel::CullParams cull{};
    cull.grid = grid;
    cull.capacity = outLists.capacity;
    cull.aabbs = {aabbs.data(), clusterCount};
    cull.bounds = {outLists.bounds.data(), totalCount};
    cull.slice_capacity = totalCount;
    cull.slice_lights = {outLists.sliceLights.data(), static_cast<u32>(outLists.sliceLights.size())};
    cull.slice_counts = {outLists.sliceCounts.data(), desc.slicesZ};
    cull.out_lights = {outLists.slots.data(), static_cast<u32>(outLists.slots.size())};
    cull.out_counts = {outLists.counts.data(), clusterCount};
    cull.out_dropped = {outLists.dropped.data(), clusterCount};

    bool onDevice = false;
#if defined(FUSE_HAS_CUDA)
    onDevice = (backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
               kernel::backend_available(kernel::Backend::Cuda) && launchClusteredCullCuda(bounds, bin, cull, nullptr);
#endif
    if (!onDevice) {
        // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback.
        kernel::launch(backend, clustered_kernel::make_linear_launch(clustered_kernel::kBoundsName, totalCount),
                       clustered_kernel::BoundsKernel{}, bounds);
        kernel::launch(backend, clustered_kernel::make_linear_launch(clustered_kernel::kBinName, desc.slicesZ),
                       clustered_kernel::BinKernel{}, bin);
        kernel::launch(backend, clustered_kernel::make_linear_launch(clustered_kernel::kCullName, clusterCount),
                       clustered_kernel::CullKernel{}, cull);
    }

    for (u32 c = 0; c < clusterCount; ++c) {
        result.lightsCulled += outLists.counts[c];
        result.lightsDroppedOverflow += outLists.dropped[c];
        result.clustersAtCapacity += outLists.dropped[c] != 0u ? 1u : 0u;
    }
    return result;
}

ClusterCullResult cluster_math::cullLightsToClusters(const ClusterDesc& desc,
                                                     const ClusterCameraDesc& camera,
                                                     const std::vector<ClusterAABB>& aabbs,
                                                     const std::vector<PointLightInput>& pointLights,
                                                     const std::vector<SpotLightInput>& spotLights,
                                                     std::vector<std::vector<u32>>& outPerClusterLights,
                                                     kernel::Backend backend) {
    ClusterCullLists lists{};
    const ClusterCullResult result =
        cullLightsToClusterLists(desc, camera, aabbs, pointLights, spotLights, lists, backend);
    const u32 clusterCount = desc.clusterCount();
    outPerClusterLights.resize(clusterCount);
    for (u32 c = 0; c < clusterCount; ++c) {
        const u32* first = lists.slots.data() + static_cast<usize>(c) * lists.capacity;
        outPerClusterLights[c].assign(first, first + lists.counts[c]);
    }
    return result;
}

void cluster_math::compactClusterLists(const ClusterCullLists& lists,
                                       u32 clusterCount,
                                       ClusterGridSoA& grid,
                                       kernel::Backend backend) {
    grid.lightList.clear();
    grid.grid.clear();
    if (clusterCount == 0u) {
        return;
    }
    grid.grid.assign(clusterCount, ClusterGridEntry{});
    if (lists.counts.size() < clusterCount) {
        return; // Nothing culled: every cell is empty.
    }
    // Exclusive scan (serial, cluster order): deterministic offsets on every backend.
    std::vector<u32> offsets(clusterCount);
    u32 total = 0u;
    for (u32 c = 0; c < clusterCount; ++c) {
        offsets[c] = total;
        grid.grid[c].offset = total;
        grid.grid[c].count = lists.counts[c];
        total += lists.counts[c];
    }
    grid.lightList.resize(total);
    if (total == 0u) {
        return;
    }
    clustered_kernel::CompactParams params{};
    params.capacity = lists.capacity;
    params.lights = {lists.slots.data(), static_cast<u32>(lists.slots.size())};
    params.counts = {lists.counts.data(), clusterCount};
    params.offsets = {offsets.data(), clusterCount};
    params.out_light_list = {grid.lightList.data(), total};
    kernel::launch(backend, clustered_kernel::make_linear_launch(clustered_kernel::kCompactName, clusterCount),
                   clustered_kernel::CompactKernel{}, params);
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

    cluster_math::buildClusterAabbs(m_desc, camera, m_gridSoA.aabbs, m_backend);
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

    const ClusterCullResult result = cluster_math::cullLightsToClusterLists(
        m_desc, camera, m_gridSoA.aabbs, pointLights, spotLights, m_pendingClusterLights, m_backend);
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

    // The fixed-capacity lists are already capped at maxLightsPerCluster.
    cluster_math::compactClusterLists(m_pendingClusterLights, m_desc.clusterCount(), m_gridSoA, m_backend);
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
