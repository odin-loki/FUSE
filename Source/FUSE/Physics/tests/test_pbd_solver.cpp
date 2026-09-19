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

void testPreflightIslandBuildGuards() {
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

    ContactIslandGraph graph;
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
}

void testPreflightIslandSolveBodiesGuards() {
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

    SolverWorkBuffers work;
    work.init(4, 0, 2);
    work.contactManifolds().clear();
    const IslandConstraintSolvePreflight combinedPreflight = preflight_island_constraint_solve(
        graph.island(activeIsland), bodies, work.contactManifolds(), constraints);
    expectTrue(!combinedPreflight.can_solve(), "combined solve preflight rejects all-sleeping island");
    expectTrue(should_skip_island_constraint_solve(
                   graph.island(activeIsland), bodies, work.contactManifolds(), constraints),
               "should_skip_island_constraint_solve on all-sleeping island");
}

void testPreflightIslandSleepWakeGuards() {
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
}

void testContactIslandGraphBuildRejectReasonGuards() {
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

    expectTrue(contactIslandGraphBuildRejectReason(4, contacts, constraints) ==
                   ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies,
               "graph build reject reason flags out-of-range contacts");
    expectTrue(contactIslandGraphBuildRejectsForReason(4,
                                                       contacts,
                                                       constraints,
                                                       ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies),
               "graph build rejectsForReason matches out-of-range contacts");
    expectTrue(std::strcmp(contactIslandGraphBuildRejectReasonName(
                               ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies),
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

    ContactIslandGraph graph;
    expectTrue(!graph.buildGuarded(4, contacts, constraints),
               "buildGuarded rejects unsafe out-of-range contacts");
    expectTrue(graph.islandCount() == 0u, "rejected buildGuarded clears graph");

    expectTrue(contactIslandGraphBuildRejectReason(0, {}, {}) ==
                   ContactIslandGraphBuildRejectReason::EmptyInput,
               "graph build reject reason flags empty input");
    expectTrue(!graph.buildGuarded(0, {}, {}), "buildGuarded skips empty zero-body input");
    expectTrue(graph.islandCount() == 0u, "skipped empty buildGuarded clears graph");
}

void testIslandBuildRejectReasonGuards() {
    std::vector<narrowphase::ContactManifold> contacts;
    contacts.push_back(narrowphase::ContactManifold{});
    contacts.back().valid = true;
    contacts.back().bodyA = 0;
    contacts.back().bodyB = 1;

    const std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 4, .bodyB = 5, .restLength = 2.f},
    };

    const IslandBuildPreflight preflight = preflight_island_build(4, contacts, constraints);
    expectTrue(preflight.reason == ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies,
               "island build preflight surfaces distance reject reason");
    expectTrue(canSkipIslandBuild(4, contacts, constraints),
               "canSkipIslandBuild true for out-of-range distance refs");
    expectTrue(!shouldRunIslandBuild(4, contacts, constraints),
               "shouldRunIslandBuild false for out-of-range distance refs");
}

void testIslandDispatchRejectReasonGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

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

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(canSkipIslandDispatch(emptyGraph, 1.f / 60.f),
               "canSkipIslandDispatch true for empty graph");
    expectTrue(islandDispatchRejectReason(emptyGraph, 1.f / 60.f) ==
                   IslandDispatchRejectReason::NoDispatchableIslands,
               "empty graph dispatch reject reason is no dispatchable islands");
}

void testIslandSolveJobRejectReasonGuards() {
    IslandSolveJob invalid{};
    const f32 dt = 1.f / 60.f;
    expectTrue(islandSolveJobRejectReason(invalid, dt) == IslandSolveJobRejectReason::EmptyJob,
               "default job reject reason is empty job");
    expectTrue(islandSolveJobRejectsForReason(invalid, 0.f, IslandSolveJobRejectReason::InvalidDt),
               "job rejectsForReason flags invalid dt before empty job");
    expectTrue(std::strcmp(islandSolveJobRejectReasonName(IslandSolveJobRejectReason::EmptyJob), "EmptyJob") == 0,
               "job reject reason name is stable");

    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(3, contacts, constraints);

    const IslandSolveJob job = extract_island(graph, graph.bodyIsland(0));
    const IslandSolveJobRejectPreflight preflight = preflightIslandSolveJobReject(job, dt);
    expectTrue(preflight.can_dispatch(), "constrained job reject preflight can dispatch");
    expectTrue(shouldRunIslandSolveJob(job, dt), "shouldRunIslandSolveJob true for constrained job");
    expectTrue(!canSkipIslandSolveJob(job, dt), "canSkipIslandSolveJob false for constrained job");
}

void testIslandConstraintSolveRejectReasonGuards() {
    ContactIslandGraph graph;
    std::vector<narrowphase::ContactManifold> contacts;
    std::vector<DistanceConstraint> constraints = {
        DistanceConstraint{.bodyA = 0, .bodyB = 1, .restLength = 2.f},
    };
    graph.build(2, contacts, constraints);

    RigidBodySoA bodies;
    bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    bodies.addBody({2.f, 0.f, 0.f}, 1.f, RB_SLEEPING);

    const ContactIslandGraph::Island& island = graph.island(0);
    const IslandConstraintSolveRejectPreflight preflight =
        preflightIslandConstraintSolveReject(island, bodies, contacts, constraints);
    expectTrue(preflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies,
               "constraint solve reject reason flags no movable bodies");
    expectTrue(islandConstraintSolveRejectsForReason(island,
                                                     bodies,
                                                     contacts,
                                                     constraints,
                                                     IslandConstraintSolveRejectReason::NoMovableBodies),
               "constraint solve rejectsForReason matches no movable bodies");
    expectTrue(canSkipIslandConstraintSolve(island, bodies, contacts, constraints),
               "canSkipIslandConstraintSolve true for all-sleeping island");
    expectTrue(!shouldRunIslandConstraintSolve(island, bodies, contacts, constraints),
               "shouldRunIslandConstraintSolve false for all-sleeping island");
}

void testIslandSleepWakeRejectReasonGuards() {
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
}

void testIslandPipelineDispatchRejectReasonGuards() {
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

    const IslandPipelineDispatchPreflight preflight = preflightIslandPipelineDispatch(graph, bodies, dt);
    expectTrue(preflight.can_dispatch(), "pipeline preflight allows mixed active island graph");
    expectTrue(shouldRunIslandPipelineDispatch(graph, bodies, dt),
               "shouldRunIslandPipelineDispatch true for mixed graph");
    expectTrue(islandPipelineDispatchRejectReason(graph, bodies, dt) ==
                   IslandPipelineDispatchRejectReason::None,
               "pipeline reject reason is none for mixed graph");

    bodies.flags[0] |= RB_SLEEPING;
    expectTrue(islandPipelineDispatchRejectReason(graph, bodies, dt) ==
                   IslandPipelineDispatchRejectReason::AllIslandsSleeping,
               "pipeline reject reason flags all-sleeping graph");
    expectTrue(canSkipIslandPipelineDispatch(graph, bodies, dt),
               "canSkipIslandPipelineDispatch true when all islands sleeping");
    expectTrue(islandPipelineDispatchRejectsForReason(graph,
                                                      bodies,
                                                      dt,
                                                      IslandPipelineDispatchRejectReason::AllIslandsSleeping),
               "pipeline rejectsForReason matches all-sleeping graph");

    bodies.flags[0] &= ~RB_SLEEPING;
    const IslandBatchDispatchResult pipelineBatch = dispatch_island_pipeline_guarded(bodies,
                                                                                   graph,
                                                                                   work,
                                                                                   constraints,
                                                                                   dt,
                                                                                   0.f,
                                                                                   invMassFn);
    expectTrue(!pipelineBatch.skipped, "pipeline guarded dispatch runs for mixed graph");
    expectTrue(pipelineBatch.solvedCount == graph.constrainedIslandCount(),
               "pipeline guarded dispatch solves all constrained islands");
    expectTrue((bodies.flags[1] & RB_SLEEPING) == 0u,
               "pipeline guarded dispatch wakes mixed island sleepers");

    const IslandBatchDispatchResult preflightBatch = dispatch_all_islands_with_preflight(bodies,
                                                                                         graph,
                                                                                         work,
                                                                                         constraints,
                                                                                         dt,
                                                                                         0.f,
                                                                                         invMassFn);
    expectTrue(!preflightBatch.skipped, "dispatch_all_islands_with_preflight runs constrained graph");
    expectTrue(preflightBatch.solvedCount == graph.constrainedIslandCount(),
               "dispatch_all_islands_with_preflight solves all constrained islands");

    ContactIslandGraph emptyGraph;
    emptyGraph.build(0, {}, {});
    expectTrue(dispatch_all_islands_with_preflight(bodies,
                                                   emptyGraph,
                                                   work,
                                                   constraints,
                                                   dt,
                                                   0.f,
                                                   invMassFn)
                   .skipped,
               "dispatch_all_islands_with_preflight skips empty graph");
}

void testBodyFlagHelpers() {
    expectTrue(is_body_sleeping(RB_SLEEPING), "is_body_sleeping detects sleeping flag");
    expectTrue(!is_body_sleeping(0u), "is_body_sleeping false for awake body");
    expectTrue(is_body_static_or_kinematic(RB_STATIC), "static flag detected");
    expectTrue(is_body_static_or_kinematic(RB_KINEMATIC), "kinematic flag detected");
    expectTrue(!is_body_static_or_kinematic(0u), "dynamic body is not static/kinematic");

    RigidBodySoA bodies;
    const u32 dynamic = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 sleeping = bodies.addBody({1.f, 0.f, 0.f}, 1.f, RB_SLEEPING);
    const u32 staticBody = bodies.addBody({2.f, 0.f, 0.f}, 0.f, RB_STATIC);
    expectTrue(is_body_movable(bodies, dynamic), "awake dynamic body is movable");
    expectTrue(!is_body_movable(bodies, sleeping), "sleeping body is not movable");
    expectTrue(!is_body_movable(bodies, staticBody), "static body is not movable");
    expectTrue(!is_body_movable(bodies, 99u), "out-of-range body is not movable");
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
    testBodyFlagHelpers();
    testEarlyExitWhenResidualBelowTolerance();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_pbd_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_pbd_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

// --- deepen additive from deepen-b4-pbd-island-solve-guards-848c ---
void testShouldSkipIslandSolveGuards() {
    expectTrue(should_skip_island_solve(invalid), "default job is skipped");
    expectTrue(!should_solve_island(invalid), "should_skip mirrors should_solve");
            expectTrue(should_skip_island_solve(job), "empty island job is skipped");
            expectTrue(!should_skip_island_solve(job), "constrained island job is not skipped");
    expectTrue(should_skip_island_solve(outOfRange), "out-of-range island job is skipped");
        expectTrue(!should_skip_island_solve(job), "collected index is not skipped");
    expectTrue(should_skip_all_island_solves(loneBodies),
               "should_skip_all_island_solves for lone unconstrained bodies");
    expectTrue(!should_skip_all_island_solves(constrained),
    expectTrue(should_skip_all_island_solves(graph), "skip-all mirrors stats no-work");
void testPreflightWarmStartIsland() {
    IslandWarmStartPreflight emptyPreflight{};
                                            emptyPreflight),
    IslandWarmStartPreflight preflight{};
    IslandWarmStartPreflight zeroPreflight{};
    zeroPreflight.hasDistanceLambdas = false;
    zeroPreflight.hasContactLambdas = false;
    zeroPreflight.hasContactImpulses = false;
                                      zeroPreflight);
    testPreflightWarmStartIsland();

// --- deepen additive from deepen-b4-pbd-island-guards-4cc0 ---
void testPreflightIslandSolveGuards() {
    bool foundEmptyPreflight = false;
    bool foundConstrainedPreflight = false;
            foundEmptyPreflight = true;
            foundConstrainedPreflight = true;
    expectTrue(foundEmptyPreflight, "preflight_island_solve covers empty island");
    expectTrue(foundConstrainedPreflight, "preflight_island_solve covers constrained island");
void testWarmStartPreflightGuards() {
void testFrameLambdaWarmStartPreflightSkip() {
    testPreflightIslandSolveGuards();
    testWarmStartPreflightGuards();
    testFrameLambdaWarmStartPreflightSkip();

// --- deepen additive from deepen-b4-pbd-island-preflight-warmstart-4254 ---
void testIslandIndexDispatchableGuard() {
    expectTrue(should_skip_frame_warm_start(emptyGraph, priorDistance, priorContact),
               "should_skip_frame_warm_start on empty graph");
    expectTrue(should_skip_frame_warm_start(graph, {}, {}),
               "should_skip_frame_warm_start when no prior data");
void testWarmStartAllIslandsGuarded() {

// --- deepen additive from deepen-b4-pbd-island-preflight-warmstart-8200 ---
    const IslandWarmStartGraphPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "graph warm-start preflight skips empty graph");
    expectTrue(!emptyPreflight.can_warm_start(), "empty graph cannot warm-start");
    expectTrue(should_skip_warm_start_graph(emptyGraph, priorDistance, priorContact),
               "should_skip_warm_start_graph on empty graph");
    const IslandWarmStartGraphPreflight lonePreflight =
    expectTrue(lonePreflight.skipped, "graph warm-start preflight skips lone bodies");
    expectTrue(lonePreflight.stats.emptyCount == loneBodies.islandCount(),
               "should_skip false when warm-startable islands exist");
        const IslandWarmStartPreflight islandPreflight =
        expectTrue(islandPreflight.can_warm_start(), "collected index passes island preflight");
void testWarmStartIslandByIndexGuarded() {

// --- deepen additive from deepen-b4-pbd-island-dispatch-warmstart-guards-e8ca ---
void testPreflightContactImpulseWarmStartGuards() {
    expectTrue(constrainedPreflight.nonZeroImpulseCoverage == 1u,
    expectTrue(should_skip_contact_impulse_warm_start_island_index(graph, graph.islandCount() + 1u),
               "should_skip_contact_impulse_warm_start_island_index on out-of-range index");
        expectTrue(should_skip_contact_impulse_warm_start_island(island),
                   "should_skip_contact_impulse_warm_start_island on empty island");
void testPreflightWarmStartIslandCombinedGuards() {
    const IslandCombinedWarmStartPreflight combinedPreflight =
    expectTrue(!combinedPreflight.skipped, "combined preflight does not skip constrained island");
    expectTrue(combinedPreflight.lambdas.can_warm_start(), "combined preflight sees prior lambda data");
    expectTrue(combinedPreflight.impulses.can_warm_start(), "combined preflight sees non-zero impulses");
    expectTrue(combinedPreflight.can_warm_start(), "combined island can warm-start");
    testPreflightContactImpulseWarmStartGuards();
    testPreflightWarmStartIslandCombinedGuards();

// --- deepen additive from deepen-b4-pbd-island-dispatch-warmstart-guards-c488 ---
void testShouldSkipIslandDispatchJobGuard() {
    expectTrue(should_skip_island_dispatch_job(invalid, 1.f / 60.f),
    expectTrue(should_skip_island_dispatch_job(invalid, 0.f),
            foundEmptySkip = should_skip_island_dispatch_job(job, 1.f / 60.f);
            foundConstrainedDispatch = !should_skip_island_dispatch_job(job, 1.f / 60.f);
            expectTrue(should_skip_island_dispatch_job(job, 0.f),
    expectTrue(foundEmptySkip, "should_skip_island_dispatch_job covers empty island");
    expectTrue(foundConstrainedDispatch, "should_skip_island_dispatch_job allows constrained island");
        const IslandContactImpulseWarmStartPreflight preflight =
    expectTrue(!constrainedPreflight.invalidDt, "valid dt passes impulse preflight");
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can impulse warm-start");
    expectTrue(invalidDtPreflight.invalidDt, "zero dt fails impulse preflight");
    expectTrue(!invalidDtPreflight.can_warm_start(), "impulse warm-start blocked for invalid dt");
void testWarmStartGraphContactImpulsesGuarded() {

// --- deepen additive from deepen-b4-pbd-island-guards-7571 ---
void testPreflightContactImpulseGraphGuards() {
    const IslandContactImpulseWarmStartGraphPreflight graphPreflight =
    expectTrue(!graphPreflight.skipped, "graph impulse preflight does not skip when warm-startable islands exist");
    expectTrue(graphPreflight.can_warm_start(), "graph impulse preflight can warm-start with impulse data");
    expectTrue(graphPreflight.stats.warmStartableCount == graph.constrainedIslandCount(),
    expectTrue(graphPreflight.stats.emptyCount + graphPreflight.stats.warmStartableCount +
                       graphPreflight.stats.noImpulseDataCount ==
                   graphPreflight.stats.totalIslands,
                   graphPreflight.stats.warmStartableCount,
    expectTrue(!should_skip_contact_impulse_warm_start_graph(graph, contacts),
               "should_skip_contact_impulse_warm_start_graph false when islands can seed");
    expectTrue(should_skip_contact_impulse_warm_start_graph(graph, contacts),
void testPreflightContactImpulseDispatchGuardsDt() {
    const IslandContactImpulseDispatchPreflight validPreflight =
    expectTrue(!validPreflight.skipped, "impulse dispatch preflight does not skip constrained graph");
    expectTrue(!validPreflight.invalidDt, "impulse dispatch preflight accepts positive dt");
    expectTrue(validPreflight.can_warm_start(), "impulse dispatch preflight can warm-start with valid dt");
    expectTrue(!should_skip_contact_impulse_dispatch(graph, contacts, dt),
               "should_skip_contact_impulse_dispatch false with valid dt");
    const IslandContactImpulseDispatchPreflight invalidPreflight =
    expectTrue(invalidPreflight.invalidDt, "impulse dispatch preflight flags invalid dt");
    expectTrue(!invalidPreflight.can_warm_start(), "impulse dispatch preflight cannot warm-start with invalid dt");
    expectTrue(should_skip_contact_impulse_dispatch(graph, contacts, 0.f),
               "should_skip_contact_impulse_dispatch true with invalid dt");
    const IslandCombinedWarmStartPreflight indexPreflight =
    expectTrue(!indexPreflight.skipped, "combined index preflight does not skip constrained island");
    expectTrue(indexPreflight.can_warm_start(), "combined index preflight can warm-start");
    const IslandCombinedWarmStartPreflight outOfRange =
    testPreflightContactImpulseGraphGuards();
    testPreflightContactImpulseDispatchGuardsDt();

// --- deepen additive from deepen-pbd-island-guards-6957 ---
void testPreflightIslandSolveJobGuards() {
    const IslandSolveJobPreflight constrainedPreflight =
    expectTrue(!constrainedPreflight.skipped, "constrained island job preflight not skipped");
    expectTrue(constrainedPreflight.can_dispatch(), "constrained island job can dispatch");
    expectTrue(constrainedPreflight.indices.valid, "constrained island indices are valid");
    const IslandSolveJobPreflight outOfRange =
        const IslandSolveJobPreflight emptyPreflight =
        expectTrue(emptyPreflight.skipped, "empty island job preflight is skipped");
        expectTrue(should_skip_island_solve_index(graph, islandIndex, contacts.size(), constraints.size()),
                   "should_skip_island_solve_index on empty island");
    expectTrue(!should_skip_island_solve_invalid_indices(job, contacts.size(), constraints.size()),
    expectTrue(should_skip_island_solve_invalid_indices(badJob, contacts.size(), constraints.size()),
void testPreflightWarmStartIslandContactImpulses() {
    expectTrue(constrainedPreflight.priorImpulseCoverage == 1u,
    const IslandContactImpulseWarmStartPreflight invalidDt =
    expectTrue(should_skip_warm_start_contact_impulses(graph.island(islandA), contacts, 0.f),
               "should_skip on invalid dt");
        expectTrue(should_skip_warm_start_contact_impulses_index(graph, islandIndex, contacts, dt),
                   "should_skip_contact_impulses_index on empty island");
void testPreflightWarmStartContactImpulsesGraph() {
               "should_skip false when impulses exist");
               "should_skip graph impulse warm-start on empty graph");
void testPreflightWarmStartIslandCombined() {
    const IslandCombinedWarmStartPreflight combined =
    const IslandCombinedWarmStartPreflight lambdaOnly =
    const IslandCombinedWarmStartPreflight emptyCombined =
    testPreflightIslandSolveJobGuards();
    testPreflightWarmStartIslandContactImpulses();
    testPreflightWarmStartContactImpulsesGraph();
    testPreflightWarmStartIslandCombined();

// --- deepen additive from pbd-island-guards-deepen-1f2e ---
void testPreflightIslandSolveInputsGuards() {
    const IslandSolveInputsPreflight validPreflight =
    expectTrue(!validPreflight.skipped, "solve-inputs preflight does not skip constrained island");
    expectTrue(validPreflight.can_solve(), "solve-inputs preflight can solve with in-range indices");
    expectTrue(validPreflight.ownedContactCount == 1u, "solve-inputs preflight counts owned contacts");
    expectTrue(validPreflight.ownedDistanceCount == 1u, "solve-inputs preflight counts owned distances");
    expectTrue(!should_skip_island_solve_inputs(graph.island(constrainedIndex), 1u, 1u),
               "should_skip_island_solve_inputs false for valid inputs");
    const IslandSolveInputsPreflight outOfRangeContacts =
    expectTrue(should_skip_island_solve_inputs(graph.island(constrainedIndex), 0u, 1u),
               "should_skip_island_solve_inputs true for OOB contacts");
    const IslandSolveInputsPreflight outOfRangeIndex =
        const IslandSolveInputsPreflight emptyPreflight = preflight_island_solve_inputs(island, 1u, 1u);
        expectTrue(emptyPreflight.skipped, "solve-inputs preflight skips empty island");
        expectTrue(!emptyPreflight.can_solve(), "empty island cannot pass solve-inputs preflight");
    const IslandContactImpulsePreflight validPreflight =
    expectTrue(!validPreflight.skipped, "impulse preflight does not skip contact island");
    expectTrue(!validPreflight.invalidDt, "impulse preflight accepts valid dt");
    expectTrue(validPreflight.ownedContactCount == 1u, "impulse preflight counts owned contacts");
    expectTrue(validPreflight.inRangeContactCount == 1u, "impulse preflight counts in-range contacts");
    expectTrue(validPreflight.nonZeroImpulseCount == 1u, "impulse preflight counts non-zero impulses");
    expectTrue(validPreflight.can_warm_start(), "impulse preflight can warm-start with non-zero impulse");
    expectTrue(!should_skip_warm_start_contact_impulses(graph.island(islandA), contacts, dt),
               "should_skip false when impulse data exists");
    const IslandContactImpulsePreflight invalidDt =
               "should_skip true when dt is invalid");
    const IslandContactImpulsePreflight outOfRangeIndex =
        const IslandContactImpulsePreflight emptyPreflight =
        expectTrue(should_skip_contact_impulse_warm_start_island_index(graph, islandIndex),
                   "should_skip_contact_impulse_warm_start_island_index on empty island");
void testPreflightCombinedWarmStartGuards() {
    expectTrue(!combinedPreflight.skipped, "combined preflight does not skip contact island");
    expectTrue(combinedPreflight.lambda.can_warm_start(), "combined preflight lambda path can warm-start");
    expectTrue(combinedPreflight.impulse.can_warm_start(), "combined preflight impulse path can warm-start");
    expectTrue(combinedPreflight.can_warm_start(), "combined preflight can warm-start with both paths");
    expectTrue(!should_skip_warm_start_island_combined(graph.island(islandA),
               "should_skip false when combined paths can seed");
    const IslandCombinedWarmStartPreflight noDataPreflight =
    expectTrue(!noDataPreflight.can_warm_start(), "combined preflight cannot warm-start without data and invalid dt");
    expectTrue(should_skip_warm_start_island_combined(graph.island(islandA), contacts, 0.f, {}, {}),
               "should_skip true when combined paths cannot seed");
void testContactImpulseWarmStartGraphBatchGuards() {
    expectTrue(!graphPreflight.skipped, "graph impulse preflight does not skip when seedable islands exist");
    expectTrue(graphPreflight.can_warm_start(), "graph impulse preflight can warm-start");
    expectTrue(graphPreflight.stats.impulseSeedableCount == graph.constrainedIslandCount(),
    expectTrue(graphPreflight.stats.emptyCount + graphPreflight.stats.impulseSeedableCount +
                       graphPreflight.stats.noImpulseDataCount + graphPreflight.stats.invalidDtCount ==
                   graphPreflight.stats.impulseSeedableCount,
               "should_skip_warm_start_contact_impulses_graph false when seedable");
    expectTrue(should_skip_warm_start_contact_impulses_graph(graph, contacts, 0.f),
    testPreflightIslandSolveInputsGuards();
    testPreflightCombinedWarmStartGuards();

// --- deepen additive from deepen-pbd-island-guards-426b ---
void testPreflightDispatchIslandIndexGuards() {
    const IslandJobDispatchPreflight outOfRange =
    expectTrue(should_skip_dispatch_island_index(graph, graph.islandCount() + 1u, 1.f / 60.f),
               "should_skip_dispatch_island_index on out-of-range index");
    const IslandJobDispatchPreflight invalidDt = preflight_dispatch_island_index(graph, 0u, 0.f);
    const IslandJobDispatchPreflight constrained =
    expectTrue(!should_skip_dispatch_island_index(graph, constrainedIndex, 1.f / 60.f),
               "should_skip false for constrained island with valid dt");
    const IslandContactImpulsePreflight constrainedPreflight =
    expectTrue(constrainedPreflight.seedableContactCount == 1u,
    expectTrue(constrainedPreflight.can_warm_start(), "contact island can seed impulse warm-start");
    const IslandContactImpulsePreflight zeroImpulsePreflight =
    expectTrue(!zeroImpulsePreflight.skipped, "impulse preflight does not skip zero-impulse contact island");
    expectTrue(zeroImpulsePreflight.seedableContactCount == 0u,
    expectTrue(!zeroImpulsePreflight.can_warm_start(), "zero impulse island cannot seed");
    const IslandContactImpulsePreflight outOfRange =
    const IslandContactImpulseGraphPreflight preflight =
    expectTrue(!should_skip_contact_impulse_warm_start_graph(graph, contacts, dt),
               "should_skip_contact_impulse_warm_start_graph false when seedable");
    expectTrue(should_skip_contact_impulse_warm_start_graph(emptyGraph, contacts, dt),
    expectTrue(should_skip_contact_impulse_warm_start_graph(graph, contacts, 0.f),
    testPreflightDispatchIslandIndexGuards();

// --- deepen additive from deepen-b4-pbd-island-solver-ac66 ---
    const IslandSolveJobPreflight invalidPreflight = preflight_island_solve_job(invalid);
    expectTrue(invalidPreflight.outOfRange, "null-island job preflight marks out-of-range");
    expectTrue(invalidPreflight.skipped, "null-island job preflight is skipped");
    expectTrue(!invalidPreflight.can_dispatch(), "null-island job cannot dispatch");
        const IslandSolveJobPreflight preflight = preflight_island_solve_job(job);
void testIsValidContactImpulseWarmStartDtGuard() {
void testPreflightIslandContactImpulsesGuards() {
        const IslandContactImpulsePreflight preflight = preflight_island_contact_impulses(island, contacts, dt);
        expectTrue(should_skip_contact_impulse_island(island, contacts, dt),
                   "should_skip_contact_impulse_island on empty island");
    expectTrue(!constrainedPreflight.skipped, "preflight does not skip constrained contact island");
    expectTrue(constrainedPreflight.ownedContactCount == 1u, "preflight counts owned contact slots");
    expectTrue(constrainedPreflight.impulseCoverage == 1u, "preflight counts non-zero impulse coverage");
    expectTrue(constrainedPreflight.can_warm_start(), "constrained island can warm-start impulses");
    const IslandContactImpulsePreflight invalidDtPreflight =
    expectTrue(!invalidDtPreflight.can_warm_start(), "invalid dt blocks impulse warm-start");
    const IslandContactImpulseGraphPreflight preflight = preflight_contact_impulse_graph(graph, contacts, dt);
    expectTrue(!should_skip_contact_impulse_graph(graph, contacts, dt),
               "should_skip_contact_impulse_graph false when impulses exist");
    expectTrue(should_skip_contact_impulse_graph(emptyGraph, contacts, dt),
    const IslandContactImpulseGraphPreflight invalidDtPreflight =
    expectTrue(invalidDtPreflight.invalidDt, "graph impulse preflight marks invalid dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "invalid dt blocks graph impulse warm-start");
    expectTrue(should_skip_contact_impulse_graph(graph, contacts, 0.f),
               "should_skip_contact_impulse_graph on invalid dt");
void testPreflightIslandContactImpulsesByIndex() {
    expectTrue(should_skip_contact_impulse_island_index(graph, graph.islandCount() + 1u, contacts, dt),
               "should_skip_contact_impulse_island_index on out-of-range index");
        expectTrue(emptyPreflight.skipped, "index impulse preflight skips empty island");
        expectTrue(should_skip_contact_impulse_island_index(graph, islandIndex, contacts, dt),
                   "should_skip_contact_impulse_island_index on empty island");
    expectTrue(!constrainedPreflight.skipped, "index impulse preflight does not skip constrained island");
    expectTrue(constrainedPreflight.can_warm_start(), "index impulse preflight can warm-start constrained island");
    testPreflightIslandContactImpulsesGuards();
    testPreflightIslandContactImpulsesByIndex();

// --- deepen additive from deepen-pbd-island-guards-f8cf ---
void testPreflightDispatchIslandJobGuards() {
    const IslandDispatchJobPreflight invalidPreflight = preflight_dispatch_island_job(invalid, 1.f / 60.f);
    expectTrue(invalidPreflight.emptyJob, "default job preflight marks empty job");
    expectTrue(should_skip_dispatch_island_job(invalid, 1.f / 60.f),
               "should_skip_dispatch_island_job on default job");
        const IslandDispatchJobPreflight preflight = preflight_dispatch_island_job(job, 1.f / 60.f);
        const IslandDispatchJobPreflight invalidDt = preflight_dispatch_island_job(job, 0.f);
    expectTrue(constrainedPreflight.impulseCoverage == 1u, "impulse preflight counts warm impulses");
        expectTrue(should_skip_warm_start_contact_impulses_island(island),
                   "should_skip_warm_start_contact_impulses_island on empty island");
    const IslandContactImpulseGraphPreflight graphPreflight =
    expectTrue(!graphPreflight.skipped, "graph impulse preflight does not skip when impulses exist");
               "should_skip false when impulse islands exist");
               "should_skip impulse graph warm-start on empty graph");
    testPreflightDispatchIslandJobGuards();

// --- deepen additive from deepen-pbd-island-guards-b61c ---
void testPreflightIslandDispatchFromJobs() {
    const IslandDispatchJobPreflight validPreflight = preflight_island_dispatch_from_jobs(jobs, 1.f / 60.f);
    expectTrue(!validPreflight.invalidDt, "job preflight accepts valid dt");
    expectTrue(!validPreflight.skipped, "job preflight does not skip constrained jobs");
    expectTrue(validPreflight.can_dispatch(), "job preflight can dispatch constrained jobs");
    expectTrue(validPreflight.stats.dispatchableCount == graph.constrainedIslandCount(),
    expectTrue(count_dispatchable_jobs(jobs) == validPreflight.stats.dispatchableCount,
    expectTrue(!should_skip_island_dispatch_from_jobs(jobs, 1.f / 60.f),
               "should_skip false for constrained jobs with valid dt");
    const IslandDispatchJobPreflight invalidDtPreflight = preflight_island_dispatch_from_jobs(jobs, 0.f);
    expectTrue(!invalidDtPreflight.can_dispatch(), "job preflight blocked on invalid dt");
    expectTrue(should_skip_island_dispatch_from_jobs(jobs, 0.f),
               "should_skip_island_dispatch_from_jobs on invalid dt");
    const IslandDispatchJobPreflight emptyPreflight = preflight_island_dispatch_from_jobs(emptyJobs, 1.f / 60.f);
    expectTrue(emptyPreflight.skipped, "job preflight skips empty job list");
               "should_skip_warm_start_contact_impulses_graph false when impulses exist");
    const IslandContactImpulseWarmStartGraphPreflight invalidDtPreflight =
    expectTrue(invalidDtPreflight.invalidDt, "impulse graph preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "impulse graph preflight blocked on invalid dt");
               "should_skip_warm_start_contact_impulses_graph on invalid dt");
void testPreflightWarmStartCombinedGraphAndIndex() {
    const IslandCombinedWarmStartGraphPreflight preflight =
    expectTrue(!should_skip_warm_start_combined_graph(graph, contacts, dt, priorDistance, priorContact),
               "should_skip_warm_start_combined_graph false when islands can seed");
    expectTrue(should_skip_warm_start_combined_island_index(graph, graph.islandCount() + 1u, dt),
               "should_skip_warm_start_combined_island_index on out-of-range index");
        expectTrue(should_skip_warm_start_combined_island_index(graph, islandIndex, dt),
                   "should_skip_warm_start_combined_island_index on empty island");
    const IslandCombinedWarmStartGraphPreflight invalidDtPreflight =
    expectTrue(invalidDtPreflight.invalidDt, "combined graph preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_warm_start(), "combined graph preflight blocked on invalid dt");
    testPreflightIslandDispatchFromJobs();
    testPreflightWarmStartCombinedGraphAndIndex();

// --- deepen additive from pbd-island-guards-deepen-0fe3 ---
void testPreflightSolveIslandJobStaleIndices() {
    const IslandSolveJobPreflight validPreflight =
    expectTrue(!validPreflight.skipped, "valid job preflight does not skip constrained island");
    expectTrue(!validPreflight.hasStaleIndices, "valid job has no stale indices");
    expectTrue(validPreflight.validContactCount == 1u, "valid job counts owned contacts");
    expectTrue(validPreflight.validDistanceCount == 0u, "valid job has no distance constraints");
    expectTrue(validPreflight.can_solve(), "valid job can solve");
    expectTrue(!should_skip_solve_island_job_stale(validJob, contacts, constraints),
               "should_skip_solve_island_job_stale false for valid job");
    const IslandSolveJobPreflight stalePreflight =
    expectTrue(stalePreflight.hasStaleIndices, "truncated contacts mark stale indices");
    expectTrue(stalePreflight.staleContactCount == 1u, "stale preflight counts stale contacts");
    expectTrue(!stalePreflight.can_solve(), "stale job cannot solve");
    expectTrue(should_skip_solve_island_job_stale(validJob, truncatedContacts, constraints),
               "should_skip_solve_island_job_stale on stale contacts");
    const IslandSolveJobPreflight emptyPreflight = preflight_solve_island_job(emptyJob, contacts, constraints);
    expectTrue(emptyPreflight.skipped, "empty job preflight is skipped");
void testPreflightDispatchIslandByIndex() {
    const IslandDispatchIndexPreflight validPreflight =
    expectTrue(!validPreflight.skipped, "dispatch index preflight does not skip constrained island");
    expectTrue(!validPreflight.invalidDt, "dispatch index preflight accepts valid dt");
    expectTrue(validPreflight.can_dispatch(), "dispatch index preflight can dispatch constrained island");
    expectTrue(should_solve_island(validPreflight.job), "dispatch index preflight extracts dispatchable job");
    expectTrue(!should_skip_dispatch_island_index(graph, constrainedIndex, dt),
               "should_skip_dispatch_island_index false for constrained island");
    const IslandDispatchIndexPreflight invalidDt =
    expectTrue(should_skip_dispatch_island_index(graph, constrainedIndex, 0.f),
               "should_skip_dispatch_island_index on invalid dt");
    const IslandDispatchIndexPreflight outOfRange =
    expectTrue(should_skip_dispatch_island_index(graph, graph.islandCount() + 1u, dt),
        const IslandDispatchIndexPreflight emptyPreflight =
        expectTrue(emptyPreflight.skipped, "dispatch index preflight skips empty island");
        expectTrue(should_skip_dispatch_island_index(graph, islandIndex, dt),
                   "should_skip_dispatch_island_index on empty island");
void testPreflightWarmStartCombinedGraphGuards() {
               "should_skip_warm_start_combined_graph false when prior data exists");
    expectTrue(!indexPreflight.skipped, "combined index preflight does not skip contact island");
    expectTrue(indexPreflight.can_warm_start(), "combined index preflight can warm-start contact island");
    expectTrue(should_skip_warm_start_combined_graph(zeroImpulseGraph, zeroImpulseContacts, dt, {}, {}),
    testPreflightSolveIslandJobStaleIndices();
    testPreflightDispatchIslandByIndex();
    testPreflightWarmStartCombinedGraphGuards();

// --- deepen additive from deepen-pbd-island-guards-faad ---
    const IslandBuildPreflight preflight = preflight_island_build(3, contacts, constraints);
    expectTrue(contactBuildRejectReason(contacts[0], 3) == IslandBuildRejectReason::None,
    expectTrue(contactBuildRejectReason(contacts[1], 3) == IslandBuildRejectReason::InvalidContact,
    expectTrue(contactBuildRejectReason(contacts[2], 3) == IslandBuildRejectReason::SelfPair,
    expectTrue(contactBuildRejectReason(contacts[3], 3) == IslandBuildRejectReason::OutOfRangeBody,
    expectTrue(emptyPreflight.skipped, "build preflight skips empty inputs");
void testValidateIslandIndicesGuard() {
void testPreflightSleepPassGuards() {
    const SleepPassPreflight preflight = preflight_sleep_pass(bodies, linearThreshold, angularThreshold);
    const SleepPassPreflight inactivePreflight =
    expectTrue(inactivePreflight.skipped, "sleep preflight skips when no active dynamic bodies");
    expectTrue(should_skip_sleep_pass(inactive, linearThreshold, angularThreshold),
               "should_skip_sleep_pass on inactive scene");
void testPreflightWakeCandidatesGuards() {
    const WakePreflight preflight = preflight_wake_candidates(bodies, linearThreshold, angularThreshold);
    const WakePreflight none = preflight_wake_candidates(bodies, linearThreshold, angularThreshold);
void testIslandInactiveAndConstraintPreflights() {
    expectTrue(should_skip_island_solve_all_inactive(bodies, island),
               "should_skip_island_solve_all_inactive on sleeping island");
    expectTrue(!should_skip_island_solve_all_inactive(bodies, island),
    const IslandSolveWorkPreflight workPreflight = preflight_island_solve_work(bodies, work);
    expectTrue(workPreflight.insufficientBufferCapacity, "work preflight flags insufficient buffer");
    expectTrue(!workPreflight.can_solve(), "work preflight cannot solve with undersized buffer");
    const IslandSolveWorkPreflight sufficient = preflight_island_solve_work(bodies, work);
    const IslandConstraintIndexPreflight indexPreflight =
    expectTrue(!indexPreflight.skipped, "constraint index preflight does not skip constrained island");
    expectTrue(indexPreflight.indices_valid(), "constraint indices valid for consistent graph");
    expectTrue(indexPreflight.ownedDistanceCount == 1u, "constraint index preflight counts distance slots");
    const IslandConstraintIndexPreflight oobPreflight =
    expectTrue(oobPreflight.skipped, "constraint index preflight skips out-of-range island");
    testPreflightSleepPassGuards();
    testPreflightWakeCandidatesGuards();
    testIslandInactiveAndConstraintPreflights();

// --- deepen additive from deepen-pbd-island-guards-88d5 ---
void testPreflightDispatchableIslandJobs() {
    const IslandDispatchJobBatchPreflight validPreflight = preflight_dispatchable_island_jobs(graph, jobs, dt);
    expectTrue(!validPreflight.skipped, "job batch preflight does not skip dispatchable jobs");
    expectTrue(validPreflight.can_dispatch(), "job batch preflight can dispatch constrained jobs");
    expectTrue(validPreflight.jobCount == jobs.size(), "job batch preflight records job count");
    expectTrue(!should_skip_dispatchable_island_jobs(jobs, dt),
               "should_skip false for dispatchable job batch with valid dt");
    const IslandDispatchJobBatchPreflight invalidDtPreflight =
    expectTrue(invalidDtPreflight.graph.invalidDt, "job batch preflight rejects invalid dt");
    expectTrue(!invalidDtPreflight.can_dispatch(), "job batch preflight cannot dispatch with invalid dt");
    expectTrue(should_skip_dispatchable_island_jobs(jobs, 0.f),
               "should_skip_dispatchable_island_jobs on invalid dt");
    expectTrue(should_skip_dispatchable_island_jobs(emptyJobs, dt),
               "should_skip_dispatchable_island_jobs on empty job list");
    const IslandDispatchJobBatchPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "job batch preflight skips empty job list");
               "should_skip false for impulse graph with valid dt");
    expectTrue(invalidDtPreflight.invalidDt, "impulse graph preflight rejects invalid dt");
    expectTrue(should_skip_warm_start_combined_island_index(graph, graph.bodyIsland(0), 0.f),
               "should_skip_warm_start_combined_island_index on invalid dt");
               "should_skip false for combined graph with valid dt");
    testPreflightDispatchableIslandJobs();

// --- deepen additive from pbd-island-sleep-build-preflights-cb2c ---
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
void testPreflightIslandConstraintSolveGuards() {
    const IslandConstraintSolvePreflight validPreflight =
    expectTrue(!validPreflight.skipped, "constraint preflight does not skip constrained island");
    expectTrue(!validPreflight.invalidDt, "constraint preflight accepts valid dt");
    expectTrue(validPreflight.resolvableConstraintCount == 3u,
    expectTrue(validPreflight.can_solve(), "constrained island can solve");
    const IslandConstraintSolvePreflight invalidDtPreflight =
    expectTrue(invalidDtPreflight.invalidDt, "constraint preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_solve(), "constraint preflight cannot solve with invalid dt");
    const IslandConstraintSolvePreflight outOfRangeContactPreflight =
    expectTrue(outOfRangeContactPreflight.outOfRangeContactCount == 1u,
    expectTrue(outOfRangeContactPreflight.outOfRangeDistanceCount == 0u,
    expectTrue(outOfRangeContactPreflight.resolvableConstraintCount == 2u,
    const IslandConstraintSolvePreflight outOfRangeDistancePreflight =
    expectTrue(outOfRangeDistancePreflight.outOfRangeDistanceCount == 1u,
    expectTrue(outOfRangeDistancePreflight.resolvableConstraintCount == 0u,
    const IslandConstraintSolvePreflight indexPreflight =
    expectTrue(indexPreflight.can_solve(), "index constraint preflight can solve constrained island");
    const IslandSleepPreflight awakeSleepPreflight =
    expectTrue(!awakeSleepPreflight.allSleeping, "awake island is not all-sleeping");
    expectTrue(!awakeSleepPreflight.can_skip_solve(), "awake island cannot skip solve for sleep");
    const IslandSleepPreflight sleepingPreflight =
    expectTrue(sleepingPreflight.allSleeping, "all-sleeping island flagged");
    expectTrue(sleepingPreflight.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(sleepingIsland)),
               "should_skip_island_solve_for_sleep on all-sleeping island");
    const IslandWakePreflight awakeWakePreflight =
    expectTrue(awakeWakePreflight.should_wake(), "awake constrained island should wake");
    expectTrue(awakeWakePreflight.awakeDynamicCount == 2u,
            expectTrue(!should_skip_island_solve_for_sleep(bodies, island),
    const IslandSolveSleepPreflight solvePreflight = preflight_island_solve_sleep_by_index(
    expectTrue(solvePreflight.can_solve(), "combined sleep preflight can solve awake island");
    const IslandSolveSleepPreflight sleepingSolvePreflight = preflight_island_solve_sleep_by_index(
    expectTrue(!sleepingSolvePreflight.can_solve(), "combined sleep preflight skips all-sleeping island");
void testDispatchAllIslandsSleepGuarded() {
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(bodies, graph, dt);
    expectTrue(!should_skip_island_dispatch_sleep(bodies, graph, dt),
               "should_skip false for graph with solvable island");
    testPreflightIslandConstraintSolveGuards();

// --- deepen additive from deepen-pbd-island-guards-a261 ---
void testPreflightIslandGraphBuildGuards() {
    const IslandBuildPreflight zeroBodies = preflight_island_graph_build(0, contacts, constraints);
    const IslandBuildPreflight validBuild = preflight_island_graph_build(5, contacts, constraints);
    expectTrue(should_skip_island_graph_build(0u), "should_skip_island_graph_build for zero bodies");
    expectTrue(!should_skip_island_graph_build(5u), "should_skip_island_graph_build allows positive count");
    const IslandSolveJobPreflight awakePreflight =
    expectTrue(!awakePreflight.skipped, "solve preflight does not skip constrained island");
    expectTrue(awakePreflight.can_solve(), "awake island has solvable constraints");
    expectTrue(awakePreflight.solvableConstraintPairs == 2u,
    const IslandSolveJobPreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.skipped, "solve preflight does not skip sleeping island shell");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island has no solvable pairs");
    expectTrue(sleepingPreflight.immovableConstraintPairs == 1u,
    expectTrue(should_skip_solve_island_all_sleeping(graph.island(sleepingIsland), bodies),
               "should_skip_solve_island_all_sleeping for sleeping island");
    expectTrue(!should_skip_solve_island_all_sleeping(graph.island(awakeIsland), bodies),
    expectTrue(should_skip_solve_island_all_static(staticGraph.island(0), staticBodies),
               "should_skip_solve_island_all_static for static island");
    expectTrue(should_skip_solve_island_job_preflight(sleepingPreflight),
               "should_skip_solve_island_job_preflight when nothing is solvable");
    const IslandSolveJobPreflight oobPreflight =
    expectTrue(oobPreflight.outOfRangeDistances == 1u, "solve preflight counts out-of-range distance index");
    const IslandSolveJobPreflight invalidIndexPreflight =
    expectTrue(invalidIndexPreflight.skipped, "solve preflight by index skips out-of-range island");
    const IslandSleepPreflight sleepPreflight =
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
    const IslandSleepWakePreflight wakePreflight =
    expectTrue(!wakePreflight.skipped, "sleep/wake preflight does not skip contact island");
    expectTrue(wakePreflight.activeContactCount == 1u, "sleep/wake preflight counts active contacts");
    expectTrue(wakePreflight.contactsTouchingSleepingBody == 1u,
    expectTrue(wakePreflight.should_wake, "sleep/wake preflight requests wake on mixed contact");
    const IslandSleepPreflight invalidSleepPreflight =
    expectTrue(invalidSleepPreflight.skipped, "sleep preflight by index skips out-of-range island");
    const IslandSleepWakePreflight invalidWakePreflight =
    expectTrue(invalidWakePreflight.skipped, "sleep/wake preflight by index skips out-of-range island");
    testPreflightIslandGraphBuildGuards();

// --- deepen additive from deepen-pbd-island-sleep-build-guards-9e33 ---
    const IslandBuildPreflight validPreflight = preflight_island_build(4, contacts, constraints);
    expectTrue(!validPreflight.invalidBodyCount, "valid build preflight accepts in-range indices");
    expectTrue(validPreflight.can_build(), "valid build preflight can build");
    expectTrue(validPreflight.validContactCount == 1u, "build preflight counts valid contact");
    expectTrue(validPreflight.validDistanceCount == 2u, "build preflight counts valid distance constraints");
    expectTrue(!should_skip_island_build(4, contacts, constraints),
               "should_skip false for valid build inputs");
    const IslandBuildPreflight invalidPreflight = preflight_island_build(2, contacts, constraints);
    expectTrue(invalidPreflight.invalidBodyCount, "build preflight rejects out-of-range distance indices");
    expectTrue(invalidPreflight.outOfRangeDistanceCount == 1u,
    expectTrue(!invalidPreflight.can_build(), "out-of-range build preflight cannot build");
    expectTrue(should_skip_island_build(2, contacts, constraints),
               "should_skip true for out-of-range build inputs");
void testPreflightIslandSleepGuards() {
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepingPreflight.dynamicBodyCount == 2u, "sleep preflight counts dynamic bodies");
    expectTrue(sleepingPreflight.sleepingBodyCount == 1u, "sleep preflight counts sleeping body");
    expectTrue(sleepingPreflight.awakeBodyCount == 1u, "sleep preflight counts awake body");
    expectTrue(!sleepingPreflight.allSleeping, "mixed island is not all sleeping");
    expectTrue(!should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
    const IslandSleepPreflight allSleepingPreflight =
    expectTrue(allSleepingPreflight.allSleeping, "all dynamic bodies sleeping marks allSleeping");
    expectTrue(allSleepingPreflight.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_sleeping_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip_sleeping true for all-sleeping island");
    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(graphPreflight.stats.allSleepingCount >= 1u,
    expectTrue(should_skip_awake_island_dispatch(graph, bodies),
void testPreflightIslandWakeGuards() {
    const IslandWakePreflight neighborPreflight =
    expectTrue(!neighborPreflight.skipped, "wake preflight does not skip contact island");
    expectTrue(neighborPreflight.hasAwakeNeighborContact, "wake preflight sees awake neighbor contact");
    expectTrue(neighborPreflight.should_wake(), "sleeping body should wake on awake neighbor contact");
    const IslandWakePreflight forcePreflight =
    expectTrue(forcePreflight.wakeCandidateCount > 0u, "wake preflight counts force-driven wake candidate");
void testSolveIslandPreflightAndAwakeDispatch() {
    const IslandConstraintSolvePreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.can_solve(), "constraint preflight skips all-sleeping island");
    expectTrue(should_skip_solve_island_preflight(graph.island(sleepingIsland), bodies, constraints, work, dt),
               "should_skip_solve true for all-sleeping island");
    const IslandConstraintSolvePreflight awakePreflight =
    expectTrue(awakePreflight.can_solve(), "constraint preflight allows awake island");
    testPreflightIslandSleepGuards();
    testPreflightIslandWakeGuards();
    testSolveIslandPreflightAndAwakeDispatch();

// --- deepen additive from pbd-island-guards-deepen-5934 ---
    const IslandBuildPreflight zeroBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(should_skip_island_build(0), "should_skip_island_build on zero bodies");
void testPreflightIslandConstraintIndices() {
    const IslandConstraintIndexPreflight preflight =
    const IslandConstraintIndexPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "constraint index preflight skips empty island");
void testPreflightSleepingIslandGuards() {
    expectTrue(sleepingPreflight.allSleeping, "sleep preflight detects all-sleeping island");
    expectTrue(sleepingPreflight.movableBodyCount == 0u, "sleep preflight reports zero movable bodies");
    expectTrue(!sleepingPreflight.can_solve(), "sleep preflight cannot solve all-sleeping island");
               "should_skip_sleeping_island_solve on all-sleeping island");
    const IslandSleepPreflight awakePreflight =
    expectTrue(awakePreflight.movableBodyCount == 1u, "sleep preflight sees awake lone body");
    expectTrue(awakePreflight.can_solve(), "sleep preflight can solve awake island");
    expectTrue(!should_skip_sleeping_island_solve(graph.island(awakeIsland), bodies),
               "should_skip_sleeping_island_solve false for awake island");
    expectTrue(should_skip_sleeping_island_solve_job(sleepingJob, bodies),
               "should_skip_sleeping_island_solve_job on all-sleeping island");
    const IslandSleepPreflight oobPreflight =
    expectTrue(oobPreflight.skipped, "sleep preflight by index skips out-of-range island");
void testPreflightWakeOnImpulseGuards() {
    const WakeOnImpulsePreflight noImpulse = preflight_wake_on_impulse(bodies, 0);
    const WakeOnImpulsePreflight withImpulse = preflight_wake_on_impulse(bodies, 0);
    const WakeOnImpulsePreflight oob = preflight_wake_on_impulse(bodies, 99);
void testPreflightIslandBodyPartition() {
    const IslandBodyPartitionPreflight preflight = preflight_island_body_partition(graph);
void testPreflightConstraintIterations() {
    const ConstraintIterationPreflight preflight = preflight_constraint_iterations(params);
    expectTrue(!should_skip_constraint_iterations(params),
               "should_skip_constraint_iterations false for positive iterations");
    const ConstraintIterationPreflight zeroPreflight = preflight_constraint_iterations(params);
    expectTrue(zeroPreflight.skipped, "constraint iteration preflight skips zero iterations");
    expectTrue(!zeroPreflight.can_iterate(), "constraint iteration preflight cannot iterate at zero");
    expectTrue(should_skip_constraint_iterations(params),
               "should_skip_constraint_iterations true for zero iterations");
void testDispatchSolveIslandSleepGuarded() {
    testPreflightIslandConstraintIndices();
    testPreflightSleepingIslandGuards();
    testPreflightWakeOnImpulseGuards();
    testPreflightIslandBodyPartition();
    testPreflightConstraintIterations();

// --- deepen additive from deepen-pbd-island-guards-5425 ---
    expectTrue(!should_skip_island_build(4), "should not skip build with positive body count");
    const IslandBuildPreflight zeroPreflight = preflight_island_build(0, contacts, constraints);
    expectTrue(zeroPreflight.zeroBodies, "zero body count flagged");
    expectTrue(zeroPreflight.skipped, "zero body build preflight skipped");
    expectTrue(!zeroPreflight.can_build(), "zero body count cannot build");
void testBuildIslandGraphGuarded() {
void testIslandSleepSolvePreflight() {
    const IslandSleepSolvePreflight sleepingPreflight =
    expectTrue(sleepingPreflight.stats.sleepingCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepingPreflight.stats.awakeDynamicCount == 0u, "sleep preflight sees no awake dynamics");
    expectTrue(sleepingPreflight.allDynamicSleeping, "all dynamic bodies marked sleeping");
    expectTrue(!sleepingPreflight.can_solve(), "sleeping island cannot solve");
    const IslandSleepSolvePreflight awakePreflight =
    expectTrue(awakePreflight.skipped, "sleep preflight skips empty island");
    expectTrue(should_skip_sleeping_island_solve_index(graph, graph.islandCount() + 1u, bodies),
void testIslandWakePreflight() {
    const IslandWakePreflight preflight =
    expectTrue(!should_skip_island_sleep_detection(graph.island(0), bodies),
    expectTrue(should_skip_island_sleep_detection(graph.island(0), staticBodies),
void testSolveIslandJobPreflightGuards() {
    const IslandSolveJobPreflight invalidDt =
    expectTrue(sleepingPreflight.sleep.allDynamicSleeping, "solve job preflight sees sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "solve job preflight blocks all-sleeping island");
    expectTrue(should_skip_solve_island_job_preflight(graph.island(0), bodies, 1.f / 60.f),
               "should_skip_solve_island_job_preflight on sleeping island");
    expectTrue(awakePreflight.can_solve(), "solve job preflight allows awake island");
    testIslandSleepSolvePreflight();
    testIslandWakePreflight();
    testSolveIslandJobPreflightGuards();

// --- deepen additive from deepen-pbd-island-guards-ac8e ---
    const IslandBuildPreflight emptyBodies = preflight_island_build(0, contacts, constraints);
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::EmptyBodyCount),
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip_island_build on zero bodies");
    const IslandBuildPreflight invalidContact = preflight_island_build(2, contacts, {});
    expectTrue(island_build_rejects_for_reason(2, contacts, {}, IslandBuildRejectReason::InvalidContactBodyIndex),
    const IslandBuildPreflight invalidDistance = preflight_island_build(2, contacts, constraints);
    const IslandBuildPreflight validBuild = preflight_island_build(2, contacts, constraints);
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyBodyCount),
void testPreflightIslandSleepAndWakeGuards() {
    expectTrue(sleepingPreflight.fullySleeping, "sleep preflight marks fully sleeping island");
    expectTrue(!sleepingPreflight.can_solve(), "fully sleeping island cannot solve");
    expectTrue(should_skip_solve_sleeping_island(bodies, graph.island(sleepingIsland)),
               "should_skip_solve_sleeping_island on sleeping island");
    expectTrue(!awakePreflight.fullySleeping, "awake dynamic body prevents fully sleeping flag");
    expectTrue(awakePreflight.can_solve(), "awake island can solve");
    expectTrue(awakePreflight.stats.activeCount == 1u, "sleep stats count active dynamic body");
    const IslandWakePreflight forceWake =
    const IslandWakePreflight velocityWake =
    expectTrue(should_skip_solve_sleeping_island_index(bodies, graph, graph.islandCount() + 1u),
               "should_skip_solve_sleeping_island_index on out-of-range");
    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(bodies, graph);
    expectTrue(graphPreflight.fullySleepingIslandCount >= 1u,
    expectTrue(should_skip_island_sleep_dispatch(bodies, graph),
    const IslandSleepGraphPreflight awakeGraphPreflight = preflight_island_sleep_graph(bodies, graph);
    expectTrue(awakeGraphPreflight.has_active_islands(), "sleep graph preflight sees active islands");
    expectTrue(!should_skip_island_sleep_dispatch(bodies, graph),
    const IslandConstraintSolvePreflight sleepingSolve =
    expectTrue(should_skip_island_constraint_solve(bodies, graph.island(sleepingIsland), dt),
               "should_skip_island_constraint_solve on sleeping island");
    const IslandConstraintSolvePreflight invalidDt =
    const IslandConstraintSolvePreflight awakeSolve =
    const IslandConstraintSolvePreflight outOfRange =
    testPreflightIslandSleepAndWakeGuards();

// --- deepen additive from deepen-pbd-island-build-sleep-wake-cc0e ---
    const IslandBuildPreflight emptyPreflight = preflight_island_build(0u, contacts, constraints);
    expectTrue(emptyPreflight.skipped, "build preflight skips zero body count");
    expectTrue(emptyPreflight.reason == IslandBuildRejectReason::EmptyBodyCount,
    expectTrue(!emptyPreflight.can_build(), "build preflight cannot build with zero bodies");
    expectTrue(should_skip_island_build(0u, contacts, constraints),
               "should_skip_island_build on zero body count");
    const IslandBuildPreflight validPreflight = preflight_island_build(2u, {}, constraints);
    expectTrue(!validPreflight.skipped, "build preflight does not skip positive body count");
    expectTrue(validPreflight.can_build(), "build preflight can build with bodies");
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(bodies, graph.island(0));
    expectTrue(!sleepPreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(sleepPreflight.sleepingBodyCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(sleepPreflight.awakeBodyCount == 0u, "sleep preflight reports zero awake bodies");
    expectTrue(sleepPreflight.allDynamicSleeping, "sleep preflight marks all-dynamic-sleeping island");
    expectTrue(!sleepPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_island_solve_sleeping(bodies, graph.island(0)),
               "should_skip_island_solve_sleeping on all-sleeping island");
    const IslandSleepPreflight awakePreflight = preflight_island_sleep(bodies, graph.island(0));
    expectTrue(awakePreflight.awakeBodyCount == 1u, "sleep preflight counts one awake body");
    expectTrue(!awakePreflight.allDynamicSleeping, "mixed island is not all-sleeping");
    expectTrue(awakePreflight.can_solve(), "mixed island can solve");
        const IslandSleepPreflight emptyPreflight = preflight_island_sleep(bodies, island);
        expectTrue(emptyPreflight.skipped, "sleep preflight skips empty island");
    expectTrue(!graphPreflight.skipped, "sleep graph preflight does not skip constrained graph");
    expectTrue(graphPreflight.has_awake_islands(), "sleep graph preflight sees awake island");
    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, graph.island(0));
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip constrained island");
    expectTrue(wakePreflight.sleepingBodyCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts force-driven wake candidate");
    expectTrue(wakePreflight.needs_wake(), "wake preflight needs wake when force applied");
    expectTrue(should_skip_island_wake_check(graph.island(0)) == false,
        expectTrue(should_skip_island_wake_check(island), "should_skip_island_wake_check on empty island");
        const IslandWakePreflight emptyWake = preflight_island_wake(bodies, island);
        const IslandWakePreflight indexWake =
    const IslandConstraintSolvePreflight constrainedPreflight =
    expectTrue(constrainedPreflight.can_solve(), "constraint solve preflight allows awake island");
    expectTrue(!should_skip_island_constraint_solve(bodies, graph, graph.bodyIsland(2), dt),
               "should_skip false for awake constrained island");
    const IslandConstraintSolvePreflight sleepingConstraintPreflight =
    expectTrue(!sleepingConstraintPreflight.can_solve(),
    expectTrue(should_skip_island_constraint_solve(bodies, graph, graph.bodyIsland(0), dt),
               "should_skip true for all-sleeping island");

// --- deepen additive from deepen-pbd-island-guards-2fdd ---
    expectTrue(emptyPreflight.emptyBodyCount, "build preflight marks empty body count");
    expectTrue(validPreflight.can_build(), "build preflight can build with positive body count");
    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(bodies, sleepingIsle);
    expectTrue(sleepingPreflight.allSleeping, "both-sleeping island is all sleeping");
    expectTrue(sleepingPreflight.allStaticOrSleeping, "both-sleeping island has no active dynamics");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot solve");
    expectTrue(should_skip_solve_sleeping_island(bodies, sleepingIsle),
               "should_skip_solve_sleeping_island on all-sleeping island");
    const IslandSleepPreflight activePreflight = preflight_island_sleep(bodies, activeIsle);
    expectTrue(activePreflight.stats.activeDynamicCount == 2u,
    expectTrue(activePreflight.can_solve(), "active island can solve");
    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, sleepingIsle);
    expectTrue(wakePreflight.forceWakeCount == 1u, "sleep preflight counts force wake candidate");
    expectTrue(wakePreflight.can_wake(), "sleeping island with force can wake");
    const IslandWakePreflight activeWakePreflight = preflight_island_wake(bodies, activeIsle);
    expectTrue(!activeWakePreflight.can_wake(), "active-only island has no wake candidates");
    expectTrue(should_skip_island_wake(bodies, activeIsle), "should_skip_island_wake on active island");
void testWakeIslandBodiesGuarded() {
    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve(
    expectTrue(!sleepingPreflight.skipped, "constraint preflight does not skip constrained island");
    expectTrue(sleepingPreflight.resolvableConstraintCount == 0u,
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot constraint-solve");
    const IslandConstraintSolvePreflight activePreflight = preflight_island_constraint_solve(
    expectTrue(activePreflight.resolvableConstraintCount == 2u,
    expectTrue(activePreflight.can_solve(), "active island can constraint-solve");
    const IslandConstraintSolvePreflight invalidDtPreflight = preflight_island_constraint_solve(
void testSolveIslandJobGuarded() {

// --- deepen additive from deepen-pbd-island-guards-fd7c ---
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodies,
    expectTrue(island_build_rejects_for_reason(0, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
    const IslandBuildPreflight noConstraints = preflight_island_build(3, {}, {});
    expectTrue(noConstraints.reason == IslandBuildRejectReason::NoConstraints,
    const IslandBuildPreflight constrained = preflight_island_build(2, contacts, constraints);
    expectTrue(constrained.reason == IslandBuildRejectReason::None,
void testPreflightIslandSolveBodiesSleepGuards() {
    const IslandSolveBodyPreflight sleepingPreflight =
    expectTrue(sleepingPreflight.allSleeping, "solve preflight flags all-sleeping island");
    expectTrue(should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
    const IslandSolveBodyPreflight awakePreflight =
    expectTrue(awakePreflight.can_solve(), "mixed island can solve with awake dynamic body");
    expectTrue(!should_skip_island_solve_for_sleep(bodies, graph.island(constrainedIndex)),
               "should_skip false when island has awake dynamic body");
    expectTrue(should_skip_island_solve_job_for_sleep(job, bodies, graph.island(constrainedIndex)),
               "should_skip_island_solve_job_for_sleep on all-sleeping island");
void testDispatchSolveIslandWithBodyGuards() {
    expectTrue(sleepPreflight.can_sleep(), "sleep preflight allows low-velocity contact island");
    expectTrue(sleepPreflight.dynamicCount == 2u, "sleep preflight counts dynamic bodies");
    const IslandSleepPreflight invalidDt =
    expectTrue(should_skip_island_sleep(graph.island(islandA), 0.f),
               "should_skip_island_sleep on invalid dt");
    expectTrue(wakePreflight.should_wake(), "wake preflight sees above-threshold velocity");
    expectTrue(wakePreflight.aboveThresholdCount == 1u,
void testSleepWakeIslandGuardedBatch() {
    const IslandSleepGraphPreflight graphSleep =
    testPreflightIslandSolveBodiesSleepGuards();

// --- deepen additive from deepen-pbd-island-guards-bda2 ---
    expectTrue(emptyBodies.reason == IslandBuildRejectReason::EmptyBodyCount,
    const IslandBuildPreflight loneBodies = preflight_island_build(2, {}, {});
    expectTrue(!should_skip_island_build(2, {}, {}), "should_skip false for lone bodies");
    const IslandBuildPreflight constrained = preflight_island_build(3, contacts, constraints);
    expectTrue(!awakePreflight.skipped, "constraint solve preflight does not skip awake island");
    expectTrue(awakePreflight.can_solve(), "awake constrained island can solve");
    expectTrue(awakePreflight.constraintCount == 1u, "constraint solve preflight counts constraints");
    expectTrue(awakePreflight.awakeDynamicCount == 2u, "constraint solve preflight counts awake dynamics");
    expectTrue(!should_skip_island_constraint_solve(bodies, graph.island(constrainedIndex), 1.f / 60.f),
    const IslandConstraintSolvePreflight allSleepingPreflight =
    expectTrue(allSleepingPreflight.allDynamicSleeping, "constraint solve preflight detects all sleeping");
    expectTrue(!allSleepingPreflight.can_solve(), "all-sleeping island cannot constraint solve");
    expectTrue(should_skip_island_constraint_solve(bodies, graph.island(constrainedIndex), 1.f / 60.f),
               "should_skip true when all dynamic bodies sleeping");
        expectTrue(should_skip_island_constraint_solve_index(bodies, graph, islandIndex, 1.f / 60.f),
                   "should_skip constraint solve on empty island");
    expectTrue(!sleepPreflight.skipped, "sleep preflight does not skip contact island");
    expectTrue(sleepPreflight.can_consider_sleep(), "slow contact island can consider sleep");
    expectTrue(sleepPreflight.belowThresholdCount == 2u, "sleep preflight counts below-threshold bodies");
    expectTrue(!sleepPreflight.all_dynamic_sleeping(), "contact island is not all sleeping yet");
    expectTrue(allSleepingPreflight.all_dynamic_sleeping(), "sleep preflight detects all-sleeping island");
    expectTrue(should_skip_island_sleep_index(graph, graph.islandCount() + 2u),
               "should_skip sleep index on out-of-range island");
    const IslandWakePreflight wakeSleeping =
    const IslandWakePreflight wakeContact =
        expectTrue(should_skip_island_sleep_check(island), "should_skip sleep check on empty island");
        expectTrue(should_skip_island_wake_check(island), "should_skip wake check on empty island");

// --- deepen additive from pbd-island-guards-f0a5 ---
    expectTrue(validPreflight.can_build(), "valid body count can build island graph");
    expectTrue(validPreflight.skippedContactCount == 1u, "build preflight counts skipped contacts");
    expectTrue(validPreflight.inRangeContactCount == 1u, "build preflight counts in-range contacts");
    expectTrue(validPreflight.inRangeDistanceCount == 1u, "build preflight counts in-range distances");
    expectTrue(validPreflight.outOfRangeDistanceCount == 1u, "build preflight counts out-of-range distances");
    expectTrue(validPreflight.unionCandidateCount == 2u, "build preflight counts union candidates");
    expectTrue(!should_skip_island_build(4, contacts, constraints), "should_skip false for valid build");
    const IslandBuildPreflight degeneratePreflight = preflight_island_build(0, contacts, constraints);
    expectTrue(!degeneratePreflight.can_build(), "zero body count with constraints cannot build");
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip true for degenerate build");
    const IslandSleepPreflight awakePreflight = preflight_island_sleep(graph.island(awakeIsland), bodies);
    expectTrue(!awakePreflight.skipped, "awake island sleep preflight not skipped");
    expectTrue(!should_skip_island_solve_sleeping(graph.island(awakeIsland), bodies),
               "should_skip false for awake island");
    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(sleepingPreflight.all_sleeping(), "all-sleeping island flagged");
    expectTrue(should_skip_island_solve_sleeping(graph.island(sleepingIsland), bodies),
    const IslandWakePreflight wakePreflight = preflight_island_wake(island, bodies, contacts);
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip mixed island");
    expectTrue(wakePreflight.should_wake(), "mixed awake/sleeping island should wake");
    expectTrue(wakePreflight.contactWakeCount == 1u, "wake preflight counts contact wake candidates");
    expectTrue(!should_skip_island_wake(island, bodies, contacts), "should_skip false when wake needed");
    const IslandWakePreflight forcePreflight = preflight_island_wake(forcedGraph.island(0),
    expectTrue(forcePreflight.externalForceCount == 1u, "wake preflight counts external force");
    expectTrue(forcePreflight.should_wake(), "external force flags wake on sleeping island");
void testPreflightIslandDispatchWithSleepGuards() {
    const IslandSleepDispatchPreflight preflight = preflight_island_dispatch_with_sleep(graph, dt, bodies);
    expectTrue(!should_skip_island_dispatch_with_sleep(graph, dt, bodies),
               "should_skip false when solvable island exists");
    const IslandSleepDispatchPreflight sleepingPreflight =
    expectTrue(sleepingPreflight.skipped, "all-sleeping graph skipped by sleep dispatch preflight");
    expectTrue(!sleepingPreflight.can_dispatch(), "all-sleeping graph cannot dispatch with sleep guard");
    expectTrue(sleepingPreflight.solvableIslandCount == 0u, "all-sleeping graph has zero solvable islands");
    expectTrue(should_skip_island_dispatch_with_sleep(sleepingGraph, dt, allSleepingBodies),
               "should_skip true when every island is sleeping");
    expectTrue(should_skip_solve_island_job_with_sleep(job, 1.f / 60.f, bodies),
    testPreflightIslandDispatchWithSleepGuards();

// --- deepen additive from pbd-island-guards-deepen-bcee ---
    const IslandBuildPreflight valid = preflight_island_build(5, contacts, constraints);
    expectTrue(!should_skip_island_build(5, contacts, constraints),
               "should_skip false for valid body count");
               "should_skip true for zero bodies with constraints");
    const IslandBuildPreflight emptyInput = preflight_island_build(0, {}, {});
    expectTrue(!should_skip_island_build(0, {}, {}), "should_skip false for empty inputs");
void testPreflightIslandSleepForSolveGuards() {
    expectTrue(!awakePreflight.skipped, "sleep preflight does not skip awake island");
    expectTrue(!awakePreflight.allSleeping, "awake island is not all-sleeping");
    expectTrue(awakePreflight.awakeCount == 2u, "sleep preflight counts awake dynamic bodies");
    expectTrue(!should_skip_solve_sleeping_island(graph.island(awakeIsland), bodies),
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip sleeping island");
    expectTrue(sleepingPreflight.allSleeping, "sleeping island is all-sleeping");
    expectTrue(sleepingPreflight.sleepingCount == 2u, "sleep preflight counts sleeping bodies");
    expectTrue(should_skip_solve_sleeping_island(graph.island(sleepingIsland), bodies),
    expectTrue(!graphPreflight.skipped, "graph sleep preflight does not skip mixed graph");
    expectTrue(graphPreflight.can_dispatch(), "mixed graph has solveable islands");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "graph sleep preflight counts solveable island");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep preflight counts sleeping island");
    expectTrue(!should_skip_island_solve_for_sleep(graph, bodies),
               "should_skip graph false when solveable islands exist");
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip sleeping island");
    expectTrue(wakePreflight.needs_wake_check(), "sleeping island needs wake check");
    expectTrue(!wakePreflight.can_skip_wake_check(), "sleeping island cannot skip wake check");
    expectTrue(wakePreflight.sleepingCount == 2u, "wake preflight counts sleeping bodies");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts wake candidates");
    expectTrue(!should_skip_island_wake_check(graph.island(sleepingIsland), bodies),
               "should_skip wake false when sleeping bodies exist");
    const IslandWakePreflight awakeWakePreflight = preflight_island_wake_by_index(
    expectTrue(awakeWakePreflight.can_skip_wake_check(),
    expectTrue(should_skip_island_wake_check(graph.island(awakeIsland), bodies),
               "should_skip wake true when no sleeping bodies");
void testPreflightIslandSolveCombinedGuards() {
    const IslandSolveCombinedPreflight awakeCombined =
    expectTrue(!should_skip_island_solve_combined(awakeJob, bodies, contacts, constraints, dt),
               "should_skip combined false for awake island");
    const IslandSolveCombinedPreflight sleepingCombined =
    expectTrue(should_skip_island_solve_combined(sleepingJob, bodies, contacts, constraints, dt),
               "should_skip combined true for sleeping island");
    const IslandSolveCombinedPreflight invalidDt =
    const IslandSolveCombinedPreflight nonFiniteDt = preflight_island_solve_combined(
void testPreflightIslandDispatchNonFiniteDt() {
    const IslandDispatchPreflight finitePreflight = preflight_island_dispatch(graph, 1.f / 60.f);
    expectTrue(!finitePreflight.nonFiniteDt, "finite dt passes non-finite guard");
    expectTrue(finitePreflight.can_dispatch(), "finite dt can dispatch");
    const IslandDispatchPreflight infPreflight =
    expectTrue(infPreflight.nonFiniteDt, "infinite dt fails non-finite guard");
    expectTrue(!infPreflight.can_dispatch(), "infinite dt cannot dispatch");
    expectTrue(should_skip_island_dispatch(graph, std::numeric_limits<f32>::infinity()),
               "should_skip dispatch true for infinite dt");
    const IslandSolveJobPreflight jobPreflight =
    expectTrue(jobPreflight.invalidDt, "NaN dt fails valid-dt guard");
    expectTrue(!jobPreflight.can_dispatch(), "NaN dt cannot dispatch job");
    testPreflightIslandSleepForSolveGuards();
    testPreflightIslandSolveCombinedGuards();
    testPreflightIslandDispatchNonFiniteDt();

// --- deepen additive from deepen-pbd-island-guards-3045 ---
    const IslandBuildPreflight contactPreflight = preflight_island_build(3, contacts, {});
    expectTrue(contactPreflight.skipped, "build preflight skips out-of-range contact bodies");
    expectTrue(contactPreflight.reason == IslandBuildRejectReason::OutOfRangeContactBody,
    expectTrue(contactPreflight.outOfRangeContactCount == 1u,
    expectTrue(should_skip_island_build(3, contacts, {}), "should_skip build on out-of-range contact");
    const IslandBuildPreflight distancePreflight = preflight_island_build(4, {}, constraints);
    expectTrue(distancePreflight.skipped, "build preflight skips out-of-range distance bodies");
    expectTrue(distancePreflight.reason == IslandBuildRejectReason::OutOfRangeDistanceBody,
    expectTrue(should_skip_island_build(4, {}, constraints), "should_skip build on out-of-range distance");
    const IslandBuildPreflight validPreflight = preflight_island_build(5, contacts, constraints);
    expectTrue(!validPreflight.skipped, "build preflight accepts in-range inputs");
    expectTrue(validPreflight.can_build(), "build preflight can build in-range graph");
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::OutOfRangeContactBody),
    expectTrue(island_build_rejects_for_reason(3, contacts, {}, IslandBuildRejectReason::OutOfRangeContactBody),
void testPreflightIslandBodyRefsGuards() {
    const IslandBodyRefsPreflight preflight = preflight_island_body_refs(island, bodies);
    expectTrue(!should_skip_island_body_refs(island, bodies), "should_skip false for in-range bodies");
    const IslandBodyRefsPreflight stalePreflight = preflight_island_body_refs(staleIsland, bodies);
    expectTrue(!stalePreflight.can_solve(), "stale body refs preflight cannot solve");
    expectTrue(should_skip_island_body_refs(staleIsland, bodies), "should_skip true for stale body refs");
    expectTrue(!sleepingPreflight.skipped, "sleep preflight does not skip contact island");
    expectTrue(sleepingPreflight.allSleeping, "sleep preflight marks all-sleeping island");
               "should_skip sleeping island solve");
    const IslandSleepPreflight activePreflight = preflight_island_sleep_state(graph.island(activeIsland), bodies);
    expectTrue(activePreflight.activeCount == 1u, "sleep preflight counts active dynamic body");
    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(sleepingIsland), bodies);
    expectTrue(wakePreflight.forcedWakeCount == 1u, "wake preflight counts forced wake body");
    expectTrue(wakePreflight.can_wake(), "wake preflight can wake forced island");
    expectTrue(!should_skip_island_wake(graph.island(sleepingIsland), bodies),
               "should_skip wake false when force present");
               "should_skip wake true when no forced wake");
    const IslandConstraintSolvePreflight preflight =
    expectTrue(!should_skip_island_constraint_solve(island, bodies, contacts, {}, dt),
               "should_skip false for solvable island");
    expectTrue(should_skip_island_constraint_solve(island, bodies, contacts, {}, dt),
    testPreflightIslandBodyRefsGuards();

// --- deepen additive from pbd-island-sleep-wake-guards-28a6 ---
    const IslandBuildPreflight validPreflight = preflight_island_build(4u, contacts, constraints);
    expectTrue(!validPreflight.skipped, "valid body count passes island build preflight");
    expectTrue(validPreflight.can_build(), "island build preflight can build with valid inputs");
    expectTrue(validPreflight.validContactCount == 1u, "island build preflight counts valid contacts");
    expectTrue(validPreflight.invalidContactCount == 1u, "island build preflight counts invalid contacts");
    expectTrue(validPreflight.validDistanceCount == 1u, "island build preflight counts valid distance constraints");
    expectTrue(!should_skip_island_build(4u, contacts, constraints),
               "should_skip false for valid island build inputs");
    const IslandBuildPreflight zeroBodies = preflight_island_build(0u, contacts, constraints);
               "should_skip true for zero body count");
    expectTrue(sleepingPreflight.allSleeping, "all-dynamic-sleeping island flagged");
    expectTrue(should_skip_sleeping_island(graph.island(sleepingIsland), bodies),
    expectTrue(!should_skip_sleeping_island(graph.island(awakeIsland), bodies),
    expectTrue(graphPreflight.has_awake_islands(), "mixed graph has awake islands");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep stats count all-sleeping island");
    expectTrue(graphPreflight.stats.partiallyAwakeCount == 1u, "graph sleep stats count awake island");
    expectTrue(!should_skip_island_solve_all_sleeping(graph, bodies),
               "should_skip false when awake islands exist");
    expectTrue(!wakePreflight.skipped, "wake preflight does not skip contact island");
    expectTrue(wakePreflight.needsWake, "sleeping body adjacent to awake neighbor needs wake");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts one wake candidate");
    const IslandWakeGraphPreflight graphWake = preflight_island_wake_graph(graph, bodies, contacts);
    expectTrue(!should_skip_island_wake_graph(graph, bodies, contacts),
               "should_skip false when wake candidates exist");
    expectTrue(should_skip_island_wake_graph(allAwakeGraph, awakeBodies, contacts),
               "should_skip true when no sleeping bodies need wake");

// --- deepen additive from deepen-pbd-island-guards-6182 ---
    expectTrue(should_skip_island_build(0u, contacts, constraints), "should_skip build for zero bodies");
    expectTrue(island_build_rejects_for_reason(0u, contacts, constraints, IslandBuildRejectReason::ZeroBodies),
    const IslandBuildPreflight valid = preflight_island_build(4u, contacts, constraints);
    expectTrue(!should_skip_island_build(4u, contacts, constraints), "should_skip false for valid build");
void testIslandBuildGuardedSkipsZeroBodies() {
    const IslandSleepWakePreflight sleepingPreflight =
    expectTrue(sleepingPreflight.all_sleeping(), "both-sleeping island flagged all sleeping");
    expectTrue(sleepingPreflight.should_remain_asleep(), "sleeping island should remain asleep");
               "should_skip sleeping island");
    const IslandSleepWakePreflight mixedPreflight =
    expectTrue(mixedPreflight.has_awake_dynamic(), "mixed island has awake dynamic body");
    expectTrue(mixedPreflight.should_wake(), "mixed island should wake");
    expectTrue(mixedPreflight.can_solve(), "mixed island can solve");
    expectTrue(!should_skip_solve_sleeping_island(graph.island(mixedIsland), bodies),
               "should_skip false for awake dynamic island");
    expectTrue(should_skip_island_constraint_solve(island, bodies, contacts, constraints),
               "should_skip combined solve for all-sleeping island");
    expectTrue(awakePreflight.can_solve(), "constraint solve preflight allows partially awake island");
    expectTrue(!should_skip_island_constraint_solve(island, bodies, contacts, constraints),
void testPreflightIslandSleepWakeGraphGuards() {
    const IslandSleepWakeGraphPreflight preflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(!should_skip_island_solve_sleep_wake(graph, bodies),
               "should_skip graph false when solvable island exists");
    const IslandSleepWakeGraphPreflight allSleeping =
    expectTrue(should_skip_island_solve_sleep_wake(graph, bodies),
               "should_skip graph true when no solvable islands");
    testPreflightIslandSleepWakeGraphGuards();

// --- deepen additive from deepen-pbd-island-guards-a489 ---
    expectTrue(zeroBodies.reason == IslandBuildRejectReason::ZeroBodyCount,
    const IslandBuildPreflight clean = preflight_island_build(3, contacts, constraints);
    expectTrue(!should_skip_island_build(3, contacts, constraints),
               "should_skip false for clean build inputs");
    const IslandBuildPreflight staleContact = preflight_island_build(3, contacts, constraints);
    expectTrue(staleContact.reason == IslandBuildRejectReason::StaleContactBodyRefs,
    const IslandBuildPreflight staleDistancePreflight = preflight_island_build(3, {}, staleDistance);
    expectTrue(staleDistancePreflight.staleDistanceRefCount == 1u,
    expectTrue(staleDistancePreflight.reason == IslandBuildRejectReason::StaleDistanceBodyRefs,
    expectTrue(std::strcmp(islandBuildRejectReasonName(IslandBuildRejectReason::ZeroBodyCount),
    expectTrue(sleepingPreflight.all_dynamic_sleeping(),
    expectTrue(should_skip_island_solve_for_sleep(graph.island(sleepingIsland), bodies),
               "should_skip sleep true for all-sleeping island");
    const IslandSleepWakePreflight awakePreflight =
    expectTrue(!should_skip_island_solve_for_sleep(graph.island(awakeIsland), bodies),
               "should_skip sleep false for awake island");
    expectTrue(wakePreflight.needs_wake(), "wake preflight flags sleeping constrained island");
    const IslandSleepWakeGraphPreflight graphPreflight = preflight_island_sleep_wake_graph(graph, bodies);
    expectTrue(graphPreflight.stats.solvableCount == 1u, "graph sleep preflight counts solvable island");
    expectTrue(!should_skip_island_sleep_wake_graph(graph, bodies),
               "should_skip graph false when solvable islands exist");
    expectTrue(should_skip_island_solve_for_sleep_index(graph, graph.islandCount() + 1u, bodies),
    expectTrue(!sleepingPreflight.can_solve(), "combined preflight rejects all-sleeping island");
    expectTrue(should_skip_island_constraint_solve(sleepingJob, bodies, contacts, constraints, dt),
               "should_skip combined true for all-sleeping island");
    expectTrue(awakePreflight.can_solve(), "combined preflight accepts awake island");
    expectTrue(awakePreflight.job.can_dispatch(), "combined preflight job dispatchable");
    expectTrue(awakePreflight.refs.can_solve(), "combined preflight refs solvable");
    expectTrue(awakePreflight.bodies.can_solve(), "combined preflight bodies solvable");
    expectTrue(awakePreflight.sleepWake.can_solve(), "combined preflight sleep/wake solvable");
    expectTrue(!should_skip_island_constraint_solve(awakeJob, bodies, contacts, constraints, dt),
    const IslandSolveBodyRefsPreflight bodyRefs = preflight_island_solve_bodies(staleIsland, bodies);
    expectTrue(!should_skip_island_solve_bodies(staleIsland, bodies),
               "should_skip bodies false when in-range bodies exist");

// --- deepen additive from pbd-island-guards-deepen-77cf ---
    const IslandBuildPreflight validPreflight = preflight_island_build(2, contacts, constraints);
    expectTrue(validPreflight.can_build(), "valid island build preflight can build");
    expectTrue(validPreflight.inRangeContactCount == 1u, "valid preflight counts in-range contacts");
    expectTrue(validPreflight.inRangeDistanceCount == 1u, "valid preflight counts in-range distances");
    expectTrue(!should_skip_island_build(2, contacts, constraints), "should_skip false for valid inputs");
    expectTrue(std::strcmp(islandBuildRejectReasonName(IslandBuildRejectReason::ZeroBodyCount), "ZeroBodyCount") == 0,
    const IslandBuildPreflight outOfRangeContact = preflight_island_build(2, contacts, constraints);
    expectTrue(outOfRangeContact.reason == IslandBuildRejectReason::OutOfRangeContactBodies,
    const IslandBuildPreflight outOfRangeDistance = preflight_island_build(2, contacts, constraints);
    expectTrue(outOfRangeDistance.reason == IslandBuildRejectReason::OutOfRangeDistanceBodies,
    expectTrue(!should_skip_island_body_refs(island, bodies), "should_skip false for valid body refs");
void testPreflightIslandSolveRefsCombined() {
    const IslandSolveRefsPreflight preflight =
    expectTrue(!should_skip_island_solve_refs(island, bodies, contacts, constraints),
               "should_skip false for valid combined refs");
    const IslandSleepPreflight awakePreflight = preflight_island_sleep(island, awakeBodies);
    expectTrue(!awakePreflight.skipped, "sleep preflight does not skip constrained island");
    expectTrue(awakePreflight.dynamicAwakeCount == 2u, "sleep preflight counts awake dynamic bodies");
    expectTrue(!awakePreflight.all_dynamic_sleeping(), "awake island is not fully sleeping");
    expectTrue(!should_skip_island_solve_for_sleep(island, awakeBodies),
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(island, sleepingBodies);
    expectTrue(sleepPreflight.all_dynamic_sleeping(), "all dynamic bodies sleeping");
    expectTrue(sleepPreflight.can_skip_solve(), "sleep preflight can skip solve");
    expectTrue(should_skip_island_solve_for_sleep(island, sleepingBodies),
               "should_skip sleep true for fully sleeping island");
    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, sleepingBodies);
    expectTrue(graphPreflight.stats.fullySleepingCount == 1u,
    expectTrue(graphPreflight.all_fully_sleeping(), "graph reports all islands fully sleeping");
    expectTrue(wakePreflight.wakeCandidateCount >= 1u, "wake preflight counts moving body");
    expectTrue(wakePreflight.penetratingContactCount >= 1u, "wake preflight counts penetrating contact");
    expectTrue(wakePreflight.should_wake(), "wake preflight should wake island");
    expectTrue(forcePreflight.externalForceCount >= 1u, "wake preflight counts external force");
    expectTrue(forcePreflight.should_wake(), "external force triggers wake preflight");
    const IslandWakeGraphPreflight graphPreflight =
    expectTrue(graphPreflight.any_should_wake(), "graph wake preflight finds wakeable island");
    testPreflightIslandSolveRefsCombined();

// --- deepen additive from deepen-pbd-island-guards-2105 ---
    const IslandBuildPreflight validPreflight = preflight_island_graph_build(2, contacts, constraints);
    expectTrue(validPreflight.validContactCount == 1u, "valid build preflight counts contacts");
    expectTrue(validPreflight.validDistanceCount == 1u, "valid build preflight counts distance constraints");
    expectTrue(!should_skip_island_graph_build(2, contacts, constraints),
    expectTrue(should_skip_island_graph_build(0, contacts, constraints),
    const IslandBuildPreflight staleContactPreflight = preflight_island_graph_build(2, staleContacts, {});
    expectTrue(staleContactPreflight.skipped, "out-of-range contact body is skipped");
    expectTrue(staleContactPreflight.skippedContactCount == 1u,
    const IslandBuildPreflight staleDistancePreflight = preflight_island_graph_build(2, {}, staleConstraints);
    expectTrue(staleDistancePreflight.skipped, "out-of-range distance body is skipped");
    expectTrue(staleDistancePreflight.skippedDistanceCount == 1u,
    expectTrue(should_skip_sleeping_island_solve(graph.island(islandIndex), bodies),
    expectTrue(should_skip_sleeping_island_solve_index(graph, islandIndex, bodies),
    const IslandSleepPreflight mixed =
    expectTrue(!should_skip_sleeping_island_solve(graph.island(islandIndex), bodies),
               "should_skip_sleeping false when awake body exists");
void testPreflightIslandSleepGraphGuards() {
    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(!should_skip_sleeping_island_solve(graph.island(awakeIndices[0]), bodies),
    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(islandIndex), bodies);
    expectTrue(wakePreflight.sleepingDynamicCount == 1u, "wake preflight counts sleeping dynamic");
    expectTrue(wakePreflight.awakeDynamicCount == 1u, "wake preflight counts awake dynamic");
    expectTrue(wakePreflight.should_wake(), "mixed island should wake sleeping bodies");
    expectTrue(!should_skip_island_wake(graph.island(islandIndex), bodies),
               "should_skip_island_wake false when wake is needed");
    expectTrue(should_skip_island_wake(graph.island(islandIndex), bodies),
               "should_skip_island_wake true after all dynamics are awake");
    const IslandSolveBodiesPreflight staticOnly =
    expectTrue(should_skip_island_solve_bodies(graph.island(islandIndex), bodies),
               "should_skip_island_solve_bodies for static-only island");
    const IslandSolveBodiesPreflight outOfRange =
    const IslandSolvePreflightCombined sleepingCombined = preflight_island_solve_combined(
    const IslandSolvePreflightCombined awakeCombined = preflight_island_solve_combined(
    testPreflightIslandSleepGraphGuards();

// --- deepen additive from deepen-pbd-island-guards-358e ---
               "should_skip false when build preflight can build");
    const IslandBuildPreflight allStale = preflight_island_build(2, staleContacts, staleConstraints);
    expectTrue(allStale.reason == IslandBuildRejectReason::AllConstraintsStale,
    expectTrue(should_skip_island_build(2, staleContacts, staleConstraints),
               "should_skip true when all constraint refs are stale");
    expectTrue(!sleepingPreflight.can_attempt_sleep(),
    expectTrue(should_skip_island_solve_all_sleeping(graph.island(sleepingIsland), bodies),
               "should_skip all-sleeping island solve");
    expectTrue(awakePreflight.can_attempt_sleep(), "awake island can attempt sleep");
    expectTrue(!awakePreflight.all_dynamic_sleeping(), "awake island is not all sleeping");
    expectTrue(!should_skip_island_solve_all_sleeping(graph.island(awakeIsland), bodies),
    expectTrue(wakePreflight.should_wake(), "sleeping island with constraints is wake candidate");
    expectTrue(wakePreflight.ownedConstraintCount == 1u, "wake preflight counts owned constraints");
    const IslandWakePreflight awakeWakePreflight = preflight_island_wake(graph.island(awakeIsland), bodies);
    expectTrue(!awakeWakePreflight.should_wake(), "all-awake island is not a wake candidate");
    expectTrue(awakeWakePreflight.awakeBodyCount == 2u, "wake preflight counts awake bodies");
    expectTrue(awakeWakePreflight.sleepingBodyCount == 0u, "awake island has no sleeping bodies");
void testPreflightIslandSolveParticipationGuards() {
    const IslandSolveParticipationPreflight inactivePreflight =
    expectTrue(!inactivePreflight.can_solve(), "sleeping/static island has no participation");
    expectTrue(should_skip_island_solve_no_participation(graph.island(inactiveIsland), bodies),
               "should_skip no-participation island solve");
    expectTrue(inactivePreflight.sleepingBodyCount == 1u,
    expectTrue(inactivePreflight.staticOrKinematicBodyCount == 1u,
    const IslandSolveParticipationPreflight activePreflight =
    expectTrue(activePreflight.can_solve(), "awake dynamic island can participate in solve");
    expectTrue(!should_skip_island_solve_no_participation(graph.island(activeIsland), bodies),
               "should_skip false when island has participating bodies");
    expectTrue(activePreflight.participatingBodyCount == 2u,
void testIslandSleepWakeGraphGuards() {
    expectTrue(!should_skip_island_wake_graph(graph, bodies),
               "should_skip wake graph false when candidates exist");
    expectTrue(should_skip_island_wake_graph(emptyGraph, bodies),
               "should_skip wake graph true for empty graph");
    testPreflightIslandSolveParticipationGuards();

// --- deepen additive from deepen-pbd-island-guards-a375 ---
               "should_skip_island_build true for zero bodies");
void testBuildGuardedMatchesBuildOnValidPath() {
    expectTrue(!should_skip_island_body_refs(graph.island(0), bodies),
               "should_skip false when body refs are in range");
               "should_skip true when island body refs are stale");
void testPreflightIslandSleepStateGuards() {
    expectTrue(awakePreflight.awakeDynamicCount == 2u, "sleep preflight counts awake dynamics");
    expectTrue(awakePreflight.can_solve_awake(), "awake island can solve");
    expectTrue(!should_skip_solve_sleeping_island(graph.island(awakeIsland), awakeBodies),
    expectTrue(sleepingPreflight.all_dynamic_sleeping(), "all-dynamic-sleeping island flagged");
    expectTrue(!sleepingPreflight.can_solve_awake(), "all-sleeping island cannot solve awake");
    expectTrue(should_skip_solve_sleeping_island(graph.island(sleepingIsland), awakeBodies),
    expectTrue(wakePreflight.needs_wake(), "mixed island needs wake");
    expectTrue(!should_skip_island_dispatch_for_sleep(graph, bodies),
    const IslandSleepGraphPreflight allSleepPreflight = preflight_island_sleep_graph(graph, allSleeping);
    expectTrue(allSleepPreflight.skipped, "graph sleep preflight skips all-sleeping constrained graph");
    expectTrue(should_skip_island_dispatch_for_sleep(graph, allSleeping),
               "should_skip true when every constrained island is all-sleeping");
void testSolveIslandJobSleepGuarded() {
    const IslandSolvePassPreflight passPreflight = preflight_island_solve_pass(
    expectTrue(!passPreflight.can_solve(), "combined solve pass preflight rejects all-sleeping island");
    expectTrue(should_skip_island_solve_pass(
               "should_skip combined solve pass for all-sleeping island");
    testPreflightIslandSleepStateGuards();

// --- deepen additive from deepen-pbd-island-guards-9b4f ---
    const IslandBuildPreflight preflight = preflight_island_build(4u, contacts, constraints);
    const IslandBuildPreflight emptyPreflight = preflight_island_build(0u, {}, {});
    expectTrue(emptyPreflight.skipped, "build preflight skips empty no-op inputs");
    expectTrue(!emptyPreflight.can_build(), "build preflight cannot build empty no-op inputs");
    expectTrue(should_skip_island_build(0u, {}, {}), "should_skip build true for empty no-op inputs");
void testBuildGuardedAndBuildStats() {
    const IslandSleepPreflight sleepingPreflight = preflight_island_sleep(bodies, graph.island(sleepingIsland));
    expectTrue(sleepingPreflight.awakeDynamicCount == 0u, "sleep preflight sees no awake dynamic bodies");
    expectTrue(sleepingPreflight.is_fully_sleeping(), "sleep preflight marks fully sleeping island");
    expectTrue(should_skip_solve_fully_sleeping_island(bodies, graph.island(sleepingIsland)),
               "should_skip fully sleeping island");
    const IslandSleepPreflight awakePreflight = preflight_island_sleep(bodies, graph.island(awakeIsland));
    expectTrue(awakePreflight.awakeDynamicCount == 1u, "sleep preflight counts awake dynamic body");
    expectTrue(awakePreflight.staticOrKinematicCount == 1u, "sleep preflight counts static body");
    expectTrue(!awakePreflight.is_fully_sleeping(), "awake island is not fully sleeping");
    const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, graph.island(wakeIsland), contacts);
    expectTrue(wakePreflight.ownedContactCount == 1u, "wake preflight counts owned contacts");
    expectTrue(wakePreflight.awakeParticipantCount == 1u, "wake preflight counts awake participant");
    expectTrue(wakePreflight.wakeCandidateCount == 1u, "wake preflight counts wake candidate contact");
    expectTrue(wakePreflight.can_wake(), "mixed island can wake");
    const IslandWakePreflight noWakePreflight =
    expectTrue(!noWakePreflight.can_wake(), "all-sleeping island cannot wake from contacts");
    const IslandWakePreflight outOfRange =
void testSolveIslandJobGuardedSleepAndRefs() {
    const IslandSolveSleepPreflight preflight =
    expectTrue(should_skip_solve_island_with_sleep(bodies, graph.island(0), contacts, constraints),
               "should_skip combined sleep/refs preflight");
    const IslandSolveSleepPreflight awakePreflight =
    expectTrue(awakePreflight.can_solve(), "awake island passes combined solve preflight");

// --- deepen additive from pbd-island-sleep-wake-guards-c801 ---
    const IslandBuildPreflight emptyScene = preflight_island_build(0u, {}, {});
    expectTrue(should_skip_island_build(0u, {}, {}), "should_skip_island_build on empty scene");
    const IslandSleepWakePreflight sleepingPreflight = preflight_island_sleep_wake(sleeping, bodies);
    expectTrue(!sleepingPreflight.skipped, "sleep/wake preflight does not skip constrained island");
    expectTrue(sleepingPreflight.sleepingCount == 2u, "sleep/wake preflight counts sleeping bodies");
    expectTrue(sleepingPreflight.wakeableCount == 0u, "sleep/wake preflight finds no wakeable bodies");
    expectTrue(sleepingPreflight.is_all_sleeping(), "sleeping pair island is all sleeping");
    expectTrue(should_skip_sleeping_island_solve(sleeping, bodies),
    const IslandSleepWakePreflight wakeablePreflight = preflight_island_sleep_wake(wakeable, bodies);
    expectTrue(wakeablePreflight.wakeableCount == 1u, "mixed island has one wakeable body");
    expectTrue(wakeablePreflight.staticOrKinematicCount == 1u, "mixed island has one static body");
    expectTrue(wakeablePreflight.can_solve(), "wakeable island can solve");
    expectTrue(!should_skip_sleeping_island_solve(wakeable, bodies),
               "should_skip false for wakeable island");
    const IslandSleepWakePreflight outOfRange =
               "should_skip_sleeping_island_solve_index on out-of-range index");
    expectTrue(!graphPreflight.skipped, "graph sleep/wake preflight does not skip mixed graph");
    expectTrue(graphPreflight.can_dispatch(), "graph sleep/wake preflight can dispatch");
    expectTrue(graphPreflight.stats.wakeableCount == 1u, "graph sleep/wake preflight counts wakeable islands");
    expectTrue(graphPreflight.stats.allSleepingCount == 1u, "graph sleep/wake preflight counts all-sleeping islands");
    expectTrue(!should_skip_sleeping_island_graph(graph, bodies),
               "should_skip_sleeping_island_graph false when wakeable islands exist");
    expectTrue(should_skip_island_constraint_solve(graph.island(sleepingIsland), bodies, contacts, constraints),
    const IslandConstraintSolvePreflight contactSolve =
    expectTrue(!should_skip_island_constraint_solve(graph.island(contactIsland), bodies, contacts, constraints),
               "should_skip false for wakeable contact island");

// --- deepen additive from deepen-pbd-island-guards-12c0 ---
    expectTrue(should_skip_island_build(0, contacts, constraints), "should_skip build on zero bodies");
    const IslandSleepSolvePreflight sleepPreflight = preflight_island_sleep_solve(island, bodies);
    expectTrue(sleepPreflight.awakeBodyCount == 1u, "sleep preflight counts awake bodies");
    expectTrue(sleepPreflight.can_solve(), "mixed sleep/awake island can solve");
    expectTrue(!should_skip_solve_sleeping_island(island, bodies),
    const IslandSleepSolvePreflight allSleepPreflight = preflight_island_sleep_solve(island, allSleepingBodies);
    expectTrue(!allSleepPreflight.can_solve(), "all-sleeping dynamic island cannot solve");
    expectTrue(should_skip_solve_sleeping_island(island, allSleepingBodies),
    const IslandSleepGraphPreflight graphPreflight = preflight_island_sleep_graph(graph, allSleepingBodies);
    expectTrue(graphPreflight.sleepingOnlyCount >= 1u, "graph sleep preflight counts sleeping-only islands");
    expectTrue(graphPreflight.awakeCount == 0u, "graph sleep preflight has no awake islands");
    expectTrue(!graphPreflight.can_dispatch(), "graph sleep preflight cannot dispatch all sleeping");
    expectTrue(should_skip_solve_all_sleeping_islands(graph, allSleepingBodies),
               "should_skip all sleeping graph dispatch");
    const IslandWakePreflight wakePreflight = preflight_island_wake(island, allSleepingBodies, contacts);
    expectTrue(wakePreflight.sleepingDynamicCount == 2u, "wake preflight counts sleeping dynamics");
    expectTrue(wakePreflight.nonZeroImpulseCount == 1u, "wake preflight counts non-zero impulses");
    expectTrue(wakePreflight.should_wake(), "wake preflight should wake on external impulse");
    expectTrue(!should_skip_wake_island(island, allSleepingBodies, contacts),
               "should_skip wake false when impulse signal exists");
void testPreflightSolveIslandWithBodiesGuards() {
    const IslandSolveBodyPreflight preflight =
    expectTrue(should_skip_solve_island_with_bodies(island, bodies, contacts, constraints),
    expectTrue(awakePreflight.can_solve(), "combined body preflight can solve with awake body");
    testPreflightSolveIslandWithBodiesGuards();

// --- deepen additive from pbd-island-sleep-wake-preflights-1600 ---
               "should_skip false when valid constraints exist despite OOR refs");
                   IslandBuildRejectReason::OutOfRangeBodyRef,
    const IslandBuildPreflight cleanPreflight = preflight_island_build(4, {contacts[0]}, {constraints[0]});
    expectTrue(!cleanPreflight.rejected, "clean build preflight is not rejected");
    expectTrue(cleanPreflight.can_build(), "clean build preflight can build");
    expectTrue(cleanPreflight.has_constraints(), "clean build preflight has constraints");
    expectTrue(should_skip_island_build(0, {}, {}), "should_skip true when no constraints to partition");
    expectTrue(sleepingPreflight.allSleeping, "sleeping island is all sleeping");
    expectTrue(sleepingPreflight.allInactive, "sleeping island is all inactive");
    expectTrue(should_skip_inactive_island_solve(graph.island(sleepingIsland), bodies),
               "should_skip inactive sleeping island");
    const IslandSleepWakePreflight activePreflight =
    expectTrue(activePreflight.activeDynamicCount == 2u, "active island has dynamic bodies");
    expectTrue(!should_skip_inactive_island_solve(graph.island(activeIsland), bodies),
               "should_skip false for active island");
    expectTrue(graphPreflight.stats.activeCount == 1u, "graph sleep preflight counts active island");
               "should_skip graph false when active islands exist");
    expectTrue(should_skip_inactive_island_solve_index(graph, graph.islandCount() + 1u, bodies),
               "should_skip index true for out-of-range island");
void testPreflightIslandSolvableConstraintRefsGuards() {
    const IslandConstraintRefsPreflight sleepingRefs = preflight_island_solvable_constraint_refs(
    expectTrue(should_skip_island_solvable_constraint_refs(graph.island(sleepingIsland), bodies, contacts, constraints),
               "should_skip solvable refs on sleeping island");
    const IslandConstraintRefsPreflight activeRefs = preflight_island_solvable_constraint_refs(
    expectTrue(!should_skip_island_solvable_constraint_refs(graph.island(activeIsland), bodies, contacts, constraints),
               "should_skip false for solvable active island");
    const IslandSolveBodiesPreflight solveBodies =
    expectTrue(!should_skip_island_solve_bodies(graph.island(activeIsland), bodies, contacts, constraints),
               "should_skip solve bodies false for active island");
    const IslandSolveBodiesPreflight inactiveSolveBodies =
    expectTrue(should_skip_island_solve_bodies(graph.island(sleepingIsland), bodies, contacts, constraints),
               "should_skip solve bodies true for sleeping island");
void testPreflightIslandDispatchWithBodiesGuards() {
    const IslandDispatchBodiesPreflight preflight = preflight_island_dispatch_with_bodies(graph, bodies, dt);
    expectTrue(should_skip_island_dispatch_with_bodies(graph, bodies, dt),
               "should_skip dispatch with bodies on all-sleeping graph");
    const IslandDispatchBodiesPreflight awakePreflight = preflight_island_dispatch_with_bodies(graph, bodies, dt);
    expectTrue(awakePreflight.can_dispatch(), "combined dispatch preflight dispatches when one body wakes");
    expectTrue(!should_skip_island_dispatch_with_bodies(graph, bodies, dt),
               "should_skip false after wake");
    testPreflightIslandSolvableConstraintRefsGuards();
    testPreflightIslandDispatchWithBodiesGuards();

// --- deepen additive from deepen-pbd-island-sleep-build-guards-e836 ---
    expectTrue(emptyPreflight.skipped, "build preflight skips completely empty inputs");
    expectTrue(should_skip_island_build(0, {}, {}), "should_skip build on empty inputs");
    expectTrue(sleepingPreflight.allDynamicSleeping, "both-sleeping island flagged allDynamicSleeping");
    expectTrue(sleepingPreflight.can_sleep(), "all-sleeping island can sleep");
    expectTrue(graphPreflight.can_solve(), "graph sleep preflight can solve with awake island");
    expectTrue(graphPreflight.stats.solvableCount >= 1u, "graph sleep preflight counts solvable islands");
               "should_skip dispatch for sleep false with awake island");
    expectTrue(solvableIndices.size() == graphPreflight.stats.solvableCount,
    expectTrue(!sleepingPreflight.invalidDt, "constraint solve preflight accepts valid dt");
    expectTrue(sleepingPreflight.refs.can_solve(), "sleeping island has in-range refs");
    expectTrue(!sleepingPreflight.sleepWake.can_solve(), "sleeping island fails sleep/wake preflight");
               "should_skip constraint solve for all-sleeping island");
    const IslandConstraintSolvePreflight awakePreflight = preflight_island_constraint_solve(
    expectTrue(awakePreflight.can_solve(), "awake island passes constraint solve preflight");
               "should_skip false for awake island constraint solve");
    expectTrue(invalidDtPreflight.invalidDt, "constraint solve preflight rejects zero dt");
    expectTrue(!invalidDtPreflight.can_solve(), "invalid dt cannot constraint-solve");
               "should_skip constraint solve on invalid dt");
    const IslandConstraintSolvePreflight stalePreflight =
    expectTrue(!stalePreflight.can_solve(), "stale refs fail combined constraint solve preflight");
void testAllSleepingGraphDispatchPreflight() {
    expectTrue(graphPreflight.skipped, "all-sleeping graph preflight is skipped");
    expectTrue(!graphPreflight.can_solve(), "all-sleeping graph cannot solve");
    expectTrue(should_skip_island_dispatch_for_sleep(graph, bodies),
               "should_skip dispatch for sleep on all-sleeping graph");
    testAllSleepingGraphDispatchPreflight();

// --- deepen additive from deepen-pbd-island-guards-9f8d ---
void testContactIslandGraphBuildPreflightGuards() {
    const ContactIslandGraphBuildPreflight preflight = preflightContactIslandGraphBuild(2, contacts, constraints);
    expectTrue(preflight.reason == ContactIslandGraphBuildRejectReason::SelfContact,
    expectTrue(std::strcmp(contactIslandGraphBuildRejectReasonName(preflight.reason), "SelfContact") == 0,
    const IslandBuildPreflight islandPreflight = preflight_island_build(2, contacts, constraints);
    expectTrue(islandPreflight.has_degenerate_refs(), "island build preflight flags self-contact");
    expectTrue(!islandPreflight.can_build(), "island build preflight rejects self-contact");
void testPreflightIslandConstraintSolveGraphGuards() {
    const IslandConstraintSolveGraphPreflight preflight =
    const IslandConstraintSolvePreflight byIndex =
    expectTrue(!should_skip_island_constraint_solve_by_index(
               "should_skip false for solveable island index");
    const IslandConstraintSolvePreflight blockedIndex = preflight_island_constraint_solve_by_index(
    expectTrue(should_skip_island_constraint_solve_by_index(
               "should_skip true for all-sleeping island index");
void testGuardedIslandSolvePipelineWithWake() {
    const IslandSolvePipelinePreflight pipelinePreflight = preflight_island_solve_pipeline(
    expectTrue(pipelinePreflight.can_solve(), "pipeline preflight allows mixed island");
    expectTrue(pipelinePreflight.should_wake_first(), "pipeline preflight requests wake before solve");
    expectTrue(!should_skip_island_solve_pipeline(graph.island(mixedIsland), bodies, work.contactManifolds(), constraints),
               "should_skip pipeline false for mixed island");
    const IslandSolvePipelinePreflight sleepingPipeline = preflight_island_solve_pipeline(
    expectTrue(should_skip_island_solve_pipeline(graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints),
               "should_skip pipeline true for all-sleeping island");
    testContactIslandGraphBuildPreflightGuards();
    testPreflightIslandConstraintSolveGraphGuards();

// --- deepen additive from deepen-pbd-island-guards-73b7 ---
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::None), "None") == 0,
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyInput), "EmptyInput") == 0,
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::UnsafeRefs), "UnsafeRefs") == 0,
    expectTrue(island_build_reject_reason(0, {}, {}) == IslandBuildRejectReason::EmptyInput,
    expectTrue(island_build_rejects_for_reason(0, {}, {}, IslandBuildRejectReason::EmptyInput),
    expectTrue(island_build_reject_reason(4, contacts, constraints) == IslandBuildRejectReason::UnsafeRefs,
    expectTrue(unsafeResult.reason == IslandBuildRejectReason::UnsafeRefs,
    expectTrue(preflight.reason == IslandBuildRejectReason::UnsafeRefs,
void testSolveIslandJobGuardedAndPipeline() {
    const IslandConstraintSolvePreflight mixedPreflight =
    expectTrue(!mixedPreflight.skipped, "constraint solve index preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island can solve via index preflight");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island blocked by index preflight");
    expectTrue(should_skip_island_constraint_solve_index(graph, sleepingIsland, bodies, contacts, constraints),
               "should_skip constraint solve index on all-sleeping island");
    const IslandSolvePipelinePreflight pipeline =

// --- deepen additive from deepen-pbd-island-guards-e84e ---
void testPreflightIslandGraphBuildDeepenGuards() {
    const IslandGraphBuildPreflight graphPreflight = preflight_island_graph_build(3, contacts, constraints);
    expectTrue(!graphPreflight.skipped, "graph build preflight does not skip valid partition inputs");
    expectTrue(graphPreflight.stats.selfPairContactCount == 1u,
    expectTrue(graphPreflight.stats.selfPairDistanceCount == 0u,
    expectTrue(graphPreflight.has_degenerate_refs(), "graph build preflight flags degenerate refs");
    expectTrue(graphPreflight.can_build(), "self-pairs alone do not block guarded build");
    const IslandBuildPreflight pbdPreflight = preflight_island_build(3, contacts, constraints);
    expectTrue(pbdPreflight.stats.selfPairContactCount == 1u,
    expectTrue(pbdPreflight.has_degenerate_refs(), "pbd build preflight flags degenerate refs");
    const IslandGraphIntegrityPreflight integrity =
    expectTrue(!should_skip_island_graph_integrity(graph, 3, static_cast<u32>(contacts.size()),
               "should_skip integrity false for valid graph");
void testPreflightIslandSolvePipelineGuards() {
    const IslandSolvePipelinePreflight mixedPipeline =
    expectTrue(!should_skip_island_solve_pipeline(graph.island(mixedIsland), bodies, contacts, constraints),
    expectTrue(should_skip_island_solve_pipeline(graph.island(sleepingIsland), bodies, contacts, constraints),
    const IslandSolvePipelinePreflight outOfRangePipeline =
void testDispatchIslandPipelineBatchGuards() {
    const IslandDispatchBodiesPreflight dispatchPreflight =
    expectTrue(!dispatchPreflight.skipped, "dispatch-with-bodies preflight does not skip mixed graph");
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch-with-bodies preflight can dispatch");
    expectTrue(dispatchPreflight.wake.can_wake(), "dispatch-with-bodies preflight sees wakeable island");
               "should_skip dispatch-with-bodies false for mixed graph");
    testPreflightIslandGraphBuildDeepenGuards();
    testPreflightIslandSolvePipelineGuards();

// --- deepen additive from deepen-pbd-island-guards-0f38 ---
void testSolveIslandJobGuardedConstraintPreflight() {
    const IslandConstraintSolvePreflight mixedPreflight = preflight_island_constraint_solve_by_index(
    expectTrue(!mixedPreflight.skipped, "index constraint preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint solve preflight");
    const IslandConstraintSolvePreflight sleepingPreflight = preflight_island_constraint_solve_by_index(
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint solve preflight");
               "should_skip constraint solve by index on all-sleeping island");
    const IslandSleepAwareDispatchPreflight preflight =
    expectTrue(!should_skip_island_sleep_aware_dispatch(graph, bodies, dt),
               "should_skip sleep-aware dispatch false for mixed graph");
    testSolveIslandJobGuardedConstraintPreflight();

// --- deepen additive from deepen-pbd-island-guards-574a ---
void testIslandDeepenRejectReasonGuards() {
        island_build_reject_reason(4, contacts, constraints) == IslandBuildRejectReason::OutOfRangeContactBodies,
        island_build_rejects_for_reason(4, contacts, constraints, IslandBuildRejectReason::OutOfRangeContactBodies),
        island_build_reject_reason(0, {}, {}) == IslandBuildRejectReason::EmptyInputs,
        std::strcmp(islandBuildRejectReasonName(IslandBuildRejectReason::OutOfRangeContactBodies),
    const IslandBuildDeepenPreflight buildDeepen = preflight_island_build_deepen(4, contacts, constraints);
    expectTrue(should_skip_island_build_deepen(4, contacts, constraints),
               "should_skip_island_build_deepen on unsafe refs");
        island_build_reject_reason(4, safeContacts, safeConstraints) == IslandBuildRejectReason::None,
            IslandConstraintSolveRejectReason::None,
            graph.island(mixedIsland), bodies, work.contactManifolds(), twoIslands, IslandConstraintSolveRejectReason::None),
    expectTrue(should_skip_island_constraint_solve_deepen(
               "should_skip_island_constraint_solve_deepen on all-sleeping island");
    const IslandConstraintSolveDeepenPreflight solveDeepen = preflight_island_constraint_solve_deepen(
    expectTrue(solveDeepen.reason == IslandConstraintSolveRejectReason::NoMovableBodies,
        island_solve_job_reject_reason(constrainedJob, 1.f / 60.f) == IslandSolveJobRejectReason::None,
        island_solve_job_reject_reason(constrainedJob, 0.f) == IslandSolveJobRejectReason::InvalidDt,
    expectTrue(should_skip_solve_island_job_deepen(constrainedJob, 0.f),
               "should_skip_solve_island_job_deepen on invalid dt");
        island_solve_job_reject_reason(invalidJob, 1.f / 60.f) == IslandSolveJobRejectReason::OutOfRangeIndex,
        island_solve_job_rejects_for_reason(invalidJob, 1.f / 60.f, IslandSolveJobRejectReason::OutOfRangeIndex),
        island_dispatch_reject_reason(graph, 1.f / 60.f) == IslandDispatchRejectReason::None,
        island_dispatch_rejects_for_reason(graph, 0.f, IslandDispatchRejectReason::InvalidDt),
    expectTrue(should_skip_island_dispatch_deepen(graph, 0.f),
               "should_skip_island_dispatch_deepen on invalid dt");
        island_dispatch_reject_reason(emptyGraph, 1.f / 60.f) == IslandDispatchRejectReason::NoDispatchableIslands,
        island_sleep_solve_reject_reason(graph.island(mixedIsland), bodies) == IslandSleepSolveRejectReason::None,
    expectTrue(should_skip_island_sleep_solve_deepen(graph.island(mixedIsland), bodies),
               "should_skip_island_sleep_solve_deepen on all-sleeping island");
            IslandSleepSolveRejectReason::OutOfRangeIndex,
    const IslandSleepSolveDeepenPreflight sleepDeepen =
    expectTrue(sleepDeepen.reason == IslandSleepSolveRejectReason::AllSleeping,
        island_wake_reject_reason(graph.island(mixedIsland), bodies) == IslandWakeRejectReason::None,
        island_wake_reject_reason(graph.island(sleepingIsland), bodies) == IslandWakeRejectReason::NoMixedSleepState,
    expectTrue(should_skip_island_wake_deepen(graph.island(sleepingIsland), bodies),
               "should_skip_island_wake_deepen on all-sleeping island");
            IslandWakeRejectReason::OutOfRangeIndex,
    const IslandWakeDeepenPreflight wakeDeepen = preflight_island_wake_deepen(graph.island(mixedIsland), bodies);
    testIslandDeepenRejectReasonGuards();

// --- deepen additive from deepen-pbd-island-guards-bdbf ---
void testPreflightIslandGraphIntegrityGuards() {
    const IslandGraphIntegrityPreflight preflight =
    expectTrue(!should_skip_island_graph_integrity(graph, 2, contacts, constraints),
               "should_skip false for consistent graph");
    const IslandGraphIntegrityPreflight mismatchPreflight =
    expectTrue(!mismatchPreflight.is_consistent(), "shrunk contact slots fail integrity");
    expectTrue(mismatchPreflight.stats.orphanedContactRefCount >= 1u,
    const IslandGraphIntegrityPreflight bodyMismatchPreflight =
    expectTrue(!bodyMismatchPreflight.is_consistent(), "undersized body count fails integrity");
    expectTrue(bodyMismatchPreflight.stats.outOfRangeBodyIndexCount >= 1u,
void testPreflightIslandConstraintBodyRefsGuards() {
    const IslandConstraintRefsPreflight safePreflight =
    expectTrue(!safePreflight.has_unsafe_body_refs(), "in-range contact bodies pass body-ref preflight");
    expectTrue(safePreflight.can_solve(), "in-range island can solve with body-ref checks");
    const IslandConstraintRefsPreflight unsafePreflight =
    expectTrue(unsafePreflight.has_unsafe_body_refs(), "out-of-range contact body fails body-ref preflight");
    expectTrue(!unsafePreflight.can_solve(), "unsafe body refs block solve preflight");
    expectTrue(unsafePreflight.outOfRangeContactBodyCount == 1u,
    expectTrue(unsafePreflight.outOfRangeDistanceBodyCount == 1u,
void testSolveIslandJobGuardedSkipsAllSleeping() {
void testPreflightIslandWakeAndSolveGuards() {
    const IslandWakeAndSolvePreflight mixedPreflight = preflight_island_wake_and_solve(
    expectTrue(!mixedPreflight.skipped, "wake-and-solve preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island can wake-and-solve");
    expectTrue(mixedPreflight.should_wake_first(), "mixed island should wake before solve");
    const IslandWakeAndSolvePreflight sleepingPreflight = preflight_island_wake_and_solve(
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island cannot wake-and-solve");
    expectTrue(should_skip_island_wake_and_solve(
               "should_skip wake-and-solve on all-sleeping island");
    const IslandWakeAndSolveGraphPreflight graphPreflight =
    expectTrue(graphPreflight.can_dispatch(), "wake-and-solve graph preflight can dispatch mixed graph");
    expectTrue(!should_skip_island_wake_and_solve_graph(graph, bodies, 1.f / 60.f),
               "should_skip wake-and-solve graph false when mixed island exists");
    testPreflightIslandGraphIntegrityGuards();
    testPreflightIslandConstraintBodyRefsGuards();
    testPreflightIslandWakeAndSolveGuards();

// --- deepen additive from deepen-pbd-island-guards-3f17 ---
void testIslandRejectReasonGuards() {
                                               IslandBuildRejectReason::OutOfRangeContactRefs),
    const IslandBuildPreflight buildPreflight = preflight_island_build(4, contacts, constraints);
    expectTrue(buildPreflight.reason == IslandBuildRejectReason::OutOfRangeContactRefs,
    expectTrue(!buildPreflight.can_build(), "build preflight cannot build with reject reason");
                                               IslandSleepRejectReason::AllSleeping),
                   IslandSleepRejectReason::None,
                   IslandSleepRejectReason::OutOfRangeIslandIndex,
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(graph.island(sleepingIsland), bodies);
    expectTrue(sleepPreflight.reason == IslandSleepRejectReason::AllSleeping,
    expectTrue(island_wake_rejects_for_reason(graph.island(mixedIsland), bodies, IslandWakeRejectReason::None),
                                              IslandWakeRejectReason::NoActiveDynamic),
                   IslandWakeRejectReason::OutOfRangeIslandIndex,
    const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(mixedIsland), bodies);
    expectTrue(wakePreflight.reason == IslandWakeRejectReason::None,
    expectTrue(wakePreflight.should_wake_sleepers(), "wake preflight should wake mixed island");
    const IslandSleepGraphPreflight sleepGraphPreflight = preflight_island_sleep_graph(graph, bodies);
    expectTrue(sleepGraphPreflight.reason == IslandSleepGraphRejectReason::None,
    expectTrue(sleepGraphPreflight.has_solveable_islands(),
    const IslandWakeGraphPreflight wakeGraphPreflight = preflight_island_wake_graph(graph, bodies);
    expectTrue(wakeGraphPreflight.reason == IslandWakeGraphRejectReason::None,
    expectTrue(wakeGraphPreflight.can_wake(), "wake graph preflight can wake mixed island");
    const IslandConstraintSolvePreflight solvePreflight = preflight_island_constraint_solve(
    expectTrue(solvePreflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies,
                   IslandConstraintSolveRejectReason::OutOfRangeIslandIndex,
    testIslandRejectReasonGuards();

// --- deepen additive from pbd-island-deepen-preflights-1b60 ---
void testPreflightIslandBuildRejectReasonGuards() {
                   static_cast<fuse::u32>(IslandBuildRejectReason::OutOfRangeContactRefs),
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::OutOfRangeContactRefs),
    ContactIslandGraph withPreflightGraph;
    expectTrue(build_island_graph_with_preflight(withPreflightGraph, 4, contacts, constraints) ==
void testPreflightIslandConstraintSolveDeepenGuards() {
    const IslandConstraintSolvePreflight mixedPreflight = preflight_island_constraint_solve(
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint-solve deepen preflight");
    expectTrue(static_cast<fuse::u32>(mixedPreflight.reason) ==
                   static_cast<fuse::u32>(IslandConstraintSolveRejectReason::None),
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint-solve deepen preflight");
                                                          IslandConstraintSolveRejectReason::AllSleeping),
    const IslandConstraintSolveGraphPreflight graphPreflight =
    expectTrue(graphPreflight.has_solveable_islands(), "graph constraint-solve preflight has solveable island");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "graph counts one solveable island");
                                                          IslandConstraintSolveRejectReason::StaleConstraintRefs),
    const IslandDispatchDeepenPreflight dispatchPreflight = preflight_island_dispatch_deepen(
    expectTrue(dispatchPreflight.can_dispatch(), "deepen dispatch preflight accepts mixed island");
void testPreflightIslandSleepWakeRejectReasonGuards() {
                                                     IslandSleepSolveRejectReason::AllSleeping),
    expectTrue(island_wake_reject_reason(graph.island(mixedIsland), bodies) == IslandWakeRejectReason::None,
                                              IslandWakeRejectReason::NoMixedSleepState),
    testPreflightIslandBuildRejectReasonGuards();
    testPreflightIslandConstraintSolveDeepenGuards();
    testPreflightIslandSleepWakeRejectReasonGuards();

// --- deepen additive from deepen-pbd-island-reject-reasons-a666 ---
    expectTrue(island_build_rejects_for_reason(4, contacts, constraints, IslandBuildRejectReason::OutOfRangeRefs),
    expectTrue(island_build_reject_reason_name(IslandBuildRejectReason::OutOfRangeRefs) != nullptr,
    expectTrue(island_build_reject_reason(4, {}, {}) == IslandBuildRejectReason::None,
    expectTrue(buildPreflight.reason == IslandBuildRejectReason::OutOfRangeRefs,
    expectTrue(island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason::NoMovableBodies) !=
    expectTrue(island_sleep_reject_reason(graph.island(mixedIsland), bodies) == IslandSleepRejectReason::None,
                                              IslandWakeRejectReason::NoWakeTarget),
    expectTrue(sleepGraph.reason == IslandSleepGraphRejectReason::None,
    expectTrue(island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason::None) != nullptr,
    expectTrue(wakeGraph.reason == IslandWakeGraphRejectReason::None,
    expectTrue(island_sleep_graph_reject_reason(emptyGraph, bodies) == IslandSleepGraphRejectReason::EmptyGraph,
    expectTrue(island_wake_graph_reject_reason(emptyGraph, bodies) == IslandWakeGraphRejectReason::EmptyGraph,

// --- deepen additive from deepen-pbd-island-preflights-573c ---
void testPreflightIslandBuildDeepenGuards() {
    expectTrue(island_build_rejects_for_reason(0, {}, {}, IslandBuildRejectReason::EmptyInputs),
    expectTrue(std::strcmp(island_build_reject_reason_name(IslandBuildRejectReason::EmptyInputs), "EmptyInputs") ==
    const IslandBuildDeepenPreflight deepenPreflight = preflight_island_build_deepen(4, contacts, constraints);
    expectTrue(!deepenPreflight.can_build(), "build deepen preflight cannot build unsafe refs");
    expectTrue(deepenPreflight.reason == IslandBuildRejectReason::OutOfRangeContactBodies,
    const IslandConstraintSolveDeepenPreflight mixedDeepen = preflight_island_constraint_solve_deepen(
    expectTrue(mixedDeepen.reason == IslandConstraintSolveRejectReason::None,
    const IslandConstraintSolvePreflight byIndexPreflight = preflight_island_constraint_solve_by_index(
    expectTrue(byIndexPreflight.can_solve(), "constraint-solve by-index preflight succeeds for mixed island");
    const IslandConstraintSolvePreflight outOfRangePreflight = preflight_island_constraint_solve_by_index(
    expectTrue(outOfRangePreflight.skipped, "constraint-solve by-index skips out-of-range island");
               "should_skip constraint-solve by-index on out-of-range island");
void testDispatchSolveIslandWithPreflightGuards() {
    const IslandFullDispatchPreflight mixedDispatchPreflight = preflight_dispatch_solve_island(
    expectTrue(mixedDispatchPreflight.can_dispatch(), "full dispatch preflight allows mixed island");
    expectTrue(mixedDispatchPreflight.wake.should_wake_sleepers(),
                   IslandSleepRejectReason::NotAllSleeping,
    expectTrue(std::strcmp(island_wake_reject_reason_name(IslandWakeRejectReason::NoMixedSleepState),
    testPreflightIslandBuildDeepenGuards();
    testDispatchSolveIslandWithPreflightGuards();

// --- deepen additive from deepen-pbd-island-guards-9a14 ---
void testGuardedIslandSleepAwareDispatch() {
    const IslandConstraintSolvePreflight outOfRangeSolve =
    const IslandSolveBodiesPreflight outOfRangeBodies =
    const IslandSleepAwareDispatchPreflight mixedPreflight =
    expectTrue(mixedPreflight.can_dispatch(), "sleep-aware dispatch preflight allows mixed island");
               "should_skip sleep-aware dispatch false for mixed island");
    const IslandSleepAwareDispatchPreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.can_dispatch(), "sleep-aware dispatch preflight rejects all-sleeping island");
    expectTrue(should_skip_island_sleep_aware_dispatch(
               "should_skip sleep-aware dispatch true for all-sleeping island");
    const IslandSleepAwareGraphPreflight graphPreflight = preflight_island_sleep_aware_graph(graph, bodies, dt);
    expectTrue(graphPreflight.can_dispatch(), "sleep-aware graph preflight has dispatchable islands");

// --- deepen additive from deepen-pbd-island-guards-ecc4 ---
void testSolveIslandJobWithBodiesGuards() {
    const IslandConstraintSolveJobPreflight mixedPreflight =
    expectTrue(mixedPreflight.can_solve(), "mixed island job preflight can solve");
    expectTrue(!should_skip_solve_island_job_with_bodies(mixedJob, bodies, work.contactManifolds(), constraints, dt),
    const IslandConstraintSolveJobPreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island job preflight cannot solve");
    expectTrue(should_skip_solve_island_job_with_bodies(sleepingJob, bodies, work.contactManifolds(), constraints, dt),
void testIslandWakeResultAndSleepDispatchGuards() {
    const IslandSleepDispatchPreflight sleepDispatch = preflight_island_sleep_dispatch(graph, bodies, dt);
    expectTrue(!should_skip_island_sleep_dispatch(graph, bodies, dt),
               "should_skip sleep dispatch false for mixed graph");
    expectTrue(should_skip_island_sleep_dispatch(graph, bodies, dt),
               "should_skip sleep dispatch true when every island is all-sleeping");

// --- deepen additive from deepen-pbd-island-guards-1f40 ---
void testIslandBuildResultAndDegenerateGuards() {
    const IslandBuildPreflight preflight = preflight_island_build(2, contacts, constraints);
               "should_skip_island_build on degenerate refs");
    expectTrue(!should_skip_island_constraint_solve_graph(graph, bodies, contacts, constraints),
               "should_skip constraint-solve graph false when mixed island exists");
    expectTrue(should_skip_island_constraint_solve_graph(graph, bodies, contacts, constraints),
               "should_skip constraint-solve graph true when all islands are sleeping");
void testSolveIslandJobGuardedAndConstraintDispatch() {
void testPreflightIslandSleepWakeCombinedGuards() {
    const IslandSleepWakePreflight combined = preflight_island_sleep_wake(graph.island(mixedIsland), bodies);
    const IslandSleepWakePreflight byIndex =
    const IslandDispatchSolveablePreflight dispatchPreflight =
    expectTrue(dispatchPreflight.can_dispatch(), "dispatch-solveable preflight can dispatch");
    expectTrue(!should_skip_island_dispatch_solveable(graph, bodies, contacts, constraints, dt),
               "should_skip dispatch-solveable false for mixed graph");
    expectTrue(should_skip_island_dispatch_solveable(emptyGraph, bodies, contacts, constraints, dt),
               "should_skip dispatch-solveable true for empty graph");
    testPreflightIslandSleepWakeCombinedGuards();

// --- deepen additive from deepen-pbd-island-guards-0bfe ---
void testIslandBuildResultGuards() {
    expectTrue(should_skip_island_constraint_solve_by_index(graph, sleepingIsland, bodies, contacts, constraints),
    expectTrue(!graphPreflight.skipped, "constraint solve graph preflight has solveable island");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "constraint solve graph counts solveable island");
    expectTrue(graphPreflight.stats.blockedByBodiesCount == 1u,
               "should_skip constraint solve graph false when mixed island exists");
    const IslandSolveBodiesPreflight bodiesByIndex =
    expectTrue(!should_skip_island_sleep_dispatch(graph, bodies, 1.f / 60.f),
    const IslandSleepDispatchPreflight allSleepingDispatch =
    expectTrue(should_skip_island_sleep_dispatch(graph, bodies, 1.f / 60.f),
               "should_skip sleep dispatch true for all-sleeping graph");

// --- deepen additive from deepen-pbd-island-guards-a022 ---
void testContactIslandGraphBuildGuarded() {
    const IslandGraphBuildPreflight preflight = preflightIslandGraphBuild(4, contacts, constraints);
void testPreflightIslandSolvePassGuards() {
    const IslandSolvePassPreflight mixedPreflight =
    expectTrue(!mixedPreflight.skipped, "solve-pass preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes solve-pass preflight");
    expectTrue(!mixedPreflight.sleep.allSleeping, "mixed island is not all-sleeping in solve-pass preflight");
    const IslandSolvePassPreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails solve-pass preflight");
    expectTrue(should_skip_island_solve_pass(graph.island(sleepingIsland), bodies, work.contactManifolds(), constraints),
               "should_skip_island_solve_pass on all-sleeping island");
    const IslandSolvePassPreflight outOfRange =
void testDispatchSolveIslandWithWakeGuarded() {
void testDispatchAllIslandsWithWakeGuarded() {
    testPreflightIslandSolvePassGuards();

// --- deepen additive from deepen-pbd-island-guards-2fe2 ---
void testPreflightBuiltIslandGraphGuards() {
    const IslandBuiltGraphPreflight preflight =
    expectTrue(!should_skip_built_island_graph(graph, 4, contacts, constraints),
               "should_skip false for consistent built graph");
    const IslandBuiltGraphPreflight lonePreflight =
    expectTrue(lonePreflight.is_consistent(), "lone-body built graph has no unsafe refs");
    const IslandBuiltGraphPreflight emptyPreflight =
    expectTrue(emptyPreflight.skipped, "built graph preflight skips when graph is empty");
void testPreflightIslandConstraintSolveByIndex() {
    expectTrue(mixedPreflight.can_solve(), "mixed island passes constraint solve by index");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails constraint solve by index");
void testPreflightIslandDispatchSleepGuards() {
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(graph, bodies, dt);
    expectTrue(!should_skip_island_dispatch_sleep(graph, bodies, dt),
               "should_skip dispatch-sleep false for mixed graph");
    const IslandDispatchSleepPreflight allSleeping = preflight_island_dispatch_sleep(graph, bodies, dt);
    expectTrue(should_skip_island_dispatch_sleep(graph, bodies, dt),
               "should_skip dispatch-sleep true for all-sleeping graph");
    const IslandSolveableGraphPreflight solveablePreflight =
    expectTrue(solveablePreflight.has_solveable(), "solveable graph preflight has one island");
    expectTrue(!should_skip_island_solveable_graph(graph, bodies, work.contactManifolds(), constraints),
               "should_skip solveable graph false for mixed graph");
    testPreflightBuiltIslandGraphGuards();
    testPreflightIslandConstraintSolveByIndex();
    testPreflightIslandDispatchSleepGuards();

// --- deepen additive from deepen-pbd-island-guards-b232 ---
                   mixed, bodies, contacts, constraints, IslandConstraintSolveRejectReason::None),
                   allSleeping, bodies, contacts, constraints, IslandConstraintSolveRejectReason::AllSleeping),
                   staleIsland, bodies, contacts, constraints, IslandConstraintSolveRejectReason::StaleRefs),
                   emptyIsland, bodies, contacts, constraints, IslandConstraintSolveRejectReason::EmptyIsland),
    expectTrue(std::strcmp(island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason::AllSleeping),
    const IslandWakeThenSolvePreflight preflight =
    expectTrue(!should_skip_wake_then_solve_island(graph.island(mixedIsland), bodies, contacts, constraints),
               "should_skip wake-then-solve false for mixed island");

// --- deepen additive from deepen-pbd-island-guards-491a ---
    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve_by_index(
               "should_skip true for out-of-range island index");
void testPreflightIslandSolveableGraph() {
    const IslandSolveableGraphPreflight preflight = preflight_island_solveable_graph(
               "should_skip false when solveable island exists");
    const IslandSolveableGraphPreflight allActive = preflight_island_solveable_graph(
void testOutOfRangeBodyCountInPreflights() {
    const IslandSolveBodiesPreflight bodiesPreflight = preflight_island_solve_bodies(island, bodies);
    expectTrue(bodiesPreflight.outOfRangeBodyCount == 1u,
    expectTrue(bodiesPreflight.inRangeBodyCount == 2u, "solve-bodies preflight counts in-range bodies");
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep(island, bodies);
    expectTrue(sleepPreflight.outOfRangeBodyCount == 1u, "sleep preflight counts out-of-range body index");
    const IslandWakePreflight wakePreflight = preflight_island_wake(island, bodies);
    expectTrue(wakePreflight.outOfRangeBodyCount == 1u, "wake preflight counts out-of-range body index");
    testPreflightIslandSolveableGraph();
    testOutOfRangeBodyCountInPreflights();

// --- deepen additive from deepen-pbd-island-guards-c9b6 ---
    expectTrue(preflight.reason == IslandBuildRejectReason::DegenerateRefs,

// --- deepen additive from deepen-pbd-island-guards-efe7 ---
void testContactIslandGraphBuildGuardsOorRefs() {
void testPreflightIslandSolveDispatchGuards() {
    const IslandSolveDispatchPreflight mixedPreflight =
    expectTrue(!mixedPreflight.skipped, "solve-dispatch preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_dispatch(), "mixed island can dispatch constraint solve");
    expectTrue(!should_skip_island_solve_dispatch(graph.island(mixedIsland),
               "should_skip solve-dispatch false for mixed island");
    const IslandSolveDispatchPreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.can_dispatch(), "all-sleeping island cannot dispatch solve");
    expectTrue(should_skip_island_solve_dispatch(graph.island(sleepingIsland),
               "should_skip solve-dispatch true for all-sleeping island");
    const IslandSolveDispatchPreflight outOfRange =
    const IslandSleepSolveDispatchPreflight sleepDispatch = preflight_island_sleep_dispatch(graph, bodies, 1.f / 60.f);
               "should_skip sleep-dispatch false for mixed graph");
               "should_skip sleep-dispatch true when all constrained islands sleep");
void testDispatchSolveIslandGuarded() {
    testPreflightIslandSolveDispatchGuards();

// --- deepen additive from deepen-pbd-island-sleep-wake-dispatch-db11 ---
void testPreflightIslandSleepWakeDispatch() {
    const IslandSleepWakeDispatchPreflight preflight = preflight_island_sleep_wake_dispatch(graph, bodies, dt);
    expectTrue(!should_skip_island_sleep_wake_dispatch(graph, bodies, dt),
               "should_skip false for mixed sleep/wake graph");
    expectTrue(should_skip_island_sleep_wake_dispatch(emptyGraph, bodies, dt),
               "should_skip true for empty graph");
void testDispatchSolveIslandWithSleepWakeGuards() {
void testDispatchAllIslandsWithSleepWakeGuards() {
    testPreflightIslandSleepWakeDispatch();

// --- deepen additive from deepen-pbd-island-guards-3d4d ---
    const IslandSolveDispatchPreflight mixedPreflight = preflight_island_solve_dispatch(
    expectTrue(!mixedPreflight.skipped, "solve dispatch preflight does not skip mixed island");
    expectTrue(!should_skip_island_solve_dispatch(graph.island(mixedIsland), bodies, contacts, constraints),
               "should_skip false for mixed solveable island");
    const IslandSolveDispatchPreflight sleepingPreflight = preflight_island_solve_dispatch(
    expectTrue(sleepingPreflight.sleep.can_skip_solve(), "all-sleeping island can skip solve");
    expectTrue(should_skip_island_solve_dispatch(graph.island(sleepingIsland), bodies, contacts, constraints),
void testDispatchSolveIslandGuardedWakesAndSolves() {
void testDispatchAllIslandsGuardedBatch() {

// --- deepen additive from deepen-pbd-island-guards-d51d ---
void testPreflightIslandBuildSelfReferentialGuards() {
               "should_skip_island_build on self-referential refs");
    const IslandSolvePipelinePreflight mixedPipeline = preflight_island_solve_pipeline(
    const IslandSolvePipelinePreflight outOfRange =
    testPreflightIslandBuildSelfReferentialGuards();

// --- deepen additive from deepen-pbd-island-guards-ae58 ---
void testPreflightIslandBuildSelfContactGuard() {
    const IslandBuildPreflight preflight = preflight_island_build(2, contacts, {});
void testPreflightIslandConstraintRefsOutOfRangeCounts() {
    expectTrue(!mixedPreflight.skipped, "index constraint-solve preflight does not skip mixed island");
    expectTrue(mixedPreflight.can_solve(), "mixed island passes index constraint-solve preflight");
    expectTrue(!sleepingPreflight.can_solve(), "all-sleeping island fails index constraint-solve preflight");
               "should_skip index constraint-solve on all-sleeping island");
void testDispatchSolveIslandConstraintGuarded() {
    const IslandWakeThenSolvePreflight preflight = preflight_wake_then_solve_island_by_index(
               "should_skip solveable graph false when mixed island exists");
    testPreflightIslandBuildSelfContactGuard();
    testPreflightIslandConstraintRefsOutOfRangeCounts();

// --- deepen additive from deepen-pbd-island-guards-fbc0 ---
void testPreflightSolveIslandJobFullGuards() {
    const IslandFullSolvePreflight preflight =
    expectTrue(!should_skip_solve_island_job_full(job, bodies, work.contactManifolds(), constraints, 1.f / 60.f),
               "should_skip false for solveable island");
    const IslandFullSolvePreflight sleepingPreflight =
    expectTrue(!sleepingPreflight.can_solve(), "full job preflight rejects all-sleeping island");
    expectTrue(should_skip_solve_island_job_full(job, bodies, work.contactManifolds(), constraints, 1.f / 60.f),
void testWakeAndSolveIslandGuarded() {
    const IslandSleepWakeSolvePreflight mixedPreflight = preflight_island_sleep_wake_solve(
    expectTrue(!mixedPreflight.skipped, "sleep-wake-solve preflight does not skip mixed island");
    expectTrue(mixedPreflight.needs_wake(), "mixed island needs wake before solve");
    expectTrue(mixedPreflight.can_solve(), "mixed island can solve after wake");
    expectTrue(should_skip_island_sleep_wake_solve(
               "should_skip sleep-wake-solve for all-sleeping island");
    const IslandSleepWakeSolveGraphPreflight graphPreflight = preflight_island_sleep_wake_solve_graph(
    expectTrue(!graphPreflight.skipped, "graph sleep-wake-solve preflight has solveable island");
    expectTrue(graphPreflight.stats.solveableCount == 1u, "graph counts one solveable island after wake");
void testPreflightIslandSolveBodiesOutOfRangeCount() {
    const IslandSolveBodiesPreflight preflight = preflight_island_solve_bodies(island, bodies);
    testPreflightSolveIslandJobFullGuards();
    testPreflightIslandSolveBodiesOutOfRangeCount();

// --- deepen additive from deepen-pbd-island-guards-dcf7 ---
void testBuildIslandGraphInRangeGuarded() {
    expectTrue(!should_skip_island_constraint_solve_graph(graph, bodies, work.contactManifolds(), constraints),
               "should_skip constraint-solve graph false when mixed islands exist");
    const IslandSolveBodiesPreflight staleBodies = preflight_island_solve_bodies(staleIsland, bodies);
void testSolveIslandJobGuardedAndSleepAwareDispatch() {
    const IslandSleepAwareDispatchPreflight sleepDispatch =
    expectTrue(!should_skip_island_sleep_aware_dispatch(graph, freshBodies, dt),
