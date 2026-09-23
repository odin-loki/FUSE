#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/spatial/svo.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::physics {

struct VoxelMaterial {
    f32 hardness = 1.f;
    /// Mass per cubic metre.
    f32 density = 1.f;
    u32 debrisMaterialId = 0;
    bool breakable = true;
};

struct DestructionEvent {
    fuse::ecs::EntityID target{};
    vec3 impactPoint{};
    vec3 impactNormal{};
    f32 impulse = 0.f;
    /// 0 derives the radius from impulse and hardness (when the impulse breaks the material).
    f32 carveRadius = 0.f;
};

struct MeshFragment {
    std::vector<vec3> verts;
    std::vector<u32> indices;
};

/// A destructible entity's voxels and material (owned by the PhysicsManager).
struct DestructibleVolume {
    VoxelVolume volume;
    VoxelMaterial material;
};

/// A debris rigid body spawned by a destruction event.
struct DebrisSpawn {
    fuse::ecs::EntityID entity{};
    fuse::ecs::EntityID source{};
    u32 voxelCount = 0;
    f32 mass = 0.f;
    MeshFragment mesh; ///< dual-contoured, world space, watertight
};

/// B4.7 — voxel carve -> dual-contoured debris rigid bodies.
class DestructionSystem {
public:
    static f32 deriveCarveRadius(f32 impulse, const VoxelMaterial& material);
    static bool shouldDestroy(f32 impulse, const VoxelMaterial& material);

    /// For each event on a registered target: carve the sphere, detach every piece that no
    /// longer reaches the anchored layer, and spawn it as a debris entity (Transform at its
    /// centre of mass, RigidBody with mass = voxels * voxel volume * density, box Collider over
    /// its bounds, velocity away from the impact) with its dual-contoured mesh.
    static void processEvents(const std::vector<DestructionEvent>& events,
                              std::unordered_map<u32, DestructibleVolume>& targets,
                              fuse::ecs::Registry& registry,
                              std::vector<DebrisSpawn>& spawned);

    static u32 spawnedDebrisCount() { return s_spawnedDebris; }

    /// Dual-contoured mesh of a fragment (built from a tight sub-volume).
    static MeshFragment meshFragment(const VoxelFragment& fragment, const VoxelVolume& source);

private:
    static u32 s_spawnedDebris;
};

} // namespace fuse::physics
