#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/gjk.hpp>
#include <fuse/physics/physics_data.hpp>

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::physics::f32 actual, fuse::physics::f32 expected, fuse::physics::f32 tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s (got %.6f expected %.6f)\n", message, actual, expected);
        ++g_failures;
    }
}

void testSphereSphereCollision() {
    const auto manifold = fuse::physics::narrowphase::collideSphereSphere(
        {0.f, 0.f, 0.f},
        1.f,
        {1.5f, 0.f, 0.f},
        1.f,
        0u,
        1u);
    expectTrue(manifold.valid, "overlapping spheres produce contact");
    expectTrue(manifold.penetrationDepth > 0.f, "penetration depth positive");
}

void testSpherePlaneCollision() {
    const auto manifold = fuse::physics::narrowphase::collideSpherePlane(
        {0.f, 0.5f, 0.f},
        1.f,
        {0.f, 1.f, 0.f},
        0.f,
        0u,
        1u);
    expectTrue(manifold.valid, "sphere intersecting ground plane produces contact");
}

void testBoxSphereCollision() {
    const auto manifold = fuse::physics::narrowphase::collideBoxSphere(
        {0.f, 1.2f, 0.f},
        0.5f,
        {0.f, 0.f, 0.f},
        {1.f, 1.f, 1.f},
        0u,
        1u);
    expectTrue(manifold.valid, "sphere resting on box top produces contact");
    expectNear(manifold.penetrationDepth, 0.3f, 1e-4f, "box-sphere penetration depth");
    expectNear(manifold.contactNormal.y, 1.f, 1e-4f, "box-sphere normal points up");
}

void testCapsuleSphereCollision() {
    const auto manifold = fuse::physics::narrowphase::collideCapsuleSphere(
        {0.f, 0.f, 0.7f},
        0.5f,
        {0.f, 0.f, 0.f},
        {0.4f, 1.f, 0.f},
        0u,
        1u);
    expectTrue(manifold.valid, "sphere overlapping capsule hemisphere produces contact");
    expectTrue(manifold.penetrationDepth > 0.f, "capsule-sphere penetration depth positive");
}

void testContactBufferClearReuse() {
    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.reserve(8u);
    buffer.preparePairSlots(2u);
    fuse::physics::narrowphase::ContactManifold first{};
    first.valid = true;
    first.bodyA = 0u;
    first.bodyB = 1u;
    first.penetrationDepth = 0.25f;
    buffer.writeSlot(0u, first);
    expectTrue(buffer.compact() == 1u, "compact keeps valid slot");

    buffer.clear();
    expectTrue(buffer.activeCount == 0u, "clear resets active count");
    expectTrue(buffer.pairSlotCount == 0u, "clear resets pair slots");

    buffer.preparePairSlots(4u);
    fuse::physics::narrowphase::ContactManifold second = first;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    buffer.writeSlot(3u, first);
    expectTrue(buffer.compact() == 2u, "reuse after clear compacts new contacts");
}

void testRunNarrowphaseIntoBufferJobSafe() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;

    const fuse::u32 sphereBody = bodies.addBody({0.f, 1.2f, 0.f}, 1.f);
    const fuse::u32 boxBody = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Sphere, sphereBody, {0.5f, 0.f, 0.f});
    shapes.addShape(fuse::physics::CollisionShapeType::Box, boxBody, {1.f, 1.f, 1.f});

    const std::vector<fuse::physics::broadphase::CandidatePair> pairs = {{sphereBody, boxBody}};

    fuse::physics::narrowphase::ContactBufferSoA buffer;
    buffer.reserve(1u);
    fuse::physics::narrowphase::runNarrowphaseIntoBuffer(pairs, bodies, shapes, buffer);

    expectTrue(buffer.activeCount == 1u, "parallel narrowphase resolves box-sphere pair");
    const auto manifold = buffer.manifoldAt(0u);
    expectTrue(manifold.valid, "buffer manifold valid");
    expectTrue(manifold.bodyA == sphereBody && manifold.bodyB == boxBody, "buffer preserves body indices");
}

void testGjkSupportAndEpaStub() {
    const fuse::physics::vec3 hull[] = {
        {-1.f, 0.f, 0.f},
        {1.f, 0.f, 0.f},
    };
    const fuse::physics::vec3 supportPoint =
        fuse::physics::narrowphase::support(hull, 2, {1.f, 0.f, 0.f});
    expectTrue(supportPoint.x == 1.f, "support picks furthest hull point");

    const auto manifold = fuse::physics::narrowphase::epa(hull, 2, hull, 2, 0u, 1u);
    expectTrue(manifold.valid, "epa stub reports overlap for identical hulls");
}

} // namespace

int main() {
    testSphereSphereCollision();
    testSpherePlaneCollision();
    testBoxSphereCollision();
    testCapsuleSphereCollision();
    testContactBufferClearReuse();
    testRunNarrowphaseIntoBufferJobSafe();
    testGjkSupportAndEpaStub();

    if (g_failures == 0) {
        std::printf("fuse_physics_narrowphase_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_narrowphase_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
