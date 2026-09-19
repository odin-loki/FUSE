#include <fuse/core/init.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/constraint_accumulation.hpp>
#include <fuse/physics/solver/contact_island_graph.hpp>
#include <fuse/physics/solver/pbd_island_solve.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <tuple>

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

void testSphereGroundFallTime() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 sphere = bodies.addBody({0.f, 10.f, 0.f}, 1.f, 0);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Sphere, sphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);

    PBDSolver solver;
    solver.init(2, 8, 0);

    SolverParams params;
    params.substeps = 8;
    params.iterations = 12;
    params.gravity = {0.f, -9.81f, 0.f};
    params.broadphase.cellSize = 2.f;
    params.broadphase.tableSize = 1024;

    const f32 dt = 1.f / 120.f;
    for (int step = 0; step < 420; ++step) {
        solver.step(bodies, shapes, params, dt);
    }

    expectNear(bodies.positions[sphere].y, 0.5f, 0.15f, "sphere rests on ground plane");
    expectTrue(bodies.positions[sphere].y <= 0.65f, "sphere settles near ground contact height");
}

void testOverlappingSpheresSeparate() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 4, 0);
    SolverParams params;
    params.substeps = 1;
    params.iterations = 20;
    params.broadphase.cellSize = 4.f;

    solver.step(bodies, shapes, params, 1.f / 60.f);

    const f32 separation = (bodies.positions[bodyA] - bodies.positions[bodyB]).length();
    expectNear(separation, 2.f, 0.05f, "overlapping spheres separate to sum of radii");
}

void testDistanceConstraintHoldsLength() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 4, 1);
    solver.setDistanceConstraints({DistanceConstraint{
        .bodyA = bodyA,
        .bodyB = bodyB,
        .restLength = 2.f,
        .compliance = 0.f,
    }});

    SolverParams params;
    params.substeps = 4;
    params.iterations = 16;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    for (int i = 0; i < 30; ++i) {
        bodies.forces[bodyB] = {50.f, 0.f, 0.f};
        solver.step(bodies, shapes, params, 1.f / 60.f);
    }

    const f32 dist = (bodies.positions[bodyA] - bodies.positions[bodyB]).length();
    expectNear(dist, 2.f, 0.05f, "distance constraint holds rest length under load");
}

void testCompliantSpringStretchesUnderLoad() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({1.5f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 4, 1);
    const f32 restLength = 1.5f;
    solver.setDistanceConstraints({DistanceConstraint{
        .bodyA = bodyA,
        .bodyB = bodyB,
        .restLength = restLength,
        .compliance = 0.5f,
    }});

    SolverParams params;
    params.substeps = 4;
    params.iterations = 8;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    for (int i = 0; i < 60; ++i) {
        bodies.forces[bodyB] = {200.f, 0.f, 0.f};
        solver.step(bodies, shapes, params, 1.f / 60.f);
    }

    const f32 dist = (bodies.positions[bodyA] - bodies.positions[bodyB]).length();
    expectTrue(dist > restLength + 0.05f, "compliant spring stretches beyond rest length under sustained load");
    expectTrue(dist < restLength + 1.5f, "compliant spring stretch remains bounded");
}

void testRestLengthSpringRecoversAfterRelease() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 4, 1);
    const f32 restLength = 2.f;
    solver.setDistanceConstraints({DistanceConstraint{
        .bodyA = bodyA,
        .bodyB = bodyB,
        .restLength = restLength,
        .compliance = 0.f,
    }});

    SolverParams params;
    params.substeps = 4;
    params.iterations = 24;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    for (int i = 0; i < 20; ++i) {
        bodies.forces[bodyB] = {120.f, 0.f, 0.f};
        solver.step(bodies, shapes, params, 1.f / 60.f);
    }

    for (int i = 0; i < 80; ++i) {
        solver.step(bodies, shapes, params, 1.f / 60.f);
    }

    const f32 dist = (bodies.positions[bodyA] - bodies.positions[bodyB]).length();
    expectNear(dist, restLength, 0.08f, "rigid rest-length spring recovers after load release");
}

void testSolverReportsIterationCount() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);

    PBDSolver solver;
    solver.init(1, 0, 0);

    SolverParams params;
    params.substeps = 1;
    params.iterations = 17;
    params.gravity = {};

    solver.step(bodies, shapes, params, 1.f / 60.f);
    expectTrue(solver.lastIterationCount() == 17u, "solver records configured iteration count");
}

void testContactIslandPartitionsDisconnectedGroups() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyC = bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {1.f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {1.f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyC, {1.f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(3, 8, 0);

    SolverParams params;
    params.substeps = 1;
    params.iterations = 4;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    solver.step(bodies, shapes, params, 1.f / 60.f);

    const ContactIslandGraph& graph = solver.islandGraph();
    expectTrue(graph.islandCount() >= 2u, "disconnected contact groups form separate islands");

    const u32 islandA = graph.bodyIsland(bodyA);
    const u32 islandB = graph.bodyIsland(bodyB);
    const u32 islandC = graph.bodyIsland(bodyC);
    expectTrue(islandA == islandB, "touching bodies share an island");
    expectTrue(islandC != islandA, "isolated body is in a different island");
}

void testSleepDetection() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 sphere = bodies.addBody({0.f, 0.5f, 0.f}, 1.f, 0);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Sphere, sphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);

    PBDSolver solver;
    solver.init(2, 2, 0);
    SolverParams params;
    params.sleepTimeRequired = 0.2f;
    params.substeps = 1;
    params.iterations = 8;
    params.broadphase.cellSize = 2.f;

    for (int i = 0; i < 240; ++i) {
        solver.step(bodies, shapes, params, 1.f / 120.f);
    }

    expectTrue((bodies.flags[sphere] & RB_SLEEPING) != 0u, "resting body enters sleep state");
}

void testRestLengthSpringConvergesUnderIterations() {
    CollisionShapeSoA shapes;
    const f32 restLength = 2.f;

    auto setupChain = [&](RigidBodySoA& bodies) {
        bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
        bodies.addBody({2.5f, 0.f, 0.f}, 1.f, 0);
        bodies.addBody({5.5f, 0.f, 0.f}, 1.f, 0);
        for (u32 i = 0; i < bodies.count(); ++i) {
            shapes.addShape(CollisionShapeType::Sphere, i, {0.1f, 0.f, 0.f});
        }
    };

    RigidBodySoA fewIterBodies;
    setupChain(fewIterBodies);

    PBDSolver fewIterSolver;
    fewIterSolver.init(3, 0, 2);
    fewIterSolver.setDistanceConstraints({
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = restLength},
        DistanceConstraint{.bodyA = 1, .bodyB = 2, .restLength = restLength},
    });

    SolverParams fewParams;
    fewParams.substeps = 1;
    fewParams.iterations = 1;
    fewParams.gravity = {};
    fewParams.broadphase.cellSize = 4.f;
    fewIterSolver.step(fewIterBodies, shapes, fewParams, 1.f / 60.f);
    const f32 errFew =
        std::fabs((fewIterBodies.positions[0] - fewIterBodies.positions[1]).length() - restLength) +
        std::fabs((fewIterBodies.positions[1] - fewIterBodies.positions[2]).length() - restLength);

    shapes = CollisionShapeSoA{};
    RigidBodySoA manyIterBodies;
    setupChain(manyIterBodies);

    PBDSolver manyIterSolver;
    manyIterSolver.init(3, 0, 2);
    manyIterSolver.setDistanceConstraints({
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = restLength},
        DistanceConstraint{.bodyA = 1, .bodyB = 2, .restLength = restLength},
    });

    SolverParams manyParams = fewParams;
    manyParams.iterations = 32;
    manyIterSolver.step(manyIterBodies, shapes, manyParams, 1.f / 60.f);
    const f32 errMany =
        std::fabs((manyIterBodies.positions[0] - manyIterBodies.positions[1]).length() - restLength) +
        std::fabs((manyIterBodies.positions[1] - manyIterBodies.positions[2]).length() - restLength);

    expectTrue(errMany < errFew, "more iterations converge rest-length spring closer to target");
    expectTrue(errMany < 0.05f, "rest-length spring converges under sufficient iterations");
}

void testMultiIslandIndependentSolve() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 pair0A = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 pair0B = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    const u32 pair1A = bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    const u32 pair1B = bodies.addBody({22.f, 0.f, 0.f}, 1.f, 0);
    for (u32 i = 0; i < bodies.count(); ++i) {
        shapes.addShape(CollisionShapeType::Sphere, i, {0.1f, 0.f, 0.f});
    }

    PBDSolver solver;
    solver.init(4, 0, 2);
    const f32 restLength = 2.f;
    solver.setDistanceConstraints({
        DistanceConstraint{.bodyA = pair0A, .bodyB = pair0B, .restLength = restLength},
        DistanceConstraint{.bodyA = pair1A, .bodyB = pair1B, .restLength = restLength},
    });

    SolverParams params;
    params.substeps = 1;
    params.iterations = 24;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    const vec3 pair1BStart = bodies.positions[pair1B];
    for (int i = 0; i < 40; ++i) {
        bodies.forces[pair0B] = {80.f, 0.f, 0.f};
        solver.step(bodies, shapes, params, 1.f / 60.f);
    }

    const f32 dist0 = (bodies.positions[pair0A] - bodies.positions[pair0B]).length();
    const f32 dist1 = (bodies.positions[pair1A] - bodies.positions[pair1B]).length();
    const f32 pair1BShift = (bodies.positions[pair1B] - pair1BStart).length();

    expectNear(dist0, restLength, 0.08f, "loaded island spring holds rest length");
    expectNear(dist1, restLength, 0.05f, "remote island spring stays at rest length");
    expectTrue(pair1BShift < 0.05f, "remote island bodies remain independent under local load");
    expectTrue(solver.islandGraph().islandCount() >= 2u, "disconnected springs form separate islands");
}

void testConstraintResidualDecreasesWithIterations() {
    CollisionShapeSoA shapes;
    const f32 restLength = 2.f;

    auto setupChain = [&](RigidBodySoA& bodies) {
        bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
        bodies.addBody({2.5f, 0.f, 0.f}, 1.f, 0);
        bodies.addBody({5.5f, 0.f, 0.f}, 1.f, 0);
        for (u32 i = 0; i < bodies.count(); ++i) {
            shapes.addShape(CollisionShapeType::Sphere, i, {0.1f, 0.f, 0.f});
        }
    };

    RigidBodySoA fewIterBodies;
    setupChain(fewIterBodies);

    PBDSolver fewIterSolver;
    fewIterSolver.init(3, 0, 2);
    fewIterSolver.setDistanceConstraints({
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = restLength},
        DistanceConstraint{.bodyA = 1, .bodyB = 2, .restLength = restLength},
    });

    SolverParams fewParams;
    fewParams.substeps = 1;
    fewParams.iterations = 1;
    fewParams.gravity = {};
    fewParams.broadphase.cellSize = 4.f;
    fewIterSolver.step(fewIterBodies, shapes, fewParams, 1.f / 60.f);
    const f32 residualFew = fewIterSolver.lastConstraintResidual();

    shapes = CollisionShapeSoA{};
    RigidBodySoA manyIterBodies;
    setupChain(manyIterBodies);

    PBDSolver manyIterSolver;
    manyIterSolver.init(3, 0, 2);
    manyIterSolver.setDistanceConstraints({
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = restLength},
        DistanceConstraint{.bodyA = 1, .bodyB = 2, .restLength = restLength},
    });

    SolverParams manyParams = fewParams;
    manyParams.iterations = 32;
    manyIterSolver.step(manyIterBodies, shapes, manyParams, 1.f / 60.f);
    const f32 residualMany = manyIterSolver.lastConstraintResidual();

    expectTrue(residualMany < residualFew, "constraint residual decreases with more iterations");
    expectTrue(residualMany <= 0.05f, "constraint residual reaches tolerance stub after enough iterations");
}

void testDistanceLambdaAccumulatesInSolver() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({2.05f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 0, 1);
    solver.setDistanceConstraints({DistanceConstraint{
        .bodyA = bodyA,
        .bodyB = bodyB,
        .restLength = 2.f,
    }});

    SolverParams params;
    params.substeps = 1;
    params.iterations = 1;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    solver.step(bodies, shapes, params, 1.f / 60.f);
    expectTrue(solver.workBuffers().distanceLambdas().size() >= 1u,
               "solver retains per-distance lambda warm-start slot");
    expectTrue(std::fabs(solver.workBuffers().distanceLambdas()[0]) > 1e-6f,
               "solver accumulates distance lambda during constraint iteration");
}

void testWarmStartLambdaFeedsAccumulation() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.05f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    const DistanceConstraint constraint{
        .bodyA = 0,
        .bodyB = 1,
        .restLength = 2.f,
    };

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const f32 dt = 1.f / 60.f;
    const f32 invMass = 1.f;

    auto runPass = [&](f32 startLambda) {
        RigidBodySoA localBodies = bodies;
        f32 lambda = startLambda;
        work.clearPositionDeltasForBodies(constraint.bodyA, constraint.bodyB);
        const f32 violation = accumulateDistanceSpringCorrection(localBodies,
                                                                 constraint,
                                                                 invMass,
                                                                 invMass,
                                                                 dt,
                                                                 lambda,
                                                                 work.positionDeltas());
        work.applyPositionDeltas(localBodies);
        const f32 dist = (localBodies.predictedPositions[0] - localBodies.predictedPositions[1]).length();
        return std::tuple<f32, f32, f32>{lambda, dist, violation};
    };

    const auto cold = runPass(0.f);
    const auto warm = runPass(0.05f);

    expectTrue(std::get<2>(cold) > 0.f, "spring correction reports constraint violation");
    expectTrue(std::fabs(std::get<0>(warm) - std::get<0>(cold)) > 1e-6f,
               "warm-start lambda seeds accumulation differently than cold start");
    expectNear(std::get<1>(warm), std::get<1>(cold), 1e-4f, "warm-start reaches same projected distance");
}

void testIslandSplitPartitionsDisconnectedSprings() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    graph.build(4, contacts, constraints);

    expectTrue(graph.constrainedIslandCount() == 2u,
               "disconnected spring pairs split into separate constrained islands");
    expectTrue(graph.islandCount() >= graph.constrainedIslandCount(),
               "total islands include optional empty body-only islands");

    const u32 islandFor01 = graph.bodyIsland(0);
    const u32 islandFor23 = graph.bodyIsland(2);
    expectTrue(islandFor01 != islandFor23, "island split keeps disconnected spring groups apart");
    expectTrue(!graph.island(islandFor01).isEmpty(), "first spring pair island carries constraints");
    expectTrue(!graph.island(islandFor23).isEmpty(), "second spring pair island carries constraints");
}

void testEmptyIslandHasNoConstraints() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    graph.build(3, contacts, constraints);

    bool foundEmpty = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (island.isEmpty()) {
            foundEmpty = true;
            expectTrue(island.bodyIndices.size() == 1u,
                       "empty island holds a lone unconstrained body");
            expectTrue(island.contactIndices.empty(), "empty island has no contacts");
            expectTrue(island.distanceIndices.empty(), "empty island has no distance constraints");
        }
    }

    expectTrue(foundEmpty, "unconstrained body yields an empty island");
    expectTrue(graph.bodyIsland(2) != graph.bodyIsland(0),
               "empty island body maps separately from constrained group");
    expectTrue(graph.constrainedIslandCount() == 1u,
               "only the connected spring pair forms a constrained island");
}

void testWarmStartLambdaSeedHelpers() {
    SolverWorkBuffers work;
    work.init(2, 1, 1);
    work.ensureLambdaCapacity(1, 1);

    const f32 dt = 1.f / 60.f;
    work.seedContactLambdaFromImpulse(0, 2.5f, dt);
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "contact lambda seeded from warm-start impulse stub");

    work.seedContactLambdaFromImpulse(0, 5.f, dt);
    expectNear(work.contactLambdas()[0], 2.5f * dt, 1e-6f,
               "contact lambda seed preserves existing warm-start value");

    work.seedDistanceLambda(0, 0.25f);
    expectNear(work.distanceLambdas()[0], 0.25f, 1e-6f, "distance lambda seeded from prior value");

    work.seedDistanceLambda(0, 0.5f);
    expectNear(work.distanceLambdas()[0], 0.25f, 1e-6f,
               "distance lambda seed preserves accumulated warm-start value");
}

void testLambdasPersistWithoutMidFrameClear() {
    SolverWorkBuffers work;
    work.init(2, 0, 1);
    work.ensureLambdaCapacity(0, 1);
    work.distanceLambda(0) = 0.42f;

    // Later substeps in PBDSolver::step skip clearLambdas(); ensureLambdaCapacity must not reset slots.
    work.ensureLambdaCapacity(0, 1);
    expectNear(work.distanceLambdas()[0], 0.42f, 1e-6f,
               "distance lambda persists across substeps when clearLambdas is not invoked");

    work.clearLambdas();
    expectNear(work.distanceLambdas()[0], 0.f, 1e-6f,
               "clearLambdas resets warm-start slots on the first substep of a frame");
}

void testApplyPositionDeltasClearsBodySlots() {
    SolverWorkBuffers work;
    work.init(2, 0, 1);

    work.positionDeltas()[0].delta = {0.1f, 0.f, 0.f};
    work.positionDeltas()[0].writeCount = 1;

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    work.applyPositionDeltas(bodies);
    expectNear(bodies.predictedPositions[0].x, 0.1f, 1e-6f, "applyPositionDeltas commits accumulated delta");
    expectTrue(work.positionDeltas()[0].writeCount == 0u,
               "applyPositionDeltas clears slot after commit for next constraint pass");
}

void testDistanceLambdaWarmStartsAcrossFrames() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({2.05f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 0, 1);
    solver.setDistanceConstraints({DistanceConstraint{
        .bodyA = bodyA,
        .bodyB = bodyB,
        .restLength = 2.f,
    }});

    SolverParams params;
    params.substeps = 1;
    params.iterations = 4;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;
    const f32 dt = 1.f / 60.f;

    solver.step(bodies, shapes, params, dt);
    const f32 lambdaAfterFirstFrame = solver.workBuffers().distanceLambdas()[0];
    expectTrue(std::fabs(lambdaAfterFirstFrame) > 1e-6f,
               "first frame accumulates distance lambda for warm-start seed");

    solver.step(bodies, shapes, params, dt);
    expectTrue(std::fabs(solver.workBuffers().distanceLambdas()[0]) > 1e-6f,
               "second frame retains distance lambda warm-start across clearLambdas");
    expectNear((bodies.positions[bodyA] - bodies.positions[bodyB]).length(),
               2.f,
               0.05f,
               "warm-started spring converges across consecutive frames");
}

void testExtractIslandFlagsEmptyAndConstrained() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    graph.build(3, contacts, constraints);

    bool foundEmptyJob = false;
    bool foundConstrainedJob = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        expectTrue(job.islandIndex == islandIndex, "extract_island records island index");
        expectTrue(job.island != nullptr, "extract_island binds island pointer");
        if (job.empty) {
            foundEmptyJob = true;
            expectTrue(job.constraintCount == 0u, "empty job reports zero constraints");
            expectTrue(!island_has_constraints(*job.island), "empty job maps to constraint-free island");
        } else {
            foundConstrainedJob = true;
            expectTrue(job.constraintCount > 0u, "constrained job reports positive constraint count");
            expectTrue(island_has_constraints(*job.island), "non-empty job maps to constrained island");
        }
    }

    expectTrue(foundEmptyJob, "extract_island surfaces empty island guard");
    expectTrue(foundConstrainedJob, "extract_island surfaces constrained island job");
    expectTrue(extract_island(graph, graph.islandCount() + 1u).empty,
               "extract_island guards out-of-range island index");
}

void testSolveIslandJobSkipsEmptyIsland() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);

    bool foundEmptyIsland = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty || job.island == nullptr) {
            continue;
        }
        foundEmptyIsland = true;
        const bool solved = solve_island_job(bodies,
                                             *job.island,
                                             work,
                                             constraints,
                                             1.f / 60.f,
                                             0.f,
                                             [](const RigidBodySoA&, u32) { return 1.f; });
        expectTrue(!solved, "solve_island_job returns false for empty islands");
    }

    expectTrue(foundEmptyIsland, "graph exposes empty island for solve guard test");
}

void testPerPairDeltaApplicationDistance() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    const DistanceConstraint constraint{
        .bodyA = 0,
        .bodyB = 1,
        .restLength = 2.f,
    };

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    f32 lambda = 0.f;
    const f32 dt = 1.f / 60.f;

    per_pair_delta_application(bodies,
                             work,
                             constraint.bodyA,
                             constraint.bodyB,
                             1.f,
                             1.f,
                             constraint,
                             dt,
                             lambda);

    const f32 dist = (bodies.predictedPositions[0] - bodies.predictedPositions[1]).length();
    expectTrue(dist < 2.1f, "per_pair_delta_application moves bodies toward rest length");
    expectTrue(std::fabs(lambda) > 1e-6f, "per_pair_delta_application accumulates distance lambda");
    expectTrue(work.positionDeltas()[0].writeCount == 0u,
               "per_pair_delta_application clears body slots after apply");
}

void testFrameLambdaWarmStartReseedsDistance() {
    SolverWorkBuffers work;
    work.init(2, 0, 2);
    work.ensureLambdaCapacity(0, 2);
    work.distanceLambda(0) = 0.33f;
    work.distanceLambda(1) = 0.11f;

    const std::vector<f32> prior = work.distanceLambdas();
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 1, .bodyB = 2, .restLength = 2.f},
    };

    work.distanceLambda(0) = 0.f;
    work.distanceLambda(1) = 0.f;
    frame_lambda_warm_start(work, constraints, prior);

    expectNear(work.distanceLambdas()[0], 0.33f, 1e-6f, "frame_lambda_warm_start reseeds first distance slot");
    expectNear(work.distanceLambdas()[1], 0.11f, 1e-6f, "frame_lambda_warm_start reseeds second distance slot");
}

void testIslandConstraintCount() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
        DistanceConstraint{.bodyA = 3, .bodyB = 4, .restLength = 2.f},
    };

    graph.build(5, contacts, constraints);

    u32 constrainedCount = 0;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        const u32 count = island_constraint_count(island);
        if (island.isEmpty()) {
            expectTrue(count == 0u, "empty island reports zero constraints");
        } else {
            expectTrue(count > 0u, "constrained island reports positive constraint count");
            constrainedCount += count;
        }
    }

    expectTrue(constrainedCount == 3u, "constraint count sums contacts and distance constraints");
}

void testExtractIslandJobsBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    expectTrue(jobs.size() == graph.islandCount(), "extract_island_jobs matches island count");

    u32 constrainedJobs = 0;
    for (const IslandSolveJob& job : jobs) {
        expectTrue(job.island != nullptr, "batch extract binds island pointer");
        if (!job.empty) {
            ++constrainedJobs;
            expectTrue(job.constraintCount == 1u, "single-spring island reports one constraint");
            expectTrue(should_solve_island(job), "constrained job passes should_solve_island");
        } else {
            expectTrue(job.constraintCount == 0u, "empty job reports zero constraints");
            expectTrue(!should_solve_island(job), "empty job fails should_solve_island");
        }
    }

    expectTrue(constrainedJobs == graph.constrainedIslandCount(),
               "batch extract surfaces all constrained islands");
}

void testShouldSolveIslandGuards() {
    IslandSolveJob invalid{};
    expectTrue(!should_solve_island(invalid), "default job is not dispatchable");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    bool foundEmptyGuard = false;
    bool foundConstrainedGuard = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            foundEmptyGuard = true;
            expectTrue(!should_solve_island(job), "empty island job is not dispatchable");
        } else {
            foundConstrainedGuard = true;
            expectTrue(should_solve_island(job), "constrained island job is dispatchable");
        }
    }

    expectTrue(foundEmptyGuard, "should_solve_island guard covers empty island");
    expectTrue(foundConstrainedGuard, "should_solve_island guard covers constrained island");
}

void testSolveIslandJobReturnsTrueForConstrained() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);

    const IslandSolveJob job = extract_island(graph, 0);
    expectTrue(should_solve_island(job), "spring island is dispatchable");
    const bool solved = solve_island_job(bodies,
                                         *job.island,
                                         work,
                                         constraints,
                                         1.f / 60.f,
                                         0.f,
                                         [](const RigidBodySoA&, u32) { return 1.f; });
    expectTrue(solved, "solve_island_job returns true for constrained island");
    const f32 dist = (bodies.predictedPositions[0] - bodies.predictedPositions[1]).length();
    expectTrue(dist < 2.1f, "constrained island solve moves bodies toward rest length");
}

void testSolveIslandJobClearsIslandBodyDeltas() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    work.positionDeltas()[0].delta = {9.f, 0.f, 0.f};
    work.positionDeltas()[0].writeCount = 1;
    work.positionDeltas()[1].delta = {-9.f, 0.f, 0.f};
    work.positionDeltas()[1].writeCount = 1;

    const IslandSolveJob job = extract_island(graph, 0);
    solve_island_job(bodies,
                     *job.island,
                     work,
                     constraints,
                     1.f / 60.f,
                     0.f,
                     [](const RigidBodySoA&, u32) { return 1.f; });

    expectTrue(work.positionDeltas()[0].writeCount == 0u,
               "solve_island_job clears island body delta slots after apply");
    expectTrue(work.positionDeltas()[1].writeCount == 0u,
               "solve_island_job clears all island body delta slots after apply");
}

void testFrameLambdaWarmStartReseedsContact() {
    SolverWorkBuffers work;
    work.init(2, 2, 1);
    work.ensureLambdaCapacity(2, 1);
    work.contactLambda(0) = 0.44f;
    work.contactLambda(1) = 0.22f;
    work.distanceLambda(0) = 0.33f;

    const std::vector<f32> priorContacts = work.contactLambdas();
    const std::vector<f32> priorDistance = work.distanceLambdas();
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    work.contactLambda(0) = 0.f;
    work.contactLambda(1) = 0.f;
    work.distanceLambda(0) = 0.f;
    frame_lambda_warm_start(work, constraints, priorDistance, priorContacts);

    expectNear(work.contactLambdas()[0], 0.44f, 1e-6f, "frame_lambda_warm_start reseeds first contact slot");
    expectNear(work.contactLambdas()[1], 0.22f, 1e-6f, "frame_lambda_warm_start reseeds second contact slot");
    expectNear(work.distanceLambdas()[0], 0.33f, 1e-6f, "frame_lambda_warm_start still reseeds distance slots");
}

void testIslandIndexValidGuard() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    expectTrue(island_index_valid(graph, 0u), "first island index is valid");
    expectTrue(island_index_valid(graph, graph.islandCount() - 1u), "last island index is valid");
    expectTrue(!island_index_valid(graph, graph.islandCount()), "at-limit island index is invalid");
    expectTrue(!island_index_valid(graph, graph.islandCount() + 5u),
               "out-of-range island index is invalid");
}

void testComputeIslandSolveStats() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
        DistanceConstraint{.bodyA = 3, .bodyB = 4, .restLength = 2.f},
    };
    graph.build(6, contacts, constraints);

    const IslandSolveStats stats = compute_island_solve_stats(graph);
    expectTrue(stats.totalIslands == graph.islandCount(), "stats report total island count");
    expectTrue(stats.constrainedCount == graph.constrainedIslandCount(),
               "stats constrained count matches graph");
    expectTrue(stats.emptyCount + stats.constrainedCount == stats.totalIslands,
               "empty and constrained counts partition total islands");
    expectTrue(stats.dispatchableCount == stats.constrainedCount,
               "all constrained islands are dispatchable");
    expectTrue(count_dispatchable_islands(graph) == stats.dispatchableCount,
               "count_dispatchable_islands matches stats");
}

void testHasDispatchableIslandsEarlyOut() {
    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(!has_dispatchable_islands(emptyGraph), "empty graph has no dispatchable islands");

    ContactIslandGraph loneBodies;
    loneBodies.build(2, {}, {});
    expectTrue(!has_dispatchable_islands(loneBodies),
               "lone unconstrained bodies yield no dispatchable islands");

    ContactIslandGraph constrained;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    constrained.build(3, contacts, constraints);
    expectTrue(has_dispatchable_islands(constrained),
               "mixed empty and constrained graph has dispatchable islands");
}

void testDispatchSolveIslandGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    expectTrue(!dispatch_solve_island(bodies,
                                      graph,
                                      graph.islandCount() + 1u,
                                      work,
                                      constraints,
                                      1.f / 60.f,
                                      0.f,
                                      invMassFn),
               "dispatch_solve_island guards out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        foundEmptySkip = !dispatch_solve_island(bodies,
                                                graph,
                                                islandIndex,
                                                work,
                                                constraints,
                                                1.f / 60.f,
                                                0.f,
                                                invMassFn);
        break;
    }
    expectTrue(foundEmptySkip, "dispatch_solve_island skips empty islands");

    expectTrue(dispatch_solve_island(bodies,
                                     graph,
                                     graph.bodyIsland(0),
                                     work,
                                     constraints,
                                     1.f / 60.f,
                                     0.f,
                                     invMassFn),
              "dispatch_solve_island resolves constrained island");
}

void testWarmStartIslandLambdasSelective() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    const u32 islandB = graph.bodyIsland(2);
    warm_start_island_lambdas(work, graph.island(islandA), priorDistance, priorContact);

    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "island warm-start seeds owned distance slot");
    expectNear(work.distanceLambdas()[1], 0.f, 1e-6f,
               "island warm-start skips remote distance slot");
    expectNear(work.contactLambdas()[0], 0.33f, 1e-6f,
               "island warm-start seeds owned contact slot");
    expectNear(work.contactLambdas()[1], 0.f, 1e-6f,
               "island warm-start skips remote contact slot");

    work.clearLambdas();
    warm_start_island_lambdas(work, graph.island(islandB), priorDistance, priorContact);
    expectNear(work.distanceLambdas()[1], 0.22f, 1e-6f,
               "second island warm-start seeds its distance slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f,
               "second island warm-start seeds its contact slot");
}

void testWarmStartIslandContactImpulses() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const f32 dt = 1.f / 60.f;
    warm_start_island_contact_impulses(work, graph.island(graph.bodyIsland(0)), contacts, dt);
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "island impulse warm-start seeds non-zero contact lambda");
    expectNear(work.contactLambdas()[1], 0.f, 1e-6f,
               "island impulse warm-start skips zero-impulse remote contact");
}

void testPreflightIslandSolveSkipsEmptyGraph() {
    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});

    const IslandSolvePreflight preflight = preflight_island_solve(emptyGraph);
    expectTrue(preflight.skipped, "preflight marks empty graph as skipped");
    expectTrue(!preflight.can_dispatch(), "preflight cannot dispatch on empty graph");
    expectTrue(should_skip_island_solve(emptyGraph), "should_skip_island_solve on empty graph");
    expectTrue(all_islands_empty(emptyGraph), "all_islands_empty on zero islands");

    ContactIslandGraph loneBodies;
    loneBodies.build(2, {}, {});
    const IslandSolvePreflight lonePreflight = preflight_island_solve(loneBodies);
    expectTrue(lonePreflight.skipped, "preflight skips lone unconstrained bodies");
    expectTrue(all_islands_empty(loneBodies), "all islands empty when no constraints exist");
    expectTrue(collect_dispatchable_island_indices(loneBodies).empty(),
               "collect_dispatchable_island_indices empty for lone bodies");
}

void testPreflightIslandSolveDispatchesConstrained() {
void testShouldSkipIslandSolveGuards() {
    IslandSolveJob invalid{};
    expectTrue(should_skip_island_solve(invalid), "default job is skipped");
    expectTrue(!should_solve_island(invalid), "should_skip mirrors should_solve");

void testPreflightIslandSolveGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            expectTrue(should_skip_island_solve(job), "empty island job is skipped");
        } else {
            expectTrue(!should_skip_island_solve(job), "constrained island job is not skipped");

    const IslandSolveJob outOfRange = extract_island(graph, graph.islandCount() + 2u);
    expectTrue(should_skip_island_solve(outOfRange), "out-of-range island job is skipped");

void testCollectDispatchableIslandIndices() {
    expectTrue(!preflight_island_solve(graph, graph.islandCount() + 1u),
               "preflight_island_solve rejects out-of-range index");

    bool foundEmptyPreflight = false;
    bool foundConstrainedPreflight = false;
        const bool ready = preflight_island_solve(graph, islandIndex);
            foundEmptyPreflight = true;
            expectTrue(!ready, "preflight_island_solve rejects empty island");
            foundConstrainedPreflight = true;
            expectTrue(ready, "preflight_island_solve accepts constrained island");
            expectTrue(ready == should_solve_island(job),
                       "preflight_island_solve matches should_solve_island");
        }

    expectTrue(foundEmptyPreflight, "preflight_island_solve covers empty island");
    expectTrue(foundConstrainedPreflight, "preflight_island_solve covers constrained island");

void testFilterDispatchableJobs() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const IslandSolvePreflight preflight = preflight_island_solve(graph);
    expectTrue(!preflight.skipped, "preflight does not skip constrained graph");
    expectTrue(preflight.can_dispatch(), "preflight can dispatch constrained islands");
    expectTrue(preflight.stats.dispatchableCount == graph.constrainedIslandCount(),
               "preflight dispatchable count matches constrained islands");
    expectTrue(!should_skip_island_solve(graph), "should_skip false when islands are dispatchable");
    expectTrue(!all_islands_empty(graph), "not all islands empty in mixed graph");

    const std::vector<u32> indices = collect_dispatchable_island_indices(graph);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect_dispatchable_island_indices returns constrained count");
    for (u32 islandIndex : indices) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        expectTrue(should_solve_island(job), "collected index passes should_solve_island");
    }

void testDispatchSolveIslandResultOutcomes() {
               "dispatchable indices match constrained island count");

        expectTrue(should_solve_island(job), "collected index is dispatchable");
        expectTrue(!should_skip_island_solve(job), "collected index is not skipped");

    const std::vector<IslandSolveJob> jobs = collect_dispatchable_island_jobs(graph);
    expectTrue(jobs.size() == indices.size(), "dispatchable jobs match index collection");
    for (const IslandSolveJob& job : jobs) {
        expectTrue(should_solve_island(job), "collected job is dispatchable");
        expectTrue(job.constraintCount > 0u, "dispatchable job carries constraints");

void testAllIslandsEmptyFastPath() {
    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(all_islands_empty(emptyGraph), "zero-body graph is all empty");

    ContactIslandGraph loneBodies;
    loneBodies.build(3, {}, {});
    expectTrue(all_islands_empty(loneBodies), "lone bodies without constraints are all empty");
    expectTrue(should_skip_all_island_solves(loneBodies),
               "should_skip_all_island_solves for lone unconstrained bodies");

    ContactIslandGraph constrained;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    constrained.build(3, contacts, constraints);
    expectTrue(!all_islands_empty(constrained), "mixed graph is not all empty");
    expectTrue(!should_skip_all_island_solves(constrained),
               "constrained graph should not skip all solves");

void testIslandSolveStatsHasWork() {
    ContactIslandGraph graph;
    graph.build(2, {}, {});
    const IslandSolveStats emptyStats = compute_island_solve_stats(graph);
    expectTrue(!island_solve_stats_has_work(emptyStats),
               "stats without dispatchable islands report no work");
    expectTrue(should_skip_all_island_solves(graph), "skip-all mirrors stats no-work");

    graph.build(2, {}, constraints);
    const IslandSolveStats stats = compute_island_solve_stats(graph);
    expectTrue(island_solve_stats_has_work(stats), "stats with constrained island report work");
    expectTrue(stats.dispatchableCount == collect_dispatchable_island_indices(graph).size(),
               "stats dispatchable count matches collected indices");

void testDispatchSolveIslandResult() {
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandDispatchResult outOfRange = dispatch_solve_island_result(bodies,
                                                                         graph,
                                                                         graph.islandCount() + 2u,
                                                                         work,
                                                                         constraints,
                                                                         1.f / 60.f,
                                                                         0.f,
                                                                         invMassFn);
    expectTrue(outOfRange.skipped, "out-of-range dispatch result is skipped");
    expectTrue(!outOfRange.solved, "out-of-range dispatch result is not solved");
    const IslandSolveDispatchResult outOfRange = dispatch_solve_island_result(bodies,
                                                                              graph.islandCount() + 1u,
    expectTrue(!outOfRange.solved, "out-of-range dispatch result does not solve");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (!job.empty) {
            continue;
        const IslandDispatchResult emptyResult = dispatch_solve_island_result(bodies,
                                                                              islandIndex,
        expectTrue(emptyResult.skipped, "empty island dispatch result is skipped");
        expectTrue(!emptyResult.solved, "empty island dispatch result is not solved");
        foundEmptySkip = true;
        break;
    expectTrue(foundEmptySkip, "dispatch_solve_island_result covers empty island skip");

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandDispatchResult solved = dispatch_solve_island_result(bodies,
                                                                     constrainedIndex,
    expectTrue(solved.solved, "constrained island dispatch result is solved");
    expectTrue(!solved.skipped, "constrained island dispatch result is not skipped");
    expectTrue(solved.islandIndex == constrainedIndex, "dispatch result records island index");

void testDispatchAllIslandsBatch() {
    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    const std::vector<IslandSolveJob> dispatchable = filter_dispatchable_jobs(jobs);

    expectTrue(dispatchable.size() == graph.constrainedIslandCount(),
               "filter_dispatchable_jobs keeps only constrained islands");
    for (const IslandSolveJob& job : dispatchable) {
        expectTrue(should_solve_island(job), "filtered job passes should_solve_island");
        expectTrue(!job.empty, "filtered job is non-empty");

void testDispatchSolveAllIslands() {
    work.init(0, 0, 0);
    expectTrue(dispatch_solve_all_islands(bodies,
                                          emptyGraph,
                                          {},
                                          invMassFn) == 0u,
               "dispatch_solve_all_islands early-outs on empty graph");

    loneBodies.build(2, {}, {});
    work.init(2, 0, 0);
                                          loneBodies,
               "dispatch_solve_all_islands early-outs when no islands are dispatchable");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    graph.build(5, contacts, constraints);

    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const u32 solvedCount = dispatch_all_islands(bodies,
    expectTrue(solvedCount == graph.constrainedIslandCount(),
               "dispatch_all_islands solves all constrained islands");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(dispatch_all_islands(bodies,
                                    emptyGraph,
                                    invMassFn) == 0u,
               "dispatch_all_islands early-outs on empty graph");

void testPreflightWarmStartIslandGuards() {
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        const IslandWarmStartPreflight preflight =
            preflight_warm_start_island(island, priorDistance, priorContact);
        expectTrue(preflight.skipped, "preflight skips empty island warm-start");
        expectTrue(!preflight.can_warm_start(), "empty island cannot warm-start");
        expectTrue(should_skip_warm_start_island(island), "should_skip_warm_start_island on empty island");
    expectTrue(foundEmptySkip, "graph exposes empty island for warm-start preflight");

    const u32 islandA = graph.bodyIsland(0);
    const IslandWarmStartPreflight constrainedPreflight =
        preflight_warm_start_island(graph.island(islandA), priorDistance, priorContact);
    expectTrue(!constrainedPreflight.skipped, "preflight does not skip constrained island");
    expectTrue(constrainedPreflight.ownedDistanceCount == 1u,
               "preflight counts owned distance slots");
    expectTrue(constrainedPreflight.ownedContactCount == 1u,
               "preflight counts owned contact slots");
    expectTrue(constrainedPreflight.priorDistanceCoverage == 1u,
               "preflight counts prior distance coverage");
    expectTrue(constrainedPreflight.priorContactCoverage == 1u,
               "preflight counts non-zero prior contact coverage");
    expectTrue(constrainedPreflight.can_warm_start(), "constrained island can warm-start");

void testWarmStartIslandLambdasGuarded() {
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);


    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.15f};
    const std::vector<f32> priorContact = {0.25f};

    work.clearLambdas();
        foundEmptySkip = !warm_start_island_lambdas_guarded(work, island, priorDistance, priorContact);
    expectTrue(foundEmptySkip, "guarded warm-start skips empty island");
    expectNear(work.distanceLambdas()[0], 0.f, 1e-6f,
               "guarded skip leaves distance lambda untouched");

    expectTrue(warm_start_island_lambdas_guarded(work,
                                                 graph.island(islandA),
                                                 priorDistance,
                                                 priorContact),
               "guarded warm-start succeeds for constrained island");
    expectNear(work.distanceLambdas()[0], 0.15f, 1e-6f,
               "guarded warm-start seeds owned distance slot");
    expectNear(work.contactLambdas()[0], 0.25f, 1e-6f,
               "guarded warm-start seeds owned contact slot");

    expectTrue(!warm_start_island_lambdas_guarded(work,
                                                  {},
                                                  {}),
               "guarded warm-start skips when no prior data exists");

void testWarmStartIslandContactImpulsesGuarded() {
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    contacts.back().warmNormalImpulse = 4.f;

    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
        foundEmptySkip = !warm_start_island_contact_impulses_guarded(work, island, contacts, dt);
    expectTrue(foundEmptySkip, "guarded impulse warm-start skips empty island");

    expectTrue(warm_start_island_contact_impulses_guarded(work, graph.island(islandA), contacts, dt),
               "guarded impulse warm-start succeeds for contact island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "guarded impulse warm-start seeds contact lambda");

void testIsValidIslandSolveDtGuard() {
    expectTrue(is_valid_island_solve_dt(1.f / 60.f), "positive dt is valid for island solve");
    expectTrue(!is_valid_island_solve_dt(0.f), "zero dt is invalid for island solve");
    expectTrue(!is_valid_island_solve_dt(-1.f / 60.f), "negative dt is invalid for island solve");

void testPreflightIslandDispatchGuardsDt() {
    graph.build(2, contacts, constraints);

    const IslandDispatchPreflight validPreflight = preflight_island_dispatch(graph, 1.f / 60.f);
    expectTrue(!validPreflight.invalidDt, "valid dt passes dispatch preflight");
    expectTrue(validPreflight.can_dispatch(), "constrained graph can dispatch with valid dt");

    const IslandDispatchPreflight invalidPreflight = preflight_island_dispatch(graph, 0.f);
    expectTrue(invalidPreflight.invalidDt, "zero dt fails dispatch preflight");
    expectTrue(!invalidPreflight.can_dispatch(), "dispatch blocked when dt is invalid");
    expectTrue(should_skip_island_dispatch(graph, 0.f), "should_skip_island_dispatch on invalid dt");
    expectTrue(!should_skip_island_dispatch(graph, 1.f / 60.f),
               "should_skip false for constrained graph with valid dt");

void testDispatchSolveIslandResultSkipsInvalidDt() {


    work.init(2, 0, 1);

    const IslandDispatchResult invalidDt = dispatch_solve_island_result(bodies,
                                                                        0u,
    expectTrue(invalidDt.skipped, "invalid dt dispatch result is skipped");
    expectTrue(!invalidDt.solved, "invalid dt dispatch result is not solved");
    expectTrue(!dispatch_solve_island(bodies,
                                       invMassFn),
               "dispatch_solve_island guards invalid dt");

void testDispatchAllIslandsResultBatch() {



    const IslandBatchDispatchResult batch = dispatch_all_islands_result(bodies,
    expectTrue(!batch.skipped, "batch dispatch does not skip constrained graph");
    expectTrue(batch.dispatchableCount == graph.constrainedIslandCount(),
               "batch dispatch records dispatchable count");
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "batch dispatch solves all constrained islands");
    expectTrue(batch.any_solved(), "batch dispatch reports solved islands");

    const IslandBatchDispatchResult emptyBatch = dispatch_all_islands_result(bodies,
    expectTrue(emptyBatch.skipped, "batch dispatch skips empty graph");
    expectTrue(!emptyBatch.any_solved(), "empty graph batch reports no solved islands");

    const IslandBatchDispatchResult invalidDtBatch = dispatch_all_islands_result(bodies,
    expectTrue(invalidDtBatch.skipped, "batch dispatch skips invalid dt");
    expectTrue(invalidDtBatch.solvedCount == 0u, "invalid dt batch solves zero islands");

void testPreflightFrameWarmStartGuards() {
    const std::vector<DistanceConstraint> constraints = {
    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.34f, 0.f};

    const FrameWarmStartPreflight preflight =
        preflight_frame_lambda_warm_start(constraints, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "frame preflight does not skip when prior data exists");
    expectTrue(preflight.can_warm_start(), "frame preflight can warm-start with prior data");
    expectTrue(preflight.distanceSlotCount == 1u, "frame preflight counts distance slots");
    expectTrue(preflight.contactSlotCount == 2u, "frame preflight counts contact slots");
    expectTrue(preflight.priorDistanceCoverage == 1u, "frame preflight counts prior distance coverage");
    expectTrue(preflight.priorContactCoverage == 1u,
               "frame preflight counts non-zero prior contact coverage");

    const FrameWarmStartPreflight emptyPrior =
        preflight_frame_lambda_warm_start(constraints, {}, {});
    expectTrue(emptyPrior.skipped, "frame preflight skips when no prior data exists");
    expectTrue(!emptyPrior.can_warm_start(), "frame preflight cannot warm-start without prior data");
    expectTrue(should_skip_frame_warm_start({}, {}), "should_skip_frame_warm_start on empty priors");
    expectTrue(!should_skip_frame_warm_start(priorDistance, priorContact),
               "should_skip false when prior data exists");

void testFrameLambdaWarmStartGuarded() {
    work.init(2, 2, 1);
    work.ensureLambdaCapacity(2, 1);
    work.distanceLambda(0) = 0.42f;
    work.contactLambda(0) = 0.24f;

    const std::vector<f32> priorDistance = work.distanceLambdas();
    const std::vector<f32> priorContact = work.contactLambdas();

    work.distanceLambda(0) = 0.99f;
    work.contactLambda(0) = 0.99f;
    expectTrue(!frame_lambda_warm_start_guarded(work, constraints, {}, {}),
               "guarded frame warm-start skips when no prior data exists");
               "guarded skip still clears distance lambda slots");

    expectTrue(frame_lambda_warm_start_guarded(work, constraints, priorDistance, priorContact),
               "guarded frame warm-start succeeds with prior data");
    expectNear(work.distanceLambdas()[0], 0.42f, 1e-6f,
               "guarded frame warm-start reseeds distance slot");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "guarded frame warm-start reseeds contact slot");

void testWarmStartGraphLambdasGuarded() {

    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;


    const std::vector<f32> priorContact = {0.33f, 0.44f};

    const u32 warmedCount =
        warm_start_graph_lambdas_guarded(work, graph, priorDistance, priorContact);
    expectTrue(warmedCount == graph.constrainedIslandCount(),
               "graph warm-start seeds all constrained islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "graph warm-start seeds first distance slot");
    expectNear(work.distanceLambdas()[1], 0.22f, 1e-6f,
               "graph warm-start seeds second distance slot");
    expectNear(work.contactLambdas()[0], 0.33f, 1e-6f,
               "graph warm-start seeds first contact slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f,
               "graph warm-start seeds second contact slot");

    expectTrue(warm_start_graph_lambdas_guarded(work, graph, {}, {}) == 0u,
               "graph warm-start skips all islands when no prior data exists");

void testShouldSkipIslandSolveJobGuard() {
    IslandSolveJob invalid{};
    expectTrue(should_skip_island_solve_job(invalid), "default job is skipped");
    expectTrue(!should_solve_island(invalid), "should_skip mirrors should_solve inverse");


    bool foundConstrainedDispatch = false;
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            foundEmptySkip = should_skip_island_solve_job(job);
        } else {
            foundConstrainedDispatch = !should_skip_island_solve_job(job);

    expectTrue(foundEmptySkip, "should_skip_island_solve_job covers empty island");
    expectTrue(foundConstrainedDispatch, "should_skip_island_solve_job allows constrained island");

void testShouldDispatchIslandIndexGuard() {

    expectTrue(!should_dispatch_island_index(graph, graph.islandCount() + 1u),
               "should_dispatch_island_index guards out-of-range index");

        if (extract_island(graph, islandIndex).empty) {
            foundEmptySkip = !should_dispatch_island_index(graph, islandIndex);
            foundConstrainedDispatch = should_dispatch_island_index(graph, islandIndex);

    expectTrue(foundEmptySkip, "should_dispatch_island_index skips empty island");
    expectTrue(foundConstrainedDispatch, "should_dispatch_island_index allows constrained island");

void testCollectDispatchableIslandJobs() {

    const std::vector<IslandSolveJob> allJobs = extract_island_jobs(graph);
    const std::vector<IslandSolveJob> filtered = filter_dispatchable_jobs(allJobs);
    const std::vector<IslandSolveJob> collected = collect_dispatchable_island_jobs(graph);

    expectTrue(filtered.size() == graph.constrainedIslandCount(),
               "filter_dispatchable_jobs keeps constrained islands only");
    expectTrue(collected.size() == filtered.size(),
               "collect_dispatchable_island_jobs matches filtered batch");
    for (const IslandSolveJob& job : collected) {
        expectTrue(should_solve_island(job), "collected job passes should_solve_island");
        expectTrue(!should_skip_island_solve_job(job), "collected job is not skipped");

void testPreflightWarmStartGraphGuards() {



    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "graph preflight does not skip when warm-startable islands exist");
    expectTrue(preflight.can_warm_start(), "graph preflight can warm-start with prior data");
    expectTrue(preflight.stats.warmStartableCount == graph.constrainedIslandCount(),
               "graph preflight counts warm-startable islands");
    expectTrue(preflight.stats.emptyCount + preflight.stats.warmStartableCount +
                       preflight.stats.noPriorDataCount ==
                   preflight.stats.totalIslands,
               "warm-start stats partition total islands");
    expectTrue(count_warm_startable_islands(graph, priorDistance, priorContact) ==
                   preflight.stats.warmStartableCount,
               "count_warm_startable_islands matches stats");
    expectTrue(has_warm_startable_islands(graph, priorDistance, priorContact),
               "has_warm_startable_islands true when prior data exists");
    expectTrue(!should_skip_warm_start_graph(graph, priorDistance, priorContact),
               "should_skip_warm_start_graph false when islands can seed");

    const std::vector<u32> indices = collect_warm_startable_island_indices(graph, priorDistance, priorContact);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect_warm_startable_island_indices returns constrained count");

    expectTrue(should_skip_warm_start_graph(graph, {}, {}), "graph warm-start skipped without prior data");
    expectTrue(preflight_warm_start_graph(graph, {}, {}).skipped,
               "graph preflight skipped without prior data");

void testPreflightWarmStartIslandByIndex() {


    const IslandWarmStartPreflight outOfRange =
        preflight_warm_start_island_by_index(graph, graph.islandCount() + 1u, priorDistance, priorContact);
    expectTrue(outOfRange.skipped, "index preflight skips out-of-range island");
    expectTrue(should_skip_warm_start_island_index(graph, graph.islandCount() + 1u),
               "should_skip_warm_start_island_index on out-of-range index");

        const IslandWarmStartPreflight emptyPreflight =
            preflight_warm_start_island_by_index(graph, islandIndex, priorDistance, priorContact);
        expectTrue(emptyPreflight.skipped, "index preflight skips empty island");
        expectTrue(should_skip_warm_start_island_index(graph, islandIndex),
                   "should_skip_warm_start_island_index on empty island");
    expectTrue(foundEmptySkip, "graph exposes empty island for index warm-start preflight");

        preflight_warm_start_island_by_index(graph, constrainedIndex, priorDistance, priorContact);
    expectTrue(!constrainedPreflight.skipped, "index preflight does not skip constrained island");
    expectTrue(constrainedPreflight.can_warm_start(), "index preflight can warm-start constrained island");

void testWarmStartIslandLambdasResultAndBatch() {




    const IslandWarmStartResult outOfRange =
        warm_start_island_lambdas_result(work, graph, graph.islandCount() + 3u, priorDistance, priorContact);
    expectTrue(outOfRange.skipped, "warm_start result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range warm_start result is not warmed");

    const u32 warmedCount = warm_start_all_islands_guarded(work, graph, priorDistance, priorContact);
               "warm_start_all_islands_guarded seeds all constrained islands");
               "batch warm-start seeds first distance slot");
               "batch warm-start seeds second contact slot");

    expectTrue(warm_start_island_lambdas_by_index_guarded(work,
               "index guarded warm-start succeeds for constrained island");
               "index guarded warm-start seeds owned distance slot");

void testWarmStartIslandCombinedGuarded() {
    work.init(4, 2, 1);

    contacts.back().warmNormalImpulse = 3.5f;


    const std::vector<f32> priorContact = {0.24f};

        foundEmptySkip = !warm_start_island_combined_guarded(work,
                                                             island,
                                                             contacts,
                                                             dt,
                                                             priorContact);
    expectTrue(foundEmptySkip, "combined warm-start skips empty island");

    expectTrue(warm_start_island_combined_guarded(work,
               "combined warm-start succeeds for contact island with prior data");
               "combined warm-start seeds prior contact lambda");
               "combined warm-start retains non-zero contact lambda from impulse seed");

void testIsValidWarmStartDtGuard() {
    expectTrue(is_valid_warm_start_dt(1.f / 60.f), "positive dt is valid for warm-start");
    expectTrue(!is_valid_warm_start_dt(0.f), "zero dt is invalid for warm-start");
    expectTrue(is_valid_warm_start_dt(1.f / 60.f) == is_valid_island_solve_dt(1.f / 60.f),
               "warm-start dt guard matches island solve dt guard");

void testDispatchSolveIslandJobGuards() {

    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    work.init(3, 0, 1);

    expectTrue(!dispatch_solve_island_job(bodies, invalid, work, constraints, dt, 0.f, invMassFn),
               "dispatch_solve_island_job skips default job");

    const IslandDispatchResult invalidJobResult =
        dispatch_solve_island_job_result(bodies, invalid, work, constraints, 0.f, 0.f, invMassFn);
    expectTrue(invalidJobResult.skipped, "job dispatch result skips invalid dt");

    bool foundConstrainedSolve = false;
    for (const IslandSolveJob& job : extract_island_jobs(graph)) {
            foundEmptySkip = !dispatch_solve_island_job(bodies, job, work, constraints, dt, 0.f, invMassFn);
            foundConstrainedSolve = dispatch_solve_island_job(bodies, job, work, constraints, dt, 0.f, invMassFn);
    expectTrue(foundEmptySkip, "dispatch_solve_island_job skips empty island job");
    expectTrue(foundConstrainedSolve, "dispatch_solve_island_job solves constrained island job");

void testWarmStartAllIslandsResultBatch() {




    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "batch warm-start does not skip constrained graph");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "batch warm-start records warm-startable count");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "batch warm-start seeds all constrained islands");
    expectTrue(batch.any_warmed(), "batch warm-start reports warmed islands");

    const IslandBatchWarmStartResult emptyPrior =
        warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyPrior.skipped, "batch warm-start skips when no prior data exists");
    expectTrue(!emptyPrior.any_warmed(), "empty prior batch reports no warmed islands");

void testPreflightWarmStartContactImpulsesGuards() {
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.back().warmNormalImpulse = 0.f;

    graph.build(5, contacts, {});

    const IslandContactImpulseWarmStartPreflight constrainedPreflight =
        preflight_warm_start_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(!constrainedPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(!constrainedPreflight.invalidDt, "impulse preflight accepts valid dt");
    expectTrue(constrainedPreflight.ownedContactCount == 1u, "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.nonZeroImpulseCount == 1u,
               "impulse preflight counts non-zero impulses");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can warm-start impulses");

    const IslandContactImpulseWarmStartPreflight invalidDtPreflight =
        preflight_warm_start_contact_impulses(graph.island(islandA), contacts, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "impulse preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "impulse preflight cannot warm-start with invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(islandA), 0.f),
               "should_skip_warm_start_contact_impulses on invalid dt");

    const IslandContactImpulseWarmStartPreflight outOfRange =
        preflight_warm_start_contact_impulses_by_index(graph, graph.islandCount() + 1u, contacts, dt);
    expectTrue(outOfRange.skipped, "index impulse preflight skips out-of-range island");
    expectTrue(should_skip_warm_start_contact_impulses_index(graph, graph.islandCount() + 1u, dt),
               "should_skip_warm_start_contact_impulses_index on out-of-range index");

        const IslandContactImpulseWarmStartPreflight emptyPreflight =
            preflight_warm_start_contact_impulses(island, contacts, dt);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_warm_start_contact_impulses(island, dt),
                   "should_skip_warm_start_contact_impulses on empty island");
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");

void testWarmStartContactImpulsesResultAndBatch() {

    contacts.back().warmNormalImpulse = 3.f;

    graph.build(4, contacts, {});

        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 2u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");

    const IslandWarmStartResult invalidDt =
        warm_start_island_contact_impulses_result(work, graph, graph.bodyIsland(0), contacts, 0.f);
    expectTrue(invalidDt.skipped, "impulse result skips invalid dt");
    expectTrue(!warm_start_island_contact_impulses_guarded(work, graph.island(graph.bodyIsland(0)), contacts, 0.f),
               "guarded impulse warm-start skips invalid dt");

    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, islandA, contacts, dt),
               "index guarded impulse warm-start succeeds for contact island");
               "index guarded impulse warm-start seeds contact lambda");

        warm_start_all_islands_contact_impulses_result(work, graph, contacts, dt);
    expectTrue(!batch.skipped, "impulse batch does not skip contact graph");
    expectTrue(batch.warmedCount == 1u, "impulse batch seeds only non-zero impulse island");
    expectTrue(batch.any_warmed(), "impulse batch reports warmed island");

    expectTrue(warm_start_all_islands_contact_impulses_guarded(work, graph, contacts, dt) == 1u,
               "impulse batch guarded count matches warmed islands");

    const IslandBatchWarmStartResult invalidDtBatch =
        warm_start_all_islands_contact_impulses_result(work, graph, contacts, 0.f);
    expectTrue(invalidDtBatch.skipped, "impulse batch skips invalid dt");

void testPreflightWarmStartCombinedIsland() {

        const IslandSolveDispatchResult emptyResult = dispatch_solve_island_result(bodies,
        foundEmptySkip = emptyResult.skipped && !emptyResult.solved;
        expectTrue(emptyResult.islandIndex == islandIndex, "empty result records island index");
    expectTrue(foundEmptySkip, "dispatch result skips empty islands");

    const IslandSolveDispatchResult solved = dispatch_solve_island_result(bodies,
    expectTrue(!solved.skipped, "constrained dispatch result is not skipped");
    expectTrue(solved.solved, "constrained dispatch result solves island");
    expectTrue(solved.islandIndex == constrainedIndex, "solved result records island index");

void testPreflightWarmStartIsland() {
    work.init(4, 2, 2);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;


    const IslandCombinedWarmStartPreflight preflight =
        preflight_warm_start_combined_island(graph.island(islandA), contacts, dt, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "combined preflight does not skip contact island");
    expectTrue(preflight.lambdas.can_warm_start(), "combined preflight sees prior lambda data");
    expectTrue(preflight.impulses.can_warm_start(), "combined preflight sees non-zero impulses");
    expectTrue(preflight.can_warm_start(), "combined preflight can warm-start");
    bodies.addBody({40.f, 0.f, 0.f}, 1.f, 0);

    work.init(5, 0, 2);
    const u32 solvedCount = dispatch_solve_all_islands(bodies,
                                                       graph,
                                                       work,
                                                       constraints,
                                                       1.f / 60.f,
                                                       0.f,
                                                       invMassFn);
               "dispatch_solve_all_islands resolves all constrained islands");
    expectTrue(solvedCount == filter_dispatchable_jobs(extract_island_jobs(graph)).size(),
               "dispatch_solve_all_islands count matches filtered job count");
}

void testWarmStartPreflightGuards() {
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    bool foundEmptyWarmStartGuard = false;
        if (island.isEmpty()) {
            foundEmptyWarmStartGuard = true;
            expectTrue(!should_warm_start_island(island),
                       "should_warm_start_island rejects empty island");
            expectTrue(should_warm_start_island(island),
                       "should_warm_start_island accepts constrained island");
    expectTrue(foundEmptyWarmStartGuard, "warm-start preflight covers empty island");

    expectTrue(!has_prior_lambda_warm_start({}, {}),
               "has_prior_lambda_warm_start rejects empty prior buffers");
    expectTrue(has_prior_lambda_warm_start({0.f}, {}),
               "has_prior_lambda_warm_start accepts distance prior buffer");
    expectTrue(has_prior_lambda_warm_start({}, {0.f}),
               "has_prior_lambda_warm_start accepts contact prior buffer");

    expectTrue(!preflight_frame_lambda_warm_start({}, {}, {}),
               "preflight_frame_lambda_warm_start rejects empty inputs");
    expectTrue(preflight_frame_lambda_warm_start(constraints, {0.1f}, {}),
               "preflight_frame_lambda_warm_start accepts distance warm-start");
    expectTrue(preflight_frame_lambda_warm_start({}, {}, {0.2f}),
               "preflight_frame_lambda_warm_start accepts contact-only warm-start");

void testFrameLambdaWarmStartPreflightSkip() {
    work.ensureLambdaCapacity(0, 1);

    frame_lambda_warm_start(work, {}, {}, {});
               "frame_lambda_warm_start preflight skip preserves existing lambda slots");

void testWarmStartIslandLambdasSkipsEmptyIsland() {


    const std::vector<f32> priorDistance = {0.11f};
    const std::vector<f32> priorContact = {0.33f};

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandCombinedWarmStartPreflight emptyPreflight =
            preflight_warm_start_combined_island(island, contacts, dt, priorDistance, priorContact);
        expectTrue(emptyPreflight.skipped, "combined preflight skips empty island");
        expectTrue(!emptyPreflight.can_warm_start(), "empty island cannot combined warm-start");
    expectTrue(foundEmptySkip, "graph exposes empty island for combined preflight");

    work.clearLambdas();
    const IslandWarmStartResult combinedResult =
        warm_start_island_combined_result(work, graph, islandA, contacts, dt, priorDistance, priorContact);
    expectTrue(combinedResult.warmed, "combined result warms contact island");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "combined result seeds prior contact lambda");

    const u32 warmedCount =
        warm_start_all_islands_combined_guarded(work, graph, contacts, dt, priorDistance, priorContact);
    expectTrue(warmedCount == 1u, "combined graph batch warms contact island only");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "combined graph batch seeds contact island");

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "island build preflight skips zero bodies");
    expectTrue(zeroBodies.zeroBodies, "island build preflight flags zero bodies");
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodies,
               "island build reject reason is ZeroBodies");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip_island_build on zero bodies");
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
               "island_build_rejects_for_reason matches ZeroBodies");

    const IslandBuildPreflight noConstraints = preflight_island_build(3, {}, {});
    expectTrue(!noConstraints.skipped, "island build preflight does not skip lone bodies");
    expectTrue(noConstraints.noConstraints, "island build preflight flags no constraints");
    expectTrue(noConstraints.reason == IslandBuildRejectReason::NoConstraints,
               "island build reject reason is NoConstraints");
    expectTrue(noConstraints.stats.constraintEdgeCount == 0u,
               "island build stats report zero constraint edges");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    const IslandBuildPreflight constrained = preflight_island_build(2, contacts, constraints);
    expectTrue(!constrained.skipped, "island build preflight does not skip constrained scene");
    expectTrue(constrained.reason == IslandBuildRejectReason::None,
               "island build reject reason is None for constrained scene");
    expectTrue(constrained.stats.validContactCount == 1u,
               "island build stats count valid contacts");
    expectTrue(constrained.stats.constraintEdgeCount == 2u,
               "island build stats sum contacts and distance constraints");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "island build preflight skips zero bodies");
    expectTrue(zeroBodies.zeroBodies, "island build preflight flags zero bodies");
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodies,
               "island build reject reason is ZeroBodies");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip_island_build on zero bodies");
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
               "island_build_rejects_for_reason matches ZeroBodies");

    const IslandBuildPreflight noConstraints = preflight_island_build(3, {}, {});
    expectTrue(!noConstraints.skipped, "island build preflight does not skip lone bodies");
    expectTrue(noConstraints.noConstraints, "island build preflight flags no constraints");
    expectTrue(noConstraints.reason == IslandBuildRejectReason::NoConstraints,
               "island build reject reason is NoConstraints");
    expectTrue(noConstraints.stats.constraintEdgeCount == 0u,
               "island build stats report zero constraint edges");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    const IslandBuildPreflight constrained = preflight_island_build(2, contacts, constraints);
    expectTrue(!constrained.skipped, "island build preflight does not skip constrained scene");
    expectTrue(constrained.reason == IslandBuildRejectReason::None,
               "island build reject reason is None for constrained scene");
    expectTrue(constrained.stats.validContactCount == 1u,
               "island build stats count valid contacts");
    expectTrue(constrained.stats.constraintEdgeCount == 2u,
               "island build stats sum contacts and distance constraints");
}

void testBuildIslandGraphGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(!build_island_graph_guarded(graph, 0, contacts, constraints),
               "guarded build skips zero bodies");
    expectTrue(graph.islandCount() == 0u, "guarded skip clears graph");

    expectTrue(build_island_graph_guarded(graph, 3, contacts, constraints),
               "guarded build runs for constrained scene");
    expectTrue(graph.islandCount() >= 1u, "guarded build populates islands");
    expectTrue(graph.constrainedIslandCount() == 1u,
               "guarded build surfaces constrained island");
}

void testPreflightIslandSolveBodiesSleepGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandSolveBodyPreflight sleepingPreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(sleepingPreflight.allSleeping, "solve preflight flags all-sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_for_sleep on all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSolveBodyPreflight awakePreflight =
    expectTrue(awakePreflight.can_solve(), "mixed island can solve with awake dynamic body");
    expectTrue(!should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip false when island has awake dynamic body");

    const IslandSolveJob job = extract_island(graph, constrainedIndex);
    expectTrue(should_solve_island_with_bodies(job, bodies, graph.island(constrainedIndex)),
               "should_solve_island_with_bodies allows awake constrained island");
    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(should_skip_island_solve_job_for_sleep(job, bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_job_for_sleep on all-sleeping island");
}

void testDispatchSolveIslandWithBodyGuards() {
    graph.build(2, contacts, constraints);

    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandDispatchResult skipped =
        dispatch_solve_island_with_body_guards_result(bodies,
                                                      graph,
                                                      0u,
                                                      work,
                                                      constraints,
                                                      1.f / 60.f,
                                                      0.f,
                                                      invMassFn);
    expectTrue(skipped.skipped, "body-guarded dispatch skips all-sleeping island");
    expectTrue(!skipped.solved, "body-guarded dispatch does not solve sleeping island");

    expectTrue(dispatch_solve_island_with_body_guards(bodies,
                                                    invMassFn),
               "body-guarded dispatch solves island with awake dynamic body");

void testPreflightIslandSleepWakeGuards() {
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[0] = {};
    bodies.linearVelocities[1] = {};
    bodies.angularVelocities[0] = {};
    bodies.angularVelocities[1] = {};

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    const IslandSleepPreflight sleepPreflight =
        preflight_island_sleep(bodies, graph.island(islandA), params, dt);
    expectTrue(sleepPreflight.can_sleep(), "sleep preflight allows low-velocity contact island");
    expectTrue(sleepPreflight.dynamicCount == 2u, "sleep preflight counts dynamic bodies");

    const IslandSleepPreflight invalidDt =
        preflight_island_sleep(bodies, graph.island(islandA), params, 0.f);
    expectTrue(invalidDt.invalidDt, "sleep preflight rejects zero dt");
    expectTrue(!invalidDt.can_sleep(), "sleep preflight cannot sleep with invalid dt");
    expectTrue(should_skip_island_sleep(graph.island(islandA), 0.f),
               "should_skip_island_sleep on invalid dt");

    bodies.linearVelocities[0] = {1.f, 0.f, 0.f};
    const IslandWakePreflight wakePreflight =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(wakePreflight.should_wake(), "wake preflight sees above-threshold velocity");
    expectTrue(wakePreflight.aboveThresholdCount == 1u,
               "wake preflight counts above-threshold bodies");

    const IslandWakePreflight sleepingWake =
    expectTrue(sleepingWake.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(sleepingWake.should_wake(), "wake preflight should wake sleeping island");

void testSleepWakeIslandGuardedBatch() {

    bodies.sleepTimers[0] = 0.09f;
    bodies.sleepTimers[1] = 0.09f;


    expectTrue(sleep_island_bodies_guarded(bodies, graph.island(islandA), params, dt),
               "guarded sleep accumulates timers for low-velocity island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) != 0u, "guarded sleep marks first body sleeping");
    expectTrue((bodies.flags[1] & RB_SLEEPING) != 0u, "guarded sleep marks second body sleeping");

    const IslandSleepGraphPreflight graphSleep =
        preflight_island_sleep_graph(bodies, graph, params, dt);
    expectTrue(graphSleep.skipped, "graph sleep preflight skips when no island can sleep");

    expectTrue(wake_island_bodies_guarded(bodies, graph.island(islandA)),
               "guarded wake clears sleeping flag on island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "guarded wake clears first body sleep flag");
    expectTrue(bodies.sleepTimers[0] == 0.f, "guarded wake resets sleep timer");

    const u32 wokeCount = wake_all_islands_guarded(bodies, graph, params);
    expectTrue(wokeCount == 1u, "batch wake guarded wakes contact island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "batch wake clears sleeping flags");

void testPreflightSolveIslandJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandSolveBodyPreflight sleepingPreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(sleepingPreflight.allSleeping, "solve preflight flags all-sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_for_sleep on all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSolveBodyPreflight awakePreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(awakePreflight.can_solve(), "mixed island can solve with awake dynamic body");
    expectTrue(!should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip false when island has awake dynamic body");

    const IslandSolveJob job = extract_island(graph, constrainedIndex);
    expectTrue(should_solve_island_with_bodies(job, bodies, graph.island(constrainedIndex)),
               "should_solve_island_with_bodies allows awake constrained island");
    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(should_skip_island_solve_job_for_sleep(job, bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_job_for_sleep on all-sleeping island");
}

void testDispatchSolveIslandWithBodyGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandDispatchResult skipped =
        dispatch_solve_island_with_body_guards_result(bodies,
                                                      graph,
                                                      0u,
                                                      work,
                                                      constraints,
                                                      1.f / 60.f,
                                                      0.f,
                                                      invMassFn);
    expectTrue(skipped.skipped, "body-guarded dispatch skips all-sleeping island");
    expectTrue(!skipped.solved, "body-guarded dispatch does not solve sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    expectTrue(dispatch_solve_island_with_body_guards(bodies,
                                                    graph,
                                                    0u,
                                                    work,
                                                    constraints,
                                                    1.f / 60.f,
                                                    0.f,
                                                    invMassFn),
               "body-guarded dispatch solves island with awake dynamic body");
}

void testPreflightIslandSleepWakeGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[0] = {};
    bodies.linearVelocities[1] = {};
    bodies.angularVelocities[0] = {};
    bodies.angularVelocities[1] = {};

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    const IslandSleepPreflight sleepPreflight =
        preflight_island_sleep(bodies, graph.island(islandA), params, dt);
    expectTrue(sleepPreflight.can_sleep(), "sleep preflight allows low-velocity contact island");
    expectTrue(sleepPreflight.dynamicCount == 2u, "sleep preflight counts dynamic bodies");

    const IslandSleepPreflight invalidDt =
        preflight_island_sleep(bodies, graph.island(islandA), params, 0.f);
    expectTrue(invalidDt.invalidDt, "sleep preflight rejects zero dt");
    expectTrue(!invalidDt.can_sleep(), "sleep preflight cannot sleep with invalid dt");
    expectTrue(should_skip_island_sleep(graph.island(islandA), 0.f),
               "should_skip_island_sleep on invalid dt");

    bodies.linearVelocities[0] = {1.f, 0.f, 0.f};
    const IslandWakePreflight wakePreflight =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(wakePreflight.should_wake(), "wake preflight sees above-threshold velocity");
    expectTrue(wakePreflight.aboveThresholdCount == 1u,
               "wake preflight counts above-threshold bodies");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    bodies.linearVelocities[0] = {};
    const IslandWakePreflight sleepingWake =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(sleepingWake.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(sleepingWake.should_wake(), "wake preflight should wake sleeping island");
}

void testSleepWakeIslandGuardedBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.sleepTimers[0] = 0.09f;
    bodies.sleepTimers[1] = 0.09f;

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    expectTrue(sleep_island_bodies_guarded(bodies, graph.island(islandA), params, dt),
               "guarded sleep accumulates timers for low-velocity island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) != 0u, "guarded sleep marks first body sleeping");
    expectTrue((bodies.flags[1] & RB_SLEEPING) != 0u, "guarded sleep marks second body sleeping");

    const IslandSleepGraphPreflight graphSleep =
        preflight_island_sleep_graph(bodies, graph, params, dt);
    expectTrue(graphSleep.skipped, "graph sleep preflight skips when no island can sleep");

    expectTrue(wake_island_bodies_guarded(bodies, graph.island(islandA)),
               "guarded wake clears sleeping flag on island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "guarded wake clears first body sleep flag");
    expectTrue(bodies.sleepTimers[0] == 0.f, "guarded wake resets sleep timer");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const u32 wokeCount = wake_all_islands_guarded(bodies, graph, params);
    expectTrue(wokeCount == 1u, "batch wake guarded wakes contact island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "batch wake clears sleeping flags");
}

void testPreflightSolveIslandJobGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
        warm_start_island_lambdas(work, island, priorDistance, priorContact);
        expectNear(work.distanceLambdas()[0], 0.f, 1e-6f,
                   "warm_start_island_lambdas early-outs on empty island");
        expectNear(work.contactLambdas()[0], 0.f, 1e-6f,
                   "warm_start_island_lambdas early-out leaves contact slots cold");
    }
    expectTrue(foundEmptySkip, "warm_start_island_lambdas empty-island early-out exercised");

void testWarmStartIslandContactImpulsesSkipsEmptyIsland() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    IslandSolveJob invalid{};
    const IslandSolveJobPreflight invalidPreflight = preflight_solve_island_job(invalid, dt);
    expectTrue(invalidPreflight.skipped, "job preflight skips default job");
    expectTrue(!invalidPreflight.can_dispatch(), "default job cannot dispatch");
    expectTrue(should_skip_solve_island_job(invalid, dt), "should_skip_solve_island_job on default job");

    const IslandSolveJobPreflight invalidDtPreflight = preflight_solve_island_job(invalid, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "job preflight rejects zero dt");
    expectTrue(should_skip_solve_island_job(invalid, 0.f), "should_skip_solve_island_job on invalid dt");

    bool foundConstrainedDispatch = false;
    for (const IslandSolveJob& job : extract_island_jobs(graph)) {
        const IslandSolveJobPreflight preflight = preflight_solve_island_job(job, dt);
        if (job.empty) {
            foundEmptySkip = preflight.skipped && !preflight.can_dispatch();
        } else {
            foundConstrainedDispatch = preflight.can_dispatch() && !should_skip_solve_island_job(job, dt);
    expectTrue(foundEmptySkip, "job preflight skips empty island job");
    expectTrue(foundConstrainedDispatch, "job preflight allows constrained island job");

void testSolveIslandJobGuardsInvalidDt() {
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const IslandSolveJob job = extract_island(graph, 0);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    expectTrue(!solve_island_job(bodies, *job.island, work, constraints, 0.f, 0.f, invMassFn),
               "solve_island_job guards zero dt");
    expectTrue(!solve_island_job(bodies, *job.island, work, constraints, -1.f / 60.f, 0.f, invMassFn),
               "solve_island_job guards negative dt");

void testIsFiniteIslandSolveDtGuard() {
    expectTrue(is_finite_island_solve_dt(1.f / 60.f), "finite positive dt is valid");
    expectTrue(!is_finite_island_solve_dt(0.f), "zero dt is not finite-positive");
    expectTrue(!is_finite_island_solve_dt(std::numeric_limits<f32>::infinity()),
               "infinite dt is not finite-positive");
    expectTrue(!is_finite_island_solve_dt(std::numeric_limits<f32>::quiet_NaN()),
               "NaN dt is not finite-positive");
    expectTrue(is_finite_warm_start_dt(1.f / 60.f), "finite warm-start dt matches solve guard");

void testPreflightIslandConstraintRefsGuards() {
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().valid = false;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    graph.build(4, contacts, constraints);

    const ContactIslandGraph::Island& island = graph.island(islandA);
    const IslandConstraintRefsPreflight preflight =
        preflight_island_constraint_refs(island, contacts, constraints);
    expectTrue(!preflight.skipped, "constraint refs preflight does not skip contact island");
    expectTrue(preflight.ownedContactCount == 1u, "constraint refs preflight counts owned contacts");
    expectTrue(preflight.inRangeContactCount == 1u, "constraint refs preflight counts in-range contacts");
    expectTrue(preflight.validContactCount == 1u, "constraint refs preflight counts valid contacts");
    expectTrue(preflight.can_solve(), "contact island can solve with in-range refs");
    expectTrue(!should_skip_island_constraint_refs(island, contacts, constraints),
               "should_skip false when in-range refs exist");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 1};
    staleIsland.contactIndices = {9u};
    staleIsland.distanceIndices = {5u};
    const IslandConstraintRefsPreflight stalePreflight =
        preflight_island_constraint_refs(staleIsland, contacts, constraints);
    expectTrue(!stalePreflight.can_solve(), "stale refs preflight cannot solve");
    expectTrue(should_skip_island_constraint_refs(staleIsland, contacts, constraints),
               "should_skip true when all refs are out of range");

    work.init(4, 2, 1);
    work.contactManifolds() = contacts;
    expectTrue(!solve_island_job(bodies,
                                 staleIsland,
                                 work,
                                 constraints,
                                 1.f / 60.f,
                                 0.f,
                                 [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job skips island with only stale constraint refs");

void testShouldSkipWarmStartContactImpulsesWithContacts() {
    contacts.back().warmNormalImpulse = 0.f;
    contacts.back().warmNormalImpulse = 2.f;

    graph.build(5, contacts, {});

    const u32 zeroImpulseIsland = graph.bodyIsland(0);
    const u32 nonZeroIsland = graph.bodyIsland(2);
    expectTrue(!should_skip_warm_start_contact_impulses(graph.island(zeroImpulseIsland), dt),
               "two-arg skip cannot detect zero impulses without contact scan");
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(zeroImpulseIsland), contacts, dt),
               "three-arg skip rejects island with only zero impulses");
    expectTrue(!should_skip_warm_start_contact_impulses(graph.island(nonZeroIsland), contacts, dt),
               "three-arg skip allows island with non-zero impulses");

void testPreflightWarmStartContactImpulsesGraphGuards() {
    contacts.back().warmNormalImpulse = 2.5f;


    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "graph impulse preflight does not skip contact graph");
    expectTrue(!preflight.invalidDt, "graph impulse preflight accepts valid dt");
    expectTrue(preflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(preflight.stats.warmStartableCount == 1u,
               "graph impulse preflight counts warm-startable islands");
    expectTrue(preflight.stats.noImpulseCount >= 1u,
               "graph impulse preflight counts zero-impulse constrained islands");
    expectTrue(count_warm_startable_contact_impulse_islands(graph, contacts, dt) == 1u,
               "count_warm_startable_contact_impulse_islands matches stats");
    expectTrue(has_warm_startable_contact_impulse_islands(graph, contacts, dt),
               "has_warm_startable_contact_impulse_islands true when impulses exist");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip graph false when warm-startable islands exist");

    const std::vector<u32> indices = collect_warm_startable_contact_impulse_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == 1u, "collect impulse warm-start indices returns one island");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(preflight_warm_start_contact_impulses_graph(emptyGraph, contacts, dt).skipped,
               "graph impulse preflight skips empty graph");
    expectTrue(should_skip_warm_start_contact_impulses_graph(emptyGraph, contacts, dt),
               "should_skip graph true for empty graph");

    const IslandContactImpulseWarmStartGraphPreflight invalidDt =
        preflight_warm_start_contact_impulses_graph(graph, contacts, 0.f);
    expectTrue(invalidDt.invalidDt, "graph impulse preflight rejects zero dt");
    expectTrue(!invalidDt.can_warm_start(), "graph impulse preflight cannot warm-start with invalid dt");

void testWarmStartAllIslandsCombinedResultBatch() {
    work.ensureLambdaCapacity(2, 1);

    contacts.back().warmNormalImpulse = 3.f;



    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_combined_result(work, graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!batch.skipped, "combined batch does not skip contact graph");
    expectTrue(batch.warmStartableCount == 1u, "combined batch records warm-startable count");
    expectTrue(batch.warmedCount == 1u, "combined batch warms contact island");
    expectTrue(batch.any_warmed(), "combined batch reports warmed island");
    expectTrue(warm_start_all_islands_combined_guarded(work, graph, contacts, dt, priorDistance, priorContact) ==
                   batch.warmedCount,
               "combined guarded count matches batch result");

    const IslandBatchWarmStartResult invalidDt =
        warm_start_all_islands_combined_result(work, graph, contacts, 0.f, priorDistance, priorContact);
    expectTrue(invalidDt.skipped, "combined batch skips invalid dt");
    expectTrue(!invalidDt.any_warmed(), "invalid dt combined batch reports no warmed islands");

void testPreflightIslandBuildGuards() {
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},

    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip valid partition inputs");
    expectTrue(preflight.has_unsafe_refs(), "build preflight flags out-of-range constraint refs");
    expectTrue(!preflight.can_build(), "build preflight cannot build unsafe contact refs");
    expectTrue(should_skip_island_build(4, contacts, constraints),
               "should_skip_island_build on unsafe valid out-of-range contacts");
    expectTrue(preflight.stats.bodyCount == 4u, "build preflight records body count");
    expectTrue(preflight.stats.validContactCount == 2u, "build preflight counts valid contacts");
    expectTrue(preflight.stats.inRangeContactCount == 1u, "build preflight counts in-range contacts");
    expectTrue(preflight.stats.outOfRangeContactBodyCount == 1u,
               "build preflight counts out-of-range contact bodies");
    expectTrue(preflight.stats.inRangeDistanceCount == 1u, "build preflight counts in-range distance constraints");
    expectTrue(preflight.stats.outOfRangeDistanceBodyCount == 1u,
               "build preflight counts out-of-range distance bodies");

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0, {}, {});
    expectTrue(emptyPreflight.skipped, "build preflight skips zero-body empty inputs");
    expectTrue(should_skip_island_build(0, {}, {}), "should_skip_island_build on empty inputs");

    expectTrue(!build_island_graph_guarded(graph, 0, {}, {}), "guarded build skips empty inputs");
    expectTrue(graph.islandCount() == 0u, "skipped guarded build clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(contacts[0]);
    const std::vector<DistanceConstraint> safeConstraints = {constraints[0]};
    expectTrue(!build_island_graph_guarded(graph, 4, contacts, constraints),
               "guarded build rejects unsafe out-of-range contact refs");
    expectTrue(graph.islandCount() == 0u, "unsafe guarded build clears graph");

    expectTrue(build_island_graph_guarded(graph, 4, safeContacts, safeConstraints),
               "guarded build succeeds for in-range inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build forms constrained island");

void testPreflightIslandSolveBodiesGuards() {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},

    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, 0);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 activeIsland = graph.bodyIsland(2);
    const IslandSolveBodiesPreflight sleepingPreflight =
        preflight_island_solve_bodies(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "solve-bodies preflight does not skip constrained island");
    expectTrue(sleepingPreflight.movableCount == 1u, "mixed island has one movable body");
    expectTrue(sleepingPreflight.sleepingCount == 1u, "mixed island counts sleeping body");
    expectTrue(sleepingPreflight.can_solve(), "mixed island can still solve");

    bodies.flags[2] |= RB_SLEEPING;
    bodies.flags[3] |= RB_SLEEPING;
    const IslandSolveBodiesPreflight allSleepingPreflight =
        preflight_island_solve_bodies(graph.island(activeIsland), bodies);
    expectTrue(!allSleepingPreflight.can_solve(), "all-sleeping island cannot solve bodies");
    expectTrue(should_skip_island_solve_bodies(graph.island(activeIsland), bodies),
               "should_skip_island_solve_bodies on all-sleeping island");

    work.init(4, 0, 2);
    work.contactManifolds().clear();
    const IslandConstraintSolvePreflight combinedPreflight = preflight_island_constraint_solve(
        graph.island(activeIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!combinedPreflight.can_solve(), "combined solve preflight rejects all-sleeping island");
    expectTrue(should_skip_island_constraint_solve(
                   graph.island(activeIsland), bodies, work.contactManifolds(), constraints),
               "should_skip_island_constraint_solve on all-sleeping island");

void testPreflightIslandSleepWakeGuards() {

    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandSleepPreflight mixedSleep = preflight_island_sleep(graph.island(mixedIsland), bodies);
    expectTrue(!mixedSleep.skipped, "sleep preflight does not skip mixed island");
    expectTrue(!mixedSleep.allSleeping, "mixed island is not all-sleeping");
    expectTrue(mixedSleep.activeDynamicCount == 1u, "mixed island has one active dynamic body");
    expectTrue(!should_skip_island_sleep_solve(graph.island(mixedIsland), bodies),
               "should_skip sleep solve false for mixed island");

    const IslandSleepPreflight allSleeping = preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(allSleeping.allSleeping, "all-dynamic-sleeping island is all-sleeping");
    expectTrue(allSleeping.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_island_sleep_solve(graph.island(sleepingIsland), bodies),
               "should_skip sleep solve true for all-sleeping island");

    const IslandWakePreflight mixedWake = preflight_island_wake(graph.island(mixedIsland), bodies);
    expectTrue(mixedWake.should_wake_sleepers(), "mixed island should wake sleepers");
    expectTrue(!should_skip_island_wake(graph.island(mixedIsland), bodies),
               "should_skip island wake false for mixed island");

    const IslandWakePreflight sleepingWake = preflight_island_wake(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingWake.should_wake_sleepers(), "all-sleeping island has no wake target");
    expectTrue(should_skip_island_wake(graph.island(sleepingIsland), bodies),
               "should_skip island wake true when no active dynamic body");

    const IslandSleepGraphPreflight sleepGraph = preflight_island_sleep_graph(graph, bodies);
    expectTrue(!sleepGraph.skipped, "sleep graph preflight has solveable islands");
    expectTrue(sleepGraph.stats.mixedSleepCount == 1u, "sleep graph counts mixed island");
    expectTrue(sleepGraph.stats.allSleepingCount == 1u, "sleep graph counts all-sleeping island");
    expectTrue(!should_skip_island_sleep_solve_graph(graph, bodies),
               "should_skip sleep graph false when mixed island exists");

    const IslandWakeGraphPreflight wakeGraph = preflight_island_wake_graph(graph, bodies);
    expectTrue(wakeGraph.can_wake(), "wake graph preflight can wake mixed island");
    expectTrue(wakeGraph.stats.wakeableCount == 1u, "wake graph counts wakeable island");
    expectTrue(collect_wakeable_island_indices(graph, bodies).size() == 1u,
               "collect wakeable indices returns mixed island");
    expectTrue(collect_nonsleeping_island_indices(graph, bodies).size() == 1u,
               "collect nonsleeping indices skips all-sleeping island");

    expectTrue(wake_island_sleepers_guarded(bodies, graph.island(mixedIsland)),
               "guarded wake activates mixed island sleepers");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "guarded wake clears sleeping flag");
    expectNear(bodies.sleepTimers[1], 0.f, 1e-6f, "guarded wake resets sleep timer");
    expectTrue(wake_all_island_sleepers_guarded(bodies, graph) == 0u,
               "batch wake does not re-wake already active island");

    const IslandSleepPreflight outOfRange = preflight_island_sleep_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "sleep index preflight skips out-of-range island");
    const IslandWakePreflight outOfRangeWake = preflight_island_wake_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRangeWake.skipped, "wake index preflight skips out-of-range island");

void testContactIslandGraphBuildRejectReasonGuards() {


    expectTrue(contactIslandGraphBuildRejectReason(4, contacts, constraints) ==
                   ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies,
               "graph build reject reason flags out-of-range contacts");
    expectTrue(contactIslandGraphBuildRejectsForReason(4,
                                                       contacts,
                                                       ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies),
               "graph build rejectsForReason matches out-of-range contacts");
    expectTrue(std::strcmp(contactIslandGraphBuildRejectReasonName(
                           "OutOfRangeContactBodies") == 0,
               "graph build reject reason name is stable");

    const ContactIslandGraphBuildPreflight preflight = preflightContactIslandGraphBuild(4, contacts, constraints);
    expectTrue(!preflight.can_build(), "graph build preflight rejects unsafe refs");
    expectTrue(preflight.reason == ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies,
               "graph build preflight records reject reason");
    expectTrue(canSkipContactIslandGraphBuild(4, contacts, constraints),
               "canSkipContactIslandGraphBuild true for unsafe refs");
    expectTrue(!shouldRunContactIslandGraphBuild(4, contacts, constraints),
               "shouldRunContactIslandGraphBuild false for unsafe refs");

    expectTrue(!graph.buildGuarded(4, contacts, constraints),
               "buildGuarded rejects unsafe out-of-range contacts");
    expectTrue(graph.islandCount() == 0u, "rejected buildGuarded clears graph");

    expectTrue(contactIslandGraphBuildRejectReason(0, {}, {}) ==
                   ContactIslandGraphBuildRejectReason::EmptyInput,
               "graph build reject reason flags empty input");
    expectTrue(!graph.buildGuarded(0, {}, {}), "buildGuarded skips empty zero-body input");
    expectTrue(graph.islandCount() == 0u, "skipped empty buildGuarded clears graph");

void testIslandBuildRejectReasonGuards() {


    expectTrue(preflight.reason == ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies,
               "island build preflight surfaces distance reject reason");
    expectTrue(canSkipIslandBuild(4, contacts, constraints),
               "canSkipIslandBuild true for out-of-range distance refs");
    expectTrue(!shouldRunIslandBuild(4, contacts, constraints),
               "shouldRunIslandBuild false for out-of-range distance refs");

void testIslandDispatchRejectReasonGuards() {

    expectTrue(islandDispatchRejectReason(graph, 1.f / 60.f) == IslandDispatchRejectReason::None,
               "valid graph and dt pass dispatch reject reason");
    expectTrue(islandDispatchRejectsForReason(graph, 0.f, IslandDispatchRejectReason::InvalidDt),
               "dispatch rejectsForReason flags invalid dt");
    expectTrue(std::strcmp(islandDispatchRejectReasonName(IslandDispatchRejectReason::InvalidDt), "InvalidDt") == 0,
               "dispatch reject reason name is stable");

    const IslandDispatchRejectPreflight preflight = preflightIslandDispatchReject(graph, 1.f / 60.f);
    expectTrue(preflight.can_dispatch(), "dispatch reject preflight allows constrained graph");
    expectTrue(shouldRunIslandDispatch(graph, 1.f / 60.f),
               "shouldRunIslandDispatch true for constrained graph");

    expectTrue(canSkipIslandDispatch(emptyGraph, 1.f / 60.f),
               "canSkipIslandDispatch true for empty graph");
    expectTrue(islandDispatchRejectReason(emptyGraph, 1.f / 60.f) ==
                   IslandDispatchRejectReason::NoDispatchableIslands,
               "empty graph dispatch reject reason is no dispatchable islands");

void testIslandSolveJobRejectReasonGuards() {
    expectTrue(islandSolveJobRejectReason(invalid, dt) == IslandSolveJobRejectReason::EmptyJob,
               "default job reject reason is empty job");
    expectTrue(islandSolveJobRejectsForReason(invalid, 0.f, IslandSolveJobRejectReason::InvalidDt),
               "job rejectsForReason flags invalid dt before empty job");
    expectTrue(std::strcmp(islandSolveJobRejectReasonName(IslandSolveJobRejectReason::EmptyJob), "EmptyJob") == 0,
               "job reject reason name is stable");


    const IslandSolveJob job = extract_island(graph, graph.bodyIsland(0));
    const IslandSolveJobRejectPreflight preflight = preflightIslandSolveJobReject(job, dt);
    expectTrue(preflight.can_dispatch(), "constrained job reject preflight can dispatch");
    expectTrue(shouldRunIslandSolveJob(job, dt), "shouldRunIslandSolveJob true for constrained job");
    expectTrue(!canSkipIslandSolveJob(job, dt), "canSkipIslandSolveJob false for constrained job");

void testIslandConstraintSolveRejectReasonGuards() {

    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const ContactIslandGraph::Island& island = graph.island(0);
    const IslandConstraintSolveRejectPreflight preflight =
        preflightIslandConstraintSolveReject(island, bodies, contacts, constraints);
    expectTrue(preflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies,
               "constraint solve reject reason flags no movable bodies");
    expectTrue(islandConstraintSolveRejectsForReason(island,
                                                     bodies,
                                                     IslandConstraintSolveRejectReason::NoMovableBodies),
               "constraint solve rejectsForReason matches no movable bodies");
    expectTrue(canSkipIslandConstraintSolve(island, bodies, contacts, constraints),
               "canSkipIslandConstraintSolve true for all-sleeping island");
    expectTrue(!shouldRunIslandConstraintSolve(island, bodies, contacts, constraints),
               "shouldRunIslandConstraintSolve false for all-sleeping island");

void testIslandSleepWakeRejectReasonGuards() {



    expectTrue(islandWakeRejectReason(graph.island(mixedIsland), bodies) == IslandWakeRejectReason::None,
               "mixed island wake reject reason is none");
    expectTrue(islandWakeRejectReason(graph.island(sleepingIsland), bodies) ==
                   IslandWakeRejectReason::NoMixedSleepState,
               "all-sleeping island wake reject reason is no mixed sleep state");
    expectTrue(islandSleepSolveRejectReason(graph.island(sleepingIsland), bodies) ==
                   IslandSleepSolveRejectReason::AllSleeping,
               "all-sleeping island sleep solve reject reason is all sleeping");
    expectTrue(canSkipIslandWake(graph.island(sleepingIsland), bodies),
               "canSkipIslandWake true when no mixed sleep state");
    expectTrue(shouldRunIslandWake(graph.island(mixedIsland), bodies),
               "shouldRunIslandWake true for mixed island");

    const IslandSleepGraphRejectPreflight sleepGraph = preflightIslandSleepGraphReject(graph, bodies);
    expectTrue(sleepGraph.has_solveable_islands(), "sleep graph reject preflight has solveable islands");
    expectTrue(shouldRunIslandSleepGraph(graph, bodies),
               "shouldRunIslandSleepGraph true when mixed island exists");

    const IslandWakeGraphRejectPreflight wakeGraph = preflightIslandWakeGraphReject(graph, bodies);
    expectTrue(wakeGraph.can_wake(), "wake graph reject preflight can wake mixed island");
    expectTrue(shouldRunIslandWakeGraph(graph, bodies),
               "shouldRunIslandWakeGraph true when wakeable island exists");

void testIslandPipelineDispatchRejectReasonGuards() {



    const IslandPipelineDispatchPreflight preflight = preflightIslandPipelineDispatch(graph, bodies, dt);
    expectTrue(preflight.can_dispatch(), "pipeline preflight allows mixed active island graph");
    expectTrue(shouldRunIslandPipelineDispatch(graph, bodies, dt),
               "shouldRunIslandPipelineDispatch true for mixed graph");
    expectTrue(islandPipelineDispatchRejectReason(graph, bodies, dt) ==
                   IslandPipelineDispatchRejectReason::None,
               "pipeline reject reason is none for mixed graph");

    bodies.flags[0] |= RB_SLEEPING;
                   IslandPipelineDispatchRejectReason::AllIslandsSleeping,
               "pipeline reject reason flags all-sleeping graph");
    expectTrue(canSkipIslandPipelineDispatch(graph, bodies, dt),
               "canSkipIslandPipelineDispatch true when all islands sleeping");
    expectTrue(islandPipelineDispatchRejectsForReason(graph,
                                                      dt,
                                                      IslandPipelineDispatchRejectReason::AllIslandsSleeping),
               "pipeline rejectsForReason matches all-sleeping graph");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandBatchDispatchResult pipelineBatch = dispatch_island_pipeline_guarded(bodies,
                                                                                   graph,
                                                                                   invMassFn);
    expectTrue(!pipelineBatch.skipped, "pipeline guarded dispatch runs for mixed graph");
    expectTrue(pipelineBatch.solvedCount == graph.constrainedIslandCount(),
               "pipeline guarded dispatch solves all constrained islands");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u,
               "pipeline guarded dispatch wakes mixed island sleepers");

    const IslandBatchDispatchResult preflightBatch = dispatch_all_islands_with_preflight(bodies,
    expectTrue(!preflightBatch.skipped, "dispatch_all_islands_with_preflight runs constrained graph");
    expectTrue(preflightBatch.solvedCount == graph.constrainedIslandCount(),
               "dispatch_all_islands_with_preflight solves all constrained islands");

    expectTrue(dispatch_all_islands_with_preflight(bodies,
                                                   emptyGraph,
                                                   invMassFn)
                   .skipped,
               "dispatch_all_islands_with_preflight skips empty graph");

void testIslandRejectReasonGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    expectTrue(island_build_rejects_for_reason(0, {}, {}, IslandBuildRejectReason::EmptyInput),
               "build reject reason flags empty input");
    expectTrue(island_build_rejects_for_reason(4, contacts, constraints,
                                               IslandBuildRejectReason::OutOfRangeContactRefs),
               "build reject reason flags out-of-range contact refs");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyInput),
                           "EmptyInput") == 0,
               "build reject reason name for EmptyInput");

    const IslandBuildPreflight buildPreflight = preflight_island_build(4, contacts, constraints);
    expectTrue(buildPreflight.reason == IslandBuildRejectReason::OutOfRangeContactRefs,
               "build preflight carries reject reason");
    expectTrue(!buildPreflight.can_build(), "build preflight cannot build with reject reason");

    ContactIslandGraph graph;
    expectTrue(!graph.buildGuarded(4, contacts, constraints),
               "graph buildGuarded rejects unsafe contact refs");
    expectTrue(graph.islandCount() == 0u, "buildGuarded clears graph on reject");
    expectTrue(!island_graph_build_inputs_valid(4, contacts, constraints),
               "island_graph_build_inputs_valid rejects unsafe refs");

    const std::vector<DistanceConstraint> islandConstraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, {}, islandConstraints);
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    expectTrue(island_sleep_rejects_for_reason(graph.island(sleepingIsland), bodies,
                                               IslandSleepRejectReason::AllSleeping),
               "sleep reject reason flags all-sleeping island");
    expectTrue(island_sleep_reject_reason(graph.island(mixedIsland), bodies) ==
                   IslandSleepRejectReason::None,
               "sleep reject reason None for mixed island");
    expectTrue(island_sleep_reject_reason_by_index(graph, graph.islandCount() + 1u, bodies) ==
                   IslandSleepRejectReason::OutOfRangeIslandIndex,
               "sleep reject reason flags out-of-range index");

    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(sleepPreflight.reason == IslandSleepRejectReason::AllSleeping,
               "sleep preflight carries reject reason");

    expectTrue(island_wake_rejects_for_reason(graph.island(mixedIsland), bodies, IslandWakeRejectReason::None),
               "wake reject reason None for mixed island");
    expectTrue(island_wake_rejects_for_reason(graph.island(sleepingIsland), bodies,
                                              IslandWakeRejectReason::NoActiveDynamic),
               "wake reject reason flags all-sleeping island");
    expectTrue(island_wake_reject_reason_by_index(graph, graph.islandCount() + 1u, bodies) ==
                   IslandWakeRejectReason::OutOfRangeIslandIndex,
               "wake reject reason flags out-of-range index");

    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(mixedIsland), bodies);
    expectTrue(wakePreflight.reason == IslandWakeRejectReason::None,
               "wake preflight carries reject reason None for mixed island");
    expectTrue(wakePreflight.should_wake_sleepers(), "wake preflight should wake mixed island");

    const IslandSleepGraphPreflight sleepGraphPreflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(sleepGraphPreflight.reason == IslandSleepGraphRejectReason::None,
               "sleep graph preflight carries reject reason None when mixed island exists");
    expectTrue(sleepGraphPreflight.has_solveable_islands(),
               "sleep graph preflight has solveable islands with mixed state");

    const IslandWakeGraphPreflight wakeGraphPreflight = preflight_island_wake_graph(graph, bodies);
    expectTrue(wakeGraphPreflight.reason == IslandWakeGraphRejectReason::None,
               "wake graph preflight carries reject reason None when wakeable island exists");
    expectTrue(wakeGraphPreflight.can_wake(), "wake graph preflight can wake mixed island");

    bodies.flags[2] |= RB_SLEEPING;
    bodies.flags[3] |= RB_SLEEPING;
    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();
    const IslandConstraintSolvePreflight solvePreflight = preflight_island_constraint_solve(
        graph.island(sleepingIsland), bodies, work.contactManifolds(), islandConstraints);
    expectTrue(solvePreflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies,
               "constraint solve preflight carries NoMovableBodies reject reason");
    expectTrue(island_constraint_solve_rejects_for_reason(
                   graph.island(sleepingIsland), bodies, work.contactManifolds(), islandConstraints,
                   IslandConstraintSolveRejectReason::NoMovableBodies),
               "constraint solve rejects_for_reason matches all-sleeping island");
    expectTrue(island_constraint_solve_reject_reason_by_index(
                   graph, graph.islandCount() + 1u, bodies, work.contactManifolds(), islandConstraints) ==
                   IslandConstraintSolveRejectReason::OutOfRangeIslandIndex,
               "constraint solve reject reason flags out-of-range index");
}

void testPreflightIslandBuildRejectReasonGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(island_build_rejects_for_reason(4, contacts, constraints,
                                               IslandBuildRejectReason::OutOfRangeContactRefs),
               "build rejects for out-of-range contact refs");
    expectTrue(static_cast<fuse::u32>(island_build_reject_reason(4, contacts, constraints)) ==
                   static_cast<fuse::u32>(IslandBuildRejectReason::OutOfRangeContactRefs),
               "build reject reason matches out-of-range contacts");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::OutOfRangeContactRefs),
                           "OutOfRangeContactRefs") == 0,
               "build reject reason name is stable");

    expectTrue(island_build_rejects_for_reason(0, {}, {}, IslandBuildRejectReason::EmptyInput),
               "build rejects empty input");

    ContactIslandGraph withPreflightGraph;
    ContactIslandGraph guardedGraph;
    expectTrue(build_island_graph_with_preflight(withPreflightGraph, 4, contacts, constraints) ==
                   build_island_graph_guarded(guardedGraph, 4, contacts, constraints),
               "build_with_preflight mirrors guarded build outcome");
}

void testPreflightIslandConstraintSolveDeepenGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    SolverWorkBuffers work;
    work.init(4, 0, 2);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    const IslandConstraintSolvePreflight mixedPreflight = preflight_island_constraint_solve(
        graph.island(mixedIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint-solve deepen preflight");
    expectTrue(static_cast<fuse::u32>(mixedPreflight.reason) ==
                   static_cast<fuse::u32>(IslandConstraintSolveRejectReason::None),
               "mixed island has no constraint-solve reject reason");

    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve(
        graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint-solve deepen preflight");
    expectTrue(island_constraint_solve_rejects_for_reason(graph.island(sleepingIsland),
                                                          bodies,
                                                          work.contactManifolds(),
                                                          constraints,
                                                          IslandConstraintSolveRejectReason::AllSleeping),
               "constraint-solve rejects all-sleeping island");

    const IslandConstraintSolveGraphPreflight graphPreflight =
        preflight_island_constraint_solve_graph(graph, bodies, work.contactManifolds(), constraints);
    expectTrue(graphPreflight.has_solveable_islands(), "graph constraint-solve preflight has solveable island");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "graph counts one solveable island");
    expectTrue(collect_solveable_island_indices(graph, bodies, work.contactManifolds(), constraints).size() ==
                   1u,
               "collect solveable indices returns mixed island");

    work.init(2, 1, 1);
    std::vector<narrowphase::ContactManifold> staleContacts;
    staleContacts.push_back(narrowphase::ContactManifold{});
    staleContacts.back().valid = true;
    staleContacts.back().bodyA = 0;
    staleContacts.back().bodyB = 1;
    work.contactManifolds() = staleContacts;

    ContactIslandGraph staleGraph;
    staleGraph.build(2, staleContacts, {});
    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = staleGraph.island(0).bodyIndices;
    staleIsland.contactIndices.push_back(99u);
    expectTrue(island_constraint_solve_rejects_for_reason(staleIsland,
                                                          bodies,
                                                          staleContacts,
                                                          constraints,
                                                          IslandConstraintSolveRejectReason::StaleConstraintRefs),
               "constraint-solve rejects stale contact refs");

    const IslandDispatchDeepenPreflight dispatchPreflight = preflight_island_dispatch_deepen(
        graph, mixedIsland, bodies, work.contactManifolds(), constraints, 1.f / 60.f);
    expectTrue(dispatchPreflight.can_dispatch(), "deepen dispatch preflight accepts mixed island");
    expectTrue(solve_island_job_with_preflight(bodies,
                                               graph.island(mixedIsland),
                                               work,
                                               constraints,
                                               1.f / 60.f,
                                               0.f,
                                               [](const RigidBodySoA& bodySoA, u32 index) {
                                                   return bodySoA.invMasses[index];
                                               }),
               "solve_island_job_with_preflight solves mixed island");
    expectTrue(!solve_island_job_with_preflight(bodies,
                                                graph.island(sleepingIsland),
                                                work,
                                                constraints,
                                                1.f / 60.f,
                                                0.f,
                                                [](const RigidBodySoA& bodySoA, u32 index) {
                                                    return bodySoA.invMasses[index];
                                                }),
               "solve_island_job_with_preflight skips all-sleeping island");
    expectTrue(dispatch_solve_island_with_preflight(bodies,
                                                      graph,
                                                      mixedIsland,
                                                      work,
                                                      constraints,
                                                      1.f / 60.f,
                                                      0.f,
                                                      [](const RigidBodySoA& bodySoA, u32 index) {
                                                          return bodySoA.invMasses[index];
                                                      }),
               "dispatch_solve_island_with_preflight solves mixed island");
    expectTrue(!dispatch_solve_island_with_preflight(bodies,
                                                     graph,
                                                     sleepingIsland,
                                                     work,
                                                     constraints,
                                                     1.f / 60.f,
                                                     0.f,
                                                     [](const RigidBodySoA& bodySoA, u32 index) {
                                                         return bodySoA.invMasses[index];
                                                     }),
               "dispatch_solve_island_with_preflight skips all-sleeping island");
}

void testPreflightIslandSleepWakeRejectReasonGuards() {
    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, {}, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    expectTrue(island_sleep_solve_rejects_for_reason(graph.island(sleepingIsland),
                                                     bodies,
                                                     IslandSleepSolveRejectReason::AllSleeping),
               "sleep solve rejects all-sleeping island");
    expectTrue(island_wake_reject_reason(graph.island(mixedIsland), bodies) == IslandWakeRejectReason::None,
               "mixed island has no wake reject reason");
    expectTrue(island_wake_rejects_for_reason(graph.island(sleepingIsland),
                                              bodies,
                                              IslandWakeRejectReason::NoMixedSleepState),
               "uniform sleeping island rejects wake");

    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, mixedIsland);
    expectTrue(wakeResult.woke, "wake result activates mixed island");
    expectTrue(wakeResult.islandIndex == mixedIsland, "wake result records island index");

    const IslandBatchWakeResult batchResult = wake_all_island_sleepers_result(bodies, graph);
    expectTrue(batchResult.skipped, "batch wake skips when no wakeable islands remain");
    expectTrue(!batchResult.any_woke(), "batch wake reports no additional woke islands");
}

void testContactIslandGraphBodyRangeHelpers() {
    expectTrue(ContactIslandGraph::bodies_in_range(0, 1, 4), "in-range contact bodies pass range check");
    expectTrue(!ContactIslandGraph::bodies_in_range(3, 4, 4), "out-of-range contact bodies fail range check");
    expectTrue(ContactIslandGraph::is_self_contact(2, 2), "self contact detected");
    expectTrue(!ContactIslandGraph::is_self_contact(0, 1), "distinct bodies are not self contact");
}

void testIslandBuildResultAndSelfContactStats() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 0;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 1;
    contacts.back().bodyB = 2;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(preflight.stats.selfContactCount == 1u, "build preflight counts self contacts");
    expectTrue(preflight.stats.invalidContactCount == 1u, "build preflight counts invalid contacts");
    expectTrue(preflight.has_unsafe_refs(), "out-of-range valid contact still marks unsafe refs");
    expectTrue(!preflight.can_build(), "unsafe refs block build preflight");

    ContactIslandGraph graph;
    const IslandBuildResult skipped = build_island_graph_result(graph, 4, contacts, constraints);
    expectTrue(!skipped.built, "build result reports skipped unsafe build");
    expectTrue(skipped.unsafeRefs, "build result flags unsafe refs");
    expectTrue(graph.islandCount() == 0u, "skipped build result clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(narrowphase::ContactManifold{});
    safeContacts.back().valid = true;
    safeContacts.back().bodyA = 0;
    safeContacts.back().bodyB = 1;
    const IslandBuildResult built = build_island_graph_result(graph, 4, safeContacts, constraints);
    expectTrue(built.built, "build result reports successful in-range build");
    expectTrue(!built.unsafeRefs, "safe build result has no unsafe refs");
    expectTrue(graph.constrainedIslandCount() == 1u, "build result forms constrained island");
}

void testSolveIslandJobWithBodiesGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    const IslandSolveJob mixedJob = extract_island(graph, mixedIsland);
    const IslandSolveJob sleepingJob = extract_island(graph, sleepingIsland);
    const f32 dt = 1.f / 60.f;

    const IslandConstraintSolveJobPreflight mixedPreflight =
        preflight_solve_island_job_with_bodies(mixedJob, bodies, work.contactManifolds(), constraints, dt);
    expectTrue(mixedPreflight.can_solve(), "mixed island job preflight can solve");
    expectTrue(!should_skip_solve_island_job_with_bodies(mixedJob, bodies, work.contactManifolds(), constraints, dt),
               "mixed island job is not skipped");

    const IslandConstraintSolveJobPreflight sleepingPreflight =
        preflight_solve_island_job_with_bodies(sleepingJob, bodies, work.contactManifolds(), constraints, dt);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island job preflight cannot solve");
    expectTrue(should_skip_solve_island_job_with_bodies(sleepingJob, bodies, work.contactManifolds(), constraints, dt),
               "all-sleeping island job is skipped");

    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(mixedIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        [](const RigidBodySoA&, u32) { return 1.f; }),
               "guarded solve succeeds for mixed island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         [](const RigidBodySoA&, u32) { return 1.f; }),
               "guarded solve skips all-sleeping island");

    const std::vector<u32> solveable =
        collect_solveable_island_indices(graph, bodies, work.contactManifolds(), constraints);
    expectTrue(solveable.size() == 1u, "collect solveable indices skips all-sleeping island");

    const IslandBatchBodiesDispatchResult batch = dispatch_all_islands_with_bodies_result(
        bodies, graph, work, constraints, dt, 0.f, [](const RigidBodySoA&, u32) { return 1.f; });
    expectTrue(batch.solveableCount == 1u, "batch bodies dispatch counts solveable islands");
    expectTrue(batch.solvedCount == 1u, "batch bodies dispatch solves mixed island");
    expectTrue(batch.any_solved(), "batch bodies dispatch reports solved work");
}

void testIslandWakeResultAndSleepDispatchGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();

    const u32 mixedIsland = graph.bodyIsland(0);
    const f32 dt = 1.f / 60.f;

    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, mixedIsland);
    expectTrue(wakeResult.woke, "wake result reports woke island");
    expectTrue(wakeResult.bodiesWoken == 1u, "wake result counts awakened bodies");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake result clears sleeping flag");

    const IslandWakeResult repeatWake = wake_island_sleepers_result(bodies, graph, mixedIsland);
    expectTrue(!repeatWake.woke, "repeat wake result skips already-awake island");
    expectTrue(repeatWake.skipped, "repeat wake result is skipped");

    bodies.flags[1] |= RB_SLEEPING;
    const IslandBatchWakeResult batchWake = wake_all_island_sleepers_result(bodies, graph);
    expectTrue(batchWake.any_woke(), "batch wake result reports woke islands");
    expectTrue(batchWake.wokeCount == 1u, "batch wake result counts woke islands");
    expectTrue(batchWake.bodiesWoken == 1u, "batch wake result counts awakened bodies");

    const IslandSleepDispatchPreflight sleepDispatch = preflight_island_sleep_dispatch(graph, bodies, dt);
    expectTrue(sleepDispatch.can_dispatch(), "sleep dispatch preflight can dispatch mixed graph");
    expectTrue(!should_skip_island_sleep_dispatch(graph, bodies, dt),
               "should_skip sleep dispatch false for mixed graph");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    bodies.flags[2] |= RB_SLEEPING;
    bodies.flags[3] |= RB_SLEEPING;
    expectTrue(should_skip_island_sleep_dispatch(graph, bodies, dt),
               "should_skip sleep dispatch true when every island is all-sleeping");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const IslandBatchSleepDispatchResult wakeAndSolve = dispatch_all_islands_wake_and_solve_result(
        bodies, graph, work, constraints, dt, 0.f, [](const RigidBodySoA&, u32) { return 1.f; });
    expectTrue(wakeAndSolve.wokeCount == 1u, "wake-and-solve batch wakes mixed island");
    expectTrue(wakeAndSolve.bodiesWoken == 1u, "wake-and-solve batch counts awakened bodies");
    expectTrue(wakeAndSolve.solvedCount == 1u, "wake-and-solve batch solves mixed island");
    expectTrue(wakeAndSolve.any_solved(), "wake-and-solve batch reports solved work");
}

void testContactIslandGraphPartitionHelpers() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 8;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    expectTrue(ContactIslandGraph::partitionBodyInRange(4, 0), "partition body in range");
    expectTrue(!ContactIslandGraph::partitionBodyInRange(4, 4), "partition body out of range");
    expectTrue(ContactIslandGraph::contactPartitionInRange(4, contacts[0]),
               "in-range contact partition accepted");
    expectTrue(!ContactIslandGraph::contactPartitionInRange(4, contacts[1]),
               "out-of-range contact partition rejected");
    expectTrue(ContactIslandGraph::distancePartitionInRange(4, constraints[0]),
               "in-range distance partition accepted");
    expectTrue(!ContactIslandGraph::distancePartitionInRange(4, constraints[1]),
               "out-of-range distance partition rejected");
    expectTrue(ContactIslandGraph::countUnionableContacts(4, contacts) == 1u,
               "unionable contact count excludes out-of-range bodies");
    expectTrue(ContactIslandGraph::countUnionableDistanceConstraints(4, constraints) == 1u,
               "unionable distance count excludes out-of-range bodies");
}

void testIslandBuildResultGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    const IslandBuildResult unsafe =
        build_island_graph_result(graph, 4, contacts, constraints);
    expectTrue(unsafe.skipped, "build result skips unsafe refs");
    expectTrue(unsafe.unsafeRefs, "build result flags unsafe refs");
    expectTrue(!unsafe.built, "build result does not build unsafe refs");
    expectTrue(graph.islandCount() == 0u, "unsafe build result clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(contacts[0]);
    const IslandBuildResult built = build_island_graph_result(graph, 4, safeContacts, constraints);
    expectTrue(built.built, "build result builds in-range inputs");
    expectTrue(!built.skipped, "build result does not skip valid inputs");
    expectTrue(!built.unsafeRefs, "build result has no unsafe refs");
    expectTrue(graph.constrainedIslandCount() == 1u, "build result forms constrained island");
}

void testPreflightIslandConstraintSolveGraphGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandConstraintSolvePreflight mixedPreflight = preflight_island_constraint_solve_by_index(
        graph, mixedIsland, bodies, contacts, constraints);
    expectTrue(!mixedPreflight.skipped, "constraint solve index preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint solve preflight");

    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve_by_index(
        graph, sleepingIsland, bodies, contacts, constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint solve preflight");
    expectTrue(should_skip_island_constraint_solve_by_index(graph, sleepingIsland, bodies, contacts, constraints),
               "should_skip constraint solve by index on all-sleeping island");

    const IslandConstraintSolveGraphPreflight graphPreflight =
        preflight_island_constraint_solve_graph(graph, bodies, contacts, constraints);
    expectTrue(!graphPreflight.skipped, "constraint solve graph preflight has solveable island");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "constraint solve graph counts solveable island");
    expectTrue(graphPreflight.stats.blockedByBodiesCount == 1u,
               "constraint solve graph counts body-blocked island");
    expectTrue(collect_solveable_island_indices(graph, bodies, contacts, constraints).size() == 1u,
               "collect solveable indices returns mixed island");
    expectTrue(!should_skip_island_constraint_solve_graph(graph, bodies, contacts, constraints),
               "should_skip constraint solve graph false when mixed island exists");

    const IslandSolveBodiesPreflight bodiesByIndex =
        preflight_island_solve_bodies_by_index(graph, mixedIsland, bodies);
    expectTrue(!bodiesByIndex.skipped, "solve bodies by index does not skip mixed island");
    expectTrue(bodiesByIndex.movableCount == 1u, "solve bodies by index counts movable body");

    const IslandSolveBodiesPreflight outOfRange =
        preflight_island_solve_bodies_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "solve bodies by index skips out-of-range island");
}

void testSolveIslandJobGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 activeIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(activeIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve succeeds for active island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
}

void testIslandWakeResultAndSleepDispatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    const u32 mixedIsland = graph.bodyIsland(0);
    const IslandWakeResult wakeResult =
        wake_island_sleepers_by_index_result(bodies, graph, mixedIsland);
    expectTrue(wakeResult.woke, "wake result activates mixed island sleepers");
    expectTrue(wakeResult.wokeBodyCount == 1u, "wake result counts woke bodies");
    expectTrue(!wakeResult.skipped, "wake result does not skip mixed island");

    const IslandBatchWakeResult batchWake = wake_all_island_sleepers_result(bodies, graph);
    expectTrue(batchWake.skipped, "batch wake skips when no wakeable islands remain");
    expectTrue(!batchWake.any_woke(), "batch wake reports no additional woke islands");

    bodies.flags[1] |= RB_SLEEPING;
    const IslandWakeResult repeatWake =
        wake_island_sleepers_result(bodies, graph.island(mixedIsland), mixedIsland);
    expectTrue(repeatWake.woke, "repeat wake result activates sleeper again");
    expectTrue(repeatWake.wokeBodyCount == 1u, "repeat wake result counts one body");

    const IslandSleepDispatchPreflight sleepDispatch =
        preflight_island_sleep_dispatch(graph, bodies, 1.f / 60.f);
    expectTrue(!sleepDispatch.skipped, "sleep dispatch preflight does not skip mixed graph");
    expectTrue(sleepDispatch.can_dispatch(), "sleep dispatch preflight can dispatch mixed graph");
    expectTrue(!should_skip_island_sleep_dispatch(graph, bodies, 1.f / 60.f),
               "should_skip sleep dispatch false for mixed graph");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const IslandSleepDispatchPreflight allSleepingDispatch =
        preflight_island_sleep_dispatch(graph, bodies, 1.f / 60.f);
    expectTrue(!allSleepingDispatch.can_dispatch(),
               "sleep dispatch preflight cannot dispatch all-sleeping graph");
    expectTrue(should_skip_island_sleep_dispatch(graph, bodies, 1.f / 60.f),
               "should_skip sleep dispatch true for all-sleeping graph");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    bodies.flags[2] |= RB_SLEEPING;
    bodies.flags[3] |= RB_SLEEPING;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;
    const IslandBatchDispatchResult wakeDispatch =
        dispatch_all_islands_with_wake_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!wakeDispatch.skipped, "wake dispatch batch does not skip mixed graph");
    expectTrue(wakeDispatch.solvedCount == 1u, "wake dispatch batch solves nonsleeping island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake dispatch batch wakes mixed island sleeper");
}

void testBodyFlagHelpers() {
    expectTrue(is_body_sleeping(RB_SLEEPING), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_sleeping(0u), "is_body_sleeping false for awake body");
    expectTrue(is_body_static_or_kinematic(RB_STATIC), "static flag detected");
    expectTrue(is_body_static_or_kinematic(RB_KINEMATIC), "kinematic flag detected");
    expectTrue(!is_body_static_or_kinematic(0u), "dynamic body is not static/kinematic");

    const u32 dynamic = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleeping = bodies.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 staticBody = bodies.addBody({2.f, 0.f, 0.f}, 0.f, RB_STATIC);
    expectTrue(is_body_movable(bodies, dynamic), "awake dynamic body is movable");
    expectTrue(!is_body_movable(bodies, sleeping), "sleeping body is not movable");
    expectTrue(!is_body_movable(bodies, staticBody), "static body is not movable");
    expectTrue(!is_body_movable(bodies, 99u), "out-of-range body is not movable");



    const std::vector<f32> priorDistance = {0.15f, 0.f};
    const std::vector<f32> priorContact = {0.25f, 0.f};

    IslandWarmStartPreflight emptyPreflight{};
    expectTrue(!preflight_warm_start_island(graph.island(graph.bodyIsland(3)),
                                            priorDistance,
                                            priorContact,
                                            emptyPreflight),
               "empty island preflight returns false");

    IslandWarmStartPreflight preflight{};
    expectTrue(preflight_warm_start_island(graph.island(islandA),
                                           preflight),
               "constrained island preflight returns true");
    expectTrue(preflight.hasDistanceLambdas, "preflight detects distance lambda seeds");
    expectTrue(preflight.hasContactLambdas, "preflight detects contact lambda seeds");
    expectTrue(preflight.hasContactImpulses, "preflight detects contact impulse seeds");
    expectTrue(preflight.distanceSlotCount == 1u, "preflight counts owned distance slots");
    expectTrue(preflight.contactSlotCount == 1u, "preflight counts owned contact slots");

    warm_start_island_lambdas_guarded(work,
                                      graph.island(islandA),
                                      preflight);
    warm_start_island_contact_impulses_guarded(work, graph.island(islandA), contacts, dt, preflight);
    expectNear(work.distanceLambdas()[0], 0.15f, 1e-6f, "guarded lambda warm-start seeds distance slot");
    expectNear(work.contactLambdas()[0], 0.25f, 1e-6f, "guarded lambda warm-start seeds contact slot");
               "guarded impulse warm-start contributes to contact lambda");

    IslandWarmStartPreflight zeroPreflight{};
    zeroPreflight.hasDistanceLambdas = false;
    zeroPreflight.hasContactLambdas = false;
    zeroPreflight.hasContactImpulses = false;
                                      zeroPreflight);
    expectNear(work.distanceLambdas()[0], 0.f, 1e-6f, "zero preflight skips lambda warm-start");

    expectTrue(preflight_and_warm_start_island(work,
                                               dt),
               "preflight_and_warm_start_island applies guarded seeds");
    expectNear(work.distanceLambdas()[0], 0.15f, 1e-6f,
               "preflight_and_warm_start_island seeds distance lambda");
    expectNear(work.contactLambdas()[0], 0.25f, 1e-6f,
               "preflight_and_warm_start_island seeds contact lambda");
    const f32 dt = 1.f / 60.f;
    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        work.clearLambdas();
        warm_start_island_contact_impulses(work, island, contacts, dt);
        expectNear(work.contactLambdas()[0], 0.f, 1e-6f,
                   "warm_start_island_contact_impulses early-outs on empty island");
    expectTrue(foundEmptySkip, "warm_start_island_contact_impulses empty-island early-out exercised");
}

void testIslandIndexDispatchableGuard() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    expectTrue(!island_index_dispatchable(graph, graph.islandCount()),
               "out-of-range island index is not dispatchable");
    expectTrue(island_index_dispatchable(graph, graph.bodyIsland(0)),
               "constrained island index is dispatchable");

    bool foundEmptyNotDispatchable = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        foundEmptyNotDispatchable = !island_index_dispatchable(graph, islandIndex);
        break;
    }
    expectTrue(foundEmptyNotDispatchable, "empty island index is not dispatchable");
}

void testBuildIslandSolveDispatchPlan() {
    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandSolveDispatchPlan emptyPlan = build_island_solve_dispatch_plan(emptyGraph);
    expectTrue(emptyPlan.preflight.skipped, "empty graph plan is skipped");
    expectTrue(!emptyPlan.can_dispatch(), "empty graph plan cannot dispatch");
    expectTrue(emptyPlan.dispatchIndices.empty(), "empty graph plan has no indices");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const IslandSolveDispatchPlan plan = build_island_solve_dispatch_plan(graph);
    expectTrue(!plan.preflight.skipped, "constrained graph plan is not skipped");
    expectTrue(plan.can_dispatch(), "constrained graph plan can dispatch");
    expectTrue(plan.dispatchIndices.size() == graph.constrainedIslandCount(),
               "plan indices match constrained island count");
    for (u32 islandIndex : plan.dispatchIndices) {
        expectTrue(island_index_dispatchable(graph, islandIndex),
                   "plan index passes island_index_dispatchable");
    }
}

void testDispatchIslandsFromPlan() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandSolveDispatchPlan plan = build_island_solve_dispatch_plan(graph);
    const u32 solvedCount = dispatch_islands_from_plan(bodies,
                                                       graph,
                                                       plan,
                                                       work,
                                                       constraints,
                                                       1.f / 60.f,
                                                       0.f,
                                                       invMassFn);
    expectTrue(solvedCount == graph.constrainedIslandCount(),
               "dispatch_islands_from_plan solves all constrained islands");

    IslandSolveDispatchPlan emptyPlan{};
    emptyPlan.preflight.skipped = true;
    expectTrue(dispatch_islands_from_plan(bodies,
                                          graph,
                                          emptyPlan,
                                          work,
                                          constraints,
                                          1.f / 60.f,
                                          0.f,
                                          invMassFn) == 0u,
               "dispatch_islands_from_plan early-outs on skipped plan");
}

void testComputeIslandWarmStartStats() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    const IslandWarmStartStats stats = compute_island_warm_start_stats(graph, priorDistance, priorContact);
    expectTrue(stats.totalIslands == graph.islandCount(), "warm-start stats report total islands");
    expectTrue(stats.warmStartableCount == graph.constrainedIslandCount(),
               "warm-start stats count constrained islands with prior data");
    expectTrue(stats.skippedCount + stats.warmStartableCount <= stats.totalIslands,
               "skipped and warm-startable partition total islands");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandWarmStartStats emptyStats = compute_island_warm_start_stats(emptyGraph, priorDistance, priorContact);
    expectTrue(emptyStats.warmStartableCount == 0u, "empty graph has no warm-startable islands");
    expectTrue(should_skip_frame_warm_start(emptyGraph, priorDistance, priorContact),
               "should_skip_frame_warm_start on empty graph");
}

void testCollectWarmStartableIslandIndices() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    const std::vector<u32> indices = collect_warm_startable_island_indices(graph, priorDistance, priorContact);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect_warm_startable_island_indices returns constrained count");
    for (u32 islandIndex : indices) {
        const IslandWarmStartPreflight preflight =
            preflight_warm_start_island(graph.island(islandIndex), priorDistance, priorContact);
        expectTrue(preflight.can_warm_start(), "collected index passes warm-start preflight");
    }

    expectTrue(collect_warm_startable_island_indices(graph, {}, {}).empty(),
               "collect_warm_startable_island_indices empty when no prior data");
    expectTrue(should_skip_frame_warm_start(graph, {}, {}),
               "should_skip_frame_warm_start when no prior data");
}

void testWarmStartAllIslandsGuarded() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const u32 seededCount = warm_start_all_islands_guarded(work, graph, priorDistance, priorContact);
    expectTrue(seededCount == graph.constrainedIslandCount(),
               "warm_start_all_islands_guarded seeds all constrained islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "batch warm-start seeds first distance slot");
    expectNear(work.distanceLambdas()[1], 0.22f, 1e-6f,
               "batch warm-start seeds second distance slot");
    expectNear(work.contactLambdas()[0], 0.33f, 1e-6f,
               "batch warm-start seeds first contact slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f,
               "batch warm-start seeds second contact slot");

    work.clearLambdas();
    expectTrue(warm_start_all_islands_guarded(work, graph, {}, {}) == 0u,
               "warm_start_all_islands_guarded early-outs with no prior data");
    expectNear(work.distanceLambdas()[0], 0.f, 1e-6f,
               "batch warm-start skip leaves distance lambda untouched");
}

void testShouldDispatchIslandIndexGuard() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    expectTrue(!should_dispatch_island_index(graph, graph.islandCount() + 1u),
               "should_dispatch_island_index guards out-of-range index");

    bool foundEmptySkip = false;
    bool foundConstrainedDispatch = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (extract_island(graph, islandIndex).empty) {
            foundEmptySkip = !should_dispatch_island_index(graph, islandIndex);
        } else {
            foundConstrainedDispatch = should_dispatch_island_index(graph, islandIndex);
        }
    }

    expectTrue(foundEmptySkip, "should_dispatch_island_index skips empty island");
    expectTrue(foundConstrainedDispatch, "should_dispatch_island_index allows constrained island");
}

void testDispatchAllIslandsBatchResult() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandDispatchBatchResult batch = dispatch_all_islands_batch(bodies,
                                                                     graph,
                                                                     work,
                                                                     constraints,
                                                                     1.f / 60.f,
                                                                     0.f,
                                                                     invMassFn);
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "batch dispatch solves all constrained islands");
    expectTrue(batch.skippedCount == 0u, "batch dispatch has no skips for constrained graph");
    expectTrue(batch.attemptedCount() == batch.solvedCount,
               "batch attempted count matches solved count");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandDispatchBatchResult emptyBatch = dispatch_all_islands_batch(bodies,
                                                                           emptyGraph,
                                                                           work,
                                                                           constraints,
                                                                           1.f / 60.f,
                                                                           0.f,
                                                                           invMassFn);
    expectTrue(emptyBatch.solvedCount == 0u, "batch dispatch early-outs on empty graph");
    expectTrue(emptyBatch.skippedCount == 0u, "batch dispatch reports zero skips on empty graph");
}

void testPreflightWarmStartGraphGuards() {
    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandWarmStartGraphPreflight emptyPreflight =
        preflight_warm_start_graph(emptyGraph, priorDistance, priorContact);
    expectTrue(emptyPreflight.skipped, "graph warm-start preflight skips empty graph");
    expectTrue(!emptyPreflight.can_warm_start(), "empty graph cannot warm-start");
    expectTrue(should_skip_warm_start_graph(emptyGraph, priorDistance, priorContact),
               "should_skip_warm_start_graph on empty graph");

    ContactIslandGraph loneBodies;
    loneBodies.build(2, {}, {});
    const IslandWarmStartGraphPreflight lonePreflight =
        preflight_warm_start_graph(loneBodies, priorDistance, priorContact);
    expectTrue(lonePreflight.skipped, "graph warm-start preflight skips lone bodies");
    expectTrue(lonePreflight.stats.emptyCount == loneBodies.islandCount(),
               "lone bodies count as empty islands");
    expectTrue(collect_warm_startable_island_indices(loneBodies, priorDistance, priorContact).empty(),
               "no warm-startable indices for lone bodies");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const IslandWarmStartStats stats = compute_island_warm_start_stats(graph, priorDistance, priorContact);
    expectTrue(stats.totalIslands == graph.islandCount(), "warm-start stats report total islands");
    expectTrue(stats.warmStartableCount == 2u, "two constrained islands can warm-start");
    expectTrue(stats.emptyCount + stats.warmStartableCount + stats.noPriorDataCount == stats.totalIslands,
               "warm-start stats partition all islands");
    expectTrue(count_warm_startable_islands(graph, priorDistance, priorContact) == stats.warmStartableCount,
               "count_warm_startable_islands matches stats");
    expectTrue(has_warm_startable_islands(graph, priorDistance, priorContact),
               "mixed graph has warm-startable islands");

    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "graph warm-start preflight does not skip constrained graph");
    expectTrue(preflight.can_warm_start(), "constrained graph can warm-start");
    expectTrue(!should_skip_warm_start_graph(graph, priorDistance, priorContact),
               "should_skip false when warm-startable islands exist");

    const std::vector<u32> indices = collect_warm_startable_island_indices(graph, priorDistance, priorContact);
    expectTrue(indices.size() == 2u, "collect returns warm-startable island count");
    for (u32 islandIndex : indices) {
        const IslandWarmStartPreflight islandPreflight =
            preflight_warm_start_island_by_index(graph, islandIndex, priorDistance, priorContact);
        expectTrue(islandPreflight.can_warm_start(), "collected index passes island preflight");
    }
}

void testWarmStartIslandByIndexGuarded() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.15f};
    const std::vector<f32> priorContact = {0.25f};

    work.clearLambdas();
    expectTrue(!warm_start_island_lambdas_by_index_guarded(work,
                                                             graph,
                                                             graph.islandCount() + 3u,
                                                             priorDistance,
                                                             priorContact),
               "index guarded warm-start skips out-of-range island");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (!graph.island(islandIndex).isEmpty()) {
            continue;
        }
        foundEmptySkip =
            !warm_start_island_lambdas_by_index_guarded(work, graph, islandIndex, priorDistance, priorContact);
        expectTrue(should_skip_warm_start_island_index(graph, islandIndex),
                   "should_skip_warm_start_island_index on empty island");
        break;
    }
    expectTrue(foundEmptySkip, "index guarded warm-start skips empty island");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    const IslandWarmStartResult result =
        warm_start_island_lambdas_result(work, graph, islandA, priorDistance, priorContact);
    expectTrue(result.warmed, "warm_start_island_lambdas_result seeds constrained island");
    expectTrue(!result.skipped, "warm_start_island_lambdas_result not skipped for constrained island");
    expectTrue(result.islandIndex == islandA, "warm_start result records island index");
    expectNear(work.distanceLambdas()[0], 0.15f, 1e-6f, "index result warm-start seeds distance slot");
    expectNear(work.contactLambdas()[0], 0.25f, 1e-6f, "index result warm-start seeds contact slot");
}

void testWarmStartAllIslandsGuarded() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const u32 warmedCount =
        warm_start_all_islands_guarded(work, graph, priorDistance, priorContact);
    expectTrue(warmedCount == 2u, "batch warm-start seeds all warm-startable islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f, "batch warm-start seeds first distance slot");
    expectNear(work.distanceLambdas()[1], 0.22f, 1e-6f, "batch warm-start seeds second distance slot");
    expectNear(work.contactLambdas()[0], 0.33f, 1e-6f, "batch warm-start seeds first contact slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f, "batch warm-start seeds second contact slot");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(warm_start_all_islands_guarded(work, emptyGraph, priorDistance, priorContact) == 0u,
               "batch warm-start early-outs on empty graph");
}

void testWarmStartIslandCombinedGuarded() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.2f};
    const std::vector<f32> priorContact = {0.3f};
    const f32 dt = 1.f / 60.f;

    work.clearLambdas();
    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = !warm_start_island_combined_guarded(work, island, contacts, dt, priorDistance, priorContact);
        break;
    }
    expectTrue(foundEmptySkip, "combined warm-start skips empty island");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_combined_guarded(work,
                                                  graph.island(islandA),
                                                  contacts,
                                                  dt,
                                                  priorDistance,
                                                  priorContact),
               "combined warm-start succeeds for contact+distance island");
    expectNear(work.distanceLambdas()[0], 0.2f, 1e-6f, "combined warm-start seeds distance lambda");
    expectNear(work.contactLambdas()[0], 0.3f, 1e-6f, "combined warm-start seeds contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "combined warm-start applies impulse seed when prior contact exists");
}

void testShouldDispatchIslandIndexGuard() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    expectTrue(!should_dispatch_island_index(graph, graph.islandCount() + 1u),
               "should_dispatch_island_index rejects out-of-range index");

    bool foundEmptyGuard = false;
    bool foundConstrainedGuard = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        const bool dispatchable = should_dispatch_island_index(graph, islandIndex);
        if (job.empty) {
            foundEmptyGuard = true;
            expectTrue(!dispatchable, "empty island index is not dispatchable");
        } else {
            foundConstrainedGuard = true;
            expectTrue(dispatchable, "constrained island index is dispatchable");
        }
    }
    expectTrue(foundEmptyGuard, "should_dispatch_island_index covers empty island");
    expectTrue(foundConstrainedGuard, "should_dispatch_island_index covers constrained island");

    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    const std::vector<IslandSolveJob> dispatchableJobs = filter_dispatchable_jobs(jobs);
    expectTrue(dispatchableJobs.size() == graph.constrainedIslandCount(),
               "filter_dispatchable_jobs keeps only constrained islands");
    for (const IslandSolveJob& job : dispatchableJobs) {
        expectTrue(should_solve_island(job), "filtered job passes should_solve_island");
    }
}

void testPreflightWarmStartGraphGuards() {
    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandWarmStartGraphPreflight emptyPreflight =
        preflight_warm_start_graph(emptyGraph, priorDistance, priorContact);
    expectTrue(emptyPreflight.skipped, "graph warm-start preflight skips empty graph");
    expectTrue(!emptyPreflight.can_warm_start(), "empty graph cannot warm-start");
    expectTrue(should_skip_warm_start_graph(emptyGraph, priorDistance, priorContact),
               "should_skip_warm_start_graph on empty graph");

    ContactIslandGraph loneBodies;
    loneBodies.build(2, {}, {});
    const IslandWarmStartGraphPreflight lonePreflight =
        preflight_warm_start_graph(loneBodies, priorDistance, priorContact);
    expectTrue(lonePreflight.skipped, "graph warm-start preflight skips lone bodies");
    expectTrue(lonePreflight.stats.emptyCount == loneBodies.islandCount(),
               "lone bodies count as empty islands");
    expectTrue(collect_warm_startable_island_indices(loneBodies, priorDistance, priorContact).empty(),
               "no warm-startable indices for lone bodies");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const IslandWarmStartStats stats = compute_island_warm_start_stats(graph, priorDistance, priorContact);
    expectTrue(stats.totalIslands == graph.islandCount(), "warm-start stats report total islands");
    expectTrue(stats.warmStartableCount == 2u, "two constrained islands can warm-start");
    expectTrue(stats.emptyCount + stats.warmStartableCount + stats.noPriorDataCount == stats.totalIslands,
               "warm-start stats partition all islands");
    expectTrue(count_warm_startable_islands(graph, priorDistance, priorContact) == stats.warmStartableCount,
               "count_warm_startable_islands matches stats");
    expectTrue(has_warm_startable_islands(graph, priorDistance, priorContact),
               "mixed graph has warm-startable islands");

    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "graph warm-start preflight does not skip constrained graph");
    expectTrue(preflight.can_warm_start(), "constrained graph can warm-start");
    expectTrue(!should_skip_warm_start_graph(graph, priorDistance, priorContact),
               "should_skip false when warm-startable islands exist");

    const std::vector<u32> indices = collect_warm_startable_island_indices(graph, priorDistance, priorContact);
    expectTrue(indices.size() == 2u, "collect returns warm-startable island count");
    for (u32 islandIndex : indices) {
        const IslandWarmStartPreflight islandPreflight =
            preflight_warm_start_island_by_index(graph, islandIndex, priorDistance, priorContact);
        expectTrue(islandPreflight.can_warm_start(), "collected index passes island preflight");
    }
}

void testWarmStartIslandByIndexGuarded() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.15f};
    const std::vector<f32> priorContact = {0.25f};

    work.clearLambdas();
    expectTrue(!warm_start_island_lambdas_by_index_guarded(work,
                                                             graph,
                                                             graph.islandCount() + 3u,
                                                             priorDistance,
                                                             priorContact),
               "index guarded warm-start skips out-of-range island");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (!graph.island(islandIndex).isEmpty()) {
            continue;
        }
        foundEmptySkip =
            !warm_start_island_lambdas_by_index_guarded(work, graph, islandIndex, priorDistance, priorContact);
        expectTrue(should_skip_warm_start_island_index(graph, islandIndex),
                   "should_skip_warm_start_island_index on empty island");
        break;
    }
    expectTrue(foundEmptySkip, "index guarded warm-start skips empty island");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    const IslandWarmStartResult result =
        warm_start_island_lambdas_result(work, graph, islandA, priorDistance, priorContact);
    expectTrue(result.warmed, "warm_start_island_lambdas_result seeds constrained island");
    expectTrue(!result.skipped, "warm_start_island_lambdas_result not skipped for constrained island");
    expectTrue(result.islandIndex == islandA, "warm_start result records island index");
    expectNear(work.distanceLambdas()[0], 0.15f, 1e-6f, "index result warm-start seeds distance slot");
    expectNear(work.contactLambdas()[0], 0.25f, 1e-6f, "index result warm-start seeds contact slot");
}

void testWarmStartAllIslandsGuarded() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const u32 warmedCount =
        warm_start_all_islands_guarded(work, graph, priorDistance, priorContact);
    expectTrue(warmedCount == 2u, "batch warm-start seeds all warm-startable islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f, "batch warm-start seeds first distance slot");
    expectNear(work.distanceLambdas()[1], 0.22f, 1e-6f, "batch warm-start seeds second distance slot");
    expectNear(work.contactLambdas()[0], 0.33f, 1e-6f, "batch warm-start seeds first contact slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f, "batch warm-start seeds second contact slot");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(warm_start_all_islands_guarded(work, emptyGraph, priorDistance, priorContact) == 0u,
               "batch warm-start early-outs on empty graph");
}

void testWarmStartIslandCombinedGuarded() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.2f};
    const std::vector<f32> priorContact = {0.3f};
    const f32 dt = 1.f / 60.f;

    work.clearLambdas();
    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = !warm_start_island_combined_guarded(work, island, contacts, dt, priorDistance, priorContact);
        break;
    }
    expectTrue(foundEmptySkip, "combined warm-start skips empty island");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_combined_guarded(work,
                                                  graph.island(islandA),
                                                  contacts,
                                                  dt,
                                                  priorDistance,
                                                  priorContact),
               "combined warm-start succeeds for contact+distance island");
    expectNear(work.distanceLambdas()[0], 0.2f, 1e-6f, "combined warm-start seeds distance lambda");
    expectNear(work.contactLambdas()[0], 0.3f, 1e-6f, "combined warm-start seeds contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "combined warm-start applies impulse seed when prior contact exists");
}

void testIsDispatchableIslandIndexGuard() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    expectTrue(is_dispatchable_island_index(graph, graph.bodyIsland(0)),
               "constrained island index is dispatchable");
    expectTrue(!is_dispatchable_island_index(graph, graph.islandCount()),
               "at-limit island index is not dispatchable");
    expectTrue(!is_dispatchable_island_index(graph, graph.islandCount() + 3u),
               "out-of-range island index is not dispatchable");

    bool foundEmptyIndex = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        foundEmptyIndex = true;
        expectTrue(!is_dispatchable_island_index(graph, islandIndex),
                   "empty island index is not dispatchable");
        break;
    }
    expectTrue(foundEmptyIndex, "graph exposes empty island for dispatchable index guard");
}

void testFilterDispatchableJobs() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<IslandSolveJob> allJobs = extract_island_jobs(graph);
    const std::vector<IslandSolveJob> dispatchable = filter_dispatchable_jobs(allJobs);

    expectTrue(dispatchable.size() == graph.constrainedIslandCount(),
               "filter_dispatchable_jobs keeps constrained islands only");
    expectTrue(dispatchable.size() < allJobs.size(),
               "filter_dispatchable_jobs drops empty islands");
    for (const IslandSolveJob& job : dispatchable) {
        expectTrue(should_solve_island(job), "filtered job passes should_solve_island");
        expectTrue(!job.empty, "filtered job is not empty");
    }

    IslandSolveJob invalid{};
    expectTrue(filter_dispatchable_jobs({invalid}).empty(),
               "filter_dispatchable_jobs drops invalid jobs");
}

void testDispatchSolveIslandJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    IslandSolveJob invalid{};
    expectTrue(!dispatch_solve_island_job(bodies, invalid, work, constraints, dt, 0.f, invMassFn),
               "dispatch_solve_island_job skips invalid job");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        foundEmptySkip =
            !dispatch_solve_island_job(bodies, job, work, constraints, dt, 0.f, invMassFn);
        break;
    }
    expectTrue(foundEmptySkip, "dispatch_solve_island_job skips empty island job");

    const IslandSolveJob constrained = extract_island(graph, graph.bodyIsland(0));
    expectTrue(dispatch_solve_island_job(bodies, constrained, work, constraints, dt, 0.f, invMassFn),
               "dispatch_solve_island_job solves constrained island job");
    expectTrue(!dispatch_solve_island_job(bodies, constrained, work, constraints, 0.f, 0.f, invMassFn),
               "dispatch_solve_island_job guards invalid dt");
}

void testDispatchIslandJobsResultBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({40.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(5, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    const IslandBatchDispatchResult batch = dispatch_island_jobs_result(bodies,
                                                                        graph,
                                                                        jobs,
                                                                        work,
                                                                        constraints,
                                                                        dt,
                                                                        0.f,
                                                                        invMassFn);
    expectTrue(!batch.skipped, "job batch dispatch does not skip constrained graph");
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "job batch dispatch solves all constrained islands");
    expectTrue(batch.dispatchableCount == graph.constrainedIslandCount(),
               "job batch dispatch records dispatchable count");

    const IslandBatchDispatchResult invalidDtBatch = dispatch_island_jobs_result(bodies,
                                                                                graph,
                                                                                jobs,
                                                                                work,
                                                                                constraints,
                                                                                0.f,
                                                                                0.f,
                                                                                invMassFn);
    expectTrue(invalidDtBatch.skipped, "job batch dispatch skips invalid dt");
    expectTrue(invalidDtBatch.invalidDt, "job batch dispatch records invalid dt");
    expectTrue(invalidDtBatch.solvedCount == 0u, "invalid dt batch solves zero islands");
}

void testPreflightGraphWarmStartGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    const GraphWarmStartPreflight preflight =
        preflight_graph_warm_start(graph, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "graph preflight does not skip when prior data exists");
    expectTrue(preflight.can_warm_start(), "graph preflight can warm-start with prior data");
    expectTrue(preflight.dispatchableCount == graph.constrainedIslandCount(),
               "graph preflight counts dispatchable islands");
    expectTrue(preflight.skippedEmptyCount > 0u, "graph preflight counts empty islands");
    expectTrue(preflight.priorDistanceCoverage == 2u,
               "graph preflight aggregates prior distance coverage");
    expectTrue(preflight.priorContactCoverage == 1u,
               "graph preflight aggregates non-zero prior contact coverage");

    const GraphWarmStartPreflight emptyPrior = preflight_graph_warm_start(graph, {}, {});
    expectTrue(emptyPrior.skipped, "graph preflight skips when no prior data exists");
    expectTrue(!emptyPrior.can_warm_start(), "graph preflight cannot warm-start without prior data");
    expectTrue(should_skip_graph_warm_start({}, {}), "should_skip_graph_warm_start on empty priors");
    expectTrue(!should_skip_graph_warm_start(priorDistance, priorContact),
               "should_skip false when prior data exists");
}

void testWarmStartAllIslandsResultBatch() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const IslandWarmStartBatchResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "batch warm-start does not skip when prior data exists");
    expectTrue(batch.any_warmed(), "batch warm-start reports warmed islands");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "batch warm-start seeds all constrained islands");
    expectTrue(batch.skippedEmptyCount > 0u, "batch warm-start counts skipped empty islands");
    expectTrue(!batch.skipped, "warm_start_all_islands_result does not skip constrained graph");
               "warm_start_all_islands_result seeds all constrained islands");
    expectTrue(batch.any_warmed(), "warm_start batch reports warmed islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "batch warm-start seeds first distance slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f,
               "batch warm-start seeds second contact slot");

    work.clearLambdas();
    const IslandWarmStartBatchResult emptyBatch = warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyBatch.skipped, "batch warm-start skips when no prior data exists");
    expectTrue(!emptyBatch.any_warmed(), "empty prior batch reports no warmed islands");
    expectTrue(emptyBatch.skippedNoPriorCount == graph.constrainedIslandCount(),
               "empty prior batch counts constrained islands as skipped");
}

void testWarmStartDispatchableIslandsGuarded() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    work.clearLambdas();
    const u32 warmedCount =
        warm_start_dispatchable_islands_guarded(work, graph, priorDistance, priorContact);
    expectTrue(warmedCount == graph.constrainedIslandCount(),
               "dispatchable warm-start seeds all constrained islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "dispatchable warm-start seeds owned distance slot");
    expectNear(work.distanceLambdas()[1], 0.22f, 1e-6f,
               "dispatchable warm-start seeds remote distance slot on second island");

    expectTrue(warm_start_dispatchable_islands_guarded(work, graph, {}, {}) == 0u,
               "dispatchable warm-start skips when no prior data exists");
}

void testEmptyIslandGraphGuards() {
    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(empty_island_count(emptyGraph) == 0u, "empty graph reports zero empty islands");
    expectTrue(!graph_has_empty_islands(emptyGraph), "empty graph has no empty islands");
    expectTrue(!graph_has_constrained_islands(emptyGraph), "empty graph has no constrained islands");

    ContactIslandGraph loneBodies;
    loneBodies.build(2, {}, {});
    expectTrue(empty_island_count(loneBodies) == loneBodies.islandCount(),
               "lone bodies are all empty islands");
    expectTrue(graph_has_empty_islands(loneBodies), "lone bodies graph has empty islands");
    expectTrue(!graph_has_constrained_islands(loneBodies),
               "lone bodies graph has no constrained islands");
    expectTrue(all_islands_empty(loneBodies), "all islands empty for unconstrained bodies");

    ContactIslandGraph mixed;
    mixed.build(3, contacts, constraints);
    expectTrue(graph_has_empty_islands(mixed), "mixed graph has empty islands");
    expectTrue(graph_has_constrained_islands(mixed), "mixed graph has constrained islands");
    expectTrue(empty_island_count(mixed) + mixed.constrainedIslandCount() == mixed.islandCount(),
               "empty and constrained islands partition total");

void testShouldSkipIslandSolveIndexGuard() {
void testPreflightDispatchIslandJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    expectTrue(should_skip_island_solve_index(graph, graph.islandCount() + 2u),
               "should_skip_island_solve_index on out-of-range index");

    bool foundEmptySkip = false;
    bool foundConstrainedDispatch = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (extract_island(graph, islandIndex).empty) {
            foundEmptySkip = should_skip_island_solve_index(graph, islandIndex);
        } else {
            foundConstrainedDispatch = !should_skip_island_solve_index(graph, islandIndex);
        }

    expectTrue(foundEmptySkip, "should_skip_island_solve_index skips empty island");
    expectTrue(foundConstrainedDispatch, "should_skip_island_solve_index allows constrained island");

void testDispatchSolveIslandJobGuard() {
    const IslandSolveJob invalid{};
    const IslandDispatchJobPreflight invalidPreflight = preflight_dispatch_island_job(invalid, 1.f / 60.f);
    expectTrue(invalidPreflight.emptyJob, "default job preflight marks empty job");
    expectTrue(!invalidPreflight.can_dispatch(), "default job cannot dispatch");
    expectTrue(should_skip_dispatch_island_job(invalid, 1.f / 60.f),
               "should_skip_dispatch_island_job on default job");

        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            continue;
        foundConstrainedDispatch = true;
        const IslandDispatchJobPreflight preflight = preflight_dispatch_island_job(job, 1.f / 60.f);
        expectTrue(!preflight.emptyJob, "constrained job preflight is not empty");
        expectTrue(preflight.can_dispatch(), "constrained job can dispatch with valid dt");

        const IslandDispatchJobPreflight invalidDt = preflight_dispatch_island_job(job, 0.f);
        expectTrue(invalidDt.invalidDt, "constrained job preflight rejects invalid dt");
        expectTrue(!invalidDt.can_dispatch(), "constrained job blocked with invalid dt");
    expectTrue(foundConstrainedDispatch, "dispatch job preflight covers constrained island");

void testDispatchSolveIslandJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    IslandSolveJob invalid{};
    expectTrue(!dispatch_solve_island_job(bodies,
                                          invalid,
                                          work,
                                          constraints,
                                          1.f / 60.f,
                                          0.f,
                                          invMassFn),
               "dispatch_solve_island_job skips invalid job");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        foundEmptySkip = !dispatch_solve_island_job(bodies,
                                                    job,
                                                    invMassFn);
        break;
    expectTrue(foundEmptySkip, "dispatch_solve_island_job skips empty island job");

    const IslandSolveJob constrained = extract_island(graph, graph.bodyIsland(0));
    expectTrue(dispatch_solve_island_job(bodies,
                                         constrained,

    work.init(2, 0, 1);

    const IslandSolveJob job = extract_island(graph, 0);
                                         graph,
                                         work,
                                         constraints,
                                         1.f / 60.f,
                                         0.f,
                                         invMassFn),
               "dispatch_solve_island_job resolves constrained job");
}

void testDispatchIslandsAtIndices() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({40.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(5, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const std::vector<u32> indices = collect_dispatchable_island_indices(graph);
    const IslandBatchDispatchResult batch = dispatch_islands_at_indices_result(bodies,
                                                                                 graph,
                                                                                 indices,
                                                                                 work,
                                                                                 constraints,
                                                                                 1.f / 60.f,
                                                                                 0.f,
                                                                                 invMassFn);
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "dispatch_islands_at_indices_result solves constrained islands");
    expectTrue(batch.any_solved(), "dispatch at indices reports solved islands");

    const std::vector<u32> mixedIndices = {
        graph.islandCount() + 9u,
        graph.bodyIsland(0),
        graph.bodyIsland(4),
    const IslandBatchDispatchResult mixedBatch = dispatch_islands_at_indices_result(bodies,
                                                                                    mixedIndices,
    expectTrue(mixedBatch.solvedCount == 1u, "mixed index list solves one constrained island");
    expectTrue(mixedBatch.skippedCount >= 2u, "mixed index list skips invalid and empty indices");

    expectTrue(dispatch_islands_at_indices(bodies,
                                           {},
                                           invMassFn) == 0u,
               "dispatch_islands_at_indices on empty list solves zero islands");

void testExtractWarmStartJobsAndFilters() {
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;


    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.f};

    const std::vector<IslandWarmStartJob> jobs = extract_warm_start_jobs(graph, priorDistance, priorContact);
    expectTrue(jobs.size() == graph.islandCount(), "extract_warm_start_jobs matches island count");

    u32 warmStartableJobs = 0;
    for (const IslandWarmStartJob& job : jobs) {
        expectTrue(job.island != nullptr, "warm-start job binds island pointer");
        if (job.empty) {
            expectTrue(!should_warm_start_island(job), "empty warm-start job is not warm-startable");
            expectTrue(should_skip_warm_start_island_job(job), "empty warm-start job is skipped");
        } else if (job.canWarmStart) {
            ++warmStartableJobs;
            expectTrue(should_warm_start_island(job), "warm-startable job passes should_warm_start_island");

    const std::vector<IslandWarmStartJob> filtered = filter_warm_startable_jobs(jobs);
    expectTrue(filtered.size() == warmStartableJobs, "filter_warm_startable_jobs keeps warm-startable jobs");
    expectTrue(filtered.size() == graph.constrainedIslandCount(),
               "filtered warm-start jobs match constrained island count");

    const IslandWarmStartJob outOfRange =
        extract_warm_start_job(graph, graph.islandCount() + 1u, priorDistance, priorContact);
    expectTrue(outOfRange.empty, "out-of-range warm-start job is empty");
    expectTrue(should_skip_warm_start_island_job(outOfRange), "out-of-range warm-start job is skipped");

void testWarmStartAllIslandsResultBatch() {
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;


    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const IslandWarmStartBatchResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "warm_start_all_islands_result does not skip with prior data");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "warm_start_all_islands_result seeds all constrained islands");
    expectTrue(batch.any_warmed(), "warm_start batch reports warmed islands");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "warm_start batch records warm-startable count");

    const IslandWarmStartBatchResult emptyPrior = warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyPrior.skipped, "warm_start_all_islands_result skips without prior data");
    expectTrue(!emptyPrior.any_warmed(), "empty prior batch reports no warmed islands");

void testWarmStartIslandJobGuarded() {
    work.init(4, 1, 1);
    work.ensureLambdaCapacity(1, 1);


    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.17f};
    const std::vector<f32> priorContact = {0.29f};

    IslandWarmStartJob invalid{};
    expectTrue(!warm_start_island_job_guarded(work, invalid, priorDistance, priorContact),
               "warm_start_island_job_guarded skips invalid job");

    const IslandWarmStartJob constrained =
        extract_warm_start_job(graph, graph.bodyIsland(0), priorDistance, priorContact);
    expectTrue(warm_start_island_job_guarded(work, constrained, priorDistance, priorContact),
               "warm_start_island_job_guarded seeds constrained job");
    expectNear(work.distanceLambdas()[0], 0.17f, 1e-6f,
               "job guarded warm-start seeds owned distance slot");

void testWarmStartIslandContactImpulsesResult() {
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    contacts.back().warmNormalImpulse = 5.f;

    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 1u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range impulse result is not warmed");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (!graph.island(islandIndex).isEmpty()) {
            continue;
        const IslandWarmStartResult emptyResult =
            warm_start_island_contact_impulses_result(work, graph, islandIndex, contacts, dt);
        expectTrue(emptyResult.skipped, "impulse result skips empty island");
        foundEmptySkip = true;
        break;
    expectTrue(foundEmptySkip, "impulse result covers empty island skip");

    const u32 islandA = graph.bodyIsland(0);
    const IslandWarmStartResult warmed =
        warm_start_island_contact_impulses_result(work, graph, islandA, contacts, dt);
    expectTrue(warmed.warmed, "impulse result seeds constrained island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "impulse result writes contact lambda");





    expectTrue(!batch.skipped, "batch warm-start does not skip constrained graph");
               "batch warm-start records warm-startable count");
               "batch warm-start seeds all constrained islands");
    expectTrue(batch.any_warmed(), "batch warm-start reports warmed islands");

    expectTrue(emptyPrior.skipped, "batch warm-start skips when no prior data exists");
               "dispatch_solve_island_job resolves constrained island");

    const IslandDispatchResult invalidDt = dispatch_solve_island_job_result(bodies,
                                                                            job,
    expectTrue(invalidDt.skipped, "dispatch_solve_island_job_result skips invalid dt");
    expectTrue(!invalidDt.solved, "dispatch_solve_island_job_result does not solve invalid dt");
}

void testPreflightContactImpulseWarmStartGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulseWarmStartPreflight constrainedPreflight =
        preflight_contact_impulse_warm_start_island(graph.island(islandA), contacts);
    expectTrue(!constrainedPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(constrainedPreflight.ownedContactCount == 1u,
               "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.nonZeroImpulseCoverage == 1u,
               "impulse preflight counts non-zero impulse coverage");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can warm-start impulses");

    const IslandContactImpulseWarmStartPreflight outOfRange =
        preflight_contact_impulse_warm_start_island_by_index(graph, graph.islandCount() + 1u, contacts);
    expectTrue(outOfRange.skipped, "index impulse preflight skips out-of-range island");
    expectTrue(should_skip_contact_impulse_warm_start_island_index(graph, graph.islandCount() + 1u),
               "should_skip_contact_impulse_warm_start_island_index on out-of-range index");
    const f32 dt = 1.f / 60.f;
    expectTrue(is_valid_contact_impulse_warm_start_dt(dt), "positive dt valid for impulse warm-start");
    expectTrue(!is_valid_contact_impulse_warm_start_dt(0.f), "zero dt invalid for impulse warm-start");
    expectTrue(contact_has_warm_impulse(contacts[0]), "non-zero impulse passes contact_has_warm_impulse");
    expectTrue(!contact_has_warm_impulse(contacts[1]), "zero impulse fails contact_has_warm_impulse");

    const IslandContactImpulsePreflight constrainedPreflight =
        preflight_warm_start_island_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(constrainedPreflight.ownedContactCount == 1u, "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.impulseCoverage == 1u, "impulse preflight counts warm impulses");

    const IslandContactImpulsePreflight invalidDt =
        preflight_warm_start_island_contact_impulses(graph.island(islandA), contacts, 0.f);
    expectTrue(invalidDt.invalidDt, "impulse preflight rejects invalid dt");
    expectTrue(!invalidDt.can_warm_start(), "impulse warm-start blocked with invalid dt");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulseWarmStartPreflight emptyPreflight =
            preflight_contact_impulse_warm_start_island(island, contacts);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_contact_impulse_warm_start_island(island),
                   "should_skip_contact_impulse_warm_start_island on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse warm-start preflight");

void testWarmStartIslandContactImpulsesResultAndBatch() {
        const IslandContactImpulsePreflight emptyPreflight =
            preflight_warm_start_island_contact_impulses(island, contacts, dt);
        expectTrue(should_skip_warm_start_contact_impulses_island(island),
                   "should_skip_warm_start_contact_impulses_island on empty island");
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");

    const IslandContactImpulseGraphPreflight graphPreflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!graphPreflight.skipped, "graph impulse preflight does not skip when impulses exist");
    expectTrue(graphPreflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(has_impulse_warm_startable_islands(graph, contacts),
               "has_impulse_warm_startable_islands true when impulses exist");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip false when impulse islands exist");

void testWarmStartContactImpulsesResultAndBatch() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 4.f;
    contacts.back().warmNormalImpulse = 1.5f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandWarmStartResult outOfRange =
    const IslandContactImpulseResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 2u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range impulse result is not warmed");

    work.clearLambdas();
    const IslandContactImpulseBatchResult batch =
        warm_start_all_islands_contact_impulses_result(work, graph, contacts, dt);
    expectTrue(!batch.skipped, "impulse batch does not skip when impulses exist");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "impulse batch seeds all constrained islands");
    expectTrue(batch.any_warmed(), "impulse batch reports warmed islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "impulse batch seeds first contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "impulse batch seeds second contact lambda");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, islandA, contacts, dt),
               "index guarded impulse warm-start succeeds for contact island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "index guarded impulse warm-start seeds contact lambda");

    work.clearLambdas();
    const std::vector<u32> indices = collect_contact_impulse_warm_startable_island_indices(graph, contacts);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect_contact_impulse_warm_startable_island_indices returns constrained count");

    const u32 warmedCount = warm_start_all_contact_impulses_guarded(work, graph, contacts, dt);
    expectTrue(warmedCount == graph.constrainedIslandCount(),
               "warm_start_all_contact_impulses_guarded seeds all impulse islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "batch impulse warm-start seeds first contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "batch impulse warm-start seeds second contact lambda");
}

void testPreflightWarmStartIslandCombinedGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.15f};
    const std::vector<f32> priorContact = {0.25f};

    const u32 islandA = graph.bodyIsland(0);
    const IslandCombinedWarmStartPreflight combinedPreflight =
        preflight_warm_start_island_combined(graph.island(islandA), contacts, priorDistance, priorContact);
    expectTrue(!combinedPreflight.skipped, "combined preflight does not skip constrained island");
    expectTrue(combinedPreflight.lambdas.can_warm_start(), "combined preflight sees prior lambda data");
    expectTrue(combinedPreflight.impulses.can_warm_start(), "combined preflight sees non-zero impulses");
    expectTrue(combinedPreflight.can_warm_start(), "combined island can warm-start");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        foundEmptySkip = true;
        const IslandCombinedWarmStartPreflight emptyPreflight =
            preflight_warm_start_island_combined(island, contacts, priorDistance, priorContact);
        expectTrue(emptyPreflight.skipped, "combined preflight skips empty island");
        expectTrue(!emptyPreflight.can_warm_start(), "empty island cannot combined warm-start");
    expectTrue(foundEmptySkip, "graph exposes empty island for combined warm-start preflight");

void testIsValidWarmStartDtGuard() {
    expectTrue(is_valid_warm_start_dt(1.f / 60.f), "positive dt is valid for impulse warm-start");
    expectTrue(!is_valid_warm_start_dt(0.f), "zero dt is invalid for impulse warm-start");
    expectTrue(!is_valid_warm_start_dt(-1.f / 60.f), "negative dt is invalid for impulse warm-start");

void testShouldSkipIslandDispatchJobGuard() {
    IslandSolveJob invalid{};
    expectTrue(should_skip_island_dispatch_job(invalid, 1.f / 60.f),
               "default job is skipped for dispatch");
    expectTrue(should_skip_island_dispatch_job(invalid, 0.f),
               "default job is skipped even without dt check first");


    bool foundConstrainedDispatch = false;
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            foundEmptySkip = should_skip_island_dispatch_job(job, 1.f / 60.f);
        } else {
            foundConstrainedDispatch = !should_skip_island_dispatch_job(job, 1.f / 60.f);
            expectTrue(should_skip_island_dispatch_job(job, 0.f),
                       "constrained job skipped when dt is invalid");

    expectTrue(foundEmptySkip, "should_skip_island_dispatch_job covers empty island");
    expectTrue(foundConstrainedDispatch, "should_skip_island_dispatch_job allows constrained island");

void testDispatchSolveIslandJobGuards() {

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    expectTrue(!dispatch_solve_island_job(bodies,
                                          invalid,
                                          work,
                                          constraints,
                                          dt,
                                          0.f,
                                          invMassFn),
               "job dispatch skips default job");

    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    bool foundConstrainedSolve = false;
    for (const IslandSolveJob& job : jobs) {
            foundEmptySkip = !dispatch_solve_island_job(bodies,
                                                        job,
                                                        invMassFn);
            const IslandDispatchResult emptyResult = dispatch_solve_island_job_result(bodies,
            expectTrue(emptyResult.skipped, "job dispatch result skips empty island");
            foundConstrainedSolve = dispatch_solve_island_job(bodies,
            const IslandDispatchResult solvedResult = dispatch_solve_island_job_result(bodies,
            expectTrue(solvedResult.solved, "job dispatch result solves constrained island");
            expectTrue(solvedResult.islandIndex == job.islandIndex,
                       "job dispatch result records island index");

    expectTrue(foundEmptySkip, "job dispatch covers empty island skip");
    expectTrue(foundConstrainedSolve, "job dispatch solves constrained island");

void testDispatchDispatchableJobsBatch() {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    graph.build(4, contacts, constraints);

    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);

    work.init(4, 0, 2);

    const IslandBatchDispatchResult batch = dispatch_dispatchable_jobs(bodies,
                                                                       jobs,
    expectTrue(!batch.skipped, "dispatch_dispatchable_jobs does not skip constrained graph");
    expectTrue(batch.dispatchableCount == graph.constrainedIslandCount(),
               "dispatch_dispatchable_jobs records dispatchable count");
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "dispatch_dispatchable_jobs solves all constrained islands");
    expectTrue(batch.any_solved(), "dispatch_dispatchable_jobs reports solved islands");

    const IslandBatchDispatchResult invalidDtBatch = dispatch_dispatchable_jobs(bodies,
    expectTrue(invalidDtBatch.skipped, "dispatch_dispatchable_jobs skips invalid dt");
    expectTrue(invalidDtBatch.solvedCount == 0u, "invalid dt batch solves zero islands");

    const IslandBatchDispatchResult emptyBatch = dispatch_dispatchable_jobs(bodies,
                                                                            {invalid},
    expectTrue(emptyBatch.skipped, "dispatch_dispatchable_jobs skips when no dispatchable jobs");

void testWarmStartAllIslandsResultBatch() {
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    const IslandWarmStartBatchResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "warm_start_all_islands_result does not skip constrained graph");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "warm_start_all_islands_result records warm-startable count");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "warm_start_all_islands_result warms all constrained islands");
    expectTrue(batch.any_warmed(), "warm_start_all_islands_result reports warmed islands");

    const IslandWarmStartBatchResult emptyPrior =
        warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyPrior.skipped, "warm_start_all_islands_result skips without prior data");
    expectTrue(!emptyPrior.any_warmed(), "empty prior batch reports no warmed islands");

void testPreflightWarmStartContactImpulsesGuards() {
    contacts.back().warmNormalImpulse = 2.5f;

    graph.build(3, contacts, {});

        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(island, contacts, dt);
        expectTrue(preflight.skipped, "impulse preflight skips empty island");
        expectTrue(!preflight.can_warm_start(), "empty island cannot impulse warm-start");
        expectTrue(should_skip_warm_start_contact_impulses(island, dt),
                   "should_skip_warm_start_contact_impulses on empty island");
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");

    const IslandContactImpulseWarmStartPreflight constrainedPreflight =
        preflight_warm_start_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(!constrainedPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(!constrainedPreflight.invalidDt, "valid dt passes impulse preflight");
    expectTrue(constrainedPreflight.ownedContactCount == 1u,
               "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can impulse warm-start");

    const IslandContactImpulseWarmStartPreflight invalidDtPreflight =
        preflight_warm_start_contact_impulses(graph.island(islandA), contacts, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "zero dt fails impulse preflight");
    expectTrue(!invalidDtPreflight.can_warm_start(), "impulse warm-start blocked for invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(islandA), 0.f),
               "should_skip_warm_start_contact_impulses on invalid dt");

void testWarmStartGraphContactImpulsesGuarded() {
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    contacts.back().warmNormalImpulse = 3.f;
    contacts.back().warmNormalImpulse = 4.f;

    graph.build(4, contacts, {});

    const u32 warmedCount = warm_start_graph_contact_impulses_guarded(work, graph, contacts, dt);
               "graph impulse warm-start seeds all contact islands");
               "graph impulse warm-start seeds first contact lambda");
               "graph impulse warm-start seeds second contact lambda");

    expectTrue(warm_start_graph_contact_impulses_guarded(work, graph, contacts, 0.f) == 0u,
               "graph impulse warm-start skips invalid dt");

    expectTrue(!warm_start_island_contact_impulses_by_index_guarded(work,
                                                                    graph,
                                                                    graph.islandCount() + 1u,
                                                                    contacts,
                                                                    dt),
               "index impulse warm-start guards out-of-range island");

    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, islandA, contacts, dt),
               "index impulse warm-start succeeds for contact island");

void testWarmStartIslandCombinedResultOutcomes() {
    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(warm_start_all_islands_contact_impulses_guarded(work, emptyGraph, contacts, dt) == 0u,
               "impulse batch guarded skips empty graph");
    expectTrue(should_skip_warm_start_contact_impulses_graph(emptyGraph, contacts, dt),
               "should_skip impulse graph warm-start on empty graph");

void testWarmStartIslandCombinedGuarded() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const IslandWarmStartResult outOfRange =
        warm_start_island_combined_result(work,
                                          graph,
                                          graph.islandCount() + 2u,
                                          contacts,
                                          dt,
                                          priorDistance,
                                          priorContact);
    expectTrue(outOfRange.skipped, "combined result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range combined result is not warmed");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (!graph.island(islandIndex).isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandWarmStartResult emptyResult =
            warm_start_island_combined_result(work,
                                              graph,
                                              islandIndex,
                                              contacts,
                                              dt,
                                              priorDistance,
                                              priorContact);
        expectTrue(emptyResult.skipped, "combined result skips empty island");
        expectTrue(!emptyResult.warmed, "empty island combined result is not warmed");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for combined result");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    const IslandWarmStartResult warmed =
        warm_start_island_combined_result(work,
                                          graph,
                                          islandA,
                                          contacts,
                                          dt,
                                          priorDistance,
                                          priorContact);
    expectTrue(warmed.warmed, "combined result warms constrained island");
    expectTrue(!warmed.skipped, "constrained island combined result is not skipped");
    expectTrue(warmed.islandIndex == islandA, "combined result records island index");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "combined result seeds prior contact lambda");
}

void testWarmStartAllIslandsResultBatch() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const IslandWarmStartBatchResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "batch warm-start does not skip constrained graph");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "batch warm-start records warm-startable count");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "batch warm-start seeds all constrained islands");
    expectTrue(batch.any_warmed(), "batch warm-start reports warmed islands");

    work.clearLambdas();
    const IslandWarmStartBatchResult emptyPrior = warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyPrior.skipped, "batch warm-start skips when no prior data exists");
    expectTrue(!emptyPrior.any_warmed(), "empty prior batch reports no warmed islands");
}

void testPreflightContactImpulseWarmStartGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulseWarmStartPreflight constrainedPreflight =
        preflight_contact_impulse_warm_start_island(graph.island(islandA), contacts);
    expectTrue(!constrainedPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(constrainedPreflight.ownedContactCount == 1u,
               "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.nonZeroImpulseCoverage == 1u,
               "impulse preflight counts non-zero impulse coverage");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can warm-start impulses");

    const IslandContactImpulseWarmStartPreflight outOfRange =
        preflight_contact_impulse_warm_start_island_by_index(graph, graph.islandCount() + 1u, contacts);
    expectTrue(outOfRange.skipped, "index impulse preflight skips out-of-range island");
    expectTrue(should_skip_contact_impulse_warm_start_island_index(graph, graph.islandCount() + 1u),
               "should_skip_contact_impulse_warm_start_island_index on out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulseWarmStartPreflight emptyPreflight =
            preflight_contact_impulse_warm_start_island(island, contacts);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_contact_impulse_warm_start_island(island),
                   "should_skip_contact_impulse_warm_start_island on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse warm-start preflight");
}

void testPreflightContactImpulseGraphGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 1.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 2.5f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const IslandContactImpulseWarmStartGraphPreflight graphPreflight =
        preflight_contact_impulse_warm_start_graph(graph, contacts);
    expectTrue(!graphPreflight.skipped, "graph impulse preflight does not skip when warm-startable islands exist");
    expectTrue(graphPreflight.can_warm_start(), "graph impulse preflight can warm-start with impulse data");
    expectTrue(graphPreflight.stats.warmStartableCount == graph.constrainedIslandCount(),
               "graph impulse preflight counts warm-startable islands");
    expectTrue(graphPreflight.stats.emptyCount + graphPreflight.stats.warmStartableCount +
                       graphPreflight.stats.noImpulseDataCount ==
                   graphPreflight.stats.totalIslands,
               "impulse warm-start stats partition total islands");
    expectTrue(count_contact_impulse_warm_startable_islands(graph, contacts) ==
                   graphPreflight.stats.warmStartableCount,
               "count_contact_impulse_warm_startable_islands matches stats");
    expectTrue(has_contact_impulse_warm_startable_islands(graph, contacts),
               "has_contact_impulse_warm_startable_islands true when impulse data exists");
    expectTrue(!should_skip_contact_impulse_warm_start_graph(graph, contacts),
               "should_skip_contact_impulse_warm_start_graph false when islands can seed");

    const std::vector<u32> indices = collect_contact_impulse_warm_startable_island_indices(graph, contacts);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect_contact_impulse_warm_startable_island_indices returns constrained count");

    contacts[0].warmNormalImpulse = 0.f;
    contacts[1].warmNormalImpulse = 0.f;
    expectTrue(should_skip_contact_impulse_warm_start_graph(graph, contacts),
               "graph impulse warm-start skipped without non-zero impulses");
    expectTrue(preflight_contact_impulse_warm_start_graph(graph, contacts).skipped,
               "graph impulse preflight skipped without non-zero impulses");
}

void testPreflightContactImpulseDispatchGuardsDt() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseDispatchPreflight validPreflight =
        preflight_contact_impulse_dispatch(graph, contacts, dt);
    expectTrue(!validPreflight.skipped, "impulse dispatch preflight does not skip constrained graph");
    expectTrue(!validPreflight.invalidDt, "impulse dispatch preflight accepts positive dt");
    expectTrue(validPreflight.can_warm_start(), "impulse dispatch preflight can warm-start with valid dt");
    expectTrue(!should_skip_contact_impulse_dispatch(graph, contacts, dt),
               "should_skip_contact_impulse_dispatch false with valid dt");

    const IslandContactImpulseDispatchPreflight invalidPreflight =
        preflight_contact_impulse_dispatch(graph, contacts, 0.f);
    expectTrue(invalidPreflight.invalidDt, "impulse dispatch preflight flags invalid dt");
    expectTrue(!invalidPreflight.can_warm_start(), "impulse dispatch preflight cannot warm-start with invalid dt");
    expectTrue(should_skip_contact_impulse_dispatch(graph, contacts, 0.f),
               "should_skip_contact_impulse_dispatch true with invalid dt");
}

void testWarmStartIslandContactImpulsesResultAndBatch() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 4.f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 2u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range impulse result is not warmed");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, islandA, contacts, dt),
               "index guarded impulse warm-start succeeds for contact island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "index guarded impulse warm-start seeds contact lambda");

    work.clearLambdas();
    const u32 warmedCount = warm_start_all_contact_impulses_guarded(work, graph, contacts, dt);
    expectTrue(warmedCount == graph.constrainedIslandCount(),
               "warm_start_all_contact_impulses_guarded seeds all impulse islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "batch impulse warm-start seeds first contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "batch impulse warm-start seeds second contact lambda");

    work.clearLambdas();
    const IslandContactImpulseWarmStartBatchResult batch =
        warm_start_all_contact_impulses_result(work, graph, contacts, dt);
    expectTrue(!batch.skipped, "impulse batch result does not skip constrained graph");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "impulse batch result records warm-startable count");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "impulse batch result seeds all constrained islands");
    expectTrue(batch.any_warmed(), "impulse batch result reports warmed islands");

    work.clearLambdas();
    const u32 graphWarmed = warm_start_graph_contact_impulses_guarded(work, graph, contacts, dt);
    expectTrue(graphWarmed == graph.constrainedIslandCount(),
               "warm_start_graph_contact_impulses_guarded seeds all impulse islands");

    work.clearLambdas();
    const IslandContactImpulseWarmStartBatchResult invalidDtBatch =
        warm_start_all_contact_impulses_result(work, graph, contacts, 0.f);
    expectTrue(invalidDtBatch.skipped, "impulse batch result skips with invalid dt");
    expectTrue(!invalidDtBatch.any_warmed(), "invalid dt impulse batch reports no warmed islands");
}

void testPreflightWarmStartIslandCombinedGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.15f};
    const std::vector<f32> priorContact = {0.25f};

    const u32 islandA = graph.bodyIsland(0);
    const IslandCombinedWarmStartPreflight combinedPreflight =
        preflight_warm_start_island_combined(graph.island(islandA), contacts, priorDistance, priorContact);
    expectTrue(!combinedPreflight.skipped, "combined preflight does not skip constrained island");
    expectTrue(combinedPreflight.lambdas.can_warm_start(), "combined preflight sees prior lambda data");
    expectTrue(combinedPreflight.impulses.can_warm_start(), "combined preflight sees non-zero impulses");
    expectTrue(combinedPreflight.can_warm_start(), "combined island can warm-start");

    const IslandCombinedWarmStartPreflight indexPreflight =
        preflight_warm_start_island_combined_by_index(graph, islandA, contacts, priorDistance, priorContact);
    expectTrue(!indexPreflight.skipped, "combined index preflight does not skip constrained island");
    expectTrue(indexPreflight.can_warm_start(), "combined index preflight can warm-start");

    const IslandCombinedWarmStartPreflight outOfRange =
        preflight_warm_start_island_combined_by_index(graph,
                                                      graph.islandCount() + 1u,
                                                      contacts,
                                                      priorDistance,
                                                      priorContact);
    expectTrue(outOfRange.skipped, "combined index preflight skips out-of-range island");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandCombinedWarmStartPreflight emptyPreflight =
            preflight_warm_start_island_combined(island, contacts, priorDistance, priorContact);
        expectTrue(emptyPreflight.skipped, "combined preflight skips empty island");
        expectTrue(!emptyPreflight.can_warm_start(), "empty island cannot combined warm-start");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for combined warm-start preflight");
}

void testValidateIslandConstraintIndices() {
    ContactIslandGraph::Island island;
    island.contactIndices = {0, 1};
    island.distanceIndices = {0, 1};

    const IslandConstraintIndexValidation valid =
        validate_island_constraint_indices(island, 3, 3);
    expectTrue(valid.valid, "all indices in range are valid");
    expectTrue(valid.invalidContactIndices == 0u, "no invalid contact indices");
    expectTrue(valid.invalidDistanceIndices == 0u, "no invalid distance indices");

    island.contactIndices = {0, 1, 5};
    island.distanceIndices = {0, 2};
    const IslandConstraintIndexValidation invalid =
        validate_island_constraint_indices(island, 3, 2);
    expectTrue(!invalid.valid, "out-of-range indices are invalid");
    expectTrue(invalid.invalidDistanceIndices == 1u, "counts invalid distance index");
    expectTrue(invalid.invalidContactIndices == 1u, "counts invalid contact index");
}

void testPreflightIslandSolveJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandSolveJobPreflight constrainedPreflight =
        preflight_island_solve_job(graph, constrainedIndex, contacts.size(), constraints.size());
    expectTrue(!constrainedPreflight.skipped, "constrained island job preflight not skipped");
    expectTrue(constrainedPreflight.can_dispatch(), "constrained island job can dispatch");
    expectTrue(constrainedPreflight.indices.valid, "constrained island indices are valid");

    const IslandSolveJobPreflight outOfRange =
        preflight_island_solve_by_index(graph, graph.islandCount() + 1u, contacts.size(), constraints.size());
    expectTrue(outOfRange.skipped, "out-of-range island job preflight is skipped");
    expectTrue(!outOfRange.can_dispatch(), "out-of-range island job cannot dispatch");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        const IslandSolveJobPreflight emptyPreflight =
            preflight_island_solve_by_index(graph, islandIndex, contacts.size(), constraints.size());
        expectTrue(emptyPreflight.skipped, "empty island job preflight is skipped");
        expectTrue(should_skip_island_solve_index(graph, islandIndex, contacts.size(), constraints.size()),
                   "should_skip_island_solve_index on empty island");
        foundEmptySkip = true;
        break;
    }
    expectTrue(foundEmptySkip, "preflight covers empty island skip");
}

void testShouldSkipIslandSolveInvalidIndices() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const IslandSolveJob job = extract_island(graph, graph.bodyIsland(0));
    expectTrue(!should_skip_island_solve_invalid_indices(job, contacts.size(), constraints.size()),
               "valid indices are not skipped");

    ContactIslandGraph::Island badIsland = *job.island;
    badIsland.contactIndices.push_back(99u);
    IslandSolveJob badJob = job;
    badJob.island = &badIsland;
    expectTrue(should_skip_island_solve_invalid_indices(badJob, contacts.size(), constraints.size()),
               "out-of-range contact index is skipped");
}

void testPreflightWarmStartIslandContactImpulses() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulseWarmStartPreflight constrainedPreflight =
        preflight_warm_start_island_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(!constrainedPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(constrainedPreflight.ownedContactCount == 1u, "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.priorImpulseCoverage == 1u,
               "impulse preflight counts non-zero impulse coverage");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can warm-start impulses");

    const IslandContactImpulseWarmStartPreflight invalidDt =
        preflight_warm_start_island_contact_impulses(graph.island(islandA), contacts, 0.f);
    expectTrue(invalidDt.invalidDt, "zero dt fails impulse preflight");
    expectTrue(!invalidDt.can_warm_start(), "impulse warm-start blocked on invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(islandA), contacts, 0.f),
               "should_skip on invalid dt");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        const IslandContactImpulseWarmStartPreflight emptyPreflight =
            preflight_warm_start_island_contact_impulses(island, contacts, dt);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_warm_start_contact_impulses_index(graph, islandIndex, contacts, dt),
                   "should_skip_contact_impulses_index on empty island");
        foundEmptySkip = true;
        break;
    }
    expectTrue(foundEmptySkip, "impulse preflight covers empty island");
}

void testWarmStartIslandContactImpulsesResultAndBatch() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 5.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 6.f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 1u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "impulse result does not warm out-of-range island");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulseWarmStartResult warmed =
        warm_start_island_contact_impulses_result(work, graph, islandA, contacts, dt);
    expectTrue(warmed.warmed, "impulse result warms contact island");
    expectTrue(!warmed.skipped, "impulse result is not skipped for contact island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "impulse result seeds contact lambda");

    work.clearLambdas();
    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, islandA, contacts, dt),
               "by-index guarded impulse warm-start succeeds");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "by-index guarded impulse warm-start seeds lambda");

    work.clearLambdas();
    const u32 batchWarmed = warm_start_all_island_contact_impulses_guarded(work, graph, contacts, dt);
    expectTrue(batchWarmed == graph.constrainedIslandCount(),
               "batch impulse warm-start seeds all constrained islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "batch impulse warm-start seeds first island lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "batch impulse warm-start seeds second island lambda");

    work.clearLambdas();
    expectTrue(warm_start_all_island_contact_impulses_guarded(work, graph, contacts, 0.f) == 0u,
               "batch impulse warm-start early-outs on invalid dt");
}

void testPreflightWarmStartContactImpulsesGraph() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 1.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "graph impulse preflight does not skip with warm contacts");
    expectTrue(preflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(preflight.stats.warmStartableCount == graph.constrainedIslandCount(),
               "graph impulse preflight counts warm-startable islands");
    expectTrue(has_contact_impulse_warm_startable_islands(graph, contacts, dt),
               "has_contact_impulse_warm_startable_islands true with impulses");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip false when impulses exist");

    const std::vector<u32> indices = collect_contact_impulse_warm_startable_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect impulse warm-startable indices matches constrained count");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(should_skip_warm_start_contact_impulses_graph(emptyGraph, contacts, dt),
               "should_skip graph impulse warm-start on empty graph");
}

void testPreflightWarmStartIslandCombined() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const IslandCombinedWarmStartPreflight combined =
        preflight_warm_start_island_combined(graph.island(graph.bodyIsland(0)),
                                             contacts,
                                             dt,
                                             priorDistance,
                                             priorContact);
    expectTrue(!combined.skipped, "combined preflight does not skip contact island");
    expectTrue(combined.can_warm_start(), "combined preflight can warm-start with lambda and impulse");
    expectTrue(combined.lambda.can_warm_start(), "combined preflight lambda arm is warm-startable");
    expectTrue(combined.impulse.can_warm_start(), "combined preflight impulse arm is warm-startable");

    ContactIslandGraph::Island zeroImpulseIsland = graph.island(graph.bodyIsland(0));
    contacts[0].warmNormalImpulse = 0.f;
    const IslandCombinedWarmStartPreflight lambdaOnly =
        preflight_warm_start_island_combined(zeroImpulseIsland, contacts, dt, priorDistance, priorContact);
    expectTrue(lambdaOnly.can_warm_start(), "combined preflight can warm-start from lambda only");
    expectTrue(!lambdaOnly.impulse.can_warm_start(), "impulse arm blocked when no prior impulses");

    ContactIslandGraph::Island emptyIsland;
    const IslandCombinedWarmStartPreflight emptyCombined =
        preflight_warm_start_island_combined(emptyIsland, contacts, dt, priorDistance, priorContact);
    expectTrue(emptyCombined.skipped, "combined preflight skips empty island");
    expectTrue(!emptyCombined.can_warm_start(), "empty island cannot combined warm-start");
}

void testPreflightIslandSolveInputsGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandSolveInputsPreflight validPreflight =
        preflight_island_solve_inputs(graph.island(constrainedIndex), 1u, 1u);
    expectTrue(!validPreflight.skipped, "solve-inputs preflight does not skip constrained island");
    expectTrue(validPreflight.can_solve(), "solve-inputs preflight can solve with in-range indices");
    expectTrue(validPreflight.ownedContactCount == 1u, "solve-inputs preflight counts owned contacts");
    expectTrue(validPreflight.ownedDistanceCount == 1u, "solve-inputs preflight counts owned distances");
    expectTrue(island_contact_indices_in_range(graph.island(constrainedIndex), 1u),
               "island_contact_indices_in_range true when contacts in range");
    expectTrue(island_distance_indices_in_range(graph.island(constrainedIndex), 1u),
               "island_distance_indices_in_range true when distances in range");
    expectTrue(!should_skip_island_solve_inputs(graph.island(constrainedIndex), 1u, 1u),
               "should_skip_island_solve_inputs false for valid inputs");

    const IslandSolveInputsPreflight outOfRangeContacts =
        preflight_island_solve_inputs(graph.island(constrainedIndex), 0u, 1u);
    expectTrue(!outOfRangeContacts.contactsInRange, "solve-inputs preflight flags out-of-range contacts");
    expectTrue(!outOfRangeContacts.can_solve(), "solve-inputs preflight cannot solve with OOB contacts");
    expectTrue(should_skip_island_solve_inputs(graph.island(constrainedIndex), 0u, 1u),
               "should_skip_island_solve_inputs true for OOB contacts");

    const IslandSolveInputsPreflight outOfRangeIndex =
        preflight_island_solve_inputs_by_index(graph, graph.islandCount() + 2u, 1u, 1u);
    expectTrue(outOfRangeIndex.skipped, "solve-inputs index preflight skips out-of-range island");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandSolveInputsPreflight emptyPreflight = preflight_island_solve_inputs(island, 1u, 1u);
        expectTrue(emptyPreflight.skipped, "solve-inputs preflight skips empty island");
        expectTrue(!emptyPreflight.can_solve(), "empty island cannot pass solve-inputs preflight");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for solve-inputs preflight");
}

void testPreflightContactImpulseWarmStartGuards() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 4.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const f32 dt = 1.f / 60.f;
    expectTrue(is_valid_contact_impulse_warm_start_dt(dt), "positive dt valid for impulse warm-start");
    expectTrue(!is_valid_contact_impulse_warm_start_dt(0.f), "zero dt invalid for impulse warm-start");

    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulsePreflight validPreflight =
        preflight_warm_start_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(!validPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(!validPreflight.invalidDt, "impulse preflight accepts valid dt");
    expectTrue(validPreflight.ownedContactCount == 1u, "impulse preflight counts owned contacts");
    expectTrue(validPreflight.inRangeContactCount == 1u, "impulse preflight counts in-range contacts");
    expectTrue(validPreflight.nonZeroImpulseCount == 1u, "impulse preflight counts non-zero impulses");
    expectTrue(validPreflight.can_warm_start(), "impulse preflight can warm-start with non-zero impulse");
    expectTrue(!should_skip_warm_start_contact_impulses(graph.island(islandA), contacts, dt),
               "should_skip false when impulse data exists");

    const IslandContactImpulsePreflight invalidDt =
        preflight_warm_start_contact_impulses(graph.island(islandA), contacts, 0.f);
    expectTrue(invalidDt.invalidDt, "impulse preflight flags invalid dt");
    expectTrue(!invalidDt.can_warm_start(), "impulse preflight cannot warm-start with invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(islandA), contacts, 0.f),
               "should_skip true when dt is invalid");

    const IslandContactImpulsePreflight outOfRangeIndex =
        preflight_warm_start_contact_impulses_by_index(graph, graph.islandCount() + 1u, contacts, dt);
    expectTrue(outOfRangeIndex.skipped, "impulse index preflight skips out-of-range island");
    expectTrue(should_skip_contact_impulse_warm_start_island_index(graph, graph.islandCount() + 1u),
               "should_skip_contact_impulse_warm_start_island_index on out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulsePreflight emptyPreflight =
            preflight_warm_start_contact_impulses(island, contacts, dt);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_contact_impulse_warm_start_island_index(graph, islandIndex),
                   "should_skip_contact_impulse_warm_start_island_index on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");
}

void testPreflightCombinedWarmStartGuards() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    const IslandCombinedWarmStartPreflight combinedPreflight =
        preflight_warm_start_island_combined(graph.island(islandA), contacts, dt, priorDistance, priorContact);
    expectTrue(!combinedPreflight.skipped, "combined preflight does not skip contact island");
    expectTrue(combinedPreflight.lambda.can_warm_start(), "combined preflight lambda path can warm-start");
    expectTrue(combinedPreflight.impulse.can_warm_start(), "combined preflight impulse path can warm-start");
    expectTrue(combinedPreflight.can_warm_start(), "combined preflight can warm-start with both paths");
    expectTrue(!should_skip_warm_start_island_combined(graph.island(islandA),
                                                       contacts,
                                                       dt,
                                                       priorDistance,
                                                       priorContact),
               "should_skip false when combined paths can seed");

    const IslandCombinedWarmStartPreflight noDataPreflight =
        preflight_warm_start_island_combined(graph.island(islandA), contacts, 0.f, {}, {});
    expectTrue(!noDataPreflight.can_warm_start(), "combined preflight cannot warm-start without data and invalid dt");
    expectTrue(should_skip_warm_start_island_combined(graph.island(islandA), contacts, 0.f, {}, {}),
               "should_skip true when combined paths cannot seed");
}

void testContactImpulseWarmStartGraphBatchGuards() {
    SolverWorkBuffers work;
    work.init(5, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 1.5f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseWarmStartGraphPreflight graphPreflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!graphPreflight.skipped, "graph impulse preflight does not skip when seedable islands exist");
    expectTrue(graphPreflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(graphPreflight.stats.impulseSeedableCount == graph.constrainedIslandCount(),
               "graph impulse preflight counts seedable islands");
    expectTrue(graphPreflight.stats.emptyCount + graphPreflight.stats.impulseSeedableCount +
                       graphPreflight.stats.noImpulseDataCount + graphPreflight.stats.invalidDtCount ==
                   graphPreflight.stats.totalIslands,
               "impulse warm-start stats partition total islands");
    expectTrue(count_impulse_warm_startable_islands(graph, contacts, dt) ==
                   graphPreflight.stats.impulseSeedableCount,
               "count_impulse_warm_startable_islands matches stats");
    expectTrue(has_impulse_warm_startable_islands(graph, contacts, dt),
               "has_impulse_warm_startable_islands true when impulses exist");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip_warm_start_contact_impulses_graph false when seedable");

    const std::vector<u32> indices = collect_impulse_warm_startable_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == graph.constrainedIslandCount(),
               "collect_impulse_warm_startable_island_indices returns constrained count");

    work.clearLambdas();
    const u32 warmedCount = warm_start_all_islands_contact_impulses_guarded(work, graph, contacts, dt);
    expectTrue(warmedCount == graph.constrainedIslandCount(),
               "batch impulse warm-start seeds all constrained islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "batch impulse warm-start seeds first contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "batch impulse warm-start seeds second contact lambda");

    const IslandContactImpulseWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 5u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse warm-start result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range impulse warm-start result is not warmed");

    work.clearLambdas();
    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandContactImpulseWarmStartResult seeded =
        warm_start_island_contact_impulses_result(work, graph, constrainedIndex, contacts, dt);
    expectTrue(seeded.warmed, "impulse warm-start result seeds constrained island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "impulse warm-start result seeds contact lambda");

    expectTrue(should_skip_warm_start_contact_impulses_graph(graph, contacts, 0.f),
               "graph impulse warm-start skipped with invalid dt");
    expectTrue(preflight_warm_start_contact_impulses_graph(graph, contacts, 0.f).skipped,
               "graph impulse preflight skipped with invalid dt");
}

void testPreflightDispatchIslandIndexGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const IslandJobDispatchPreflight outOfRange =
        preflight_dispatch_island_index(graph, graph.islandCount() + 1u, 1.f / 60.f);
    expectTrue(outOfRange.skipped, "index dispatch preflight skips out-of-range island");
    expectTrue(!outOfRange.can_dispatch(), "out-of-range index cannot dispatch");
    expectTrue(should_skip_dispatch_island_index(graph, graph.islandCount() + 1u, 1.f / 60.f),
               "should_skip_dispatch_island_index on out-of-range index");

    const IslandJobDispatchPreflight invalidDt = preflight_dispatch_island_index(graph, 0u, 0.f);
    expectTrue(invalidDt.invalidDt, "index dispatch preflight marks invalid dt");
    expectTrue(!invalidDt.can_dispatch(), "invalid dt blocks index dispatch");

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandJobDispatchPreflight constrained =
        preflight_dispatch_island_index(graph, constrainedIndex, 1.f / 60.f);
    expectTrue(!constrained.skipped, "index dispatch preflight does not skip constrained island");
    expectTrue(constrained.can_dispatch(), "constrained island can dispatch with valid dt");
    expectTrue(!should_skip_dispatch_island_index(graph, constrainedIndex, 1.f / 60.f),
               "should_skip false for constrained island with valid dt");
}

void testDispatchIslandJobsBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const std::vector<IslandSolveJob> jobs = collect_dispatchable_island_jobs(graph);
    const u32 solvedCount = dispatch_island_jobs(bodies,
                                                 graph,
                                                 jobs,
                                                 work,
                                                 constraints,
                                                 1.f / 60.f,
                                                 0.f,
                                                 invMassFn);
    expectTrue(solvedCount == graph.constrainedIslandCount(),
               "dispatch_island_jobs solves all dispatchable jobs");

    expectTrue(dispatch_island_jobs(bodies,
                                    graph,
                                    jobs,
                                    work,
                                    constraints,
                                    0.f,
                                    0.f,
                                    invMassFn) == 0u,
               "dispatch_island_jobs early-outs on invalid dt");
}

void testPreflightContactImpulseWarmStartGuards() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const f32 dt = 1.f / 60.f;
    expectTrue(is_valid_contact_impulse_warm_start_dt(dt), "positive dt valid for impulse warm-start");
    expectTrue(!is_valid_contact_impulse_warm_start_dt(0.f), "zero dt invalid for impulse warm-start");

    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulsePreflight constrainedPreflight =
        preflight_warm_start_island_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(!constrainedPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(constrainedPreflight.ownedContactCount == 1u, "impulse preflight counts owned contacts");
    expectTrue(constrainedPreflight.seedableContactCount == 1u,
               "impulse preflight counts non-zero warm-start impulses");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can seed impulse warm-start");

    const IslandContactImpulsePreflight zeroImpulsePreflight =
        preflight_warm_start_island_contact_impulses(graph.island(graph.bodyIsland(2)), contacts, dt);
    expectTrue(!zeroImpulsePreflight.skipped, "impulse preflight does not skip zero-impulse contact island");
    expectTrue(zeroImpulsePreflight.seedableContactCount == 0u,
               "zero impulse island has no seedable contacts");
    expectTrue(!zeroImpulsePreflight.can_warm_start(), "zero impulse island cannot seed");

    const IslandContactImpulsePreflight outOfRange =
        preflight_warm_start_island_contact_impulses_by_index(graph, graph.islandCount() + 1u, contacts, dt);
    expectTrue(outOfRange.skipped, "index impulse preflight skips out-of-range island");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulsePreflight emptyPreflight =
            preflight_warm_start_island_contact_impulses(island, contacts, dt);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_contact_impulse_warm_start_island(island),
                   "should_skip_contact_impulse_warm_start_island on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");
}

void testPreflightContactImpulseGraphGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "graph impulse preflight does not skip when seedable islands exist");
    expectTrue(preflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(preflight.stats.seedableCount == 1u, "graph impulse preflight counts seedable islands");
    expectTrue(preflight.stats.noImpulseCount >= 1u, "graph impulse preflight counts zero-impulse islands");
    expectTrue(count_seedable_contact_impulse_islands(graph, contacts, dt) == 1u,
               "count_seedable_contact_impulse_islands matches stats");
    expectTrue(has_seedable_contact_impulse_islands(graph, contacts, dt),
               "has_seedable_contact_impulse_islands true when impulses exist");
    expectTrue(!should_skip_contact_impulse_warm_start_graph(graph, contacts, dt),
               "should_skip_contact_impulse_warm_start_graph false when seedable");

    const std::vector<u32> indices = collect_contact_impulse_seedable_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == 1u, "collect_contact_impulse_seedable_island_indices returns seedable count");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(should_skip_contact_impulse_warm_start_graph(emptyGraph, contacts, dt),
               "graph impulse warm-start skipped on empty graph");
    expectTrue(preflight_warm_start_contact_impulses_graph(emptyGraph, contacts, dt).skipped,
               "graph impulse preflight skipped on empty graph");

    expectTrue(should_skip_contact_impulse_warm_start_graph(graph, contacts, 0.f),
               "graph impulse warm-start skipped on invalid dt");
}

void testWarmStartContactImpulseResultAndBatch() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 4.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 1.5f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 2u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range impulse result is not warmed");

    work.clearLambdas();
    const IslandContactImpulseBatchResult batch =
        warm_start_all_islands_contact_impulses_result(work, graph, contacts, dt);
    expectTrue(!batch.skipped, "batch impulse warm-start does not skip seedable graph");
    expectTrue(batch.warmedCount == 2u, "batch impulse warm-start seeds all seedable islands");
    expectTrue(batch.any_warmed(), "batch impulse warm-start reports warmed islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "batch impulse warm-start seeds first contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "batch impulse warm-start seeds second contact lambda");

    work.clearLambdas();
    const u32 constrainedIndex = graph.bodyIsland(0);
    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, constrainedIndex, contacts, dt),
               "index guarded impulse warm-start succeeds for seedable island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "index guarded impulse warm-start seeds contact lambda");

    work.clearLambdas();
    expectTrue(warm_start_all_islands_contact_impulses_guarded(work, graph, contacts, dt) == 2u,
               "guarded batch impulse warm-start matches batch result count");
}

void testWarmStartAllIslandsResultBatch() {
    SolverWorkBuffers work;
    work.init(5, 2, 2);
    work.ensureLambdaCapacity(2, 2);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<f32> priorDistance = {0.11f, 0.22f};
    const std::vector<f32> priorContact = {0.33f, 0.44f};

    work.clearLambdas();
    const IslandWarmStartBatchResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "warm_start batch result does not skip seedable graph");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "warm_start batch result seeds all constrained islands");
    expectTrue(batch.any_warmed(), "warm_start batch result reports warmed islands");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "warm_start batch result records warm-startable count");

    work.clearLambdas();
    const IslandWarmStartBatchResult emptyPrior =
        warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyPrior.skipped, "warm_start batch result skips without prior data");
    expectTrue(!emptyPrior.any_warmed(), "empty prior batch reports no warmed islands");
}

void testPreflightWarmStartIslandCombined() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    const IslandCombinedWarmStartPreflight preflight =
        preflight_warm_start_island_combined(graph.island(islandA), contacts, dt, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "combined preflight does not skip contact island");
    expectTrue(preflight.can_warm_start(), "combined preflight can warm-start with prior and contacts");
    expectTrue(preflight.lambda.can_warm_start(), "combined preflight sees prior lambda coverage");
    expectTrue(preflight.impulse.ownedContactCount == 1u, "combined preflight counts owned contacts");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandCombinedWarmStartPreflight emptyPreflight =
            preflight_warm_start_island_combined(island, contacts, dt, priorDistance, priorContact);
        expectTrue(emptyPreflight.skipped, "combined preflight skips empty island");
        expectTrue(!emptyPreflight.can_warm_start(), "empty island cannot combined warm-start");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for combined preflight");
}

void testPreflightIslandSolveJobGuards() {
    IslandSolveJob invalid{};
    const IslandSolveJobPreflight invalidPreflight = preflight_island_solve_job(invalid);
    expectTrue(invalidPreflight.outOfRange, "null-island job preflight marks out-of-range");
    expectTrue(invalidPreflight.skipped, "null-island job preflight is skipped");
    expectTrue(!invalidPreflight.can_dispatch(), "null-island job cannot dispatch");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    bool foundEmptySkip = false;
    bool foundConstrainedDispatch = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        const IslandSolveJobPreflight preflight = preflight_island_solve_job(job);
        if (job.empty) {
            foundEmptySkip = true;
            expectTrue(preflight.empty, "empty job preflight marks empty");
            expectTrue(preflight.skipped, "empty job preflight is skipped");
            expectTrue(!preflight.can_dispatch(), "empty job cannot dispatch");
        } else {
            foundConstrainedDispatch = true;
            expectTrue(preflight.dispatchable, "constrained job preflight is dispatchable");
            expectTrue(preflight.can_dispatch(), "constrained job can dispatch");
            expectTrue(preflight.can_dispatch() == should_solve_island(job),
                       "job preflight mirrors should_solve_island");
        }
    }

    expectTrue(foundEmptySkip, "job preflight covers empty island");
    expectTrue(foundConstrainedDispatch, "job preflight covers constrained island");
}

void testIsValidContactImpulseWarmStartDtGuard() {
    expectTrue(is_valid_contact_impulse_warm_start_dt(1.f / 60.f),
               "positive dt is valid for contact-impulse warm-start");
    expectTrue(!is_valid_contact_impulse_warm_start_dt(0.f),
               "zero dt is invalid for contact-impulse warm-start");
    expectTrue(!is_valid_contact_impulse_warm_start_dt(-1.f / 60.f),
               "negative dt is invalid for contact-impulse warm-start");
}

void testPreflightIslandContactImpulsesGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const f32 dt = 1.f / 60.f;

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulsePreflight preflight = preflight_island_contact_impulses(island, contacts, dt);
        expectTrue(preflight.skipped, "preflight skips empty island contact impulses");
        expectTrue(!preflight.can_warm_start(), "empty island cannot warm-start impulses");
        expectTrue(should_skip_contact_impulse_island(island, contacts, dt),
                   "should_skip_contact_impulse_island on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");

    const u32 islandA = graph.bodyIsland(0);
    const IslandContactImpulsePreflight constrainedPreflight =
        preflight_island_contact_impulses(graph.island(islandA), contacts, dt);
    expectTrue(!constrainedPreflight.skipped, "preflight does not skip constrained contact island");
    expectTrue(constrainedPreflight.ownedContactCount == 1u, "preflight counts owned contact slots");
    expectTrue(constrainedPreflight.impulseCoverage == 1u, "preflight counts non-zero impulse coverage");
    expectTrue(constrainedPreflight.can_warm_start(), "constrained island can warm-start impulses");

    const IslandContactImpulsePreflight invalidDtPreflight =
        preflight_island_contact_impulses(graph.island(islandA), contacts, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "zero dt fails impulse preflight");
    expectTrue(!invalidDtPreflight.can_warm_start(), "invalid dt blocks impulse warm-start");
}

void testPreflightContactImpulseGraphGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulseGraphPreflight preflight = preflight_contact_impulse_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "graph impulse preflight does not skip when impulses exist");
    expectTrue(!preflight.invalidDt, "graph impulse preflight accepts valid dt");
    expectTrue(preflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(preflight.stats.warmStartableCount == 1u,
               "graph impulse preflight counts one warm-startable island");
    expectTrue(preflight.stats.noImpulseCount >= 1u,
               "graph impulse preflight counts zero-impulse constrained island");
    expectTrue(preflight.stats.emptyCount + preflight.stats.warmStartableCount +
                       preflight.stats.noImpulseCount ==
                   preflight.stats.totalIslands,
               "impulse stats partition total islands");
    expectTrue(count_contact_impulse_warm_startable_islands(graph, contacts, dt) ==
                   preflight.stats.warmStartableCount,
               "count_contact_impulse_warm_startable_islands matches stats");
    expectTrue(has_contact_impulse_warm_startable_islands(graph, contacts, dt),
               "has_contact_impulse_warm_startable_islands true when impulses exist");
    expectTrue(!should_skip_contact_impulse_graph(graph, contacts, dt),
               "should_skip_contact_impulse_graph false when impulses exist");

    const std::vector<u32> indices = collect_contact_impulse_warm_startable_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == 1u, "collect returns one warm-startable impulse island");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(should_skip_contact_impulse_graph(emptyGraph, contacts, dt),
               "graph impulse warm-start skipped on empty graph");
    expectTrue(preflight_contact_impulse_graph(emptyGraph, contacts, dt).skipped,
               "graph impulse preflight skipped on empty graph");

    const IslandContactImpulseGraphPreflight invalidDtPreflight =
        preflight_contact_impulse_graph(graph, contacts, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "graph impulse preflight marks invalid dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "invalid dt blocks graph impulse warm-start");
    expectTrue(should_skip_contact_impulse_graph(graph, contacts, 0.f),
               "should_skip_contact_impulse_graph on invalid dt");
}

void testPreflightIslandContactImpulsesByIndex() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 1.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
    const IslandContactImpulsePreflight outOfRange =
        preflight_island_contact_impulses_by_index(graph, graph.islandCount() + 1u, contacts, dt);
    expectTrue(outOfRange.skipped, "index impulse preflight skips out-of-range island");
    expectTrue(should_skip_contact_impulse_island_index(graph, graph.islandCount() + 1u, contacts, dt),
               "should_skip_contact_impulse_island_index on out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulsePreflight emptyPreflight =
            preflight_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);
        expectTrue(emptyPreflight.skipped, "index impulse preflight skips empty island");
        expectTrue(should_skip_contact_impulse_island_index(graph, islandIndex, contacts, dt),
                   "should_skip_contact_impulse_island_index on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for index impulse preflight");

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandContactImpulsePreflight constrainedPreflight =
        preflight_island_contact_impulses_by_index(graph, constrainedIndex, contacts, dt);
    expectTrue(!constrainedPreflight.skipped, "index impulse preflight does not skip constrained island");
    expectTrue(constrainedPreflight.can_warm_start(), "index impulse preflight can warm-start constrained island");
}

void testWarmStartIslandContactImpulsesResultAndBatch() {
    SolverWorkBuffers work;
    work.init(5, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 5.f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const f32 dt = 1.f / 60.f;

    const IslandContactImpulseWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 2u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse warm_start result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range impulse warm_start result is not warmed");

    const IslandContactImpulseWarmStartResult invalidDt =
        warm_start_island_contact_impulses_result(work, graph, graph.bodyIsland(0), contacts, 0.f);
    expectTrue(invalidDt.skipped, "impulse warm_start result skips invalid dt");
    expectTrue(!invalidDt.warmed, "invalid dt impulse warm_start result is not warmed");

    work.clearLambdas();
    const IslandBatchContactImpulseWarmStartResult batch =
        warm_start_all_island_contact_impulses_result(work, graph, contacts, dt);
    expectTrue(!batch.skipped, "batch impulse warm-start does not skip constrained graph");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "batch impulse warm-start records warm-startable count");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "batch impulse warm-start seeds all constrained islands");
    expectTrue(batch.any_warmed(), "batch impulse warm-start reports warmed islands");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "batch impulse warm-start seeds first contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[1]) > 1e-6f,
               "batch impulse warm-start seeds second contact lambda");

    work.clearLambdas();
    const u32 constrainedIndex = graph.bodyIsland(0);
    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work,
                                                                   graph,
                                                                   constrainedIndex,
                                                                   contacts,
                                                                   dt),
               "index guarded impulse warm-start succeeds for constrained island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "index guarded impulse warm-start seeds owned contact lambda");

    work.clearLambdas();
    expectTrue(warm_start_all_island_contact_impulses_guarded(work, graph, contacts, dt) ==
                   graph.constrainedIslandCount(),
               "all guarded impulse warm-start seeds constrained island count");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandBatchContactImpulseWarmStartResult emptyBatch =
        warm_start_all_island_contact_impulses_result(work, emptyGraph, contacts, dt);
    expectTrue(emptyBatch.skipped, "batch impulse warm-start skips empty graph");
    expectTrue(!emptyBatch.any_warmed(), "empty graph batch reports no warmed islands");
}

void testPreflightIslandDispatchFromJobs() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    const IslandDispatchJobPreflight validPreflight = preflight_island_dispatch_from_jobs(jobs, 1.f / 60.f);
    expectTrue(!validPreflight.invalidDt, "job preflight accepts valid dt");
    expectTrue(!validPreflight.skipped, "job preflight does not skip constrained jobs");
    expectTrue(validPreflight.can_dispatch(), "job preflight can dispatch constrained jobs");
    expectTrue(validPreflight.stats.dispatchableCount == graph.constrainedIslandCount(),
               "job preflight dispatchable count matches constrained islands");
    expectTrue(count_dispatchable_jobs(jobs) == validPreflight.stats.dispatchableCount,
               "count_dispatchable_jobs matches job preflight stats");
    expectTrue(has_dispatchable_jobs(jobs), "has_dispatchable_jobs true for constrained jobs");
    expectTrue(!should_skip_island_dispatch_from_jobs(jobs, 1.f / 60.f),
               "should_skip false for constrained jobs with valid dt");

    const IslandDispatchJobPreflight invalidDtPreflight = preflight_island_dispatch_from_jobs(jobs, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "job preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_dispatch(), "job preflight blocked on invalid dt");
    expectTrue(should_skip_island_dispatch_from_jobs(jobs, 0.f),
               "should_skip_island_dispatch_from_jobs on invalid dt");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const std::vector<IslandSolveJob> emptyJobs = extract_island_jobs(emptyGraph);
    const IslandDispatchJobPreflight emptyPreflight = preflight_island_dispatch_from_jobs(emptyJobs, 1.f / 60.f);
    expectTrue(emptyPreflight.skipped, "job preflight skips empty job list");
    expectTrue(!has_dispatchable_jobs(emptyJobs), "has_dispatchable_jobs false for empty jobs");
}

void testPreflightWarmStartContactImpulsesGraph() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});
    const f32 dt = 1.f / 60.f;

    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "impulse graph preflight does not skip contact graph");
    expectTrue(!preflight.invalidDt, "impulse graph preflight accepts valid dt");
    expectTrue(preflight.can_warm_start(), "impulse graph preflight can warm-start");
    expectTrue(preflight.stats.warmStartableCount == 1u,
               "impulse graph preflight counts one warm-startable island");
    expectTrue(preflight.stats.emptyCount + preflight.stats.warmStartableCount +
                       preflight.stats.zeroImpulseCount ==
                   preflight.stats.totalIslands,
               "impulse warm-start stats partition total islands");
    expectTrue(count_warm_startable_contact_impulse_islands(graph, contacts, dt) ==
                   preflight.stats.warmStartableCount,
               "count_warm_startable_contact_impulse_islands matches stats");
    expectTrue(has_warm_startable_contact_impulse_islands(graph, contacts, dt),
               "has_warm_startable_contact_impulse_islands true when impulses exist");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip_warm_start_contact_impulses_graph false when impulses exist");

    const std::vector<u32> indices = collect_warm_startable_contact_impulse_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == 1u, "collect impulse indices returns warm-startable count");

    const IslandContactImpulseWarmStartGraphPreflight invalidDtPreflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "impulse graph preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "impulse graph preflight blocked on invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses_graph(graph, contacts, 0.f),
               "should_skip_warm_start_contact_impulses_graph on invalid dt");
}

void testPreflightWarmStartCombinedGraphAndIndex() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const IslandCombinedWarmStartGraphPreflight preflight =
        preflight_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "combined graph preflight does not skip contact graph");
    expectTrue(!preflight.invalidDt, "combined graph preflight accepts valid dt");
    expectTrue(preflight.can_warm_start(), "combined graph preflight can warm-start");
    expectTrue(preflight.stats.warmStartableCount == 1u,
               "combined graph preflight counts one warm-startable island");
    expectTrue(count_warm_startable_combined_islands(graph, contacts, dt, priorDistance, priorContact) ==
                   preflight.stats.warmStartableCount,
               "count_warm_startable_combined_islands matches stats");
    expectTrue(has_warm_startable_combined_islands(graph, contacts, dt, priorDistance, priorContact),
               "has_warm_startable_combined_islands true when prior data exists");
    expectTrue(!should_skip_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact),
               "should_skip_warm_start_combined_graph false when islands can seed");

    const std::vector<u32> indices =
        collect_warm_startable_combined_island_indices(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(indices.size() == 1u, "collect combined indices returns warm-startable count");

    const IslandCombinedWarmStartPreflight outOfRange =
        preflight_warm_start_combined_island_by_index(graph,
                                                      graph.islandCount() + 1u,
                                                      contacts,
                                                      dt,
                                                      priorDistance,
                                                      priorContact);
    expectTrue(outOfRange.skipped, "combined index preflight skips out-of-range island");
    expectTrue(should_skip_warm_start_combined_island_index(graph, graph.islandCount() + 1u, dt),
               "should_skip_warm_start_combined_island_index on out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        expectTrue(should_skip_warm_start_combined_island_index(graph, islandIndex, dt),
                   "should_skip_warm_start_combined_island_index on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for combined index guard");

    const IslandCombinedWarmStartGraphPreflight invalidDtPreflight =
        preflight_warm_start_combined_graph(graph, contacts, 0.f, priorDistance, priorContact);
    expectTrue(invalidDtPreflight.invalidDt, "combined graph preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "combined graph preflight blocked on invalid dt");
}

void testWarmStartAllIslandsCombinedResultBatch() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    work.clearLambdas();
    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_combined_result(work, graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!batch.skipped, "combined batch does not skip contact graph");
    expectTrue(batch.warmStartableCount == 1u, "combined batch records warm-startable count");
    expectTrue(batch.warmedCount == 1u, "combined batch warms contact island");
    expectTrue(batch.any_warmed(), "combined batch reports warmed island");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "combined batch seeds prior contact lambda");

    work.clearLambdas();
    expectTrue(warm_start_all_islands_combined_guarded(work, graph, contacts, dt, priorDistance, priorContact) == 1u,
               "combined guarded count matches batch warmed count");

    work.clearLambdas();
    const IslandBatchWarmStartResult invalidDtBatch =
        warm_start_all_islands_combined_result(work, graph, contacts, 0.f, priorDistance, priorContact);
    expectTrue(invalidDtBatch.skipped, "combined batch skips invalid dt");
    expectTrue(!invalidDtBatch.any_warmed(), "invalid dt combined batch reports no warmed islands");
}

void testPreflightSolveIslandJobStaleIndices() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const IslandSolveJob validJob = extract_island(graph, graph.bodyIsland(0));
    const IslandSolveJobPreflight validPreflight =
        preflight_solve_island_job(validJob, contacts, constraints);
    expectTrue(!validPreflight.skipped, "valid job preflight does not skip constrained island");
    expectTrue(!validPreflight.hasStaleIndices, "valid job has no stale indices");
    expectTrue(validPreflight.validContactCount == 1u, "valid job counts owned contacts");
    expectTrue(validPreflight.validDistanceCount == 0u, "valid job has no distance constraints");
    expectTrue(validPreflight.can_solve(), "valid job can solve");
    expectTrue(!has_stale_island_indices(validJob, contacts, constraints),
               "has_stale_island_indices false for valid job");
    expectTrue(!should_skip_solve_island_job_stale(validJob, contacts, constraints),
               "should_skip_solve_island_job_stale false for valid job");

    std::vector<narrowphase::ContactManifold> truncatedContacts;
    const IslandSolveJobPreflight stalePreflight =
        preflight_solve_island_job(validJob, truncatedContacts, constraints);
    expectTrue(stalePreflight.hasStaleIndices, "truncated contacts mark stale indices");
    expectTrue(stalePreflight.staleContactCount == 1u, "stale preflight counts stale contacts");
    expectTrue(!stalePreflight.can_solve(), "stale job cannot solve");
    expectTrue(should_skip_solve_island_job_stale(validJob, truncatedContacts, constraints),
               "should_skip_solve_island_job_stale on stale contacts");

    const IslandSolveJobPreflight outOfRange =
        preflight_solve_island_job_by_index(graph, graph.islandCount() + 1u, contacts, constraints);
    expectTrue(outOfRange.skipped, "index job preflight skips out-of-range island");

    IslandSolveJob emptyJob{};
    const IslandSolveJobPreflight emptyPreflight = preflight_solve_island_job(emptyJob, contacts, constraints);
    expectTrue(emptyPreflight.skipped, "empty job preflight is skipped");
}

void testPreflightDispatchIslandByIndex() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const f32 dt = 1.f / 60.f;
    const u32 constrainedIndex = graph.bodyIsland(0);

    const IslandDispatchIndexPreflight validPreflight =
        preflight_dispatch_island_by_index(graph, constrainedIndex, dt);
    expectTrue(!validPreflight.skipped, "dispatch index preflight does not skip constrained island");
    expectTrue(!validPreflight.invalidDt, "dispatch index preflight accepts valid dt");
    expectTrue(validPreflight.can_dispatch(), "dispatch index preflight can dispatch constrained island");
    expectTrue(should_solve_island(validPreflight.job), "dispatch index preflight extracts dispatchable job");
    expectTrue(!should_skip_dispatch_island_index(graph, constrainedIndex, dt),
               "should_skip_dispatch_island_index false for constrained island");

    const IslandDispatchIndexPreflight invalidDt =
        preflight_dispatch_island_by_index(graph, constrainedIndex, 0.f);
    expectTrue(invalidDt.invalidDt, "dispatch index preflight rejects zero dt");
    expectTrue(!invalidDt.can_dispatch(), "dispatch index preflight cannot dispatch with invalid dt");
    expectTrue(should_skip_dispatch_island_index(graph, constrainedIndex, 0.f),
               "should_skip_dispatch_island_index on invalid dt");

    const IslandDispatchIndexPreflight outOfRange =
        preflight_dispatch_island_by_index(graph, graph.islandCount() + 1u, dt);
    expectTrue(outOfRange.skipped, "dispatch index preflight skips out-of-range island");
    expectTrue(should_skip_dispatch_island_index(graph, graph.islandCount() + 1u, dt),
               "should_skip_dispatch_island_index on out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandDispatchIndexPreflight emptyPreflight =
            preflight_dispatch_island_by_index(graph, islandIndex, dt);
        expectTrue(emptyPreflight.skipped, "dispatch index preflight skips empty island");
        expectTrue(should_skip_dispatch_island_index(graph, islandIndex, dt),
                   "should_skip_dispatch_island_index on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for dispatch index preflight");
}

void testDispatchAllIslandJobsResult() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(5, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const std::vector<IslandSolveJob> jobs = collect_dispatchable_island_jobs(graph);
    expectTrue(jobs.size() == graph.constrainedIslandCount(),
               "collected jobs match constrained island count");

    const IslandBatchDispatchResult batch =
        dispatch_all_island_jobs_result(bodies, jobs, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "job batch dispatch does not skip constrained graph");
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "job batch dispatch solves all constrained islands");
    expectTrue(batch.dispatchableCount == graph.constrainedIslandCount(),
               "job batch dispatch counts all constrained islands");
    expectTrue(batch.any_solved(), "job batch dispatch reports solved islands");

    const IslandBatchDispatchResult invalidDtBatch =
        dispatch_all_island_jobs_result(bodies, jobs, work, constraints, 0.f, 0.f, invMassFn);
    expectTrue(invalidDtBatch.skipped, "job batch dispatch skips invalid dt");

    IslandSolveJob emptyJob{};
    const IslandBatchDispatchResult emptyBatch =
        dispatch_all_island_jobs_result(bodies, {emptyJob}, work, constraints, dt, 0.f, invMassFn);
    expectTrue(emptyBatch.skipped, "job batch dispatch skips empty job list");

    expectTrue(dispatch_all_island_jobs(bodies, jobs, work, constraints, dt, 0.f, invMassFn) ==
                   graph.constrainedIslandCount(),
               "dispatch_all_island_jobs count matches constrained islands");
}

void testPreflightWarmStartContactImpulsesGraphGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});
    const f32 dt = 1.f / 60.f;

    const IslandContactImpulseWarmStartStats stats =
        compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    expectTrue(stats.totalIslands == graph.islandCount(), "impulse graph stats report total islands");
    expectTrue(stats.warmStartableCount == 1u, "impulse graph stats count warm-startable island");
    expectTrue(stats.emptyCount + stats.warmStartableCount + stats.noImpulseCount == stats.totalIslands,
               "impulse graph stats partition total islands");
    expectTrue(count_warm_startable_contact_impulse_islands(graph, contacts, dt) == 1u,
               "count_warm_startable_contact_impulse_islands matches stats");
    expectTrue(has_warm_startable_contact_impulse_islands(graph, contacts, dt),
               "has_warm_startable_contact_impulse_islands true when impulses exist");

    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "impulse graph preflight does not skip when warm-startable islands exist");
    expectTrue(!preflight.invalidDt, "impulse graph preflight accepts valid dt");
    expectTrue(preflight.can_warm_start(), "impulse graph preflight can warm-start");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip_warm_start_contact_impulses_graph false when impulses exist");

    const std::vector<u32> indices = collect_warm_startable_contact_impulse_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == 1u, "impulse graph collects one warm-startable island index");

    const IslandContactImpulseWarmStartGraphPreflight invalidDt =
        preflight_warm_start_contact_impulses_graph(graph, contacts, 0.f);
    expectTrue(invalidDt.invalidDt, "impulse graph preflight rejects zero dt");
    expectTrue(!invalidDt.can_warm_start(), "impulse graph preflight cannot warm-start with invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses_graph(graph, contacts, 0.f),
               "should_skip_warm_start_contact_impulses_graph on invalid dt");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(should_skip_warm_start_contact_impulses_graph(emptyGraph, contacts, dt),
               "impulse graph warm-start skipped on empty graph");
    expectTrue(preflight_warm_start_contact_impulses_graph(emptyGraph, contacts, dt).skipped,
               "impulse graph preflight skipped on empty graph");
}

void testPreflightWarmStartCombinedGraphGuards() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const IslandCombinedWarmStartStats stats =
        compute_island_combined_warm_start_stats(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(stats.totalIslands == graph.islandCount(), "combined graph stats report total islands");
    expectTrue(stats.warmStartableCount == 1u, "combined graph stats count warm-startable island");
    expectTrue(stats.emptyCount + stats.warmStartableCount + stats.noDataCount == stats.totalIslands,
               "combined graph stats partition total islands");
    expectTrue(count_warm_startable_combined_islands(graph, contacts, dt, priorDistance, priorContact) == 1u,
               "count_warm_startable_combined_islands matches stats");
    expectTrue(has_warm_startable_combined_islands(graph, contacts, dt, priorDistance, priorContact),
               "has_warm_startable_combined_islands true when prior data exists");

    const IslandCombinedWarmStartGraphPreflight preflight =
        preflight_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "combined graph preflight does not skip when warm-startable islands exist");
    expectTrue(!preflight.invalidDt, "combined graph preflight accepts valid dt");
    expectTrue(preflight.can_warm_start(), "combined graph preflight can warm-start");
    expectTrue(!should_skip_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact),
               "should_skip_warm_start_combined_graph false when prior data exists");

    const std::vector<u32> indices =
        collect_warm_startable_combined_island_indices(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(indices.size() == 1u, "combined graph collects one warm-startable island index");

    const u32 islandA = graph.bodyIsland(0);
    const IslandCombinedWarmStartPreflight indexPreflight =
        preflight_warm_start_combined_island_by_index(graph, islandA, contacts, dt, priorDistance, priorContact);
    expectTrue(!indexPreflight.skipped, "combined index preflight does not skip contact island");
    expectTrue(indexPreflight.can_warm_start(), "combined index preflight can warm-start contact island");

    const IslandCombinedWarmStartPreflight outOfRange =
        preflight_warm_start_combined_island_by_index(graph,
                                                      graph.islandCount() + 1u,
                                                      contacts,
                                                      dt,
                                                      priorDistance,
                                                      priorContact);
    expectTrue(outOfRange.skipped, "combined index preflight skips out-of-range island");

    std::vector<narrowphase::ContactManifold> zeroImpulseContacts;
    zeroImpulseContacts.push_back(narrowphase::ContactManifold{});
    zeroImpulseContacts.back().valid = true;
    zeroImpulseContacts.back().bodyA = 0;
    zeroImpulseContacts.back().bodyB = 1;
    zeroImpulseContacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph zeroImpulseGraph;
    zeroImpulseGraph.build(3, zeroImpulseContacts, {});
    expectTrue(should_skip_warm_start_combined_graph(zeroImpulseGraph, zeroImpulseContacts, dt, {}, {}),
               "combined graph warm-start skipped without prior data or impulses");
    expectTrue(preflight_warm_start_combined_graph(zeroImpulseGraph, zeroImpulseContacts, dt, {}, {}).skipped,
               "combined graph preflight skipped without prior data or impulses");
}

void testWarmStartAllIslandsCombinedResultBatch() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 3.f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_combined_result(work, graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!batch.skipped, "combined batch result does not skip contact graph");
    expectTrue(batch.warmedCount == 1u, "combined batch result warms contact island only");
    expectTrue(batch.warmStartableCount == 1u, "combined batch result counts warm-startable island");
    expectTrue(batch.any_warmed(), "combined batch result reports warmed island");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "combined batch result seeds prior contact lambda");

    work.clearLambdas();
    const IslandBatchWarmStartResult invalidDtBatch =
        warm_start_all_islands_combined_result(work, graph, contacts, 0.f, priorDistance, priorContact);
    expectTrue(invalidDtBatch.skipped, "combined batch result skips invalid dt");

    work.clearLambdas();
    expectTrue(warm_start_all_islands_combined_guarded(work, graph, contacts, dt, priorDistance, priorContact) == 1u,
               "combined guarded count matches batch warmed count");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 0;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 9;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 2, .restLength = 1.f},
        DistanceConstraint{.bodyA = 1, .bodyB = 8, .restLength = 1.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(3, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip non-empty inputs");
    expectTrue(preflight.validContactCount == 1u, "build preflight counts valid contacts");
    expectTrue(preflight.invalidContactCount == 1u, "build preflight counts invalid contacts");
    expectTrue(preflight.selfPairContactCount == 1u, "build preflight counts self-pair contacts");
    expectTrue(preflight.oobContactCount == 1u, "build preflight counts out-of-range contacts");
    expectTrue(preflight.validDistanceCount == 1u, "build preflight counts valid distance constraints");
    expectTrue(preflight.selfPairDistanceCount == 1u, "build preflight counts self-pair distance constraints");
    expectTrue(preflight.oobDistanceCount == 1u, "build preflight counts out-of-range distance constraints");

    expectTrue(contactBuildRejectReason(contacts[0], 3) == IslandBuildRejectReason::None,
               "valid contact passes build reject reason");
    expectTrue(contactBuildRejectReason(contacts[1], 3) == IslandBuildRejectReason::InvalidContact,
               "invalid contact is rejected");
    expectTrue(contactBuildRejectReason(contacts[2], 3) == IslandBuildRejectReason::SelfPair,
               "self-pair contact is rejected");
    expectTrue(contactBuildRejectReason(contacts[3], 3) == IslandBuildRejectReason::OutOfRangeBody,
               "out-of-range contact is rejected");
    expectTrue(is_valid_body_pair(0, 1, 3), "valid body pair passes guard");
    expectTrue(!is_valid_body_pair(0, 0, 3), "self-pair fails body pair guard");

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0, {}, {});
    expectTrue(emptyPreflight.skipped, "build preflight skips empty inputs");
    expectTrue(should_skip_island_build(0, {}, {}), "should_skip_island_build on empty inputs");
}

void testValidateIslandIndicesGuard() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const IslandBuildValidation valid =
        validate_island_indices(graph, static_cast<u32>(contacts.size()), static_cast<u32>(constraints.size()));
    expectTrue(valid.valid, "validate_island_indices passes for consistent graph");
    expectTrue(valid.oobContactIndexCount == 0u, "no out-of-range contact indices");
    expectTrue(valid.oobDistanceIndexCount == 0u, "no out-of-range distance indices");

    const IslandBuildValidation undersized =
        validate_island_indices(graph, 0u, 0u);
    expectTrue(!undersized.valid, "validate_island_indices fails when slot counts are too small");
    expectTrue(undersized.oobContactIndexCount > 0u || undersized.oobDistanceIndexCount > 0u,
               "undersized validation reports out-of-range indices");
}

void testPreflightSleepPassGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_STATIC);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[2] = {0.001f, 0.f, 0.f};
    bodies.addBody({3.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[3] = {1.f, 0.f, 0.f};

    const f32 linearThreshold = 0.01f;
    const f32 angularThreshold = 0.01f;

    const SleepPassPreflight preflight = preflight_sleep_pass(bodies, linearThreshold, angularThreshold);
    expectTrue(!preflight.skipped, "sleep preflight does not skip when active bodies exist");
    expectTrue(preflight.can_sleep_pass(), "sleep preflight can run with active bodies");
    expectTrue(preflight.stats.staticCount == 1u, "sleep preflight counts static bodies");
    expectTrue(preflight.stats.sleepingCount == 1u, "sleep preflight counts sleeping bodies");
    expectTrue(preflight.stats.activeCount == 2u, "sleep preflight counts active bodies");
    expectTrue(preflight.stats.sleepCandidateCount == 1u, "sleep preflight counts sleep candidates");

    expectTrue(is_sleep_candidate_body(bodies, 2, linearThreshold, angularThreshold),
               "low-velocity body is sleep candidate");
    expectTrue(!is_sleep_candidate_body(bodies, 3, linearThreshold, angularThreshold),
               "high-velocity body is not sleep candidate");
    expectTrue(count_sleeping_bodies(bodies) == 1u, "count_sleeping_bodies tallies RB_SLEEPING");

    RigidBodySoA inactive;
    inactive.addBody({0.f, 0.f, 0.f}, 1.f, RB_STATIC);
    inactive.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const SleepPassPreflight inactivePreflight =
        preflight_sleep_pass(inactive, linearThreshold, angularThreshold);
    expectTrue(inactivePreflight.skipped, "sleep preflight skips when no active dynamic bodies");
    expectTrue(should_skip_sleep_pass(inactive, linearThreshold, angularThreshold),
               "should_skip_sleep_pass on inactive scene");
}

void testPreflightWakeCandidatesGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.linearVelocities[1] = {0.5f, 0.f, 0.f};
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    const f32 linearThreshold = 0.01f;
    const f32 angularThreshold = 0.01f;

    const WakePreflight preflight = preflight_wake_candidates(bodies, linearThreshold, angularThreshold);
    expectTrue(!preflight.skipped, "wake preflight does not skip when sleeping bodies exist");
    expectTrue(preflight.has_wake_candidates(), "wake preflight finds wake candidates");
    expectTrue(preflight.stats.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(preflight.stats.wakeCandidateCount == 1u, "wake preflight counts velocity wake candidates");
    expectTrue(preflight.stats.restingSleepingCount == 1u, "wake preflight counts resting sleeping bodies");

    expectTrue(is_wake_candidate_body(bodies, 1, linearThreshold, angularThreshold),
               "sleeping body above threshold is wake candidate");
    expectTrue(!is_wake_candidate_body(bodies, 0, linearThreshold, angularThreshold),
               "resting sleeping body is not wake candidate");
    expectTrue(!is_wake_candidate_body(bodies, 2, linearThreshold, angularThreshold),
               "awake body is not wake candidate");

    const WakePreflight none = preflight_wake_candidates(bodies, linearThreshold, angularThreshold);
    expectTrue(none.stats.wakeCandidateCount == 1u, "wake candidate count stable across calls");
}

void testIslandInactiveAndConstraintPreflights() {
    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, {}, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    const ContactIslandGraph::Island& island = graph.island(0);
    expectTrue(island_all_bodies_inactive(bodies, island), "all-sleeping island is inactive");
    expectTrue(!island_has_active_bodies(bodies, island), "inactive island has no active bodies");
    expectTrue(should_skip_island_solve_all_inactive(bodies, island),
               "should_skip_island_solve_all_inactive on sleeping island");

    bodies.flags[1] &= ~RB_SLEEPING;
    expectTrue(island_has_active_bodies(bodies, island), "island gains active body after wake");
    expectTrue(!should_skip_island_solve_all_inactive(bodies, island),
               "active island is not skipped by inactive guard");

    SolverWorkBuffers work;
    work.init(1, 0, 1);
    const IslandSolveWorkPreflight workPreflight = preflight_island_solve_work(bodies, work);
    expectTrue(workPreflight.insufficientBufferCapacity, "work preflight flags insufficient buffer");
    expectTrue(!workPreflight.can_solve(), "work preflight cannot solve with undersized buffer");

    work.init(4, 0, 1);
    const IslandSolveWorkPreflight sufficient = preflight_island_solve_work(bodies, work);
    expectTrue(sufficient.can_solve(), "work preflight can solve with sufficient buffer");

    const IslandConstraintIndexPreflight indexPreflight =
        preflight_island_constraint_indices(island, bodies, 0u, static_cast<u32>(constraints.size()));
    expectTrue(!indexPreflight.skipped, "constraint index preflight does not skip constrained island");
    expectTrue(indexPreflight.indices_valid(), "constraint indices valid for consistent graph");
    expectTrue(indexPreflight.ownedDistanceCount == 1u, "constraint index preflight counts distance slots");

    const IslandConstraintIndexPreflight oobPreflight =
        preflight_island_constraint_indices_by_index(graph, graph.islandCount() + 1u, bodies, 0u, 1u);
    expectTrue(oobPreflight.skipped, "constraint index preflight skips out-of-range island");

    expectTrue(is_valid_constraint_body_pair(bodies, 0, 1), "valid constraint body pair passes");
    expectTrue(!is_valid_constraint_body_pair(bodies, 0, 9), "out-of-range constraint body pair fails");
    expectTrue(is_body_sleeping(RB_SLEEPING), "is_body_sleeping recognizes flag");
    expectTrue(is_body_static_or_kinematic(RB_STATIC), "is_body_static_or_kinematic recognizes static");
}

void testPreflightDispatchableIslandJobs() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<IslandSolveJob> jobs = collect_dispatchable_island_jobs(graph);
    const f32 dt = 1.f / 60.f;

    const IslandDispatchJobBatchPreflight validPreflight = preflight_dispatchable_island_jobs(graph, jobs, dt);
    expectTrue(!validPreflight.skipped, "job batch preflight does not skip dispatchable jobs");
    expectTrue(validPreflight.can_dispatch(), "job batch preflight can dispatch constrained jobs");
    expectTrue(validPreflight.jobCount == jobs.size(), "job batch preflight records job count");
    expectTrue(!should_skip_dispatchable_island_jobs(jobs, dt),
               "should_skip false for dispatchable job batch with valid dt");

    const IslandDispatchJobBatchPreflight invalidDtPreflight =
        preflight_dispatchable_island_jobs(graph, jobs, 0.f);
    expectTrue(invalidDtPreflight.graph.invalidDt, "job batch preflight rejects invalid dt");
    expectTrue(!invalidDtPreflight.can_dispatch(), "job batch preflight cannot dispatch with invalid dt");
    expectTrue(should_skip_dispatchable_island_jobs(jobs, 0.f),
               "should_skip_dispatchable_island_jobs on invalid dt");

    const std::vector<IslandSolveJob> emptyJobs;
    expectTrue(should_skip_dispatchable_island_jobs(emptyJobs, dt),
               "should_skip_dispatchable_island_jobs on empty job list");
    const IslandDispatchJobBatchPreflight emptyPreflight =
        preflight_dispatchable_island_jobs(graph, emptyJobs, dt);
    expectTrue(emptyPreflight.skipped, "job batch preflight skips empty job list");
}

void testDispatchDispatchableIslandJobsResult() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const std::vector<IslandSolveJob> jobs = collect_dispatchable_island_jobs(graph);
    const IslandBatchDispatchResult batch =
        dispatch_dispatchable_island_jobs_result(bodies, jobs, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "job batch dispatch does not skip constrained jobs");
    expectTrue(batch.dispatchableCount == jobs.size(), "job batch dispatch records job count");
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "job batch dispatch solves all constrained islands");
    expectTrue(batch.any_solved(), "job batch dispatch reports solved islands");

    expectTrue(dispatch_dispatchable_island_jobs(bodies, jobs, work, constraints, dt, 0.f, invMassFn) ==
                   graph.constrainedIslandCount(),
               "job batch dispatch count matches constrained islands");

    const IslandBatchDispatchResult invalidDtBatch =
        dispatch_dispatchable_island_jobs_result(bodies, jobs, work, constraints, 0.f, 0.f, invMassFn);
    expectTrue(invalidDtBatch.skipped, "job batch dispatch skips invalid dt");
}

void testPreflightWarmStartContactImpulsesGraphGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.5f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});
    const f32 dt = 1.f / 60.f;

    const IslandContactImpulseWarmStartStats stats =
        compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    expectTrue(stats.totalIslands == graph.islandCount(), "impulse stats report total island count");
    expectTrue(stats.warmStartableCount == 1u, "impulse stats count warm-startable island");
    expectTrue(stats.emptyCount + stats.warmStartableCount + stats.noImpulseCount == stats.totalIslands,
               "impulse stats partition total islands");
    expectTrue(count_warm_startable_contact_impulse_islands(graph, contacts, dt) == stats.warmStartableCount,
               "count_warm_startable_contact_impulse_islands matches stats");
    expectTrue(has_warm_startable_contact_impulse_islands(graph, contacts, dt),
               "graph has warm-startable contact-impulse islands");

    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    expectTrue(!preflight.skipped, "impulse graph preflight does not skip contact graph");
    expectTrue(preflight.can_warm_start(), "impulse graph preflight can warm-start");
    expectTrue(!should_skip_warm_start_contact_impulses_graph(graph, contacts, dt),
               "should_skip false for impulse graph with valid dt");

    const std::vector<u32> indices = collect_contact_impulse_warm_startable_island_indices(graph, contacts, dt);
    expectTrue(indices.size() == 1u, "collect impulse warm-startable indices returns one island");

    const IslandContactImpulseWarmStartGraphPreflight invalidDtPreflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "impulse graph preflight rejects invalid dt");
    expectTrue(should_skip_warm_start_contact_impulses_graph(graph, contacts, 0.f),
               "should_skip_warm_start_contact_impulses_graph on invalid dt");
}

void testPreflightWarmStartCombinedGraphGuards() {
    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.ensureLambdaCapacity(2, 1);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    const IslandCombinedWarmStartPreflight indexPreflight =
        preflight_warm_start_combined_island_by_index(graph, graph.bodyIsland(0), contacts, dt, priorDistance, priorContact);
    expectTrue(!indexPreflight.skipped, "combined index preflight does not skip contact island");
    expectTrue(indexPreflight.can_warm_start(), "combined index preflight can warm-start");

    const IslandCombinedWarmStartPreflight outOfRange =
        preflight_warm_start_combined_island_by_index(graph, graph.islandCount() + 1u, contacts, dt, priorDistance, priorContact);
    expectTrue(outOfRange.skipped, "combined index preflight skips out-of-range island");
    expectTrue(should_skip_warm_start_combined_island_index(graph, graph.islandCount() + 1u, dt),
               "should_skip_warm_start_combined_island_index on out-of-range index");
    expectTrue(should_skip_warm_start_combined_island_index(graph, graph.bodyIsland(0), 0.f),
               "should_skip_warm_start_combined_island_index on invalid dt");

    const IslandCombinedWarmStartStats stats =
        compute_island_combined_warm_start_stats(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(stats.totalIslands == graph.islandCount(), "combined stats report total island count");
    expectTrue(stats.warmStartableCount == 1u, "combined stats count warm-startable island");
    expectTrue(count_combined_warm_startable_islands(graph, contacts, dt, priorDistance, priorContact) ==
                   stats.warmStartableCount,
               "count_combined_warm_startable_islands matches stats");
    expectTrue(has_combined_warm_startable_islands(graph, contacts, dt, priorDistance, priorContact),
               "graph has combined warm-startable islands");

    const IslandCombinedWarmStartGraphPreflight preflight =
        preflight_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "combined graph preflight does not skip contact graph");
    expectTrue(preflight.can_warm_start(), "combined graph preflight can warm-start");
    expectTrue(!should_skip_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact),
               "should_skip false for combined graph with valid dt");

    const std::vector<u32> indices =
        collect_combined_warm_startable_island_indices(graph, contacts, dt, priorDistance, priorContact);
    expectTrue(indices.size() == 1u, "collect combined warm-startable indices returns one island");

    work.clearLambdas();
    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_combined_result(work, graph, contacts, dt, priorDistance, priorContact);
    expectTrue(!batch.skipped, "combined batch result does not skip contact graph");
    expectTrue(batch.warmedCount == 1u, "combined batch result warms contact island only");
    expectTrue(batch.any_warmed(), "combined batch result reports warmed island");

    const IslandBatchWarmStartResult invalidDtBatch =
        warm_start_all_islands_combined_result(work, graph, contacts, 0.f, priorDistance, priorContact);
    expectTrue(invalidDtBatch.skipped, "combined batch result skips invalid dt");
}

void testPreflightIslandGraphBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 99;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 99, .restLength = 2.f},
    };

    const IslandBuildStats stats = compute_island_build_stats(5, contacts, constraints);
    expectTrue(stats.bodyCount == 5u, "build stats records body count");
    expectTrue(stats.contactCount == 3u, "build stats records contact count");
    expectTrue(stats.invalidContacts == 1u, "build stats counts invalid contacts");
    expectTrue(stats.outOfRangeContacts == 1u, "build stats counts out-of-range contacts");
    expectTrue(stats.outOfRangeDistanceConstraints == 1u,
               "build stats counts out-of-range distance constraints");
    expectTrue(stats.validUnionEdges == 2u, "build stats counts valid union edges");

    const IslandBuildPreflight zeroBodies = preflight_island_graph_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "build preflight skips zero body count");
    expectTrue(!zeroBodies.can_build(), "zero body count cannot build");

    const IslandBuildPreflight validBuild = preflight_island_graph_build(5, contacts, constraints);
    expectTrue(!validBuild.skipped, "build preflight does not skip valid body count");
    expectTrue(validBuild.can_build(), "positive body count can build");
    expectTrue(should_skip_island_graph_build(0u), "should_skip_island_graph_build for zero bodies");
    expectTrue(!should_skip_island_graph_build(5u), "should_skip_island_graph_build allows positive count");
}

void testPreflightSolveIslandJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        if ((bodySoA.flags[index] & RB_SLEEPING) != 0u) {
            return 0.f;
        }
        return bodySoA.invMasses[index];
    };

    const u32 awakeIsland = graph.bodyIsland(0);
    const IslandSolveJobPreflight awakePreflight =
        preflight_solve_island_job(graph.island(awakeIsland), contacts, constraints, bodies, invMassFn);
    expectTrue(!awakePreflight.skipped, "solve preflight does not skip constrained island");
    expectTrue(awakePreflight.can_solve(), "awake island has solvable constraints");
    expectTrue(awakePreflight.solvableConstraintPairs == 2u,
               "awake island counts contact and distance constraints");

    const u32 sleepingIsland = graph.bodyIsland(2);
    const IslandSolveJobPreflight sleepingPreflight =
        preflight_solve_island_job(graph.island(sleepingIsland), contacts, constraints, bodies, invMassFn);
    expectTrue(!sleepingPreflight.skipped, "solve preflight does not skip sleeping island shell");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island has no solvable pairs");
    expectTrue(sleepingPreflight.immovableConstraintPairs == 1u,
               "sleeping island distance pair is immovable");
    expectTrue(should_skip_solve_island_all_sleeping(graph.island(sleepingIsland), bodies),
               "should_skip_solve_island_all_sleeping for sleeping island");
    expectTrue(!should_skip_solve_island_all_sleeping(graph.island(awakeIsland), bodies),
               "awake island is not all-sleeping");

    ContactIslandGraph staticGraph;
    std::vector<DistanceConstraint> staticConstraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    staticGraph.build(2, contacts, staticConstraints);
    RigidBodySoA staticBodies;
    staticBodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    staticBodies.addBody({2.f, 0.f, 0.f}, 0.f, RB_STATIC);
    expectTrue(should_skip_solve_island_all_static(staticGraph.island(0), staticBodies),
               "should_skip_solve_island_all_static for static island");
    expectTrue(should_skip_solve_island_job_preflight(sleepingPreflight),
               "should_skip_solve_island_job_preflight when nothing is solvable");

    ContactIslandGraph oobGraph;
    oobGraph.build(2, contacts, constraints);
    ContactIslandGraph::Island oobIsland = oobGraph.island(awakeIsland);
    oobIsland.distanceIndices.push_back(99u);
    const IslandSolveJobPreflight oobPreflight =
        preflight_solve_island_job(oobIsland, contacts, constraints, bodies, invMassFn);
    expectTrue(oobPreflight.outOfRangeDistances == 1u, "solve preflight counts out-of-range distance index");

    const IslandSolveJobPreflight invalidIndexPreflight =
        preflight_solve_island_job_by_index(graph, graph.islandCount() + 1u, contacts, constraints, bodies, invMassFn);
    expectTrue(invalidIndexPreflight.skipped, "solve preflight by index skips out-of-range island");
}

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    const u32 awake = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleeping = bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 staticBody = bodies.addBody({4.f, 0.f, 0.f}, 0.f, RB_STATIC);
    bodies.forces[awake] = {1.f, 0.f, 0.f};
    bodies.flags[awake] |= RB_CCD;

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = awake;
    contacts.back().bodyB = sleeping;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = awake, .bodyB = staticBody, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(3, contacts, constraints);

    const u32 islandIndex = graph.bodyIsland(awake);
    const IslandSleepPreflight sleepPreflight =
        preflight_island_sleep_state(graph.island(islandIndex), bodies);
    expectTrue(!sleepPreflight.skipped, "sleep preflight does not skip populated island");
    expectTrue(sleepPreflight.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(sleepPreflight.sleepingBodyCount == 1u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepPreflight.staticBodyCount == 1u, "sleep preflight counts static bodies in island");
    expectTrue(sleepPreflight.bodiesWithForces == 1u, "sleep preflight counts bodies with forces");
    expectTrue(sleepPreflight.bodiesWithCcd == 1u, "sleep preflight counts CCD bodies");
    expectTrue(!sleepPreflight.all_dynamic_sleeping(), "mixed island is not all sleeping");
    expectTrue(!sleepPreflight.all_static(), "mixed island is not all static");

    expectTrue(should_skip_sleep_detection_for_body(bodies, awake),
               "should_skip_sleep_detection_for body with forces and CCD");
    expectTrue(!should_skip_sleep_detection_for_body(bodies, sleeping),
               "sleeping dynamic body without forces is eligible for sleep detection");

    const IslandSleepWakePreflight wakePreflight =
        preflight_island_sleep_wake(graph.island(islandIndex), bodies, contacts);
    expectTrue(!wakePreflight.skipped, "sleep/wake preflight does not skip contact island");
    expectTrue(wakePreflight.activeContactCount == 1u, "sleep/wake preflight counts active contacts");
    expectTrue(wakePreflight.contactsTouchingSleepingBody == 1u,
               "sleep/wake preflight counts mixed sleep contacts");
    expectTrue(wakePreflight.should_wake, "sleep/wake preflight requests wake on mixed contact");
    expectTrue(should_wake_island_on_contact(graph.island(islandIndex), bodies, contacts),
               "should_wake_island_on_contact for awake/sleeping pair");

    const IslandSleepPreflight invalidSleepPreflight =
        preflight_island_sleep_state_by_index(graph, graph.islandCount() + 2u, bodies);
    expectTrue(invalidSleepPreflight.skipped, "sleep preflight by index skips out-of-range island");

    const IslandSleepWakePreflight invalidWakePreflight =
        preflight_island_sleep_wake_by_index(graph, graph.islandCount() + 2u, bodies, contacts);
    expectTrue(invalidWakePreflight.skipped, "sleep/wake preflight by index skips out-of-range island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    const IslandBuildPreflight validPreflight = preflight_island_build(4, contacts, constraints);
    expectTrue(!validPreflight.invalidBodyCount, "valid build preflight accepts in-range indices");
    expectTrue(validPreflight.can_build(), "valid build preflight can build");
    expectTrue(validPreflight.validContactCount == 1u, "build preflight counts valid contact");
    expectTrue(validPreflight.validDistanceCount == 2u, "build preflight counts valid distance constraints");
    expectTrue(is_valid_island_build_body_count(4, contacts, constraints),
               "is_valid_island_build_body_count accepts in-range indices");
    expectTrue(!should_skip_island_build(4, contacts, constraints),
               "should_skip false for valid build inputs");

    ContactIslandGraph graph;
    graph.build_guarded(4, contacts, constraints);
    expectTrue(graph.constrainedIslandCount() == 2u,
               "build_guarded partitions valid constraints into islands");

    const IslandBuildPreflight invalidPreflight = preflight_island_build(2, contacts, constraints);
    expectTrue(invalidPreflight.invalidBodyCount, "build preflight rejects out-of-range distance indices");
    expectTrue(invalidPreflight.outOfRangeDistanceCount == 1u,
               "build preflight counts out-of-range distance constraints");
    expectTrue(!invalidPreflight.can_build(), "out-of-range build preflight cannot build");
    expectTrue(should_skip_island_build(2, contacts, constraints),
               "should_skip true for out-of-range build inputs");

    graph.build_guarded(2, contacts, constraints);
    expectTrue(graph.islandCount() == 0u, "build_guarded clears graph when build is skipped");
}

void testPreflightIslandSleepGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(sleepingPreflight.sleepingBodyCount == 1u, "sleep preflight counts sleeping body");
    expectTrue(sleepingPreflight.awakeBodyCount == 1u, "sleep preflight counts awake body");
    expectTrue(!sleepingPreflight.allSleeping, "mixed island is not all sleeping");
    expectTrue(!should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
               "mixed island is not skipped for solve");

    bodies.flags[1] |= RB_SLEEPING;
    const IslandSleepPreflight allSleepingPreflight =
        preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(allSleepingPreflight.allSleeping, "all dynamic bodies sleeping marks allSleeping");
    expectTrue(allSleepingPreflight.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip_sleeping true for all-sleeping island");
    expectTrue(is_island_all_sleeping(graph.island(sleepingIsland), bodies),
               "is_island_all_sleeping true when every dynamic body sleeps");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(graphPreflight.stats.allSleepingCount >= 1u,
               "sleep graph preflight counts all-sleeping island");
    expectTrue(!has_awake_islands(graph, bodies), "no awake islands when all dynamic bodies sleep");
    expectTrue(should_skip_awake_island_dispatch(graph, bodies),
               "awake dispatch skipped when all islands are sleeping");
}

void testPreflightIslandWakeGuards() {
    RigidBodySoA bodies;
    const u32 sleeping = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 awake = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = sleeping;
    contacts.back().bodyB = awake;
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(2, contacts, {});

    const u32 islandIndex = graph.bodyIsland(sleeping);
    const IslandWakePreflight neighborPreflight =
        preflight_island_wake(graph.island(islandIndex), bodies, contacts);
    expectTrue(!neighborPreflight.skipped, "wake preflight does not skip contact island");
    expectTrue(neighborPreflight.hasAwakeNeighborContact, "wake preflight sees awake neighbor contact");
    expectTrue(neighborPreflight.should_wake(), "sleeping body should wake on awake neighbor contact");
    expectTrue(should_wake_island(graph.island(islandIndex), bodies, contacts),
               "should_wake true for awake-neighbor contact");

    bodies.flags[sleeping] |= RB_SLEEPING;
    const u32 wokenByNeighbor = wake_island_bodies_guarded(bodies, graph.island(islandIndex), contacts);
    expectTrue(wokenByNeighbor == 1u, "wake guard clears sleep flag for neighbor wake");
    expectTrue((bodies.flags[sleeping] & RB_SLEEPING) == 0u, "sleeping body woken by neighbor contact");

    bodies.flags[sleeping] |= RB_SLEEPING;
    bodies.forces[sleeping] = {5.f, 0.f, 0.f};
    const IslandWakePreflight forcePreflight =
        preflight_island_wake(graph.island(islandIndex), bodies, contacts);
    expectTrue(forcePreflight.wakeCandidateCount > 0u, "wake preflight counts force-driven wake candidate");
    expectTrue(wake_island_bodies_by_index_guarded(bodies, graph, islandIndex, contacts) == 1u,
               "index wake guard clears sleep flag for forced wake");
}

void testSolveIslandPreflightAndAwakeDispatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const u32 sleepingIsland = graph.bodyIsland(0);
    const IslandConstraintSolvePreflight sleepingPreflight =
        preflight_solve_island(graph.island(sleepingIsland), bodies, constraints, work, dt);
    expectTrue(!sleepingPreflight.can_solve(), "constraint preflight skips all-sleeping island");
    expectTrue(should_skip_solve_island_preflight(graph.island(sleepingIsland), bodies, constraints, work, dt),
               "should_skip_solve true for all-sleeping island");

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "solve_island_job_guarded skips all-sleeping island");
    expectTrue(!dispatch_solve_awake_island(bodies,
                                            graph,
                                            sleepingIsland,
                                            work,
                                            constraints,
                                            dt,
                                            0.f,
                                            invMassFn),
               "dispatch_solve_awake_island skips all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    const IslandConstraintSolvePreflight awakePreflight =
        preflight_solve_island(graph.island(sleepingIsland), bodies, constraints, work, dt);
    expectTrue(awakePreflight.can_solve(), "constraint preflight allows awake island");
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(sleepingIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "solve_island_job_guarded solves awake constrained island");

    const IslandSolveJob job = extract_island(graph, sleepingIsland);
    expectTrue(dispatch_solve_awake_island_job(bodies, job, work, constraints, dt, 0.f, invMassFn),
               "dispatch_solve_awake_island_job solves awake constrained job");
    expectTrue(collect_awake_island_indices(graph, bodies).size() >= 1u,
               "collect_awake_island_indices returns awake island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back({});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back({});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back({});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 99;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 50, .restLength = 2.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(3, contacts, constraints);
    expectTrue(!preflight.skipped, "island build preflight does not skip non-zero body count");
    expectTrue(preflight.validContactCount == 1u, "island build preflight counts valid contacts");
    expectTrue(preflight.invalidContactCount == 1u, "island build preflight counts invalid contacts");
    expectTrue(preflight.rejects.invalidContactPairCount == 1u,
               "island build preflight counts out-of-range contact pairs");
    expectTrue(preflight.rejects.invalidDistancePairCount == 1u,
               "island build preflight counts out-of-range distance pairs");
    expectTrue(!island_build_inputs_valid(3, contacts, constraints),
               "island build inputs invalid when orphans exist");

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "island build preflight skips zero body count");
    expectTrue(should_skip_island_build(0), "should_skip_island_build on zero bodies");

    ContactIslandGraph guardedGraph;
    guardedGraph.build_guarded(3, contacts, constraints);
    expectTrue(guardedGraph.constrainedIslandCount() == 1u,
               "build_guarded ignores invalid pairs and forms one constrained island");

    ContactIslandGraph referenceGraph;
    std::vector<narrowphase::ContactManifold> validContacts;
    validContacts.push_back({});
    validContacts.back().valid = true;
    validContacts.back().bodyA = 0;
    validContacts.back().bodyB = 1;
    const std::vector<DistanceConstraint> validConstraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    referenceGraph.build(3, validContacts, validConstraints);
    expectTrue(guardedGraph.constrainedIslandCount() == referenceGraph.constrainedIslandCount(),
               "build_guarded matches reference build on filtered valid inputs");
}

void testPreflightIslandConstraintIndices() {
    ContactIslandGraph::Island island;
    island.bodyIndices = {0, 1};
    island.contactIndices = {0, 5};
    island.distanceIndices = {0, 3};

    const IslandConstraintIndexPreflight preflight =
        preflight_island_constraint_indices(island, 2, 2);
    expectTrue(!preflight.skipped, "constraint index preflight does not skip constrained island");
    expectTrue(preflight.ownedContactCount == 2u, "constraint index preflight counts owned contacts");
    expectTrue(preflight.ownedDistanceCount == 2u, "constraint index preflight counts owned distances");
    expectTrue(preflight.orphanedContactIndexCount == 1u,
               "constraint index preflight flags orphaned contact index");
    expectTrue(preflight.orphanedDistanceIndexCount == 1u,
               "constraint index preflight flags orphaned distance index");
    expectTrue(!preflight.can_solve(), "constraint index preflight cannot solve with orphans");

    ContactIslandGraph::Island emptyIsland;
    emptyIsland.bodyIndices = {2};
    const IslandConstraintIndexPreflight emptyPreflight =
        preflight_island_constraint_indices(emptyIsland, 2, 2);
    expectTrue(emptyPreflight.skipped, "constraint index preflight skips empty island");
}

void testPreflightSleepingIslandGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 2, .restLength = 0.f},
    };
    graph.build(3, contacts, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const IslandSleepPreflight sleepingPreflight =
        preflight_sleeping_island(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.allSleeping, "sleep preflight detects all-sleeping island");
    expectTrue(sleepingPreflight.movableBodyCount == 0u, "sleep preflight reports zero movable bodies");
    expectTrue(!sleepingPreflight.can_solve(), "sleep preflight cannot solve all-sleeping island");
    expectTrue(should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip_sleeping_island_solve on all-sleeping island");

    const u32 awakeIsland = graph.bodyIsland(2);
    const IslandSleepPreflight awakePreflight =
        preflight_sleeping_island(graph.island(awakeIsland), bodies);
    expectTrue(awakePreflight.movableBodyCount == 1u, "sleep preflight sees awake lone body");
    expectTrue(awakePreflight.can_solve(), "sleep preflight can solve awake island");
    expectTrue(!should_skip_sleeping_island_solve(graph.island(awakeIsland), bodies),
               "should_skip_sleeping_island_solve false for awake island");

    const IslandSolveJob sleepingJob = extract_island(graph, sleepingIsland);
    expectTrue(should_skip_sleeping_island_solve_job(sleepingJob, bodies),
               "should_skip_sleeping_island_solve_job on all-sleeping island");

    const IslandSleepPreflight oobPreflight =
        preflight_sleeping_island_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(oobPreflight.skipped, "sleep preflight by index skips out-of-range island");

    expectTrue(is_body_sleeping(bodies, 0), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_sleeping(bodies, 2), "is_body_sleeping false for awake body");
    expectTrue(!body_has_effective_mass(bodies, 0), "body_has_effective_mass false for sleeping body");
    expectTrue(body_has_effective_mass(bodies, 2), "body_has_effective_mass true for awake body");
}

void testPreflightWakeOnImpulseGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.forces[0] = {0.f, 0.f, 0.f};

    const WakeOnImpulsePreflight noImpulse = preflight_wake_on_impulse(bodies, 0);
    expectTrue(!noImpulse.skipped, "wake preflight does not skip in-range body");
    expectTrue(noImpulse.sleeping, "wake preflight sees sleeping body");
    expectTrue(!noImpulse.shouldWake, "wake preflight does not wake on zero force");
    expectTrue(!should_wake_body_on_impulse(bodies, 0), "should_wake_body_on_impulse false on zero force");

    bodies.forces[0] = {0.01f, 0.f, 0.f};
    const WakeOnImpulsePreflight withImpulse = preflight_wake_on_impulse(bodies, 0);
    expectTrue(withImpulse.shouldWake, "wake preflight wakes sleeping body on non-zero force");
    expectTrue(should_wake_body_on_impulse(bodies, 0), "should_wake_body_on_impulse true on non-zero force");

    const WakeOnImpulsePreflight oob = preflight_wake_on_impulse(bodies, 99);
    expectTrue(oob.skipped, "wake preflight skips out-of-range body");
}

void testPreflightIslandBodyPartition() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const IslandBodyPartitionPreflight preflight = preflight_island_body_partition(graph);
    expectTrue(!preflight.skipped, "body partition preflight does not skip non-empty graph");
    expectTrue(preflight.partitionValid, "body partition preflight validates disjoint islands");
    expectTrue(preflight.duplicateBodyCount == 0u, "body partition preflight finds no duplicates");
    expectTrue(preflight.can_dispatch(), "body partition preflight can dispatch valid partition");
}

void testPreflightConstraintIterations() {
    SolverParams params;
    params.iterations = 8;
    params.residualTolerance = 0.01f;

    const ConstraintIterationPreflight preflight = preflight_constraint_iterations(params);
    expectTrue(!preflight.skipped, "constraint iteration preflight does not skip positive iterations");
    expectTrue(preflight.can_iterate(), "constraint iteration preflight can iterate");
    expectTrue(!should_skip_constraint_iterations(params),
               "should_skip_constraint_iterations false for positive iterations");

    params.iterations = 0;
    const ConstraintIterationPreflight zeroPreflight = preflight_constraint_iterations(params);
    expectTrue(zeroPreflight.skipped, "constraint iteration preflight skips zero iterations");
    expectTrue(!zeroPreflight.can_iterate(), "constraint iteration preflight cannot iterate at zero");
    expectTrue(should_skip_constraint_iterations(params),
               "should_skip_constraint_iterations true for zero iterations");
}

void testDispatchSolveIslandSleepGuarded() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back({});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(2, contacts, {});

    SolverWorkBuffers work;
    work.init(2, 1, 0);
    work.contactManifolds() = contacts;

    const auto invMassFn = [](const RigidBodySoA& soa, u32 index) {
        if ((soa.flags[index] & RB_SLEEPING) != 0u) {
            return 0.f;
        }
        return soa.invMasses[index];
    };

    const u32 islandIndex = graph.bodyIsland(0);
    expectTrue(!dispatch_solve_island_sleep_guarded(bodies,
                                                  graph,
                                                  islandIndex,
                                                  work,
                                                  {},
                                                  1.f / 60.f,
                                                  0.f,
                                                  invMassFn),
               "dispatch_solve_island_sleep_guarded skips all-sleeping island");
    expectTrue(dispatch_solve_island(bodies,
                                     graph,
                                     islandIndex,
                                     work,
                                     {},
                                     1.f / 60.f,
                                     0.f,
                                     invMassFn),
              "dispatch_solve_island still runs on all-sleeping island without sleep guard");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight emptyBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(emptyBodies.skipped, "build preflight skips zero body count");
    expectTrue(!emptyBodies.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::EmptyBodyCount),
               "build reject reason flags empty body count");
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip_island_build on zero bodies");

    ContactIslandGraph graph;
    expectTrue(!graph.build_guarded(0, contacts, constraints), "build_guarded rejects zero body count");
    expectTrue(graph.islandCount() == 0u, "build_guarded clears graph on reject");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 99;
    const IslandBuildPreflight invalidContact = preflight_island_build(2, contacts, {});
    expectTrue(invalidContact.skipped, "build preflight skips invalid contact body index");
    expectTrue(invalidContact.stats.invalidContactCount == 1u,
               "build preflight counts invalid contact");
    expectTrue(island_build_rejects_for_reason(2, contacts, {}, IslandBuildRejectReason::InvalidContactBodyIndex),
               "build reject reason flags invalid contact index");

    contacts.clear();
    constraints = {DistanceConstraint{.bodyA = 0, .bodyB = 5, .restLength = 2.f}};
    const IslandBuildPreflight invalidDistance = preflight_island_build(2, contacts, constraints);
    expectTrue(invalidDistance.skipped, "build preflight skips invalid distance body index");
    expectTrue(invalidDistance.stats.invalidDistanceCount == 1u,
               "build preflight counts invalid distance constraint");

    constraints = {DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f}};
    const IslandBuildPreflight validBuild = preflight_island_build(2, contacts, constraints);
    expectTrue(!validBuild.skipped, "build preflight accepts valid inputs");
    expectTrue(validBuild.can_build(), "build preflight can build valid graph");
    expectTrue(graph.build_guarded(2, contacts, constraints), "build_guarded succeeds for valid inputs");
    expectTrue(graph.islandCount() >= 1u, "build_guarded populates islands");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyBodyCount),
                           "EmptyBodyCount") == 0,
               "reject reason name resolves EmptyBodyCount");
}

void testPreflightIslandSleepAndWakeGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_STATIC);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep(bodies, graph.island(sleepingIsland));
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.fullySleeping, "sleep preflight marks fully sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "fully sleeping island cannot solve");
    expectTrue(should_skip_solve_sleeping_island(bodies, graph.island(sleepingIsland)),
               "should_skip_solve_sleeping_island on sleeping island");
    expectTrue(is_island_fully_sleeping(bodies, graph.island(sleepingIsland)),
               "is_island_fully_sleeping detects sleeping dynamic with static partner");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSleepPreflight awakePreflight =
        preflight_island_sleep(bodies, graph.island(sleepingIsland));
    expectTrue(!awakePreflight.fullySleeping, "awake dynamic body prevents fully sleeping flag");
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(awakePreflight.stats.activeCount == 1u, "sleep stats count active dynamic body");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.forces[0] = {50.f, 0.f, 0.f};
    const IslandWakePreflight forceWake =
        preflight_island_wake(bodies, graph.island(sleepingIsland), 0.01f, 0.01f);
    expectTrue(forceWake.hasExternalForce, "wake preflight detects external force on sleeping body");
    expectTrue(forceWake.can_wake(), "wake preflight can wake on external force");

    bodies.forces[0] = {};
    bodies.linearVelocities[0] = {0.5f, 0.f, 0.f};
    const IslandWakePreflight velocityWake =
        preflight_island_wake(bodies, graph.island(sleepingIsland), 0.01f, 0.01f);
    expectTrue(velocityWake.hasVelocityWake, "wake preflight detects velocity wake");
    expectTrue(should_wake_island(bodies, graph.island(sleepingIsland), 0.01f, 0.01f),
               "should_wake_island true when velocity exceeds threshold");

    const IslandSleepPreflight outOfRange =
        preflight_island_sleep_by_index(bodies, graph, graph.islandCount() + 1u);
    expectTrue(outOfRange.skipped, "sleep preflight by index skips out-of-range");
    expectTrue(should_skip_solve_sleeping_island_index(bodies, graph, graph.islandCount() + 1u),
               "should_skip_solve_sleeping_island_index on out-of-range");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(bodies, graph);
    expectTrue(graphPreflight.fullySleepingIslandCount >= 1u,
               "sleep graph preflight counts fully sleeping islands");
    expectTrue(should_skip_island_sleep_dispatch(bodies, graph),
               "sleep dispatch skipped when all constrained islands sleep");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSleepGraphPreflight awakeGraphPreflight = preflight_island_sleep_graph(bodies, graph);
    expectTrue(awakeGraphPreflight.has_active_islands(), "sleep graph preflight sees active islands");
    expectTrue(!should_skip_island_sleep_dispatch(bodies, graph),
               "sleep dispatch not skipped when awake islands exist");

    const std::vector<u32> awakeIndices = collect_awake_island_indices(bodies, graph);
    expectTrue(awakeIndices.size() == graph.constrainedIslandCount(),
               "collect_awake_island_indices returns all constrained awake islands");
}

void testPreflightIslandConstraintSolveGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_STATIC);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    const u32 sleepingIsland = graph.bodyIsland(0);
    const f32 dt = 1.f / 60.f;

    const IslandConstraintSolvePreflight sleepingSolve =
        preflight_island_constraint_solve(bodies, graph.island(sleepingIsland), dt);
    expectTrue(!sleepingSolve.can_solve(), "constraint solve preflight rejects fully sleeping island");
    expectTrue(sleepingSolve.constraintCount == 1u, "constraint solve preflight counts constraints");
    expectTrue(should_skip_island_constraint_solve(bodies, graph.island(sleepingIsland), dt),
               "should_skip_island_constraint_solve on sleeping island");

    const IslandConstraintSolvePreflight invalidDt =
        preflight_island_constraint_solve(bodies, graph.island(sleepingIsland), 0.f);
    expectTrue(invalidDt.invalidDt, "constraint solve preflight rejects zero dt");
    expectTrue(!invalidDt.can_solve(), "constraint solve preflight cannot solve with invalid dt");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandConstraintSolvePreflight awakeSolve =
        preflight_island_constraint_solve(bodies, graph.island(sleepingIsland), dt);
    expectTrue(awakeSolve.can_solve(), "constraint solve preflight accepts awake island");

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        if ((bodySoA.flags[index] & RB_STATIC) != 0u || (bodySoA.flags[index] & RB_SLEEPING) != 0u) {
            return 0.f;
        }
        return bodySoA.invMasses[index];
    };

    bodies.flags[0] |= RB_SLEEPING;
    const IslandDispatchResult sleepingDispatch = dispatch_solve_awake_island_result(bodies,
                                                                                     graph,
                                                                                     sleepingIsland,
                                                                                     work,
                                                                                     constraints,
                                                                                     dt,
                                                                                     0.f,
                                                                                     invMassFn);
    expectTrue(sleepingDispatch.skipped, "awake dispatch skips fully sleeping island");
    expectTrue(!sleepingDispatch.solved, "awake dispatch does not solve sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandDispatchResult awakeDispatch = dispatch_solve_awake_island_result(bodies,
                                                                                  graph,
                                                                                  sleepingIsland,
                                                                                  work,
                                                                                  constraints,
                                                                                  dt,
                                                                                  0.f,
                                                                                  invMassFn);
    expectTrue(awakeDispatch.solved, "awake dispatch solves awake island");
    expectTrue(!awakeDispatch.skipped, "awake dispatch does not skip awake island");
    expectTrue(dispatch_solve_awake_island(bodies,
                                           graph,
                                           sleepingIsland,
                                           work,
                                           constraints,
                                           dt,
                                           0.f,
                                           invMassFn),
               "dispatch_solve_awake_island succeeds for awake island");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(bodies, graph, graph.islandCount() + 2u, dt);
    expectTrue(outOfRange.skipped, "constraint solve preflight by index skips out-of-range");
}

void testPreflightIslandBuildGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0u, contacts, constraints);
    expectTrue(emptyPreflight.skipped, "build preflight skips zero body count");
    expectTrue(emptyPreflight.reason == IslandBuildRejectReason::EmptyBodyCount,
               "build preflight reports EmptyBodyCount");
    expectTrue(!emptyPreflight.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(should_skip_island_build(0u, contacts, constraints),
               "should_skip_island_build on zero body count");
    expectTrue(!graph.build_guarded(0u, contacts, constraints),
               "build_guarded returns false for zero body count");
    expectTrue(graph.islandCount() == 0u, "build_guarded clears graph on skip");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 99;
    const IslandBuildInput input = count_island_build_input(2u, contacts, constraints);
    expectTrue(input.validContactCount == 0u, "build input counts zero valid out-of-range contacts");
    expectTrue(input.invalidContactCount == 1u, "build input counts invalid contacts");

    constraints.push_back(DistanceConstraint{.bodyA = 0, .bodyB = 5, .restLength = 2.f});
    const IslandBuildInput mixedInput = count_island_build_input(2u, contacts, constraints);
    expectTrue(mixedInput.invalidConstraintCount == 1u, "build input counts invalid distance constraints");

    const IslandBuildPreflight validPreflight = preflight_island_build(2u, {}, constraints);
    expectTrue(!validPreflight.skipped, "build preflight does not skip positive body count");
    expectTrue(validPreflight.can_build(), "build preflight can build with bodies");
    expectTrue(graph.build_guarded(2u, {}, constraints), "build_guarded succeeds for valid input");
    expectTrue(graph.islandCount() > 0u, "build_guarded populates islands");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyBodyCount),
                           "EmptyBodyCount") == 0,
               "build reject reason name resolves EmptyBodyCount");
}

void testPreflightIslandSleepGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(bodies, graph.island(0));
    expectTrue(!sleepPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepPreflight.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(sleepPreflight.sleepingBodyCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepPreflight.awakeBodyCount == 0u, "sleep preflight reports zero awake bodies");
    expectTrue(sleepPreflight.allDynamicSleeping, "sleep preflight marks all-dynamic-sleeping island");
    expectTrue(!sleepPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_sleeping(bodies, graph.island(0)),
               "should_skip_island_solve_sleeping on all-sleeping island");
    expectTrue(island_all_dynamic_bodies_sleeping(bodies, graph.island(0)),
               "island_all_dynamic_bodies_sleeping true for sleeping pair");
    expectTrue(!island_has_awake_dynamic_bodies(bodies, graph.island(0)),
               "island_has_awake_dynamic_bodies false for sleeping pair");

    bodies.flags[1] &= ~RB_SLEEPING;
    const IslandSleepPreflight awakePreflight = preflight_island_sleep(bodies, graph.island(0));
    expectTrue(awakePreflight.awakeBodyCount == 1u, "sleep preflight counts one awake body");
    expectTrue(!awakePreflight.allDynamicSleeping, "mixed island is not all-sleeping");
    expectTrue(awakePreflight.can_solve(), "mixed island can solve");
    expectTrue(is_awake_dynamic_body(bodies, 1u), "awake dynamic body detected");
    expectTrue(!is_awake_dynamic_body(bodies, 0u), "sleeping body is not awake dynamic");

    ContactIslandGraph loneGraph;
    loneGraph.build(2, {}, {});
    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < loneGraph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = loneGraph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandSleepPreflight emptyPreflight = preflight_island_sleep(bodies, island);
        expectTrue(emptyPreflight.skipped, "sleep preflight skips empty island");
        expectTrue(preflight_island_sleep_by_index(loneGraph, bodies, loneGraph.islandCount() + 1u).skipped,
                   "sleep preflight by index skips out-of-range");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for sleep preflight");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(bodies, graph);
    expectTrue(!graphPreflight.skipped, "sleep graph preflight does not skip constrained graph");
    expectTrue(graphPreflight.has_awake_islands(), "sleep graph preflight sees awake island");
    expectTrue(has_awake_islands(bodies, graph), "has_awake_islands true after wake one body");
}

void testPreflightIslandWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.forces[0] = {10.f, 0.f, 0.f};

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, graph.island(0));
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakePreflight.sleepingBodyCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts force-driven wake candidate");
    expectTrue(wakePreflight.needs_wake(), "wake preflight needs wake when force applied");
    expectTrue(body_has_wake_impetus(bodies, 0u), "body_has_wake_impetus true for forced sleeping body");
    expectTrue(!body_has_wake_impetus(bodies, 1u), "body_has_wake_impetus false without impetus");

    bodies.forces[0] = {};
    bodies.linearVelocities[1] = {0.05f, 0.f, 0.f};
    const IslandWakePreflight velocityWake =
        preflight_island_wake(bodies, graph.island(0), 0.01f);
    expectTrue(velocityWake.wakeCandidateCount == 1u, "wake preflight counts velocity wake candidate");
    expectTrue(should_skip_island_wake_check(graph.island(0)) == false,
               "wake check does not skip constrained island");

    ContactIslandGraph emptyConstraintGraph;
    emptyConstraintGraph.build(2, {}, {});
    bool foundEmptyWakeSkip = false;
    for (u32 islandIndex = 0; islandIndex < emptyConstraintGraph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = emptyConstraintGraph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptyWakeSkip = true;
        expectTrue(should_skip_island_wake_check(island), "should_skip_island_wake_check on empty island");
        const IslandWakePreflight emptyWake = preflight_island_wake(bodies, island);
        expectTrue(emptyWake.skipped, "wake preflight skips empty island");
        const IslandWakePreflight indexWake =
            preflight_island_wake_by_index(emptyConstraintGraph, bodies, islandIndex);
        expectTrue(indexWake.skipped, "wake preflight by index skips empty constraint island");
    }
    expectTrue(foundEmptyWakeSkip, "graph exposes empty island for wake preflight");
}

void testDispatchSolveIslandSleepGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    expectTrue(!dispatch_solve_island_sleep_guarded(bodies,
                                                     graph,
                                                     graph.bodyIsland(0),
                                                     work,
                                                     constraints,
                                                     dt,
                                                     0.f,
                                                     invMassFn),
               "sleep-guarded dispatch skips all-sleeping island");

    const IslandDispatchResult sleepingResult = dispatch_solve_island_sleep_guarded_result(
        bodies, graph, graph.bodyIsland(0), work, constraints, dt, 0.f, invMassFn);
    expectTrue(sleepingResult.skipped, "sleep-guarded result skips all-sleeping island");
    expectTrue(!sleepingResult.solved, "sleep-guarded result does not solve sleeping island");

    expectTrue(dispatch_solve_island_sleep_guarded(bodies,
                                                   graph,
                                                   graph.bodyIsland(2),
                                                   work,
                                                   constraints,
                                                   dt,
                                                   0.f,
                                                   invMassFn),
              "sleep-guarded dispatch solves awake island");

    const IslandConstraintSolvePreflight constrainedPreflight =
        preflight_island_constraint_solve(bodies, graph, graph.bodyIsland(2), dt);
    expectTrue(constrainedPreflight.can_solve(), "constraint solve preflight allows awake island");
    expectTrue(!should_skip_island_constraint_solve(bodies, graph, graph.bodyIsland(2), dt),
               "should_skip false for awake constrained island");

    const IslandConstraintSolvePreflight sleepingConstraintPreflight =
        preflight_island_constraint_solve(bodies, graph, graph.bodyIsland(0), dt);
    expectTrue(!sleepingConstraintPreflight.can_solve(),
               "constraint solve preflight blocks all-sleeping island");
    expectTrue(should_skip_island_constraint_solve(bodies, graph, graph.bodyIsland(0), dt),
               "should_skip true for all-sleeping island");

    const std::vector<u32> awakeIndices = collect_awake_dispatchable_island_indices(bodies, graph);
    expectTrue(awakeIndices.size() == 1u, "collect_awake_dispatchable_island_indices returns awake island only");

    const u32 solvedCount = dispatch_awake_islands(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(solvedCount == 1u, "dispatch_awake_islands solves only awake islands");

    const IslandBatchDispatchResult batch = dispatch_awake_islands_result(
        bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(batch.solvedCount == 1u, "dispatch_awake_islands_result solves one awake island");
    expectTrue(batch.any_solved(), "awake batch dispatch reports solved island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 2;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 99, .restLength = 2.f},
    };

    expectTrue(!is_valid_island_build_body_count(0u), "zero body count is invalid for island build");
    expectTrue(is_valid_island_build_body_count(3u), "positive body count is valid for island build");

    const IslandBuildInputStats stats = compute_island_build_input_stats(3u, contacts, constraints);
    expectTrue(stats.bodyCount == 3u, "build stats record body count");
    expectTrue(stats.validContactCount == 1u, "build stats count only valid contacts");
    expectTrue(stats.distanceConstraintCount == 1u, "build stats count distance constraints");
    expectTrue(stats.invalidDistanceBodyRefs == 1u, "build stats count out-of-range distance body refs");

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0u, contacts, constraints);
    expectTrue(emptyPreflight.skipped, "build preflight skips zero body count");
    expectTrue(emptyPreflight.emptyBodyCount, "build preflight marks empty body count");
    expectTrue(!emptyPreflight.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(should_skip_island_build(0u, contacts, constraints),
               "should_skip_island_build on zero body count");

    const IslandBuildPreflight validPreflight = preflight_island_build(3u, contacts, constraints);
    expectTrue(!validPreflight.skipped, "build preflight does not skip positive body count");
    expectTrue(validPreflight.can_build(), "build preflight can build with positive body count");
}

void testBuildIslandGraphGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(!build_island_graph_guarded(graph, 0u, contacts, constraints),
               "guarded build skips zero body count");
    expectTrue(graph.islandCount() == 0u, "guarded skip clears graph");

    expectTrue(build_island_graph_guarded(graph, 3u, contacts, constraints),
               "guarded build succeeds for positive body count");
    expectTrue(graph.islandCount() > 0u, "guarded build produces islands");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build preserves constrained island count");
}

void testPreflightIslandSleepAndWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);
    bodies.forces[0] = {5.f, 0.f, 0.f};

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 activeIsland = graph.bodyIsland(2);
    const ContactIslandGraph::Island& sleepingIsle = graph.island(sleepingIsland);
    const ContactIslandGraph::Island& activeIsle = graph.island(activeIsland);

    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(bodies, sleepingIsle);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.allSleeping, "both-sleeping island is all sleeping");
    expectTrue(sleepingPreflight.allStaticOrSleeping, "both-sleeping island has no active dynamics");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(island_all_bodies_sleeping(bodies, sleepingIsle),
               "island_all_bodies_sleeping on both-sleeping island");
    expectTrue(should_skip_solve_sleeping_island(bodies, sleepingIsle),
               "should_skip_solve_sleeping_island on all-sleeping island");

    const IslandSleepPreflight activePreflight = preflight_island_sleep(bodies, activeIsle);
    expectTrue(activePreflight.stats.activeDynamicCount == 2u,
               "active island reports dynamic bodies");
    expectTrue(activePreflight.can_solve(), "active island can solve");

    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, sleepingIsle);
    expectTrue(wakePreflight.forceWakeCount == 1u, "sleep preflight counts force wake candidate");
    expectTrue(wakePreflight.can_wake(), "sleeping island with force can wake");
    expectTrue(should_wake_island(bodies, sleepingIsle), "should_wake_island with force candidate");

    const IslandWakePreflight activeWakePreflight = preflight_island_wake(bodies, activeIsle);
    expectTrue(!activeWakePreflight.can_wake(), "active-only island has no wake candidates");
    expectTrue(should_skip_island_wake(bodies, activeIsle), "should_skip_island_wake on active island");

    const IslandWakePreflight outOfRangeWake =
        preflight_island_wake_by_index(bodies, graph, graph.islandCount() + 1u);
    expectTrue(outOfRangeWake.skipped, "wake preflight skips out-of-range island index");
}

void testWakeIslandBodiesGuarded() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.sleepTimers[0] = 0.5f;
    bodies.sleepTimers[1] = 0.5f;
    bodies.forces[0] = {12.f, 0.f, 0.f};

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    ContactIslandGraph graph;
    graph.build(2, contacts, {});

    const u32 wokenCount = wake_island_bodies_guarded(bodies, graph.island(0));
    expectTrue(wokenCount == 1u, "guarded wake activates force-driven sleeping body");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "force wake clears sleeping flag on body A");
    expectTrue((bodies.flags[1] & RB_SLEEPING) != 0u, "non-force sleeping body remains asleep");
    expectNear(bodies.sleepTimers[0], 0.f, 1e-6f, "force wake resets sleep timer");

    expectTrue(wake_island_bodies_by_index_guarded(bodies, graph, graph.islandCount() + 2u) == 0u,
               "index guarded wake skips out-of-range island");
}

void testPreflightIslandConstraintSolveGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);
    const f32 dt = 1.f / 60.f;

    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve(
        bodies,
        graph.island(graph.bodyIsland(0)),
        constraints,
        contacts,
        dt);
    expectTrue(!sleepingPreflight.skipped, "constraint preflight does not skip constrained island");
    expectTrue(sleepingPreflight.resolvableConstraintCount == 0u,
               "all-sleeping island has zero resolvable constraints");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot constraint-solve");

    const IslandConstraintSolvePreflight activePreflight = preflight_island_constraint_solve(
        bodies,
        graph.island(graph.bodyIsland(2)),
        constraints,
        contacts,
        dt);
    expectTrue(activePreflight.resolvableConstraintCount == 2u,
               "active island counts contact and distance resolvable constraints");
    expectTrue(activePreflight.can_solve(), "active island can constraint-solve");

    const IslandConstraintSolvePreflight invalidDtPreflight = preflight_island_constraint_solve(
        bodies,
        graph.island(graph.bodyIsland(2)),
        constraints,
        contacts,
        0.f);
    expectTrue(invalidDtPreflight.invalidDt, "constraint preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_solve(), "constraint preflight cannot solve with invalid dt");
    expectTrue(should_skip_island_constraint_solve(bodies,
                                                   graph.island(graph.bodyIsland(0)),
                                                   constraints,
                                                   contacts,
                                                   dt),
               "should_skip_island_constraint_solve on all-sleeping island");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(bodies,
                                                   graph,
                                                   graph.islandCount() + 1u,
                                                   constraints,
                                                   contacts,
                                                   dt);
    expectTrue(outOfRange.skipped, "constraint preflight skips out-of-range island index");
}

void testSolveIslandJobGuarded() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.contactManifolds() = contacts;
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        return island_effective_inv_mass(bodySoA, index);
    };

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(graph.bodyIsland(0)),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");

    const vec3 activeStartB = bodies.predictedPositions[3];
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(graph.bodyIsland(2)),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve resolves active island");
    const f32 dist = (bodies.predictedPositions[2] - bodies.predictedPositions[3]).length();
    expectTrue(dist < 12.1f, "guarded active island solve moves bodies toward rest length");
    expectTrue((bodies.predictedPositions[3] - activeStartB).length() > 1e-6f,
               "guarded active island solve modifies predicted positions");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "island build preflight skips zero bodies");
    expectTrue(zeroBodies.zeroBodies, "island build preflight flags zero bodies");
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodies,
               "island build reject reason is ZeroBodies");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip_island_build on zero bodies");
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
               "island_build_rejects_for_reason matches ZeroBodies");

    const IslandBuildPreflight noConstraints = preflight_island_build(3, {}, {});
    expectTrue(!noConstraints.skipped, "island build preflight does not skip lone bodies");
    expectTrue(noConstraints.noConstraints, "island build preflight flags no constraints");
    expectTrue(noConstraints.reason == IslandBuildRejectReason::NoConstraints,
               "island build reject reason is NoConstraints");
    expectTrue(noConstraints.stats.constraintEdgeCount == 0u,
               "island build stats report zero constraint edges");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    const IslandBuildPreflight constrained = preflight_island_build(2, contacts, constraints);
    expectTrue(!constrained.skipped, "island build preflight does not skip constrained scene");
    expectTrue(constrained.reason == IslandBuildRejectReason::None,
               "island build reject reason is None for constrained scene");
    expectTrue(constrained.stats.validContactCount == 1u,
               "island build stats count valid contacts");
    expectTrue(constrained.stats.constraintEdgeCount == 2u,
               "island build stats sum contacts and distance constraints");
}

void testBuildIslandGraphGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(!build_island_graph_guarded(graph, 0, contacts, constraints),
               "guarded build skips zero bodies");
    expectTrue(graph.islandCount() == 0u, "guarded skip clears graph");

    expectTrue(build_island_graph_guarded(graph, 3, contacts, constraints),
               "guarded build runs for constrained scene");
    expectTrue(graph.islandCount() >= 1u, "guarded build populates islands");
    expectTrue(graph.constrainedIslandCount() == 1u,
               "guarded build surfaces constrained island");
}

void testPreflightIslandSolveBodiesSleepGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandSolveBodyPreflight sleepingPreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(sleepingPreflight.allSleeping, "solve preflight flags all-sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_for_sleep on all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSolveBodyPreflight awakePreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(awakePreflight.can_solve(), "mixed island can solve with awake dynamic body");
    expectTrue(!should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip false when island has awake dynamic body");

    const IslandSolveJob job = extract_island(graph, constrainedIndex);
    expectTrue(should_solve_island_with_bodies(job, bodies, graph.island(constrainedIndex)),
               "should_solve_island_with_bodies allows awake constrained island");
    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(should_skip_island_solve_job_for_sleep(job, bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_job_for_sleep on all-sleeping island");
}

void testDispatchSolveIslandWithBodyGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandDispatchResult skipped =
        dispatch_solve_island_with_body_guards_result(bodies,
                                                      graph,
                                                      0u,
                                                      work,
                                                      constraints,
                                                      1.f / 60.f,
                                                      0.f,
                                                      invMassFn);
    expectTrue(skipped.skipped, "body-guarded dispatch skips all-sleeping island");
    expectTrue(!skipped.solved, "body-guarded dispatch does not solve sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    expectTrue(dispatch_solve_island_with_body_guards(bodies,
                                                    graph,
                                                    0u,
                                                    work,
                                                    constraints,
                                                    1.f / 60.f,
                                                    0.f,
                                                    invMassFn),
               "body-guarded dispatch solves island with awake dynamic body");
}

void testPreflightIslandSleepWakeGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[0] = {};
    bodies.linearVelocities[1] = {};
    bodies.angularVelocities[0] = {};
    bodies.angularVelocities[1] = {};

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    const IslandSleepPreflight sleepPreflight =
        preflight_island_sleep(bodies, graph.island(islandA), params, dt);
    expectTrue(sleepPreflight.can_sleep(), "sleep preflight allows low-velocity contact island");
    expectTrue(sleepPreflight.dynamicCount == 2u, "sleep preflight counts dynamic bodies");

    const IslandSleepPreflight invalidDt =
        preflight_island_sleep(bodies, graph.island(islandA), params, 0.f);
    expectTrue(invalidDt.invalidDt, "sleep preflight rejects zero dt");
    expectTrue(!invalidDt.can_sleep(), "sleep preflight cannot sleep with invalid dt");
    expectTrue(should_skip_island_sleep(graph.island(islandA), 0.f),
               "should_skip_island_sleep on invalid dt");

    bodies.linearVelocities[0] = {1.f, 0.f, 0.f};
    const IslandWakePreflight wakePreflight =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(wakePreflight.should_wake(), "wake preflight sees above-threshold velocity");
    expectTrue(wakePreflight.aboveThresholdCount == 1u,
               "wake preflight counts above-threshold bodies");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    bodies.linearVelocities[0] = {};
    const IslandWakePreflight sleepingWake =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(sleepingWake.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(sleepingWake.should_wake(), "wake preflight should wake sleeping island");
}

void testSleepWakeIslandGuardedBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.sleepTimers[0] = 0.09f;
    bodies.sleepTimers[1] = 0.09f;

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    expectTrue(sleep_island_bodies_guarded(bodies, graph.island(islandA), params, dt),
               "guarded sleep accumulates timers for low-velocity island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) != 0u, "guarded sleep marks first body sleeping");
    expectTrue((bodies.flags[1] & RB_SLEEPING) != 0u, "guarded sleep marks second body sleeping");

    const IslandSleepGraphPreflight graphSleep =
        preflight_island_sleep_graph(bodies, graph, params, dt);
    expectTrue(graphSleep.skipped, "graph sleep preflight skips when no island can sleep");

    expectTrue(wake_island_bodies_guarded(bodies, graph.island(islandA)),
               "guarded wake clears sleeping flag on island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "guarded wake clears first body sleep flag");
    expectTrue(bodies.sleepTimers[0] == 0.f, "guarded wake resets sleep timer");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const u32 wokeCount = wake_all_islands_guarded(bodies, graph, params);
    expectTrue(wokeCount == 1u, "batch wake guarded wakes contact island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "batch wake clears sleeping flags");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 2;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 99, .restLength = 2.f},
    };

    expectTrue(!is_valid_island_build_body_count(0u), "zero body count is invalid for island build");
    expectTrue(is_valid_island_build_body_count(3u), "positive body count is valid for island build");

    const IslandBuildInputStats stats = compute_island_build_input_stats(3u, contacts, constraints);
    expectTrue(stats.bodyCount == 3u, "build stats record body count");
    expectTrue(stats.validContactCount == 1u, "build stats count only valid contacts");
    expectTrue(stats.distanceConstraintCount == 1u, "build stats count distance constraints");
    expectTrue(stats.invalidDistanceBodyRefs == 1u, "build stats count out-of-range distance body refs");

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0u, contacts, constraints);
    expectTrue(emptyPreflight.skipped, "build preflight skips zero body count");
    expectTrue(emptyPreflight.emptyBodyCount, "build preflight marks empty body count");
    expectTrue(!emptyPreflight.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(should_skip_island_build(0u, contacts, constraints),
               "should_skip_island_build on zero body count");

    const IslandBuildPreflight validPreflight = preflight_island_build(3u, contacts, constraints);
    expectTrue(!validPreflight.skipped, "build preflight does not skip positive body count");
    expectTrue(validPreflight.can_build(), "build preflight can build with positive body count");
}

void testBuildIslandGraphGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(!build_island_graph_guarded(graph, 0u, contacts, constraints),
               "guarded build skips zero body count");
    expectTrue(graph.islandCount() == 0u, "guarded skip clears graph");

    expectTrue(build_island_graph_guarded(graph, 3u, contacts, constraints),
               "guarded build succeeds for positive body count");
    expectTrue(graph.islandCount() > 0u, "guarded build produces islands");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build preserves constrained island count");
}

void testPreflightIslandSleepAndWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);
    bodies.forces[0] = {5.f, 0.f, 0.f};

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 activeIsland = graph.bodyIsland(2);
    const ContactIslandGraph::Island& sleepingIsle = graph.island(sleepingIsland);
    const ContactIslandGraph::Island& activeIsle = graph.island(activeIsland);

    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(bodies, sleepingIsle);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.allSleeping, "both-sleeping island is all sleeping");
    expectTrue(sleepingPreflight.allStaticOrSleeping, "both-sleeping island has no active dynamics");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(island_all_bodies_sleeping(bodies, sleepingIsle),
               "island_all_bodies_sleeping on both-sleeping island");
    expectTrue(should_skip_solve_sleeping_island(bodies, sleepingIsle),
               "should_skip_solve_sleeping_island on all-sleeping island");

    const IslandSleepPreflight activePreflight = preflight_island_sleep(bodies, activeIsle);
    expectTrue(activePreflight.stats.activeDynamicCount == 2u,
               "active island reports dynamic bodies");
    expectTrue(activePreflight.can_solve(), "active island can solve");

    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, sleepingIsle);
    expectTrue(wakePreflight.forceWakeCount == 1u, "sleep preflight counts force wake candidate");
    expectTrue(wakePreflight.can_wake(), "sleeping island with force can wake");
    expectTrue(should_wake_island(bodies, sleepingIsle), "should_wake_island with force candidate");

    const IslandWakePreflight activeWakePreflight = preflight_island_wake(bodies, activeIsle);
    expectTrue(!activeWakePreflight.can_wake(), "active-only island has no wake candidates");
    expectTrue(should_skip_island_wake(bodies, activeIsle), "should_skip_island_wake on active island");

    const IslandWakePreflight outOfRangeWake =
        preflight_island_wake_by_index(bodies, graph, graph.islandCount() + 1u);
    expectTrue(outOfRangeWake.skipped, "wake preflight skips out-of-range island index");
}

void testWakeIslandBodiesGuarded() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.sleepTimers[0] = 0.5f;
    bodies.sleepTimers[1] = 0.5f;
    bodies.forces[0] = {12.f, 0.f, 0.f};

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    ContactIslandGraph graph;
    graph.build(2, contacts, {});

    const u32 wokenCount = wake_island_bodies_guarded(bodies, graph.island(0));
    expectTrue(wokenCount == 1u, "guarded wake activates force-driven sleeping body");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "force wake clears sleeping flag on body A");
    expectTrue((bodies.flags[1] & RB_SLEEPING) != 0u, "non-force sleeping body remains asleep");
    expectNear(bodies.sleepTimers[0], 0.f, 1e-6f, "force wake resets sleep timer");

    expectTrue(wake_island_bodies_by_index_guarded(bodies, graph, graph.islandCount() + 2u) == 0u,
               "index guarded wake skips out-of-range island");
}

void testPreflightIslandConstraintSolveGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);
    const f32 dt = 1.f / 60.f;

    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve(
        bodies,
        graph.island(graph.bodyIsland(0)),
        constraints,
        contacts,
        dt);
    expectTrue(!sleepingPreflight.skipped, "constraint preflight does not skip constrained island");
    expectTrue(sleepingPreflight.resolvableConstraintCount == 0u,
               "all-sleeping island has zero resolvable constraints");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot constraint-solve");

    const IslandConstraintSolvePreflight activePreflight = preflight_island_constraint_solve(
        bodies,
        graph.island(graph.bodyIsland(2)),
        constraints,
        contacts,
        dt);
    expectTrue(activePreflight.resolvableConstraintCount == 2u,
               "active island counts contact and distance resolvable constraints");
    expectTrue(activePreflight.can_solve(), "active island can constraint-solve");

    const IslandConstraintSolvePreflight invalidDtPreflight = preflight_island_constraint_solve(
        bodies,
        graph.island(graph.bodyIsland(2)),
        constraints,
        contacts,
        0.f);
    expectTrue(invalidDtPreflight.invalidDt, "constraint preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_solve(), "constraint preflight cannot solve with invalid dt");
    expectTrue(should_skip_island_constraint_solve(bodies,
                                                   graph.island(graph.bodyIsland(0)),
                                                   constraints,
                                                   contacts,
                                                   dt),
               "should_skip_island_constraint_solve on all-sleeping island");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(bodies,
                                                   graph,
                                                   graph.islandCount() + 1u,
                                                   constraints,
                                                   contacts,
                                                   dt);
    expectTrue(outOfRange.skipped, "constraint preflight skips out-of-range island index");
}

void testSolveIslandJobGuarded() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.contactManifolds() = contacts;
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        return island_effective_inv_mass(bodySoA, index);
    };

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(graph.bodyIsland(0)),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");

    const vec3 activeStartB = bodies.predictedPositions[3];
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(graph.bodyIsland(2)),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve resolves active island");
    const f32 dist = (bodies.predictedPositions[2] - bodies.predictedPositions[3]).length();
    expectTrue(dist < 12.1f, "guarded active island solve moves bodies toward rest length");
    expectTrue((bodies.predictedPositions[3] - activeStartB).length() > 1e-6f,
               "guarded active island solve modifies predicted positions");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight emptyBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(emptyBodies.skipped, "island build preflight skips zero body count");
    expectTrue(emptyBodies.reason == IslandBuildRejectReason::EmptyBodyCount,
               "island build preflight reports EmptyBodyCount");
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip_island_build on zero bodies");

    ContactIslandGraph guardedGraph;
    expectTrue(!build_island_graph_guarded(guardedGraph, 0, contacts, constraints),
               "guarded build skips zero body count");
    expectTrue(guardedGraph.islandCount() == 0u, "guarded skip leaves graph empty");

    const IslandBuildPreflight loneBodies = preflight_island_build(2, {}, {});
    expectTrue(!loneBodies.skipped, "island build preflight allows lone bodies");
    expectTrue(loneBodies.noConstraints, "lone bodies flagged as noConstraints");
    expectTrue(!should_skip_island_build(2, {}, {}), "should_skip false for lone bodies");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 9;
    contacts.back().bodyB = 10;

    const IslandBuildPreflight constrained = preflight_island_build(3, contacts, constraints);
    expectTrue(constrained.can_build(), "island build preflight allows constrained graph");
    expectTrue(!constrained.noConstraints, "constrained graph is not noConstraints");
    expectTrue(constrained.validContactCount == 1u, "island build counts valid contacts only");
    expectTrue(constrained.invalidContactCount == 1u, "island build counts invalid contacts");
    expectTrue(count_valid_island_contacts(3, contacts) == 1u, "count_valid_island_contacts matches preflight");
    expectTrue(has_island_build_constraints(3, contacts, constraints),
               "has_island_build_constraints true for mixed graph");

    ContactIslandGraph graph;
    expectTrue(build_island_graph_guarded(graph, 3, contacts, constraints),
               "guarded build succeeds for constrained graph");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build produces constrained island");

    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyBodyCount),
                             "EmptyBodyCount") == 0,
               "island build reject reason name resolves EmptyBodyCount");
}

void testPreflightIslandConstraintSolveGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandConstraintSolvePreflight awakePreflight =
        preflight_island_constraint_solve(bodies, graph.island(constrainedIndex), 1.f / 60.f);
    expectTrue(!awakePreflight.skipped, "constraint solve preflight does not skip awake island");
    expectTrue(awakePreflight.can_solve(), "awake constrained island can solve");
    expectTrue(awakePreflight.constraintCount == 1u, "constraint solve preflight counts constraints");
    expectTrue(awakePreflight.awakeDynamicCount == 2u, "constraint solve preflight counts awake dynamics");
    expectTrue(!should_skip_island_constraint_solve(bodies, graph.island(constrainedIndex), 1.f / 60.f),
               "should_skip false for awake constrained island");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const IslandConstraintSolvePreflight allSleepingPreflight =
        preflight_island_constraint_solve(bodies, graph.island(constrainedIndex), 1.f / 60.f);
    expectTrue(allSleepingPreflight.allDynamicSleeping, "constraint solve preflight detects all sleeping");
    expectTrue(!allSleepingPreflight.can_solve(), "all-sleeping island cannot constraint solve");
    expectTrue(should_skip_island_constraint_solve(bodies, graph.island(constrainedIndex), 1.f / 60.f),
               "should_skip true when all dynamic bodies sleeping");

    const IslandConstraintSolvePreflight invalidDt =
        preflight_island_constraint_solve_by_index(bodies, graph, constrainedIndex, 0.f);
    expectTrue(invalidDt.invalidDt, "constraint solve preflight rejects zero dt");
    expectTrue(!invalidDt.can_solve(), "invalid dt blocks constraint solve");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(bodies, graph, graph.islandCount() + 1u, 1.f / 60.f);
    expectTrue(outOfRange.skipped, "constraint solve index preflight skips out-of-range");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        expectTrue(should_skip_island_constraint_solve_index(bodies, graph, islandIndex, 1.f / 60.f),
                   "should_skip constraint solve on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for constraint solve preflight");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(3, 0, 1);
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(constrainedIndex),
                                        work,
                                        constraints,
                                        1.f / 60.f,
                                        0.f,
                                        [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job_guarded solves awake constrained island");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(constrainedIndex),
                                         work,
                                         constraints,
                                         1.f / 60.f,
                                         0.f,
                                         [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job_guarded skips all-sleeping island");
}

void testPreflightIslandSleepWakeGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({40.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[0] = {0.001f, 0.f, 0.f};
    bodies.linearVelocities[1] = {0.001f, 0.f, 0.f};
    bodies.forces[2] = {5.f, 0.f, 0.f};

    const f32 sleepLinear = 0.01f;
    const f32 sleepAngular = 0.01f;

    const u32 contactIsland = graph.bodyIsland(0);
    const IslandSleepPreflight sleepPreflight =
        preflight_island_sleep(bodies, graph.island(contactIsland), sleepLinear, sleepAngular);
    expectTrue(!sleepPreflight.skipped, "sleep preflight does not skip contact island");
    expectTrue(sleepPreflight.can_consider_sleep(), "slow contact island can consider sleep");
    expectTrue(sleepPreflight.belowThresholdCount == 2u, "sleep preflight counts below-threshold bodies");
    expectTrue(!sleepPreflight.all_dynamic_sleeping(), "contact island is not all sleeping yet");

    const u32 sleepingIsland = graph.bodyIsland(2);
    const IslandSleepPreflight allSleepingPreflight =
        preflight_island_sleep_by_index(bodies, graph, sleepingIsland, sleepLinear, sleepAngular);
    expectTrue(allSleepingPreflight.all_dynamic_sleeping(), "sleep preflight detects all-sleeping island");
    expectTrue(should_skip_island_sleep_index(graph, graph.islandCount() + 2u),
               "should_skip sleep index on out-of-range island");

    const IslandWakePreflight wakeSleeping =
        preflight_island_wake(bodies, graph.island(sleepingIsland));
    expectTrue(!wakeSleeping.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakeSleeping.should_wake(), "sleeping island with distance constraint should wake");
    expectTrue(wakeSleeping.ownedDistanceCount == 1u, "wake preflight counts owned distance constraints");
    expectTrue(wakeSleeping.hasExternalForces, "wake preflight detects external forces on sleeping body");

    bodies.forces[2] = {};
    const IslandWakePreflight wakeContact =
        preflight_island_wake_by_index(bodies, graph, contactIsland);
    expectTrue(!wakeContact.should_wake(), "awake contact island does not need wake");
    expectTrue(wakeContact.awakeBodyCount == 2u, "wake preflight counts awake dynamic bodies");
    expectTrue(wakeContact.ownedContactCount == 1u, "wake preflight counts owned contacts");

    bool foundEmptySleepSkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySleepSkip = true;
        expectTrue(should_skip_island_sleep_check(island), "should_skip sleep check on empty island");
        expectTrue(should_skip_island_wake_check(island), "should_skip wake check on empty island");
        const IslandWakePreflight emptyWake = preflight_island_wake(bodies, island);
        expectTrue(emptyWake.skipped, "wake preflight skips empty island");
        expectTrue(!emptyWake.should_wake(), "empty island should not wake");
    }
    expectTrue(foundEmptySleepSkip, "graph exposes empty island for sleep/wake preflight");

    const IslandSleepWakeStats stats =
        compute_island_sleep_wake_stats(bodies, graph, sleepLinear, sleepAngular);
    expectTrue(stats.totalIslands == graph.islandCount(), "sleep/wake stats report total islands");
    expectTrue(stats.sleepCandidateCount >= 1u, "sleep/wake stats count sleep candidates");
    expectTrue(stats.allSleepingCount >= 1u, "sleep/wake stats count all-sleeping islands");
    expectTrue(stats.wakeCandidateCount >= 1u, "sleep/wake stats count wake candidates");
    expectTrue(stats.emptyCount + stats.sleepCandidateCount <= stats.totalIslands,
               "sleep/wake stats partition constrained islands");

    expectTrue(is_body_sleeping(RB_SLEEPING), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_dynamic_awake(bodies, 2), "is_body_dynamic_awake false for sleeping body");
    expectTrue(is_body_dynamic_awake(bodies, 0), "is_body_dynamic_awake true for awake dynamic body");
    expectTrue(island_has_awake_dynamic_bodies(bodies, graph.island(contactIsland)),
               "island_has_awake_dynamic_bodies true for contact island");
    expectTrue(!island_all_dynamic_bodies_sleeping(bodies, graph.island(contactIsland)),
               "island_all_dynamic_bodies_sleeping false for awake island");
    expectTrue(island_all_dynamic_bodies_sleeping(bodies, graph.island(sleepingIsland)),
               "island_all_dynamic_bodies_sleeping true for sleeping island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "island build preflight skips zero bodies");
    expectTrue(zeroBodies.zeroBodies, "island build preflight flags zero bodies");
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodies,
               "island build reject reason is ZeroBodies");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip_island_build on zero bodies");
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
               "island_build_rejects_for_reason matches ZeroBodies");

    const IslandBuildPreflight noConstraints = preflight_island_build(3, {}, {});
    expectTrue(!noConstraints.skipped, "island build preflight does not skip lone bodies");
    expectTrue(noConstraints.noConstraints, "island build preflight flags no constraints");
    expectTrue(noConstraints.reason == IslandBuildRejectReason::NoConstraints,
               "island build reject reason is NoConstraints");
    expectTrue(noConstraints.stats.constraintEdgeCount == 0u,
               "island build stats report zero constraint edges");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    const IslandBuildPreflight constrained = preflight_island_build(2, contacts, constraints);
    expectTrue(!constrained.skipped, "island build preflight does not skip constrained scene");
    expectTrue(constrained.reason == IslandBuildRejectReason::None,
               "island build reject reason is None for constrained scene");
    expectTrue(constrained.stats.validContactCount == 1u,
               "island build stats count valid contacts");
    expectTrue(constrained.stats.constraintEdgeCount == 2u,
               "island build stats sum contacts and distance constraints");
}

void testBuildIslandGraphGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(!build_island_graph_guarded(graph, 0, contacts, constraints),
               "guarded build skips zero bodies");
    expectTrue(graph.islandCount() == 0u, "guarded skip clears graph");

    expectTrue(build_island_graph_guarded(graph, 3, contacts, constraints),
               "guarded build runs for constrained scene");
    expectTrue(graph.islandCount() >= 1u, "guarded build populates islands");
    expectTrue(graph.constrainedIslandCount() == 1u,
               "guarded build surfaces constrained island");
}

void testPreflightIslandSolveBodiesSleepGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandSolveBodyPreflight sleepingPreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(sleepingPreflight.allSleeping, "solve preflight flags all-sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_for_sleep on all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSolveBodyPreflight awakePreflight =
        preflight_island_solve_bodies(bodies, graph.island(constrainedIndex));
    expectTrue(awakePreflight.can_solve(), "mixed island can solve with awake dynamic body");
    expectTrue(!should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip false when island has awake dynamic body");

    const IslandSolveJob job = extract_island(graph, constrainedIndex);
    expectTrue(should_solve_island_with_bodies(job, bodies, graph.island(constrainedIndex)),
               "should_solve_island_with_bodies allows awake constrained island");
    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(should_skip_island_solve_job_for_sleep(job, bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_job_for_sleep on all-sleeping island");
}

void testDispatchSolveIslandWithBodyGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandDispatchResult skipped =
        dispatch_solve_island_with_body_guards_result(bodies,
                                                      graph,
                                                      0u,
                                                      work,
                                                      constraints,
                                                      1.f / 60.f,
                                                      0.f,
                                                      invMassFn);
    expectTrue(skipped.skipped, "body-guarded dispatch skips all-sleeping island");
    expectTrue(!skipped.solved, "body-guarded dispatch does not solve sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    expectTrue(dispatch_solve_island_with_body_guards(bodies,
                                                    graph,
                                                    0u,
                                                    work,
                                                    constraints,
                                                    1.f / 60.f,
                                                    0.f,
                                                    invMassFn),
               "body-guarded dispatch solves island with awake dynamic body");
}

void testPreflightIslandSleepWakeGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[0] = {};
    bodies.linearVelocities[1] = {};
    bodies.angularVelocities[0] = {};
    bodies.angularVelocities[1] = {};

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    const IslandSleepPreflight sleepPreflight =
        preflight_island_sleep(bodies, graph.island(islandA), params, dt);
    expectTrue(sleepPreflight.can_sleep(), "sleep preflight allows low-velocity contact island");
    expectTrue(sleepPreflight.dynamicCount == 2u, "sleep preflight counts dynamic bodies");

    const IslandSleepPreflight invalidDt =
        preflight_island_sleep(bodies, graph.island(islandA), params, 0.f);
    expectTrue(invalidDt.invalidDt, "sleep preflight rejects zero dt");
    expectTrue(!invalidDt.can_sleep(), "sleep preflight cannot sleep with invalid dt");
    expectTrue(should_skip_island_sleep(graph.island(islandA), 0.f),
               "should_skip_island_sleep on invalid dt");

    bodies.linearVelocities[0] = {1.f, 0.f, 0.f};
    const IslandWakePreflight wakePreflight =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(wakePreflight.should_wake(), "wake preflight sees above-threshold velocity");
    expectTrue(wakePreflight.aboveThresholdCount == 1u,
               "wake preflight counts above-threshold bodies");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    bodies.linearVelocities[0] = {};
    const IslandWakePreflight sleepingWake =
        preflight_island_wake(bodies, graph.island(islandA), params);
    expectTrue(sleepingWake.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(sleepingWake.should_wake(), "wake preflight should wake sleeping island");
}

void testSleepWakeIslandGuardedBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(3, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({0.5f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.sleepTimers[0] = 0.09f;
    bodies.sleepTimers[1] = 0.09f;

    IslandSleepParams params;
    params.timeRequired = 0.1f;
    const f32 dt = 1.f / 60.f;

    const u32 islandA = graph.bodyIsland(0);
    expectTrue(sleep_island_bodies_guarded(bodies, graph.island(islandA), params, dt),
               "guarded sleep accumulates timers for low-velocity island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) != 0u, "guarded sleep marks first body sleeping");
    expectTrue((bodies.flags[1] & RB_SLEEPING) != 0u, "guarded sleep marks second body sleeping");

    const IslandSleepGraphPreflight graphSleep =
        preflight_island_sleep_graph(bodies, graph, params, dt);
    expectTrue(graphSleep.skipped, "graph sleep preflight skips when no island can sleep");

    expectTrue(wake_island_bodies_guarded(bodies, graph.island(islandA)),
               "guarded wake clears sleeping flag on island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "guarded wake clears first body sleep flag");
    expectTrue(bodies.sleepTimers[0] == 0.f, "guarded wake resets sleep timer");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const u32 wokeCount = wake_all_islands_guarded(bodies, graph, params);
    expectTrue(wokeCount == 1u, "batch wake guarded wakes contact island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "batch wake clears sleeping flags");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    const IslandBuildPreflight validPreflight = preflight_island_build(4, contacts, constraints);
    expectTrue(validPreflight.can_build(), "valid body count can build island graph");
    expectTrue(validPreflight.validContactCount == 1u, "build preflight counts valid contacts");
    expectTrue(validPreflight.skippedContactCount == 1u, "build preflight counts skipped contacts");
    expectTrue(validPreflight.inRangeContactCount == 1u, "build preflight counts in-range contacts");
    expectTrue(validPreflight.inRangeDistanceCount == 1u, "build preflight counts in-range distances");
    expectTrue(validPreflight.outOfRangeDistanceCount == 1u, "build preflight counts out-of-range distances");
    expectTrue(validPreflight.unionCandidateCount == 2u, "build preflight counts union candidates");
    expectTrue(!should_skip_island_build(4, contacts, constraints), "should_skip false for valid build");

    const IslandBuildPreflight degeneratePreflight = preflight_island_build(0, contacts, constraints);
    expectTrue(!degeneratePreflight.can_build(), "zero body count with constraints cannot build");
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip true for degenerate build");
    expectTrue(!is_valid_island_build_body_count(0u), "zero body count is invalid for build");

    ContactIslandGraph graph;
    expectTrue(build_contact_island_graph_guarded(graph, 4, contacts, constraints),
               "guarded build succeeds for valid inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build produces constrained island");
    expectTrue(!build_contact_island_graph_guarded(graph, 0, contacts, constraints),
               "guarded build skips degenerate inputs");
    expectTrue(graph.islandCount() == 0u, "guarded skip clears graph");
}

void testPreflightIslandSleepGuards() {
    RigidBodySoA bodies;
    const u32 awakeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 awakeB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleepingA = bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingB = bodies.addBody({12.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 ground = bodies.addBody({0.f, -1.f, 0.f}, 0.f, RB_STATIC);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = awakeA, .bodyB = awakeB, .restLength = 2.f},
        DistanceConstraint{.bodyA = sleepingA, .bodyB = sleepingB, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

    const u32 awakeIsland = graph.bodyIsland(awakeA);
    const u32 sleepingIsland = graph.bodyIsland(sleepingA);

    expectTrue(is_awake_dynamic_body(bodies, awakeA), "awake dynamic body detected");
    expectTrue(is_sleeping_body(bodies, sleepingA), "sleeping body detected");
    expectTrue(!is_dynamic_body(bodies, ground), "static body is not dynamic");

    const IslandSleepPreflight awakePreflight = preflight_island_sleep(graph.island(awakeIsland), bodies);
    expectTrue(!awakePreflight.skipped, "awake island sleep preflight not skipped");
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(!should_skip_island_solve_sleeping(graph.island(awakeIsland), bodies),
               "should_skip false for awake island");

    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(sleepingPreflight.all_sleeping(), "all-sleeping island flagged");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_sleeping(graph.island(sleepingIsland), bodies),
               "should_skip true for all-sleeping island");

    const IslandSleepStats stats = compute_island_sleep_stats(graph, bodies);
    expectTrue(stats.solvableCount == 1u, "sleep stats count solvable island");
    expectTrue(stats.allSleepingCount == 1u, "sleep stats count all-sleeping island");
    expectTrue(has_solvable_sleep_islands(graph, bodies), "has solvable sleep islands");
    expectTrue(collect_solvable_sleep_island_indices(graph, bodies).size() == 1u,
               "collect solvable sleep indices returns awake island");
}

void testPreflightIslandWakeGuards() {
    RigidBodySoA bodies;
    const u32 awakeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleepingB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = awakeA;
    contacts.back().bodyB = sleepingB;

    ContactIslandGraph graph;
    graph.build(2, contacts, {});

    const ContactIslandGraph::Island& island = graph.island(graph.bodyIsland(awakeA));
    const IslandWakePreflight wakePreflight = preflight_island_wake(island, bodies, contacts);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip mixed island");
    expectTrue(wakePreflight.should_wake(), "mixed awake/sleeping island should wake");
    expectTrue(wakePreflight.contactWakeCount == 1u, "wake preflight counts contact wake candidates");
    expectTrue(!should_skip_island_wake(island, bodies, contacts), "should_skip false when wake needed");

    expectTrue((bodies.flags[sleepingB] & RB_SLEEPING) != 0u, "body starts sleeping");
    const u32 wokenCount = wake_island_bodies_guarded(bodies, island, contacts);
    expectTrue(wokenCount == 1u, "guarded wake clears sleeping flag");
    expectTrue((bodies.flags[sleepingB] & RB_SLEEPING) == 0u, "sleeping body woken by guarded wake");

    RigidBodySoA forcedBodies;
    const u32 forcedSleepingA = forcedBodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 forcedSleepingB = forcedBodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    forcedBodies.forces[forcedSleepingA] = {10.f, 0.f, 0.f};
    ContactIslandGraph forcedGraph;
    forcedGraph.build(2, {}, {DistanceConstraint{.bodyA = forcedSleepingA,
                                                   .bodyB = forcedSleepingB,
                                                   .restLength = 2.f}});
    const IslandWakePreflight forcePreflight = preflight_island_wake(forcedGraph.island(0),
                                                                     forcedBodies,
                                                                     contacts);
    expectTrue(forcePreflight.externalForceCount == 1u, "wake preflight counts external force");
    expectTrue(forcePreflight.should_wake(), "external force flags wake on sleeping island");
}

void testPreflightIslandDispatchWithSleepGuards() {
    RigidBodySoA bodies;
    const u32 awakeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 awakeB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = awakeA, .bodyB = awakeB, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, {}, constraints);

    const f32 dt = 1.f / 60.f;
    const IslandSleepDispatchPreflight preflight = preflight_island_dispatch_with_sleep(graph, dt, bodies);
    expectTrue(!preflight.skipped, "sleep dispatch preflight does not skip mixed graph");
    expectTrue(preflight.can_dispatch(), "sleep dispatch preflight can dispatch awake island");
    expectTrue(preflight.solvableIslandCount == 1u, "sleep dispatch preflight counts solvable island");
    expectTrue(preflight.allSleepingIslandCount == 1u, "sleep dispatch preflight counts sleeping island");
    expectTrue(!should_skip_island_dispatch_with_sleep(graph, dt, bodies),
               "should_skip false when solvable island exists");

    RigidBodySoA allSleepingBodies;
    allSleepingBodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    allSleepingBodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    ContactIslandGraph sleepingGraph;
    sleepingGraph.build(2, {}, constraints);
    const IslandSleepDispatchPreflight sleepingPreflight =
        preflight_island_dispatch_with_sleep(sleepingGraph, dt, allSleepingBodies);
    expectTrue(sleepingPreflight.skipped, "all-sleeping graph skipped by sleep dispatch preflight");
    expectTrue(!sleepingPreflight.can_dispatch(), "all-sleeping graph cannot dispatch with sleep guard");
    expectTrue(sleepingPreflight.solvableIslandCount == 0u, "all-sleeping graph has zero solvable islands");
    expectTrue(should_skip_island_dispatch_with_sleep(sleepingGraph, dt, allSleepingBodies),
               "should_skip true when every island is sleeping");
}

void testSolveIslandJobSkipsAllSleepingIsland() {
    RigidBodySoA bodies;
    const u32 sleepingA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingB = bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = sleepingA, .bodyB = sleepingB, .restLength = 2.f},
    };
    graph.build(2, {}, constraints);

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const IslandSolveJob job = extract_island(graph, 0);
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        if ((bodySoA.flags[index] & RB_SLEEPING) != 0u) {
            return 0.f;
        }
        return bodySoA.invMasses[index];
    };

    expectTrue(should_skip_solve_island_job_with_sleep(job, 1.f / 60.f, bodies),
               "job sleep preflight skips all-sleeping island");
    expectTrue(!solve_island_job(bodies, *job.island, work, constraints, 1.f / 60.f, 0.f, invMassFn),
               "solve_island_job skips all-sleeping island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 99;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 3, .bodyB = 8, .restLength = 2.f},
    };

    const IslandBuildPreflight valid = preflight_island_build(5, contacts, constraints);
    expectTrue(valid.can_build(), "valid build preflight can build");
    expectTrue(valid.bodyCount == 5u, "build preflight records body count");
    expectTrue(valid.contactCount == 2u, "build preflight records contact count");
    expectTrue(valid.validContactCount == 1u, "build preflight counts in-range valid contacts");
    expectTrue(valid.outOfRangeContactCount == 1u, "build preflight counts out-of-range contacts");
    expectTrue(valid.outOfRangeDistanceCount == 1u, "build preflight counts out-of-range distance refs");
    expectTrue(valid.has_out_of_range_refs(), "build preflight reports out-of-range refs");
    expectTrue(!should_skip_island_build(5, contacts, constraints),
               "should_skip false for valid body count");

    const IslandBuildPreflight emptyBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(emptyBodies.skipped, "build preflight skips zero bodies with constraints");
    expectTrue(!emptyBodies.can_build(), "zero-body constrained input cannot build");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip true for zero bodies with constraints");

    const IslandBuildPreflight emptyInput = preflight_island_build(0, {}, {});
    expectTrue(!emptyInput.skipped, "build preflight does not skip empty inputs");
    expectTrue(emptyInput.can_build(), "empty inputs can build empty graph");
    expectTrue(!should_skip_island_build(0, {}, {}), "should_skip false for empty inputs");

    ContactIslandGraph graph;
    graph.build(5, contacts, constraints);
    expectTrue(graph.constrainedIslandCount() >= 1u,
               "build still produces constrained islands with partial out-of-range refs");
}

void testPreflightIslandSleepForSolveGuards() {
    RigidBodySoA bodies;
    const u32 awakeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 awakeB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleepingA = bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingB = bodies.addBody({12.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = awakeA, .bodyB = awakeB, .restLength = 2.f},
        DistanceConstraint{.bodyA = sleepingA, .bodyB = sleepingB, .restLength = 2.f},
    };
    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    const u32 awakeIsland = graph.bodyIsland(awakeA);
    const u32 sleepingIsland = graph.bodyIsland(sleepingA);

    const IslandSleepPreflight awakePreflight =
        preflight_island_sleep_for_solve(graph.island(awakeIsland), bodies);
    expectTrue(!awakePreflight.skipped, "sleep preflight does not skip awake island");
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(!awakePreflight.allSleeping, "awake island is not all-sleeping");
    expectTrue(awakePreflight.awakeCount == 2u, "sleep preflight counts awake dynamic bodies");
    expectTrue(!should_skip_solve_sleeping_island(graph.island(awakeIsland), bodies),
               "should_skip false for awake island");

    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep_for_solve(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(sleepingPreflight.allSleeping, "sleeping island is all-sleeping");
    expectTrue(sleepingPreflight.sleepingCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(should_skip_solve_sleeping_island(graph.island(sleepingIsland), bodies),
               "should_skip true for all-sleeping island");
    expectTrue(island_all_bodies_sleeping(graph.island(sleepingIsland), bodies),
               "island_all_bodies_sleeping true for sleeping island");
    expectTrue(count_sleeping_bodies_in_island(graph.island(sleepingIsland), bodies) == 2u,
               "count_sleeping_bodies_in_island counts sleeping bodies");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(!graphPreflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(graphPreflight.can_dispatch(), "mixed graph has solveable islands");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "graph sleep preflight counts solveable island");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep preflight counts sleeping island");
    expectTrue(!should_skip_island_solve_for_sleep(graph, bodies),
               "should_skip graph false when solveable islands exist");

    const IslandSleepPreflight outOfRange =
        preflight_island_sleep_for_solve_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "sleep preflight by index skips out-of-range island");
}

void testPreflightIslandWakeGuards() {
    RigidBodySoA bodies;
    const u32 sleepingStill = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingMoving = bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.linearVelocities[sleepingMoving] = {0.5f, 0.f, 0.f};
    const u32 awake = bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    const u32 awakePartner = bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = sleepingStill, .bodyB = sleepingMoving, .restLength = 2.f},
        DistanceConstraint{.bodyA = awake, .bodyB = awakePartner, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    const f32 linearThreshold = 0.01f;
    const f32 angularThreshold = 0.01f;
    const u32 sleepingIsland = graph.bodyIsland(sleepingStill);
    const IslandWakePreflight wakePreflight = preflight_island_wake(
        graph.island(sleepingIsland), bodies, linearThreshold, angularThreshold);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip sleeping island");
    expectTrue(wakePreflight.needs_wake_check(), "sleeping island needs wake check");
    expectTrue(!wakePreflight.can_skip_wake_check(), "sleeping island cannot skip wake check");
    expectTrue(wakePreflight.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts wake candidates");
    expectTrue(!should_skip_island_wake_check(graph.island(sleepingIsland), bodies),
               "should_skip wake false when sleeping bodies exist");

    const u32 awakeIsland = graph.bodyIsland(awake);
    const IslandWakePreflight awakeWakePreflight = preflight_island_wake_by_index(
        graph, awakeIsland, bodies, linearThreshold, angularThreshold);
    expectTrue(awakeWakePreflight.can_skip_wake_check(),
               "awake island can skip wake check when no sleeping bodies");
    expectTrue(should_skip_island_wake_check(graph.island(awakeIsland), bodies),
               "should_skip wake true when no sleeping bodies");
    expectTrue(is_body_sleeping(bodies, sleepingStill), "is_body_sleeping detects sleeping body");
    expectTrue(is_body_awake(bodies, awake), "is_body_awake detects awake body");
}

void testPreflightIslandSolveCombinedGuards() {
    RigidBodySoA bodies;
    const u32 awakeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 awakeB = bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    const u32 sleepingA = bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingB = bodies.addBody({12.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = awakeA, .bodyB = awakeB, .restLength = 2.f},
        DistanceConstraint{.bodyA = sleepingA, .bodyB = sleepingB, .restLength = 2.f},
    };
    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds() = contacts;
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandSolveJob awakeJob = extract_island(graph, graph.bodyIsland(awakeA));
    const IslandSolveCombinedPreflight awakeCombined =
        preflight_island_solve_combined(awakeJob, bodies, contacts, constraints, dt);
    expectTrue(!awakeCombined.skipped, "combined preflight does not skip awake island");
    expectTrue(awakeCombined.can_dispatch(), "combined preflight can dispatch awake island");
    expectTrue(!should_skip_island_solve_combined(awakeJob, bodies, contacts, constraints, dt),
               "should_skip combined false for awake island");
    expectTrue(dispatch_solve_island_combined_guarded(bodies,
                                                      awakeJob,
                                                      work,
                                                      constraints,
                                                      dt,
                                                      0.f,
                                                      invMassFn),
               "combined guarded dispatch solves awake island");

    const IslandSolveJob sleepingJob = extract_island(graph, graph.bodyIsland(sleepingA));
    const IslandSolveCombinedPreflight sleepingCombined =
        preflight_island_solve_combined(sleepingJob, bodies, contacts, constraints, dt);
    expectTrue(!sleepingCombined.can_dispatch(), "combined preflight cannot dispatch sleeping island");
    expectTrue(should_skip_island_solve_combined(sleepingJob, bodies, contacts, constraints, dt),
               "should_skip combined true for sleeping island");
    expectTrue(!dispatch_solve_island_combined_guarded(bodies,
                                                       sleepingJob,
                                                       work,
                                                       constraints,
                                                       dt,
                                                       0.f,
                                                       invMassFn),
               "combined guarded dispatch skips sleeping island");

    const IslandSolveCombinedPreflight invalidDt =
        preflight_island_solve_combined(awakeJob, bodies, contacts, constraints, 0.f);
    expectTrue(!invalidDt.can_dispatch(), "combined preflight blocks invalid dt");
    expectTrue(invalidDt.job.invalidDt, "combined preflight records invalid dt");

    const IslandSolveCombinedPreflight nonFiniteDt = preflight_island_solve_combined(
        awakeJob, bodies, contacts, constraints, std::numeric_limits<f32>::infinity());
    expectTrue(!nonFiniteDt.can_dispatch(), "combined preflight blocks non-finite dt");
    expectTrue(nonFiniteDt.job.nonFiniteDt, "combined preflight records non-finite dt");
}

void testPreflightIslandDispatchNonFiniteDt() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const IslandDispatchPreflight finitePreflight = preflight_island_dispatch(graph, 1.f / 60.f);
    expectTrue(!finitePreflight.nonFiniteDt, "finite dt passes non-finite guard");
    expectTrue(finitePreflight.can_dispatch(), "finite dt can dispatch");

    const IslandDispatchPreflight infPreflight =
        preflight_island_dispatch(graph, std::numeric_limits<f32>::infinity());
    expectTrue(infPreflight.nonFiniteDt, "infinite dt fails non-finite guard");
    expectTrue(!infPreflight.can_dispatch(), "infinite dt cannot dispatch");
    expectTrue(should_skip_island_dispatch(graph, std::numeric_limits<f32>::infinity()),
               "should_skip dispatch true for infinite dt");

    const IslandSolveJobPreflight jobPreflight =
        preflight_solve_island_job(extract_island(graph, 0), std::numeric_limits<f32>::quiet_NaN());
    expectTrue(jobPreflight.invalidDt, "NaN dt fails valid-dt guard");
    expectTrue(!jobPreflight.can_dispatch(), "NaN dt cannot dispatch job");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 1;
    contacts.back().bodyB = 4;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 3, .bodyB = 4, .restLength = 2.f},
    };

    const IslandBuildPreflight contactPreflight = preflight_island_build(3, contacts, {});
    expectTrue(contactPreflight.skipped, "build preflight skips out-of-range contact bodies");
    expectTrue(contactPreflight.reason == IslandBuildRejectReason::OutOfRangeContactBody,
               "build preflight reports out-of-range contact body");
    expectTrue(contactPreflight.outOfRangeContactCount == 1u,
               "build preflight counts one out-of-range contact");
    expectTrue(should_skip_island_build(3, contacts, {}), "should_skip build on out-of-range contact");

    const IslandBuildPreflight distancePreflight = preflight_island_build(4, {}, constraints);
    expectTrue(distancePreflight.skipped, "build preflight skips out-of-range distance bodies");
    expectTrue(distancePreflight.reason == IslandBuildRejectReason::OutOfRangeDistanceBody,
               "build preflight reports out-of-range distance body");
    expectTrue(should_skip_island_build(4, {}, constraints), "should_skip build on out-of-range distance");

    const IslandBuildPreflight validPreflight = preflight_island_build(5, contacts, constraints);
    expectTrue(!validPreflight.skipped, "build preflight accepts in-range inputs");
    expectTrue(validPreflight.can_build(), "build preflight can build in-range graph");

    ContactIslandGraph graph;
    expectTrue(!build_guarded(graph, 3, contacts, {}), "build_guarded rejects out-of-range contact");
    expectTrue(graph.islandCount() == 0u, "rejected build leaves graph empty");
    expectTrue(build_guarded(graph, 5, contacts, constraints), "build_guarded builds in-range graph");
    expectTrue(graph.islandCount() > 0u, "guarded build populates islands");

    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::OutOfRangeContactBody),
                           "OutOfRangeContactBody") == 0,
               "build reject reason name resolves OutOfRangeContactBody");
    expectTrue(island_build_rejects_for_reason(3, contacts, {}, IslandBuildRejectReason::OutOfRangeContactBody),
               "build_rejects_for_reason matches out-of-range contact");
}

void testPreflightIslandBodyRefsGuards() {
    ContactIslandGraph graph;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, {}, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    const ContactIslandGraph::Island& island = graph.island(0);
    const IslandBodyRefsPreflight preflight = preflight_island_body_refs(island, bodies);
    expectTrue(!preflight.skipped, "body refs preflight does not skip constrained island");
    expectTrue(preflight.inRangeBodyCount == 2u, "body refs preflight counts in-range bodies");
    expectTrue(preflight.can_solve(), "constrained island passes body refs preflight");
    expectTrue(!should_skip_island_body_refs(island, bodies), "should_skip false for in-range bodies");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 9};
    staleIsland.distanceIndices = {0};
    const IslandBodyRefsPreflight stalePreflight = preflight_island_body_refs(staleIsland, bodies);
    expectTrue(!stalePreflight.can_solve(), "stale body refs preflight cannot solve");
    expectTrue(should_skip_island_body_refs(staleIsland, bodies), "should_skip true for stale body refs");
}

void testPreflightIslandSleepAndWakeGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    graph.build(4, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({11.f, 0.f, 0.f}, 1.f, RB_STATIC);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 activeIsland = graph.bodyIsland(2);

    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep_state(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip contact island");
    expectTrue(sleepingPreflight.allSleeping, "sleep preflight marks all-sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(is_island_all_sleeping(graph.island(sleepingIsland), bodies),
               "is_island_all_sleeping true for sleeping pair");
    expectTrue(should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip sleeping island solve");

    const IslandSleepPreflight activePreflight = preflight_island_sleep_state(graph.island(activeIsland), bodies);
    expectTrue(activePreflight.activeCount == 1u, "sleep preflight counts active dynamic body");
    expectTrue(activePreflight.can_solve(), "active island can solve");
    expectTrue(is_island_wake_candidate(graph.island(activeIsland), bodies),
               "active island is wake candidate");

    bodies.forces[0] = {5.f, 0.f, 0.f};
    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(sleepingIsland), bodies);
    expectTrue(wakePreflight.sleepingBodyCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(wakePreflight.forcedWakeCount == 1u, "wake preflight counts forced wake body");
    expectTrue(wakePreflight.can_wake(), "wake preflight can wake forced island");
    expectTrue(!should_skip_island_wake(graph.island(sleepingIsland), bodies),
               "should_skip wake false when force present");

    bodies.forces[0] = {};
    expectTrue(should_skip_island_wake(graph.island(sleepingIsland), bodies),
               "should_skip wake true when no forced wake");
}

void testPreflightIslandConstraintSolveGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    graph.build(2, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 1, 0);
    work.contactManifolds() = contacts;

    const f32 dt = 1.f / 60.f;
    const ContactIslandGraph::Island& island = graph.island(0);
    const IslandConstraintSolvePreflight preflight =
        preflight_island_constraint_solve(island, bodies, contacts, {}, dt);
    expectTrue(!preflight.skipped, "constraint solve preflight does not skip active island");
    expectTrue(!preflight.invalidDt, "constraint solve preflight accepts valid dt");
    expectTrue(preflight.can_solve(), "active island passes combined constraint solve preflight");
    expectTrue(!should_skip_island_constraint_solve(island, bodies, contacts, {}, dt),
               "should_skip false for solvable island");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    const IslandConstraintSolvePreflight sleepingSolve =
        preflight_island_constraint_solve(island, bodies, contacts, {}, dt);
    expectTrue(!sleepingSolve.can_solve(), "all-sleeping island fails combined solve preflight");
    expectTrue(should_skip_island_constraint_solve(island, bodies, contacts, {}, dt),
               "should_skip true for all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    const IslandConstraintSolvePreflight invalidDt =
        preflight_island_constraint_solve(island, bodies, contacts, {}, 0.f);
    expectTrue(invalidDt.invalidDt, "constraint solve preflight rejects zero dt");
    expectTrue(!invalidDt.can_solve(), "invalid dt cannot solve");

    expectTrue(solve_island_job_guarded(bodies,
                                        island,
                                        work,
                                        {},
                                        dt,
                                        0.f,
                                        [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job_guarded solves active island");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(!solve_island_job_guarded(bodies,
                                         island,
                                         work,
                                         {},
                                         dt,
                                         0.f,
                                         [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job_guarded skips all-sleeping island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 99;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 3, .bodyB = 4, .restLength = 2.f},
    };

    expectTrue(!is_valid_island_build_body_count(0u), "zero body count is invalid for island build");
    expectTrue(is_valid_island_build_body_index(1u, 4u), "in-range body index is valid");
    expectTrue(!is_valid_island_build_body_index(4u, 4u), "at-limit body index is invalid");

    const IslandBuildPreflight validPreflight = preflight_island_build(4u, contacts, constraints);
    expectTrue(!validPreflight.skipped, "valid body count passes island build preflight");
    expectTrue(validPreflight.can_build(), "island build preflight can build with valid inputs");
    expectTrue(validPreflight.validContactCount == 1u, "island build preflight counts valid contacts");
    expectTrue(validPreflight.invalidContactCount == 1u, "island build preflight counts invalid contacts");
    expectTrue(validPreflight.validDistanceCount == 1u, "island build preflight counts valid distance constraints");
    expectTrue(validPreflight.invalidDistanceCount == 1u,
               "island build preflight counts invalid distance constraints");
    expectTrue(!should_skip_island_build(4u, contacts, constraints),
               "should_skip false for valid island build inputs");

    const IslandBuildPreflight zeroBodies = preflight_island_build(0u, contacts, constraints);
    expectTrue(zeroBodies.skipped, "zero body count skips island build preflight");
    expectTrue(!zeroBodies.can_build(), "zero body count cannot build island graph");
    expectTrue(should_skip_island_build(0u, contacts, constraints),
               "should_skip true for zero body count");
}

void testPreflightIslandSleepGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    expectTrue(is_rigid_body_sleeping(RB_SLEEPING), "sleeping flag helper detects RB_SLEEPING");
    expectTrue(!is_rigid_body_dynamic_awake(RB_SLEEPING), "sleeping body is not dynamic awake");
    expectTrue(is_rigid_body_dynamic_awake(0u), "default dynamic body is awake");

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 awakeIsland = graph.bodyIsland(2);
    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.allSleeping, "all-dynamic-sleeping island flagged");
    expectTrue(sleepingPreflight.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_sleeping_island(graph.island(sleepingIsland), bodies),
               "should_skip true for all-sleeping island");
    expectTrue(is_island_all_sleeping(graph.island(sleepingIsland), bodies),
               "is_island_all_sleeping true for sleeping island");

    const IslandSleepPreflight awakePreflight = preflight_island_sleep(graph.island(awakeIsland), bodies);
    expectTrue(!awakePreflight.allSleeping, "awake island is not all-sleeping");
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(!should_skip_sleeping_island(graph.island(awakeIsland), bodies),
               "should_skip false for awake island");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(graphPreflight.has_awake_islands(), "mixed graph has awake islands");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep stats count all-sleeping island");
    expectTrue(graphPreflight.stats.partiallyAwakeCount == 1u, "graph sleep stats count awake island");
    expectTrue(!should_skip_island_solve_all_sleeping(graph, bodies),
               "should_skip false when awake islands exist");

    const std::vector<u32> awakeIndices = collect_awake_island_indices(graph, bodies);
    expectTrue(awakeIndices.size() == 1u, "collect_awake_island_indices returns one awake island");
    expectTrue(awakeIndices[0] == awakeIsland, "awake index matches awake island");
}

void testPreflightIslandWakeGuards() {
    RigidBodySoA bodies;
    const u32 sleepingBody = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 awakeBody = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    const u32 staticBody = bodies.addBody({0.f, -5.f, 0.f}, 0.f, RB_STATIC);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = sleepingBody;
    contacts.back().bodyB = awakeBody;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = sleepingBody;
    contacts.back().bodyB = staticBody;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const u32 islandIndex = graph.bodyIsland(sleepingBody);
    const IslandWakePreflight wakePreflight =
        preflight_island_wake(graph.island(islandIndex), bodies, contacts);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip contact island");
    expectTrue(wakePreflight.needsWake, "sleeping body adjacent to awake neighbor needs wake");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts one wake candidate");
    expectTrue(island_has_wake_candidates(graph.island(islandIndex), bodies, contacts),
               "island_has_wake_candidates true for mixed sleep/awake contact");

    const std::vector<u32> wakeBodies =
        collect_island_wake_body_indices(graph.island(islandIndex), bodies, contacts);
    expectTrue(wakeBodies.size() == 1u, "collect wake bodies returns one candidate");
    expectTrue(wakeBodies[0] == sleepingBody, "wake candidate is the sleeping body");

    const IslandWakeGraphPreflight graphWake = preflight_island_wake_graph(graph, bodies, contacts);
    expectTrue(graphWake.has_wake_candidates(), "graph wake preflight has wake candidates");
    expectTrue(graphWake.stats.wakeCandidateCount == 1u, "graph wake stats count wake island");
    expectTrue(!should_skip_island_wake_graph(graph, bodies, contacts),
               "should_skip false when wake candidates exist");

    ContactIslandGraph allAwakeGraph;
    RigidBodySoA awakeBodies;
    awakeBodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    awakeBodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    allAwakeGraph.build(2, contacts, {});
    expectTrue(should_skip_island_wake_graph(allAwakeGraph, awakeBodies, contacts),
               "should_skip true when no sleeping bodies need wake");
}

void testDispatchAllAwakeIslandsSkipsSleeping() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const IslandBatchDispatchResult batch = dispatch_all_awake_islands_result(bodies,
                                                                              graph,
                                                                              work,
                                                                              constraints,
                                                                              1.f / 60.f,
                                                                              0.f,
                                                                              invMassFn);
    expectTrue(batch.solvedCount == 1u, "sleep-aware batch solves only awake island");
    expectTrue(batch.skippedCount == 0u, "sleep-aware batch does not count awake island as skipped");
    expectTrue(batch.any_solved(), "sleep-aware batch reports solved island");

    const u32 sleepingIndex = graph.bodyIsland(0);
    const IslandDispatchResult sleepingResult = dispatch_solve_island_sleep_guarded_result(
        bodies,
        graph,
        sleepingIndex,
        work,
        constraints,
        1.f / 60.f,
        0.f,
        invMassFn);
    expectTrue(sleepingResult.skipped, "sleep-guarded dispatch skips all-sleeping island");
    expectTrue(!sleepingResult.solved, "sleep-guarded dispatch does not solve sleeping island");

    const u32 awakeIndex = graph.bodyIsland(2);
    const IslandDispatchResult awakeResult = dispatch_solve_island_sleep_guarded_result(
        bodies,
        graph,
        awakeIndex,
        work,
        constraints,
        1.f / 60.f,
        0.f,
        invMassFn);
    expectTrue(awakeResult.solved, "sleep-guarded dispatch solves awake island");
    expectTrue(!awakeResult.skipped, "sleep-guarded dispatch does not skip awake island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 4;
    contacts.back().bodyB = 5;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 9, .bodyB = 10, .restLength = 1.f},
    };

    const IslandBuildPreflight zeroBodies = preflight_island_build(0u, contacts, constraints);
    expectTrue(zeroBodies.zeroBodies, "build preflight flags zero bodies");
    expectTrue(!zeroBodies.can_build(), "build preflight rejects zero bodies");
    expectTrue(should_skip_island_build(0u, contacts, constraints), "should_skip build for zero bodies");
    expectTrue(island_build_rejects_for_reason(0u, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
               "reject reason matches zero bodies");

    const IslandBuildPreflight valid = preflight_island_build(4u, contacts, constraints);
    expectTrue(valid.can_build(), "build preflight accepts populated body count");
    expectTrue(valid.validContactCount == 2u, "build preflight counts valid contacts");
    expectTrue(valid.inRangeContactCount == 1u, "build preflight counts in-range valid contacts");
    expectTrue(valid.skippedContactCount == 2u, "build preflight counts skipped contacts");
    expectTrue(valid.inRangeDistanceCount == 1u, "build preflight counts in-range distance constraints");
    expectTrue(valid.skippedDistanceCount == 1u, "build preflight counts skipped distance constraints");
    expectTrue(valid.connectableConstraintCount == 2u, "build preflight counts connectable constraints");
    expectTrue(!should_skip_island_build(4u, contacts, constraints), "should_skip false for valid build");
}

void testIslandBuildGuardedSkipsZeroBodies() {
    ContactIslandGraph graph;
    expectTrue(!graph.build_guarded(0u, {}, {}), "build_guarded returns false for zero bodies");
    expectTrue(graph.islandCount() == 0u, "build_guarded clears graph on skip");

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    expectTrue(graph.build_guarded(2u, contacts, constraints), "build_guarded succeeds for valid input");
    expectTrue(graph.constrainedIslandCount() == 1u, "build_guarded populates constrained island");

    ContactIslandGraph unguarded;
    unguarded.build(2u, contacts, constraints);
    expectTrue(unguarded.islandCount() == graph.islandCount(),
               "unguarded build matches guarded island count on valid path");
}

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({6.f, 0.f, 0.f}, 1.f, RB_STATIC);

    ContactIslandGraph graph;
    graph.build(4, {}, {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    });

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 mixedIsland = graph.bodyIsland(2);

    const IslandSleepWakePreflight sleepingPreflight =
        preflight_island_sleep_wake(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.all_sleeping(), "both-sleeping island flagged all sleeping");
    expectTrue(sleepingPreflight.should_remain_asleep(), "sleeping island should remain asleep");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_solve_sleeping_island(graph.island(sleepingIsland), bodies),
               "should_skip sleeping island");

    const IslandSleepWakePreflight mixedPreflight =
        preflight_island_sleep_wake(graph.island(mixedIsland), bodies);
    expectTrue(mixedPreflight.has_awake_dynamic(), "mixed island has awake dynamic body");
    expectTrue(mixedPreflight.should_wake(), "mixed island should wake");
    expectTrue(mixedPreflight.can_solve(), "mixed island can solve");
    expectTrue(!should_skip_solve_sleeping_island(graph.island(mixedIsland), bodies),
               "should_skip false for awake dynamic island");

    expectTrue(island_body_is_sleeping(bodies, 0), "body helper detects sleeping");
    expectTrue(island_body_is_static_or_kinematic(bodies, 3), "body helper detects static");
    expectTrue(island_body_is_awake_dynamic(bodies, 2), "body helper detects awake dynamic");
}

void testPreflightIslandConstraintSolveGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(2, contacts, constraints);
    const ContactIslandGraph::Island& island = graph.island(0);

    const IslandConstraintSolvePreflight preflight =
        preflight_island_constraint_solve(island, bodies, contacts, constraints);
    expectTrue(!preflight.skipped, "constraint solve preflight does not skip constrained island");
    expectTrue(preflight.refs.can_solve(), "constraint solve preflight has in-range refs");
    expectTrue(!preflight.sleepWake.can_solve(), "constraint solve preflight rejects all-sleeping island");
    expectTrue(!preflight.can_solve(), "combined constraint solve preflight cannot solve sleeping island");
    expectTrue(should_skip_island_constraint_solve(island, bodies, contacts, constraints),
               "should_skip combined solve for all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandConstraintSolvePreflight awakePreflight =
        preflight_island_constraint_solve(island, bodies, contacts, constraints);
    expectTrue(awakePreflight.can_solve(), "constraint solve preflight allows partially awake island");
    expectTrue(!should_skip_island_constraint_solve(island, bodies, contacts, constraints),
               "should_skip false when island has awake dynamic body");
}

void testSolveIslandJobSkipsAllSleepingIsland() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    ContactIslandGraph graph;
    graph.build(2, {}, {DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f}});

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const IslandSolveJob job = extract_island(graph, 0);
    expectTrue(should_solve_island(job), "sleeping island job still dispatchable by constraint count");
    expectTrue(!solve_island_job(bodies,
                                 *job.island,
                                 work,
                                 {DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f}},
                                 1.f / 60.f,
                                 0.f,
                                 [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job skips all-sleeping island");
}

void testPreflightIslandSleepWakeGraphGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({6.f, 0.f, 0.f}, 1.f, RB_STATIC);

    ContactIslandGraph graph;
    graph.build(4, {}, {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    });

    const IslandSleepWakeGraphPreflight preflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!preflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(preflight.can_solve(), "graph sleep preflight can solve");
    expectTrue(preflight.stats.solvableCount == 1u, "graph sleep preflight counts solvable island");
    expectTrue(preflight.stats.allSleepingCount == 1u, "graph sleep preflight counts all-sleeping island");
    expectTrue(count_solvable_islands(graph, bodies) == 1u, "count_solvable_islands matches stats");
    expectTrue(has_solvable_islands(graph, bodies), "has_solvable_islands true for mixed graph");
    expectTrue(!should_skip_island_solve_sleep_wake(graph, bodies),
               "should_skip graph false when solvable island exists");

    for (u32 i = 0; i < bodies.count(); ++i) {
        if ((bodies.flags[i] & RB_STATIC) == 0u) {
            bodies.flags[i] |= RB_SLEEPING;
        }
    }
    const IslandSleepWakeGraphPreflight allSleeping =
        preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(allSleeping.skipped, "graph sleep preflight skips all-sleeping graph");
    expectTrue(should_skip_island_solve_sleep_wake(graph, bodies),
               "should_skip graph true when no solvable islands");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.zeroBodyCount, "build preflight flags zero body count");
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodyCount,
               "build preflight reason is ZeroBodyCount");
    expectTrue(!zeroBodies.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip_island_build on zero body count");

    ContactIslandGraph guardedGraph;
    expectTrue(!build_island_graph_guarded(guardedGraph, 0, contacts, constraints),
               "guarded build skips zero body count without mutating");
    expectTrue(guardedGraph.islandCount() == 0u, "guarded build leaves graph empty on skip");

    const IslandBuildPreflight clean = preflight_island_build(3, contacts, constraints);
    expectTrue(clean.inputs_clean(), "clean build inputs are marked clean");
    expectTrue(clean.can_build(), "clean build inputs can build");
    expectTrue(!should_skip_island_build(3, contacts, constraints),
               "should_skip false for clean build inputs");

    contacts.back().bodyB = 9u;
    const IslandBuildPreflight staleContact = preflight_island_build(3, contacts, constraints);
    expectTrue(staleContact.staleContactRefCount == 1u, "build preflight counts stale contact refs");
    expectTrue(staleContact.reason == IslandBuildRejectReason::StaleContactBodyRefs,
               "build preflight reason is StaleContactBodyRefs");
    expectTrue(staleContact.can_build(), "stale contact refs still allow unguarded build");

    ContactIslandGraph staleGraph;
    staleGraph.build(3, contacts, constraints);
    expectTrue(staleGraph.islandCount() > 0u, "unguarded build still proceeds with stale contact refs");

    std::vector<DistanceConstraint> staleDistance = {
        DistanceConstraint{.bodyA = 0, .bodyB = 8, .restLength = 2.f},
    };
    const IslandBuildPreflight staleDistancePreflight = preflight_island_build(3, {}, staleDistance);
    expectTrue(staleDistancePreflight.staleDistanceRefCount == 1u,
               "build preflight counts stale distance refs");
    expectTrue(staleDistancePreflight.reason == IslandBuildRejectReason::StaleDistanceBodyRefs,
               "build preflight reason is StaleDistanceBodyRefs");

    expectTrue(std::strcmp(islandBuildRejectReasonName(IslandBuildRejectReason::ZeroBodyCount),
                           "ZeroBodyCount") == 0,
               "build reject reason name resolves ZeroBodyCount");
}

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 awakeIsland = graph.bodyIsland(2);

    const IslandSleepWakePreflight sleepingPreflight =
        preflight_island_sleep_wake(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.all_dynamic_sleeping(),
               "sleep preflight detects all-dynamic-sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_for_sleep(graph.island(sleepingIsland), bodies),
               "should_skip sleep true for all-sleeping island");

    const IslandSleepWakePreflight awakePreflight =
        preflight_island_sleep_wake(graph.island(awakeIsland), bodies);
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(!should_skip_island_solve_for_sleep(graph.island(awakeIsland), bodies),
               "should_skip sleep false for awake island");

    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(sleepingIsland), bodies);
    expectTrue(wakePreflight.needs_wake(), "wake preflight flags sleeping constrained island");
    expectTrue(should_wake_island(graph.island(sleepingIsland), bodies),
               "should_wake true for sleeping constrained island");
    expectTrue(!should_wake_island(graph.island(awakeIsland), bodies),
               "should_wake false for awake island");

    const IslandSleepWakeGraphPreflight graphPreflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!graphPreflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(graphPreflight.stats.solvableCount == 1u, "graph sleep preflight counts solvable island");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u,
               "graph sleep preflight counts all-sleeping island");
    expectTrue(!should_skip_island_sleep_wake_graph(graph, bodies),
               "should_skip graph false when solvable islands exist");

    const std::vector<u32> solvable = collect_solvable_island_indices(graph, bodies);
    expectTrue(solvable.size() == 1u, "collect solvable indices returns awake island only");
    expectTrue(solvable.front() == awakeIsland, "solvable index matches awake island");

    expectTrue(should_skip_island_solve_for_sleep_index(graph, graph.islandCount() + 1u, bodies),
               "sleep skip by index guards out-of-range");
}

void testPreflightIslandConstraintSolveGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);
    const f32 dt = 1.f / 60.f;

    const IslandSolveJob sleepingJob = extract_island(graph, graph.bodyIsland(0));
    const IslandConstraintSolvePreflight sleepingPreflight =
        preflight_island_constraint_solve(sleepingJob, bodies, contacts, constraints, dt);
    expectTrue(!sleepingPreflight.can_solve(), "combined preflight rejects all-sleeping island");
    expectTrue(should_skip_island_constraint_solve(sleepingJob, bodies, contacts, constraints, dt),
               "should_skip combined true for all-sleeping island");

    const IslandSolveJob awakeJob = extract_island(graph, graph.bodyIsland(2));
    const IslandConstraintSolvePreflight awakePreflight =
        preflight_island_constraint_solve(awakeJob, bodies, contacts, constraints, dt);
    expectTrue(awakePreflight.can_solve(), "combined preflight accepts awake island");
    expectTrue(awakePreflight.job.can_dispatch(), "combined preflight job dispatchable");
    expectTrue(awakePreflight.refs.can_solve(), "combined preflight refs solvable");
    expectTrue(awakePreflight.bodies.can_solve(), "combined preflight bodies solvable");
    expectTrue(awakePreflight.sleepWake.can_solve(), "combined preflight sleep/wake solvable");
    expectTrue(!should_skip_island_constraint_solve(awakeJob, bodies, contacts, constraints, dt),
               "should_skip combined false for awake island");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 1};
    staleIsland.contactIndices = {9u};
    const IslandSolveBodyRefsPreflight bodyRefs = preflight_island_solve_bodies(staleIsland, bodies);
    expectTrue(bodyRefs.inRangeBodyCount == 2u, "body refs preflight counts in-range bodies");
    expectTrue(!should_skip_island_solve_bodies(staleIsland, bodies),
               "should_skip bodies false when in-range bodies exist");
}

void testPreflightIslandGraphBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandBuildPreflight validPreflight = preflight_island_graph_build(2, contacts, constraints);
    expectTrue(validPreflight.can_build(), "valid build preflight can build");
    expectTrue(validPreflight.validContactCount == 1u, "valid build preflight counts contacts");
    expectTrue(validPreflight.validDistanceCount == 1u, "valid build preflight counts distance constraints");
    expectTrue(!should_skip_island_graph_build(2, contacts, constraints),
               "should_skip false for valid build inputs");
    expectTrue(is_valid_island_build_body_index(0u, 2u), "body index 0 is valid for count 2");
    expectTrue(!is_valid_island_build_body_index(2u, 2u), "at-limit body index is invalid");

    const IslandBuildPreflight zeroBodies = preflight_island_graph_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "zero body count is skipped");
    expectTrue(!zeroBodies.can_build(), "zero body count cannot build");
    expectTrue(should_skip_island_graph_build(0, contacts, constraints),
               "should_skip true for zero body count");

    std::vector<narrowphase::ContactManifold> staleContacts = contacts;
    staleContacts.back().bodyB = 5u;
    const IslandBuildPreflight staleContactPreflight = preflight_island_graph_build(2, staleContacts, {});
    expectTrue(staleContactPreflight.skipped, "out-of-range contact body is skipped");
    expectTrue(staleContactPreflight.skippedContactCount == 1u,
               "stale contact preflight counts skipped contacts");

    std::vector<DistanceConstraint> staleConstraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 9u, .restLength = 2.f},
    };
    const IslandBuildPreflight staleDistancePreflight = preflight_island_graph_build(2, {}, staleConstraints);
    expectTrue(staleDistancePreflight.skipped, "out-of-range distance body is skipped");
    expectTrue(staleDistancePreflight.skippedDistanceCount == 1u,
               "stale distance preflight counts skipped constraints");
}

void testBuildIslandGraphGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    graph.build(2, contacts, constraints);
    expectTrue(graph.islandCount() >= 1u, "baseline build succeeds for valid inputs");

    ContactIslandGraph guardedGraph;
    expectTrue(build_island_graph_guarded(guardedGraph, 2, contacts, constraints),
               "guarded build succeeds for valid inputs");
    expectTrue(guardedGraph.islandCount() == graph.islandCount(),
               "guarded build matches direct build island count");

    ContactIslandGraph rejectedGraph;
    contacts.back().bodyB = 8u;
    expectTrue(!build_island_graph_guarded(rejectedGraph, 2, contacts, {}),
               "guarded build rejects out-of-range contact bodies");
    expectTrue(rejectedGraph.islandCount() == 0u,
               "guarded reject leaves graph empty");
}

void testPreflightIslandSleepGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const u32 islandIndex = graph.bodyIsland(0);
    const IslandSleepPreflight allSleeping =
        preflight_island_sleep(graph.island(islandIndex), bodies);
    expectTrue(!allSleeping.skipped, "sleep preflight does not skip constrained island");
    expectTrue(allSleeping.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(allSleeping.sleepingBodyCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(allSleeping.allSleeping, "all dynamic bodies sleeping");
    expectTrue(allSleeping.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_sleeping_island_solve(graph.island(islandIndex), bodies),
               "should_skip_sleeping true for all-sleeping island");
    expectTrue(should_skip_sleeping_island_solve_index(graph, islandIndex, bodies),
               "index sleep skip guard matches island guard");
    expectTrue(should_skip_sleeping_island_solve_index(graph, graph.islandCount() + 1u, bodies),
               "index sleep skip guard rejects out-of-range index");

    bodies.flags[1] &= ~RB_SLEEPING;
    const IslandSleepPreflight mixed =
        preflight_island_sleep(graph.island(islandIndex), bodies);
    expectTrue(!mixed.allSleeping, "mixed island is not all-sleeping");
    expectTrue(mixed.awakeDynamicCount == 1u, "mixed island has one awake dynamic body");
    expectTrue(!mixed.can_skip_solve(), "mixed island cannot skip solve");
    expectTrue(!should_skip_sleeping_island_solve(graph.island(islandIndex), bodies),
               "should_skip_sleeping false when awake body exists");
}

void testPreflightIslandSleepGraphGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(!preflight.skipped, "sleep graph preflight does not skip mixed graph");
    expectTrue(preflight.awakeCount == 1u, "sleep graph preflight counts awake constrained island");
    expectTrue(preflight.allSleepingCount == 1u, "sleep graph preflight counts all-sleeping island");
    expectTrue(has_awake_islands(graph, bodies), "has_awake_islands true for mixed graph");
    expectTrue(count_awake_islands(graph, bodies) == 1u, "count_awake_islands matches stats");

    const std::vector<u32> awakeIndices = collect_awake_island_indices(graph, bodies);
    expectTrue(awakeIndices.size() == 1u, "collect_awake_island_indices returns one island");
    expectTrue(!should_skip_sleeping_island_solve(graph.island(awakeIndices[0]), bodies),
               "collected awake island is not all-sleeping");
}

void testPreflightIslandWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const u32 islandIndex = graph.bodyIsland(0);
    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(islandIndex), bodies);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakePreflight.sleepingDynamicCount == 1u, "wake preflight counts sleeping dynamic");
    expectTrue(wakePreflight.awakeDynamicCount == 1u, "wake preflight counts awake dynamic");
    expectTrue(wakePreflight.should_wake(), "mixed island should wake sleeping bodies");
    expectTrue(!should_skip_island_wake(graph.island(islandIndex), bodies),
               "should_skip_island_wake false when wake is needed");

    expectTrue(wake_island_bodies_guarded(bodies, graph.island(islandIndex)),
               "guarded wake succeeds for mixed island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "guarded wake clears sleeping flag");
    expectTrue(bodies.sleepTimers[0] == 0.f, "guarded wake resets sleep timer");
    expectTrue(should_skip_island_wake(graph.island(islandIndex), bodies),
               "should_skip_island_wake true after all dynamics are awake");

    bodies.flags[0] |= RB_SLEEPING;
    expectTrue(wake_island_bodies_by_index_guarded(bodies, graph, islandIndex),
               "index guarded wake succeeds");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "index guarded wake clears sleeping flag");
    expectTrue(!wake_island_bodies_by_index_guarded(bodies, graph, graph.islandCount() + 3u),
               "index guarded wake rejects out-of-range index");
}

void testPreflightIslandSolveBodiesGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    bodies.addBody({2.f, 0.f, 0.f}, 0.f, RB_STATIC);

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const u32 islandIndex = graph.bodyIsland(0);
    const IslandSolveBodiesPreflight staticOnly =
        preflight_island_solve_bodies(graph.island(islandIndex), bodies);
    expectTrue(!staticOnly.skipped, "solve bodies preflight does not skip constrained island");
    expectTrue(staticOnly.immobileCount == 2u, "static island counts immobile bodies");
    expectTrue(staticOnly.awakeDynamicCount == 0u, "static island has no awake dynamics");
    expectTrue(!staticOnly.can_solve(), "static-only island cannot solve");
    expectTrue(should_skip_island_solve_bodies(graph.island(islandIndex), bodies),
               "should_skip_island_solve_bodies for static-only island");

    const IslandSolveBodiesPreflight outOfRange =
        preflight_island_solve_bodies_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "solve bodies index preflight skips out-of-range island");
}

void testDispatchSolveIslandSkipSleeping() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    expectTrue(!dispatch_solve_island_skip_sleeping(bodies,
                                                    graph,
                                                    0u,
                                                    work,
                                                    constraints,
                                                    1.f / 60.f,
                                                    0.f,
                                                    invMassFn),
               "skip-sleeping dispatch does not solve all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    expectTrue(dispatch_solve_island_skip_sleeping(bodies,
                                                   graph,
                                                   0u,
                                                   work,
                                                   constraints,
                                                   1.f / 60.f,
                                                   0.f,
                                                   invMassFn),
              "skip-sleeping dispatch solves awake island");
}

void testPreflightIslandSolveCombinedGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(2, contacts, constraints);
    const u32 islandIndex = graph.bodyIsland(0);

    const IslandSolvePreflightCombined sleepingCombined = preflight_island_solve_combined(
        graph.island(islandIndex), bodies, contacts, constraints);
    expectTrue(!sleepingCombined.skipped, "combined preflight does not skip constrained island");
    expectTrue(sleepingCombined.refs.can_solve(), "combined preflight sees valid constraint refs");
    expectTrue(sleepingCombined.sleep.can_skip_solve(), "combined preflight sees all-sleeping island");
    expectTrue(!sleepingCombined.can_solve(), "combined preflight cannot solve all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    const IslandSolvePreflightCombined awakeCombined = preflight_island_solve_combined(
        graph.island(islandIndex), bodies, contacts, constraints);
    expectTrue(awakeCombined.can_solve(), "combined preflight can solve awake island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 5;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 2, .restLength = 2.f},
        DistanceConstraint{.bodyA = 0, .bodyB = 9, .restLength = 2.f},
    };

    expectTrue(is_valid_island_build_body_count(0u), "zero body count is valid for build preflight");
    expectTrue(is_valid_island_build_body_count(4u), "positive body count is valid for build preflight");

    const IslandBuildPreflight preflight = preflight_island_build(4u, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip constrained scene");
    expectTrue(preflight.can_build(), "build preflight can build constrained scene");
    expectTrue(preflight.validContactCount == 1u, "build preflight counts in-range valid contacts");
    expectTrue(preflight.invalidContactCount == 2u, "build preflight counts out-of-range/invalid contacts");
    expectTrue(preflight.validDistanceCount == 1u, "build preflight counts in-range distance constraints");
    expectTrue(preflight.invalidDistanceCount == 1u, "build preflight counts out-of-range distance constraints");
    expectTrue(preflight.has_valid_constraints(), "build preflight reports valid constraints");

    const IslandBuildPreflight emptyScene = preflight_island_build(0u, {}, {});
    expectTrue(emptyScene.skipped, "build preflight skips empty scene with no constraints");
    expectTrue(should_skip_island_build(0u, {}, {}), "should_skip_island_build on empty scene");

    ContactIslandGraph graph;
    expectTrue(!graph.build_guarded(0u, {}, {}), "build_guarded returns false when preflight skips");
    expectTrue(graph.islandCount() == 0u, "skipped build_guarded leaves empty graph");

    expectTrue(graph.build_guarded(4u, contacts, constraints), "build_guarded succeeds for valid scene");
    expectTrue(graph.islandCount() > 0u, "build_guarded populates islands");
    expectTrue(graph.constrainedIslandCount() >= 1u, "build_guarded retains constrained islands");
}

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({7.f, 0.f, 0.f}, 0.f, RB_STATIC);

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, {}, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 wakeableIsland = graph.bodyIsland(2);

    expectTrue(is_sleeping_body(RB_SLEEPING), "is_sleeping_body detects sleeping flag");
    expectTrue(!is_sleeping_body(0u), "is_sleeping_body false for dynamic body");
    expectTrue(is_static_or_kinematic_body(RB_STATIC), "is_static_or_kinematic_body detects static flag");
    expectTrue(is_solver_inactive_body(RB_SLEEPING), "is_solver_inactive_body includes sleeping");
    expectTrue(is_solver_inactive_body(RB_STATIC), "is_solver_inactive_body includes static");

    const ContactIslandGraph::Island& sleeping = graph.island(sleepingIsland);
    const IslandSleepWakePreflight sleepingPreflight = preflight_island_sleep_wake(sleeping, bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep/wake preflight does not skip constrained island");
    expectTrue(sleepingPreflight.sleepingCount == 2u, "sleep/wake preflight counts sleeping bodies");
    expectTrue(sleepingPreflight.wakeableCount == 0u, "sleep/wake preflight finds no wakeable bodies");
    expectTrue(sleepingPreflight.is_all_sleeping(), "sleeping pair island is all sleeping");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(is_island_all_sleeping(sleeping, bodies), "is_island_all_sleeping true for sleeping island");
    expectTrue(should_skip_sleeping_island_solve(sleeping, bodies),
               "should_skip_sleeping_island_solve on all-sleeping island");

    const ContactIslandGraph::Island& wakeable = graph.island(wakeableIsland);
    const IslandSleepWakePreflight wakeablePreflight = preflight_island_sleep_wake(wakeable, bodies);
    expectTrue(wakeablePreflight.wakeableCount == 1u, "mixed island has one wakeable body");
    expectTrue(wakeablePreflight.staticOrKinematicCount == 1u, "mixed island has one static body");
    expectTrue(wakeablePreflight.can_solve(), "wakeable island can solve");
    expectTrue(is_island_wakeable(wakeable, bodies), "is_island_wakeable true for mixed island");
    expectTrue(!should_skip_sleeping_island_solve(wakeable, bodies),
               "should_skip false for wakeable island");

    const IslandSleepWakePreflight outOfRange =
        preflight_island_sleep_wake_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "index sleep/wake preflight skips out-of-range island");
    expectTrue(should_skip_sleeping_island_solve_index(graph, graph.islandCount() + 1u, bodies),
               "should_skip_sleeping_island_solve_index on out-of-range index");

    const IslandSleepWakeGraphPreflight graphPreflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!graphPreflight.skipped, "graph sleep/wake preflight does not skip mixed graph");
    expectTrue(graphPreflight.can_dispatch(), "graph sleep/wake preflight can dispatch");
    expectTrue(graphPreflight.stats.wakeableCount == 1u, "graph sleep/wake preflight counts wakeable islands");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep/wake preflight counts all-sleeping islands");
    expectTrue(has_wakeable_islands(graph, bodies), "has_wakeable_islands true for mixed graph");
    expectTrue(!should_skip_sleeping_island_graph(graph, bodies),
               "should_skip_sleeping_island_graph false when wakeable islands exist");

    const std::vector<u32> wakeableIndices = collect_wakeable_island_indices(graph, bodies);
    expectTrue(wakeableIndices.size() == 1u, "collect_wakeable_island_indices returns one island");
}

void testPreflightIslandConstraintSolveGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({7.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 contactIsland = graph.bodyIsland(2);

    const IslandConstraintSolvePreflight sleepingSolve =
        preflight_island_constraint_solve(graph.island(sleepingIsland), bodies, contacts, constraints);
    expectTrue(!sleepingSolve.skipped, "constraint solve preflight does not skip constrained island");
    expectTrue(sleepingSolve.refs.can_solve(), "sleeping island has in-range constraint refs");
    expectTrue(!sleepingSolve.sleepWake.can_solve(), "sleeping island fails sleep/wake preflight");
    expectTrue(!sleepingSolve.can_solve(), "combined preflight rejects all-sleeping island");
    expectTrue(should_skip_island_constraint_solve(graph.island(sleepingIsland), bodies, contacts, constraints),
               "should_skip_island_constraint_solve on all-sleeping island");

    const IslandConstraintSolvePreflight contactSolve =
        preflight_island_constraint_solve(graph.island(contactIsland), bodies, contacts, constraints);
    expectTrue(contactSolve.can_solve(), "wakeable contact island passes combined preflight");
    expectTrue(!should_skip_island_constraint_solve(graph.island(contactIsland), bodies, contacts, constraints),
               "should_skip false for wakeable contact island");

    SolverWorkBuffers work;
    work.init(4, 2, 1);
    work.contactManifolds() = contacts;

    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        if ((bodySoA.flags[index] & RB_STATIC) != 0u || (bodySoA.flags[index] & RB_SLEEPING) != 0u) {
            return 0.f;
        }
        return bodySoA.invMasses[index];
    };

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "solve_island_job_guarded skips all-sleeping island");
    expectTrue(solve_island_job_guarded(bodies,
                                          graph.island(contactIsland),
                                          work,
                                          constraints,
                                          dt,
                                          0.f,
                                          invMassFn),
               "solve_island_job_guarded solves wakeable contact island");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 4;
    contacts.back().bodyB = 5;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 9, .restLength = 2.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(3, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip non-zero body count");
    expectTrue(preflight.can_build(), "build preflight can build with valid body count");
    expectTrue(preflight.bodyCount == 3u, "build preflight records body count");
    expectTrue(preflight.inRangeContactCount == 1u, "build preflight counts in-range contacts");
    expectTrue(preflight.outOfRangeContactCount == 1u, "build preflight counts out-of-range contacts");
    expectTrue(preflight.validContactCount == 1u, "build preflight counts valid contacts");
    expectTrue(preflight.inRangeDistanceCount == 1u, "build preflight counts in-range distance constraints");
    expectTrue(preflight.outOfRangeDistanceCount == 1u, "build preflight counts out-of-range distance constraints");
    expectTrue(contact_references_in_range_body(0, 1, 3), "in-range contact refs pass");
    expectTrue(!contact_references_in_range_body(4, 5, 3), "out-of-range contact refs fail");
    expectTrue(distance_constraint_references_in_range_body(constraints[0], 3),
               "in-range distance constraint refs pass");
    expectTrue(!distance_constraint_references_in_range_body(constraints[1], 3),
               "out-of-range distance constraint refs fail");

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroBodies.skipped, "build preflight skips zero body count");
    expectTrue(!zeroBodies.can_build(), "zero body count cannot build");
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip build on zero bodies");

    ContactIslandGraph graph;
    graph.build(3, contacts, constraints);
    const u32 constrainedCount = graph.constrainedIslandCount();

    ContactIslandGraph guardedGraph;
    build_island_graph_guarded(guardedGraph, 3, contacts, constraints);
    expectTrue(guardedGraph.islandCount() == graph.islandCount(),
               "guarded build matches direct build island count");
    expectTrue(guardedGraph.constrainedIslandCount() == constrainedCount,
               "guarded build matches direct build constrained count");

    ContactIslandGraph skippedGraph;
    build_island_graph_guarded(skippedGraph, 0, contacts, constraints);
    expectTrue(skippedGraph.islandCount() == 0u, "guarded build clears graph on skip");
}

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({5.f, 0.f, 0.f}, 1.f, RB_STATIC);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 1.5f;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(3, contacts, constraints);
    const u32 islandA = graph.bodyIsland(0);
    const ContactIslandGraph::Island& island = graph.island(islandA);

    const IslandSleepSolvePreflight sleepPreflight = preflight_island_sleep_solve(island, bodies);
    expectTrue(!sleepPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepPreflight.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(sleepPreflight.sleepingBodyCount == 1u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepPreflight.awakeBodyCount == 1u, "sleep preflight counts awake bodies");
    expectTrue(sleepPreflight.can_solve(), "mixed sleep/awake island can solve");
    expectTrue(!should_skip_solve_sleeping_island(island, bodies),
               "should_skip false when island has awake dynamic body");
    expectTrue(is_body_sleeping(bodies.flags[0]), "is_body_sleeping detects sleeping flag");
    expectTrue(is_body_dynamic_awake(bodies.flags[1]), "is_body_dynamic_awake detects awake dynamic body");
    expectTrue(!island_all_dynamic_bodies_sleeping(island, bodies),
               "island not all sleeping when one body is awake");
    expectTrue(island_has_awake_dynamic_body(island, bodies), "island has awake dynamic body");

    RigidBodySoA allSleepingBodies = bodies;
    allSleepingBodies.flags[1] |= RB_SLEEPING;
    const IslandSleepSolvePreflight allSleepPreflight = preflight_island_sleep_solve(island, allSleepingBodies);
    expectTrue(!allSleepPreflight.can_solve(), "all-sleeping dynamic island cannot solve");
    expectTrue(should_skip_solve_sleeping_island(island, allSleepingBodies),
               "should_skip true when all dynamic bodies sleeping");
    expectTrue(island_all_dynamic_bodies_sleeping(island, allSleepingBodies),
               "island_all_dynamic_bodies_sleeping true for all sleeping dynamics");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, allSleepingBodies);
    expectTrue(graphPreflight.sleepingOnlyCount >= 1u, "graph sleep preflight counts sleeping-only islands");
    expectTrue(graphPreflight.awakeCount == 0u, "graph sleep preflight has no awake islands");
    expectTrue(!graphPreflight.can_dispatch(), "graph sleep preflight cannot dispatch all sleeping");
    expectTrue(should_skip_solve_all_sleeping_islands(graph, allSleepingBodies),
               "should_skip all sleeping graph dispatch");

    const IslandWakePreflight wakePreflight = preflight_island_wake(island, allSleepingBodies, contacts);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakePreflight.sleepingDynamicCount == 2u, "wake preflight counts sleeping dynamics");
    expectTrue(wakePreflight.nonZeroImpulseCount == 1u, "wake preflight counts non-zero impulses");
    expectTrue(wakePreflight.should_wake(), "wake preflight should wake on external impulse");
    expectTrue(!should_skip_wake_island(island, allSleepingBodies, contacts),
               "should_skip wake false when impulse signal exists");

    RigidBodySoA wakeBodies = allSleepingBodies;
    expectTrue(wake_island_bodies_guarded(island, wakeBodies, contacts),
               "guarded wake clears sleeping flags");
    expectTrue((wakeBodies.flags[0] & RB_SLEEPING) == 0u, "guarded wake clears sleeping body A");
    expectTrue((wakeBodies.flags[1] & RB_SLEEPING) == 0u, "guarded wake clears sleeping body B");
    expectTrue(wakeBodies.sleepTimers[0] == 0.f, "guarded wake resets sleep timer A");
}

void testPreflightSolveIslandWithBodiesGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(2, contacts, constraints);
    const ContactIslandGraph::Island& island = graph.island(0);

    const IslandSolveBodyPreflight preflight =
        preflight_solve_island_with_bodies(island, bodies, contacts, constraints);
    expectTrue(!preflight.skipped, "combined body preflight does not skip constrained island");
    expectTrue(preflight.refs.can_solve(), "combined body preflight sees in-range refs");
    expectTrue(!preflight.sleep.can_solve(), "combined body preflight rejects all-sleeping island");
    expectTrue(!preflight.can_solve(), "combined body preflight cannot solve all-sleeping island");
    expectTrue(should_skip_solve_island_with_bodies(island, bodies, contacts, constraints),
               "should_skip combined true for all-sleeping island");

    SolverWorkBuffers work;
    work.init(2, 1, 1);
    work.contactManifolds() = contacts;
    expectTrue(!solve_island_job(bodies,
                                 island,
                                 work,
                                 constraints,
                                 1.f / 60.f,
                                 0.f,
                                 [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job skips all-sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandSolveBodyPreflight awakePreflight =
        preflight_solve_island_with_bodies(island, bodies, contacts, constraints);
    expectTrue(awakePreflight.can_solve(), "combined body preflight can solve with awake body");
    expectTrue(solve_island_job(bodies,
                                island,
                                work,
                                constraints,
                                1.f / 60.f,
                                0.f,
                                [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job runs when island has awake dynamic body");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = false;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 2;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 5;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
        DistanceConstraint{.bodyA = 0, .bodyB = 9, .restLength = 2.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(preflight.bodyCount == 4u, "build preflight records body count");
    expectTrue(preflight.contactCount == 3u, "build preflight records contact count");
    expectTrue(preflight.distanceConstraintCount == 2u, "build preflight records distance count");
    expectTrue(preflight.validContactCount == 1u, "build preflight counts valid in-range contacts");
    expectTrue(preflight.validDistanceCount == 1u, "build preflight counts valid in-range distances");
    expectTrue(preflight.outOfRangeBodyRefCount == 2u, "build preflight counts out-of-range body refs");
    expectTrue(preflight.rejected, "build preflight rejects out-of-range body refs");
    expectTrue(!preflight.can_build(), "build preflight cannot build with out-of-range refs");
    expectTrue(preflight.has_constraints(), "build preflight still sees partitionable constraints");
    expectTrue(!should_skip_island_build(4, contacts, constraints),
               "should_skip false when valid constraints exist despite OOR refs");
    expectTrue(island_build_reject_reason(4, contacts, constraints) ==
                   IslandBuildRejectReason::OutOfRangeBodyRef,
               "build reject reason flags out-of-range body refs");

    const IslandBuildPreflight cleanPreflight = preflight_island_build(4, {contacts[0]}, {constraints[0]});
    expectTrue(!cleanPreflight.rejected, "clean build preflight is not rejected");
    expectTrue(cleanPreflight.can_build(), "clean build preflight can build");
    expectTrue(cleanPreflight.has_constraints(), "clean build preflight has constraints");
    expectTrue(should_skip_island_build(0, {}, {}), "should_skip true when no constraints to partition");

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);
    expectTrue(graph.islandCount() > 0u, "build still partitions graph with out-of-range refs");
}

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({7.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_STATIC);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(6, contacts, constraints);

    expectTrue(is_sleeping_body(bodies, 0), "is_sleeping_body detects sleeping flag");
    expectTrue(!is_sleeping_body(bodies, 2), "dynamic body is not sleeping");
    expectTrue(is_inactive_solver_body(bodies, 4), "static body is inactive for solver");
    expectTrue(is_fully_inactive_constraint_pair(bodies, 0, 1),
               "both-sleeping pair is fully inactive");

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 activeIsland = graph.bodyIsland(2);
    const IslandSleepWakePreflight sleepingPreflight =
        preflight_island_sleep_wake(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.allSleeping, "sleeping island is all sleeping");
    expectTrue(sleepingPreflight.allInactive, "sleeping island is all inactive");
    expectTrue(!sleepingPreflight.can_solve(), "sleeping island cannot solve");
    expectTrue(should_skip_inactive_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip inactive sleeping island");

    const IslandSleepWakePreflight activePreflight =
        preflight_island_sleep_wake(graph.island(activeIsland), bodies);
    expectTrue(activePreflight.activeDynamicCount == 2u, "active island has dynamic bodies");
    expectTrue(activePreflight.can_solve(), "active island can solve");
    expectTrue(!should_skip_inactive_island_solve(graph.island(activeIsland), bodies),
               "should_skip false for active island");

    const IslandSleepWakeGraphPreflight graphPreflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!graphPreflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(graphPreflight.stats.activeCount == 1u, "graph sleep preflight counts active island");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep preflight counts sleeping island");
    expectTrue(has_active_islands(graph, bodies), "has_active_islands true in mixed graph");
    expectTrue(!should_skip_island_sleep_wake_graph(graph, bodies),
               "should_skip graph false when active islands exist");

    const std::vector<u32> activeIndices = collect_active_island_indices(graph, bodies);
    expectTrue(activeIndices.size() == 1u, "collect_active_island_indices returns active island");

    const IslandSleepWakePreflight outOfRange =
        preflight_island_sleep_wake_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "index sleep preflight skips out-of-range island");
    expectTrue(should_skip_inactive_island_solve_index(graph, graph.islandCount() + 1u, bodies),
               "should_skip index true for out-of-range island");
}

void testPreflightIslandSolvableConstraintRefsGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({7.f, 0.f, 0.f}, 1.f, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(5, contacts, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const u32 activeIsland = graph.bodyIsland(2);
    const IslandConstraintRefsPreflight sleepingRefs = preflight_island_solvable_constraint_refs(
        graph.island(sleepingIsland), bodies, contacts, constraints);
    expectTrue(sleepingRefs.can_solve(), "legacy can_solve still true with in-range refs");
    expectTrue(!sleepingRefs.has_solvable_constraints(),
               "solvable preflight rejects both-sleeping contact pair");
    expectTrue(should_skip_island_solvable_constraint_refs(graph.island(sleepingIsland), bodies, contacts, constraints),
               "should_skip solvable refs on sleeping island");

    const IslandConstraintRefsPreflight activeRefs = preflight_island_solvable_constraint_refs(
        graph.island(activeIsland), bodies, contacts, constraints);
    expectTrue(activeRefs.solvableContactCount == 1u, "active island has solvable contact");
    expectTrue(activeRefs.has_solvable_constraints(), "active island has solvable constraints");
    expectTrue(!should_skip_island_solvable_constraint_refs(graph.island(activeIsland), bodies, contacts, constraints),
               "should_skip false for solvable active island");

    const IslandSolveBodiesPreflight solveBodies =
        preflight_island_solve_bodies(graph.island(activeIsland), bodies, contacts, constraints);
    expectTrue(solveBodies.can_solve(), "combined solve bodies preflight allows active island");
    expectTrue(!should_skip_island_solve_bodies(graph.island(activeIsland), bodies, contacts, constraints),
               "should_skip solve bodies false for active island");

    const IslandSolveBodiesPreflight inactiveSolveBodies =
        preflight_island_solve_bodies(graph.island(sleepingIsland), bodies, contacts, constraints);
    expectTrue(!inactiveSolveBodies.can_solve(), "combined solve bodies preflight rejects sleeping island");
    expectTrue(should_skip_island_solve_bodies(graph.island(sleepingIsland), bodies, contacts, constraints),
               "should_skip solve bodies true for sleeping island");
}

void testPreflightIslandDispatchWithBodiesGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    ContactIslandGraph graph;
    graph.build(2, contacts, {});
    const f32 dt = 1.f / 60.f;

    const IslandDispatchBodiesPreflight preflight = preflight_island_dispatch_with_bodies(graph, bodies, dt);
    expectTrue(preflight.dispatch.can_dispatch(), "dispatch half accepts constrained graph with valid dt");
    expectTrue(preflight.sleepWake.skipped, "sleep/wake half skips all-inactive graph");
    expectTrue(!preflight.can_dispatch(), "combined dispatch preflight cannot dispatch sleeping graph");
    expectTrue(should_skip_island_dispatch_with_bodies(graph, bodies, dt),
               "should_skip dispatch with bodies on all-sleeping graph");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandDispatchBodiesPreflight awakePreflight = preflight_island_dispatch_with_bodies(graph, bodies, dt);
    expectTrue(awakePreflight.can_dispatch(), "combined dispatch preflight dispatches when one body wakes");
    expectTrue(!should_skip_island_dispatch_with_bodies(graph, bodies, dt),
               "should_skip false after wake");
}

void testContactIslandGraphBuildPreflightGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 1;
    contacts.back().bodyB = 1;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const ContactIslandGraphBuildPreflight preflight = preflightContactIslandGraphBuild(2, contacts, constraints);
    expectTrue(preflight.has_self_contacts(), "graph build preflight flags self-contact");
    expectTrue(!preflight.can_build(), "graph build preflight rejects self-contact");
    expectTrue(preflight.reason == ContactIslandGraphBuildRejectReason::SelfContact,
               "graph build preflight records self-contact reason");
    expectTrue(std::strcmp(contactIslandGraphBuildRejectReasonName(preflight.reason), "SelfContact") == 0,
               "graph build reject reason name matches self-contact");
    expectTrue(shouldSkipContactIslandGraphBuild(2, contacts, constraints),
               "shouldSkipContactIslandGraphBuild on self-contact");

    ContactIslandGraph graph;
    expectTrue(!graph.buildGuarded(2, contacts, constraints), "graph buildGuarded rejects self-contact");
    expectTrue(graph.islandCount() == 0u, "rejected graph buildGuarded clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(contacts[0]);
    expectTrue(graph.buildGuarded(2, safeContacts, constraints), "graph buildGuarded succeeds for safe inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "safe graph buildGuarded forms constrained island");

    const IslandBuildPreflight islandPreflight = preflight_island_build(2, contacts, constraints);
    expectTrue(islandPreflight.has_degenerate_refs(), "island build preflight flags self-contact");
    expectTrue(!islandPreflight.can_build(), "island build preflight rejects self-contact");
}

void testPreflightIslandConstraintSolveGraphGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();

    const IslandConstraintSolveGraphPreflight preflight =
        preflight_island_constraint_solve_graph(graph, bodies, work.contactManifolds(), constraints);
    expectTrue(!preflight.skipped, "constraint-solve graph preflight has solveable islands");
    expectTrue(preflight.can_solve_any(), "constraint-solve graph preflight can solve");
    expectTrue(preflight.stats.solveableCount == 1u, "constraint-solve graph counts solveable island");
    expectTrue(preflight.stats.allSleepingCount == 1u, "constraint-solve graph counts all-sleeping island");
    expectTrue(has_constraint_solveable_islands(graph, bodies, work.contactManifolds(), constraints),
               "has_constraint_solveable_islands true when mixed island exists");
    expectTrue(collect_constraint_solveable_island_indices(graph, bodies, work.contactManifolds(), constraints)
                       .size() == 1u,
               "collect constraint-solveable indices returns mixed island");

    const IslandConstraintSolvePreflight byIndex =
        preflight_island_constraint_solve_by_index(graph, graph.bodyIsland(0), bodies, work.contactManifolds(), constraints);
    expectTrue(byIndex.can_solve(), "index constraint-solve preflight allows mixed island");
    expectTrue(!should_skip_island_constraint_solve_by_index(
                   graph, graph.bodyIsland(0), bodies, work.contactManifolds(), constraints),
               "should_skip false for solveable island index");

    const IslandConstraintSolvePreflight blockedIndex = preflight_island_constraint_solve_by_index(
        graph, graph.bodyIsland(2), bodies, work.contactManifolds(), constraints);
    expectTrue(!blockedIndex.can_solve(), "index constraint-solve preflight blocks all-sleeping island");
    expectTrue(should_skip_island_constraint_solve_by_index(
                   graph, graph.bodyIsland(2), bodies, work.contactManifolds(), constraints),
               "should_skip true for all-sleeping island index");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, bodies, work.contactManifolds(), constraints);
    expectTrue(outOfRange.skipped, "index constraint-solve preflight skips out-of-range island");
}

void testGuardedIslandSolvePipelineWithWake() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    const IslandSolvePipelinePreflight pipelinePreflight = preflight_island_solve_pipeline(
        graph.island(mixedIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(pipelinePreflight.can_solve(), "pipeline preflight allows mixed island");
    expectTrue(pipelinePreflight.should_wake_first(), "pipeline preflight requests wake before solve");
    expectTrue(!should_skip_island_solve_pipeline(graph.island(mixedIsland), bodies, work.contactManifolds(), constraints),
               "should_skip pipeline false for mixed island");

    const IslandSolvePipelinePreflight sleepingPipeline = preflight_island_solve_pipeline(
        graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!sleepingPipeline.can_solve(), "pipeline preflight blocks all-sleeping island");
    expectTrue(should_skip_island_solve_pipeline(graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints),
               "should_skip pipeline true for all-sleeping island");

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
    expectTrue(solve_island_job_with_wake_guarded(bodies,
                                                  graph.island(mixedIsland),
                                                  work,
                                                  constraints,
                                                  dt,
                                                  0.f,
                                                  invMassFn),
               "wake+guarded solve runs mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake+guarded solve clears sleeping neighbor");

    const IslandSolveJob mixedJob = extract_island(graph, mixedIsland);
    expectTrue(dispatch_solve_island_job_guarded(bodies, mixedJob, work, constraints, dt, 0.f, invMassFn),
               "guarded job dispatch solves mixed island");
    expectTrue(dispatch_solve_island_with_wake_guarded(bodies, graph, mixedIsland, work, constraints, dt, 0.f, invMassFn),
               "wake guarded index dispatch solves mixed island");
    expectTrue(!dispatch_solve_island_with_wake_guarded(bodies, graph, sleepingIsland, work, constraints, dt, 0.f, invMassFn),
               "wake guarded index dispatch skips all-sleeping island");

    const IslandBatchDispatchResult batch =
        dispatch_all_islands_with_wake_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "batch wake dispatch does not skip constrained graph");
    expectTrue(batch.solvedCount == 1u, "batch wake dispatch solves only solveable island");
    expectTrue(batch.skippedCount == 0u, "batch wake dispatch does not count blocked island as skipped dispatch");
}

void testIslandBuildRejectReasonGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::None), "None") == 0,
               "reject reason name for None");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyInput), "EmptyInput") == 0,
               "reject reason name for EmptyInput");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::UnsafeRefs), "UnsafeRefs") == 0,
               "reject reason name for UnsafeRefs");

    expectTrue(island_build_reject_reason(0, {}, {}) == IslandBuildRejectReason::EmptyInput,
               "empty input rejects with EmptyInput");
    expectTrue(island_build_rejects_for_reason(0, {}, {}, IslandBuildRejectReason::EmptyInput),
               "rejectsForReason matches EmptyInput");
    expectTrue(island_build_reject_reason(4, contacts, constraints) == IslandBuildRejectReason::UnsafeRefs,
               "out-of-range refs reject with UnsafeRefs");
    expectTrue(can_skip_contact_island_build(4, contacts, constraints),
               "can_skip true for unsafe refs");
    expectTrue(!should_run_contact_island_build(4, contacts, constraints),
               "should_run false for unsafe refs");

    const ContactIslandBuildInputStats stats = compute_contact_island_build_input_stats(4, contacts, constraints);
    expectTrue(stats.outOfRangeContactBodyCount == 1u, "input stats count out-of-range contacts");
    expectTrue(stats.outOfRangeDistanceBodyCount == 1u, "input stats count out-of-range distance constraints");

    ContactIslandGraph graph;
    const IslandBuildResult unsafeResult = build_island_graph_guarded_result(graph, 4, contacts, constraints);
    expectTrue(unsafeResult.skipped, "guarded result skips unsafe refs");
    expectTrue(!unsafeResult.built, "guarded result does not build unsafe refs");
    expectTrue(unsafeResult.reason == IslandBuildRejectReason::UnsafeRefs,
               "guarded result records UnsafeRefs reason");

    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(preflight.reason == IslandBuildRejectReason::UnsafeRefs,
               "build preflight records reject reason");
    expectTrue(can_skip_island_build(4, contacts, constraints), "can_skip_island_build mirrors contact helper");
    expectTrue(!should_run_island_build(4, contacts, constraints), "should_run_island_build false for unsafe refs");
}

void testSolveIslandJobGuardedAndPipeline() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds() = contacts;
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandConstraintSolvePreflight mixedPreflight =
        preflight_island_constraint_solve_by_index(graph, mixedIsland, bodies, contacts, constraints);
    expectTrue(!mixedPreflight.skipped, "constraint solve index preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island can solve via index preflight");

    const IslandConstraintSolvePreflight sleepingPreflight =
        preflight_island_constraint_solve_by_index(graph, sleepingIsland, bodies, contacts, constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island blocked by index preflight");
    expectTrue(should_skip_island_constraint_solve_index(graph, sleepingIsland, bodies, contacts, constraints),
               "should_skip constraint solve index on all-sleeping island");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, bodies, contacts, constraints);
    expectTrue(outOfRange.skipped, "constraint solve index preflight skips out-of-range island");

    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(mixedIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve succeeds for mixed island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");

    const IslandConstraintSolveResult solveResult =
        solve_island_job_result(bodies, graph, mixedIsland, work, constraints, dt, 0.f, invMassFn);
    expectTrue(solveResult.solved, "solve result succeeds for mixed island");
    expectTrue(solveResult.islandIndex == mixedIsland, "solve result records island index");

    const IslandSolvePipelinePreflight pipeline =
        preflight_island_solve_pipeline(graph, mixedIsland, bodies, contacts, constraints, dt);
    expectTrue(!pipeline.skipped, "solve pipeline preflight does not skip mixed island");
    expectTrue(pipeline.can_solve(), "solve pipeline preflight can solve mixed island");

    const IslandSolvePipelinePreflight sleepingPipeline =
        preflight_island_solve_pipeline(graph, sleepingIsland, bodies, contacts, constraints, dt);
    expectTrue(!sleepingPipeline.can_solve(), "solve pipeline preflight blocks all-sleeping island");

    const IslandDispatchResult guardedDispatch = dispatch_solve_island_with_solve_guards_result(
        bodies, graph, mixedIsland, work, constraints, dt, 0.f, invMassFn);
    expectTrue(guardedDispatch.solved, "solve-guards dispatch solves mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "solve-guards dispatch wakes sleepers first");

    expectTrue(!dispatch_solve_island_with_solve_guards(bodies,
                                                        graph,
                                                        sleepingIsland,
                                                        work,
                                                        constraints,
                                                        dt,
                                                        0.f,
                                                        invMassFn),
               "solve-guards dispatch skips all-sleeping island");
}

void testWakeResultAndDispatchSkippingSleepers() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds() = contacts;
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph.island(mixedIsland));
    expectTrue(wakeResult.woke, "wake result activates mixed island");
    expectTrue(wakeResult.sleepersWoken == 1u, "wake result counts one sleeper");
    expectTrue(!wakeResult.skipped, "wake result is not skipped");

    const IslandWakeResult outOfRangeWake =
        wake_island_sleepers_by_index_result(bodies, graph, graph.islandCount() + 1u);
    expectTrue(outOfRangeWake.skipped, "wake index result skips out-of-range island");

    const IslandWakeResult sleepingWake =
        wake_island_sleepers_by_index_result(bodies, graph, sleepingIsland);
    expectTrue(sleepingWake.skipped, "wake index result skips all-sleeping island with no active body");

    const IslandWakeBatchResult batch = wake_all_island_sleepers_result(bodies, graph);
    expectTrue(batch.wakeableCount == 0u, "batch wake has no remaining wakeable islands");
    expectTrue(!batch.any_woke(), "batch wake reports no additional woke islands");

    const std::vector<u32> solveable = collect_solveable_island_indices(graph, bodies);
    expectTrue(solveable.size() == 1u, "collect solveable indices skips all-sleeping island");
    expectTrue(solveable.front() == mixedIsland, "collect solveable indices keeps mixed island");

    const IslandBatchDispatchResult batchDispatch = dispatch_all_islands_skipping_sleepers_result(
        bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batchDispatch.skipped, "batch dispatch skipping sleepers does not skip graph");
    expectTrue(batchDispatch.dispatchableCount == 1u,
               "batch dispatch skipping sleepers counts one solveable island");
    expectTrue(batchDispatch.solvedCount == 1u, "batch dispatch skipping sleepers solves mixed island");
    expectTrue(dispatch_all_islands_skipping_sleepers(bodies, graph, work, constraints, dt, 0.f, invMassFn) ==
                   batchDispatch.solvedCount,
               "dispatch_all_islands_skipping_sleepers matches batch result");
}

void testPreflightIslandGraphBuildDeepenGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 0;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    const IslandGraphBuildPreflight graphPreflight = preflight_island_graph_build(3, contacts, constraints);
    expectTrue(!graphPreflight.skipped, "graph build preflight does not skip valid partition inputs");
    expectTrue(graphPreflight.stats.selfPairContactCount == 1u,
               "graph build preflight counts self-pair contacts");
    expectTrue(graphPreflight.stats.selfPairDistanceCount == 0u,
               "graph build preflight has no self-pair distance constraints");
    expectTrue(graphPreflight.has_degenerate_refs(), "graph build preflight flags degenerate refs");
    expectTrue(graphPreflight.can_build(), "self-pairs alone do not block guarded build");

    const IslandBuildPreflight pbdPreflight = preflight_island_build(3, contacts, constraints);
    expectTrue(pbdPreflight.stats.selfPairContactCount == 1u,
               "pbd build preflight mirrors self-pair contact count");
    expectTrue(pbdPreflight.has_degenerate_refs(), "pbd build preflight flags degenerate refs");

    ContactIslandGraph graph;
    expectTrue(build_island_graph_guarded(graph, 3, contacts, constraints),
               "guarded graph build succeeds with only degenerate self-pairs");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build forms one constrained island");

    const IslandGraphIntegrityPreflight integrity =
        preflight_island_graph_integrity(graph, 3, static_cast<u32>(contacts.size()),
                                         static_cast<u32>(constraints.size()));
    expectTrue(!integrity.skipped, "integrity preflight does not skip built graph");
    expectTrue(integrity.can_use(), "integrity preflight accepts in-range built graph");
    expectTrue(!should_skip_island_graph_integrity(graph, 3, static_cast<u32>(contacts.size()),
                                                   static_cast<u32>(constraints.size())),
               "should_skip integrity false for valid graph");
}

void testPreflightIslandSolvePipelineGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    SolverWorkBuffers work;
    work.init(4, 0, 2);

    const IslandSolvePipelinePreflight mixedPipeline =
        preflight_island_solve_pipeline(graph.island(mixedIsland), bodies, contacts, constraints);
    expectTrue(!mixedPipeline.skipped, "pipeline preflight does not skip mixed island");
    expectTrue(mixedPipeline.should_wake_first(), "pipeline preflight should wake mixed island");
    expectTrue(mixedPipeline.can_solve(), "pipeline preflight can solve mixed island");
    expectTrue(!should_skip_island_solve_pipeline(graph.island(mixedIsland), bodies, contacts, constraints),
               "should_skip pipeline false for mixed island");

    const IslandSolvePipelinePreflight sleepingPipeline =
        preflight_island_solve_pipeline(graph.island(sleepingIsland), bodies, contacts, constraints);
    expectTrue(!sleepingPipeline.can_solve(), "pipeline preflight rejects all-sleeping island");
    expectTrue(should_skip_island_solve_pipeline(graph.island(sleepingIsland), bodies, contacts, constraints),
               "should_skip pipeline true for all-sleeping island");

    const IslandSolvePipelinePreflight outOfRangePipeline =
        preflight_island_solve_pipeline_by_index(graph, graph.islandCount() + 1u, bodies, contacts, constraints);
    expectTrue(outOfRangePipeline.skipped, "pipeline index preflight skips out-of-range island");

    const IslandConstraintSolvePreflight byIndex =
        preflight_island_constraint_solve_by_index(graph, mixedIsland, bodies, contacts, constraints);
    expectTrue(byIndex.can_solve(), "constraint solve by index allows mixed island");

    const std::vector<u32> solveable = collect_constraint_solveable_island_indices(graph, bodies, contacts, constraints);
    expectTrue(solveable.size() == 1u, "collect solveable indices returns mixed island only");

    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    expectTrue(solve_island_job_pipeline_guarded(bodies,
                                                   graph.island(mixedIsland),
                                                   work,
                                                   constraints,
                                                   1.f / 60.f,
                                                   0.f,
                                                   invMassFn),
               "pipeline guarded solve succeeds on mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "pipeline guarded solve wakes sleeping neighbor");
    expectTrue(!solve_island_job_pipeline_guarded(bodies,
                                                  graph.island(sleepingIsland),
                                                  work,
                                                  constraints,
                                                  1.f / 60.f,
                                                  0.f,
                                                  invMassFn),
               "pipeline guarded solve skips all-sleeping island");
}

void testDispatchIslandPipelineBatchGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const IslandDispatchBodiesPreflight dispatchPreflight =
        preflight_island_dispatch_with_bodies(graph, bodies, dt);
    expectTrue(!dispatchPreflight.skipped, "dispatch-with-bodies preflight does not skip mixed graph");
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch-with-bodies preflight can dispatch");
    expectTrue(dispatchPreflight.wake.can_wake(), "dispatch-with-bodies preflight sees wakeable island");
    expectTrue(!should_skip_island_dispatch_with_bodies(graph, bodies, dt),
               "should_skip dispatch-with-bodies false for mixed graph");

    const IslandPipelineBatchDispatchResult batch =
        dispatch_all_islands_pipeline_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "pipeline batch does not skip mixed graph");
    expectTrue(batch.wakeCount == 1u, "pipeline batch records wake count");
    expectTrue(batch.solvedCount == 1u, "pipeline batch solves mixed island only");
    expectTrue(batch.sleepingSkippedCount == 1u, "pipeline batch skips all-sleeping island");
    expectTrue(batch.any_solved(), "pipeline batch reports solved island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "pipeline batch wakes sleeping neighbor");
    expectTrue(dispatch_all_islands_pipeline_guarded(bodies, graph, work, constraints, dt, 0.f, invMassFn) ==
                   batch.solvedCount,
               "pipeline guarded count matches batch result");

    const u32 mixedIsland = graph.bodyIsland(0);
    bodies.flags[1] |= RB_SLEEPING;
    const IslandDispatchResult pipelineResult = dispatch_solve_island_pipeline_result(bodies,
                                                                                    graph,
                                                                                    mixedIsland,
                                                                                    work,
                                                                                    constraints,
                                                                                    dt,
                                                                                    0.f,
                                                                                    invMassFn);
    expectTrue(pipelineResult.solved, "pipeline dispatch result solves mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "pipeline dispatch wakes sleeping neighbor");
}

void testContactIslandGraphBuildInputScan() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    const IslandBuildInputStats stats = scan_island_build_inputs(4, contacts, constraints);
    expectTrue(stats.has_unsafe_refs(), "scan flags out-of-range refs");
    expectTrue(!island_build_inputs_safe(4, contacts, constraints), "inputs unsafe with OOB refs");
    expectTrue(stats.inRangeContactCount == 1u, "scan counts in-range contacts");
    expectTrue(stats.outOfRangeDistanceBodyCount == 1u, "scan counts out-of-range distance bodies");

    ContactIslandGraph graph;
    expectTrue(!graph.buildGuarded(4, contacts, constraints), "buildGuarded rejects unsafe inputs");
    expectTrue(graph.islandCount() == 0u, "unsafe buildGuarded clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(contacts[0]);
    const std::vector<DistanceConstraint> safeConstraints = {constraints[0]};
    expectTrue(island_build_inputs_safe(4, safeContacts, safeConstraints), "safe inputs pass scan");
    expectTrue(graph.buildGuarded(4, safeContacts, safeConstraints), "buildGuarded succeeds for safe inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "buildGuarded forms constrained island");
}

void testSolveIslandJobGuardedConstraintPreflight() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandConstraintSolvePreflight mixedPreflight = preflight_island_constraint_solve_by_index(
        graph, mixedIsland, bodies, work.contactManifolds(), constraints);
    expectTrue(!mixedPreflight.skipped, "index constraint preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint solve preflight");
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(mixedIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve succeeds for mixed island");

    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve_by_index(
        graph, sleepingIsland, bodies, work.contactManifolds(), constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint solve preflight");
    expectTrue(should_skip_island_constraint_solve_by_index(
                   graph, sleepingIsland, bodies, work.contactManifolds(), constraints),
               "should_skip constraint solve by index on all-sleeping island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
    expectTrue(!dispatch_solve_island_constraint_guarded(bodies,
                                                         graph,
                                                         sleepingIsland,
                                                         work,
                                                         constraints,
                                                         dt,
                                                         0.f,
                                                         invMassFn),
               "guarded dispatch skips all-sleeping island");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, bodies, work.contactManifolds(), constraints);
    expectTrue(outOfRange.skipped, "index constraint preflight skips out-of-range island");
}

void testSleepAwareDispatchAndWakeBatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const IslandSleepAwareDispatchPreflight preflight =
        preflight_island_sleep_aware_dispatch(graph, bodies, dt);
    expectTrue(!preflight.skipped, "sleep-aware dispatch preflight has solveable islands");
    expectTrue(preflight.can_dispatch(), "sleep-aware dispatch preflight can dispatch");
    expectTrue(!should_skip_island_sleep_aware_dispatch(graph, bodies, dt),
               "should_skip sleep-aware dispatch false for mixed graph");

    const std::vector<u32> solveable =
        collect_constraint_solveable_island_indices(graph, bodies, work.contactManifolds(), constraints);
    expectTrue(solveable.size() == 1u, "collect solveable indices skips all-sleeping island");

    expectTrue(dispatch_solve_island_with_wake_guarded(bodies,
                                                       graph,
                                                       mixedIsland,
                                                       work,
                                                       constraints,
                                                       dt,
                                                       0.f,
                                                       invMassFn),
               "wake-then-solve succeeds for mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake-then-solve clears sleeping flag");

    bodies.flags[1] |= RB_SLEEPING;
    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, mixedIsland);
    expectTrue(wakeResult.woke, "wake result activates mixed island");
    expectTrue(!wakeResult.skipped, "wake result not skipped for wakeable island");

    bodies.flags[1] |= RB_SLEEPING;
    const IslandBatchWakeResult batchWake = wake_all_island_sleepers_result(bodies, graph);
    expectTrue(batchWake.any_woke(), "batch wake reports woke islands");
    expectTrue(batchWake.wokeCount == 1u, "batch wake counts mixed island");

    bodies.flags[1] |= RB_SLEEPING;
    bodies.flags[2] |= RB_SLEEPING;
    bodies.flags[3] |= RB_SLEEPING;
    const IslandSleepAwareBatchDispatchResult batch =
        dispatch_all_islands_sleep_aware_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "sleep-aware batch does not skip mixed graph");
    expectTrue(batch.solvedCount == 1u, "sleep-aware batch solves mixed island only");
    expectTrue(batch.wokeCount >= 1u, "sleep-aware batch wakes mixed island sleepers");
    expectTrue(batch.any_solved(), "sleep-aware batch reports solved island");
    expectTrue(dispatch_all_islands_sleep_aware_guarded(bodies, graph, work, constraints, dt, 0.f, invMassFn) ==
                   batch.solvedCount,
               "sleep-aware guarded count matches batch result");
}

void testIslandDeepenRejectReasonGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    narrowphase::ContactManifold validContact{};
    validContact.valid = true;
    validContact.bodyA = 0;
    validContact.bodyB = 1;
    contacts.push_back(validContact);

    narrowphase::ContactManifold outOfRangeContact = validContact;
    outOfRangeContact.bodyA = 0;
    outOfRangeContact.bodyB = 99;
    contacts.push_back(outOfRangeContact);

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 99, .restLength = 2.f},
    };

    expectTrue(
        island_build_reject_reason(4, contacts, constraints) == IslandBuildRejectReason::OutOfRangeContactBodies,
        "build deepen reject reason flags out-of-range contact bodies");
    expectTrue(
        island_build_rejects_for_reason(4, contacts, constraints, IslandBuildRejectReason::OutOfRangeContactBodies),
        "build deepen rejects_for_reason matches out-of-range contacts");
    expectTrue(
        island_build_reject_reason(0, {}, {}) == IslandBuildRejectReason::EmptyInputs,
        "build deepen reject reason flags empty inputs");
    expectTrue(
        std::strcmp(islandBuildRejectReasonName(IslandBuildRejectReason::OutOfRangeContactBodies),
                    "OutOfRangeContactBodies") == 0,
        "build deepen reject reason name");

    const IslandBuildDeepenPreflight buildDeepen = preflight_island_build_deepen(4, contacts, constraints);
    expectTrue(buildDeepen.rejected, "build deepen preflight rejects unsafe refs");
    expectTrue(!buildDeepen.can_build(), "build deepen preflight cannot build unsafe refs");
    expectTrue(should_skip_island_build_deepen(4, contacts, constraints),
               "should_skip_island_build_deepen on unsafe refs");
    expectTrue(!should_run_island_build(4, contacts, constraints),
               "should_run_island_build false on unsafe refs");

    std::vector<narrowphase::ContactManifold> safeContacts = {validContact};
    const std::vector<DistanceConstraint> safeConstraints = {constraints[0]};
    expectTrue(
        island_build_reject_reason(4, safeContacts, safeConstraints) == IslandBuildRejectReason::None,
        "build deepen reject reason none for safe inputs");
    expectTrue(should_run_island_build(4, safeContacts, safeConstraints),
               "should_run_island_build true for safe inputs");

    ContactIslandGraph graph;
    graph.build(4, safeContacts, safeConstraints);
    expectTrue(graph.islandIndexInRange(0u), "graph islandIndexInRange true for valid index");
    expectTrue(!graph.islandIndexInRange(graph.islandCount() + 1u),
               "graph islandIndexInRange false for out-of-range index");

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<DistanceConstraint> twoIslands = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, {}, twoIslands);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    SolverWorkBuffers work;
    work.init(4, 0, 2);

    expectTrue(
        island_constraint_solve_reject_reason(graph.island(mixedIsland), bodies, work.contactManifolds(), twoIslands) ==
            IslandConstraintSolveRejectReason::None,
        "constraint solve deepen reject none for mixed movable island");
    expectTrue(
        island_constraint_solve_rejects_for_reason(
            graph.island(mixedIsland), bodies, work.contactManifolds(), twoIslands, IslandConstraintSolveRejectReason::None),
        "constraint solve deepen rejects_for_reason none for mixed island");

    bodies.flags[2] |= RB_SLEEPING;
    bodies.flags[3] |= RB_SLEEPING;
    expectTrue(
        island_constraint_solve_reject_reason(graph.island(sleepingIsland), bodies, work.contactManifolds(), twoIslands) ==
            IslandConstraintSolveRejectReason::NoMovableBodies,
        "constraint solve deepen reject no movable bodies for all-sleeping island");
    expectTrue(should_skip_island_constraint_solve_deepen(
                   graph.island(sleepingIsland), bodies, work.contactManifolds(), twoIslands),
               "should_skip_island_constraint_solve_deepen on all-sleeping island");
    expectTrue(!should_run_island_constraint_solve(
                   graph.island(sleepingIsland), bodies, work.contactManifolds(), twoIslands),
               "should_run_island_constraint_solve false on all-sleeping island");

    const IslandConstraintSolveDeepenPreflight solveDeepen = preflight_island_constraint_solve_deepen(
        graph.island(sleepingIsland), bodies, work.contactManifolds(), twoIslands);
    expectTrue(solveDeepen.rejected, "constraint solve deepen preflight rejects all-sleeping island");
    expectTrue(solveDeepen.reason == IslandConstraintSolveRejectReason::NoMovableBodies,
               "constraint solve deepen preflight reason no movable bodies");

    const IslandSolveJob constrainedJob = extract_island(graph, mixedIsland);
    expectTrue(
        island_solve_job_reject_reason(constrainedJob, 1.f / 60.f) == IslandSolveJobRejectReason::None,
        "solve job deepen reject none for constrained job");
    expectTrue(should_run_solve_island_job(constrainedJob, 1.f / 60.f),
               "should_run_solve_island_job true for constrained job");
    expectTrue(
        island_solve_job_reject_reason(constrainedJob, 0.f) == IslandSolveJobRejectReason::InvalidDt,
        "solve job deepen reject invalid dt");
    expectTrue(should_skip_solve_island_job_deepen(constrainedJob, 0.f),
               "should_skip_solve_island_job_deepen on invalid dt");

    IslandSolveJob invalidJob{};
    expectTrue(
        island_solve_job_reject_reason(invalidJob, 1.f / 60.f) == IslandSolveJobRejectReason::OutOfRangeIndex,
        "solve job deepen reject out-of-range for null island job");
    expectTrue(
        island_solve_job_rejects_for_reason(invalidJob, 1.f / 60.f, IslandSolveJobRejectReason::OutOfRangeIndex),
        "solve job deepen rejects_for_reason out-of-range for null island job");

    expectTrue(
        island_dispatch_reject_reason(graph, 1.f / 60.f) == IslandDispatchRejectReason::None,
        "dispatch deepen reject none for dispatchable graph");
    expectTrue(should_run_island_dispatch(graph, 1.f / 60.f),
               "should_run_island_dispatch true for dispatchable graph");
    expectTrue(
        island_dispatch_rejects_for_reason(graph, 0.f, IslandDispatchRejectReason::InvalidDt),
        "dispatch deepen rejects_for_reason invalid dt");
    expectTrue(should_skip_island_dispatch_deepen(graph, 0.f),
               "should_skip_island_dispatch_deepen on invalid dt");

    ContactIslandGraph emptyGraph;
    expectTrue(
        island_dispatch_reject_reason(emptyGraph, 1.f / 60.f) == IslandDispatchRejectReason::NoDispatchableIslands,
        "dispatch deepen reject no dispatchable islands on empty graph");

    bodies.flags[1] &= ~RB_SLEEPING;
    expectTrue(
        island_sleep_solve_reject_reason(graph.island(mixedIsland), bodies) == IslandSleepSolveRejectReason::None,
        "sleep solve deepen reject none for mixed island");
    expectTrue(should_run_island_sleep_solve(graph.island(mixedIsland), bodies),
               "should_run_island_sleep_solve true for mixed island");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(
        island_sleep_solve_reject_reason(graph.island(mixedIsland), bodies) ==
            IslandSleepSolveRejectReason::AllSleeping,
        "sleep solve deepen reject all sleeping");
    expectTrue(should_skip_island_sleep_solve_deepen(graph.island(mixedIsland), bodies),
               "should_skip_island_sleep_solve_deepen on all-sleeping island");
    expectTrue(
        island_sleep_solve_reject_reason_by_index(graph, graph.islandCount() + 1u, bodies) ==
            IslandSleepSolveRejectReason::OutOfRangeIndex,
        "sleep solve deepen reject out-of-range index");

    const IslandSleepSolveDeepenPreflight sleepDeepen =
        preflight_island_sleep_solve_deepen_by_index(graph, mixedIsland, bodies);
    expectTrue(sleepDeepen.rejected, "sleep solve deepen preflight rejects all-sleeping island");
    expectTrue(sleepDeepen.reason == IslandSleepSolveRejectReason::AllSleeping,
               "sleep solve deepen preflight reason all sleeping");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(
        island_wake_reject_reason(graph.island(mixedIsland), bodies) == IslandWakeRejectReason::None,
        "wake deepen reject none for mixed island");
    expectTrue(should_run_island_wake(graph.island(mixedIsland), bodies),
               "should_run_island_wake true for mixed island");
    expectTrue(
        island_wake_reject_reason(graph.island(sleepingIsland), bodies) == IslandWakeRejectReason::NoMixedSleepState,
        "wake deepen reject no mixed sleep state for all-sleeping island");
    expectTrue(should_skip_island_wake_deepen(graph.island(sleepingIsland), bodies),
               "should_skip_island_wake_deepen on all-sleeping island");
    expectTrue(
        island_wake_reject_reason_by_index(graph, graph.islandCount() + 1u, bodies) ==
            IslandWakeRejectReason::OutOfRangeIndex,
        "wake deepen reject out-of-range index");

    const IslandWakeDeepenPreflight wakeDeepen = preflight_island_wake_deepen(graph.island(mixedIsland), bodies);
    expectTrue(!wakeDeepen.rejected, "wake deepen preflight accepts mixed island");
    expectTrue(wakeDeepen.can_wake(), "wake deepen preflight can wake mixed island");
}

void testPreflightIslandGraphIntegrityGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    const IslandGraphIntegrityPreflight preflight =
        preflight_island_graph_integrity(graph, 2, contacts, constraints);
    expectTrue(!preflight.skipped, "integrity preflight does not skip valid graph");
    expectTrue(preflight.is_consistent(), "valid graph passes integrity preflight");
    expectTrue(!should_skip_island_graph_integrity(graph, 2, contacts, constraints),
               "should_skip false for consistent graph");

    const IslandGraphIntegrityPreflight mismatchPreflight =
        preflight_island_graph_integrity(graph, 2, {}, constraints);
    expectTrue(!mismatchPreflight.is_consistent(), "shrunk contact slots fail integrity");
    expectTrue(mismatchPreflight.stats.orphanedContactRefCount >= 1u,
               "integrity counts orphaned contact refs");

    const IslandGraphIntegrityPreflight bodyMismatchPreflight =
        preflight_island_graph_integrity(graph, 1, contacts, constraints);
    expectTrue(!bodyMismatchPreflight.is_consistent(), "undersized body count fails integrity");
    expectTrue(bodyMismatchPreflight.stats.outOfRangeBodyIndexCount >= 1u,
               "integrity counts out-of-range body indices");

    expectTrue(build_island_graph_integrity_guarded(graph, 2, contacts, constraints),
               "integrity guarded build succeeds for valid inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "integrity guarded build forms island");
}

void testPreflightIslandConstraintBodyRefsGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 8, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const u32 islandA = graph.bodyIsland(0);
    const IslandConstraintRefsPreflight safePreflight =
        preflight_island_constraint_refs(graph.island(islandA), contacts, constraints, 4);
    expectTrue(!safePreflight.has_unsafe_body_refs(), "in-range contact bodies pass body-ref preflight");
    expectTrue(safePreflight.can_solve(), "in-range island can solve with body-ref checks");

    const u32 islandB = graph.bodyIsland(2);
    const IslandConstraintRefsPreflight unsafePreflight =
        preflight_island_constraint_refs(graph.island(islandB), contacts, constraints, 4);
    expectTrue(unsafePreflight.has_unsafe_body_refs(), "out-of-range contact body fails body-ref preflight");
    expectTrue(!unsafePreflight.can_solve(), "unsafe body refs block solve preflight");
    expectTrue(unsafePreflight.outOfRangeContactBodyCount == 1u,
               "body-ref preflight counts out-of-range contact bodies");
    expectTrue(unsafePreflight.outOfRangeDistanceBodyCount == 1u,
               "body-ref preflight counts out-of-range distance bodies");
}

void testSolveIslandJobGuardedSkipsAllSleeping() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    expectTrue(solve_island_job(bodies,
                                graph.island(0),
                                work,
                                constraints,
                                1.f / 60.f,
                                0.f,
                                invMassFn),
               "unguarded solve still runs on all-sleeping island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(0),
                                         work,
                                         constraints,
                                         1.f / 60.f,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
    expectTrue(!dispatch_solve_island_guarded(bodies,
                                              graph,
                                              0,
                                              work,
                                              constraints,
                                              1.f / 60.f,
                                              0.f,
                                              invMassFn),
               "guarded dispatch skips all-sleeping island");
}

void testPreflightIslandWakeAndSolveGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    SolverWorkBuffers work;
    work.init(4, 0, 2);

    const IslandWakeAndSolvePreflight mixedPreflight = preflight_island_wake_and_solve(
        graph.island(mixedIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!mixedPreflight.skipped, "wake-and-solve preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island can wake-and-solve");
    expectTrue(mixedPreflight.should_wake_first(), "mixed island should wake before solve");

    const IslandWakeAndSolvePreflight sleepingPreflight = preflight_island_wake_and_solve(
        graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot wake-and-solve");
    expectTrue(should_skip_island_wake_and_solve(
                   graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints),
               "should_skip wake-and-solve on all-sleeping island");

    const IslandWakeAndSolveGraphPreflight graphPreflight =
        preflight_island_wake_and_solve_graph(graph, bodies, 1.f / 60.f);
    expectTrue(graphPreflight.can_dispatch(), "wake-and-solve graph preflight can dispatch mixed graph");
    expectTrue(!should_skip_island_wake_and_solve_graph(graph, bodies, 1.f / 60.f),
               "should_skip wake-and-solve graph false when mixed island exists");
    expectTrue(collect_solveable_island_indices(graph, bodies, work.contactManifolds(), constraints).size() ==
                   1u,
               "collect solveable indices returns mixed island only");

    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        return bodySoA.invMasses[index];
    };
    const IslandWakeAndSolveResult wakeSolveResult = dispatch_solve_island_with_wake_result(
        bodies, graph, mixedIsland, work, constraints, 1.f / 60.f, 0.f, invMassFn);
    expectTrue(wakeSolveResult.woke, "wake-and-solve result wakes mixed island sleepers");
    expectTrue(wakeSolveResult.solved, "wake-and-solve result solves mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake-and-solve clears sleeping flag");

    const IslandBatchWakeAndSolveResult batchResult = dispatch_all_islands_with_wake_result(
        bodies, graph, work, constraints, 1.f / 60.f, 0.f, invMassFn);
    expectTrue(!batchResult.skipped, "batch wake-and-solve does not skip mixed graph");
    expectTrue(batchResult.solvedCount >= 1u, "batch wake-and-solve solves at least one island");

    const IslandBatchDispatchResult nonsleepingBatch = dispatch_all_nonsleeping_islands_result(
        bodies, graph, work, constraints, 1.f / 60.f, 0.f, invMassFn);
    expectTrue(nonsleepingBatch.solvedCount >= 1u, "nonsleeping batch solves active island");
}

void testIslandRejectReasonGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    expectTrue(island_build_rejects_for_reason(4, contacts, constraints, IslandBuildRejectReason::OutOfRangeRefs),
               "build reject reason flags out-of-range refs");
    expectTrue(island_build_reject_reason_name(IslandBuildRejectReason::OutOfRangeRefs) != nullptr,
               "build reject reason name is non-null");
    expectTrue(island_build_reject_reason(0, {}, {}) == IslandBuildRejectReason::EmptyInput,
               "empty build inputs report EmptyInput reason");
    expectTrue(island_build_reject_reason(4, {}, {}) == IslandBuildRejectReason::None,
               "lone body count without constraints is buildable");
    expectTrue(!should_run_island_build(4, contacts, constraints),
               "should_run_island_build false for unsafe refs");
    expectTrue(should_run_island_build(4, {contacts[0]}, {constraints[0]}),
               "should_run_island_build true for in-range inputs");

    const IslandBuildPreflight buildPreflight = preflight_island_build(4, contacts, constraints);
    expectTrue(buildPreflight.reason == IslandBuildRejectReason::OutOfRangeRefs,
               "build preflight records out-of-range reason");
    expectTrue(is_in_range_island_contact(contacts[0], 4u), "in-range contact helper accepts valid pair");
    expectTrue(!is_in_range_island_contact(contacts[1], 4u), "in-range contact helper rejects invalid pair");
    expectTrue(is_in_range_island_distance(constraints[0], 4u),
               "in-range distance helper accepts valid constraint");
    expectTrue(!is_in_range_island_distance(constraints[1], 4u),
               "in-range distance helper rejects invalid constraint");

    ContactIslandGraph graph;
    std::vector<DistanceConstraint> islandConstraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, {}, islandConstraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    SolverWorkBuffers work;
    work.init(4, 0, 2);

    expectTrue(island_constraint_solve_reject_reason(
                   graph.island(mixedIsland), bodies, work.contactManifolds(), islandConstraints) ==
                   IslandConstraintSolveRejectReason::None,
               "mixed island constraint solve reason is None");
    expectTrue(island_constraint_solve_rejects_for_reason(graph.island(sleepingIsland),
                                                          bodies,
                                                          work.contactManifolds(),
                                                          islandConstraints,
                                                          IslandConstraintSolveRejectReason::NoMovableBodies),
               "all-sleeping island constraint solve rejects for NoMovableBodies");
    expectTrue(island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason::NoMovableBodies) !=
                   nullptr,
               "constraint solve reject reason name is non-null");
    expectTrue(should_run_island_constraint_solve(
                   graph.island(mixedIsland), bodies, work.contactManifolds(), islandConstraints),
               "should_run constraint solve true for mixed island");
    expectTrue(!should_run_island_constraint_solve(
                   graph.island(sleepingIsland), bodies, work.contactManifolds(), islandConstraints),
               "should_run constraint solve false for all-sleeping island");

    expectTrue(island_sleep_reject_reason(graph.island(mixedIsland), bodies) == IslandSleepRejectReason::None,
               "mixed island sleep reason is None");
    expectTrue(island_sleep_rejects_for_reason(graph.island(sleepingIsland),
                                               bodies,
                                               IslandSleepRejectReason::AllSleeping),
               "all-sleeping island sleep rejects for AllSleeping");
    expectTrue(island_wake_reject_reason(graph.island(mixedIsland), bodies) == IslandWakeRejectReason::None,
               "mixed island wake reason is None");
    expectTrue(island_wake_rejects_for_reason(graph.island(sleepingIsland),
                                              bodies,
                                              IslandWakeRejectReason::NoWakeTarget),
               "all-sleeping island wake rejects for NoWakeTarget");
    expectTrue(should_run_island_wake(graph.island(mixedIsland), bodies),
               "should_run island wake true for mixed island");
    expectTrue(!should_run_island_wake(graph.island(sleepingIsland), bodies),
               "should_run island wake false when no wake target");

    const IslandSleepGraphPreflight sleepGraph = preflight_island_sleep_graph(graph, bodies);
    expectTrue(sleepGraph.reason == IslandSleepGraphRejectReason::None,
               "mixed graph sleep reason is None");
    expectTrue(island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason::None) != nullptr,
               "sleep graph reject reason name is non-null");

    const IslandWakeGraphPreflight wakeGraph = preflight_island_wake_graph(graph, bodies);
    expectTrue(wakeGraph.reason == IslandWakeGraphRejectReason::None,
               "wakeable graph wake reason is None");
    expectTrue(should_run_island_wake_graph(graph, bodies),
               "should_run graph wake true when wakeable islands exist");

    ContactIslandGraph emptyGraph;
    expectTrue(island_sleep_graph_reject_reason(emptyGraph, bodies) == IslandSleepGraphRejectReason::EmptyGraph,
               "empty graph sleep reason is EmptyGraph");
    expectTrue(island_wake_graph_reject_reason(emptyGraph, bodies) == IslandWakeGraphRejectReason::EmptyGraph,
               "empty graph wake reason is EmptyGraph");
}

void testPreflightIslandBuildDeepenGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    expectTrue(island_build_rejects_for_reason(4, contacts, constraints,
                                               IslandBuildRejectReason::OutOfRangeContactBodies),
               "build deepen rejects out-of-range contact bodies");
    expectTrue(island_build_rejects_for_reason(0, {}, {}, IslandBuildRejectReason::EmptyInputs),
               "build deepen rejects empty inputs");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyInputs), "EmptyInputs") ==
                   0,
               "build reject reason name for empty inputs");

    const IslandBuildDeepenPreflight deepenPreflight = preflight_island_build_deepen(4, contacts, constraints);
    expectTrue(!deepenPreflight.can_build(), "build deepen preflight cannot build unsafe refs");
    expectTrue(deepenPreflight.reason == IslandBuildRejectReason::OutOfRangeContactBodies,
               "build deepen preflight records contact reject reason");
    expectTrue(can_skip_island_build_deepen(4, contacts, constraints),
               "can_skip_island_build_deepen on unsafe refs");

    ContactIslandGraph graph;
    expectTrue(!build_island_graph_with_preflight(graph, 4, contacts, constraints),
               "build with preflight rejects unsafe refs");
    expectTrue(graph.islandCount() == 0u, "failed build-with-preflight clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(contacts[0]);
    const std::vector<DistanceConstraint> safeConstraints = {constraints[0]};
    expectTrue(build_island_graph_with_preflight(graph, 4, safeContacts, safeConstraints),
               "build with preflight succeeds for in-range inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "build with preflight forms constrained island");
}

void testPreflightIslandConstraintSolveDeepenGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandConstraintSolveDeepenPreflight mixedDeepen = preflight_island_constraint_solve_deepen(
        graph.island(mixedIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(mixedDeepen.can_solve(), "mixed island passes constraint-solve deepen preflight");
    expectTrue(mixedDeepen.reason == IslandConstraintSolveRejectReason::None,
               "mixed island has no constraint-solve reject reason");

    const IslandConstraintSolvePreflight byIndexPreflight = preflight_island_constraint_solve_by_index(
        graph, mixedIsland, bodies, work.contactManifolds(), constraints);
    expectTrue(byIndexPreflight.can_solve(), "constraint-solve by-index preflight succeeds for mixed island");
    expectTrue(preflight_island_solve_bodies_by_index(graph, mixedIsland, bodies).movableCount == 1u,
               "solve-bodies by-index counts movable body");

    const IslandConstraintSolvePreflight outOfRangePreflight = preflight_island_constraint_solve_by_index(
        graph, graph.islandCount() + 1u, bodies, work.contactManifolds(), constraints);
    expectTrue(outOfRangePreflight.skipped, "constraint-solve by-index skips out-of-range island");
    expectTrue(should_skip_island_constraint_solve_by_index(
                   graph, graph.islandCount() + 1u, bodies, work.contactManifolds(), constraints),
               "should_skip constraint-solve by-index on out-of-range island");

    expectTrue(island_constraint_solve_rejects_for_reason(graph.island(sleepingIsland),
                                                          bodies,
                                                          work.contactManifolds(),
                                                          constraints,
                                                          IslandConstraintSolveRejectReason::NoMovableBodies),
               "all-sleeping island rejects with NoMovableBodies");
    expectTrue(can_skip_island_constraint_solve_deepen(
                   graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints),
               "can_skip constraint-solve deepen on all-sleeping island");
    expectTrue(std::strcmp(island_constraint_solve_reject_reason_name(
                               IslandConstraintSolveRejectReason::NoMovableBodies),
                           "NoMovableBodies") == 0,
               "constraint-solve reject reason name");
}

void testDispatchSolveIslandWithPreflightGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.2f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();
    work.ensureLambdaCapacity(0, 2);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    const f32 dt = 1.f / 60.f;

    const IslandFullDispatchPreflight mixedDispatchPreflight = preflight_dispatch_solve_island(
        graph, mixedIsland, bodies, work.contactManifolds(), constraints, dt);
    expectTrue(mixedDispatchPreflight.can_dispatch(), "full dispatch preflight allows mixed island");
    expectTrue(mixedDispatchPreflight.wake.should_wake_sleepers(),
               "full dispatch preflight detects wakeable mixed island");

    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, mixedIsland);
    expectTrue(wakeResult.woke, "wake result activates mixed island sleepers");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake result clears sleeping flag");

    expectTrue(dispatch_solve_island_with_preflight(bodies,
                                                    graph,
                                                    mixedIsland,
                                                    work,
                                                    constraints,
                                                    dt,
                                                    0.f,
                                                    [](const RigidBodySoA& bodySoA, u32 index) {
                                                        return bodySoA.invMasses[index];
                                                    }),
               "dispatch with preflight solves mixed island after wake");

    const IslandDispatchResult sleepingDispatch = dispatch_solve_island_with_preflight_result(
        bodies,
        graph,
        sleepingIsland,
        work,
        constraints,
        dt,
        0.f,
        [](const RigidBodySoA& bodySoA, u32 index) { return bodySoA.invMasses[index]; });
    expectTrue(!sleepingDispatch.solved, "dispatch with preflight skips all-sleeping island");
    expectTrue(sleepingDispatch.skipped, "all-sleeping island dispatch is skipped");

    const IslandBatchDispatchResult batchResult = dispatch_all_islands_with_preflight_result(
        bodies,
        graph,
        work,
        constraints,
        dt,
        0.f,
        [](const RigidBodySoA& bodySoA, u32 index) { return bodySoA.invMasses[index]; });
    expectTrue(batchResult.solvedCount == 1u, "batch dispatch with preflight solves one nonsleeping island");
    expectTrue(batchResult.skippedCount == 0u,
               "batch dispatch with preflight omits all-sleeping islands before dispatch");
    expectTrue(collect_nonsleeping_island_indices(graph, bodies).size() == 1u,
               "nonsleeping collection excludes all-sleeping island");

    expectTrue(island_sleep_reject_reason(graph.island(mixedIsland), bodies) ==
                   IslandSleepRejectReason::NotAllSleeping,
               "mixed island cannot skip solve due to sleep");
    expectTrue(island_wake_reject_reason(graph.island(sleepingIsland), bodies) ==
                   IslandWakeRejectReason::NoMixedSleepState,
               "all-sleeping island wake rejects with NoMixedSleepState");
    expectTrue(std::strcmp(island_wake_reject_reason_name(IslandWakeRejectReason::NoMixedSleepState),
                           "NoMixedSleepState") == 0,
               "wake reject reason name");
}

void testGuardedIslandSleepAwareDispatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    const f32 dt = 1.f / 60.f;
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) { return bodySoA.invMasses[index]; };

    const IslandConstraintSolvePreflight outOfRangeSolve =
        preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, bodies, contacts, constraints);
    expectTrue(outOfRangeSolve.skipped, "constraint-solve by-index preflight skips out-of-range island");

    const IslandSolveBodiesPreflight outOfRangeBodies =
        preflight_island_solve_bodies_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRangeBodies.skipped, "solve-bodies by-index preflight skips out-of-range island");

    const IslandSleepAwareDispatchPreflight mixedPreflight =
        preflight_island_sleep_aware_dispatch(graph.island(mixedIsland), bodies, contacts, constraints, dt);
    expectTrue(mixedPreflight.can_dispatch(), "sleep-aware dispatch preflight allows mixed island");
    expectTrue(!should_skip_island_sleep_aware_dispatch(
                   graph.island(mixedIsland), bodies, contacts, constraints, dt),
               "should_skip sleep-aware dispatch false for mixed island");

    const IslandSleepAwareDispatchPreflight sleepingPreflight =
        preflight_island_sleep_aware_dispatch_by_index(graph, sleepingIsland, bodies, contacts, constraints, dt);
    expectTrue(!sleepingPreflight.can_dispatch(), "sleep-aware dispatch preflight rejects all-sleeping island");
    expectTrue(should_skip_island_sleep_aware_dispatch(
                   graph.island(sleepingIsland), bodies, contacts, constraints, dt),
               "should_skip sleep-aware dispatch true for all-sleeping island");

    const IslandSleepAwareGraphPreflight graphPreflight = preflight_island_sleep_aware_graph(graph, bodies, dt);
    expectTrue(graphPreflight.can_dispatch(), "sleep-aware graph preflight has dispatchable islands");
    expectTrue(collect_solveable_island_indices(graph, bodies, contacts, constraints).size() == 1u,
               "collect solveable indices skips all-sleeping island");

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(mixedIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve resolves mixed island");

    expectTrue(!dispatch_solve_island_sleep_guarded(bodies,
                                                    graph,
                                                    sleepingIsland,
                                                    work,
                                                    constraints,
                                                    dt,
                                                    0.f,
                                                    invMassFn),
               "sleep-guarded dispatch skips all-sleeping island");
    expectTrue(dispatch_solve_island_sleep_guarded(bodies,
                                                   graph,
                                                   mixedIsland,
                                                   work,
                                                   constraints,
                                                   dt,
                                                   0.f,
                                                   invMassFn),
               "sleep-guarded dispatch resolves mixed island");

    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(dispatch_solve_island_with_wake_guarded(bodies,
                                                       graph,
                                                       mixedIsland,
                                                       work,
                                                       constraints,
                                                       dt,
                                                       0.f,
                                                       invMassFn),
               "wake-guarded dispatch solves mixed island after wake");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake-guarded dispatch clears sleeping flag");

    const IslandDispatchResult sleepingResult = dispatch_solve_island_sleep_aware_result(bodies,
                                                                                         graph,
                                                                                         sleepingIsland,
                                                                                         work,
                                                                                         constraints,
                                                                                         dt,
                                                                                         0.f,
                                                                                         invMassFn);
    expectTrue(sleepingResult.skipped, "sleep-aware dispatch result skips all-sleeping island");
    expectTrue(!sleepingResult.solved, "sleep-aware dispatch result does not solve all-sleeping island");

    const IslandDispatchResult mixedResult = dispatch_solve_island_sleep_aware_result(bodies,
                                                                                      graph,
                                                                                      mixedIsland,
                                                                                      work,
                                                                                      constraints,
                                                                                      dt,
                                                                                      0.f,
                                                                                      invMassFn);
    expectTrue(mixedResult.solved, "sleep-aware dispatch result solves mixed island");

    const IslandBatchSleepAwareDispatchResult batch =
        dispatch_nonsleeping_islands_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "nonsleeping batch dispatch runs when islands are solveable");
    expectTrue(batch.solvedCount == 1u, "nonsleeping batch dispatch solves one mixed island");
    expectTrue(batch.skippedSleepCount == 0u, "nonsleeping batch does not count mixed island as sleep skip");
    expectTrue(dispatch_nonsleeping_islands_guarded(bodies, graph, work, constraints, dt, 0.f, invMassFn) == 1u,
               "nonsleeping guarded batch returns solved count");
}

void testContactIslandGraphBodyIndexHelpers() {
    narrowphase::ContactManifold inRange{};
    inRange.valid = true;
    inRange.bodyA = 0;
    inRange.bodyB = 1;
    expectTrue(contact_body_indices_in_range(inRange, 2u), "in-range contact body indices pass");
    expectTrue(!contact_body_indices_in_range(inRange, 1u), "out-of-range contact body indices fail");

    const DistanceConstraint constraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f};
    expectTrue(distance_body_indices_in_range(constraint, 2u), "in-range distance body indices pass");
    expectTrue(!distance_body_indices_in_range(constraint, 1u), "out-of-range distance body indices fail");

    expectTrue(constraint_pair_is_degenerate(1u, 1u), "degenerate pair detected");
    expectTrue(!constraint_pair_is_degenerate(0u, 1u), "distinct pair is not degenerate");
}

void testIslandBuildResultAndDegenerateGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 0;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 1, .bodyB = 1, .restLength = 1.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(2, contacts, constraints);
    expectTrue(preflight.has_degenerate_refs(), "build preflight flags self-contact and self-distance");
    expectTrue(preflight.stats.selfContactCount == 1u, "build preflight counts self-contact");
    expectTrue(preflight.stats.selfDistanceCount == 1u, "build preflight counts self-distance");
    expectTrue(!preflight.can_build(), "build preflight rejects degenerate refs");
    expectTrue(should_skip_island_build(2, contacts, constraints),
               "should_skip_island_build on degenerate refs");

    ContactIslandGraph graph;
    const IslandBuildResult result = build_island_graph_result(graph, 2, contacts, constraints);
    expectTrue(!result.built, "build result does not build degenerate graph");
    expectTrue(result.degenerateRefs, "build result records degenerate refs");
    expectTrue(graph.islandCount() == 0u, "degenerate build result clears graph");

    std::vector<narrowphase::ContactManifold> safeContacts;
    safeContacts.push_back(narrowphase::ContactManifold{});
    safeContacts.back().valid = true;
    safeContacts.back().bodyA = 0;
    safeContacts.back().bodyB = 1;
    const std::vector<DistanceConstraint> safeConstraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    expectTrue(has_in_range_constraints(2, safeContacts, safeConstraints),
               "has_in_range_constraints true for valid inputs");
    expectTrue(!has_in_range_constraints(0, {}, {}), "has_in_range_constraints false for empty inputs");

    const IslandBuildResult safeResult = build_island_graph_result(graph, 2, safeContacts, safeConstraints);
    expectTrue(safeResult.built, "build result succeeds for in-range inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "safe build result forms constrained island");
}

void testPreflightIslandConstraintSolveGraphGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const IslandConstraintSolveGraphPreflight preflight =
        preflight_island_constraint_solve_graph(graph, bodies, contacts, constraints);
    expectTrue(!preflight.skipped, "constraint-solve graph preflight has solveable islands");
    expectTrue(preflight.stats.solveableCount == 1u, "constraint-solve graph counts mixed island");
    expectTrue(preflight.stats.allSleepingCount == 1u, "constraint-solve graph counts all-sleeping island");
    expectTrue(!should_skip_island_constraint_solve_graph(graph, bodies, contacts, constraints),
               "should_skip constraint-solve graph false when mixed island exists");

    const std::vector<u32> solveable = collect_solveable_island_indices(graph, bodies, contacts, constraints);
    expectTrue(solveable.size() == 1u, "collect_solveable_island_indices returns mixed island");

    const IslandConstraintSolvePreflight byIndex =
        preflight_island_constraint_solve_by_index(graph, graph.bodyIsland(0), bodies, contacts, constraints);
    expectTrue(byIndex.can_solve(), "index constraint-solve preflight allows mixed island");
    expectTrue(preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, bodies, contacts, constraints)
                   .skipped,
               "index constraint-solve preflight skips out-of-range island");

    bodies.flags[0] |= RB_SLEEPING;
    expectTrue(should_skip_island_constraint_solve_graph(graph, bodies, contacts, constraints),
               "should_skip constraint-solve graph true when all islands are sleeping");
}

void testSolveIslandJobGuardedAndConstraintDispatch() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const IslandSolveJob job = extract_island(graph, 0);
    expectTrue(solve_island_job_guarded(bodies,
                                        *job.island,
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "solve_island_job_guarded solves constrained island");
    expectTrue(dispatch_solve_island_constraint_guarded(bodies,
                                                        graph,
                                                        0u,
                                                        work,
                                                        constraints,
                                                        dt,
                                                        0.f,
                                                        invMassFn),
               "dispatch_solve_island_constraint_guarded solves constrained island");

    bodies.flags[0] |= RB_SLEEPING;
    bodies.flags[1] |= RB_SLEEPING;
    expectTrue(!solve_island_job_guarded(bodies,
                                         *job.island,
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "solve_island_job_guarded skips all-sleeping island");
    expectTrue(!dispatch_solve_island_constraint_guarded(bodies,
                                                         graph,
                                                         0u,
                                                         work,
                                                         constraints,
                                                         dt,
                                                         0.f,
                                                         invMassFn),
               "dispatch_solve_island_constraint_guarded skips all-sleeping island");
}

void testPreflightIslandSleepWakeCombinedGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 mixedIsland = graph.bodyIsland(0);
    const IslandSleepWakePreflight combined = preflight_island_sleep_wake(graph.island(mixedIsland), bodies);
    expectTrue(!combined.skipped, "combined sleep/wake preflight does not skip mixed island");
    expectTrue(combined.can_solve_after_wake(), "mixed island can solve after wake");
    expectTrue(combined.should_wake_before_solve(), "mixed island should wake before solve");

    const IslandSleepWakePreflight byIndex =
        preflight_island_sleep_wake_by_index(graph, mixedIsland, bodies);
    expectTrue(byIndex.should_wake_before_solve(), "index combined sleep/wake preflight agrees");
    expectTrue(preflight_island_sleep_wake_by_index(graph, graph.islandCount() + 1u, bodies).skipped,
               "index combined sleep/wake preflight skips out-of-range island");
}

void testWakeIslandSleepersResultAndDispatchSolveable() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, mixedIsland);
    expectTrue(wakeResult.woke, "wake result activates mixed island sleepers");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake result clears sleeping flag");

    const IslandBatchWakeResult wakeBatch = wake_all_island_sleepers_result(bodies, graph);
    expectTrue(wakeBatch.wokeCount == 0u, "batch wake skips already-awake mixed island");

    const IslandDispatchSolveablePreflight dispatchPreflight =
        preflight_island_dispatch_solveable(graph, bodies, contacts, constraints, dt);
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch-solveable preflight can dispatch");
    expectTrue(!should_skip_island_dispatch_solveable(graph, bodies, contacts, constraints, dt),
               "should_skip dispatch-solveable false for mixed graph");

    const IslandBatchDispatchSolveableResult batch =
        dispatch_solveable_islands_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "dispatch-solveable batch does not skip mixed graph");
    expectTrue(batch.solvedCount == 1u,
               "dispatch-solveable batch solves only the mixed island after wake");
    expectTrue(batch.any_solved(), "dispatch-solveable batch reports solved islands");
    expectTrue(dispatch_solveable_islands_guarded(bodies, graph, work, constraints, dt, 0.f, invMassFn) ==
                   batch.solvedCount,
               "dispatch_solveable_islands_guarded count matches batch result");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(should_skip_island_dispatch_solveable(emptyGraph, bodies, contacts, constraints, dt),
               "should_skip dispatch-solveable true for empty graph");
}

void testContactIslandGraphBuildGuarded() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    expectTrue(graph.buildGuarded(2, contacts, constraints),
               "graph buildGuarded succeeds for in-range inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "graph buildGuarded forms constrained island");

    expectTrue(!graph.buildGuarded(0, {}, {}), "graph buildGuarded skips empty inputs");
    expectTrue(graph.islandCount() == 0u, "skipped graph buildGuarded clears graph");

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;
    expectTrue(!graph.buildGuarded(4, contacts, constraints),
               "graph buildGuarded rejects unsafe out-of-range contact refs");
    expectTrue(graph.islandCount() == 0u, "unsafe graph buildGuarded clears graph");

    const IslandGraphBuildPreflight preflight = preflightIslandGraphBuild(4, contacts, constraints);
    expectTrue(preflight.has_unsafe_refs(), "graph build preflight flags unsafe refs");
    expectTrue(!preflight.can_build(), "graph build preflight cannot build unsafe refs");
    expectTrue(shouldSkipIslandGraphBuild(4, contacts, constraints),
               "shouldSkipIslandGraphBuild on unsafe refs");
}

void testPreflightIslandSolvePassGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandSolvePassPreflight mixedPreflight =
        preflight_island_solve_pass(graph.island(mixedIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!mixedPreflight.skipped, "solve-pass preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes solve-pass preflight");
    expectTrue(!mixedPreflight.sleep.allSleeping, "mixed island is not all-sleeping in solve-pass preflight");

    const IslandSolvePassPreflight sleepingPreflight =
        preflight_island_solve_pass(graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails solve-pass preflight");
    expectTrue(should_skip_island_solve_pass(graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints),
               "should_skip_island_solve_pass on all-sleeping island");

    const IslandSolvePassPreflight outOfRange =
        preflight_island_solve_pass_by_index(graph, graph.islandCount() + 2u, bodies, work.contactManifolds(), constraints);
    expectTrue(outOfRange.skipped, "solve-pass index preflight skips out-of-range island");

    const IslandConstraintSolvePreflight byIndex =
        preflight_island_constraint_solve_by_index(graph, sleepingIsland, bodies, work.contactManifolds(), constraints);
    expectTrue(!byIndex.can_solve(), "constraint-solve index preflight rejects all-sleeping island");

    expectTrue(collect_solveable_island_indices(graph, bodies).size() == 1u,
               "collect_solveable_island_indices returns mixed island only");
}

void testSolveIslandJobGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 activeIsland = graph.bodyIsland(0);
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(activeIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve succeeds for active island");

    const u32 sleepingIsland = graph.bodyIsland(2);
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
}

void testDispatchSolveIslandWithWakeGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(2, 0, 1);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const IslandWakeAndSolveResult result = dispatch_solve_island_with_wake_result(bodies,
                                                                                   graph,
                                                                                   graph.bodyIsland(0),
                                                                                   work,
                                                                                   constraints,
                                                                                   dt,
                                                                                   0.f,
                                                                                   invMassFn);
    expectTrue(result.woke, "wake-then-solve wakes mixed island sleeper");
    expectTrue(result.solved, "wake-then-solve resolves mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake-then-solve clears sleeping flag");
    expectTrue(dispatch_solve_island_with_wake_guarded(bodies,
                                                       graph,
                                                       graph.bodyIsland(0),
                                                       work,
                                                       constraints,
                                                       dt,
                                                       0.f,
                                                       invMassFn),
               "wake-then-solve guarded dispatch succeeds after wake");
}

void testDispatchAllIslandsWithWakeGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const IslandWakeAndDispatchResult batch = dispatch_all_islands_with_wake_result(
        bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "wake batch does not skip constrained graph");
    expectTrue(batch.wokeCount == 1u, "wake batch wakes mixed island only");
    expectTrue(batch.dispatch.solvedCount == 1u, "wake batch solves mixed island only");
    expectTrue(batch.any_solved(), "wake batch reports solved island");
    expectTrue(dispatch_all_islands_with_wake_guarded(bodies, graph, work, constraints, dt, 0.f, invMassFn) == 1u,
               "wake batch guarded count matches solved islands");

    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, graph.islandCount() + 1u);
    expectTrue(wakeResult.skipped, "wake result skips out-of-range island");
}

void testPreflightBuiltIslandGraphGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 99;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 88, .restLength = 2.f},
    };

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);

    const IslandBuiltGraphPreflight preflight =
        preflight_built_island_graph(graph, 4, contacts, constraints);
    expectTrue(!preflight.skipped, "built graph preflight does not skip non-empty graph");
    expectTrue(preflight.is_consistent(), "built graph with filtered unsafe refs is consistent");
    expectTrue(!preflight.has_unsafe_refs(), "filtered build excludes out-of-range constraint refs");
    expectTrue(!should_skip_built_island_graph(graph, 4, contacts, constraints),
               "should_skip false for consistent built graph");
    expectTrue(graph.constrainedIslandCount() == 1u,
               "build guards skip out-of-range contacts and distance constraints");

    ContactIslandGraph loneGraph;
    loneGraph.build(2, {}, {});
    const IslandBuiltGraphPreflight lonePreflight =
        preflight_built_island_graph(loneGraph, 2, {}, {});
    expectTrue(lonePreflight.is_consistent(), "lone-body built graph has no unsafe refs");

    ContactIslandGraph emptyGraph;
    const IslandBuiltGraphPreflight emptyPreflight =
        preflight_built_island_graph(emptyGraph, 4, {}, {});
    expectTrue(emptyPreflight.skipped, "built graph preflight skips when graph is empty");
}

void testPreflightIslandConstraintSolveByIndex() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    SolverWorkBuffers work;
    work.init(4, 0, 2);

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandConstraintSolvePreflight mixedPreflight = preflight_island_constraint_solve_by_index(
        graph, mixedIsland, bodies, work.contactManifolds(), constraints);
    expectTrue(!mixedPreflight.skipped, "index constraint preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint solve by index");

    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve_by_index(
        graph, sleepingIsland, bodies, work.contactManifolds(), constraints);
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint solve by index");
    expectTrue(should_skip_island_constraint_solve_by_index(
                   graph, sleepingIsland, bodies, work.contactManifolds(), constraints),
               "should_skip constraint solve by index on all-sleeping island");

    const IslandConstraintSolvePreflight outOfRange =
        preflight_island_constraint_solve_by_index(
            graph, graph.islandCount() + 1u, bodies, work.contactManifolds(), constraints);
    expectTrue(outOfRange.skipped, "index constraint preflight skips out-of-range island");
}

void testSolveIslandJobGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
    expectTrue(solve_island_job_with_wake_guarded(bodies,
                                                  graph.island(mixedIsland),
                                                  work,
                                                  constraints,
                                                  dt,
                                                  0.f,
                                                  invMassFn),
               "wake-guarded solve resolves mixed-sleep island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake-guarded solve clears sleeping flag");
}

void testPreflightIslandDispatchSleepGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const f32 dt = 1.f / 60.f;
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(graph, bodies, dt);
    expectTrue(!preflight.skipped, "dispatch-sleep preflight does not skip mixed graph");
    expectTrue(preflight.can_dispatch(), "dispatch-sleep preflight can dispatch mixed graph");
    expectTrue(!should_skip_island_dispatch_sleep(graph, bodies, dt),
               "should_skip dispatch-sleep false for mixed graph");

    bodies.flags[0] |= RB_SLEEPING;
    const IslandDispatchSleepPreflight allSleeping = preflight_island_dispatch_sleep(graph, bodies, dt);
    expectTrue(allSleeping.skipped, "dispatch-sleep preflight skips all-sleeping graph");
    expectTrue(!allSleeping.can_dispatch(), "dispatch-sleep preflight cannot dispatch all-sleeping graph");
    expectTrue(should_skip_island_dispatch_sleep(graph, bodies, dt),
               "should_skip dispatch-sleep true for all-sleeping graph");
}

void testDispatchAllSolveableIslandsResult() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const IslandSolveableStats stats =
        compute_island_solveable_stats(graph, bodies, work.contactManifolds(), constraints);
    expectTrue(stats.solveableCount == 1u, "solveable stats count mixed island only");
    expectTrue(stats.allSleepingCount == 1u, "solveable stats count all-sleeping island");
    expectTrue(collect_solveable_island_indices(graph, bodies, work.contactManifolds(), constraints).size() ==
                   1u,
               "collect solveable indices returns mixed island only");

    const IslandSolveableGraphPreflight solveablePreflight =
        preflight_island_solveable_graph(graph, bodies, work.contactManifolds(), constraints);
    expectTrue(solveablePreflight.has_solveable(), "solveable graph preflight has one island");
    expectTrue(!should_skip_island_solveable_graph(graph, bodies, work.contactManifolds(), constraints),
               "should_skip solveable graph false for mixed graph");

    const IslandBatchDispatchResult batch = dispatch_all_solveable_islands_result(bodies,
                                                                                  graph,
                                                                                  work,
                                                                                  constraints,
                                                                                  dt,
                                                                                  0.f,
                                                                                  invMassFn);
    expectTrue(!batch.skipped, "solveable batch dispatch does not skip mixed graph");
    expectTrue(batch.solvedCount == 1u, "solveable batch dispatch solves mixed island only");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "solveable batch dispatch wakes sleepers first");

    for (u32 bodyIndex = 0; bodyIndex < bodies.count(); ++bodyIndex) {
        bodies.flags[bodyIndex] |= RB_SLEEPING;
    }
    const IslandBatchDispatchResult allSleepingBatch = dispatch_all_solveable_islands_result(
        bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(allSleepingBatch.skipped, "solveable batch dispatch skips all-sleeping graph");
    expectTrue(allSleepingBatch.solvedCount == 0u, "solveable batch dispatch solves nothing when all sleep");
    expectTrue(dispatch_all_solveable_islands_guarded(bodies, graph, work, constraints, dt, 0.f, invMassFn) ==
                   0u,
               "solveable guarded batch returns zero when all sleep");
}

void testContactIslandGraphSkipsOutOfRangeRefs() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    expectTrue(contact_bodies_in_range(contacts[0], 4u), "in-range contact passes body guard");
    expectTrue(!contact_bodies_in_range(contacts[1], 4u), "out-of-range contact fails body guard");
    expectTrue(distance_bodies_in_range(constraints[0], 4u), "in-range distance passes body guard");
    expectTrue(!distance_bodies_in_range(constraints[1], 4u), "out-of-range distance fails body guard");

    ContactIslandGraph graph;
    graph.build(4, contacts, constraints);
    expectTrue(graph.constrainedIslandCount() == 1u,
               "build skips out-of-range refs and keeps in-range constrained island");
    expectTrue(graph.island(graph.bodyIsland(0)).contactIndices.size() == 1u,
               "build assigns only in-range contact to island");
    expectTrue(graph.island(graph.bodyIsland(0)).distanceIndices.size() == 1u,
               "build assigns only in-range distance constraint to island");
}

void testIslandConstraintSolveRejectReasonGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({6.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    const ContactIslandGraph::Island& mixed = graph.island(mixedIsland);
    const ContactIslandGraph::Island& allSleeping = graph.island(sleepingIsland);

    expectTrue(island_constraint_solve_reject_reason_is(
                   mixed, bodies, contacts, constraints, IslandConstraintSolveRejectReason::None),
               "mixed island has no constraint-solve reject reason");
    expectTrue(island_constraint_solve_reject_reason_is(
                   allSleeping, bodies, contacts, constraints, IslandConstraintSolveRejectReason::AllSleeping),
               "all-sleeping island rejects with AllSleeping");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 1};
    staleIsland.distanceIndices = {9u};
    expectTrue(island_constraint_solve_reject_reason_is(
                   staleIsland, bodies, contacts, constraints, IslandConstraintSolveRejectReason::StaleRefs),
               "stale refs reject with StaleRefs");

    ContactIslandGraph::Island emptyIsland{};
    expectTrue(island_constraint_solve_reject_reason_is(
                   emptyIsland, bodies, contacts, constraints, IslandConstraintSolveRejectReason::EmptyIsland),
               "empty island rejects with EmptyIsland");
    expectTrue(std::strcmp(island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason::AllSleeping),
                           "AllSleeping") == 0,
               "reject reason name for AllSleeping");

    const IslandConstraintSolvePreflight byIndex =
        preflight_island_constraint_solve_by_index(graph, mixedIsland, bodies, contacts, constraints);
    expectTrue(byIndex.can_solve(), "constraint solve by-index preflight allows mixed island");
    expectTrue(preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, bodies, contacts, constraints)
                   .skipped,
               "constraint solve by-index preflight skips out-of-range island");
}

void testSolveIslandJobGuardedSkipsAllSleeping() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({6.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        return bodySoA.invMasses[index];
    };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);
    expectTrue(solve_island_job_guarded(bodies,
                                        graph.island(mixedIsland),
                                        work,
                                        constraints,
                                        dt,
                                        0.f,
                                        invMassFn),
               "guarded solve runs mixed island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(sleepingIsland),
                                         work,
                                         constraints,
                                         dt,
                                         0.f,
                                         invMassFn),
               "guarded solve skips all-sleeping island");
}

void testDispatchSolveIslandWithWakeGuarded() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({6.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        return bodySoA.invMasses[index];
    };
    const f32 dt = 1.f / 60.f;

    const u32 mixedIsland = graph.bodyIsland(0);
    const IslandWakeThenSolvePreflight preflight =
        preflight_wake_then_solve_island(graph.island(mixedIsland), bodies, contacts, constraints);
    expectTrue(preflight.can_wake_then_solve(), "wake-then-solve preflight allows mixed island");
    expectTrue(!should_skip_wake_then_solve_island(graph.island(mixedIsland), bodies, contacts, constraints),
               "should_skip wake-then-solve false for mixed island");

    expectTrue(dispatch_solve_island_with_wake_guarded(bodies,
                                                       graph,
                                                       mixedIsland,
                                                       work,
                                                       constraints,
                                                       dt,
                                                       0.f,
                                                       invMassFn),
               "dispatch with wake guarded solves mixed island");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "wake-then-solve dispatch clears sleeper flag");

    const u32 sleepingIsland = graph.bodyIsland(2);
    expectTrue(!dispatch_solve_island_with_wake_guarded(bodies,
                                                        graph,
                                                        sleepingIsland,
                                                        work,
                                                        constraints,
                                                        dt,
                                                        0.f,
                                                        invMassFn),
               "dispatch with wake guarded skips all-sleeping island");
    expectTrue(preflight_wake_then_solve_island_by_index(graph, graph.islandCount() + 2u, bodies, contacts, constraints)
                   .skipped,
               "wake-then-solve by-index preflight skips out-of-range island");
}

void testDispatchAllNonsleepingIslandsResult() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({6.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        return bodySoA.invMasses[index];
    };
    const f32 dt = 1.f / 60.f;

    const std::vector<u32> solveable = collect_solveable_island_indices(graph, bodies, contacts, constraints);
    expectTrue(solveable.size() == 1u, "collect solveable indices returns mixed island only");

    const IslandNonsleepingDispatchResult batch =
        dispatch_all_nonsleeping_islands_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.skipped, "nonsleeping batch does not skip mixed graph");
    expectTrue(batch.solveableCount == 1u, "nonsleeping batch records one solveable island");
    expectTrue(batch.solvedCount == 1u, "nonsleeping batch solves mixed island");
    expectTrue(batch.any_solved(), "nonsleeping batch reports solved island");
    expectTrue(dispatch_all_nonsleeping_islands(bodies, graph, work, constraints, dt, 0.f, invMassFn) ==
                   batch.solvedCount,
               "nonsleeping guarded count matches batch result");

    const IslandNonsleepingDispatchResult invalidDt =
        dispatch_all_nonsleeping_islands_result(bodies, graph, work, constraints, 0.f, 0.f, invMassFn);
    expectTrue(invalidDt.skipped, "nonsleeping batch skips invalid dt");
}

void testEarlyExitWhenResidualBelowTolerance() {
    CollisionShapeSoA shapes;
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.05f, 0.f, 0.f}, 1.f, 0);
    shapes.addShape(CollisionShapeType::Sphere, 0, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, 1, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 0, 1);
    solver.setDistanceConstraints({DistanceConstraint{
        .bodyA = 0,
        .bodyB = 1,
        .restLength = 2.f,
    }});

    SolverParams params;
    params.substeps = 1;
    params.iterations = 64;
    params.residualTolerance = 0.02f;
    params.gravity = {};
    params.broadphase.cellSize = 4.f;

    solver.step(bodies, shapes, params, 1.f / 60.f);
    expectTrue(solver.lastIterationCount() < params.iterations,
               "early-exit stub stops before max iterations when residual is below tolerance");
    expectTrue(solver.lastConstraintResidual() <= params.residualTolerance,
               "early-exit stub records residual at or below tolerance");
}

void testPreflightIslandBuildGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    contacts.back().bodyB = 99;

    const std::vector<DistanceConstraint> constraints = {
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 50, .restLength = 2.f},
    };

    expectTrue(!is_valid_island_build_body_count(0u), "zero body count is invalid for island build");
    expectTrue(is_valid_island_build_body_count(3u), "positive body count is valid for island build");
    expectTrue(should_skip_island_build(0u), "should_skip_island_build on zero bodies");
    expectTrue(!should_skip_island_build(3u), "should_skip false for non-zero body count");

    const IslandBuildPreflight zeroPreflight = preflight_island_build(0u, contacts, constraints);
    expectTrue(zeroPreflight.zeroBodies, "build preflight marks zero body count");
    expectTrue(zeroPreflight.skipped, "build preflight skips zero body count");
    expectTrue(!zeroPreflight.can_build(), "build preflight cannot build with zero bodies");

    const IslandBuildPreflight validPreflight = preflight_island_build(3u, contacts, constraints);
    expectTrue(!validPreflight.skipped, "build preflight does not skip valid body count");
    expectTrue(validPreflight.can_build(), "build preflight can build with valid body count");
    expectTrue(validPreflight.validContactCount == 1u, "build preflight counts valid contacts");
    expectTrue(validPreflight.invalidContactCount == 1u, "build preflight counts invalid contacts");
    expectTrue(validPreflight.validDistanceCount == 1u, "build preflight counts valid distance constraints");
    expectTrue(validPreflight.invalidDistanceCount == 1u,
               "build preflight counts invalid distance constraints");

    ContactIslandGraph graph;
    expectTrue(!build_island_graph_guarded(graph, 0u, contacts, constraints),
               "guarded build rejects zero body count");
    expectTrue(graph.islandCount() == 0u, "guarded build clears graph on failure");
    expectTrue(build_island_graph_guarded(graph, 3u, contacts, constraints),
               "guarded build succeeds for valid body count");
    expectTrue(graph.islandCount() > 0u, "guarded build populates graph");
    expectTrue(graph.build_guarded(0u, contacts, constraints) == false,
               "member build_guarded rejects zero body count");
}

void testPreflightIslandConstraintSolveGuards() {


    const IslandBuildPreflight validPreflight = preflight_island_build(2, contacts, constraints);
    expectTrue(validPreflight.can_build(), "valid island build preflight can build");
    expectTrue(validPreflight.inRangeContactCount == 1u, "valid preflight counts in-range contacts");
    expectTrue(validPreflight.inRangeDistanceCount == 1u, "valid preflight counts in-range distances");
    expectTrue(can_build_island_graph(2, contacts, constraints), "can_build true for valid inputs");
    expectTrue(!should_skip_island_build(2, contacts, constraints), "should_skip false for valid inputs");

    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(!zeroBodies.can_build(), "zero body count rejects build preflight");
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodyCount,
               "zero body count reports ZeroBodyCount reason");
    expectTrue(std::strcmp(islandBuildRejectReasonName(IslandBuildRejectReason::ZeroBodyCount), "ZeroBodyCount") == 0,
               "reject reason name matches ZeroBodyCount");

    contacts.back().bodyB = 5u;
    const IslandBuildPreflight outOfRangeContact = preflight_island_build(2, contacts, constraints);
    expectTrue(!outOfRangeContact.can_build(), "out-of-range contact rejects build preflight");
    expectTrue(outOfRangeContact.reason == IslandBuildRejectReason::OutOfRangeContactBodies,
               "out-of-range contact reports OutOfRangeContactBodies");

    constraints.back().bodyB = 9u;
    const IslandBuildPreflight outOfRangeDistance = preflight_island_build(2, contacts, constraints);
    expectTrue(!outOfRangeDistance.can_build(), "out-of-range distance rejects build preflight");
    expectTrue(outOfRangeDistance.reason == IslandBuildRejectReason::OutOfRangeDistanceBodies,
               "out-of-range distance reports OutOfRangeDistanceBodies");

void testPreflightIslandBodyRefsGuards() {
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    const ContactIslandGraph::Island& island = graph.island(0);
    const IslandBodyRefsPreflight preflight = preflight_island_body_refs(island, bodies);
    expectTrue(!preflight.skipped, "body refs preflight does not skip constrained island");
    expectTrue(preflight.inRangeBodyCount == 2u, "body refs preflight counts in-range bodies");
    expectTrue(preflight.dynamicBodyCount == 2u, "body refs preflight counts awake dynamic bodies");
    expectTrue(preflight.can_solve(), "body refs preflight can solve valid island");
    expectTrue(!should_skip_island_body_refs(island, bodies), "should_skip false for valid body refs");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 9u};
    staleIsland.distanceIndices = {0u};
    const IslandBodyRefsPreflight stalePreflight = preflight_island_body_refs(staleIsland, bodies);
    expectTrue(!stalePreflight.can_solve(), "stale body refs preflight cannot solve");
    expectTrue(should_skip_island_body_refs(staleIsland, bodies), "should_skip true for stale body refs");

void testPreflightIslandSolveRefsCombined() {

    contacts.back().valid = false;
    contacts.back().bodyA = 8;
    contacts.back().bodyB = 9;

        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},

    const IslandBuildPreflight preflight = preflight_island_build(3, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip usable constraints");
    expectTrue(preflight.bodyCount == 3u, "build preflight records body count");
    expectTrue(preflight.validContactCount == 1u, "build preflight counts valid in-range contacts");
    expectTrue(preflight.staleContactCount == 1u, "build preflight counts stale contacts");
    expectTrue(preflight.validDistanceCount == 1u, "build preflight counts valid distance constraints");
    expectTrue(preflight.staleDistanceCount == 1u, "build preflight counts stale distance constraints");
    expectTrue(preflight.can_build(), "usable constraints allow build");
    expectTrue(has_usable_island_build_constraints(3, contacts, constraints),
               "has_usable_island_build_constraints true when refs exist");
    expectTrue(!should_skip_island_build(3, contacts, constraints),
               "should_skip false when build preflight can build");

    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodies,
               "zero-body build preflight records zero-body reason");
    expectTrue(!zeroBodies.can_build(), "zero-body build with constraints is skipped");

    std::vector<narrowphase::ContactManifold> staleContacts;
    staleContacts.push_back(narrowphase::ContactManifold{});
    staleContacts.back().valid = true;
    staleContacts.back().bodyA = 4;
    staleContacts.back().bodyB = 5;
    const std::vector<DistanceConstraint> staleConstraints = {
        DistanceConstraint{.bodyA = 6, .bodyB = 7, .restLength = 2.f},
    const IslandBuildPreflight allStale = preflight_island_build(2, staleContacts, staleConstraints);
    expectTrue(allStale.reason == IslandBuildRejectReason::AllConstraintsStale,
               "all-stale build preflight records stale reason");
    expectTrue(allStale.skipped, "all-stale build preflight is skipped");
    expectTrue(should_skip_island_build(2, staleContacts, staleConstraints),
               "should_skip true when all constraint refs are stale");
    expectTrue(!has_usable_island_build_constraints(2, staleContacts, staleConstraints),
               "has_usable false when all refs are stale");

    graph.build(3, contacts, constraints);
    expectTrue(graph.constrainedIslandCount() >= 1u,
               "existing build path still partitions usable constraints");

void testPreflightIslandSleepWakeGuards() {
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);

    contacts.back().bodyA = 2;
    contacts.back().bodyB = 99u;


    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip positive body count");
    expectTrue(preflight.can_build(), "build preflight can build with in-range constraints");
    expectTrue(preflight.inRangeContactCount == 1u, "build preflight counts in-range contacts");
    expectTrue(preflight.outOfRangeContactCount == 1u, "build preflight counts out-of-range contacts");
    expectTrue(preflight.inRangeDistanceCount == 1u, "build preflight counts in-range distance constraints");
    expectTrue(preflight.outOfRangeDistanceCount == 1u,
               "build preflight counts out-of-range distance constraints");
    expectTrue(preflight.has_in_range_constraints(), "build preflight sees partitionable constraints");
    expectTrue(contact_bodies_in_range(4, 0, 1), "contact_bodies_in_range accepts in-range pair");
    expectTrue(!contact_bodies_in_range(4, 2, 99u), "contact_bodies_in_range rejects out-of-range pair");
    expectTrue(distance_constraint_bodies_in_range(4, constraints[0]),
               "distance_constraint_bodies_in_range accepts in-range constraint");
    expectTrue(!distance_constraint_bodies_in_range(4, constraints[1]),
               "distance_constraint_bodies_in_range rejects out-of-range constraint");

    expectTrue(zeroBodies.skipped, "build preflight skips zero body count");
    expectTrue(!zeroBodies.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(should_skip_island_build(0, contacts, constraints),
               "should_skip_island_build true for zero bodies");

void testBuildGuardedMatchesBuildOnValidPath() {
        DistanceConstraint{.bodyA = 2, .bodyB = 88, .restLength = 2.f},

    expectTrue(is_contact_valid_for_island_build(contacts[0], 4u),
               "valid contact passes island build guard");
    expectTrue(!is_contact_valid_for_island_build(contacts[1], 4u),
               "stale contact fails island build guard");
    expectTrue(is_distance_constraint_valid_for_island_build(constraints[0], 4u),
               "valid distance constraint passes island build guard");
    expectTrue(!is_distance_constraint_valid_for_island_build(constraints[1], 4u),
               "stale distance constraint fails island build guard");

    const IslandBuildPreflight preflight = preflight_island_build(4u, contacts, constraints);
    expectTrue(!preflight.skipped, "build preflight does not skip non-empty inputs");
    expectTrue(preflight.can_build(), "build preflight can build with valid edges");
    expectTrue(preflight.validContactCount == 1u, "build preflight counts valid contacts");

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0u, {}, {});
    expectTrue(emptyPreflight.skipped, "build preflight skips empty no-op inputs");
    expectTrue(!emptyPreflight.can_build(), "build preflight cannot build empty no-op inputs");
    expectTrue(should_skip_island_build(0u, {}, {}), "should_skip build true for empty no-op inputs");

void testBuildGuardedAndBuildStats() {
    graph.build(2, {}, {});

    expectTrue(!graph.build_guarded(0u, {}, {}), "build_guarded skips empty no-op inputs");
    expectTrue(graph.islandCount() == 2u, "build_guarded leaves prior graph unchanged on skip");
    expectTrue(is_valid_island_build_body_count(0u), "zero body count is valid for build preflight");
    expectTrue(is_valid_island_build_body_count(4u), "positive body count is valid for build preflight");

    const IslandBuildPreflight emptyPreflight = preflight_island_build(0, {}, {});
    expectTrue(emptyPreflight.skipped, "build preflight skips completely empty inputs");
    expectTrue(should_skip_island_build(0, {}, {}), "should_skip build on empty inputs");

    expectTrue(!build_guarded(graph, 0, {}, {}), "build_guarded returns false for skipped inputs");
    expectTrue(graph.islandCount() == 0u, "build_guarded clears graph on skipped build");

    contacts.back().bodyA = 9;
    contacts.back().bodyB = 10;

    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(4, contacts, constraints);

    const u32 constrainedIndex = graph.bodyIsland(0);
    const ContactIslandGraph::Island& island = graph.island(constrainedIndex);
    const f32 dt = 1.f / 60.f;

    const IslandConstraintSolvePreflight validPreflight =
        preflight_island_constraint_solve(island, 2u, 2u, dt);
    expectTrue(!validPreflight.skipped, "constraint preflight does not skip constrained island");
    expectTrue(!validPreflight.invalidDt, "constraint preflight accepts valid dt");
    expectTrue(validPreflight.resolvableConstraintCount == 3u,
               "constraint preflight counts resolvable constraints");
    expectTrue(validPreflight.can_solve(), "constrained island can solve");

    const IslandConstraintSolvePreflight invalidDtPreflight =
        preflight_island_constraint_solve(island, 2u, 2u, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "constraint preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_solve(), "constraint preflight cannot solve with invalid dt");

    const IslandConstraintSolvePreflight outOfRangeContactPreflight =
        preflight_island_constraint_solve(island, 1u, 2u, dt);
    expectTrue(outOfRangeContactPreflight.outOfRangeContactCount == 1u,
               "constraint preflight counts out-of-range contacts");
    expectTrue(outOfRangeContactPreflight.outOfRangeDistanceCount == 0u,
               "constraint preflight keeps in-range distance constraints");
    expectTrue(outOfRangeContactPreflight.resolvableConstraintCount == 2u,
               "constraint preflight counts in-range contacts and distance");

    const u32 remoteIsland = graph.bodyIsland(2);
    const IslandConstraintSolvePreflight outOfRangeDistancePreflight =
        preflight_island_constraint_solve(graph.island(remoteIsland), 2u, 1u, dt);
    expectTrue(outOfRangeDistancePreflight.outOfRangeDistanceCount == 1u,
               "constraint preflight counts out-of-range distance constraints");
    expectTrue(outOfRangeDistancePreflight.resolvableConstraintCount == 0u,
               "constraint preflight reports zero resolvable when distance is out of range");

    const IslandConstraintSolvePreflight indexPreflight =
        preflight_island_constraint_solve_by_index(graph, constrainedIndex, 2u, 2u, dt);
    expectTrue(indexPreflight.can_solve(), "index constraint preflight can solve constrained island");
    expectTrue(!preflight_island_constraint_solve_by_index(graph, graph.islandCount() + 1u, 2u, 2u, dt)
                    .can_solve(),
               "index constraint preflight skips out-of-range island");

void testPreflightIslandSleepWakeGuards() {
    RigidBodySoA bodies;
    const u32 awakeA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 awakeB = bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleepingA = bodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingB = bodies.addBody({12.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 loneSleeping = bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(!preflight.zeroBodies, "non-zero body count is not zeroBodies");
    expectTrue(preflight.can_build(), "positive body count can build");
    expectTrue(preflight.stats.validContactCount == 1u, "build preflight counts valid contacts");
    expectTrue(preflight.stats.skippedInvalidContactCount == 1u,
               "build preflight counts invalid contact indices");
    expectTrue(preflight.stats.validDistanceCount == 1u, "build preflight counts valid distance constraints");
    expectTrue(preflight.stats.skippedInvalidDistanceCount == 1u,
               "build preflight counts invalid distance indices");
    expectTrue(!should_skip_island_build(4), "should not skip build with positive body count");

    const IslandBuildPreflight zeroPreflight = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroPreflight.zeroBodies, "zero body count flagged");
    expectTrue(zeroPreflight.skipped, "zero body build preflight skipped");
    expectTrue(!zeroPreflight.can_build(), "zero body count cannot build");
    expectTrue(should_skip_island_build(0), "should_skip_island_build on zero bodies");

void testBuildIslandGraphGuarded() {
}

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},

    expectTrue(!build_island_graph_guarded(graph, 0, contacts, constraints),
               "guarded build returns false for zero bodies");
    expectTrue(graph.islandCount() == 0u, "guarded build clears graph on skip");

    expectTrue(build_island_graph_guarded(graph, 3, contacts, constraints),
               "guarded build succeeds for valid inputs");
    expectTrue(graph.constrainedIslandCount() == 1u, "guarded build produces constrained island");
    expectTrue(is_valid_island_build_body_count(3), "body count guard accepts positive count");
    expectTrue(!is_valid_island_build_body_count(0), "body count guard rejects zero");

void testIslandSleepSolvePreflight() {

    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);

        DistanceConstraint{.bodyA = awakeA, .bodyB = awakeB, .restLength = 2.f},
        DistanceConstraint{.bodyA = sleepingA, .bodyB = sleepingB, .restLength = 2.f},
    graph.build(5, contacts, constraints);

    expectTrue(is_body_sleeping(bodies, sleepingA), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_sleeping(bodies, awakeA), "is_body_sleeping false for awake body");
    expectTrue(!is_body_static_or_kinematic(bodies, awakeA),
               "is_body_static_or_kinematic false for dynamic body");

    const u32 awakeIsland = graph.bodyIsland(awakeA);
    const u32 sleepingIsland = graph.bodyIsland(sleepingA);
    const IslandSleepPreflight awakeSleepPreflight =
        preflight_island_sleep(bodies, graph.island(awakeIsland));
    expectTrue(!awakeSleepPreflight.allSleeping, "awake island is not all-sleeping");
    expectTrue(!awakeSleepPreflight.can_skip_solve(), "awake island cannot skip solve for sleep");
    expectTrue(island_has_awake_body(bodies, graph.island(awakeIsland)),
               "awake island has awake body");
    expectTrue(island_needs_wake(bodies, graph.island(awakeIsland)),
               "awake constrained island needs wake stub");

    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep(bodies, graph.island(sleepingIsland));
    expectTrue(sleepingPreflight.allSleeping, "all-sleeping island flagged");
    expectTrue(sleepingPreflight.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(sleepingIsland)),
               "should_skip_island_solve_for_sleep on all-sleeping island");
    expectTrue(!island_needs_wake(bodies, graph.island(sleepingIsland)),
               "all-sleeping island does not need wake");

    const IslandWakePreflight awakeWakePreflight =
        preflight_island_wake(bodies, graph.island(awakeIsland));
    expectTrue(awakeWakePreflight.should_wake(), "awake constrained island should wake");
    expectTrue(awakeWakePreflight.awakeDynamicCount == 2u,
               "awake island counts awake dynamic bodies");

    const IslandSleepStats stats = compute_island_sleep_stats(bodies, graph);
    expectTrue(stats.allSleepingCount == 1u, "sleep stats count all-sleeping constrained island");
    expectTrue(stats.wakeRequiredCount == 1u, "sleep stats count wake-required island");
    expectTrue(count_all_sleeping_islands(bodies, graph) == 1u,
               "count_all_sleeping_islands matches stats");

    bool foundLoneEmpty = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (island.isEmpty() && island.bodyIndices.size() == 1u &&
            island.bodyIndices[0] == loneSleeping) {
            foundLoneEmpty = true;
            expectTrue(!should_skip_island_solve_for_sleep(bodies, island),
                       "empty island is not sleep-skipped");
    expectTrue(foundLoneEmpty, "graph exposes lone sleeping body empty island");

    const IslandSolveSleepPreflight solvePreflight = preflight_island_solve_sleep_by_index(
        bodies, graph, awakeIsland, 0u, static_cast<u32>(constraints.size()), 1.f / 60.f);
    expectTrue(solvePreflight.can_solve(), "combined sleep preflight can solve awake island");

    const IslandSolveSleepPreflight sleepingSolvePreflight = preflight_island_solve_sleep_by_index(
        bodies, graph, sleepingIsland, 0u, static_cast<u32>(constraints.size()), 1.f / 60.f);
    expectTrue(!sleepingSolvePreflight.can_solve(), "combined sleep preflight skips all-sleeping island");

void testDispatchAllIslandsSleepGuarded() {
    const u32 awakeB = bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    const u32 sleepingA = bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 sleepingB = bodies.addBody({22.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    graph.build(3, contacts, constraints);

    const u32 sleepingIsland = graph.bodyIsland(0);
    const IslandSleepSolvePreflight sleepingPreflight =
        preflight_island_sleep_solve(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.stats.sleepingCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepingPreflight.stats.awakeDynamicCount == 0u, "sleep preflight sees no awake dynamics");
    expectTrue(sleepingPreflight.allDynamicSleeping, "all dynamic bodies marked sleeping");
    expectTrue(!sleepingPreflight.can_solve(), "sleeping island cannot solve");
    expectTrue(should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip_sleeping_island_solve on all-sleeping island");

    const u32 awakeIsland = graph.bodyIsland(2);
    const IslandSleepSolvePreflight awakePreflight =
        preflight_island_sleep_solve(graph.island(awakeIsland), bodies);
    expectTrue(awakePreflight.skipped, "sleep preflight skips empty island");
    expectTrue(should_skip_sleeping_island_solve_index(graph, graph.islandCount() + 1u, bodies),
               "sleep preflight index guard skips out-of-range island");

void testIslandWakePreflight() {
    graph.build(2, contacts, constraints);

    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[0] = {0.f, 0.f, 0.f};
    bodies.linearVelocities[1] = {0.5f, 0.f, 0.f};


    const ContactIslandGraph::Island& island = graph.island(0);
    const IslandSolveRefsPreflight preflight =
        preflight_island_solve_refs(island, bodies, contacts, constraints);
    expectTrue(!preflight.skipped, "combined refs preflight does not skip valid island");
    expectTrue(preflight.can_solve(), "combined refs preflight can solve valid island");
    expectTrue(!should_skip_island_solve_refs(island, bodies, contacts, constraints),
               "should_skip false for valid combined refs");

void testPreflightIslandSleepGuards() {
    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(sleepingPreflight.all_dynamic_sleeping(),
               "all-dynamic-sleeping island reports all_dynamic_sleeping");
    expectTrue(!sleepingPreflight.can_attempt_sleep(),
               "all-sleeping island cannot attempt sleep again");
    expectTrue(should_skip_island_solve_all_sleeping(graph.island(sleepingIsland), bodies),
               "should_skip all-sleeping island solve");

    const IslandSleepPreflight awakePreflight = preflight_island_sleep(graph.island(awakeIsland), bodies);
    expectTrue(awakePreflight.can_attempt_sleep(), "awake island can attempt sleep");
    expectTrue(!awakePreflight.all_dynamic_sleeping(), "awake island is not all sleeping");
    expectTrue(!should_skip_island_solve_all_sleeping(graph.island(awakeIsland), bodies),
               "should_skip false for awake island");

    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(sleepingIsland), bodies);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakePreflight.should_wake(), "sleeping island with constraints is wake candidate");
    expectTrue(wakePreflight.ownedConstraintCount == 1u, "wake preflight counts owned constraints");

    const IslandWakePreflight awakeWakePreflight = preflight_island_wake(graph.island(awakeIsland), bodies);
    expectTrue(!awakeWakePreflight.should_wake(), "all-awake island is not a wake candidate");
    expectTrue(awakeWakePreflight.awakeBodyCount == 2u, "wake preflight counts awake bodies");
    expectTrue(awakeWakePreflight.sleepingBodyCount == 0u, "awake island has no sleeping bodies");

    const IslandSleepPreflight outOfRange =
        preflight_island_sleep_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "sleep index preflight skips out-of-range island");
    const IslandWakePreflight outOfRangeWake =
        preflight_island_wake_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRangeWake.skipped, "wake index preflight skips out-of-range island");

void testPreflightIslandSolveParticipationGuards() {
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_STATIC);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);


    ContactIslandGraph directGraph;
    directGraph.build(4, contacts, constraints);

    ContactIslandGraph guardedGraph;
    expectTrue(guardedGraph.build_guarded(4, contacts, constraints),
               "build_guarded succeeds on valid inputs");
    expectTrue(guardedGraph.islandCount() == directGraph.islandCount(),
               "build_guarded island count matches build");
    expectTrue(guardedGraph.constrainedIslandCount() == directGraph.constrainedIslandCount(),
               "build_guarded constrained count matches build");

    ContactIslandGraph skippedGraph;
    expectTrue(!skippedGraph.build_guarded(0, contacts, constraints),
               "build_guarded returns false for skipped preflight");
    expectTrue(skippedGraph.islandCount() == 0u, "build_guarded clears graph when skipped");

void testPreflightIslandBodyRefsGuards() {

    const IslandWakePreflight preflight =
        preflight_island_wake(graph.island(0), bodies, 0.01f, 0.01f);
    expectTrue(!preflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(preflight.eligibleDynamicCount == 2u, "wake preflight counts dynamic bodies");
    expectTrue(preflight.belowThresholdCount == 1u, "wake preflight counts below-threshold body");
    expectTrue(preflight.aboveThresholdCount == 1u, "wake preflight counts above-threshold body");
    expectTrue(preflight.can_enter_sleep(), "wake preflight can enter sleep for resting body");
    expectTrue(preflight.needs_wake(), "wake preflight needs wake for moving body");
    expectTrue(!should_skip_island_sleep_detection(graph.island(0), bodies),
               "sleep detection runs when dynamic bodies exist");

    RigidBodySoA staticBodies;
    staticBodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    staticBodies.addBody({0.f, 1.f, 0.f}, 0.f, RB_STATIC);
    expectTrue(should_skip_island_sleep_detection(graph.island(0), staticBodies),
               "sleep detection skips static-only island");

void testSolveIslandJobPreflightGuards() {

    RigidBodySoA awakeBodies;
    awakeBodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    awakeBodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    const IslandSleepPreflight awakePreflight = preflight_island_sleep(island, awakeBodies);
    expectTrue(!awakePreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(awakePreflight.dynamicAwakeCount == 2u, "sleep preflight counts awake dynamic bodies");
    expectTrue(!awakePreflight.all_dynamic_sleeping(), "awake island is not fully sleeping");
    expectTrue(!should_skip_island_solve_for_sleep(island, awakeBodies),
               "should_skip sleep false for awake island");

    RigidBodySoA sleepingBodies;
    sleepingBodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    sleepingBodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(island, sleepingBodies);
    expectTrue(sleepPreflight.all_dynamic_sleeping(), "all dynamic bodies sleeping");
    expectTrue(sleepPreflight.can_skip_solve(), "sleep preflight can skip solve");
    expectTrue(should_skip_island_solve_for_sleep(island, sleepingBodies),
               "should_skip sleep true for fully sleeping island");

    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, sleepingBodies);
    expectTrue(graphPreflight.stats.fullySleepingCount == 1u,
               "graph sleep preflight counts fully sleeping island");
    expectTrue(graphPreflight.all_fully_sleeping(), "graph reports all islands fully sleeping");

    const std::vector<u32> sleepingIndices = collect_fully_sleeping_island_indices(graph, sleepingBodies);
    expectTrue(sleepingIndices.size() == 1u, "collect returns fully sleeping island index");
    expectTrue(preflight_island_sleep_by_index(graph, graph.islandCount() + 1u, sleepingBodies).skipped,
               "index sleep preflight skips out-of-range island");

void testPreflightIslandWakeGuards() {
    expectTrue(graph.build_guarded(4u, contacts, constraints),
               "build_guarded builds constrained graph");
    expectTrue(graph.constrainedIslandCount() == 2u, "build_guarded partitions constrained islands");

    const IslandBuildStats stats = compute_island_build_stats(graph);
    expectTrue(stats.totalIslands == graph.islandCount(), "build stats count total islands");
    expectTrue(stats.constrainedCount == graph.constrainedIslandCount(),
               "build stats count constrained islands");
    expectTrue(stats.emptyCount + stats.constrainedCount == stats.totalIslands,
               "build stats partition all islands");

    bodies.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({3.f, 0.f, 0.f}, 0.f, RB_STATIC);

        DistanceConstraint{.bodyA = 8, .bodyB = 9, .restLength = 2.f},

    expectTrue(!preflight.skipped, "build preflight does not skip non-empty inputs");
    expectTrue(preflight.can_build(), "build preflight can build with valid body count");
    expectTrue(preflight.contactCount == 3u, "build preflight counts contacts");
    expectTrue(preflight.inRangeContactCount == 2u, "build preflight counts in-range contacts");
    expectTrue(preflight.validInRangeContactCount == 1u, "build preflight counts valid in-range contacts");
    expectTrue(preflight.invalidContactCount == 1u, "build preflight counts invalid contacts");
    expectTrue(preflight.outOfRangeContactCount == 1u, "build preflight counts out-of-range contacts");
    expectTrue(preflight.inRangeDistanceCount == 2u, "build preflight counts in-range distance constraints");
    expectTrue(preflight.outOfRangeDistanceCount == 1u, "build preflight counts out-of-range distance constraints");
    expectTrue(preflight.has_buildable_constraints(), "build preflight sees buildable constraints");

    expectTrue(build_guarded(graph, 4, contacts, constraints), "build_guarded succeeds for valid inputs");
    const IslandBuildStats stats = compute_island_build_stats(graph, preflight);
    expectTrue(stats.totalIslands == graph.islandCount(), "build stats report total islands");
               "build stats constrained count matches graph");
    expectTrue(stats.orphanContactCount == preflight.outOfRangeContactCount + preflight.invalidContactCount,
               "build stats orphan contacts match input preflight");
    expectTrue(stats.orphanDistanceCount == preflight.outOfRangeDistanceCount,
               "build stats orphan distance constraints match input preflight");

    bodies.addBody({4.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({6.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({8.f, 0.f, 0.f}, 0.f, RB_STATIC);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().addPoint({0.f, 0.f, 0.f}, 0.05f);

    graph.build(2, contacts, {});

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.linearVelocities[0] = {0.5f, 0.f, 0.f};

    const f32 linearThreshold = 0.01f;
    const f32 angularThreshold = 0.01f;
    const IslandWakePreflight wakePreflight =
        preflight_island_wake(island, bodies, contacts, linearThreshold, angularThreshold);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip contact island");
    expectTrue(wakePreflight.wakeCandidateCount >= 1u, "wake preflight counts moving body");
    expectTrue(wakePreflight.penetratingContactCount >= 1u, "wake preflight counts penetrating contact");
    expectTrue(wakePreflight.should_wake(), "wake preflight should wake island");
    expectTrue(should_wake_island(island, bodies, contacts, linearThreshold, angularThreshold),
               "should_wake true for moving penetrating island");

    bodies.linearVelocities[0] = {};
    bodies.forces[1] = {0.f, 10.f, 0.f};
    const IslandWakePreflight forcePreflight =
    expectTrue(forcePreflight.externalForceCount >= 1u, "wake preflight counts external force");
    expectTrue(forcePreflight.should_wake(), "external force triggers wake preflight");

    const IslandWakeGraphPreflight graphPreflight =
        preflight_island_wake_graph(graph, bodies, contacts, linearThreshold, angularThreshold);
    expectTrue(graphPreflight.any_should_wake(), "graph wake preflight finds wakeable island");
    expectTrue(collect_wakeable_island_indices(graph, bodies, contacts, linearThreshold, angularThreshold).size() >= 1u,
               "collect wakeable indices finds island");
    expectTrue(preflight_island_wake_by_index(graph, graph.islandCount() + 1u, bodies, contacts, linearThreshold,
                                              angularThreshold)
                   .skipped,
               "index wake preflight skips out-of-range island");

void testSolveIslandJobSkipsFullySleepingIsland() {
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},

    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.predictedPositions = bodies.positions;

        DistanceConstraint{.bodyA = awakeA, .bodyB = awakeB, .restLength = 2.f},
        DistanceConstraint{.bodyA = sleepingA, .bodyB = sleepingB, .restLength = 2.f},
    graph.build(4, contacts, constraints);

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };
    const f32 dt = 1.f / 60.f;

    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(bodies, graph, dt);
    expectTrue(preflight.can_dispatch(), "sleep dispatch preflight can dispatch mixed graph");
    expectTrue(preflight.solvableCount == 1u, "sleep dispatch preflight counts solvable island");
    expectTrue(!should_skip_island_dispatch_sleep(bodies, graph, dt),
               "should_skip false for graph with solvable island");
    expectTrue(has_solvable_islands(bodies, graph), "has_solvable_islands true for mixed graph");

    const std::vector<u32> solvable = collect_solvable_island_indices(bodies, graph);
    expectTrue(solvable.size() == 1u, "collect_solvable_island_indices skips all-sleeping island");

    const IslandBatchSleepDispatchResult batch =
        dispatch_all_islands_sleep_result(bodies, graph, work, constraints, dt, 0.f, invMassFn);
    expectTrue(!batch.dispatch.skipped, "sleep batch dispatch does not skip mixed graph");
    expectTrue(batch.dispatch.solvedCount == 1u, "sleep batch dispatch solves awake island only");
    expectTrue(batch.sleepSkippedCount == 0u,
               "sleep batch dispatch does not count awake island as sleep-skipped");
    expectTrue(batch.wakeRequiredCount == 1u, "sleep batch records wake-required island");

    const u32 awakeIsland = graph.bodyIsland(awakeA);
    const IslandDispatchResult awakeResult = dispatch_solve_island_sleep_result(
        bodies, graph, awakeIsland, work, constraints, dt, 0.f, invMassFn);
    expectTrue(awakeResult.solved, "sleep-guarded dispatch solves awake island");

    const u32 sleepingIsland = graph.bodyIsland(sleepingA);
    const IslandDispatchResult sleepingResult = dispatch_solve_island_sleep_result(
        bodies, graph, sleepingIsland, work, constraints, dt, 0.f, invMassFn);
    expectTrue(sleepingResult.skipped, "sleep-guarded dispatch skips all-sleeping island");
    expectTrue(!sleepingResult.solved, "sleep-guarded dispatch does not solve all-sleeping island");
    expectTrue(!dispatch_solve_island_sleep_guarded(
                   bodies, graph, sleepingIsland, work, constraints, dt, 0.f, invMassFn),
               "sleep-guarded dispatch returns false for all-sleeping island");

    work.init(2, 0, 1);


    const IslandSolveJobPreflight invalidDt =
        preflight_solve_island_job(graph.island(0), bodies, 0.f);
    expectTrue(invalidDt.invalidDt, "solve job preflight rejects zero dt");
    expectTrue(!invalidDt.can_solve(), "solve job preflight cannot solve with invalid dt");

    const IslandSolveJobPreflight sleepingPreflight =
        preflight_solve_island_job(graph.island(0), bodies, 1.f / 60.f);
    expectTrue(sleepingPreflight.sleep.allDynamicSleeping, "solve job preflight sees sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "solve job preflight blocks all-sleeping island");
    expectTrue(should_skip_solve_island_job_preflight(graph.island(0), bodies, 1.f / 60.f),
               "should_skip_solve_island_job_preflight on sleeping island");

    bodies.flags[0] = 0;
    bodies.flags[1] = 0;
    const IslandSolveJobPreflight awakePreflight =
    expectTrue(awakePreflight.can_solve(), "solve job preflight allows awake island");
    expectTrue(!solve_island_job_guarded(bodies,
                                         graph.island(0),
                                         work,
                                         constraints,
                                         0.f,
                                         invMassFn),
               "solve_island_job_guarded skips invalid dt");
    expectTrue(solve_island_job_guarded(bodies,
                                        1.f / 60.f,
               "solve_island_job_guarded solves awake constrained island");

    const IslandSolveJobResult outOfRange =
        solve_island_job_result(bodies, graph, graph.islandCount() + 2u, work, constraints, 1.f / 60.f, 0.f, invMassFn);
    expectTrue(outOfRange.skipped, "solve_island_job_result skips out-of-range index");
    expectTrue(!outOfRange.solved, "out-of-range solve result is not solved");
    const IslandSolveJob job = extract_island(graph, 0);

    expectTrue(!solve_island_job(bodies, *job.island, work, constraints, 1.f / 60.f, 0.f, invMassFn),
               "solve_island_job skips fully sleeping constrained island");
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},

    const u32 inactiveIsland = graph.bodyIsland(0);
    const IslandSolveParticipationPreflight inactivePreflight =
        preflight_island_solve_participation(graph.island(inactiveIsland), bodies);
    expectTrue(!inactivePreflight.can_solve(), "sleeping/static island has no participation");
    expectTrue(should_skip_island_solve_no_participation(graph.island(inactiveIsland), bodies),
               "should_skip no-participation island solve");
    expectTrue(inactivePreflight.sleepingBodyCount == 1u,
               "participation preflight counts sleeping bodies");
    expectTrue(inactivePreflight.staticOrKinematicBodyCount == 1u,
               "participation preflight counts static bodies");

    const u32 activeIsland = graph.bodyIsland(2);
    const IslandSolveParticipationPreflight activePreflight =
        preflight_island_solve_participation(graph.island(activeIsland), bodies);
    expectTrue(activePreflight.can_solve(), "awake dynamic island can participate in solve");
    expectTrue(!should_skip_island_solve_no_participation(graph.island(activeIsland), bodies),
               "should_skip false when island has participating bodies");
    expectTrue(activePreflight.participatingBodyCount == 2u,
               "participation preflight counts awake dynamic bodies");

void testIslandSleepWakeGraphGuards() {
    bodies.addBody({10.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({12.f, 0.f, 0.f}, 1.f, 0);


    const u32 sleepingIsland = graph.bodyIsland(0);
    const IslandSleepWakeGraphPreflight preflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!preflight.skipped, "sleep/wake graph preflight does not skip constrained graph");
    expectTrue(preflight.has_wake_candidates(), "graph has wake candidates");
    expectTrue(!preflight.all_islands_sleeping(), "not all islands are all-sleeping");
    expectTrue(preflight.stats.wakeCandidateCount == 1u,
               "wake candidate count matches sleeping constrained island");
    expectTrue(preflight.stats.allSleepingCount >= 1u,
               "graph stats count all-sleeping constrained island");
    expectTrue(!should_skip_island_wake_graph(graph, bodies),
               "should_skip wake graph false when candidates exist");

    const std::vector<u32> wakeIndices = collect_wake_candidate_island_indices(graph, bodies);
    expectTrue(wakeIndices.size() == 1u,
               "collect wake indices returns sleeping constrained island");
    expectTrue(wakeIndices.front() == sleepingIsland,
               "wake index points at sleeping constrained island");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(preflight_island_sleep_wake_graph(emptyGraph, bodies).skipped,
               "sleep/wake graph preflight skips empty graph");
    expectTrue(should_skip_island_wake_graph(emptyGraph, bodies),
               "should_skip wake graph true for empty graph");

void testIslandBodyFlagHelpers() {
    expectTrue(is_island_body_sleeping(RB_SLEEPING), "sleeping flag helper");
    expectTrue(!is_island_body_sleeping(0u), "awake flag helper");
    expectTrue(is_island_body_static_or_kinematic(RB_STATIC), "static flag helper");
    expectTrue(is_island_body_static_or_kinematic(RB_KINEMATIC), "kinematic flag helper");
    expectTrue(!is_island_body_static_or_kinematic(0u), "dynamic flag helper");
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, 0);

    const IslandBodyRefsPreflight preflight =
        preflight_island_body_refs(graph.island(0), bodies);
    expectTrue(!preflight.skipped, "body refs preflight does not skip constrained island");
    expectTrue(preflight.inRangeBodyCount == 2u, "body refs preflight counts in-range bodies");
    expectTrue(preflight.can_solve(), "body refs preflight can solve with in-range bodies");
    expectTrue(!should_skip_island_body_refs(graph.island(0), bodies),
               "should_skip false when body refs are in range");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 9u};
    staleIsland.distanceIndices = {0};
    const IslandBodyRefsPreflight stalePreflight = preflight_island_body_refs(staleIsland, bodies);
    expectTrue(!stalePreflight.can_solve(), "stale body refs preflight cannot solve");
    expectTrue(should_skip_island_body_refs(staleIsland, bodies),
               "should_skip true when island body refs are stale");

    expectTrue(!solve_island_job(bodies,
                                 staleIsland,
                                 [](const RigidBodySoA&, u32) { return 1.f; }),
               "solve_island_job skips island with stale body refs");

void testPreflightIslandSleepStateGuards() {
    };
    graph.build(4, {}, constraints);

    awakeBodies.addBody({10.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    awakeBodies.addBody({12.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const u32 awakeIsland = graph.bodyIsland(0);
    const u32 sleepingIsland = graph.bodyIsland(2);

    const IslandSleepPreflight awakePreflight =
        preflight_island_sleep_state(graph.island(awakeIsland), awakeBodies);
    expectTrue(awakePreflight.awakeDynamicCount == 2u, "sleep preflight counts awake dynamics");
    expectTrue(awakePreflight.can_solve_awake(), "awake island can solve");
    expectTrue(!should_skip_solve_sleeping_island(graph.island(awakeIsland), awakeBodies),
               "should_skip false for awake island");

    const IslandSleepPreflight sleepingPreflight =
        preflight_island_sleep_state(graph.island(sleepingIsland), awakeBodies);
    expectTrue(sleepingPreflight.all_dynamic_sleeping(), "all-dynamic-sleeping island flagged");
    expectTrue(!sleepingPreflight.can_solve_awake(), "all-sleeping island cannot solve awake");
    expectTrue(should_skip_solve_sleeping_island(graph.island(sleepingIsland), awakeBodies),
               "should_skip true for all-sleeping island");
    expectTrue(is_sleeping_body(awakeBodies, 2), "is_sleeping_body detects sleeping flag");
    expectTrue(is_awake_dynamic_body(awakeBodies, 0), "is_awake_dynamic_body detects awake dynamic");
    expectTrue(!is_awake_dynamic_body(awakeBodies, 2), "sleeping body is not awake dynamic");

    const IslandSleepPreflight outOfRange =
        preflight_island_sleep_state_by_index(graph, graph.islandCount() + 3u, awakeBodies);
    expectTrue(outOfRange.skipped, "index sleep preflight skips out-of-range island");

    graph.build(2, {}, constraints);

    RigidBodySoA mixedBodies;
    mixedBodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    mixedBodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

        preflight_island_wake(graph.island(0), mixedBodies);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakePreflight.needs_wake(), "mixed island needs wake");
    expectTrue(should_wake_island(graph.island(0), mixedBodies),
               "should_wake true for mixed sleep/awake island");

    RigidBodySoA allAwake;
    allAwake.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    allAwake.addBody({2.f, 0.f, 0.f}, 1.f, 0);
    expectTrue(!preflight_island_wake(graph.island(0), allAwake).needs_wake(),
               "all-awake island does not need wake");

void testPreflightIslandSleepGraphGuards() {
    graph.build(5, {}, constraints);

    bodies.addBody({20.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({22.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({40.f, 0.f, 0.f}, 1.f, 0);

    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(!preflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(preflight.can_dispatch_awake(), "graph sleep preflight can dispatch awake islands");
    expectTrue(preflight.stats.awakeCount == 1u, "graph sleep preflight counts awake islands");
    expectTrue(preflight.stats.allSleepingCount == 1u, "graph sleep preflight counts all-sleeping islands");
    expectTrue(has_awake_islands(graph, bodies), "has_awake_islands true for mixed graph");
    expectTrue(!should_skip_island_dispatch_for_sleep(graph, bodies),
               "should_skip false when awake islands exist");

    const std::vector<u32> awakeIndices = collect_awake_island_indices(graph, bodies);
    expectTrue(awakeIndices.size() == 1u, "collect_awake_island_indices returns awake count");

    RigidBodySoA allSleeping = bodies;
    allSleeping.flags[0] |= RB_SLEEPING;
    allSleeping.flags[1] |= RB_SLEEPING;
    const IslandSleepGraphPreflight allSleepPreflight = preflight_island_sleep_graph(graph, allSleeping);
    expectTrue(allSleepPreflight.skipped, "graph sleep preflight skips all-sleeping constrained graph");
    expectTrue(should_skip_island_dispatch_for_sleep(graph, allSleeping),
               "should_skip true when every constrained island is all-sleeping");

void testDispatchSolveIslandSleepGuarded() {

    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({22.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);


    const u32 awakeIndex = graph.bodyIsland(0);
    const u32 sleepingIndex = graph.bodyIsland(2);

    const IslandDispatchResult awakeResult = dispatch_solve_island_sleep_guarded_result(
        bodies, graph, awakeIndex, work, constraints, 1.f / 60.f, 0.f, invMassFn);
    expectTrue(!awakeResult.skipped, "sleep-guarded dispatch does not skip awake island");

    const IslandDispatchResult sleepingResult = dispatch_solve_island_sleep_guarded_result(
        bodies, graph, sleepingIndex, work, constraints, 1.f / 60.f, 0.f, invMassFn);
    expectTrue(!sleepingResult.solved, "sleep-guarded dispatch skips all-sleeping island");
    expectTrue(sleepingResult.skipped, "sleep-guarded dispatch marks all-sleeping island skipped");

    expectTrue(dispatch_solve_island_sleep_guarded(bodies,
                                                   graph,
                                                   awakeIndex,
               "sleep-guarded dispatch entry succeeds for awake island");
    expectTrue(!dispatch_solve_island_sleep_guarded(bodies,
                                                    sleepingIndex,
               "sleep-guarded dispatch entry skips all-sleeping island");

void testSolveIslandJobSleepGuarded() {

    sleepingBodies.addBody({2.1f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    sleepingBodies.predictedPositions = sleepingBodies.positions;

    RigidBodySoA awakeBodies = sleepingBodies;
    awakeBodies.flags[0] &= ~RB_SLEEPING;


    expectTrue(!solve_island_job_sleep_guarded(sleepingBodies,
               "sleep-guarded solve skips all-sleeping island");

    expectTrue(solve_island_job_sleep_guarded(awakeBodies,
               "sleep-guarded solve runs when island has awake dynamic");

    const IslandSolvePassPreflight passPreflight = preflight_island_solve_pass(
        graph.island(0), sleepingBodies, work.contactManifolds(), constraints);
    expectTrue(!passPreflight.can_solve(), "combined solve pass preflight rejects all-sleeping island");
    expectTrue(should_skip_island_solve_pass(
                   graph.island(0), sleepingBodies, work.contactManifolds(), constraints),
               "should_skip combined solve pass for all-sleeping island");
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

    graph.build(4, contacts, {});

    const u32 awakeIsland = graph.bodyIsland(2);

    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(bodies, graph.island(sleepingIsland));
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip contact island");
    expectTrue(sleepingPreflight.sleepingCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepingPreflight.awakeDynamicCount == 0u, "sleep preflight sees no awake dynamic bodies");
    expectTrue(sleepingPreflight.is_fully_sleeping(), "sleep preflight marks fully sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "fully sleeping island cannot solve");
    expectTrue(is_island_fully_sleeping(bodies, graph.island(sleepingIsland)),
               "is_island_fully_sleeping true for sleeping island");
    expectTrue(should_skip_solve_fully_sleeping_island(bodies, graph.island(sleepingIsland)),
               "should_skip fully sleeping island");

    const IslandSleepPreflight awakePreflight = preflight_island_sleep(bodies, graph.island(awakeIsland));
    expectTrue(awakePreflight.awakeDynamicCount == 1u, "sleep preflight counts awake dynamic body");
    expectTrue(awakePreflight.staticOrKinematicCount == 1u, "sleep preflight counts static body");
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(!awakePreflight.is_fully_sleeping(), "awake island is not fully sleeping");

    const IslandSleepStats stats = compute_island_sleep_stats(graph, bodies);
    expectTrue(stats.fullySleepingCount == 1u, "sleep stats count fully sleeping island");
    expectTrue(stats.solvableCount == 1u, "sleep stats count solvable island");
    expectTrue(has_solvable_sleep_islands(graph, bodies), "graph has solvable sleep islands");

    const IslandSleepPreflight outOfRange = preflight_island_sleep_by_index(graph, graph.islandCount() + 1u, bodies);
}

void testPreflightIslandWakeGuards() {
    bodies.addBody({1.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({3.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    ContactIslandGraph graph;

    const u32 wakeIsland = graph.bodyIsland(0);
    const u32 allSleepingIsland = graph.bodyIsland(2);

    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, graph.island(wakeIsland), contacts);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip mixed island");
    expectTrue(wakePreflight.ownedContactCount == 1u, "wake preflight counts owned contacts");
    expectTrue(wakePreflight.awakeParticipantCount == 1u, "wake preflight counts awake participant");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts wake candidate contact");
    expectTrue(wakePreflight.can_wake(), "mixed island can wake");
    expectTrue(should_wake_island_bodies(bodies, graph.island(wakeIsland), contacts),
               "should_wake true for mixed island");

    const IslandWakePreflight noWakePreflight =
        preflight_island_wake(bodies, graph.island(allSleepingIsland), contacts);
    expectTrue(!noWakePreflight.can_wake(), "all-sleeping island cannot wake from contacts");
    expectTrue(!should_wake_island_bodies(bodies, graph.island(allSleepingIsland), contacts),
               "should_wake false for all-sleeping island");

    expectTrue(is_body_sleeping(bodies, 0u), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_sleeping(bodies, 1u), "is_body_sleeping rejects awake body");
    expectTrue(is_body_static_or_kinematic(bodies, 3u) == false,
               "dynamic sleeping body is not static or kinematic");

    const u32 woken = wake_island_bodies_guarded(bodies, graph.island(wakeIsland));
    expectTrue(woken == 1u, "wake guarded wakes sleeping body in mixed island");
    expectTrue((bodies.flags[0] & RB_SLEEPING) == 0u, "wake guarded clears sleeping flag");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u, "awake body remains awake");

    bodies.flags[0] |= RB_SLEEPING;
    const u32 batchWoken = wake_all_islands_guarded(bodies, graph, contacts);
    expectTrue(batchWoken == 1u, "wake_all guarded wakes sleeping island with awake neighbor");

    const IslandWakeStats wakeStats = compute_island_wake_stats(graph, bodies, contacts);
    expectTrue(wakeStats.totalIslands == graph.islandCount(), "wake stats count total islands");

    const IslandWakePreflight outOfRange =
        preflight_island_wake_by_index(graph, graph.islandCount() + 1u, bodies, contacts);
    expectTrue(outOfRange.skipped, "index wake preflight skips out-of-range island");

void testSolveIslandJobGuardedSleepAndRefs() {


    const std::vector<DistanceConstraint> constraints = {

    graph.build(2, contacts, constraints);

    work.init(2, 1, 1);
    work.contactManifolds() = contacts;
    const auto invMassFn = [](const RigidBodySoA& bodySoA, u32 index) {
        if ((bodySoA.flags[index] & RB_SLEEPING) != 0u) {
            return 0.f;
        return bodySoA.invMasses[index];

    const IslandSolveSleepPreflight preflight =
        preflight_solve_island_with_sleep(bodies, graph.island(0), contacts, constraints);
    expectTrue(!preflight.skipped, "solve sleep preflight does not skip constrained island");
    expectTrue(preflight.refs.can_solve(), "solve sleep preflight sees in-range refs");
    expectTrue(!preflight.can_solve(), "fully sleeping island fails combined solve preflight");
    expectTrue(should_skip_solve_island_with_sleep(bodies, graph.island(0), contacts, constraints),
               "should_skip combined sleep/refs preflight");

    const bool guardedSolved = solve_island_job_guarded(bodies,
                                                       invMassFn);
    expectTrue(!guardedSolved, "solve_island_job_guarded skips fully sleeping island");

    bodies.flags[0] &= ~RB_SLEEPING;
    bodies.flags[1] &= ~RB_SLEEPING;
    const IslandSolveSleepPreflight awakePreflight =
    expectTrue(awakePreflight.can_solve(), "awake island passes combined solve preflight");
               "solve_island_job_guarded solves awake island");
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 3, .bodyB = 4, .restLength = 2.f},
    };

    graph.build(5, contacts, constraints);

    expectTrue(is_body_sleeping(bodies, 0), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_sleeping(bodies, 2), "is_body_sleeping false for awake body");
    expectTrue(is_body_static_or_kinematic(bodies, 4), "is_body_static_or_kinematic detects static body");

    const u32 sleepingIsland = graph.bodyIsland(0);
    expectTrue(sleepingIsland != ContactIslandGraph::invalidIsland, "sleeping pair maps to valid island");
    const IslandSleepWakePreflight sleepingPreflight =
        preflight_island_sleep_wake(graph.island(sleepingIsland), bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.allDynamicSleeping, "both-sleeping island flagged allDynamicSleeping");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(sleepingPreflight.can_sleep(), "all-sleeping island can sleep");
    expectTrue(should_skip_island_solve_for_sleep(graph.island(sleepingIsland), bodies),
               "should_skip sleep true for all-sleeping island");

    expectTrue(awakeIsland != ContactIslandGraph::invalidIsland, "awake pair maps to valid island");
    const IslandSleepWakePreflight awakePreflight =
        preflight_island_sleep_wake(graph.island(awakeIsland), bodies);
    expectTrue(!should_skip_island_solve_for_sleep(graph.island(awakeIsland), bodies),
               "should_skip sleep false for awake island");

    const IslandSleepWakePreflight outOfRange =
        preflight_island_sleep_wake_by_index(graph, graph.islandCount() + 1u, bodies);
    expectTrue(outOfRange.skipped, "index sleep preflight skips out-of-range island");
    expectTrue(should_skip_island_solve_for_sleep_index(graph, graph.islandCount() + 1u, bodies),
               "should_skip sleep index on out-of-range island");

    const IslandSleepWakeGraphPreflight graphPreflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!graphPreflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(graphPreflight.can_solve(), "graph sleep preflight can solve with awake island");
    expectTrue(graphPreflight.stats.solvableCount >= 1u, "graph sleep preflight counts solvable islands");
    expectTrue(has_solvable_islands(graph, bodies), "has_solvable_islands true with awake island");
    expectTrue(!should_skip_island_dispatch_for_sleep(graph, bodies),
               "should_skip dispatch for sleep false with awake island");

    const std::vector<u32> solvableIndices = collect_solvable_island_indices(graph, bodies);
    expectTrue(solvableIndices.size() == graphPreflight.stats.solvableCount,
               "collect solvable indices matches stats count");

void testPreflightIslandConstraintSolveGuards() {
    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({6.f, 0.f, 0.f}, 1.f, 0);

    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;

        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},

    graph.build(4, contacts, constraints);
    const f32 dt = 1.f / 60.f;

    expectTrue(sleepingIsland != ContactIslandGraph::invalidIsland, "sleeping island index is valid");
    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve(
        graph.island(sleepingIsland), bodies, contacts, constraints, dt);
    expectTrue(!sleepingPreflight.invalidDt, "constraint solve preflight accepts valid dt");
    expectTrue(sleepingPreflight.refs.can_solve(), "sleeping island has in-range refs");
    expectTrue(!sleepingPreflight.sleepWake.can_solve(), "sleeping island fails sleep/wake preflight");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot constraint-solve");
    expectTrue(should_skip_island_constraint_solve(
                   graph.island(sleepingIsland), bodies, contacts, constraints, dt),
               "should_skip constraint solve for all-sleeping island");

    expectTrue(awakeIsland != ContactIslandGraph::invalidIsland, "awake island index is valid");
    const IslandConstraintSolvePreflight awakePreflight = preflight_island_constraint_solve(
        graph.island(awakeIsland), bodies, contacts, constraints, dt);
    expectTrue(awakePreflight.can_solve(), "awake island passes constraint solve preflight");
    expectTrue(!should_skip_island_constraint_solve(
                   graph.island(awakeIsland), bodies, contacts, constraints, dt),
               "should_skip false for awake island constraint solve");

    const IslandConstraintSolvePreflight invalidDtPreflight = preflight_island_constraint_solve(
        graph.island(awakeIsland), bodies, contacts, constraints, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "constraint solve preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_solve(), "invalid dt cannot constraint-solve");
                   graph.island(awakeIsland), bodies, contacts, constraints, 0.f),
               "should_skip constraint solve on invalid dt");

    ContactIslandGraph::Island staleIsland{};
    staleIsland.bodyIndices = {0, 1};
    staleIsland.contactIndices = {9u};
    const IslandConstraintSolvePreflight stalePreflight =
        preflight_island_constraint_solve(staleIsland, bodies, contacts, constraints, dt);
    expectTrue(!stalePreflight.can_solve(), "stale refs fail combined constraint solve preflight");

void testAllSleepingGraphDispatchPreflight() {
    bodies.addBody({4.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({6.f, 0.f, 0.f}, 1.f, RB_SLEEPING);



    expectTrue(graphPreflight.skipped, "all-sleeping graph preflight is skipped");
    expectTrue(!graphPreflight.can_solve(), "all-sleeping graph cannot solve");
    expectTrue(count_solvable_islands(graph, bodies) == 0u, "no solvable islands when all sleeping");
    expectTrue(should_skip_island_dispatch_for_sleep(graph, bodies),
               "should_skip dispatch for sleep on all-sleeping graph");
}

} // namespace

int main() {
    fuse::core::initialize();
    testSphereGroundFallTime();
    testOverlappingSpheresSeparate();
    testDistanceConstraintHoldsLength();
    testCompliantSpringStretchesUnderLoad();
    testRestLengthSpringRecoversAfterRelease();
    testSolverReportsIterationCount();
    testContactIslandPartitionsDisconnectedGroups();
    testSleepDetection();
    testRestLengthSpringConvergesUnderIterations();
    testMultiIslandIndependentSolve();
    testConstraintResidualDecreasesWithIterations();
    testDistanceLambdaAccumulatesInSolver();
    testWarmStartLambdaFeedsAccumulation();
    testIslandSplitPartitionsDisconnectedSprings();
    testEmptyIslandHasNoConstraints();
    testWarmStartLambdaSeedHelpers();
    testLambdasPersistWithoutMidFrameClear();
    testApplyPositionDeltasClearsBodySlots();
    testDistanceLambdaWarmStartsAcrossFrames();
    testExtractIslandFlagsEmptyAndConstrained();
    testSolveIslandJobSkipsEmptyIsland();
    testPerPairDeltaApplicationDistance();
    testFrameLambdaWarmStartReseedsDistance();
    testIslandConstraintCount();
    testExtractIslandJobsBatch();
    testShouldSolveIslandGuards();
    testSolveIslandJobReturnsTrueForConstrained();
    testSolveIslandJobClearsIslandBodyDeltas();
    testFrameLambdaWarmStartReseedsContact();
    testIslandIndexValidGuard();
    testComputeIslandSolveStats();
    testHasDispatchableIslandsEarlyOut();
    testDispatchSolveIslandGuards();
    testWarmStartIslandLambdasSelective();
    testWarmStartIslandContactImpulses();
    testPreflightIslandSolveSkipsEmptyGraph();
    testPreflightIslandSolveDispatchesConstrained();
    testDispatchSolveIslandResultOutcomes();
    testDispatchAllIslandsBatch();
    testPreflightWarmStartIslandGuards();
    testWarmStartIslandLambdasGuarded();
    testWarmStartIslandContactImpulsesGuarded();
    testIsValidIslandSolveDtGuard();
    testPreflightIslandDispatchGuardsDt();
    testDispatchSolveIslandResultSkipsInvalidDt();
    testDispatchAllIslandsResultBatch();
    testPreflightFrameWarmStartGuards();
    testFrameLambdaWarmStartGuarded();
    testWarmStartGraphLambdasGuarded();
    testShouldSkipIslandSolveJobGuard();
    testShouldDispatchIslandIndexGuard();
    testCollectDispatchableIslandJobs();
    testPreflightWarmStartGraphGuards();
    testPreflightWarmStartIslandByIndex();
    testWarmStartIslandLambdasResultAndBatch();
    testPreflightDispatchIslandJobGuards();
    testDispatchSolveIslandJobGuards();
    testPreflightContactImpulseWarmStartGuards();
    testWarmStartContactImpulsesResultAndBatch();
    testWarmStartIslandCombinedGuarded();
    testIsValidWarmStartDtGuard();
    testDispatchSolveIslandJobGuards();
    testWarmStartAllIslandsResultBatch();
    testPreflightWarmStartContactImpulsesGuards();
    testWarmStartContactImpulsesResultAndBatch();
    testPreflightWarmStartCombinedIsland();
    testPreflightSolveIslandJobGuards();
    testSolveIslandJobGuardsInvalidDt();
    testIsFiniteIslandSolveDtGuard();
    testPreflightIslandConstraintRefsGuards();
    testShouldSkipWarmStartContactImpulsesWithContacts();
    testPreflightWarmStartContactImpulsesGraphGuards();
    testWarmStartAllIslandsCombinedResultBatch();
    testPreflightIslandBuildGuards();
    testPreflightIslandSolveBodiesGuards();
    testPreflightIslandSleepWakeGuards();
    testContactIslandGraphBuildRejectReasonGuards();
    testIslandBuildRejectReasonGuards();
    testIslandDispatchRejectReasonGuards();
    testIslandSolveJobRejectReasonGuards();
    testIslandConstraintSolveRejectReasonGuards();
    testIslandSleepWakeRejectReasonGuards();
    testIslandPipelineDispatchRejectReasonGuards();
    testIslandDeepenRejectReasonGuards();
    testPreflightIslandGraphIntegrityGuards();
    testPreflightIslandConstraintBodyRefsGuards();
    testSolveIslandJobGuardedSkipsAllSleeping();
    testPreflightIslandWakeAndSolveGuards();
    testIslandRejectReasonGuards();
    testPreflightIslandBuildRejectReasonGuards();
    testPreflightIslandConstraintSolveDeepenGuards();
    testPreflightIslandSleepWakeRejectReasonGuards();
    testPreflightIslandBuildDeepenGuards();
    testDispatchSolveIslandWithPreflightGuards();
    testGuardedIslandSleepAwareDispatch();
    testContactIslandGraphBodyRangeHelpers();
    testIslandBuildResultAndSelfContactStats();
    testSolveIslandJobWithBodiesGuards();
    testIslandWakeResultAndSleepDispatchGuards();
    testContactIslandGraphBodyIndexHelpers();
    testIslandBuildResultAndDegenerateGuards();
    testPreflightIslandConstraintSolveGraphGuards();
    testSolveIslandJobGuardedAndConstraintDispatch();
    testPreflightIslandSleepWakeCombinedGuards();
    testWakeIslandSleepersResultAndDispatchSolveable();
    testContactIslandGraphPartitionHelpers();
    testIslandBuildResultGuards();
    testSolveIslandJobGuarded();
    testIslandWakeResultAndSleepDispatch();
    testBodyFlagHelpers();
    testShouldSkipIslandSolveGuards();
    testCollectDispatchableIslandIndices();
    testAllIslandsEmptyFastPath();
    testIslandSolveStatsHasWork();
    testDispatchSolveIslandResult();
    testPreflightWarmStartIsland();
    testPreflightIslandSolveGuards();
    testFilterDispatchableJobs();
    testDispatchSolveAllIslands();
    testWarmStartPreflightGuards();
    testFrameLambdaWarmStartPreflightSkip();
    testWarmStartIslandLambdasSkipsEmptyIsland();
    testWarmStartIslandContactImpulsesSkipsEmptyIsland();
    testIslandIndexDispatchableGuard();
    testBuildIslandSolveDispatchPlan();
    testDispatchIslandsFromPlan();
    testComputeIslandWarmStartStats();
    testCollectWarmStartableIslandIndices();
    testWarmStartAllIslandsGuarded();
    testDispatchAllIslandsBatchResult();
    testWarmStartIslandByIndexGuarded();
    testIsDispatchableIslandIndexGuard();
    testDispatchIslandJobsResultBatch();
    testPreflightGraphWarmStartGuards();
    testWarmStartDispatchableIslandsGuarded();
    testEmptyIslandGraphGuards();
    testShouldSkipIslandSolveIndexGuard();
    testDispatchSolveIslandJobGuard();
    testDispatchIslandsAtIndices();
    testExtractWarmStartJobsAndFilters();
    testWarmStartIslandJobGuarded();
    testWarmStartIslandContactImpulsesResult();
    testPreflightContactImpulseWarmStartGuards();
    testWarmStartIslandContactImpulsesResultAndBatch();
    testPreflightWarmStartIslandCombinedGuards();
    testShouldSkipIslandDispatchJobGuard();
    testDispatchDispatchableJobsBatch();
    testWarmStartGraphContactImpulsesGuarded();
    testWarmStartIslandCombinedResultOutcomes();
    testPreflightContactImpulseGraphGuards();
    testPreflightContactImpulseDispatchGuardsDt();
    testValidateIslandConstraintIndices();
    testPreflightIslandSolveJobGuards();
    testShouldSkipIslandSolveInvalidIndices();
    testPreflightWarmStartIslandContactImpulses();
    testPreflightWarmStartContactImpulsesGraph();
    testPreflightWarmStartIslandCombined();
    testPreflightIslandSolveInputsGuards();
    testPreflightCombinedWarmStartGuards();
    testContactImpulseWarmStartGraphBatchGuards();
    testPreflightDispatchIslandIndexGuards();
    testDispatchIslandJobsBatch();
    testWarmStartContactImpulseResultAndBatch();
    testIsValidContactImpulseWarmStartDtGuard();
    testPreflightIslandContactImpulsesGuards();
    testPreflightIslandContactImpulsesByIndex();
    testPreflightIslandDispatchFromJobs();
    testPreflightWarmStartCombinedGraphAndIndex();
    testPreflightSolveIslandJobStaleIndices();
    testPreflightDispatchIslandByIndex();
    testDispatchAllIslandJobsResult();
    testPreflightWarmStartCombinedGraphGuards();
    testValidateIslandIndicesGuard();
    testPreflightSleepPassGuards();
    testPreflightWakeCandidatesGuards();
    testIslandInactiveAndConstraintPreflights();
    testPreflightDispatchableIslandJobs();
    testDispatchDispatchableIslandJobsResult();
    testPreflightIslandGraphBuildGuards();
    testPreflightIslandSleepGuards();
    testPreflightIslandWakeGuards();
    testSolveIslandPreflightAndAwakeDispatch();
    testPreflightIslandConstraintIndices();
    testPreflightSleepingIslandGuards();
    testPreflightWakeOnImpulseGuards();
    testPreflightIslandBodyPartition();
    testPreflightConstraintIterations();
    testDispatchSolveIslandSleepGuarded();
    testPreflightIslandSleepAndWakeGuards();
    testPreflightIslandConstraintSolveGuards();
    testBuildIslandGraphGuarded();
    testWakeIslandBodiesGuarded();
    testSolveIslandJobGuarded();
    testPreflightIslandSolveBodiesSleepGuards();
    testDispatchSolveIslandWithBodyGuards();
    testSleepWakeIslandGuardedBatch();
    testPreflightIslandDispatchWithSleepGuards();
    testSolveIslandJobSkipsAllSleepingIsland();
    testPreflightIslandSleepForSolveGuards();
    testPreflightIslandSolveCombinedGuards();
    testPreflightIslandDispatchNonFiniteDt();
    testPreflightIslandBodyRefsGuards();
    testDispatchAllAwakeIslandsSkipsSleeping();
    testIslandBuildGuardedSkipsZeroBodies();
    testPreflightIslandSleepWakeGraphGuards();
    testPreflightIslandSleepGraphGuards();
    testDispatchSolveIslandSkipSleeping();
    testPreflightSolveIslandWithBodiesGuards();
    testPreflightIslandSolvableConstraintRefsGuards();
    testPreflightIslandDispatchWithBodiesGuards();
    testContactIslandGraphBuildPreflightGuards();
    testPreflightIslandConstraintSolveGraphGuards();
    testGuardedIslandSolvePipelineWithWake();
    testIslandBuildRejectReasonGuards();
    testSolveIslandJobGuardedAndPipeline();
    testWakeResultAndDispatchSkippingSleepers();
    testPreflightIslandGraphBuildDeepenGuards();
    testPreflightIslandSolvePipelineGuards();
    testDispatchIslandPipelineBatchGuards();
    testContactIslandGraphBuildInputScan();
    testSolveIslandJobGuardedConstraintPreflight();
    testSleepAwareDispatchAndWakeBatch();
    testIslandRejectReasonGuards();
    testContactIslandGraphBuildGuarded();
    testPreflightIslandSolvePassGuards();
    testDispatchSolveIslandWithWakeGuarded();
    testDispatchAllIslandsWithWakeGuarded();
    testPreflightBuiltIslandGraphGuards();
    testPreflightIslandConstraintSolveByIndex();
    testPreflightIslandDispatchSleepGuards();
    testDispatchAllSolveableIslandsResult();
    testContactIslandGraphSkipsOutOfRangeRefs();
    testIslandConstraintSolveRejectReasonGuards();
    testSolveIslandJobGuardedSkipsAllSleeping();
    testDispatchAllNonsleepingIslandsResult();
    testEarlyExitWhenResidualBelowTolerance();
    testPreflightIslandBuildGuards();
    testPreflightIslandConstraintSolveGuards();
    testPreflightIslandSleepWakeGuards();
    testDispatchAllIslandsSleepGuarded();
    testBuildIslandGraphGuarded();
    testIslandSleepSolvePreflight();
    testIslandWakePreflight();
    testSolveIslandJobPreflightGuards();
    testPreflightIslandBodyRefsGuards();
    testPreflightIslandSolveRefsCombined();
    testPreflightIslandSleepGuards();
    testPreflightIslandWakeGuards();
    testSolveIslandJobSkipsFullySleepingIsland();
    testPreflightIslandSolveParticipationGuards();
    testIslandSleepWakeGraphGuards();
    testIslandBodyFlagHelpers();
    testBuildGuardedMatchesBuildOnValidPath();
    testPreflightIslandSleepStateGuards();
    testPreflightIslandSleepGraphGuards();
    testDispatchSolveIslandSleepGuarded();
    testSolveIslandJobSleepGuarded();
    testBuildGuardedAndBuildStats();
    testSolveIslandJobGuardedSleepAndRefs();
    testAllSleepingGraphDispatchPreflight();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_pbd_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_pbd_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
