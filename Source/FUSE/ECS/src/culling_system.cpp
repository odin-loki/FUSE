#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/spatial/frustum.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ecs {

namespace {

f32 sdf_bounding_radius(const SDFObject& sdf) {
    switch (sdf.type) {
    case SDFPrimitive::Sphere:
        return sdf.params.x;
    case SDFPrimitive::Box:
        return std::sqrt(sdf.params.x * sdf.params.x + sdf.params.y * sdf.params.y +
                         sdf.params.z * sdf.params.z);
    default:
        return sdf.params.x;
    }
}

spatial::AABB world_aabb_for_mesh(const Transform& transform, const Mesh& mesh) {
    const vec3 corners[2] = {mesh.aabb_min, mesh.aabb_max};
    vec3 world_min{1e30f, 1e30f, 1e30f, 0.f};
    vec3 world_max{-1e30f, -1e30f, -1e30f, 0.f};

    for (const vec3& corner : corners) {
        const vec3 world = transform_point(transform.local_to_world, corner);
        world_min.x = std::min(world_min.x, world.x);
        world_min.y = std::min(world_min.y, world.y);
        world_min.z = std::min(world_min.z, world.z);
        world_max.x = std::max(world_max.x, world.x);
        world_max.y = std::max(world_max.y, world.y);
        world_max.z = std::max(world_max.z, world.z);
    }

    return spatial::AABB{world_min, world_max};
}

} // namespace

Camera::Frustum CullingSystem::extract_frustum(const mat4& view_projection) {
    return spatial::extract_frustum(view_projection);
}

bool CullingSystem::test_aabb_frustum(const Camera::Frustum& frustum, const vec3& aabb_min,
                                      const vec3& aabb_max) {
    return spatial::test_aabb_frustum(frustum, aabb_min, aabb_max);
}

bool CullingSystem::test_sphere_frustum(const Camera::Frustum& frustum, const vec3& center,
                                       f32 radius) {
    return spatial::test_sphere_frustum(frustum, center, radius);
}

CullResult CullingSystem::cull(Registry& reg, const Camera& camera, const spatial::BVH& bvh) {
    CullResult result{};
    const Camera::Frustum& frustum = camera.frustum;

    std::vector<spatial::BVHLeaf> hits;
    bvh.query_frustum(frustum, hits);

    for (const spatial::BVHLeaf& leaf : hits) {
        switch (leaf.type) {
        case spatial::BVHLeafType::Mesh:
            if (reg.has<Mesh>(leaf.entity) && reg.has<Transform>(leaf.entity)) {
                result.visible_meshes.push_back(leaf.entity);
            } else {
                ++result.culled_count;
            }
            break;
        case spatial::BVHLeafType::SDF:
            if (reg.has<SDFObject>(leaf.entity) && reg.has<Transform>(leaf.entity)) {
                result.visible_sdf_objects.push_back(leaf.entity);
            } else {
                ++result.culled_count;
            }
            break;
        case spatial::BVHLeafType::Light:
            result.visible_lights.push_back(leaf.entity);
            break;
        default:
            ++result.culled_count;
            break;
        }
    }

    reg.each<Mesh, Transform>([&](EntityID id, Mesh& mesh, Transform& transform) {
        if (!mesh.visible) {
            ++result.culled_count;
            return;
        }
        const spatial::AABB bounds = world_aabb_for_mesh(transform, mesh);
        if (!test_aabb_frustum(frustum, bounds.min, bounds.max)) {
            ++result.culled_count;
            return;
        }
        if (std::find(result.visible_meshes.begin(), result.visible_meshes.end(), id) ==
            result.visible_meshes.end()) {
            result.visible_meshes.push_back(id);
        }
    });

    reg.each<SDFObject, Transform>([&](EntityID id, SDFObject& sdf, Transform& transform) {
        if (!sdf.visible) {
            ++result.culled_count;
            return;
        }
        const vec3 center = transform_point(transform.local_to_world, {0.f, 0.f, 0.f, 1.f});
        if (!test_sphere_frustum(frustum, center, sdf_bounding_radius(sdf))) {
            ++result.culled_count;
            return;
        }
        if (std::find(result.visible_sdf_objects.begin(), result.visible_sdf_objects.end(), id) ==
            result.visible_sdf_objects.end()) {
            result.visible_sdf_objects.push_back(id);
        }
    });

    return result;
}

} // namespace fuse::ecs
