#pragma once
// E02 mesh-id registry (RE-FI-1 / RE-RUNTIME-3D-RENDER): engine mesh ids (ecs::Mesh::vertex_buffer.index()) and asset
// ids -> WP-1.1 GpuScene mesh rows, and the meshRemap table gpu_scene::GpuSceneEcsExtractor consumes.
//
// Sources: an in-memory geometry::MeshletMesh, `.fusemeshlet` bytes / files (geometry::parse_meshlet_mesh /
// load_meshlet_file), a `.fusemesh` (FMSH v1) through its meshlet sidecar (rebuilt from the FMSH bytes when missing or
// stale; needs the fuse_geometry_cook library, FUSE_SCENE_RENDERER_HAS_COOK), and procedural meshes (cube / plane /
// sphere: procedural_meshes.hpp) so scenes without cooked assets render. FMSH v2 has no meshlet chunks in this tree
// yet (meshlet_format.hpp: "FMSH v2 is meant to carry these chunks"); once it does, its reader hands the parsed
// MeshletMesh to registerMesh().
//
// register*() only queues (CPU); flush(scene) uploads the queued meshes with GpuScene::addMeshletMesh (call it between
// GpuScene::beginFrame and commit, like any scene edit) and fills remap()[engineId] = GpuScene mesh row. Re-registering
// an engine id uploads a new row and repoints the remap (the old row stays allocated: GpuScene has no mesh removal).
// Engine ids without a mesh map to kInvalidIndex (the extractor then writes an instance with no mesh).
// flush() with nothing queued is free; the remap table only grows on register (not per frame).
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::renderer::gpu_scene {
class GpuScene;
}

namespace fuse::renderer::scene_renderer {

struct ProceduralMeshDesc;

struct MeshRegistryStats {
    u32 registered = 0; ///< register*() calls that queued a mesh
    u32 uploaded = 0;   ///< meshes added to the GpuScene
    u32 failed = 0;     ///< parse / build / upload failures
};

class MeshRegistry {
public:
    static constexpr u32 kInvalidMesh = 0xFFFFFFFFu;
    /// First engine id engineIdForAsset() hands out (ids below are for callers that pick their own).
    static constexpr u32 kFirstAssetEngineId = 1u << 16;

    /// Queues `mesh` for engine id `engineId` (< 2^24). The local AABB (object space) defaults to the union of the
    /// meshlet boxes (decoded positions).
    bool registerMesh(u32 engineId, geometry::MeshletMesh mesh);
    bool registerMeshletBytes(u32 engineId, const u8* data, usize size, std::string* error = nullptr);
    bool registerMeshletFile(u32 engineId, const std::string& path, std::string* error = nullptr);
    /// `.fusemesh` (FMSH v1): its `.fusemeshlet` sidecar when current, else built from the FMSH bytes (and the sidecar
    /// refreshed). Without the cook library only a current sidecar is accepted.
    bool registerFuseMesh(u32 engineId, const std::string& fusemeshPath, std::string* error = nullptr);
    bool registerProcedural(u32 engineId, const ProceduralMeshDesc& desc, std::string* error = nullptr);

    /// Engine id of `assetId`, allocated on first use (kFirstAssetEngineId, +1, ...).
    u32 engineIdForAsset(u64 assetId);
    /// kInvalidMesh when the asset has no engine id yet.
    u32 findAsset(u64 assetId) const;

    /// Uploads every queued mesh (GpuScene::addMeshletMesh) and updates the remap. Returns the meshes uploaded.
    u32 flush(gpu_scene::GpuScene& scene);
    /// Meshes queued and not flushed yet.
    u32 pending() const { return static_cast<u32>(m_pending.size()); }

    /// GpuScene mesh row of an engine id (kInvalidMesh: unknown or not flushed yet).
    u32 gpuMesh(u32 engineId) const { return engineId < m_remap.size() ? m_remap[engineId] : kInvalidMesh; }
    /// meshRemap for gpu_scene::EcsExtractDesc / GpuSceneEcsExtractor::setMeshRemap.
    const u32* remap() const { return m_remap.data(); }
    u32 remapCount() const { return static_cast<u32>(m_remap.size()); }
    /// +1 whenever remap() changed (pointer or contents).
    u64 remapVersion() const { return m_remapVersion; }
    /// Object-space AABB of the mesh registered under `engineId` (false: unknown).
    bool localBounds(u32 engineId, f32 outMin[3], f32 outMax[3]) const;

    const MeshRegistryStats& stats() const { return m_stats; }
    void clear();

private:
    struct Pending {
        u32 engineId = 0;
        geometry::MeshletMesh mesh;
    };
    struct Bounds {
        f32 lo[3] = {0.f, 0.f, 0.f};
        f32 hi[3] = {0.f, 0.f, 0.f};
        bool valid = false;
    };

    std::vector<Pending> m_pending;
    std::vector<u32> m_remap;
    std::vector<Bounds> m_bounds;
    std::unordered_map<u64, u32> m_assets;
    u32 m_nextAssetId = kFirstAssetEngineId;
    u64 m_remapVersion = 0;
    MeshRegistryStats m_stats{};
};

} // namespace fuse::renderer::scene_renderer
