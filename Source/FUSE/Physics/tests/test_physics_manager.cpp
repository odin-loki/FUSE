#include <fuse/physics/physics_manager.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testManagerLifecycleAndStep() {
    fuse::physics::PhysicsManager manager;
    fuse::physics::PhysicsManagerDesc desc{};
    desc.maxBodies = 128;
    manager.init(desc);
    expectTrue(manager.desc().maxBodies == 128, "manager desc stored");

    fuse::physics::PhysicsRegistry registry{};
    registry.entityCount = 2;
    fuse::physics::PhysicsStreamManager streams{};
    manager.step(registry, 1.f / 60.f, streams);
    expectTrue(manager.stepCount() == 1, "step counter incremented");
    expectTrue(manager.bodies().count() == 2, "ECS entities synced to B4.1 SoA");
    manager.destroy();
}

void testQueriesAndForces() {
    fuse::physics::PhysicsManager manager;
    fuse::physics::PhysicsManagerDesc desc{};
    manager.init(desc);

    fuse::ecs::EntityID entity{3, 1};
    manager.applyImpulse(entity, {0.f, 10.f, 0.f});
    manager.applyForce(entity, {1.f, 0.f, 0.f});
    manager.setVelocity(entity, {1.f, 0.f, 0.f});

    fuse::ecs::EntityID hit{};
    fuse::physics::vec3 normal{};
    fuse::physics::f32 t = 0.f;
    expectTrue(manager.rayCast({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 10.f, hit, normal, t), "ray cast stub returns hit");

    std::vector<fuse::ecs::EntityID> results;
    manager.querySphere({0.f, 0.f, 0.f}, 2.f, results);
    expectTrue(!results.empty(), "sphere query returns results");

    fuse::physics::DestructionEvent event{};
    event.carveRadius = 1.f;
    manager.pushDestructionEvent(event);
    expectTrue(manager.pendingDestructionEvents() == 1, "destruction event queued");

    fuse::physics::PhysicsRegistry registry{};
    registry.entityCount = 1;
    fuse::physics::PhysicsStreamManager streams{};
    manager.step(registry, 1.f / 60.f, streams);
    expectTrue(manager.pendingDestructionEvents() == 0, "destruction events processed after step");

    manager.destroy();
}

} // namespace

int main() {
    testManagerLifecycleAndStep();
    testQueriesAndForces();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
