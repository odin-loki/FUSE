#pragma once

// WP-6.0 CPU reference for the acceleration structures: the same two-level scene traced with the
// ECS/Spatial CPU BVH (fuse::spatial::BVH, binned SAH, ray_cast_exact; the B3.9 BVH gates' structure).
//
//   bottom level  one spatial::BVH per mesh over its triangles (object space; leaf = triangle t of
//                 the scene index range, i.e. the BLAS primitive index)
//   top level     one spatial::BVH over the world AABBs of the instance slots whose TLAS mask would be
//                 non-zero (rt_types.hpp rtInstanceMask with "mesh has geometry" standing for the BLAS)
//   trace         ray_cast_exact over the top level; each candidate instance transforms the ray into
//                 object space with the inverse 3x4 (t is preserved: the direction is not normalised)
//                 and runs ray_cast_exact over its mesh BVH with a double-precision Moller-Trumbore
//
// Refit mirrors the GPU UPDATE builds: updateInstances() refits the top level in place
// (BVH::update_leaf_aabb + refit) when the slot set is unchanged; updateMeshPositions() refits a mesh
// BVH after deformation. Topology changes rebuild.
//
// Robustness (the GPU is a different watertight float implementation): traceClassified() also traces
// with every triangle grown and shrunk by `edgeEpsilon` in barycentric space. A ray is ROBUST when all
// three agree on the (instance, primitive) pair (or all miss): a robust hit / miss is not within
// `edgeEpsilon` of any triangle edge that could change the answer. The WP-6.0 gates require GPU ==
// CPU on every robust ray and |t_gpu - t_cpu| <= tTolerance(t) on every ray whose ids match.

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/rt/rt_types.hpp>
#include <fuse/spatial/bvh.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::rt {

struct RtRefHit {
    bool hit = false;
    u32 instance = gpu_scene::kInvalidIndex;
    u32 primitive = gpu_scene::kInvalidIndex;
    f64 t = -1.0;
    f64 u = 0.0;
    f64 v = 0.0;
};

struct RtRefClassified {
    RtRefHit hit;       ///< exact triangles
    bool robust = false;
};

/// Documented t tolerance of the parity gates: 1e-4 relative, 1e-5 absolute (float build + watertight
/// test on the device vs a double-precision reference; the scenes span ~1..100 units).
inline f64 rtTTolerance(f64 t) {
    const f64 a = t < 0.0 ? -t : t;
    return 1.0e-5 + 1.0e-4 * a;
}

class RtReferenceScene {
public:
    /// Object-space triangle list (mesh-local vertex indices). Builds the mesh BVH.
    void setMesh(u32 mesh, const f32* positions, u32 vertexCount, const u32* indices, u32 indexCount);
    /// Decodes a WP-1.2 mesh exactly like rt_decode (rtDecodePosition) and takes its scene index range.
    bool setMeshFromScene(const gpu_scene::GpuScene& scene, u32 mesh, const u16* vpos);
    /// Same topology, new positions: refits the mesh BVH in place.
    bool updateMeshPositions(u32 mesh, const f32* positions, u32 vertexCount);
    bool hasMesh(u32 mesh) const { return mesh < m_meshes.size() && m_meshes[mesh].present; }
    const std::vector<f32>& meshPositions(u32 mesh) const { return m_meshes[mesh].positions; }

    /// Top level from the scene's instance slots (full build).
    void setInstances(const gpu_scene::GpuScene& scene);
    void setInstances(const GpuInstance* instances, const GpuTransform* transforms, u32 count);
    /// Refits the top level when the traced slot set and meshes are unchanged, else rebuilds.
    /// Returns true when it refit.
    bool updateInstances(const gpu_scene::GpuScene& scene);
    bool updateInstances(const GpuInstance* instances, const GpuTransform* transforms, u32 count);

    /// Closest hit in [ray.tMin, ray.tMax] among instances with (mask & cullMask) != 0. `edgeEpsilon`
    /// grows (> 0) or shrinks (< 0) every triangle in barycentric space.
    RtRefHit trace(const RtProbeRay& ray, u32 cullMask = kRtMaskAll, f64 edgeEpsilon = 0.0) const;
    RtRefClassified traceClassified(const RtProbeRay& ray, u32 cullMask, f64 edgeEpsilon) const;

    u32 tracedInstances() const { return static_cast<u32>(m_leafSlots.size()); }
    u32 topLevelRefits() const { return m_topRefits; }

private:
    struct Mesh {
        bool present = false;
        std::vector<f32> positions; ///< xyz per vertex
        std::vector<u32> indices;
        spatial::BVH bvh;
        f32 aabbMin[3] = {0.f, 0.f, 0.f};
        f32 aabbMax[3] = {0.f, 0.f, 0.f};
    };
    struct Instance {
        u32 slot = 0;
        u32 mesh = 0;
        u32 mask = 0;
        f64 m[3][4] = {};   ///< object -> world
        f64 inv[3][4] = {}; ///< world -> object
    };

    void buildMeshBvh(Mesh& mesh, bool refit);
    spatial::AABB triangleBounds(const Mesh& mesh, u32 triangle) const;
    bool makeInstance(const GpuInstance& instance, const GpuTransform& transform, u32 slot, Instance& out,
                      spatial::AABB& bounds) const;
    void buildTop(const GpuInstance* instances, const GpuTransform* transforms, u32 count);

    std::vector<Mesh> m_meshes;
    std::vector<Instance> m_instances; ///< indexed by top-level build input
    std::vector<u32> m_leafSlots;      ///< slots of the traced instances, build order
    spatial::BVH m_top;
    u32 m_topRefits = 0;
};

} // namespace fuse::renderer::rt
