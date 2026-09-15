#include <fuse/scene/scene_manager.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

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

    fuse::jobs::JobCounter transformsDone(1);
    fuse::jobs::JobScheduler::instance().submit([&]() {
        // TransformSystem lands in B3.3 — scaffold keeps dependency edge only.
        transformsDone.signal();
    });
    transformsDone.wait();

    fuse::jobs::JobCounter camerasDone(1);
    fuse::jobs::JobScheduler::instance().submit([&]() {
        // CameraSystem lands in B3.8 — active camera handle retained here.
        camerasDone.signal();
    });
    camerasDone.wait();

    refitBvh();
    ++m_frameIndex;
}

fuse::ecs::EntityID SceneManager::createCamera(f32 fovDegrees, bool active) {
    if (!m_initialized) {
        return fuse::ecs::EntityID::null();
    }

    const fuse::ecs::EntityID camera = m_registry.create();
    if (!camera.valid()) {
        return fuse::ecs::EntityID::null();
    }

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
