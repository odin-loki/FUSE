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

} // namespace

u32 ClusteredLightCuller::clusterIndex(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc) {
    return (tileY * desc.tilesX + tileX) * desc.slicesZ + sliceZ;
}

void ClusteredLightCuller::init(const ClusterDesc& desc, ResourceManager& resources) {
    destroy();
    m_desc = desc;
    m_resources = &resources;

    const u32 clusterCount = m_desc.clusterCount();
    m_buffers.clusterCount = clusterCount;

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

    m_gridSoA.aabbs.resize(clusterCount);
    m_gridSoA.grid.resize(clusterCount);
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
    m_gridSoA.aabbs.resize(clusterCount);
    m_gridSoA.grid.resize(clusterCount);

    const f32 depthRatio = camera.farPlane / camera.nearPlane;

    for (u32 sliceZ = 0; sliceZ < m_desc.slicesZ; ++sliceZ) {
        const f32 sliceT0 = static_cast<f32>(sliceZ) / static_cast<f32>(m_desc.slicesZ);
        const f32 sliceT1 = static_cast<f32>(sliceZ + 1u) / static_cast<f32>(m_desc.slicesZ);
        const f32 nearZ = camera.nearPlane * std::pow(depthRatio, sliceT0);
        const f32 farZ = camera.nearPlane * std::pow(depthRatio, sliceT1);

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

    const u32 clusterCount = m_desc.clusterCount();
    m_gridSoA.lightList.clear();
    m_gridSoA.grid.assign(clusterCount, ClusterGridEntry{});

    u32 lightsProcessed = 0;

    for (u32 clusterIdx = 0; clusterIdx < clusterCount; ++clusterIdx) {
        const ClusterAABB& aabb = m_gridSoA.aabbs[clusterIdx];
        ClusterGridEntry& entry = m_gridSoA.grid[clusterIdx];
        entry.offset = static_cast<u32>(m_gridSoA.lightList.size());

        for (u32 lightIdx = 0; lightIdx < static_cast<u32>(pointLights.size()); ++lightIdx) {
            const PointLightInput& light = pointLights[lightIdx];
            if (entry.count >= m_desc.maxLightsPerCluster) {
                break;
            }
            if (sphereIntersectsAabb(light.position, light.radius, aabb)) {
                m_gridSoA.lightList.push_back(lightIdx);
                ++entry.count;
            }
        }

        for (u32 lightIdx = 0; lightIdx < static_cast<u32>(spotLights.size()); ++lightIdx) {
            const SpotLightInput& light = spotLights[lightIdx];
            if (entry.count >= m_desc.maxLightsPerCluster) {
                break;
            }
            const u32 encodedIdx = static_cast<u32>(pointLights.size()) + lightIdx;
            if (sphereIntersectsAabb(light.position, light.radius, aabb)) {
                m_gridSoA.lightList.push_back(encodedIdx);
                ++entry.count;
            }
        }

        lightsProcessed += entry.count;
    }

    m_stats.lightsCulled = lightsProcessed;
    m_stats.lightListEntries = static_cast<u32>(m_gridSoA.lightList.size());
}

void ClusteredLightCuller::recordCullPass(CommandBufferRecorder& recorder,
                                          const ClusterCameraDesc& camera,
                                          const std::vector<PointLightInput>& pointLights,
                                          const std::vector<SpotLightInput>& spotLights) {
    if (!m_stats.ready) {
        return;
    }

    recorder.beginPass("clustered_light_cull");
    cullLights(pointLights, spotLights, camera);
    recorder.endPass();
    ++m_stats.cullPassCount;
}

} // namespace fuse::renderer
