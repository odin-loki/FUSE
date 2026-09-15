#include <fuse/core/init.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
                     "FAIL: %s (expected %.5f, got %.5f)\n",
                     message,
                     expected,
                     actual);
        ++g_failures;
    }
}

u32 countValidContacts(const std::vector<narrowphase::ContactManifold>& manifolds) {
    u32 count = 0;
    for (const narrowphase::ContactManifold& manifold : manifolds) {
        if (manifold.valid) {
            ++count;
        }
    }
    return count;
}

void testBroadphaseToNarrowphasePipeline() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 sphere = bodies.addBody({0.f, 0.5f, 0.f}, 1.f, 0);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Sphere, sphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);

    broadphase::SpatialHashParams hashParams{};
    hashParams.bodyCount = bodies.count();
    hashParams.cellSize = 2.f;
    hashParams.tableSize = 1024;

    const std::vector<broadphase::CandidatePair> pairs =
        broadphase::runBroadphase(bodies, shapes, hashParams);
    expectTrue(!pairs.empty(), "broadphase finds sphere-plane candidate pair");

    const std::vector<narrowphase::ContactManifold> manifolds =
        narrowphase::runNarrowphase(pairs, bodies, shapes);
    expectTrue(countValidContacts(manifolds) >= 1u, "narrowphase resolves sphere-plane contact");
}

void testPbdSolverUsesBroadphaseAndNarrowphase() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 sphere = bodies.addBody({0.f, 2.f, 0.f}, 1.f, 0);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Sphere, sphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);

    PBDSolver solver;
    solver.init(2, 8, 0);

    SolverParams params;
    params.substeps = 4;
    params.iterations = 12;
    params.gravity = {0.f, -9.81f, 0.f};
    params.broadphase.cellSize = 2.f;
    params.broadphase.tableSize = 1024;

    const f32 dt = 1.f / 120.f;
    for (int step = 0; step < 300; ++step) {
        solver.step(bodies, shapes, params, dt);
    }

    expectTrue(solver.contactCount() >= 1u, "PBD solver reports contacts from broad/narrow phase");
    expectNear(bodies.positions[sphere].y, 0.5f, 0.2f, "PBD solver settles sphere on ground");
}

void testCcdPipelineOnBroadphasePairs() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 fastSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 targetSphere = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[fastSphere] = {10.f, 0.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, fastSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, targetSphere, {0.5f, 0.f, 0.f});

    broadphase::SpatialHashParams hashParams{};
    hashParams.bodyCount = bodies.count();
    hashParams.cellSize = 4.f;
    hashParams.tableSize = 1024;

    const std::vector<broadphase::CandidatePair> pairs =
        broadphase::runBroadphase(bodies, shapes, hashParams);
    expectTrue(!pairs.empty(), "broadphase finds fast sphere pair for CCD");

    std::vector<TOIResult> toiResults;
    CcdPipeline ccd;
    const u32 hitCount = ccd.sweepPairs(bodies, shapes, pairs, 1.f, toiResults);
    expectTrue(hitCount == 1u, "CCD stub reports one TOI for RB_CCD fast body");
    if (hitCount == 1u && !toiResults.empty()) {
        expectTrue(toiResults[0].valid, "CCD TOI result is valid");
        expectNear(toiResults[0].toi, 0.1f, 0.05f, "CCD TOI matches analytic sweep");
    }
}

void testPhysicsManagerEndToEndIntegration() {
    PhysicsManager manager;
    PhysicsManagerDesc desc{};
    desc.maxBodies = 16;
    desc.enableCcd = true;
    desc.solver.substeps = 4;
    desc.solver.iterations = 12;
    desc.solver.gravity = {0.f, -9.81f, 0.f};
    desc.solver.broadphase.cellSize = 2.f;
    desc.solver.broadphase.tableSize = 1024;
    manager.init(desc);

    PhysicsRegistry registry{};
    registry.entityCount = 2;
    PhysicsStreamManager streams{};

    const f32 dt = 1.f / 120.f;
    for (int step = 0; step < 300; ++step) {
        manager.step(registry, dt, streams);
    }

    expectTrue(manager.bodies().count() == 2u, "manager syncs ground + dynamic sphere");
    expectTrue(manager.shapes().count() == 2u, "manager provisions collision shapes for synced bodies");
    expectTrue(manager.stepCount() == 300u, "manager completes integrated simulation steps");
    expectNear(manager.bodies().positions[1].y, 0.5f, 0.25f, "manager sphere settles via PBD pipeline");
    expectTrue(manager.desc().enableCcd, "manager integrates CCD sweep when enabled");

    manager.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBroadphaseToNarrowphasePipeline();
    testPbdSolverUsesBroadphaseAndNarrowphase();
    testCcdPipelineOnBroadphasePairs();
    testPhysicsManagerEndToEndIntegration();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_phase4_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_phase4_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
