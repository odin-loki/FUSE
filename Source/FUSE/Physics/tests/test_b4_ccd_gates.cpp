// B4.11 CCD gate rows (master plan):
//  - high-velocity sphere (100 m/s) does not tunnel through a 0.1 m wall — discrete misses, CCD catches
//  - CCD introduces < 1 ms overhead per frame for 100 fast-moving bodies (optimised builds)
// TOI rows: sphere/plane/box sweeps are analytic (closed form), so there is no TOI binary search
// to bound; that row is recorded as not applicable in the execution plan.
#include <fuse/core/init.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

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

constexpr f32 kDt = 1.f / 60.f;
constexpr f32 kRadius = 0.1f;
constexpr f32 kWallHalf = 0.05f; // 0.1 m thick

SolverParams params() {
    SolverParams p;
    p.substeps = 4;
    p.iterations = 10;
    p.broadphase.cellSize = 2.f;
    p.broadphase.tableSize = 1024;
    return p;
}

/// Fires a sphere at 100 m/s along +z at a static wall at z = 0; returns the final z.
f32 fireAtWall(f32 startZ, bool ccd, u32& ccdHits) {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 wall = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Box, wall, {5.f, 5.f, kWallHalf});
    const u32 bullet = bodies.addBody({0.f, 0.f, startZ}, 1.f, RB_NO_GRAVITY | (ccd ? RB_CCD : 0u));
    shapes.addShape(CollisionShapeType::Sphere, bullet, {kRadius, 0.f, 0.f});
    bodies.linearVelocities[bullet] = {0.f, 0.f, 100.f};

    PBDSolver solver;
    solver.init(2, 8, 0);
    ccdHits = 0;
    for (int frame = 0; frame < 20; ++frame) {
        solver.step(bodies, shapes, params(), kDt);
        ccdHits += solver.lastCcdHitCount();
    }
    return bodies.positions[bullet].z;
}

void testNoTunnellingThroughThinWall() {
    // Start so every discrete substep sample (100 m/s * dt/4 = 0.4167 m apart) straddles
    // the wall: one sample at z = -0.2, the next at z = +0.217.
    const f32 stride = 100.f * kDt / 4.f;
    const f32 straddleStart = -0.2f - 5.f * stride;
    u32 hits = 0;
    const f32 discreteZ = fireAtWall(straddleStart, false, hits);
    std::printf("CCD: discrete-only final z %.3f (wall at |z| <= %.2f)\n", discreteZ, kWallHalf);
    expectTrue(discreteZ > kWallHalf + kRadius, "discrete stepping alone tunnels through the 0.1 m wall");

    int tunnelled = 0;
    u32 totalHits = 0;
    std::mt19937 rng(100u);
    std::uniform_real_distribution<f32> offset(0.f, stride);
    for (int shot = 0; shot < 50; ++shot) {
        const f32 start = shot == 0 ? straddleStart : -3.f - offset(rng);
        const f32 z = fireAtWall(start, true, hits);
        tunnelled += z > -kWallHalf ? 1 : 0;
        totalHits += hits;
    }
    std::printf("CCD: 50 shots at 100 m/s, %d tunnelled, %u CCD clamps\n", tunnelled, totalHits);
    expectTrue(tunnelled == 0, "CCD catches every 100 m/s shot at the 0.1 m wall");
    expectTrue(totalHits >= 50u, "each shot is clamped by CCD");
}

void testFastSpheresCollideHeadOn() {
    for (int ccd = 0; ccd < 2; ++ccd) {
        RigidBodySoA bodies;
        CollisionShapeSoA shapes;
        const u32 flags = RB_NO_GRAVITY | (ccd != 0 ? RB_CCD : 0u);
        const u32 a = bodies.addBody({0.f, 0.f, -1.1f}, 1.f, flags);
        const u32 b = bodies.addBody({0.f, 0.f, 1.3f}, 1.f, flags);
        shapes.addShape(CollisionShapeType::Sphere, a, {kRadius, 0.f, 0.f});
        shapes.addShape(CollisionShapeType::Sphere, b, {kRadius, 0.f, 0.f});
        bodies.linearVelocities[a] = {0.f, 0.f, 100.f};
        bodies.linearVelocities[b] = {0.f, 0.f, -100.f};
        PBDSolver solver;
        solver.init(2, 8, 0);
        for (int frame = 0; frame < 5; ++frame) {
            solver.step(bodies, shapes, params(), kDt);
        }
        const bool passed = bodies.positions[a].z > bodies.positions[b].z;
        std::printf("CCD head-on (%s): a.z %.3f b.z %.3f\n", ccd != 0 ? "ccd" : "discrete", bodies.positions[a].z,
                    bodies.positions[b].z);
        if (ccd != 0) {
            expectTrue(!passed, "CCD stops fast spheres passing through each other");
        }
    }
}

void testCcdOverheadHundredBodies() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::mt19937 rng(1000u);
    std::uniform_real_distribution<f32> pos(-40.f, 40.f);
    std::uniform_real_distribution<f32> dir(-1.f, 1.f);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    for (int i = 0; i < 1000; ++i) { // scenery the fast bodies sweep past
        const u32 body = bodies.addBody({pos(rng), 1.f + std::fabs(pos(rng)) * 0.2f, pos(rng)}, 1.f);
        shapes.addShape(i % 2 == 0 ? CollisionShapeType::Sphere : CollisionShapeType::Box, body,
                        i % 2 == 0 ? vec3{0.5f, 0.f, 0.f} : vec3{0.5f, 0.5f, 0.5f});
    }
    for (int i = 0; i < 100; ++i) {
        const u32 body = bodies.addBody({pos(rng), 5.f, pos(rng)}, 1.f, RB_CCD);
        shapes.addShape(CollisionShapeType::Sphere, body, {0.2f, 0.f, 0.f});
        bodies.linearVelocities[body] = vec3{dir(rng), dir(rng), dir(rng)}.normalized() * 100.f;
    }

    PBDSolver solver;
    solver.init(bodies.count(), 4096, 0);
    std::vector<double> samples;
    for (int i = 0; i < 21; ++i) {
        RigidBodySoA copy = bodies; // same frame each time
        const auto start = std::chrono::steady_clock::now();
        solver.applyContinuousCollision(copy, shapes, kDt);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("CCD overhead: 100 fast bodies among %u, median %.3f ms, %u clamps\n", bodies.count(), samples[10],
                solver.lastCcdHitCount());
#if defined(NDEBUG)
    expectTrue(samples[10] < 1.0, "CCD < 1 ms per frame for 100 fast-moving bodies");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    testNoTunnellingThroughThinWall();
    testFastSpheresCollideHeadOn();
    testCcdOverheadHundredBodies();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_ccd_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_ccd_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
