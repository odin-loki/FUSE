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

bool cluster_util::gridMatchesDesc(const ClusterGridSoA& grid, const ClusterDesc& desc) {
    const u32 clusterCount = ClusterDesc::clampCounts(desc).clusterCount();
    if (clusterCount == 0u) {
        return grid.grid.empty();
    }
    return grid.grid.size() == clusterCount;
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
    if (ClusterGridLayout::isEmptyGrid(desc) || !gridMatchesDesc(grid, desc)) {
        outLights.clear();
        return 0u;
    }

    const u32 clampedIndex = ClusterGridLayout::clampClusterIndex(index, desc);
    return lookupClusterLights(grid, clampedIndex, outLights);
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
    return countNonEmptyClusters(grid, clusterCount) + countEmptyClusters(grid, clusterCount) == clusterCount;
}

bool cluster_util::validateGridPopulation(const ClusterGridSoA& grid, u32 clusterCount) {
    if (!validatePopulationCounts(grid, clusterCount)) {
        return false;
    }
    if (clusterCount == 0u) {
        return true;
    }
    return ClusterLightGridLayout::validateContiguousOffsets(grid, clusterCount);
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
    const u32 count = desc.clusterCount();
    if (count == 0u) {
        return 0u;
    }
    return count - 1u;
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

bool ClusterLightGridLayout::validateContiguousOffsets(const ClusterGridSoA& grid, u32 clusterCount) {
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

    if (ClusterGridLayout::isEmptyGrid(m_desc)) {
        m_gridSoA.clear();
        m_stats.clustersBuilt = 0;
        return;
    }

    const u32 clusterCount = m_desc.clusterCount();
    if (clusterCount == 0u) {
        m_gridSoA.clear();
        m_stats.clustersBuilt = 0;
        return;
    }

    if (m_gridSoA.aabbs.size() != clusterCount || m_gridSoA.grid.size() != clusterCount) {
        m_gridSoA.allocate(m_desc);
    }

    for (u32 sliceZ = 0; sliceZ < m_desc.slicesZ; ++sliceZ) {
        const f32 nearZ = ClusterSliceLayout::computeSliceNearZ(sliceZ, m_desc, camera);
        const f32 farZ = ClusterSliceLayout::computeSliceFarZ(sliceZ, m_desc, camera);

        for (u32 tileY = 0; tileY < m_desc.tilesY; ++tileY) {
            for (u32 tileX = 0; tileX < m_desc.tilesX; ++tileX) {
                const u32 idx = clusterIndex(tileX, tileY, sliceZ, m_desc);
                ClusterAABB& aabb = m_gridSoA.aabbs[idx];

                const f32 ndcMinX = (static_cast<f32>(tileX) / static_cast<f32>(m_desc.tilesX)) * 2.f - 1.f;
                const f32 ndcMaxX =
                    (static_cast<f32>(tileX + 1u) / static_cast<f32>(m_desc.tilesX)) * 2.f - 1.f;
                const f32 ndcMinY =
                    1.f - (static_cast<f32>(tileY + 1u) / static_cast<f32>(m_desc.tilesY)) * 2.f;
                const f32 ndcMaxY = 1.f - (static_cast<f32>(tileY) / static_cast<f32>(m_desc.tilesY)) * 2.f;

                aabb.minP = {ndcMinX * nearZ + camera.position.x,
                             ndcMinY * nearZ + camera.position.y,
                             -(farZ + camera.position.z)};
                aabb.maxP = {ndcMaxX * farZ + camera.position.x,
                             ndcMaxY * farZ + camera.position.y,
                             -(nearZ + camera.position.z)};
            }
        }
    }

    m_stats.clustersBuilt = clusterCount;
}

bool ClusteredLightCuller::sphereIntersectsAabb(const fuse::math::Vec3& center,
                                                 f32 radius,
                                                 const ClusterAABB& aabb) const {
    const f32 closestX = std::clamp(center.x, aabb.minP.x, aabb.maxP.x);
    const f32 closestY = std::clamp(center.y, aabb.minP.y, aabb.maxP.y);
    const f32 closestZ = std::clamp(center.z, aabb.minP.z, aabb.maxP.z);

    const f32 dx = center.x - closestX;
    const f32 dy = center.y - closestY;
    const f32 dz = center.z - closestZ;
    return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
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

    if (ClusterGridLayout::isEmptyGrid(m_desc)) {
        m_pendingClusterLights.clear();
        rebuildLightGrid();
        m_stats.lightsCulled = 0;
        m_stats.lightListEntries = 0;
        return;
    }

    const u32 clusterCount = m_desc.clusterCount();
    if (clusterCount == 0u) {
        m_pendingClusterLights.clear();
        rebuildLightGrid();
        m_stats.lightsCulled = 0;
        m_stats.lightListEntries = 0;
        return;
    }

    std::vector<std::vector<u32>> perClusterLights(clusterCount);

    u32 lightsProcessed = 0;
    u32 clustersAtCapacity = 0;
    u32 lightsDroppedOverflow = 0;

    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        const ClusterAABB& aabb = m_gridSoA.aabbs[clusterIdx];
        std::vector<u32>& clusterLights = perClusterLights[clusterIdx];
        bool clusterOverflowed = false;

        for (u32 lightIdx = 0; lightIdx < static_cast<u32>(pointLights.size()); ++lightIdx) {
            const PointLightInput& light = pointLights[lightIdx];
            if (!sphereIntersectsAabb(light.position, light.radius, aabb)) {
                continue;
            }
            if (!cluster_util::assignLightToCluster(perClusterLights, clusterIdx, lightIdx,
                                                     m_desc.maxLightsPerCluster)) {
                ++lightsDroppedOverflow;
                clusterOverflowed = true;
            }
        }

        for (u32 lightIdx = 0; lightIdx < static_cast<u32>(spotLights.size()); ++lightIdx) {
            const SpotLightInput& light = spotLights[lightIdx];
            if (!sphereIntersectsAabb(light.position, light.radius, aabb)) {
                continue;
            }
            const u32 encodedIdx = static_cast<u32>(pointLights.size()) + lightIdx;
            if (!cluster_util::assignLightToCluster(perClusterLights, clusterIdx, encodedIdx,
                                                     m_desc.maxLightsPerCluster)) {
                ++lightsDroppedOverflow;
                clusterOverflowed = true;
            }
        }

        if (clusterOverflowed) {
            ++clustersAtCapacity;
        }
        lightsProcessed += static_cast<u32>(clusterLights.size());
    }

    m_pendingClusterLights = std::move(perClusterLights);
    rebuildLightGrid();

    m_stats.lightsCulled = lightsProcessed;
    m_stats.lightListEntries = static_cast<u32>(m_gridSoA.lightList.size());
    m_stats.clustersAtCapacity = clustersAtCapacity;
    m_stats.lightsDroppedOverflow = lightsDroppedOverflow;
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
    if (!m_stats.ready || ClusterGridLayout::isEmptyGrid(m_desc)) {
        return;
    }

    recorder.beginPass("clustered_light_cull");
    cullLights(pointLights, spotLights, camera);
    recorder.endPass();
    ++m_stats.cullPassCount;
}

} // namespace fuse::renderer
