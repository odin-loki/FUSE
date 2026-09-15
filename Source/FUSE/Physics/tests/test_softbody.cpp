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

} // namespace

int main() {
    testParticleSoALifecycle();
    testClothPinnedCornersAndGravity();
    testWindDeflection();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
