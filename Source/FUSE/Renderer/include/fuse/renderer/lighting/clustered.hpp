#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Cluster grid configuration (B5.4 — P5 §5.4).
struct ClusterDesc {
    static constexpr u32 kMaxTilesX = 32u;
    static constexpr u32 kMaxTilesY = 18u;
    static constexpr u32 kMaxSlicesZ = 64u;
    static constexpr u32 kMaxLightsPerCluster = 256u;

    u32 tilesX = 16;
    u32 tilesY = 9;
    u32 slicesZ = 24;
    u32 maxLightsPerCluster = 256;

    u32 clusterCount() const { return tilesX * tilesY * slicesZ; }
    bool isEmpty() const { return clusterCount() == 0u; }
    /// Last valid flat cluster index; returns 0 when the grid has no clusters.
    u32 maxClusterIndex() const {
        const u32 count = clusterCount();
        return count == 0u ? 0u : count - 1u;
    }

    /// Clamp tile/slice/light caps to CPU stub limits; zero dimensions remain zero (empty grid).
    static ClusterDesc clampCounts(const ClusterDesc& raw);
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

    /// Resize AABB/grid storage for a clamped cluster desc; clears the flat light list.
    void allocate(const ClusterDesc& desc);
    /// Drop all cluster entries and the flat light list.
    void clear();
    bool isEmpty() const { return grid.empty(); }
    /// True when AABB/grid storage matches the clamped cluster count for `desc`.
    bool matchesDesc(const ClusterDesc& desc) const;
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

/// Why screen-depth → cluster mapping was rejected (B5.4 deepen).
enum class ClusterScreenMappingRejectReason : u8 {
    None = 0,
    EmptyGrid,
    InvalidCamera,
    DepthOutOfRange,
};

/// Human-readable label for cluster screen-mapping reject reasons (logging / tests).
const char* clusterScreenMappingRejectReasonLabel(ClusterScreenMappingRejectReason reason);

/// Tile/cluster indexing helpers — mirrors froxel layout (B5.4 CPU path).
struct ClusterGridLayout {
    static bool isEmptyGrid(const ClusterDesc& desc);
    static u32 clusterIndex(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc);
    /// Tile/slice coords clamped to grid bounds before linear index encode.
    static u32 clusterIndexClamped(u32 tileX, u32 tileY, u32 sliceZ, const ClusterDesc& desc);
    static void decodeClusterIndex(u32 index, const ClusterDesc& desc, u32& tileX, u32& tileY, u32& sliceZ);
    static bool isValidClusterIndex(u32 index, const ClusterDesc& desc);
    /// True when `index` exceeds the valid cluster range (would be clamped).
    static bool isClusterIndexOutOfRange(u32 index, const ClusterDesc& desc);
    static u32 clampClusterIndex(u32 index, const ClusterDesc& desc);
    static u32 clampTileX(u32 tileX, const ClusterDesc& desc);
    static u32 clampTileY(u32 tileY, const ClusterDesc& desc);
    static u32 clampSliceZ(u32 sliceZ, const ClusterDesc& desc);
    /// Last valid flat cluster index; returns 0 when the grid has no clusters.
    static u32 maxClusterIndex(const ClusterDesc& desc);
    /// True when `index` equals the last valid cluster index for a non-empty grid.
    static bool isAtMaxClusterIndex(u32 index, const ClusterDesc& desc);
    /// Clamp `index` into range; returns false and zeroes `outIndex` on an empty grid.
    static bool tryClampClusterIndex(u32 index, const ClusterDesc& desc, u32& outIndex);
    static bool mapScreenDepthToClusterIndex(f32 screenX,
                                             f32 screenY,
                                             f32 viewDepth,
                                             const ClusterDesc& desc,
                                             const ClusterCameraDesc& camera,
                                             u32& outClusterIndex);
    /// Screen-depth → cluster index with reject-reason diagnostics.
    static bool tryMapScreenDepthToClusterIndex(f32 screenX,
                                                f32 screenY,
                                                f32 viewDepth,
                                                const ClusterDesc& desc,
                                                const ClusterCameraDesc& camera,
                                                u32& outClusterIndex,
                                                ClusterScreenMappingRejectReason& outReason);
};

/// Why light-grid rebuild preflight rejected the request (B5.4 deepen).
enum class LightGridRebuildRejectReason : u8 {
    None = 0,
    EmptyGrid,
    ClusterCountMismatch,
};

/// Human-readable label for light-grid rebuild reject reasons (logging / tests).
const char* lightGridRebuildRejectReasonLabel(LightGridRebuildRejectReason reason);

/// CPU light-grid packing helpers — mirrors the GPU offset rebuild pass.
struct ClusterLightGridLayout {
    /// True when `clusterCount` is zero or matches the clamped cluster count for `desc`.
    static bool canRebuildLightGrid(const ClusterDesc& desc, u32 clusterCount);
    /// Diagnose why rebuild preflight would reject; vacuously succeeds when `clusterCount` is zero.
    static bool tryCanRebuildLightGrid(const ClusterDesc& desc,
                                       u32 clusterCount,
                                       LightGridRebuildRejectReason& outReason);
    static u32 rebuildLightGrid(ClusterGridSoA& grid,
                                u32 clusterCount,
                                const std::vector<std::vector<u32>>& perClusterLights,
                                u32 maxLightsPerCluster = 0u);
    /// Rebuild using the clamped cluster count from `desc`; early-outs when the grid is empty.
    static u32 rebuildLightGridForDesc(ClusterGridSoA& grid,
                                       const ClusterDesc& desc,
                                       const std::vector<std::vector<u32>>& perClusterLights,
                                       u32 maxLightsPerCluster = 0u);
    static bool validateContiguousOffsets(const ClusterGridSoA& grid, u32 clusterCount);
    /// Validate contiguous offsets against the clamped cluster count derived from `desc`.
    static bool validateContiguousOffsetsForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc);
};

/// Why grid population validation rejected a rebuilt light grid (B5.4 deepen).
enum class GridPopulationRejectReason : u8 {
    None = 0,
    EmptyGrid,
    DescMismatch,
    UndersizedGrid,
    PopulationMismatch,
    NonContiguousOffsets,
};

/// Human-readable label for population reject reasons (logging / tests).
const char* gridPopulationRejectReasonLabel(GridPopulationRejectReason reason);

/// Why a cluster lookup preflight rejected the request (B5.4 deepen).
enum class ClusterLookupRejectReason : u8 {
    None = 0,
    EmptyGrid,
    DescMismatch,
    EmptyStorage,
};

/// Human-readable label for lookup reject reasons (logging / tests).
const char* clusterLookupRejectReasonLabel(ClusterLookupRejectReason reason);

/// CPU light-to-cluster assignment stubs — mirrors CUDA cull kernel list append.
namespace cluster_util {
/// True when light-grid storage matches the clamped cluster count for `desc`.
bool gridMatchesDesc(const ClusterGridSoA& grid, const ClusterDesc& desc);
/// True when `desc` is non-empty, storage is allocated, and sizes match.
bool isGridAccessible(const ClusterGridSoA& grid, const ClusterDesc& desc);
/// True when storage is non-empty, `desc` is non-empty, and sizes match.
bool isLightGridAccessible(const ClusterGridSoA& grid, const ClusterDesc& desc);
/// Early-out when the grid is inaccessible for index-based lookup.
bool shouldSkipClusterLookup(const ClusterGridSoA& grid, const ClusterDesc& desc);
/// Early-out when clustered cull/lookup should be skipped for an empty desc.
bool shouldSkipClusterCull(const ClusterDesc& desc);
/// Preflight guard before index-based cluster lookup; false on empty grid or desc mismatch.
bool canLookupAtIndex(const ClusterGridSoA& grid, const ClusterDesc& desc, u32 index);
/// Preflight guard before coord-based cluster lookup; false on empty grid or desc mismatch.
bool canLookupAtCoord(const ClusterGridSoA& grid, const ClusterDesc& desc, u32 tileX, u32 tileY, u32 sliceZ);
/// Diagnose why lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtIndex(const ClusterGridSoA& grid,
                         const ClusterDesc& desc,
                         u32 index,
                         ClusterLookupRejectReason& outReason);
/// Diagnose why coord lookup preflight would reject; vacuously succeeds on accessible grids.
bool tryCanLookupAtCoord(const ClusterGridSoA& grid,
                         const ClusterDesc& desc,
                         u32 tileX,
                         u32 tileY,
                         u32 sliceZ,
                         ClusterLookupRejectReason& outReason);
/// Per-cluster light count at a clamped flat index; returns 0 when grid/desc mismatch or empty.
u32 clusterLightCountAtIndex(const ClusterGridSoA& grid, const ClusterDesc& desc, u32 index);
bool tryAssignLight(std::vector<u32>& clusterLights, u32 lightIdx, u32 maxLightsPerCluster);
/// Batch-assign candidate lights; returns overflow count dropped at per-cluster capacity.
u32 assignLights(std::vector<u32>& clusterLights,
                 const std::vector<u32>& candidates,
                 u32 maxLightsPerCluster);
/// Assign one light index to a cluster row; returns false when OOB or at per-cluster capacity.
bool assignLightToCluster(std::vector<std::vector<u32>>& perClusterLights,
                          u32 clusterIdx,
                          u32 lightIdx,
                          u32 maxLightsPerCluster);
/// Copy light indices assigned to one cluster from the rebuilt flat grid.
u32 lookupClusterLights(const ClusterGridSoA& grid, u32 clusterIdx, std::vector<u32>& outLights);
/// Lookup at a clamped flat index; returns 0 when grid/desc mismatch or empty.
u32 lookupClusterLightsAtIndex(const ClusterGridSoA& grid,
                               const ClusterDesc& desc,
                               u32 index,
                               std::vector<u32>& outLights);
/// Lookup at clamped tile/slice coords; returns 0 when grid/desc mismatch or empty.
u32 lookupClusterLightsAtCoord(const ClusterGridSoA& grid,
                               const ClusterDesc& desc,
                               u32 tileX,
                               u32 tileY,
                               u32 sliceZ,
                               std::vector<u32>& outLights);
/// Per-cluster light count at clamped tile/slice coords; returns 0 when grid/desc mismatch or empty.
u32 clusterLightCountAtCoord(const ClusterGridSoA& grid,
                             const ClusterDesc& desc,
                             u32 tileX,
                             u32 tileY,
                             u32 sliceZ);
/// Lookup with guard preflight; returns false when `canLookupAtIndex` would reject the request.
bool tryLookupClusterLightsAtIndex(const ClusterGridSoA& grid,
                                   const ClusterDesc& desc,
                                   u32 index,
                                   std::vector<u32>& outLights,
                                   u32& outCount);
/// Lookup with guard preflight and reject-reason diagnostics.
bool tryLookupClusterLightsAtIndex(const ClusterGridSoA& grid,
                                   const ClusterDesc& desc,
                                   u32 index,
                                   std::vector<u32>& outLights,
                                   u32& outCount,
                                   ClusterLookupRejectReason& outReason);
/// Lookup at clamped tile/slice coords with guard preflight.
bool tryLookupClusterLightsAtCoord(const ClusterGridSoA& grid,
                                   const ClusterDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   std::vector<u32>& outLights,
                                   u32& outCount);
/// Lookup at clamped tile/slice coords with guard preflight and reject-reason diagnostics.
bool tryLookupClusterLightsAtCoord(const ClusterGridSoA& grid,
                                   const ClusterDesc& desc,
                                   u32 tileX,
                                   u32 tileY,
                                   u32 sliceZ,
                                   std::vector<u32>& outLights,
                                   u32& outCount,
                                   ClusterLookupRejectReason& outReason);
/// Per-cluster assigned-light count from the rebuilt grid; returns 0 when `clusterIdx` is OOB.
u32 clusterLightCount(const ClusterGridSoA& grid, u32 clusterIdx);
u32 countAssignedLights(const ClusterGridSoA& grid, u32 clusterCount);
/// Count clusters with at least one assigned light; returns 0 when `clusterCount` is zero.
u32 countNonEmptyClusters(const ClusterGridSoA& grid, u32 clusterCount);
u32 countEmptyClusters(const ClusterGridSoA& grid, u32 clusterCount);
/// Count clusters holding `maxLightsPerCluster` lights; returns 0 when `clusterCount` is zero.
u32 countClustersAtCapacity(const ClusterGridSoA& grid, u32 clusterCount, u32 maxLightsPerCluster);
/// True when non-empty + empty cluster counts sum to `clusterCount` and assigned lights match the flat list.
bool validatePopulationCounts(const ClusterGridSoA& grid, u32 clusterCount);
/// Population invariant plus contiguous offset packing when `clusterCount` is non-zero.
bool validateGridPopulation(const ClusterGridSoA& grid, u32 clusterCount);
/// Diagnose the first population invariant that fails; vacuously succeeds when `clusterCount` is zero.
bool tryValidateGridPopulation(const ClusterGridSoA& grid,
                               u32 clusterCount,
                               GridPopulationRejectReason& outReason);
/// Validate population against the clamped cluster count derived from `desc`.
bool validateGridPopulationForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc);
/// Diagnose population validation against `desc`; vacuously succeeds on empty grids.
bool tryValidateGridPopulationForDesc(const ClusterGridSoA& grid,
                                      const ClusterDesc& desc,
                                      GridPopulationRejectReason& outReason);
/// Assigned-light count using the clamped cluster count derived from `desc`; returns 0 on mismatch.
u32 countAssignedLightsForDesc(const ClusterGridSoA& grid, const ClusterDesc& desc);
/// True when at least one cluster holds assigned lights; false when storage is empty.
bool hasAssignedLights(const ClusterGridSoA& grid, u32 clusterCount);
} // namespace cluster_util

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
    static u32 maxClusterIndex(const ClusterDesc& desc) {
        return ClusterGridLayout::maxClusterIndex(desc);
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
