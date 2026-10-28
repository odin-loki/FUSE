#include <fuse/physics/body_slot.hpp>
#include <fuse/physics/forces/box_buoyancy.hpp>
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

void testBodySlotOk() {
    expectTrue(fuse::physics::body_slot_ok(0, 1), "slot 0 of capacity 1 is valid");
    expectTrue(fuse::physics::body_slot_ok(3, 4), "slot 3 of capacity 4 is valid");
    expectTrue(!fuse::physics::body_slot_ok(0, 0), "capacity 0 rejects every slot");
    expectTrue(!fuse::physics::body_slot_ok(4, 4), "a slot equal to capacity is out of range");
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

void testBoxBuoyancy() {
    const fuse::physics::BoxBuoyancy centered =
        fuse::physics::box_buoyancy(0.f, 1.f, 0.f, 1000.f, -10.f, 2.f, 4.f);
    expectTrue(centered.submergedFraction == 0.5f, "box centered on the water plane has fraction 0.5");
    expectTrue(centered.force.y > 0.f && centered.force.x == 0.f && centered.force.z == 0.f,
               "half-submerged box is pushed up");

    const fuse::physics::BoxBuoyancy submerged =
        fuse::physics::box_buoyancy(0.f, 1.f, 10.f, 1000.f, -10.f, 1.f, 1.f);
    expectTrue(submerged.submergedFraction == 1.f && submerged.force.y > 0.f && submerged.force.x == 0.f &&
                   submerged.force.z == 0.f,
               "fully submerged box with gravityY -10 has an upward force");

    const fuse::physics::BoxBuoyancy above =
        fuse::physics::box_buoyancy(5.f, 1.f, 0.f, 1000.f, -10.f, 1.f, 1.f);
    expectTrue(above.submergedFraction == 0.f && above.force.y == 0.f, "box fully above water has no buoyancy");

    const fuse::physics::BoxBuoyancy degenerate =
        fuse::physics::box_buoyancy(0.f, 0.f, 0.f, 1000.f, -10.f, 1.f, 1.f);
    expectTrue(degenerate.submergedFraction == 0.f && degenerate.force.y == 0.f,
               "non-positive box extent has no buoyancy");

    const fuse::physics::BoxBuoyancy noDensity =
        fuse::physics::box_buoyancy(0.f, 1.f, 0.f, 0.f, -10.f, 1.f, 1.f);
    expectTrue(noDensity.submergedFraction == 0.5f && noDensity.force.y == 0.f,
               "non-positive density keeps the submerged fraction and zero force");
}

} // namespace

int main() {
    testBodySlotOk();
    testRigidBodySoA();
    testCollisionShapeSoA();
    testContactRestitutionBounce();
    testBoxBuoyancy();

    if (g_failures == 0) {
        std::printf("fuse_physics_data_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_data_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
