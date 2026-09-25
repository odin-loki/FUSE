#pragma once

// WP-7.1 light tree (renderer plan Phase 7 "A light BVH or light tree for importance sampling"; execution
// doc WP-7.1). A binary light BVH with orientation bounds (Conty Estevez and Kulla, "Importance Sampling of
// Many Lights on the GPU", HPG 2018; the PBRT-v4 BVHLightSampler formulation) over point, spot, rectangle,
// disk and emissive-triangle lights; directional lights are sampled separately (uniformly, with probability
// dirCount / (dirCount + 1)).
//
//   std::vector<LightTreeLight> lights;             // a standalone list (makePointLight, makeTriangleLight,
//   appendSceneLights(scene, lights);               //  ...) and / or the WP-1.1 GpuScene adapter
//   appendSceneEmissiveTriangles(scene, meshes, meshCount, lights);
//   LightTree tree;
//   tree.build(lights.data(), count);               // SAH-like orientation cost (PBRT-v4 EvaluateCost)
//   ... lights move / change power, same set:  tree.refit(lights.data(), count);   // topology kept
//   ... lights added / removed:                tree.build(...)                     // full rebuild
//   LightTreeSample s = tree.sample(p, n, u0, u1, u2);   // light, selection pmf, point on the light, 1 / area
//   f32 pmf = tree.pmf(p, n, light);                     // == s.pmf bit for bit for light == s.light
//
// The CPU sampler is the single-source kernel (light_tree_kernel.hpp); the GPU twins
// (shaders/light_tree/lt_common.{glsl,slang}) return the same bits (LightTreeGpu, light_tree_gpu.hpp).
//
// Flattened layout (light_tree_types.hpp): depth-first LightTreeNode[2n - 1] (first child = next node,
// second child index stored, one emitter per leaf), LightTreeEmitter[] in input-list order (the light index
// the sampler returns is the input index; directional lights keep their index too), u32[] directional
// emitter indices.
//
// Allocations: build() / refit() / the samplers make no heap allocation once the tree has held a list at
// least as large (every table and the build scratch keep their capacity; reserve() pre-sizes them).

#include <fuse/renderer/light_tree/light_tree_kernel.hpp>
#include <fuse/renderer/light_tree/light_tree_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::gpu_scene {
struct GpuLight;
struct GpuTransform;
class GpuScene;
} // namespace fuse::renderer::gpu_scene

namespace fuse::renderer::geometry {
struct MeshletMesh;
}

namespace fuse::renderer::light_tree {

/// One input light, world space.
struct LightTreeLight {
    u32 kind = kLtKindPoint; ///< LtKind
    /// point / spot: position; rect / disk: centre; triangle: vertex 0
    f32 position[3] = {0.f, 0.f, 0.f};
    /// rect / disk: first half axis (width direction x half width / radius); triangle: vertex 1
    f32 u[3] = {0.f, 0.f, 0.f};
    /// rect / disk: second half axis (u x v = the lit-side normal); triangle: vertex 2 (counter-clockwise
    /// seen from the lit side)
    f32 v[3] = {0.f, 0.f, 0.f};
    /// spot: axis; directional: the direction the light travels (need not be unit)
    f32 direction[3] = {0.f, 0.f, -1.f};
    f32 cosInner = 1.f; ///< spot: cos of the full-intensity half angle
    f32 cosOuter = 1.f; ///< spot: cos of the cut-off half angle
    /// point / spot: radiant intensity (W/sr; a scalar such as max(colour x intensity)); area kinds: emitted
    /// radiance L (W/sr/m^2); directional: irradiance (not used for selection)
    f32 intensity = 1.f;
    bool twoSided = false; ///< area kinds: emits on both sides
    u32 source = kLtInvalid; ///< caller id, copied to LightTreeEmitter::source
};

LightTreeLight makePointLight(const f32 (&position)[3], f32 intensity, u32 source = kLtInvalid);
LightTreeLight makeSpotLight(const f32 (&position)[3], const f32 (&axis)[3], f32 cosInner, f32 cosOuter, f32 intensity,
                             u32 source = kLtInvalid);
LightTreeLight makeRectLight(const f32 (&center)[3], const f32 (&halfU)[3], const f32 (&halfV)[3], f32 radiance,
                             bool twoSided = false, u32 source = kLtInvalid);
LightTreeLight makeDiskLight(const f32 (&center)[3], const f32 (&radiusU)[3], const f32 (&radiusV)[3], f32 radiance,
                             bool twoSided = false, u32 source = kLtInvalid);
LightTreeLight makeTriangleLight(const f32 (&v0)[3], const f32 (&v1)[3], const f32 (&v2)[3], f32 radiance,
                                 bool twoSided = false, u32 source = kLtInvalid);
LightTreeLight makeDirectionalLight(const f32 (&direction)[3], f32 irradiance, u32 source = kLtInvalid);

/// Emitter record (geometry, area, power, unit normal) of one light; leafNode / bitTrail / depth unset.
LightTreeEmitter makeEmitter(const LightTreeLight& light);

struct LightTreeBuildOptions {
    u32 buckets = 12; ///< SAH buckets per axis (2..32)
};

struct LightTreeStats {
    u32 nodes = 0;
    u32 treeLights = 0;
    u32 directional = 0;
    u32 maxDepth = 0;
    u32 medianSplits = 0; ///< nodes split by count (no useful SAH split, or the depth cap)
    u32 builds = 0;
    u32 refits = 0;
};

class LightTree {
public:
    LightTree() = default;

    /// Rebuilds the tree from `count` lights. False when count >= 2^31 (the tables are then empty).
    bool build(const LightTreeLight* lights, u32 count, const LightTreeBuildOptions& options = {});
    bool build(const std::vector<LightTreeLight>& lights, const LightTreeBuildOptions& options = {}) {
        return build(lights.data(), static_cast<u32>(lights.size()), options);
    }
    /// Recomputes every emitter record and node bound bottom-up for the same light set, keeping the topology
    /// (bit trails stay valid, the result equals build()'s bounds for that topology). False (tree unchanged)
    /// when the count differs or a light moved between the directional table and the tree: call build().
    bool refit(const LightTreeLight* lights, u32 count);
    bool refit(const std::vector<LightTreeLight>& lights) {
        return refit(lights.data(), static_cast<u32>(lights.size()));
    }
    void clear();
    /// Pre-sizes every table and the build scratch for `lights` lights.
    void reserve(u32 lights);

    LightTreeView view() const;
    /// Process-wide unique content id, new after every successful build() / refit() / clear().
    u64 version() const { return m_version; }
    const std::vector<LightTreeNode>& nodes() const { return m_nodes; }
    const std::vector<LightTreeEmitter>& emitters() const { return m_emitters; }
    const std::vector<u32>& directional() const { return m_directional; }
    const LightTreeStats& stats() const { return m_stats; }

    // --- CPU sampler (light_tree_kernel.hpp) -------------------------------------------------------
    LightTreeSample sample(const f32 (&p)[3], const f32 (&n)[3], f32 u0, f32 u1, f32 u2) const;
    f32 pmf(const f32 (&p)[3], const f32 (&n)[3], u32 light) const;
    /// Importance of one node at (p, n) (lt_importance).
    f32 importance(u32 node, const f32 (&p)[3], const f32 (&n)[3]) const;

private:
    /// Double-precision node bounds (the unions run in double; nodes store them rounded outward).
    struct Bounds {
        f64 lo[3] = {0.0, 0.0, 0.0};
        f64 hi[3] = {0.0, 0.0, 0.0};
        f64 phi = 0.0;
        f64 axis[3] = {0.0, 0.0, 1.0};
        f64 cosO = 1.0;
        f64 cosE = 1.0;
        bool twoSided = false;
        bool empty = true;
    };
    static constexpr u32 kMaxBuckets = 32u;

    static Bounds leafBounds(const LightTreeLight& light, const LightTreeEmitter& emitter);
    static void unite(Bounds& a, const Bounds& b);
    static f64 cost(const Bounds& b, const Bounds& parent, u32 dim);
    static void store(const Bounds& b, bool leaf, LightTreeNode& node);
    u32 buildRange(u32 begin, u32 end, u32 depth, u32 trail);
    u32 chooseSplit(u32 begin, u32 end, u32 depth, const Bounds& centroids);
    void fitInterior();
    void bump();

    std::vector<LightTreeNode> m_nodes;
    std::vector<LightTreeEmitter> m_emitters;
    std::vector<u32> m_directional;
    // Build scratch (capacity kept).
    std::vector<Bounds> m_leaf;      ///< per emitter
    std::vector<Bounds> m_nodeBounds;///< per node
    std::vector<f32> m_centroid;     ///< 3 per emitter
    std::vector<u32> m_order;        ///< tree emitters being partitioned
    u32 m_nodeCount = 0;
    u32 m_buckets = 12;
    LightTreeStats m_stats{};
    u64 m_version = 0;
};

// --- WP-1.1 GpuScene adapter -------------------------------------------------------------------------

/// GpuLight -> LightTreeLight: point / spot / directional (gpu_scene::GpuLightType) and the WP-2.2 rectangle /
/// disk lights (ltc::kLightRect / kLightDisk, tangent decoded from flags). Intensity: max colour channel x
/// intensity. False for a free slot or an unknown type.
bool lightFromGpuLight(const gpu_scene::GpuLight& light, u32 source, LightTreeLight& out);
/// Appends every live light of the scene (source = light slot). Returns the number appended.
u32 appendSceneLights(const gpu_scene::GpuScene& scene, std::vector<LightTreeLight>& out);

/// Identifies an emissive triangle appended by the adapters (LightTreeEmitter::source indexes these).
struct EmissiveTriangleRef {
    u32 instance = kLtInvalid; ///< GpuScene instance slot (kLtInvalid for appendEmissiveTriangles)
    u32 triangle = 0;          ///< mesh triangle (MTRI order)
};

/// Appends the triangles of a WP-1.2 meshlet mesh (decoded positions, placed by `transform`) of submesh
/// `submesh` (kLtInvalid: every submesh) as triangle lights of radiance `radiance`. source =
/// refs->size() before the append when `refs` is given (one ref per triangle), else kLtInvalid.
u32 appendEmissiveTriangles(const geometry::MeshletMesh& mesh, const gpu_scene::GpuTransform& transform, f32 radiance,
                            bool twoSided, std::vector<LightTreeLight>& out, std::vector<EmissiveTriangleRef>* refs = nullptr,
                            u32 instance = kLtInvalid, u32 submesh = kLtInvalid);
/// Appends the emissive triangles of every live, visible scene instance: `meshes[i]` is the CPU meshlet mesh
/// of GpuScene mesh i (null: skipped; the GPU scene keeps no CPU positions), the radiance of a submesh is the
/// max channel of MaterialEval::emissiveRadiance of its material row (instance.material + submesh
/// material_index). Returns the number of triangles appended.
u32 appendSceneEmissiveTriangles(const gpu_scene::GpuScene& scene, const geometry::MeshletMesh* const* meshes,
                                 u32 meshCount, std::vector<LightTreeLight>& out,
                                 std::vector<EmissiveTriangleRef>* refs = nullptr, bool twoSided = false);

} // namespace fuse::renderer::light_tree
