#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/constraint_accumulation.hpp>

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

void testRigidBodySoA() {
    fuse::physics::RigidBodySoA bodies;
    bodies.reserve(4);
    const fuse::u32 first = bodies.addBody({0.f, 1.f, 0.f}, 1.f, fuse::physics::RB_NO_GRAVITY);
    const fuse::u32 second = bodies.addBody({2.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);

    expectTrue(bodies.count() == 2u, "rigid body count");
    expectTrue(first == 0u, "first body index");
    expectTrue(second == 1u, "second body index");
    expectTrue(bodies.invMasses[1] == 0.f, "static body has zero inverse mass");
}

void testCollisionShapeSoA() {
    fuse::physics::CollisionShapeSoA shapes;
    const fuse::u32 shape = shapes.addShape(fuse::physics::CollisionShapeType::Sphere, 0, {0.5f, 0.f, 0.f});
    expectTrue(shape == 0u, "shape index");
    expectTrue(shapes.count() == 1u, "shape count");
    expectTrue(shapes.scalars[0] == 0.f, "default scalar param");
}

void testContactRestitutionBounce() {
    fuse::physics::RigidBodySoA bodies;
    const fuse::u32 dynamicBody = bodies.addBody({0.f, 1.f, 0.f}, 1.f);
    const fuse::u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    bodies.linearVelocities[dynamicBody] = {0.f, -2.f, 0.f};
    bodies.restitutions[dynamicBody] = 1.f;
    bodies.restitutions[ground] = 1.f;

    fuse::physics::narrowphase::ContactManifold contact{};
    contact.valid = true;
    contact.bodyA = dynamicBody;
    contact.bodyB = ground;
    contact.contactNormal = {0.f, 1.f, 0.f};

    const fuse::f32 delta =
        fuse::physics::applyContactRestitution(bodies, contact);
    expectTrue(delta > 3.9f && delta < 4.1f, "elastic ground hit changes normal speed by 4");
    expectTrue(bodies.linearVelocities[dynamicBody].y > 1.9f && bodies.linearVelocities[dynamicBody].y < 2.1f,
               "elastic ground hit reverses the incoming velocity");
    expectTrue(bodies.linearVelocities[ground].y == 0.f, "static ground keeps zero velocity");

    const fuse::f32 separating = fuse::physics::applyContactRestitution(bodies, contact);
    expectTrue(separating == 0.f, "separating contact applies no restitution");
    expectTrue(bodies.linearVelocities[dynamicBody].y > 1.9f && bodies.linearVelocities[dynamicBody].y < 2.1f,
               "separating contact leaves the outgoing velocity");
}

} // namespace

int main() {
    testRigidBodySoA();
    testCollisionShapeSoA();
    testContactRestitutionBounce();

    if (g_failures == 0) {
        std::printf("fuse_physics_data_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_data_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
