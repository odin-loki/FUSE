#include <fuse/physics/destruction/voxel_destruction.hpp>
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

void testCarveRadiusDerivation() {
    fuse::physics::VoxelMaterial material{};
    material.hardness = 4.f;
    const fuse::physics::f32 radius = fuse::physics::DestructionSystem::deriveCarveRadius(16.f, material);
    expectTrue(radius > 0.f, "carve radius positive for non-zero impulse");
    expectTrue(fuse::physics::DestructionSystem::shouldDestroy(16.f, material), "impulse above hardness destroys");
    expectTrue(!fuse::physics::DestructionSystem::shouldDestroy(2.f, material), "impulse below hardness preserved");
}

void testSvoCarveAndQuery() {
    fuse::physics::Svo svo;
    const fuse::physics::vec3 center{1.f, 2.f, 3.f};
    expectTrue(svo.carve(center, 1.5f), "carve succeeds");
    expectTrue(svo.isCarved(center), "center voxel carved");
    expectTrue(!svo.isCarved({10.f, 10.f, 10.f}), "distant voxel not carved");
    expectTrue(svo.carvedVoxelCount() > 0, "carved voxel count tracked");
}

void testDebrisSpawnPipeline() {
    fuse::physics::PhysicsRegistry registry{};
    fuse::physics::PhysicsResourceManager resources{};
    fuse::physics::VoxelMaterial material{};
    material.hardness = 5.f;

    fuse::physics::DestructionEvent event{};
    event.target = fuse::ecs::EntityID{7, 1};
    event.impactPoint = {0.f, 0.f, 0.f};
    event.impactNormal = {0.f, 1.f, 0.f};
    event.impulse = 25.f;
    event.carveRadius = fuse::physics::DestructionSystem::deriveCarveRadius(event.impulse, material);

    fuse::physics::DestructionSystem::processEvents({event}, registry, resources);
    expectTrue(registry.entityCount > 0, "debris entity spawned");
    expectTrue(resources.meshAllocations > 0, "debris mesh allocated");
    expectTrue(fuse::physics::DestructionSystem::spawnedDebrisCount() > 0, "spawn counter incremented");
}

} // namespace

int main() {
    testCarveRadiusDerivation();
    testSvoCarveAndQuery();
    testDebrisSpawnPipeline();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
