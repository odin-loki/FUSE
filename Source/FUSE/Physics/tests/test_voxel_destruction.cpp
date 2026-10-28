#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/physics/destruction/debris_count.hpp>
#include <fuse/physics/destruction/voxel_destruction.hpp>
#include <fuse/physics/forces/explosion.hpp>
#include <fuse/physics/spatial/svo.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testDebrisCountOk() {
    expectTrue(fuse::physics::debris_count_ok(0), "debris_count_ok(0) is true");
    expectTrue(fuse::physics::debris_count_ok(64), "debris_count_ok(64) is true");
    expectTrue(!fuse::physics::debris_count_ok(-1), "debris_count_ok(-1) is false");
    expectTrue(!fuse::physics::debris_count_ok(65), "debris_count_ok(65) is false");
}

void testCarveRadiusDerivation() {
    fuse::physics::VoxelMaterial material{};
    material.hardness = 4.f;
    const fuse::physics::f32 radius = fuse::physics::DestructionSystem::deriveCarveRadius(16.f, material);
    expectTrue(radius > 0.f, "carve radius positive for non-zero impulse");
    expectTrue(fuse::physics::DestructionSystem::shouldDestroy(16.f, material), "impulse above hardness destroys");
    expectTrue(!fuse::physics::DestructionSystem::shouldDestroy(2.f, material), "impulse below hardness preserved");
}

void testVolumeCarveAndQuery() {
    fuse::physics::VoxelVolume volume;
    volume.init({0.f, 0.f, 0.f}, 0.5f, {8, 8, 8});
    volume.fill({0, 0, 0}, {7, 7, 7}, 1u);
    expectTrue(volume.solidCount() == 512u, "fill counts voxels");
    const fuse::physics::vec3 center{2.f, 2.f, 2.f};
    expectTrue(volume.carve(center, 0.8f) > 0u, "carve removes voxels");
    expectTrue(volume.get(volume.voxelAt(center)) == 0u, "centre voxel carved");
    expectTrue(volume.get({7, 7, 7}) != 0u, "distant voxel untouched");
}

void testDebrisSpawnPipeline() {
    // A bar standing on the anchored layer with a block held by a one-voxel neck.
    fuse::physics::VoxelVolume volume;
    volume.init({0.f, 0.f, 0.f}, 1.f, {3, 8, 3});
    volume.fill({1, 0, 1}, {1, 4, 1}, 1u);
    volume.fill({0, 5, 0}, {2, 7, 2}, 1u);
    std::unordered_map<fuse::u32, fuse::physics::DestructibleVolume> targets;
    const fuse::ecs::EntityID wall{7, 1};
    fuse::physics::VoxelMaterial material{};
    material.density = 2.f;
    targets[wall.index] = {volume, material};

    fuse::ecs::Registry registry;
    registry.init(64);
    fuse::physics::DestructionEvent event{};
    event.target = wall;
    event.impactPoint = {1.5f, 3.5f, 1.5f};
    event.impulse = 1.f;
    event.carveRadius = 0.6f; // removes the neck voxel (1, 3, 1)
    std::vector<fuse::physics::DebrisSpawn> spawned;
    fuse::physics::DestructionSystem::processEvents({event}, targets, registry, spawned);
    expectTrue(spawned.size() == 1u, "the block above the cut falls off as one piece");
    expectTrue(!spawned.empty() && spawned[0].voxelCount == 28u && spawned[0].mass == 56.f,
               "debris mass = voxels x voxel volume x density");
    expectTrue(!spawned.empty() && registry.get<fuse::ecs::RigidBody>(spawned[0].entity) != nullptr,
               "debris entity has a rigid body");
    expectTrue(fuse::physics::DestructionSystem::spawnedDebrisCount() > 0, "spawn counter incremented");
}

void testExplosionImpulse() {
    fuse::physics::Explosion boom{};
    boom.origin = {0.f, 0.f, 0.f};
    boom.radius = 4.f;
    boom.impulse = 8.f;
    const fuse::physics::vec3 side =
        fuse::physics::explosion_impulse(boom, fuse::physics::vec3{2.f, 0.f, 0.f}, 2.f);
    expectTrue(side.x > 0.f && side.y == 0.f && side.z == 0.f, "+X point receives a +X explosion impulse");

    const fuse::physics::vec3 outside =
        fuse::physics::explosion_impulse(boom, fuse::physics::vec3{5.f, 0.f, 0.f}, 2.f);
    expectTrue(outside.x == 0.f && outside.y == 0.f && outside.z == 0.f, "point outside the radius gets zero");

    const fuse::physics::vec3 origin =
        fuse::physics::explosion_impulse(boom, boom.origin, 2.f);
    expectTrue(origin.x == 0.f && origin.y == 0.f && origin.z == 0.f, "origin gets zero");
}

} // namespace

int main() {
    testDebrisCountOk();
    testCarveRadiusDerivation();
    testVolumeCarveAndQuery();
    testDebrisSpawnPipeline();
    testExplosionImpulse();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
