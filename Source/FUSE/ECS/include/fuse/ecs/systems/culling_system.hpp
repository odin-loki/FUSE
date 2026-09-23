#pragma once

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/spatial/bvh.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ecs {

struct CullResult {
    std::vector<EntityID> visible_meshes;
    std::vector<EntityID> visible_sdf_objects;
    std::vector<EntityID> visible_lights;
    u32 culled_count = 0;
};

struct Mesh;
struct SDFObject;
struct Transform;

struct CullOptions {
    /// True when `bvh` holds a leaf for every Mesh/SDFObject entity with current world bounds
    /// (SceneManager keeps it in sync). Culling then uses only the BVH; otherwise every entity is
    /// also tested directly so stale or partial BVHs cannot drop visible objects.
    bool bvh_covers_scene = false;
};

/// Reusable per-caller working memory for `CullingSystem::cull`: keeping one alive across frames
/// (SceneManager does) makes steady-state culling heap-free once the buffers have grown.
struct CullScratch {
    std::vector<spatial::BVHLeaf> hits;
    std::vector<u8> seen_mesh;
    std::vector<u8> seen_sdf;
};

class CullingSystem {
public:
    static CullResult cull(Registry& reg, const Camera& camera, const spatial::BVH& bvh,
                           const CullOptions& options = {});
    /// Same as above, writing into `out` (cleared first, capacity kept) and reusing `scratch`.
    static void cull(Registry& reg, const Camera& camera, const spatial::BVH& bvh, const CullOptions& options,
                     CullResult& out, CullScratch& scratch);

    /// World-space bounds used for culling: all 8 local AABB corners transformed.
    static spatial::AABB world_bounds(const Transform& transform, const Mesh& mesh);
    /// World-space bounds of the SDF's bounding sphere.
    static spatial::AABB world_bounds(const Transform& transform, const SDFObject& sdf);
    /// World-space bounding-sphere radius of an SDF: local radius x largest stretch of
    /// `local_to_world` (scaled SDFs were previously bounded by their unscaled radius).
    static f32 sdf_world_bounding_radius(const Transform& transform, const SDFObject& sdf);

    static bool test_aabb_frustum(const Camera::Frustum& frustum, const vec3& aabb_min,
                                  const vec3& aabb_max);
    static bool test_sphere_frustum(const Camera::Frustum& frustum, const vec3& center, f32 radius);

    static Camera::Frustum extract_frustum(const mat4& view_projection);
};

} // namespace fuse::ecs
