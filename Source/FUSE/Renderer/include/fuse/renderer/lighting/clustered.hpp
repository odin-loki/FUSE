#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Cluster grid configuration (B5.4 — P5 §5.4).
struct ClusterDesc {
    u32 tilesX = 16;
    u32 tilesY = 9;
    u32 slicesZ = 24;
    u32 maxLightsPerCluster = 256;

    u32 clusterCount() const { return tilesX * tilesY * slicesZ; }
};

/// World-space cluster bounds built from the camera frustum.
struct ClusterAABB {
    fuse::math::Vec3 minP{};
    fuse::math::Vec3 maxP{};
};

/// Per-cluster light list entry — offset/count into the flat light index list.
struct ClusterGridEntry {
    u32 offset = 0;
    u32 count = 0;
};

/// CPU-side cluster grid SoA — mirrors GPU buffers for stub culling and tests.
struct ClusterGridSoA {
    std::vector<ClusterAABB> aabbs;
    std::vector<ClusterGridEntry> grid;
    std::vector<u32> lightList;
};

/// GPU buffer handles for cluster build + light cull kernels (CUDA deferred).
struct ClusterBuffers {
    BufferHandle clusterAabbs{};
    BufferHandle lightGrid{};
    BufferHandle lightList{};
    BufferHandle lightSsbo{};
    u32 clusterCount = 0;
};

/// Minimal camera inputs for cluster AABB construction.
struct ClusterCameraDesc {
    fuse::math::Vec3 position{};
    f32 nearPlane = 0.1f;
    f32 farPlane = 1000.f;
    u32 screenWidth = 1920;
    u32 screenHeight = 1080;
};

/// Exponential depth-slice bounds — shared by cluster build and CPU tests.
struct ClusterSliceLayout {
    static f32 computeSliceNearZ(u32 sliceZ, const ClusterDesc& desc, const ClusterCameraDesc& camera);
    static f32 computeSliceFarZ(u32 sliceZ, const ClusterDesc& desc, const ClusterCameraDesc& camera);
    static u32 computeSliceZFromDepth(f32 viewDepth, const ClusterDesc& desc, const ClusterCameraDesc& camera);
};

/// Tile/cluster indexing helpers — mirrors froxel layout (B5.4 CPU path).
struct ClusterGridLayout {
    static u32 clusterIndex(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc);
    static void decodeClusterIndex(u32 index, const ClusterDesc& desc, u32& tileX, u32& tileY, u32& sliceZ);
    static u32 clampClusterIndex(u32 index, const ClusterDesc& desc);
    static u32 clampTileX(u32 tileX, const ClusterDesc& desc);
    static u32 clampTileY(u32 tileY, const ClusterDesc& desc);
    static u32 clampSliceZ(u32 sliceZ, const ClusterDesc& desc);
    static bool mapScreenDepthToClusterIndex(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const ClusterDesc& desc,
                                             const ClusterCameraDesc& camera,
                                             u32& outClusterIndex);
};

/// CPU light-grid packing helpers — mirrors the GPU offset rebuild pass.
struct ClusterLightGridLayout {
    static u32 rebuildLightGrid(ClusterGridSoA& grid,
                                u32 clusterCount,
                                const std::vector<std::vector<u32>>& perClusterLights,
                                u32 maxLightsPerCluster = 0u);
    static bool validateContiguousOffsets(const ClusterGridSoA& grid, u32 clusterCount);
};

/// Renderer-side point light input (decoupled from ECS).
struct PointLightInput {
    fuse::math::Vec3 position{};
    fuse::math::Vec3 color{1.f, 1.f, 1.f};
    f32 intensity = 1.f;
    f32 radius = 10.f;
};

/// Renderer-side spot light input (decoupled from ECS).
struct SpotLightInput {
    fuse::math::Vec3 position{};
    fuse::math::Vec3 direction{0.f, -1.f, 0.f};
    fuse::math::Vec3 color{1.f, 1.f, 1.f};
    f32 intensity = 1.f;
    f32 innerConeDeg = 15.f;
    f32 outerConeDeg = 30.f;
    f32 radius = 20.f;
};

/// GPU-packed point light row for the light SSBO.
struct GPUPointLight {
    fuse::math::Vec3 position{};
    f32 radius = 0.f;
    fuse::math::Vec3 color{};
    f32 intensity = 0.f;
};

struct ClusteredLightCullerStats {
    bool ready = false;
    u32 clustersBuilt = 0;
    u32 lightsCulled = 0;
    u32 lightListEntries = 0;
    u32 cullPassCount = 0;
    u32 clustersAtCapacity = 0;
    u32 lightsDroppedOverflow = 0;
};

/// CPU stub for clustered light assignment — CUDA kernels deferred to B5.4 follow-up.
class ClusteredLightCuller {
public:
    void init(const ClusterDesc& desc, ResourceManager& resources);
    void destroy();

    bool isReady() const { return m_stats.ready; }
    const ClusteredLightCullerStats& stats() const { return m_stats; }

    void updateClusters(const ClusterCameraDesc& camera);
    void cullLights(const std::vector<PointLightInput>& pointLights,
                    const std::vector<SpotLightInput>& spotLights,
                    const ClusterCameraDesc& camera);

    /// Packs per-cluster light indices into the flat light list + grid offsets (CPU rebuild stub).
    void rebuildLightGrid();

    /// Records logical cull pass work for the render graph execute stub.
    void recordCullPass(class CommandBufferRecorder& recorder,
                        const ClusterCameraDesc& camera,
                        const std::vector<PointLightInput>& pointLights,
                        const std::vector<SpotLightInput>& spotLights);

    const ClusterBuffers& buffers() const { return m_buffers; }
    const ClusterGridSoA& gridSoA() const { return m_gridSoA; }
    const ClusterDesc& desc() const { return m_desc; }

    static u32 clusterIndex(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc) {
        return ClusterGridLayout::clusterIndex(tileX, tileY, sliceZ, desc);
    }

private:
    bool sphereIntersectsAabb(const fuse::math::Vec3& center, f32 radius, const ClusterAABB& aabb) const;

    ClusterDesc m_desc{};
    ClusterBuffers m_buffers{};
    ClusterGridSoA m_gridSoA{};
    ClusteredLightCullerStats m_stats{};
    std::vector<std::vector<u32>> m_pendingClusterLights{};
    ResourceManager* m_resources = nullptr;
};

} // namespace fuse::renderer
