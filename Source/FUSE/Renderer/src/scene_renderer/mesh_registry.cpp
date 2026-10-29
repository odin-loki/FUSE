// E02 mesh-id registry: see include/fuse/renderer/scene_renderer/mesh_registry.hpp.
#include <fuse/renderer/scene_renderer/mesh_registry.hpp>

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/scene_renderer/procedural_meshes.hpp>

#if defined(FUSE_SCENE_RENDERER_HAS_COOK)
#include <fuse/renderer/geometry/meshlet_cook_hook.hpp>
#endif

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace fuse::renderer::scene_renderer {

namespace {
constexpr u32 kMaxEngineId = 1u << 24;

void setError(std::string* error, const char* what) {
    if (error != nullptr) {
        *error = what;
    }
}

/// Union of the meshlet AABBs (decoded positions).
/// Unique triangle edges (mesh vertex index pairs), decoded to object-space segments.
void meshEdges(const geometry::MeshletMesh& mesh, std::vector<f32>& out) {
    out.clear();
    std::unordered_set<u64> seen;
    const auto pos = [&](u32 v, u32 a) {
        return mesh.quant.offset[a] + static_cast<f32>(mesh.positions[static_cast<usize>(v) * 4u + a]) * mesh.quant.step[a];
    };
    for (const geometry::MeshletRecord& m : mesh.meshlets) {
        for (u32 t = 0; t < m.triangle_count; ++t) {
            const u32 packed = mesh.meshlet_triangles[m.triangle_offset + t];
            const u32 local[3] = {packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu};
            u32 g[3];
            for (u32 k = 0; k < 3u; ++k) g[k] = mesh.meshlet_vertices[m.vertex_offset + local[k]];
            for (u32 k = 0; k < 3u; ++k) {
                const u32 a = std::min(g[k], g[(k + 1) % 3u]), b = std::max(g[k], g[(k + 1) % 3u]);
                if (a == b || !seen.insert((static_cast<u64>(a) << 32) | b).second) continue;
                if (seen.size() > MeshRegistry::kMaxWireframeEdges) {
                    out.clear();
                    return;
                }
                out.insert(out.end(), {pos(a, 0), pos(a, 1), pos(a, 2), pos(b, 0), pos(b, 1), pos(b, 2)});
            }
        }
    }
}

bool meshBounds(const geometry::MeshletMesh& mesh, f32 lo[3], f32 hi[3]) {
    bool any = false;
    for (const geometry::MeshletRecord& m : mesh.meshlets) {
        for (u32 a = 0; a < 3u; ++a) {
            lo[a] = any ? std::min(lo[a], m.aabb_min[a]) : m.aabb_min[a];
            hi[a] = any ? std::max(hi[a], m.aabb_max[a]) : m.aabb_max[a];
        }
        any = true;
    }
    return any;
}
} // namespace

bool MeshRegistry::registerMesh(u32 engineId, geometry::MeshletMesh mesh) {
    if (engineId >= kMaxEngineId || mesh.meshlets.empty()) {
        ++m_stats.failed;
        return false;
    }
    if (m_bounds.size() <= engineId) {
        m_bounds.resize(static_cast<usize>(engineId) + 1u);
    }
    Bounds& b = m_bounds[engineId];
    b.valid = meshBounds(mesh, b.lo, b.hi);
    if (m_wireframes.size() <= engineId) {
        m_wireframes.resize(static_cast<usize>(engineId) + 1u);
    }
    if (m_retainWireframe) {
        meshEdges(mesh, m_wireframes[engineId]);
    } else {
        m_wireframes[engineId].clear();
    }
    m_pending.push_back(Pending{engineId, std::move(mesh)});
    ++m_stats.registered;
    return true;
}

bool MeshRegistry::registerMeshletBytes(u32 engineId, const u8* data, usize size, std::string* error) {
    geometry::MeshletMesh mesh;
    if (data == nullptr || !geometry::parse_meshlet_mesh(data, size, mesh, error)) {
        if (data == nullptr) {
            setError(error, "no data");
        }
        ++m_stats.failed;
        return false;
    }
    return registerMesh(engineId, std::move(mesh));
}

bool MeshRegistry::registerMeshletFile(u32 engineId, const std::string& path, std::string* error) {
    geometry::MeshletMesh mesh;
    if (!geometry::load_meshlet_file(path, mesh, error)) {
        ++m_stats.failed;
        return false;
    }
    return registerMesh(engineId, std::move(mesh));
}

bool MeshRegistry::registerFuseMesh(u32 engineId, const std::string& fusemeshPath, std::string* error) {
#if defined(FUSE_SCENE_RENDERER_HAS_COOK)
    geometry::MeshletMesh mesh;
    if (geometry::meshlet_sidecar_matches(fusemeshPath, nullptr)) {
        if (geometry::load_meshlet_file(geometry::meshlet_sidecar_path(fusemeshPath), mesh, error)) {
            return registerMesh(engineId, std::move(mesh));
        }
    }
    // Missing or stale sidecar: build from the FMSH bytes (and refresh the sidecar).
    if (!geometry::cook_meshlet_sidecar(fusemeshPath, error, &mesh)) {
        ++m_stats.failed;
        return false;
    }
    return registerMesh(engineId, std::move(mesh));
#else
    // No cook library: only a sidecar next to the .fusemesh (not verified against the FMSH bytes).
    return registerMeshletFile(engineId, geometry::meshlet_sidecar_path(fusemeshPath), error);
#endif
}

bool MeshRegistry::registerProcedural(u32 engineId, const ProceduralMeshDesc& desc, std::string* error) {
    geometry::MeshletMesh mesh;
    f32 lo[3];
    f32 hi[3];
    if (!buildProceduralMesh(desc, mesh, lo, hi, error)) {
        ++m_stats.failed;
        return false;
    }
    if (!registerMesh(engineId, std::move(mesh))) {
        return false;
    }
    Bounds& b = m_bounds[engineId]; // the analytic box (tighter than the quantised meshlet boxes)
    for (u32 a = 0; a < 3u; ++a) {
        b.lo[a] = lo[a];
        b.hi[a] = hi[a];
    }
    b.valid = true;
    return true;
}

u32 MeshRegistry::engineIdForAsset(u64 assetId) {
    const auto it = m_assets.find(assetId);
    if (it != m_assets.end()) {
        return it->second;
    }
    if (m_nextAssetId >= kMaxEngineId) {
        return kInvalidMesh;
    }
    const u32 id = m_nextAssetId++;
    m_assets.emplace(assetId, id);
    return id;
}

u32 MeshRegistry::findAsset(u64 assetId) const {
    const auto it = m_assets.find(assetId);
    return it != m_assets.end() ? it->second : kInvalidMesh;
}

u32 MeshRegistry::flush(gpu_scene::GpuScene& scene) {
    if (m_pending.empty()) {
        return 0u;
    }
    u32 uploaded = 0;
    for (Pending& p : m_pending) {
        const u32 row = scene.addMeshletMesh(p.mesh);
        if (row == gpu_scene::kInvalidIndex) {
            ++m_stats.failed;
            continue;
        }
        if (m_remap.size() <= p.engineId) {
            m_remap.resize(static_cast<usize>(p.engineId) + 1u, kInvalidMesh);
        }
        m_remap[p.engineId] = row;
        ++uploaded;
    }
    m_pending.clear();
    m_stats.uploaded += uploaded;
    ++m_remapVersion;
    return uploaded;
}

bool MeshRegistry::localBounds(u32 engineId, f32 outMin[3], f32 outMax[3]) const {
    if (engineId >= m_bounds.size() || !m_bounds[engineId].valid) {
        return false;
    }
    for (u32 a = 0; a < 3u; ++a) {
        outMin[a] = m_bounds[engineId].lo[a];
        outMax[a] = m_bounds[engineId].hi[a];
    }
    return true;
}

void MeshRegistry::clear() {
    m_pending.clear();
    m_remap.clear();
    m_bounds.clear();
    m_wireframes.clear();
    m_assets.clear();
    m_nextAssetId = kFirstAssetEngineId;
    ++m_remapVersion;
    m_stats = MeshRegistryStats{};
}

} // namespace fuse::renderer::scene_renderer
