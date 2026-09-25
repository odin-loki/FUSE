#include <fuse/physics/destruction/voxel_destruction.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {

u32 DestructionSystem::s_spawnedDebris = 0;

f32 DestructionSystem::deriveCarveRadius(f32 impulse, const VoxelMaterial& material) {
    const f32 hardness = std::max(material.hardness, 0.01f);
    return std::sqrt(std::max(impulse, 0.f) / hardness) * 0.25f;
}

bool DestructionSystem::shouldDestroy(f32 impulse, const VoxelMaterial& material) {
    return material.breakable && impulse >= material.hardness;
}

MeshFragment DestructionSystem::meshFragment(const VoxelFragment& fragment, const VoxelVolume& source) {
    VoxelVolume local;
    const f32 size = source.voxelSize();
    const vec3 origin = source.origin() + vec3{static_cast<f32>(fragment.min.x) * size,
                                               static_cast<f32>(fragment.min.y) * size,
                                               static_cast<f32>(fragment.min.z) * size};
    local.init(origin, size,
               {fragment.max.x - fragment.min.x + 1, fragment.max.y - fragment.min.y + 1,
                fragment.max.z - fragment.min.z + 1});
    for (const ivec3& v : fragment.voxels) {
        local.set({v.x - fragment.min.x, v.y - fragment.min.y, v.z - fragment.min.z}, 1u);
    }
    MeshFragment mesh;
    local.extractSurface(mesh.verts, mesh.indices);
    return mesh;
}

void DestructionSystem::processEvents(const std::vector<DestructionEvent>& events,
                                      std::unordered_map<u32, DestructibleVolume>& targets,
                                      fuse::ecs::Registry& registry,
                                      std::vector<DebrisSpawn>& spawned) {
    for (const DestructionEvent& event : events) {
        const auto it = targets.find(event.target.index);
        if (!event.target.valid() || it == targets.end()) {
            continue;
        }
        DestructibleVolume& target = it->second;
        f32 radius = event.carveRadius;
        if (radius <= 0.f) {
            if (!shouldDestroy(event.impulse, target.material)) {
                continue;
            }
            radius = deriveCarveRadius(event.impulse, target.material);
        }
        if (!target.material.breakable || target.volume.carve(event.impactPoint, radius) == 0u) {
            continue;
        }

        const f32 size = target.volume.voxelSize();
        const f32 voxelMass = size * size * size * target.material.density;
        for (VoxelFragment& fragment : target.volume.detachFloating()) {
            DebrisSpawn debris{};
            debris.source = event.target;
            debris.voxelCount = static_cast<u32>(fragment.voxels.size());
            debris.mass = static_cast<f32>(debris.voxelCount) * voxelMass;
            debris.mesh = meshFragment(fragment, target.volume);

            const vec3 lo = target.volume.voxelCenter(fragment.min) - vec3{size, size, size} * 0.5f;
            const vec3 hi = target.volume.voxelCenter(fragment.max) + vec3{size, size, size} * 0.5f;
            const vec3 boundsCenter = (lo + hi) * 0.5f;
            const vec3 half = (hi - lo) * 0.5f;
            vec3 away = boundsCenter - event.impactPoint;
            away = away.length() > 1e-6f ? away.normalized() : event.impactNormal * -1.f;
            const vec3 velocity = away * std::min(event.impulse / std::max(debris.mass, 1e-3f), 20.f);

            debris.entity = registry.create();
            fuse::ecs::Transform transform{};
            transform.position = {boundsCenter.x, boundsCenter.y, boundsCenter.z, 1.f};
            registry.add(debris.entity, transform);
            fuse::ecs::RigidBody body{};
            body.mass = debris.mass;
            body.inv_mass = 1.f / debris.mass;
            body.velocity = {velocity.x, velocity.y, velocity.z, 0.f};
            registry.add(debris.entity, body);
            fuse::ecs::Collider collider{};
            collider.shape = fuse::ecs::Collider::Box;
            collider.params = {half.x, half.y, half.z, 0.f};
            registry.add(debris.entity, collider);

            ++s_spawnedDebris;
            spawned.push_back(std::move(debris));
        }
    }
}

} // namespace fuse::physics
