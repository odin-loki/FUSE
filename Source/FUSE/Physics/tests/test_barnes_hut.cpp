#include <fuse/core/init.hpp>
#include <fuse/physics/broadphase/barnes_theta.hpp>
#include <fuse/physics/forces/force_volume.hpp>
#include <fuse/physics/forces/softening.hpp>
#include <fuse/physics/nbody/barnes_hut.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

using namespace fuse::physics;
using fuse::u32;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f32 actual, f32 expected, f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr,
                     "FAIL: %s (expected %.8f, got %.8f)\n",
                     message,
                     expected,
                     actual);
        ++g_failures;
    }
}

void testExactModeUsesNaiveReference() {
    std::vector<vec3> positions = {
        {0.f, 0.f, 0.f},
        {1.f, 0.f, 0.f},
        {0.f, 2.f, 0.f},
        {-1.f, -1.f, 0.5f},
        {0.5f, -0.5f, -1.f},
    };
    std::vector<f32> masses = {1.f, 1.f, 1.f, 1.f, 1.f};

    BHParams params;
    params.theta = 0.f;
    params.gravitational_G = 1.f;
    params.softening = 0.05f;

    std::vector<vec3> naive_forces;
    std::vector<vec3> bh_forces;
    compute_nbody_forces_naive(positions, masses, naive_forces, params);

    BarnesHut barnes_hut;
    barnes_hut.computeForces(positions, masses, bh_forces, params);

    expectTrue(barnes_hut.nodeCount() > 0, "Barnes-Hut builds a tree for multi-body input");

    for (u32 i = 0; i < positions.size(); ++i) {
        expectNear(bh_forces[i].x, naive_forces[i].x, 1e-4f, "exact mode force x matches naive");
        expectNear(bh_forces[i].y, naive_forces[i].y, 1e-4f, "exact mode force y matches naive");
        expectNear(bh_forces[i].z, naive_forces[i].z, 1e-4f, "exact mode force z matches naive");
    }
}

void testApproximateModeProducesForces() {
    std::vector<vec3> positions = {
        {0.f, 0.f, 0.f},
        {10.f, 0.f, 0.f},
        {0.f, 10.f, 0.f},
        {-10.f, -10.f, 0.f},
    };
    std::vector<f32> masses = {1.f, 1.f, 1.f, 1.f};

    BHParams params;
    params.theta = 0.5f;
    params.gravitational_G = 1.f;
    params.softening = 0.1f;

    BarnesHut barnes_hut;
    std::vector<vec3> bh_forces;
    barnes_hut.computeForces(positions, masses, bh_forces, params);

    expectTrue(barnes_hut.nodeCount() > 0, "approximate mode still builds a tree");
    f32 total_force = 0.f;
    for (const vec3& force : bh_forces) {
        total_force += std::fabs(force.x) + std::fabs(force.y) + std::fabs(force.z);
    }
    expectTrue(total_force > 1e-3f, "approximate mode produces non-zero accelerations");
}

void testTwoBodyFallback() {
    std::vector<vec3> positions = {{0.f, 0.f, 0.f}, {2.f, 0.f, 0.f}};
    std::vector<f32> masses = {1.f, 1.f};
    BHParams params;
    params.gravitational_G = 1.f;
    params.softening = 0.1f;

    std::vector<vec3> naive_forces;
    std::vector<vec3> bh_forces;
    compute_nbody_forces_naive(positions, masses, naive_forces, params);

    BarnesHut barnes_hut;
    barnes_hut.computeForces(positions, masses, bh_forces, params);

    expectNear(bh_forces[0].x, naive_forces[0].x, 1e-4f, "two-body BH x matches naive");
    expectNear(bh_forces[1].x, naive_forces[1].x, 1e-4f, "two-body BH x matches naive");
}

void testForceVolumes() {
    DirectionalVolume box;
    box.center = {0.f, 0.f, 0.f};
    box.halfExtents = {1.f, 1.f, 1.f};
    box.force = {0.f, 4.f, 0.f};
    box.falloff = ForceFalloff::None;
    const vec3 outside = sample_directional(box, {3.f, 0.f, 0.f});
    expectNear(outside.y, 0.f, 1e-6f, "directional volume is zero outside the box");
    const vec3 center = sample_directional(box, {0.f, 0.f, 0.f});
    expectNear(center.y, 4.f, 1e-6f, "directional volume center gets the full force");

    RadialVolume radial;
    radial.center = {0.f, 0.f, 0.f};
    radial.radius = 2.f;
    radial.strength = 5.f;
    radial.falloff = ForceFalloff::Linear;
    const vec3 origin = sample_radial(radial, {0.f, 0.f, 0.f});
    expectNear(origin.x, 0.f, 1e-6f, "radial volume at the center is zero");
    expectNear(origin.y, 0.f, 1e-6f, "radial volume at the center has no y");
}

void testBarnesThetaOk() {
    expectTrue(barnes_theta_ok(0.5f), "barnes_theta_ok(0.5f) is true");
    expectTrue(barnes_theta_ok(1.f), "barnes_theta_ok(1) is true");
    expectTrue(!barnes_theta_ok(0.f), "barnes_theta_ok(0) is false");
    expectTrue(!barnes_theta_ok(-0.1f), "barnes_theta_ok(-0.1f) is false");
    expectTrue(!barnes_theta_ok(1.1f), "barnes_theta_ok(1.1f) is false");
    expectTrue(!barnes_theta_ok(std::numeric_limits<float>::quiet_NaN()),
               "barnes_theta_ok(non-finite) is false");
}

void testSofteningOk() {
    expectTrue(softening_ok(0.1f), "softening_ok(0.1f) is true");
    expectTrue(softening_ok(1), "softening_ok(1) is true");
    expectTrue(!softening_ok(0), "softening_ok(0) is false");
    expectTrue(!softening_ok(2), "softening_ok(2) is false");
}

} // namespace

int main() {
    testBarnesThetaOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_physics_barnes_hut_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testSofteningOk();
    fuse::core::initialize();
    testForceVolumes();
    testExactModeUsesNaiveReference();
    testApproximateModeProducesForces();
    testTwoBodyFallback();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_barnes_hut_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_barnes_hut_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
