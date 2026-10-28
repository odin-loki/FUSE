#include <fuse/physics/forces/terminal_velocity.hpp>
#include <fuse/physics/solver/soft_iterations.hpp>
#include <fuse/physics/softbody/cloth_simulator.hpp>
#include <fuse/physics/softbody/particle_system.hpp>

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

void testSoftIterationsOk() {
    expectTrue(fuse::physics::soft_iterations_ok(4), "4 soft iterations is in range");
    expectTrue(!fuse::physics::soft_iterations_ok(0), "0 soft iterations is rejected");
    expectTrue(fuse::physics::soft_iterations_ok(32), "32 soft iterations is the upper bound");
    expectTrue(!fuse::physics::soft_iterations_ok(33), "33 soft iterations is rejected");
}

void testParticleSoALifecycle() {
    fuse::physics::ParticleSoA particles = fuse::physics::ParticleSoA::allocate(16);
    expectTrue(particles.capacity == 16, "particle capacity allocated");
    expectTrue(particles.positions != nullptr, "positions buffer allocated");
    fuse::physics::ParticleSoA::free(particles);
    expectTrue(particles.capacity == 0, "particle SoA freed");
}

void testClothPinnedCornersAndGravity() {
    fuse::physics::ClothSimulator cloth;
    fuse::physics::ClothDesc desc{};
    desc.rows = 8;
    desc.cols = 8;
    desc.pinnedCorners = 0b0011u;
    cloth.init(desc, {0.f, 4.f, 0.f});
    expectTrue(cloth.ready(), "cloth initialized");
    cloth.step(1.f / 60.f, {0.f, -9.81f, 0.f});
    expectTrue(cloth.particleCount() == 64, "cloth particle count matches grid");
    expectTrue(cloth.indexCount() > 0, "cloth index buffer populated");
    cloth.destroy();
}

void testWindDeflection() {
    fuse::physics::ClothSimulator cloth;
    fuse::physics::ClothDesc desc{};
    desc.rows = 4;
    desc.cols = 4;
    desc.pinnedCorners = 0u;
    cloth.init(desc, {0.f, 2.f, 0.f});
    cloth.applyWind({5.f, 0.f, 0.f});
    cloth.step(1.f / 60.f, {0.f, 0.f, 0.f});
    expectTrue(cloth.ready(), "cloth survives wind + step");
    cloth.destroy();
}

void testClampTerminalVelocity() {
    const fuse::physics::vec3 fast{6.f, 8.f, 0.f};
    const fuse::physics::vec3 clamped = fuse::physics::clamp_terminal_velocity(fast, 3.f);
    const fuse::physics::f32 len = clamped.length();
    expectTrue(len > 2.999f && len < 3.001f, "velocity of length 10 clamps to length 3");
    const fuse::physics::vec3 ortho = clamped.cross(fast);
    expectTrue(ortho.x > -1e-4f && ortho.x < 1e-4f && ortho.y > -1e-4f && ortho.y < 1e-4f &&
                   ortho.z > -1e-4f && ortho.z < 1e-4f,
               "clamped velocity stays parallel to the original");
    expectTrue(clamped.dot(fast) > 0.f, "clamped velocity keeps the same direction");

    const fuse::physics::vec3 slow{1.f, -0.5f, 0.25f};
    const fuse::physics::vec3 kept = fuse::physics::clamp_terminal_velocity(slow, 3.f);
    expectTrue(kept.x == slow.x && kept.y == slow.y && kept.z == slow.z, "short velocity is unchanged");
}

} // namespace

int main() {
    testSoftIterationsOk();
    testParticleSoALifecycle();
    testClothPinnedCornersAndGravity();
    testWindDeflection();
    testClampTerminalVelocity();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
