#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/spatial/svo.hpp>

#include <vector>

namespace fuse::physics {

struct VoxelMaterial {
    f32 hardness = 1.f;
    f32 density = 1.f;
    u32 debrisMaterialId = 0;
    bool breakable = true;
};

struct DestructionEvent {
    fuse::ecs::EntityID target{};
    vec3 impactPoint{};
    vec3 impactNormal{};
    f32 impulse = 0.f;
    f32 carveRadius = 0.f;
};

struct MeshFragment {
    std::vector<vec3> verts;
    std::vector<u32> indices;
};

/// Registry/resource stubs — full ECS wiring deferred to B4.9 integration.
struct PhysicsRegistry {
    u32 entityCount = 0;
};

struct PhysicsResourceManager {
    u32 meshAllocations = 0;
};

/// B4.7 — SVO carve → dual-contour debris rigid bodies (scaffold).
class DestructionSystem {
public:
    static f32 deriveCarveRadius(f32 impulse, const VoxelMaterial& material);
    static bool shouldDestroy(f32 impulse, const VoxelMaterial& material);

    static void processEvents(const std::vector<DestructionEvent>& events,
                              PhysicsRegistry& registry,
                              PhysicsResourceManager& resources);

    static u32 spawnedDebrisCount() { return s_spawnedDebris; }

private:
    static void applyDestruction(const DestructionEvent& event,
                                 Svo& svo,
                                 PhysicsRegistry& registry,
                                 PhysicsResourceManager& resources);

    static fuse::ecs::EntityID spawnDebris(const std::vector<vec3>& verts,
                                           const std::vector<u32>& indices,
                                           vec3 center,
                                           f32 mass,
                                           vec3 initialVelocity,
                                           PhysicsRegistry& registry,
                                           PhysicsResourceManager& resources);

    static std::vector<MeshFragment> clusterDisconnected(const std::vector<vec3>& verts,
                                                           const std::vector<u32>& indices);

    static u32 s_spawnedDebris;
};

} // namespace fuse::physics
