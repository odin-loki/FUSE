#include <fuse/scene/scene_manager.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/camera_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>

namespace fuse::scene {

void SceneManager::init(const SceneManagerDesc& desc) {
    destroy();
    m_desc = desc;

    m_registry.init();
    m_bvh.init(desc.worldSize);

    if (desc.hasVoxels) {
        SVODesc svoDesc{};
        svoDesc.origin = vec3(-desc.worldSize * 0.5f, -desc.worldSize * 0.5f, -desc.worldSize * 0.5f);
        svoDesc.rootSize = desc.worldSize;
        svoDesc.maxDepth = desc.svoDepth;
        svoDesc.storeSdf = true;
        m_svo.init(svoDesc);
    }

    m_frameIndex = 0;
    m_initialized = true;
}

void SceneManager::destroy() {
    m_svo.destroy();
    m_bvh.destroy();
    m_registry.destroy();
    m_activeCamera = fuse::ecs::EntityID::null();
    m_frameIndex = 0;
    m_initialized = false;
}

void SceneManager::refitBvh() {
    if (m_initialized) {
        m_bvh.refit();
    }
}

void SceneManager::update(f32 /*deltaSeconds*/) {
    if (!m_initialized) {
        return;
    }

    fuse::ecs::TransformSystem::update(m_registry);
    fuse::ecs::CameraSystem::update(m_registry);
    syncSpatialBvh();
    refitBvh();
    ++m_frameIndex;
}

void SceneManager::syncSpatialBvh() {
    using fuse::ecs::CullingSystem;
    using fuse::spatial::BVHLeafType;

    m_bvhSourcesScratch.clear();
    m_bvhLeaves.clear();
    m_registry.each<fuse::ecs::Mesh, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Mesh& mesh, fuse::ecs::Transform& transform) {
            m_bvhSourcesScratch.push_back({id, BVHLeafType::Mesh});
            fuse::spatial::BVHLeaf leaf{};
            leaf.type = BVHLeafType::Mesh;
            leaf.entity = id;
            leaf.aabb = CullingSystem::world_bounds(transform, mesh);
            m_bvhLeaves.push_back(leaf);
        });
    m_registry.each<fuse::ecs::SDFObject, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID id, fuse::ecs::SDFObject& sdf, fuse::ecs::Transform& transform) {
            m_bvhSourcesScratch.push_back({id, BVHLeafType::SDF});
            fuse::spatial::BVHLeaf leaf{};
            leaf.type = BVHLeafType::SDF;
            leaf.entity = id;
            leaf.aabb = CullingSystem::world_bounds(transform, sdf);
            m_bvhLeaves.push_back(leaf);
        });

    // Same entities in the same order: move bounds and refit. Otherwise (spawn/despawn or
    // archetype change mid-frame) rebuild so the BVH never holds stale or missing entities.
    if (m_bvhSourcesScratch == m_bvhSources && m_spatialBvh.leaf_count() == m_bvhLeaves.size()) {
        for (u32 i = 0; i < m_bvhLeaves.size(); ++i) {
            (void)m_spatialBvh.update_leaf_aabb(i, m_bvhLeaves[i].aabb);
        }
        m_spatialBvh.refit();
        return;
    }
    m_spatialBvh.build(m_bvhLeaves);
    m_bvhSources.swap(m_bvhSourcesScratch);
    ++m_spatialBvhRebuilds;
}

fuse::ecs::SceneData SceneManager::buildFrame(fuse::ecs::CullResult* cullOut) {
    fuse::ecs::SceneData scene;
    buildFrame(scene, cullOut);
    return scene;
}

void SceneManager::buildFrame(fuse::ecs::SceneData& out, fuse::ecs::CullResult* cullOut) {
    const fuse::ecs::Camera* camera =
        m_activeCamera.valid() ? m_registry.get<fuse::ecs::Camera>(m_activeCamera) : nullptr;
    if (!m_initialized || camera == nullptr) {
        if (cullOut != nullptr) {
            *cullOut = {};
        }
        out = {};
        return;
    }

    fuse::ecs::CullOptions options{};
    options.bvh_covers_scene = true; // syncSpatialBvh keeps a leaf for every Mesh/SDFObject
    fuse::ecs::CullingSystem::cull(m_registry, *camera, m_spatialBvh, options, m_visible, m_cullScratch);
    fuse::ecs::SceneBuildSystem::build(m_registry, m_visible, out);
    if (cullOut != nullptr) {
        *cullOut = m_visible; // copy-assign reuses the caller's capacity
    }
}

fuse::ecs::EntityID SceneManager::createCamera(f32 fovDegrees, bool active) {
    if (!m_initialized) {
        return fuse::ecs::EntityID::null();
    }

    const fuse::ecs::EntityID camera = m_registry.create();
    if (!camera.valid()) {
        return fuse::ecs::EntityID::null();
    }

    fuse::ecs::Transform transform{};
    transform.dirty = true;
    m_registry.add<fuse::ecs::Transform>(camera, transform);

    fuse::ecs::Camera cameraComponent{};
    cameraComponent.fov_deg = fovDegrees;
    cameraComponent.is_active = active;
    m_registry.add<fuse::ecs::Camera>(camera, cameraComponent);

    if (active || !m_activeCamera.valid()) {
        m_activeCamera = camera;
    }
    return camera;
}

bool SceneManager::rayCast(vec3 origin, vec3 direction, f32 maxDistance, fuse::ecs::EntityID& hitEntity,
                           f32& hitDistance) const {
    hitEntity = fuse::ecs::EntityID::null();
    if (!m_initialized) {
        return false;
    }

    f32 bvhHit = 0.f;
    if (m_bvh.rayCast(origin, direction, maxDistance, bvhHit)) {
        hitDistance = bvhHit;
        return true;
    }

    if (m_desc.hasVoxels && m_svo.isInitialized()) {
        ivec3 hitVoxel{};
        vec3 hitNormal{};
        if (m_svo.rayCast(origin, direction, maxDistance, hitVoxel, hitNormal, hitDistance)) {
            (void)hitNormal;
            (void)hitVoxel;
            return true;
        }
    }

    return false;
}

void SceneManager::querySphere(vec3 center, f32 radius, std::vector<fuse::ecs::EntityID>& results) const {
    results.clear();
    if (!m_initialized || radius <= 0.f) {
        return;
    }

    if (m_desc.hasVoxels && m_svo.isInitialized()) {
        const f32 size = m_svo.desc().rootSize / static_cast<f32>(1u << m_svo.desc().maxDepth);
        const vec3 local = center - m_svo.desc().origin;
        const s32 extent = static_cast<s32>(std::ceil(radius / size));
        const ivec3 centerVoxel{
            static_cast<s32>(std::floor(local.x / size)),
            static_cast<s32>(std::floor(local.y / size)),
            static_cast<s32>(std::floor(local.z / size)),
        };

        for (s32 dz = -extent; dz <= extent; ++dz) {
            for (s32 dy = -extent; dy <= extent; ++dy) {
                for (s32 dx = -extent; dx <= extent; ++dx) {
                    const ivec3 coord{centerVoxel.x + dx, centerVoxel.y + dy, centerVoxel.z + dz};
                    if (m_svo.get(coord) != 0u) {
                        const vec3 voxelCenter =
                            m_svo.desc().origin + toVec3(coord) * size + vec3(size * 0.5f, size * 0.5f, size * 0.5f);
                        if ((voxelCenter - center).length() <= radius) {
                            results.push_back(fuse::ecs::EntityID::null());
                        }
                    }
                }
            }
        }
    }
}

} // namespace fuse::scene
