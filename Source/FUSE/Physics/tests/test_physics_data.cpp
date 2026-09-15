#include <fuse/physics/physics_data.hpp>

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

} // namespace

int main() {
    testRigidBodySoA();
    testCollisionShapeSoA();

    if (g_failures == 0) {
        std::printf("fuse_physics_data_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_data_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
