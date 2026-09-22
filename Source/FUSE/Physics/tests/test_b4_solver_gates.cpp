// B4.11 solver gate rows (master plan):
//  - single sphere under gravity hits the ground plane at t = sqrt(2h/g)
//  - stack of 10 spheres stays stable at rest after 5 s — no drift or explosion
//  - restitution 1.0 produces an elastic bounce (equal rebound height)
//  - friction stops a sliding box at the analytical distance v0^2 / (2 mu g)
//  - distance constraint holds two bodies at rest_length +- 0.01 under external force
//  - sleep detection deactivates resting bodies (zero velocity reads)
#include <fuse/core/init.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

constexpr f32 kG = 9.81f;
constexpr f32 kDt = 1.f / 60.f;

SolverParams undampedParams() {
    SolverParams params;
    params.substeps = 8;
    params.iterations = 10;
    params.gravity = {0.f, -kG, 0.f};
    params.linearDamping = 1.f;
    params.angularDamping = 1.f;
    params.broadphase.cellSize = 2.f;
    params.broadphase.tableSize = 1024;
    return params;
}

u32 addGround(RigidBodySoA& bodies, CollisionShapeSoA& shapes) {
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    return ground;
}

u32 addSphere(RigidBodySoA& bodies, CollisionShapeSoA& shapes, vec3 position, f32 radius) {
    const u32 body = bodies.addBody(position, 1.f);
    shapes.addShape(CollisionShapeType::Sphere, body, {radius, 0.f, 0.f});
    return body;
}

void testFreeFallTime() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const f32 h = 10.f;
    const f32 r = 0.5f;
    const u32 sphere = addSphere(bodies, shapes, {0.f, h + r, 0.f}, r);
    addGround(bodies, shapes);
    bodies.restitutions[sphere] = 0.f; // stays in contact once it lands

    PBDSolver solver;
    solver.init(2, 8, 0);
    const SolverParams params = undampedParams();
    f32 hitTime = -1.f;
    for (int frame = 1; frame <= 240 && hitTime < 0.f; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        if (solver.contactCount() > 0u) {
            hitTime = static_cast<f32>(frame) * kDt;
        }
    }
    const f32 expected = std::sqrt(2.f * h / kG);
    std::printf("free fall: hit at %.4f s, analytic %.4f s, rest y %.4f\n", hitTime, expected,
                bodies.positions[sphere].y);
    // Contact is detected in the frame whose motion reaches the plane.
    expectTrue(hitTime >= expected - 1e-4f && hitTime <= expected + kDt + 1e-4f,
               "sphere reaches the ground at t = sqrt(2h/g) (within one frame)");
}

void testRestitutionElasticBounce() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const f32 h = 2.f;
    const f32 r = 0.5f;
    const u32 sphere = addSphere(bodies, shapes, {0.f, h + r, 0.f}, r);
    const u32 ground = addGround(bodies, shapes);
    bodies.restitutions[sphere] = 1.f;
    bodies.restitutions[ground] = 1.f;

    PBDSolver solver;
    solver.init(2, 8, 0);
    SolverParams params = undampedParams();
    params.sleepTimeRequired = 1e9f;
    bool bounced = false;
    f32 apex = 0.f;
    f32 previousVy = 0.f;
    for (int frame = 0; frame < 240; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        const f32 vy = bodies.linearVelocities[sphere].y;
        if (!bounced && previousVy < 0.f && vy > 0.f) {
            bounced = true;
        }
        if (bounced) {
            apex = std::max(apex, bodies.positions[sphere].y - r);
            if (previousVy > 0.f && vy <= 0.f) {
                break; // first apex after the bounce
            }
        }
        previousVy = vy;
    }
    std::printf("restitution 1.0: dropped from %.3f m, rebound apex %.3f m\n", h, apex);
    expectTrue(bounced, "sphere bounces off the plane");
    expectTrue(std::fabs(apex - h) < 0.05f * h, "elastic bounce returns to the drop height (within 5%)");

    // e = 0 on either side: no bounce.
    RigidBodySoA inelastic;
    CollisionShapeSoA inelasticShapes;
    const u32 dead = addSphere(inelastic, inelasticShapes, {0.f, h + r, 0.f}, r);
    addGround(inelastic, inelasticShapes);
    inelastic.restitutions[dead] = 0.f;
    PBDSolver solver2;
    solver2.init(2, 8, 0);
    f32 maxAfter = 0.f;
    bool landed = false;
    for (int frame = 0; frame < 180; ++frame) {
        solver2.step(inelastic, inelasticShapes, params, kDt);
        landed = landed || solver2.contactCount() > 0u;
        if (landed) {
            maxAfter = std::max(maxAfter, inelastic.positions[dead].y - r);
        }
    }
    std::printf("restitution 0.0: max height after landing %.4f m\n", maxAfter);
    expectTrue(landed && maxAfter < 0.05f, "restitution 0 does not bounce");
}

void testStackOfTenStable() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const f32 r = 0.5f;
    u32 first = 0;
    for (u32 i = 0; i < 10u; ++i) {
        const u32 s = addSphere(bodies, shapes, {0.f, r + 2.f * r * static_cast<f32>(i), 0.f}, r);
        first = i == 0u ? s : first;
    }
    addGround(bodies, shapes);

    PBDSolver solver;
    solver.init(16, 64, 0);
    SolverParams params = undampedParams();
    params.iterations = 10;
    f32 worstSpeed = 0.f;
    for (int frame = 0; frame < 300; ++frame) { // 5 s
        solver.step(bodies, shapes, params, kDt);
        for (u32 i = 0; i < 10u; ++i) {
            worstSpeed = std::max(worstSpeed, bodies.linearVelocities[first + i].length());
        }
    }
    f32 worstLateral = 0.f;
    f32 worstVertical = 0.f;
    for (u32 i = 0; i < 10u; ++i) {
        const vec3 p = bodies.positions[first + i];
        worstLateral = std::max(worstLateral, std::sqrt(p.x * p.x + p.z * p.z));
        worstVertical = std::max(worstVertical, std::fabs(p.y - (r + 2.f * r * static_cast<f32>(i))));
    }
    std::printf("stack of 10: lateral drift %.5f m, vertical error %.4f m, peak speed %.4f m/s\n", worstLateral,
                worstVertical, worstSpeed);
    expectTrue(worstLateral < 1e-3f, "stack does not drift sideways");
    expectTrue(worstVertical < 0.05f, "stack holds its rest heights (no sinking or explosion)");
    expectTrue(worstSpeed < 1.f, "no body gains explosive velocity");
}

void testFrictionStopsSlidingBox() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const f32 mu = 0.5f;
    const f32 v0 = 5.f;
    const u32 box = bodies.addBody({0.f, 0.5f, 0.f}, 1.f);
    shapes.addShape(CollisionShapeType::Box, box, {0.5f, 0.5f, 0.5f});
    const u32 ground = addGround(bodies, shapes);
    bodies.frictionDynamic[box] = mu;
    bodies.frictionDynamic[ground] = mu;
    bodies.frictionStatic[box] = mu;
    bodies.frictionStatic[ground] = mu;
    bodies.restitutions[box] = 0.f;
    bodies.linearVelocities[box] = {v0, 0.f, 0.f};

    PBDSolver solver;
    solver.init(2, 8, 0);
    SolverParams params = undampedParams();
    int framesToStop = -1;
    for (int frame = 1; frame <= 240; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        if (framesToStop < 0 && std::fabs(bodies.linearVelocities[box].x) < 1e-3f) {
            framesToStop = frame;
        }
    }
    const f32 expected = v0 * v0 / (2.f * mu * kG);
    const f32 travelled = bodies.positions[box].x;
    std::printf("friction: box slid %.4f m (analytic %.4f m), stopped after %d frames (analytic %.1f)\n", travelled,
                expected, framesToStop, v0 / (mu * kG) / kDt);
    expectTrue(std::fabs(travelled - expected) < 0.05f * expected, "sliding distance matches v0^2/(2 mu g) within 5%");
    expectTrue(framesToStop > 0, "box comes to rest");
    expectTrue(std::fabs(bodies.positions[box].y - 0.5f) < 0.02f, "box stays on the ground while sliding");
}

void testDistanceConstraintUnderForce() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 anchor = bodies.addBody({0.f, 5.f, 0.f}, 0.f, RB_STATIC);
    const u32 bob = bodies.addBody({2.f, 5.f, 0.f}, 1.f);
    shapes.addShape(CollisionShapeType::Sphere, anchor, {0.1f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bob, {0.1f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(2, 8, 1);
    DistanceConstraint link{};
    link.bodyA = anchor;
    link.bodyB = bob;
    link.restLength = 2.f;
    solver.setDistanceConstraints({link});
    SolverParams params = undampedParams();
    f32 worst = 0.f;
    for (int frame = 0; frame < 240; ++frame) {
        bodies.forces[bob] = {20.f * std::sin(static_cast<f32>(frame) * 0.1f), -30.f, 15.f};
        solver.step(bodies, shapes, params, kDt);
        worst = std::max(worst, std::fabs((bodies.positions[bob] - bodies.positions[anchor]).length() - 2.f));
    }
    std::printf("distance constraint: worst |len - rest| %.5f m over 4 s of forcing\n", worst);
    expectTrue(worst < 0.01f, "distance constraint holds rest_length +- 0.01 under external force");
}

void testSleepDeactivatesRestingBodies() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 resting = addSphere(bodies, shapes, {0.f, 0.5f, 0.f}, 0.5f);
    const u32 moving = addSphere(bodies, shapes, {5.f, 3.f, 0.f}, 0.5f);
    addGround(bodies, shapes);
    bodies.restitutions[resting] = 0.f;
    bodies.restitutions[moving] = 0.f;

    PBDSolver solver;
    solver.init(3, 8, 0);
    SolverParams params = undampedParams();
    params.sleepTimeRequired = 0.5f;
    bool movingAsleepEarly = false;
    for (int frame = 0; frame < 45; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        movingAsleepEarly = movingAsleepEarly || (bodies.flags[moving] & RB_SLEEPING) != 0u;
    }
    const bool restingAsleep = (bodies.flags[resting] & RB_SLEEPING) != 0u;
    std::printf("sleep: resting asleep=%d (v=%.6f), falling asleep during fall=%d\n", restingAsleep ? 1 : 0,
                bodies.linearVelocities[resting].length(), movingAsleepEarly ? 1 : 0);
    expectTrue(restingAsleep, "resting body is put to sleep");
    expectTrue(bodies.linearVelocities[resting].length() == 0.f, "sleeping body reads zero velocity");
    expectTrue(!movingAsleepEarly, "falling body never sleeps mid-air");
    for (int frame = 0; frame < 120; ++frame) {
        solver.step(bodies, shapes, params, kDt);
    }
    expectTrue((bodies.flags[moving] & RB_SLEEPING) != 0u && bodies.linearVelocities[moving].length() == 0.f,
               "landed body sleeps once at rest");
    expectTrue(std::fabs(bodies.positions[moving].y - 0.5f) < 0.02f, "sleeping body rests on the ground");
    expectTrue(solver.activeBodyCount() == 0u, "no active bodies once everything sleeps");
}

} // namespace

int main() {
    fuse::core::initialize();
    testFreeFallTime();
    testRestitutionElasticBounce();
    testStackOfTenStable();
    testFrictionStopsSlidingBox();
    testDistanceConstraintUnderForce();
    testSleepDeactivatesRestingBodies();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_solver_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_solver_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
