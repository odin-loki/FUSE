#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/sdf_csg.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/spatial/frustum.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ecs {

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

spatial::AABB CullingSystem::world_bounds(const Transform& transform, const Mesh& mesh) {
    vec3 world_min{1e30f, 1e30f, 1e30f, 0.f};
    vec3 world_max{-1e30f, -1e30f, -1e30f, 0.f};
    for (u32 corner = 0; corner < 8u; ++corner) {
        const vec3 local{(corner & 1u) ? mesh.aabb_max.x : mesh.aabb_min.x,
                         (corner & 2u) ? mesh.aabb_max.y : mesh.aabb_min.y,
                         (corner & 4u) ? mesh.aabb_max.z : mesh.aabb_min.z, 1.f};
        const vec3 world = transform_point(transform.local_to_world, local);
        world_min.x = std::min(world_min.x, world.x);
        world_min.y = std::min(world_min.y, world.y);
        world_min.z = std::min(world_min.z, world.z);
        world_max.x = std::max(world_max.x, world.x);
        world_max.y = std::max(world_max.y, world.y);
        world_max.z = std::max(world_max.z, world.z);
    }
    return spatial::AABB{world_min, world_max};
}

spatial::AABB CullingSystem::world_bounds(const Transform& transform, const SDFObject& sdf) {
    const vec3 center = transform_point(transform.local_to_world, {0.f, 0.f, 0.f, 1.f});
    const f32 r = sdf_bounding_radius(sdf);
    return spatial::AABB{{center.x - r, center.y - r, center.z - r, 0.f}, {center.x + r, center.y + r, center.z + r, 0.f}};
}

CullResult CullingSystem::cull(Registry& reg, const Camera& camera, const spatial::BVH& bvh,
                               const CullOptions& options) {
    CullResult result{};
    const Camera::Frustum& frustum = camera.frustum;

    // Exact per-object tests shared by the BVH and fallback paths so both agree.
    auto meshVisible = [&](const Mesh& mesh, const Transform& transform) {
        if (!mesh.visible) {
            return false;
        }
        const spatial::AABB bounds = world_bounds(transform, mesh);
        return test_aabb_frustum(frustum, bounds.min, bounds.max);
    };
    auto sdfVisible = [&](const SDFObject& sdf, const Transform& transform) {
        return sdf.visible &&
               test_sphere_frustum(frustum, transform_point(transform.local_to_world, {0.f, 0.f, 0.f, 1.f}),
                                   sdf_bounding_radius(sdf));
    };

    std::vector<spatial::BVHLeaf> hits;
    bvh.query_frustum(frustum, hits);

    // Entity-index markers so the fallback pass never adds an entity twice (was std::find: O(n^2)).
    std::vector<u8> seenMesh;
    std::vector<u8> seenSdf;
    auto mark = [](std::vector<u8>& seen, EntityID id) {
        if (id.index >= seen.size()) {
            seen.resize(static_cast<usize>(id.index) + 1u, 0u);
        }
        const bool first = seen[id.index] == 0u;
        seen[id.index] = 1u;
        return first;
    };

    for (const spatial::BVHLeaf& leaf : hits) {
        switch (leaf.type) {
        case spatial::BVHLeafType::Mesh: {
            const Mesh* mesh = reg.get<Mesh>(leaf.entity);
            const Transform* transform = reg.get<Transform>(leaf.entity);
            if (mesh != nullptr && transform != nullptr && meshVisible(*mesh, *transform) &&
                mark(seenMesh, leaf.entity)) {
                result.visible_meshes.push_back(leaf.entity);
            } else {
                ++result.culled_count;
            }
            break;
        }
        case spatial::BVHLeafType::SDF: {
            const SDFObject* sdf = reg.get<SDFObject>(leaf.entity);
            const Transform* transform = reg.get<Transform>(leaf.entity);
            if (sdf != nullptr && transform != nullptr && sdfVisible(*sdf, *transform) &&
                mark(seenSdf, leaf.entity)) {
                result.visible_sdf_objects.push_back(leaf.entity);
            } else {
                ++result.culled_count;
            }
            break;
        }
        case spatial::BVHLeafType::Light:
            if (reg.has<Transform>(leaf.entity) &&
                (reg.has<PointLight>(leaf.entity) || reg.has<DirectionalLight>(leaf.entity) ||
                 reg.has<SpotLight>(leaf.entity))) {
                result.visible_lights.push_back(leaf.entity);
            } else {
                ++result.culled_count;
            }
            break;
        default:
            ++result.culled_count;
            break;
        }
    }

    if (options.bvh_covers_scene) {
        // Leaves outside the frustum were never returned by the query.
        const usize leafCount = bvh.leaf_count();
        const usize returned = hits.size();
        result.culled_count += static_cast<u32>(leafCount > returned ? leafCount - returned : 0u);
        return result;
    }

    reg.each<Mesh, Transform>([&](EntityID id, Mesh& mesh, Transform& transform) {
        if (!meshVisible(mesh, transform)) {
            ++result.culled_count;
            return;
        }
        if (mark(seenMesh, id)) {
            result.visible_meshes.push_back(id);
        }
    });

    reg.each<SDFObject, Transform>([&](EntityID id, SDFObject& sdf, Transform& transform) {
        if (!sdfVisible(sdf, transform)) {
            ++result.culled_count;
            return;
        }
        if (mark(seenSdf, id)) {
            result.visible_sdf_objects.push_back(id);
        }
    });

    return result;
}

} // namespace fuse::ecs
