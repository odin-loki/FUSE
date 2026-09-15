#include <fuse/physics/destruction/voxel_destruction.hpp>

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

void DestructionSystem::processEvents(const std::vector<DestructionEvent>& events,
                                      PhysicsRegistry& registry,
                                      PhysicsResourceManager& resources) {
    Svo svo;
    for (const DestructionEvent& event : events) {
        applyDestruction(event, svo, registry, resources);
    }
}

void DestructionSystem::applyDestruction(const DestructionEvent& event,
                                         Svo& svo,
                                         PhysicsRegistry& registry,
                                         PhysicsResourceManager& resources) {
    if (event.carveRadius <= 0.f) {
        return;
    }

    svo.carve(event.impactPoint, event.carveRadius);

    aabb affectedRegion{
        {event.impactPoint.x - event.carveRadius * 2.f,
         event.impactPoint.y - event.carveRadius * 2.f,
         event.impactPoint.z - event.carveRadius * 2.f},
        {event.impactPoint.x + event.carveRadius * 2.f,
         event.impactPoint.y + event.carveRadius * 2.f,
         event.impactPoint.z + event.carveRadius * 2.f},
    };

    std::vector<vec3> verts;
    std::vector<u32> indices;
    if (!svo.extractMeshRegion(affectedRegion, verts, indices) || verts.empty()) {
        return;
    }

    const auto fragments = clusterDisconnected(verts, indices);
    for (const MeshFragment& fragment : fragments) {
        if (fragment.verts.size() < 4) {
            continue;
        }

        vec3 center{};
        for (const vec3& vert : fragment.verts) {
            center = center + vert;
        }
        center = center * (1.f / static_cast<f32>(fragment.verts.size()));

        const f32 mass = static_cast<f32>(fragment.verts.size()) * 0.1f;
        const vec3 debrisVelocity{
            event.impactNormal.x * -event.impulse * 0.3f,
            event.impactNormal.y * -event.impulse * 0.3f + 2.f,
            event.impactNormal.z * -event.impulse * 0.3f,
        };

        spawnDebris(fragment.verts, fragment.indices, center, mass, debrisVelocity, registry, resources);
    }
}

fuse::ecs::EntityID DestructionSystem::spawnDebris(const std::vector<vec3>& verts,
                                                   const std::vector<u32>& indices,
                                                   vec3 center,
                                                   f32 mass,
                                                   vec3 initialVelocity,
                                                   PhysicsRegistry& registry,
                                                   PhysicsResourceManager& resources) {
    if (verts.empty() || indices.empty() || mass <= 0.f) {
        return fuse::ecs::EntityID::null();
    }

    ++registry.entityCount;
    ++resources.meshAllocations;
    ++s_spawnedDebris;

    return fuse::ecs::EntityID{registry.entityCount, 1};
}

std::vector<MeshFragment> DestructionSystem::clusterDisconnected(const std::vector<vec3>& verts,
                                                                 const std::vector<u32>& indices) {
    MeshFragment fragment;
    fragment.verts = verts;
    fragment.indices = indices;
    return {fragment};
}

} // namespace fuse::physics
