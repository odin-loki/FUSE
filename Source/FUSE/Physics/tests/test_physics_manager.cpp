#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/physics_manager.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

using fuse::ecs::Collider;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;
using fuse::f32;
using fuse::u32;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

EntityID spawn(Registry& reg, fuse::physics::vec3 position, u32 shape, fuse::physics::vec3 params, bool isStatic,
               f32 scalar = 0.f) {
    const EntityID id = reg.create();
    Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.is_static = isStatic;
    reg.add(id, rb);
    Collider c{};
    c.shape = shape;
    c.params = {params.x, params.y, params.z, 0.f};
    c.scalar = scalar;
    reg.add(id, c);
    return id;
}

void testManagerLifecycleAndStep() {
    Registry reg;
    reg.init(64);
    const EntityID ground = spawn(reg, {}, Collider::Plane, {0.f, 1.f, 0.f}, true);
    const EntityID ball = spawn(reg, {0.f, 2.f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f}, false);
    reg.create(); // no physics components: ignored

    fuse::physics::PhysicsManager manager;
    fuse::physics::PhysicsManagerDesc desc{};
    desc.maxBodies = 128;
    manager.init(desc);
    expectTrue(manager.desc().maxBodies == 128, "manager desc stored");

    fuse::physics::PhysicsStreamManager streams{};
    manager.step(reg, 1.f / 60.f, streams);
    expectTrue(manager.stepCount() == 1, "step counter incremented");
    expectTrue(manager.bodies().count() == 2, "Transform+RigidBody+Collider entities synced to the SoA");
    expectTrue(manager.bodyIndex(ground) != ~0u && manager.bodyIndex(ball) != ~0u, "entity -> body mapping");
    expectTrue(reg.get<Transform>(ball)->position.y < 2.f, "gravity moved the ball and was written back");

    reg.destroy_entity(ball);
    manager.step(reg, 1.f / 60.f, streams);
    expectTrue(manager.bodies().count() == 1 && manager.bodyIndex(ground) == 0u, "destroyed entity leaves the SoA");
    manager.destroy();
}

void testQueriesAndImpulses() {
    Registry reg;
    reg.init(64);
    spawn(reg, {}, Collider::Plane, {0.f, 1.f, 0.f}, true);
    const EntityID box = spawn(reg, {5.f, 1.f, 0.f}, Collider::Box, {1.f, 1.f, 1.f}, true);
    const EntityID capsule = spawn(reg, {-5.f, 2.f, 0.f}, Collider::Capsule, {0.5f, 1.f, 0.f}, true);
    const EntityID ball = spawn(reg, {0.f, 3.f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f}, false);

    fuse::physics::PhysicsManager manager;
    fuse::physics::PhysicsManagerDesc desc{};
    desc.solver.gravity = {};
    desc.solver.linearDamping = 1.f;
    manager.init(desc);
    fuse::physics::PhysicsStreamManager streams{};
    manager.step(reg, 1.f / 60.f, streams);

    EntityID hit{};
    fuse::physics::vec3 normal{};
    f32 t = 0.f;
    expectTrue(manager.rayCast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 100.f, hit, normal, t) && hit == ball &&
                   std::fabs(t - 6.5f) < 1e-3f && normal.y > 0.99f,
               "downward ray hits the ball top first");
    expectTrue(manager.rayCast({0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, 100.f, hit, normal, t) && hit == box &&
                   std::fabs(t - 4.f) < 1e-3f && normal.x < -0.99f,
               "ray along +x hits the box face");
    expectTrue(manager.rayCast({0.f, 2.f, 0.f}, {-1.f, 0.f, 0.f}, 100.f, hit, normal, t) && hit == capsule &&
                   std::fabs(t - 4.5f) < 1e-2f,
               "ray along -x hits the capsule side");
    expectTrue(!manager.rayCast({0.f, 10.f, 0.f}, {0.f, 1.f, 0.f}, 100.f, hit, normal, t), "upward ray misses");

    std::vector<EntityID> results;
    manager.querySphere({5.f, 2.5f, 0.f}, 0.6f, results);
    expectTrue(results.size() == 1u && results[0] == box, "sphere query finds the box only");

    manager.applyImpulse(ball, {0.f, 10.f, 0.f});
    manager.step(reg, 1.f / 60.f, streams);
    expectTrue(std::fabs(reg.get<fuse::ecs::RigidBody>(ball)->velocity.y - 10.f) < 1e-4f,
               "impulse / mass lands in the component velocity");

    fuse::physics::DestructionEvent event{};
    event.carveRadius = 1.f;
    manager.pushDestructionEvent(event);
    expectTrue(manager.pendingDestructionEvents() == 1, "destruction event queued");
    manager.step(reg, 1.f / 60.f, streams);
    expectTrue(manager.pendingDestructionEvents() == 0, "destruction events processed after step");
    manager.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    testManagerLifecycleAndStep();
    testQueriesAndImpulses();
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_physics_manager: all checks passed\n");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
