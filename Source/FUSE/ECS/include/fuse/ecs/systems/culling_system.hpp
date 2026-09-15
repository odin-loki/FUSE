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

class CullingSystem {
public:
    static CullResult cull(Registry& reg, const Camera& camera, const spatial::BVH& bvh);

    static bool test_aabb_frustum(const Camera::Frustum& frustum, const vec3& aabb_min,
                                  const vec3& aabb_max);
    static bool test_sphere_frustum(const Camera::Frustum& frustum, const vec3& center, f32 radius);

    static Camera::Frustum extract_frustum(const mat4& view_projection);
};

} // namespace fuse::ecs
