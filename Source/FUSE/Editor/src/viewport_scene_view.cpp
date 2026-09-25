#include <fuse/editor/viewport_scene_view.hpp>

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/sdf_csg.hpp>
#include <fuse/ecs/systems/transform_system.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::editor {

namespace {

constexpr u32 kMaxSdfMarchSteps = 256u;
constexpr f32 kSdfHitEpsilon = 1e-4f;

ecs::vec3 at(const ecs::vec3& origin, const ecs::vec3& dir, f32 t) {
    return {origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t, 1.f};
}

/// Slab test returning entry / exit parameters; false when the ray misses or the box is behind.
bool raySlab(const ecs::vec3& origin, const ecs::vec3& dir, const ecs::vec3& bmin, const ecs::vec3& bmax,
             f32& tEnter, f32& tExit) {
    tEnter = 0.f;
    tExit = std::numeric_limits<f32>::max();
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {dir.x, dir.y, dir.z};
    const f32 lo[3] = {bmin.x, bmin.y, bmin.z};
    const f32 hi[3] = {bmax.x, bmax.y, bmax.z};
    for (u32 axis = 0; axis < 3u; ++axis) {
        if (std::fabs(d[axis]) < 1e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.f / d[axis];
        f32 t1 = (lo[axis] - o[axis]) * inv;
        f32 t2 = (hi[axis] - o[axis]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tEnter = std::max(tEnter, t1);
        tExit = std::min(tExit, t2);
        if (tExit < tEnter) {
            return false;
        }
    }
    return true;
}

/// Mesh: exact hit against the mesh's local AABB carried into world space by its transform (an
/// oriented box). The ray is mapped to local space with an affine inverse, which keeps `t`.
bool intersectMesh(const ecs::Mesh& mesh, const ecs::Transform& transform, const ViewportRay& ray, f32 maxT,
                   f32& t) {
    const ecs::mat4 toLocal = ecs::inverse_affine(transform.local_to_world);
    const ecs::vec3 o = ecs::transform_point(toLocal, ray.origin);
    const ecs::vec3 tip = ecs::transform_point(toLocal, at(ray.origin, ray.direction, 1.f));
    const ecs::vec3 d{tip.x - o.x, tip.y - o.y, tip.z - o.z, 0.f};
    f32 tEnter = 0.f;
    f32 tExit = 0.f;
    if (!raySlab(o, d, mesh.aabb_min, mesh.aabb_max, tEnter, tExit) || tEnter > maxT) {
        return false;
    }
    t = tEnter;
    return true;
}

/// SDF: sphere trace the object's own distance field inside its world bounds.
bool intersectSdf(const ecs::SDFObject& sdf, const ecs::Transform& transform, const spatial::AABB& bounds,
                  const ViewportRay& ray, f32 maxT, f32& t) {
    f32 tEnter = 0.f;
    f32 tExit = 0.f;
    if (!raySlab(ray.origin, ray.direction, bounds.min, bounds.max, tEnter, tExit)) {
        return false;
    }
    tExit = std::min(tExit, maxT);
    // Roughness displacement is not 1-Lipschitz; shorten steps so the trace cannot skip a surface.
    const f32 stepScale = sdf.roughness != 0.f ? 0.5f : 1.f;
    f32 s = tEnter;
    for (u32 step = 0; step < kMaxSdfMarchSteps && s <= tExit; ++step) {
        const f32 d = ecs::sdf_object_distance(sdf, transform, at(ray.origin, ray.direction, s));
        if (d < kSdfHitEpsilon) {
            t = s;
            return true;
        }
        s += std::max(d * stepScale, kSdfHitEpsilon);
    }
    return false;
}

} // namespace

void ViewportSceneView::syncBvh_(ecs::Registry& registry) {
    m_leaves.clear();
    registry.each<ecs::Mesh, ecs::Transform>([&](ecs::EntityID id, ecs::Mesh& mesh, ecs::Transform& transform) {
        if (!mesh.visible) {
            return;
        }
        spatial::BVHLeaf leaf{};
        leaf.type = spatial::BVHLeafType::Mesh;
        leaf.entity = id;
        leaf.aabb = ecs::CullingSystem::world_bounds(transform, mesh);
        m_leaves.push_back(leaf);
    });
    registry.each<ecs::SDFObject, ecs::Transform>(
        [&](ecs::EntityID id, ecs::SDFObject& sdf, ecs::Transform& transform) {
            if (!sdf.visible) {
                return;
            }
            spatial::BVHLeaf leaf{};
            leaf.type = spatial::BVHLeafType::SDF;
            leaf.entity = id;
            leaf.aabb = ecs::CullingSystem::world_bounds(transform, sdf);
            m_leaves.push_back(leaf);
        });

    bool sameSet = m_leaves.size() == m_lastLeaves.size() && m_bvh.leaf_count() == m_leaves.size();
    for (usize i = 0; sameSet && i < m_leaves.size(); ++i) {
        sameSet = m_leaves[i].entity == m_lastLeaves[i].entity && m_leaves[i].type == m_lastLeaves[i].type;
    }
    if (sameSet) {
        for (u32 i = 0; i < static_cast<u32>(m_leaves.size()); ++i) {
            (void)m_bvh.update_leaf_aabb(i, m_leaves[i].aabb);
        }
        m_bvh.refit();
    } else {
        spatial::BVHBuildDesc desc{};
        desc.parallel = false; // editor thread; scenes are small and the build must be deterministic
        m_bvh.build(m_leaves, desc);
        ++m_bvhRebuilds;
    }
    m_lastLeaves = m_leaves;
}

void ViewportSceneView::refreshSpatial(EditorScene& scene) {
    ecs::TransformSystem::update(scene.registry());
    syncBvh_(scene.registry());
}

const ecs::SceneData& ViewportSceneView::buildFrame(EditorScene& scene, const ViewportPanel& viewport) {
    refreshSpatial(scene);

    m_camera = ecs::Camera{};
    m_camera.fov_deg = viewport.camera().fovDeg;
    m_camera.near_plane = viewport.camera().nearPlane;
    m_camera.far_plane = viewport.camera().farPlane;
    m_camera.aspect_ratio = viewport.aspect();
    m_camera.is_active = true;
    m_camera.view = viewport.viewMatrix();
    m_camera.projection = viewport.projectionMatrix();
    m_camera.view_projection = ecs::multiply(m_camera.projection, m_camera.view);
    m_camera.frustum = ecs::CullingSystem::extract_frustum(m_camera.view_projection);

    // The BVH only holds visible Mesh / SDF entities, so it does not cover hidden ones: let the
    // culling pass also walk the registry (hidden objects are rejected there).
    ecs::CullOptions options{};
    options.bvh_covers_scene = false;
    m_cull = ecs::CullingSystem::cull(scene.registry(), m_camera, m_bvh, options);
    m_frame = ecs::SceneBuildSystem::build(scene.registry(), m_cull);
    ++m_frameCount;
    return m_frame;
}

ViewportPickResult ViewportSceneView::pickRay(EditorScene& scene, const ViewportRay& ray, f32 maxDistance) {
    refreshSpatial(scene);
    ecs::Registry& registry = scene.registry();

    const spatial::BVH::LeafIntersector intersect = [&](const spatial::BVHLeaf& leaf, f32 maxT, f32& t) {
        const ecs::Transform* transform = registry.get<ecs::Transform>(leaf.entity);
        if (transform == nullptr) {
            return false;
        }
        if (leaf.type == spatial::BVHLeafType::Mesh) {
            const ecs::Mesh* mesh = registry.get<ecs::Mesh>(leaf.entity);
            return mesh != nullptr && intersectMesh(*mesh, *transform, ray, maxT, t);
        }
        if (leaf.type == spatial::BVHLeafType::SDF) {
            const ecs::SDFObject* sdf = registry.get<ecs::SDFObject>(leaf.entity);
            return sdf != nullptr && intersectSdf(*sdf, *transform, leaf.aabb, ray, maxT, t);
        }
        return false;
    };

    ViewportPickResult result{};
    spatial::BVHLeaf hit{};
    f32 t = 0.f;
    if (m_bvh.ray_cast_exact(ray.origin, ray.direction, maxDistance, intersect, hit, t)) {
        result.hit = true;
        result.entity = hit.entity;
        result.type = hit.type;
        result.distance = t;
        result.point = at(ray.origin, ray.direction, t);
    }
    return result;
}

ViewportPickResult ViewportSceneView::pick(EditorScene& scene, const ViewportPanel& viewport, f32 px, f32 py) {
    return pickRay(scene, viewport.screenRay(px, py), viewport.camera().farPlane);
}

ViewportPickResult ViewportSceneView::pickAndSelect(EditorScene& scene, const ViewportPanel& viewport, f32 px,
                                                    f32 py, EditorState& state) {
    const ViewportPickResult result = pick(scene, viewport, px, py);
    state.selectedEntities.clear();
    state.primarySelection = ecs::EntityID::null();
    if (result.hit) {
        state.selectedEntities.push_back(result.entity);
        state.primarySelection = result.entity;
    }
    return result;
}

} // namespace fuse::editor
