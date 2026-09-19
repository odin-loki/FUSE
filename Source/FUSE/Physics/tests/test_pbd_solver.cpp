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
}

void testDispatchSolveIslandResultOutcomes() {
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

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!job.empty) {
            continue;
        }
        const IslandDispatchResult emptyResult = dispatch_solve_island_result(bodies,
                                                                              graph,
                                                                              islandIndex,
                                                                              work,
                                                                              constraints,
                                                                              1.f / 60.f,
                                                                              0.f,
                                                                              invMassFn);
        expectTrue(emptyResult.skipped, "empty island dispatch result is skipped");
        expectTrue(!emptyResult.solved, "empty island dispatch result is not solved");
        foundEmptySkip = true;
        break;
    }
    expectTrue(foundEmptySkip, "dispatch_solve_island_result covers empty island skip");

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandDispatchResult solved = dispatch_solve_island_result(bodies,
                                                                     graph,
                                                                     constrainedIndex,
                                                                     work,
                                                                     constraints,
                                                                     1.f / 60.f,
                                                                     0.f,
                                                                     invMassFn);
    expectTrue(solved.solved, "constrained island dispatch result is solved");
    expectTrue(!solved.skipped, "constrained island dispatch result is not skipped");
    expectTrue(solved.islandIndex == constrainedIndex, "dispatch result records island index");
}

void testDispatchAllIslandsBatch() {
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

    const u32 solvedCount = dispatch_all_islands(bodies,
                                                 graph,
                                                 work,
                                                 constraints,
                                                 1.f / 60.f,
                                                 0.f,
                                                 invMassFn);
    expectTrue(solvedCount == graph.constrainedIslandCount(),
               "dispatch_all_islands solves all constrained islands");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(dispatch_all_islands(bodies,
                                    emptyGraph,
                                    work,
                                    constraints,
                                    1.f / 60.f,
                                    0.f,
                                    invMassFn) == 0u,
               "dispatch_all_islands early-outs on empty graph");
}

void testPreflightWarmStartIslandGuards() {
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

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandWarmStartPreflight preflight =
            preflight_warm_start_island(island, priorDistance, priorContact);
        expectTrue(preflight.skipped, "preflight skips empty island warm-start");
        expectTrue(!preflight.can_warm_start(), "empty island cannot warm-start");
        expectTrue(should_skip_warm_start_island(island), "should_skip_warm_start_island on empty island");
    }
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
}

void testWarmStartIslandLambdasGuarded() {
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
    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = !warm_start_island_lambdas_guarded(work, island, priorDistance, priorContact);
        break;
    }
    expectTrue(foundEmptySkip, "guarded warm-start skips empty island");
    expectNear(work.distanceLambdas()[0], 0.f, 1e-6f,
               "guarded skip leaves distance lambda untouched");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_lambdas_guarded(work,
                                                 graph.island(islandA),
                                                 priorDistance,
                                                 priorContact),
               "guarded warm-start succeeds for constrained island");
    expectNear(work.distanceLambdas()[0], 0.15f, 1e-6f,
               "guarded warm-start seeds owned distance slot");
    expectNear(work.contactLambdas()[0], 0.25f, 1e-6f,
               "guarded warm-start seeds owned contact slot");

    work.clearLambdas();
    expectTrue(!warm_start_island_lambdas_guarded(work,
                                                  graph.island(islandA),
                                                  {},
                                                  {}),
               "guarded warm-start skips when no prior data exists");
}

void testWarmStartIslandContactImpulsesGuarded() {
    SolverWorkBuffers work;
    work.init(4, 2, 0);
    work.ensureLambdaCapacity(2, 0);

    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 4.f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const f32 dt = 1.f / 60.f;
    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = !warm_start_island_contact_impulses_guarded(work, island, contacts, dt);
        break;
    }
    expectTrue(foundEmptySkip, "guarded impulse warm-start skips empty island");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_contact_impulses_guarded(work, graph.island(islandA), contacts, dt),
               "guarded impulse warm-start succeeds for contact island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "guarded impulse warm-start seeds contact lambda");
}

void testIsValidIslandSolveDtGuard() {
    expectTrue(is_valid_island_solve_dt(1.f / 60.f), "positive dt is valid for island solve");
    expectTrue(!is_valid_island_solve_dt(0.f), "zero dt is invalid for island solve");
    expectTrue(!is_valid_island_solve_dt(-1.f / 60.f), "negative dt is invalid for island solve");
}

void testPreflightIslandDispatchGuardsDt() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
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
}

void testDispatchSolveIslandResultSkipsInvalidDt() {
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

    const IslandDispatchResult invalidDt = dispatch_solve_island_result(bodies,
                                                                        graph,
                                                                        0u,
                                                                        work,
                                                                        constraints,
                                                                        0.f,
                                                                        0.f,
                                                                        invMassFn);
    expectTrue(invalidDt.skipped, "invalid dt dispatch result is skipped");
    expectTrue(!invalidDt.solved, "invalid dt dispatch result is not solved");
    expectTrue(!dispatch_solve_island(bodies,
                                       graph,
                                       0u,
                                       work,
                                       constraints,
                                       0.f,
                                       0.f,
                                       invMassFn),
               "dispatch_solve_island guards invalid dt");
}

void testDispatchAllIslandsResultBatch() {
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

    const IslandBatchDispatchResult batch = dispatch_all_islands_result(bodies,
                                                                        graph,
                                                                        work,
                                                                        constraints,
                                                                        1.f / 60.f,
                                                                        0.f,
                                                                        invMassFn);
    expectTrue(!batch.skipped, "batch dispatch does not skip constrained graph");
    expectTrue(batch.dispatchableCount == graph.constrainedIslandCount(),
               "batch dispatch records dispatchable count");
    expectTrue(batch.solvedCount == graph.constrainedIslandCount(),
               "batch dispatch solves all constrained islands");
    expectTrue(batch.any_solved(), "batch dispatch reports solved islands");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    const IslandBatchDispatchResult emptyBatch = dispatch_all_islands_result(bodies,
                                                                             emptyGraph,
                                                                             work,
                                                                             constraints,
                                                                             1.f / 60.f,
                                                                             0.f,
                                                                             invMassFn);
    expectTrue(emptyBatch.skipped, "batch dispatch skips empty graph");
    expectTrue(!emptyBatch.any_solved(), "empty graph batch reports no solved islands");

    const IslandBatchDispatchResult invalidDtBatch = dispatch_all_islands_result(bodies,
                                                                                 graph,
                                                                                 work,
                                                                                 constraints,
                                                                                 0.f,
                                                                                 0.f,
                                                                                 invMassFn);
    expectTrue(invalidDtBatch.skipped, "batch dispatch skips invalid dt");
    expectTrue(invalidDtBatch.solvedCount == 0u, "invalid dt batch solves zero islands");
}

void testPreflightFrameWarmStartGuards() {
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
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
}

void testFrameLambdaWarmStartGuarded() {
    SolverWorkBuffers work;
    work.init(2, 2, 1);
    work.ensureLambdaCapacity(2, 1);
    work.distanceLambda(0) = 0.42f;
    work.contactLambda(0) = 0.24f;

    const std::vector<f32> priorDistance = work.distanceLambdas();
    const std::vector<f32> priorContact = work.contactLambdas();
    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };

    work.distanceLambda(0) = 0.99f;
    work.contactLambda(0) = 0.99f;
    expectTrue(!frame_lambda_warm_start_guarded(work, constraints, {}, {}),
               "guarded frame warm-start skips when no prior data exists");
    expectNear(work.distanceLambdas()[0], 0.f, 1e-6f,
               "guarded skip still clears distance lambda slots");

    work.distanceLambda(0) = 0.99f;
    work.contactLambda(0) = 0.99f;
    expectTrue(frame_lambda_warm_start_guarded(work, constraints, priorDistance, priorContact),
               "guarded frame warm-start succeeds with prior data");
    expectNear(work.distanceLambdas()[0], 0.42f, 1e-6f,
               "guarded frame warm-start reseeds distance slot");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "guarded frame warm-start reseeds contact slot");
}

void testWarmStartGraphLambdasGuarded() {
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

    work.clearLambdas();
    expectTrue(warm_start_graph_lambdas_guarded(work, graph, {}, {}) == 0u,
               "graph warm-start skips all islands when no prior data exists");
}

void testShouldSkipIslandSolveJobGuard() {
    IslandSolveJob invalid{};
    expectTrue(should_skip_island_solve_job(invalid), "default job is skipped");
    expectTrue(!should_solve_island(invalid), "should_skip mirrors should_solve inverse");

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
        if (job.empty) {
            foundEmptySkip = should_skip_island_solve_job(job);
        } else {
            foundConstrainedDispatch = !should_skip_island_solve_job(job);
        }
    }

    expectTrue(foundEmptySkip, "should_skip_island_solve_job covers empty island");
    expectTrue(foundConstrainedDispatch, "should_skip_island_solve_job allows constrained island");
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

void testCollectDispatchableIslandJobs() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
        DistanceConstraint{.bodyA = 2, .bodyB = 3, .restLength = 2.f},
    };
    graph.build(5, contacts, constraints);

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
    }
}

void testPreflightWarmStartGraphGuards() {
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
}

void testPreflightWarmStartIslandByIndex() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const std::vector<f32> priorDistance = {0.15f};
    const std::vector<f32> priorContact = {0.25f};

    const IslandWarmStartPreflight outOfRange =
        preflight_warm_start_island_by_index(graph, graph.islandCount() + 1u, priorDistance, priorContact);
    expectTrue(outOfRange.skipped, "index preflight skips out-of-range island");
    expectTrue(should_skip_warm_start_island_index(graph, graph.islandCount() + 1u),
               "should_skip_warm_start_island_index on out-of-range index");

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandWarmStartPreflight emptyPreflight =
            preflight_warm_start_island_by_index(graph, islandIndex, priorDistance, priorContact);
        expectTrue(emptyPreflight.skipped, "index preflight skips empty island");
        expectTrue(should_skip_warm_start_island_index(graph, islandIndex),
                   "should_skip_warm_start_island_index on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for index warm-start preflight");

    const u32 constrainedIndex = graph.bodyIsland(0);
    const IslandWarmStartPreflight constrainedPreflight =
        preflight_warm_start_island_by_index(graph, constrainedIndex, priorDistance, priorContact);
    expectTrue(!constrainedPreflight.skipped, "index preflight does not skip constrained island");
    expectTrue(constrainedPreflight.can_warm_start(), "index preflight can warm-start constrained island");
}

void testWarmStartIslandLambdasResultAndBatch() {
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

    const IslandWarmStartResult outOfRange =
        warm_start_island_lambdas_result(work, graph, graph.islandCount() + 3u, priorDistance, priorContact);
    expectTrue(outOfRange.skipped, "warm_start result skips out-of-range island");
    expectTrue(!outOfRange.warmed, "out-of-range warm_start result is not warmed");

    work.clearLambdas();
    const u32 warmedCount = warm_start_all_islands_guarded(work, graph, priorDistance, priorContact);
    expectTrue(warmedCount == graph.constrainedIslandCount(),
               "warm_start_all_islands_guarded seeds all constrained islands");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "batch warm-start seeds first distance slot");
    expectNear(work.contactLambdas()[1], 0.44f, 1e-6f,
               "batch warm-start seeds second contact slot");

    work.clearLambdas();
    const u32 constrainedIndex = graph.bodyIsland(0);
    expectTrue(warm_start_island_lambdas_by_index_guarded(work,
                                                          graph,
                                                          constrainedIndex,
                                                          priorDistance,
                                                          priorContact),
               "index guarded warm-start succeeds for constrained island");
    expectNear(work.distanceLambdas()[0], 0.11f, 1e-6f,
               "index guarded warm-start seeds owned distance slot");
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
    contacts.back().warmNormalImpulse = 3.5f;

    ContactIslandGraph graph;
    graph.build(3, contacts, {});

    const std::vector<f32> priorDistance = {0.12f};
    const std::vector<f32> priorContact = {0.24f};
    const f32 dt = 1.f / 60.f;

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = !warm_start_island_combined_guarded(work,
                                                             island,
                                                             contacts,
                                                             dt,
                                                             priorDistance,
                                                             priorContact);
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
               "combined warm-start succeeds for contact island with prior data");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "combined warm-start seeds prior contact lambda");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "combined warm-start retains non-zero contact lambda from impulse seed");
}

void testIsValidWarmStartDtGuard() {
    expectTrue(is_valid_warm_start_dt(1.f / 60.f), "positive dt is valid for warm-start");
    expectTrue(!is_valid_warm_start_dt(0.f), "zero dt is invalid for warm-start");
    expectTrue(is_valid_warm_start_dt(1.f / 60.f) == is_valid_island_solve_dt(1.f / 60.f),
               "warm-start dt guard matches island solve dt guard");
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
               "dispatch_solve_island_job skips default job");

    const IslandDispatchResult invalidJobResult =
        dispatch_solve_island_job_result(bodies, invalid, work, constraints, 0.f, 0.f, invMassFn);
    expectTrue(invalidJobResult.skipped, "job dispatch result skips invalid dt");

    bool foundEmptySkip = false;
    bool foundConstrainedSolve = false;
    for (const IslandSolveJob& job : extract_island_jobs(graph)) {
        if (job.empty) {
            foundEmptySkip = !dispatch_solve_island_job(bodies, job, work, constraints, dt, 0.f, invMassFn);
        } else {
            foundConstrainedSolve = dispatch_solve_island_job(bodies, job, work, constraints, dt, 0.f, invMassFn);
        }
    }
    expectTrue(foundEmptySkip, "dispatch_solve_island_job skips empty island job");
    expectTrue(foundConstrainedSolve, "dispatch_solve_island_job solves constrained island job");
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
    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_result(work, graph, priorDistance, priorContact);
    expectTrue(!batch.skipped, "batch warm-start does not skip constrained graph");
    expectTrue(batch.warmStartableCount == graph.constrainedIslandCount(),
               "batch warm-start records warm-startable count");
    expectTrue(batch.warmedCount == graph.constrainedIslandCount(),
               "batch warm-start seeds all constrained islands");
    expectTrue(batch.any_warmed(), "batch warm-start reports warmed islands");

    work.clearLambdas();
    const IslandBatchWarmStartResult emptyPrior =
        warm_start_all_islands_result(work, graph, {}, {});
    expectTrue(emptyPrior.skipped, "batch warm-start skips when no prior data exists");
    expectTrue(!emptyPrior.any_warmed(), "empty prior batch reports no warmed islands");
}

void testPreflightWarmStartContactImpulsesGuards() {
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

    const u32 islandA = graph.bodyIsland(0);
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

    bool foundEmptySkip = false;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island.isEmpty()) {
            continue;
        }
        foundEmptySkip = true;
        const IslandContactImpulseWarmStartPreflight emptyPreflight =
            preflight_warm_start_contact_impulses(island, contacts, dt);
        expectTrue(emptyPreflight.skipped, "impulse preflight skips empty island");
        expectTrue(should_skip_warm_start_contact_impulses(island, dt),
                   "should_skip_warm_start_contact_impulses on empty island");
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for impulse preflight");
}

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
    contacts.back().warmNormalImpulse = 0.f;

    ContactIslandGraph graph;
    graph.build(4, contacts, {});
    const f32 dt = 1.f / 60.f;

    const IslandWarmStartResult outOfRange =
        warm_start_island_contact_impulses_result(work, graph, graph.islandCount() + 2u, contacts, dt);
    expectTrue(outOfRange.skipped, "impulse result skips out-of-range island");

    const IslandWarmStartResult invalidDt =
        warm_start_island_contact_impulses_result(work, graph, graph.bodyIsland(0), contacts, 0.f);
    expectTrue(invalidDt.skipped, "impulse result skips invalid dt");
    expectTrue(!warm_start_island_contact_impulses_guarded(work, graph.island(graph.bodyIsland(0)), contacts, 0.f),
               "guarded impulse warm-start skips invalid dt");

    work.clearLambdas();
    const u32 islandA = graph.bodyIsland(0);
    expectTrue(warm_start_island_contact_impulses_by_index_guarded(work, graph, islandA, contacts, dt),
               "index guarded impulse warm-start succeeds for contact island");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "index guarded impulse warm-start seeds contact lambda");

    work.clearLambdas();
    const IslandBatchWarmStartResult batch =
        warm_start_all_islands_contact_impulses_result(work, graph, contacts, dt);
    expectTrue(!batch.skipped, "impulse batch does not skip contact graph");
    expectTrue(batch.warmedCount == 1u, "impulse batch seeds only non-zero impulse island");
    expectTrue(batch.any_warmed(), "impulse batch reports warmed island");

    work.clearLambdas();
    expectTrue(warm_start_all_islands_contact_impulses_guarded(work, graph, contacts, dt) == 1u,
               "impulse batch guarded count matches warmed islands");

    const IslandBatchWarmStartResult invalidDtBatch =
        warm_start_all_islands_contact_impulses_result(work, graph, contacts, 0.f);
    expectTrue(invalidDtBatch.skipped, "impulse batch skips invalid dt");
}

void testPreflightWarmStartCombinedIsland() {
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

    const u32 islandA = graph.bodyIsland(0);
    const IslandCombinedWarmStartPreflight preflight =
        preflight_warm_start_combined_island(graph.island(islandA), contacts, dt, priorDistance, priorContact);
    expectTrue(!preflight.skipped, "combined preflight does not skip contact island");
    expectTrue(preflight.lambdas.can_warm_start(), "combined preflight sees prior lambda data");
    expectTrue(preflight.impulses.can_warm_start(), "combined preflight sees non-zero impulses");
    expectTrue(preflight.can_warm_start(), "combined preflight can warm-start");

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
    }
    expectTrue(foundEmptySkip, "graph exposes empty island for combined preflight");

    work.clearLambdas();
    const IslandWarmStartResult combinedResult =
        warm_start_island_combined_result(work, graph, islandA, contacts, dt, priorDistance, priorContact);
    expectTrue(combinedResult.warmed, "combined result warms contact island");
    expectNear(work.contactLambdas()[0], 0.24f, 1e-6f,
               "combined result seeds prior contact lambda");

    work.clearLambdas();
    const u32 warmedCount =
        warm_start_all_islands_combined_guarded(work, graph, contacts, dt, priorDistance, priorContact);
    expectTrue(warmedCount == 1u, "combined graph batch warms contact island only");
    expectTrue(std::fabs(work.contactLambdas()[0]) > 1e-6f,
               "combined graph batch seeds contact island");
}

void testPreflightSolveIslandJobGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const f32 dt = 1.f / 60.f;
    IslandSolveJob invalid{};
    const IslandSolveJobPreflight invalidPreflight = preflight_solve_island_job(invalid, dt);
    expectTrue(invalidPreflight.skipped, "job preflight skips default job");
    expectTrue(!invalidPreflight.can_dispatch(), "default job cannot dispatch");
    expectTrue(should_skip_solve_island_job(invalid, dt), "should_skip_solve_island_job on default job");

    const IslandSolveJobPreflight invalidDtPreflight = preflight_solve_island_job(invalid, 0.f);
    expectTrue(invalidDtPreflight.invalidDt, "job preflight rejects zero dt");
    expectTrue(should_skip_solve_island_job(invalid, 0.f), "should_skip_solve_island_job on invalid dt");

    bool foundEmptySkip = false;
    bool foundConstrainedDispatch = false;
    for (const IslandSolveJob& job : extract_island_jobs(graph)) {
        const IslandSolveJobPreflight preflight = preflight_solve_island_job(job, dt);
        if (job.empty) {
            foundEmptySkip = preflight.skipped && !preflight.can_dispatch();
        } else {
            foundConstrainedDispatch = preflight.can_dispatch() && !should_skip_solve_island_job(job, dt);
        }
    }
    expectTrue(foundEmptySkip, "job preflight skips empty island job");
    expectTrue(foundConstrainedDispatch, "job preflight allows constrained island job");
}

void testSolveIslandJobGuardsInvalidDt() {
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
    const auto invMassFn = [](const RigidBodySoA&, u32) { return 1.f; };

    expectTrue(!solve_island_job(bodies, *job.island, work, constraints, 0.f, 0.f, invMassFn),
               "solve_island_job guards zero dt");
    expectTrue(!solve_island_job(bodies, *job.island, work, constraints, -1.f / 60.f, 0.f, invMassFn),
               "solve_island_job guards negative dt");
}

void testIsFiniteIslandSolveDtGuard() {
    expectTrue(is_finite_island_solve_dt(1.f / 60.f), "finite positive dt is valid");
    expectTrue(!is_finite_island_solve_dt(0.f), "zero dt is not finite-positive");
    expectTrue(!is_finite_island_solve_dt(std::numeric_limits<f32>::infinity()),
               "infinite dt is not finite-positive");
    expectTrue(!is_finite_island_solve_dt(std::numeric_limits<f32>::quiet_NaN()),
               "NaN dt is not finite-positive");
    expectTrue(is_finite_warm_start_dt(1.f / 60.f), "finite warm-start dt matches solve guard");
}

void testPreflightIslandConstraintRefsGuards() {
    ContactIslandGraph graph;
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
    };
    graph.build(4, contacts, constraints);

    const u32 islandA = graph.bodyIsland(0);
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

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    bodies.addBody({2.1f, 0.f, 0.f}, 1.f, 0);
    bodies.predictedPositions = bodies.positions;
    SolverWorkBuffers work;
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
}

void testShouldSkipWarmStartContactImpulsesWithContacts() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;
    contacts.back().warmNormalImpulse = 0.f;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 2;
    contacts.back().bodyB = 3;
    contacts.back().warmNormalImpulse = 2.f;

    ContactIslandGraph graph;
    graph.build(5, contacts, {});
    const f32 dt = 1.f / 60.f;

    const u32 zeroImpulseIsland = graph.bodyIsland(0);
    const u32 nonZeroIsland = graph.bodyIsland(2);
    expectTrue(!should_skip_warm_start_contact_impulses(graph.island(zeroImpulseIsland), dt),
               "two-arg skip cannot detect zero impulses without contact scan");
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(zeroImpulseIsland), contacts, dt),
               "three-arg skip rejects island with only zero impulses");
    expectTrue(!should_skip_warm_start_contact_impulses(graph.island(nonZeroIsland), contacts, dt),
               "three-arg skip allows island with non-zero impulses");
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
    expectTrue(warm_start_all_islands_combined_guarded(work, graph, contacts, dt, priorDistance, priorContact) ==
                   batch.warmedCount,
               "combined guarded count matches batch result");

    work.clearLambdas();
    const IslandBatchWarmStartResult invalidDt =
        warm_start_all_islands_combined_result(work, graph, contacts, 0.f, priorDistance, priorContact);
    expectTrue(invalidDt.skipped, "combined batch skips invalid dt");
    expectTrue(!invalidDt.any_warmed(), "invalid dt combined batch reports no warmed islands");
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
    testEarlyExitWhenResidualBelowTolerance();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_pbd_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_pbd_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
