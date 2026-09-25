// B4 joint gate rows (anchored distance constraints with rotational terms):
//  - a point-mass pendulum swings with the analytic small-angle period 2 pi sqrt(L / g)
//  - a rod pinned off-centre (ball-socket at its end) swings with the physical pendulum period
//    2 pi sqrt(I_pivot / (m g d)) and keeps its pivot
//  - a chain of 10 linked boxes carrying a heavy load holds every link length to +-0.01 m
//  - pendulums and the chain never gain energy (the position solve dissipates like implicit Euler:
//    a 1 m pendulum at 4 substeps / 60 Hz keeps ~2/3 of its swing energy after 10 s)
//  - a hinge (two ball-sockets on the axis) turns about its axis only
// Angle-limited joints (JointConstraint, XPBD angular constraints) and the PhysicsManager joint API:
//  - a hinge swinging hard into its min/max limits stops at them (overshoot < tolerance), no energy gain
//  - a ball-socket swing cone holds a spinning, orbiting load inside the cone
//  - a ball-socket twist limit holds a spinning load inside [minTwist, maxTwist]
//  - joints created through PhysicsManager (ECS entities) reproduce the solver-level trajectory
//  - destroying an entity (or a joint) removes its joints cleanly and wakes the survivors
//  - break force / torque thresholds raise exactly one JointBreak event and free the body
//  - fixed, distance, rope and spring joints hold pose / length / slack / m g / k stretch
//  - a ragdoll-like chain of 10 limited joints lying on the ground stays stable for 10 s
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
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

constexpr f32 kG = 9.81f;
constexpr f32 kDt = 1.f / 60.f;
constexpr f32 kPi = 3.14159265f;

SolverParams jointParams() {
    SolverParams params;
    params.substeps = 4;
    params.iterations = 10;
    params.gravity = {0.f, -kG, 0.f};
    params.linearDamping = 1.f;
    params.angularDamping = 1.f;
    params.broadphase.cellSize = 2.f;
    params.broadphase.tableSize = 1024;
    return params;
}

u32 addPivot(RigidBodySoA& bodies, vec3 position) {
    return bodies.addBody(position, 0.f, RB_STATIC);
}

u32 addBox(RigidBodySoA& bodies, CollisionShapeSoA& shapes, vec3 position, vec3 halfExtents, quat orientation,
           f32 invMass, u32 layer = 1u, u32 mask = 0xFFFFFFFFu) {
    const u32 body = bodies.addBody(position, invMass, 0u, layer, mask);
    shapes.addShape(CollisionShapeType::Box, body, halfExtents);
    bodies.orientations[body] = orientation;
    bodies.predictedOrientations[body] = orientation;
    return body;
}

/// Kinetic (linear + rotational) plus gravitational potential energy of the listed bodies.
f32 mechanicalEnergy(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes, const std::vector<u32>& list) {
    f32 energy = 0.f;
    for (u32 body : list) {
        const f32 mass = 1.f / bodies.invMasses[body];
        const vec3 v = bodies.linearVelocities[body];
        energy += 0.5f * mass * v.dot(v) + mass * kG * bodies.positions[body].y;
        u32 shape = 0;
        while (shape < shapes.count() && shapes.bodyIndices[shape] != body) {
            ++shape;
        }
        if (shape == shapes.count()) {
            continue;
        }
        const vec3 invInertia = shapeInverseInertia(static_cast<CollisionShapeType>(shapes.types[shape]),
                                                    shapes.params[shape], bodies.invMasses[body]);
        const vec3 w = inverseRotate(bodies.orientations[body], bodies.angularVelocities[body]);
        energy += 0.5f * (w.x * w.x / invInertia.x + w.y * w.y / invInertia.y + w.z * w.z / invInertia.z);
    }
    return energy;
}

/// Exact large-amplitude pendulum period factor T / T0 = 1 / AGM(1, cos(theta0 / 2)).
f32 amplitudeFactor(f32 amplitude) {
    double a = 1.0;
    double b = std::cos(0.5 * static_cast<double>(amplitude));
    for (int i = 0; i < 8; ++i) {
        const double mean = 0.5 * (a + b);
        b = std::sqrt(a * b);
        a = mean;
    }
    return static_cast<f32>(1.0 / a);
}

/// Mean period between successive upward zero crossings of the swing angle (linear interpolation).
struct PeriodMeter {
    f32 previousAngle = 0.f;
    f32 previousTime = 0.f;
    std::vector<f32> crossings;
    bool first = true;

    void sample(f32 time, f32 angle) {
        if (!first && previousAngle < 0.f && angle >= 0.f) {
            const f32 fraction = -previousAngle / (angle - previousAngle);
            crossings.push_back(previousTime + fraction * (time - previousTime));
        }
        first = false;
        previousAngle = angle;
        previousTime = time;
    }

    f32 period() const {
        if (crossings.size() < 2u) {
            return 0.f;
        }
        return (crossings.back() - crossings.front()) / static_cast<f32>(crossings.size() - 1u);
    }
};

void testPointMassPendulum() {
    constexpr f32 kLength = 1.f;
    constexpr f32 kAmplitude = 0.1f;
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const vec3 pivotPosition{0.f, 3.f, 0.f};
    const u32 pivot = addPivot(bodies, pivotPosition);
    const vec3 start = pivotPosition + vec3{std::sin(kAmplitude), -std::cos(kAmplitude), 0.f} * kLength;
    const u32 bob = bodies.addBody(start, 1.f);
    shapes.addShape(CollisionShapeType::Sphere, bob, {0.02f, 0.f, 0.f});

    PBDSolver solver;
    solver.init(4, 16, 4);
    solver.setDistanceConstraints({DistanceConstraint{pivot, bob, {}, {}, kLength, 0.f}});
    const SolverParams params = jointParams();
    PeriodMeter meter;
    const f32 startEnergy = mechanicalEnergy(bodies, shapes, {bob});
    f32 peakEnergy = startEnergy;
    f32 worstLength = 0.f;
    for (int frame = 1; frame <= 600; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        const vec3 d = bodies.positions[bob] - pivotPosition;
        meter.sample(static_cast<f32>(frame) * kDt, std::atan2(d.x, -d.y));
        peakEnergy = std::max(peakEnergy, mechanicalEnergy(bodies, shapes, {bob}));
        worstLength = std::max(worstLength, std::fabs(d.length() - kLength));
    }
    const f32 expected = 2.f * kPi * std::sqrt(kLength / kG) * amplitudeFactor(kAmplitude);
    const f32 measured = meter.period();
    const f32 error = std::fabs(measured - expected) / expected;
    const f32 lost = startEnergy - mechanicalEnergy(bodies, shapes, {bob});
    std::printf("point-mass pendulum: period %.4f s (analytic %.4f s, error %.2f%%), length error %.5f m, "
                "energy gain %.5f J, energy lost %.5f J\n",
                measured, expected, error * 100.f, worstLength, peakEnergy - startEnergy, lost);
    expectTrue(meter.crossings.size() >= 4u, "pendulum swings for several periods");
    expectTrue(error < 0.01f, "point-mass pendulum period within 1% of 2 pi sqrt(L/g)");
    expectTrue(worstLength < 1e-3f, "pendulum string keeps its length");
    expectTrue(peakEnergy - startEnergy < 1e-3f * std::fabs(startEnergy) + 1e-4f, "pendulum never gains energy");
}

void testRodPendulum(f32 amplitude, bool checkPeriod) {
    const vec3 half{0.05f, 0.5f, 0.05f};
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const vec3 pivotPosition{0.f, 3.f, 0.f};
    const u32 pivot = addPivot(bodies, pivotPosition);
    const quat tilt = quatFromAxisAngle({0.f, 0.f, 1.f}, amplitude);
    // Rod hangs from its top end (local +Y): centre = pivot - R (0, h, 0).
    const vec3 centre = pivotPosition - rotate(tilt, {0.f, half.y, 0.f});
    const u32 rod = addBox(bodies, shapes, centre, half, tilt, 1.f);

    PBDSolver solver;
    solver.init(4, 16, 4);
    solver.setDistanceConstraints({ballSocketJoint(pivot, rod, {}, {0.f, half.y, 0.f})});
    const SolverParams params = jointParams();
    PeriodMeter meter;
    const f32 startEnergy = mechanicalEnergy(bodies, shapes, {rod});
    f32 peakEnergy = startEnergy;
    f32 worstPivot = 0.f;
    for (int frame = 1; frame <= 600; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        const vec3 top = bodies.positions[rod] + rotate(bodies.orientations[rod], {0.f, half.y, 0.f});
        worstPivot = std::max(worstPivot, (top - pivotPosition).length());
        const vec3 d = bodies.positions[rod] - pivotPosition;
        meter.sample(static_cast<f32>(frame) * kDt, std::atan2(d.x, -d.y));
        peakEnergy = std::max(peakEnergy, mechanicalEnergy(bodies, shapes, {rod}));
    }
    // Physical pendulum: I_pivot = I_cm + m d^2 about z, d = half.y.
    const f32 inertiaPivot = (half.x * half.x + half.y * half.y) / 3.f + half.y * half.y;
    const f32 expected = 2.f * kPi * std::sqrt(inertiaPivot / (kG * half.y)) * amplitudeFactor(amplitude);
    const f32 measured = meter.period();
    const f32 error = std::fabs(measured - expected) / expected;
    const f32 endEnergy = mechanicalEnergy(bodies, shapes, {rod});
    std::printf("rod pendulum (amplitude %.2f rad): period %.4f s (analytic %.4f s, error %.2f%%), pivot drift "
                "%.5f m, energy gain %.5f J, energy lost %.5f J\n",
                amplitude, measured, expected, error * 100.f, worstPivot, peakEnergy - startEnergy,
                startEnergy - endEnergy);
    if (checkPeriod) {
        expectTrue(meter.crossings.size() >= 4u, "rod swings for several periods");
        expectTrue(error < 0.015f, "rod pendulum period within 1.5% of the physical-pendulum period");
    }
    expectTrue(worstPivot < 2e-3f, "ball-socket keeps the rod's end at the pivot");
    expectTrue(peakEnergy - startEnergy < 1e-3f * std::fabs(startEnergy) + 1e-4f, "rod pendulum never gains energy");
}

void testChainUnderLoad() {
    constexpr u32 kLinks = 10u;
    constexpr f32 kGap = 0.1f;
    const vec3 half{0.1f, 0.2f, 0.1f};
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const vec3 pivotPosition{0.f, 8.f, 0.f};
    const u32 pivot = addPivot(bodies, pivotPosition);
    // The chain starts horizontal and swings down; the last link is a 10x heavier load. Links sit
    // on their own collision layer so neighbours only interact through the joints.
    const quat sideways = quatFromAxisAngle({0.f, 0.f, 1.f}, 0.5f * kPi);
    std::vector<DistanceConstraint> constraints;
    std::vector<u32> links;
    u32 previous = pivot;
    vec3 previousAnchor{};
    for (u32 i = 0; i < kLinks; ++i) {
        const f32 along = kGap + half.y + static_cast<f32>(i) * (2.f * half.y + kGap);
        const f32 invMass = i + 1u == kLinks ? 0.1f : 1.f;
        const u32 link = addBox(bodies, shapes, pivotPosition + vec3{along, 0.f, 0.f}, half, sideways, invMass, 2u, 1u);
        // Local +Y points back towards the pivot (rotated by +90 deg about z: +Y -> -X).
        constraints.push_back({previous, link, previousAnchor, {0.f, half.y, 0.f}, kGap, 0.f});
        links.push_back(link);
        previous = link;
        previousAnchor = {0.f, -half.y, 0.f};
    }

    PBDSolver solver;
    solver.init(kLinks + 1u, 64, kLinks);
    solver.setDistanceConstraints(constraints);
    const SolverParams params = jointParams();
    const f32 startEnergy = mechanicalEnergy(bodies, shapes, links);
    f32 peakEnergy = startEnergy;
    f32 worstLength = 0.f;
    f32 worstRestLength = 0.f;
    f32 lowestLoad = pivotPosition.y;
    for (int frame = 1; frame <= 600; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        f32 frameWorst = 0.f;
        for (const DistanceConstraint& c : constraints) {
            const vec3 a = bodies.positions[c.bodyA] + rotate(bodies.orientations[c.bodyA], c.localAnchorA);
            const vec3 b = bodies.positions[c.bodyB] + rotate(bodies.orientations[c.bodyB], c.localAnchorB);
            frameWorst = std::max(frameWorst, std::fabs((a - b).length() - c.restLength));
        }
        worstLength = std::max(worstLength, frameWorst);
        if (frame > 480) {
            worstRestLength = std::max(worstRestLength, frameWorst);
        }
        peakEnergy = std::max(peakEnergy, mechanicalEnergy(bodies, shapes, links));
        lowestLoad = std::min(lowestLoad, bodies.positions[links.back()].y);
    }
    std::printf("chain of %u boxes (10x load): worst link error %.5f m (swinging), %.5f m (last 2 s), energy gain "
                "%.5f J, load swung down to y = %.3f\n",
                kLinks, worstLength, worstRestLength, peakEnergy - startEnergy, lowestLoad);
    expectTrue(worstLength < 0.01f, "chain links hold their length within 0.01 m under load");
    expectTrue(peakEnergy - startEnergy < 1e-3f * std::fabs(startEnergy) + 1e-3f, "swinging chain never gains energy");
    expectTrue(lowestLoad < pivotPosition.y - 4.f, "chain swings down under its load");
}

void testHinge() {
    // A door (thin box) hinged along world Y on its left edge to a static post; spun about the hinge.
    const vec3 half{0.5f, 1.f, 0.05f};
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const vec3 hingePosition{0.f, 2.f, 0.f};
    const u32 post = addPivot(bodies, hingePosition);
    const u32 door = addBox(bodies, shapes, hingePosition + vec3{half.x, 0.f, 0.f}, half, {}, 1.f);
    bodies.angularVelocities[door] = {0.f, 2.f, 0.f};
    bodies.linearVelocities[door] = vec3{0.f, 2.f, 0.f}.cross({half.x, 0.f, 0.f});
    std::vector<DistanceConstraint> constraints;
    appendHingeJoint(constraints, post, door, {}, {-half.x, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f);
    PBDSolver solver;
    solver.init(4, 16, 4);
    solver.setDistanceConstraints(constraints);
    const SolverParams params = jointParams();
    f32 worstAxis = 0.f;
    f32 worstPivot = 0.f;
    for (int frame = 1; frame <= 120; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        const vec3 axis = rotate(bodies.orientations[door], {0.f, 1.f, 0.f});
        worstAxis = std::max(worstAxis, std::acos(std::min(axis.y, 1.f)) * 180.f / kPi);
        const vec3 pivotOnDoor = bodies.positions[door] + rotate(bodies.orientations[door], {-half.x, 0.f, 0.f});
        worstPivot = std::max(worstPivot, (pivotOnDoor - hingePosition).length());
    }
    const vec3 spin = bodies.angularVelocities[door];
    std::printf("hinge: axis tilt %.4f deg, pivot drift %.5f m, spin (%.3f, %.3f, %.3f) rad/s\n", worstAxis, worstPivot,
                spin.x, spin.y, spin.z);
    expectTrue(worstAxis < 1.f, "hinged door keeps its axis vertical under gravity");
    expectTrue(worstPivot < 5e-3f, "hinged door keeps its hinge line");
    expectTrue(spin.y > 1.8f, "hinged door keeps turning about its axis");
    expectTrue(std::sqrt(spin.x * spin.x + spin.z * spin.z) < 0.05f, "hinged door does not wobble off its axis");
}

// ---------------------------------------------------------------------------------------------
// Angle-limited joints (solver level)

constexpr u32 kWorld = kJointWorldBody;

/// Rod (box, long axis local Y) hanging from `pivot` by its top end, jointed to the world.
struct HangingRod {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    u32 rod = 0;
    vec3 pivot{0.f, 3.f, 0.f};
    vec3 half{0.05f, 0.5f, 0.05f};
};

void buildHangingRod(HangingRod& scene) {
    scene.rod = addBox(scene.bodies, scene.shapes, scene.pivot - vec3{0.f, scene.half.y, 0.f}, scene.half, {}, 1.f);
}

/// Joint from the rod's top end to the world pivot: axis = world/local -Y (down the rod), normal = +X.
JointConstraint rodJoint(const HangingRod& scene, JointType type) {
    JointConstraint joint;
    joint.type = type;
    joint.bodyA = scene.rod;
    joint.bodyB = kWorld;
    joint.localAnchorA = {0.f, scene.half.y, 0.f};
    joint.localAnchorB = scene.pivot;
    joint.localAxisA = {0.f, -1.f, 0.f};
    joint.localAxisB = {0.f, -1.f, 0.f};
    joint.localNormalA = {1.f, 0.f, 0.f};
    joint.localNormalB = {1.f, 0.f, 0.f};
    return joint;
}

void spinRod(HangingRod& scene, vec3 omega) {
    scene.bodies.angularVelocities[scene.rod] = omega;
    scene.bodies.linearVelocities[scene.rod] = omega.cross(scene.bodies.positions[scene.rod] - scene.pivot);
}

f32 degrees(f32 radians) {
    return radians * 180.f / kPi;
}

void testHingeLimits() {
    HangingRod scene;
    buildHangingRod(scene);
    JointConstraint hinge = rodJoint(scene, JointType::Hinge);
    hinge.localAxisA = {0.f, 0.f, 1.f};
    hinge.localAxisB = {0.f, 0.f, 1.f};
    hinge.hingeLimit = true;
    hinge.minAngle = -0.6f;
    hinge.maxAngle = 0.4f; // asymmetric: B (world) relative to A (rod) — rod angle in [-0.4, 0.6]
    // Kicked at 5 rad/s about the hinge: free, it would swing to ~81 deg.
    spinRod(scene, {0.f, 0.f, 5.f});
    PBDSolver solver;
    solver.init(4, 16, 4);
    solver.setJoints({hinge});
    const SolverParams params = jointParams();
    const f32 startEnergy = mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod});
    f32 peakEnergy = startEnergy;
    f32 maxAngle = -10.f;
    f32 minAngle = 10.f;
    f32 worstPivot = 0.f;
    f32 worstAxis = 0.f;
    f32 worstReported = 0.f;
    u32 limitHits = 0;
    bool atLimit = false;
    for (int frame = 1; frame <= 300; ++frame) {
        solver.step(scene.bodies, scene.shapes, params, kDt);
        const vec3 d = scene.bodies.positions[scene.rod] - scene.pivot;
        const f32 angle = std::atan2(d.x, -d.y); // rod angle from vertical, about +z
        maxAngle = std::max(maxAngle, angle);
        minAngle = std::min(minAngle, angle);
        const bool nowAtLimit = angle > 0.55f || angle < -0.35f;
        limitHits += nowAtLimit && !atLimit ? 1u : 0u;
        atLimit = nowAtLimit;
        worstReported = std::max(worstReported, std::fabs(-jointHingeAngle(scene.bodies, hinge) - angle));
        const vec3 top = scene.bodies.positions[scene.rod] + rotate(scene.bodies.orientations[scene.rod], {0.f, 0.5f, 0.f});
        worstPivot = std::max(worstPivot, (top - scene.pivot).length());
        const vec3 axis = rotate(scene.bodies.orientations[scene.rod], {0.f, 0.f, 1.f});
        worstAxis = std::max(worstAxis, std::acos(std::min(axis.z, 1.f)));
        peakEnergy = std::max(peakEnergy, mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod}));
    }
    const f32 overshootMax = maxAngle - 0.6f;
    const f32 overshootMin = -0.4f - minAngle;
    std::printf("hinge limits [-0.4, 0.6] rad: reached [%.4f, %.4f] rad (overshoot %.3f / %.3f deg), %u limit hits, "
                "pivot drift %.5f m, axis tilt %.4f deg, angle readout error %.2e rad, energy gain %.5f J "
                "(start %.3f J, end %.3f J)\n",
                minAngle, maxAngle, degrees(overshootMin), degrees(overshootMax), limitHits, worstPivot,
                degrees(worstAxis), worstReported, peakEnergy - startEnergy, startEnergy,
                mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod}));
    expectTrue(maxAngle > 0.59f && minAngle < -0.39f, "hinge swings into both limits");
    expectTrue(overshootMax < 0.5f * kPi / 180.f && overshootMin < 0.5f * kPi / 180.f,
               "hinge stops at its limits within 0.5 deg");
    expectTrue(limitHits >= 2u, "hinge hits its limits repeatedly");
    expectTrue(worstPivot < 2e-3f, "limited hinge keeps its pivot");
    expectTrue(worstAxis < 0.01f, "limited hinge keeps its axis");
    expectTrue(worstReported < 1e-3f, "jointHingeAngle matches the measured angle");
    expectTrue(peakEnergy - startEnergy < 1e-3f * std::fabs(startEnergy) + 1e-4f, "limited hinge never gains energy");
}

void testSwingCone() {
    HangingRod scene;
    buildHangingRod(scene);
    JointConstraint ball = rodJoint(scene, JointType::BallSocket);
    ball.swingLimit = 0.5f;
    // Spinning load: 20 rad/s about its own axis, kicked sideways into an orbit that would swing
    // far outside the cone.
    spinRod(scene, {0.f, 20.f, 0.f});
    scene.bodies.angularVelocities[scene.rod] += vec3{3.f, 0.f, 4.f};
    scene.bodies.linearVelocities[scene.rod] = vec3{3.f, 0.f, 4.f}.cross(scene.bodies.positions[scene.rod] - scene.pivot);
    PBDSolver solver;
    solver.init(4, 16, 4);
    solver.setJoints({ball});
    const SolverParams params = jointParams();
    const f32 startEnergy = mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod});
    f32 peakEnergy = startEnergy;
    f32 maxSwing = 0.f;
    f32 worstPivot = 0.f;
    f32 minSpin = 1e9f;
    for (int frame = 1; frame <= 300; ++frame) {
        solver.step(scene.bodies, scene.shapes, params, kDt);
        const vec3 down = rotate(scene.bodies.orientations[scene.rod], {0.f, -1.f, 0.f});
        maxSwing = std::max(maxSwing, std::acos(std::clamp(-down.y, -1.f, 1.f)));
        const vec3 top = scene.bodies.positions[scene.rod] + rotate(scene.bodies.orientations[scene.rod], {0.f, 0.5f, 0.f});
        worstPivot = std::max(worstPivot, (top - scene.pivot).length());
        minSpin = std::min(minSpin, std::fabs(scene.bodies.angularVelocities[scene.rod].dot(down)));
        peakEnergy = std::max(peakEnergy, mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod}));
    }
    std::printf("swing cone 0.5 rad under a 20 rad/s spinning load: max swing %.4f rad (overshoot %.3f deg), pivot "
                "drift %.5f m, spin kept >= %.2f rad/s, energy gain %.5f J\n",
                maxSwing, degrees(maxSwing - 0.5f), worstPivot, minSpin, peakEnergy - startEnergy);
    expectTrue(maxSwing > 0.49f, "spinning load reaches the cone");
    expectTrue(maxSwing - 0.5f < 0.5f * kPi / 180.f, "swing cone respected within 0.5 deg");
    expectTrue(worstPivot < 2e-3f, "coned ball-socket keeps its pivot");
    expectTrue(minSpin > 10.f, "the cone does not stop the load's own spin");
    expectTrue(peakEnergy - startEnergy < 1e-3f * std::fabs(startEnergy) + 1e-4f, "coned ball-socket never gains energy");
}

void testTwistLimit() {
    HangingRod scene;
    buildHangingRod(scene);
    JointConstraint ball = rodJoint(scene, JointType::BallSocket);
    ball.swingLimit = 0.3f;
    ball.twistLimit = true;
    ball.minTwist = -0.4f;
    ball.maxTwist = 0.4f;
    // 12 rad/s about the rod's own axis plus a sideways swing.
    spinRod(scene, {1.5f, 12.f, 0.f});
    PBDSolver solver;
    solver.init(4, 16, 4);
    solver.setJoints({ball});
    const SolverParams params = jointParams();
    const f32 startEnergy = mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod});
    f32 peakEnergy = startEnergy;
    f32 maxTwist = 0.f;
    f32 maxSwing = 0.f;
    f32 worstPivot = 0.f;
    for (int frame = 1; frame <= 300; ++frame) {
        solver.step(scene.bodies, scene.shapes, params, kDt);
        // Swing-twist decomposition of the rod's orientation about its twist axis (local Y).
        const quat q = scene.bodies.orientations[scene.rod];
        const f32 twist = 2.f * std::atan2(q.y, q.w);
        const f32 wrapped = std::atan2(std::sin(twist), std::cos(twist));
        maxTwist = std::max(maxTwist, std::fabs(wrapped));
        f32 swing = 0.f;
        f32 jointTwist = 0.f;
        jointSwingTwist(scene.bodies, ball, swing, jointTwist);
        maxSwing = std::max(maxSwing, swing);
        const vec3 top = scene.bodies.positions[scene.rod] + rotate(q, {0.f, 0.5f, 0.f});
        worstPivot = std::max(worstPivot, (top - scene.pivot).length());
        peakEnergy = std::max(peakEnergy, mechanicalEnergy(scene.bodies, scene.shapes, {scene.rod}));
    }
    std::printf("twist limit +-0.4 rad under a 12 rad/s spin: max twist %.4f rad (overshoot %.3f deg), max swing "
                "%.4f rad (cone 0.3), pivot drift %.5f m, energy gain %.5f J\n",
                maxTwist, degrees(maxTwist - 0.4f), maxSwing, worstPivot, peakEnergy - startEnergy);
    expectTrue(maxTwist > 0.39f, "spinning load reaches the twist limit");
    expectTrue(maxTwist - 0.4f < 1.f * kPi / 180.f, "twist limit respected within 1 deg");
    expectTrue(maxSwing - 0.3f < 0.5f * kPi / 180.f, "swing cone respected alongside the twist limit");
    expectTrue(worstPivot < 2e-3f, "twist-limited ball-socket keeps its pivot");
    expectTrue(peakEnergy - startEnergy < 1e-3f * std::fabs(startEnergy) + 1e-4f, "twist limit never gains energy");
}

// ---------------------------------------------------------------------------------------------
// PhysicsManager joint API (ECS entities)

using fuse::ecs::Collider;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;

PhysicsManagerDesc managerDesc() {
    PhysicsManagerDesc desc;
    desc.maxBodies = 256;
    desc.maxContacts = 1024;
    desc.maxConstraints = 256;
    desc.solver = jointParams();
    return desc;
}

EntityID spawnBox(Registry& reg, vec3 position, vec3 half, f32 mass, quat rotation = {}, bool isStatic = false) {
    const EntityID id = reg.create();
    Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    t.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.mass = mass;
    rb.is_static = isStatic;
    rb.restitution = 0.f;
    reg.add(id, rb);
    Collider c{};
    c.shape = Collider::Box;
    c.params = {half.x, half.y, half.z, 0.f};
    reg.add(id, c);
    return id;
}

EntityID spawnGroundPlane(Registry& reg) {
    const EntityID id = reg.create();
    reg.add(id, Transform{});
    fuse::ecs::RigidBody rb{};
    rb.is_static = true;
    rb.restitution = 0.f;
    reg.add(id, rb);
    Collider c{};
    c.shape = Collider::Plane;
    c.params = {0.f, 1.f, 0.f, 0.f};
    reg.add(id, c);
    return id;
}

vec3 entityPosition(Registry& reg, EntityID id) {
    const Transform* t = reg.get<Transform>(id);
    return {t->position.x, t->position.y, t->position.z};
}

void testManagerMatchesSolver() {
    // Two rods: rod 1 hinged to the world about z with limits, rod 2 hung below it on a coned,
    // twist-limited ball-socket; both kicked. Built once through the solver directly and once
    // through PhysicsManager::createJoint on ECS entities.
    const vec3 half{0.05f, 0.5f, 0.05f};
    const vec3 pivot{0.f, 4.f, 0.f};
    const vec3 c1 = pivot - vec3{0.f, 0.5f, 0.f};
    const vec3 c2 = pivot - vec3{0.f, 1.5f, 0.f};
    const vec3 elbow = pivot - vec3{0.f, 1.f, 0.f};
    const vec3 omega1{0.f, 0.f, 4.f};
    const vec3 v1 = omega1.cross(c1 - pivot);
    const vec3 omega2{2.f, 9.f, -1.f};
    const vec3 v2 = omega1.cross(elbow - pivot) + omega2.cross(c2 - elbow);
    constexpr int kFrames = 180;

    // Solver level.
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 rod1 = addBox(bodies, shapes, c1, half, {}, 1.f);
    const u32 rod2 = addBox(bodies, shapes, c2, half, {}, 1.f);
    bodies.linearVelocities[rod1] = v1;
    bodies.angularVelocities[rod1] = omega1;
    bodies.linearVelocities[rod2] = v2;
    bodies.angularVelocities[rod2] = omega2;
    JointConstraint hinge;
    hinge.type = JointType::Hinge;
    hinge.bodyA = rod1;
    hinge.bodyB = kWorld;
    hinge.localAnchorA = {0.f, 0.5f, 0.f};
    hinge.localAnchorB = pivot;
    hinge.localAxisA = hinge.localAxisB = {0.f, 0.f, 1.f};
    hinge.localNormalA = hinge.localNormalB = {1.f, 0.f, 0.f};
    hinge.hingeLimit = true;
    hinge.minAngle = -0.5f;
    hinge.maxAngle = 0.5f;
    JointConstraint ball;
    ball.type = JointType::BallSocket;
    ball.bodyA = rod1;
    ball.bodyB = rod2;
    ball.localAnchorA = {0.f, -0.5f, 0.f};
    ball.localAnchorB = {0.f, 0.5f, 0.f};
    ball.localAxisA = ball.localAxisB = {0.f, -1.f, 0.f};
    ball.localNormalA = ball.localNormalB = {1.f, 0.f, 0.f};
    ball.swingLimit = 0.4f;
    ball.twistLimit = true;
    ball.minTwist = -0.3f;
    ball.maxTwist = 0.3f;
    PBDSolver solver;
    solver.init(8, 64, 8);
    solver.setJoints({hinge, ball});
    const SolverParams params = jointParams();
    for (int frame = 0; frame < kFrames; ++frame) {
        solver.step(bodies, shapes, params, kDt);
    }

    // PhysicsManager on ECS entities.
    Registry reg;
    reg.init(64);
    const EntityID e1 = spawnBox(reg, c1, half, 1.f);
    const EntityID e2 = spawnBox(reg, c2, half, 1.f);
    reg.get<fuse::ecs::RigidBody>(e1)->velocity = {v1.x, v1.y, v1.z, 0.f};
    reg.get<fuse::ecs::RigidBody>(e1)->angular_velocity = {omega1.x, omega1.y, omega1.z, 0.f};
    reg.get<fuse::ecs::RigidBody>(e2)->velocity = {v2.x, v2.y, v2.z, 0.f};
    reg.get<fuse::ecs::RigidBody>(e2)->angular_velocity = {omega2.x, omega2.y, omega2.z, 0.f};
    PhysicsManager manager;
    manager.init(managerDesc());
    PhysicsStreamManager streams;
    JointDesc hingeDesc = JointDesc::hinge(e1, EntityID{}, pivot, {0.f, 0.f, 1.f});
    hingeDesc.normal = {1.f, 0.f, 0.f};
    hingeDesc.hingeLimit = true;
    hingeDesc.minAngle = -0.5f;
    hingeDesc.maxAngle = 0.5f;
    JointDesc ballDesc = JointDesc::ballSocket(e1, e2, elbow, {0.f, -1.f, 0.f});
    ballDesc.normal = {1.f, 0.f, 0.f};
    ballDesc.swingLimit = 0.4f;
    ballDesc.twistLimit = true;
    ballDesc.minTwist = -0.3f;
    ballDesc.maxTwist = 0.3f;
    const JointHandle h1 = manager.createJoint(reg, hingeDesc);
    const JointHandle h2 = manager.createJoint(reg, ballDesc);
    for (int frame = 0; frame < kFrames; ++frame) {
        manager.step(reg, kDt, streams);
    }
    const f32 diff1 = (entityPosition(reg, e1) - bodies.positions[rod1]).length();
    const f32 diff2 = (entityPosition(reg, e2) - bodies.positions[rod2]).length();
    const f32 travel = (bodies.positions[rod2] - c2).length();
    f32 hingeAngle = 0.f;
    f32 swing = 0.f;
    f32 twist = 0.f;
    const bool anglesOk = manager.jointAngles(h2, hingeAngle, swing, twist);
    std::printf("PhysicsManager joints vs solver after %d frames: position difference %.2e / %.2e m (rod 2 moved %.3f m), "
                "joint 2 swing %.3f twist %.3f rad, %u joints\n",
                kFrames, diff1, diff2, travel, swing, twist, manager.jointCount());
    expectTrue(h1.valid() && h2.valid() && manager.jointCount() == 2u, "PhysicsManager creates both joints");
    expectTrue(travel > 0.1f, "the jointed rods move");
    expectTrue(diff1 < 1e-4f && diff2 < 1e-4f, "PhysicsManager joints reproduce the solver-level trajectory");
    expectTrue(anglesOk && swing < 0.4f + 0.01f && std::fabs(twist) < 0.3f + 0.02f,
               "PhysicsManager reports joint angles within the limits");
}

void testManagerEntityDestroy() {
    Registry reg;
    reg.init(64);
    const vec3 half{0.1f, 0.25f, 0.1f};
    const vec3 pivot{0.f, 5.f, 0.f};
    // world - A - B hanging at rest, plus a separate pendulum C on the world.
    const EntityID a = spawnBox(reg, pivot - vec3{0.f, 0.25f, 0.f}, half, 1.f);
    const EntityID b = spawnBox(reg, pivot - vec3{0.f, 0.75f, 0.f}, half, 1.f);
    const EntityID c = spawnBox(reg, pivot + vec3{2.f, -0.25f, 0.f}, half, 1.f);
    PhysicsManager manager;
    PhysicsManagerDesc desc = managerDesc();
    desc.solver.linearDamping = 0.98f;
    desc.solver.angularDamping = 0.95f;
    manager.init(desc);
    PhysicsStreamManager streams;
    const JointHandle ja = manager.createJoint(reg, JointDesc::ballSocket(a, EntityID{}, pivot));
    const JointHandle jab = manager.createJoint(reg, JointDesc::ballSocket(a, b, pivot - vec3{0.f, 0.5f, 0.f}));
    const JointHandle jc = manager.createJoint(reg, JointDesc::hinge(c, EntityID{}, pivot + vec3{2.f, 0.f, 0.f},
                                                                     {0.f, 0.f, 1.f}));
    for (int frame = 0; frame < 120; ++frame) {
        manager.step(reg, kDt, streams);
    }
    const bool asleep = manager.isSleeping(a) && manager.isSleeping(b) && manager.isSleeping(c);
    const f32 restY = entityPosition(reg, b).y;

    // Destroy the middle entity: both of its joints go, B wakes and falls, C is untouched.
    reg.destroy_entity(a);
    manager.step(reg, kDt, streams);
    const bool jointsGone = !manager.isJointValid(ja) && !manager.isJointValid(jab) && manager.isJointValid(jc) &&
                            manager.jointCount() == 1u && manager.joint(ja) == nullptr;
    for (int frame = 0; frame < 30; ++frame) {
        manager.step(reg, kDt, streams);
    }
    const f32 fall = restY - entityPosition(reg, b).y;

    // destroyJoint on a sleeping pendulum wakes it immediately.
    const bool cAsleep = manager.isSleeping(c);
    const bool destroyed = manager.destroyJoint(jc);
    const bool wokeNow = !manager.isSleeping(c);
    const bool staleRejected = !manager.destroyJoint(jc) && !manager.setHingeLimits(jc, true, 0.f, 1.f);
    const f32 cStart = entityPosition(reg, c).y;
    for (int frame = 0; frame < 30; ++frame) {
        manager.step(reg, kDt, streams);
    }
    const f32 cFall = cStart - entityPosition(reg, c).y;
    // A recycled slot gets a new generation: the old handle stays stale.
    const JointHandle reused = manager.createJoint(reg, JointDesc::ballSocket(c, EntityID{}, entityPosition(reg, c)));
    std::printf("entity destroy: all asleep before %d, joints of the destroyed entity removed %d (%u left), survivor "
                "fell %.3f m in 0.5 s; destroyJoint woke a sleeper %d (asleep before %d), it fell %.3f m; stale "
                "handles rejected %d, slot reuse keeps old handle stale %d\n",
                asleep, jointsGone, manager.jointCount(), fall, wokeNow, cAsleep, cFall, staleRejected,
                reused.valid() && !manager.isJointValid(jc));
    expectTrue(asleep, "hanging jointed bodies fall asleep");
    expectTrue(jointsGone, "destroying an entity removes exactly its joints");
    expectTrue(fall > 0.5f, "the body jointed to a destroyed entity wakes and falls");
    expectTrue(destroyed && cAsleep && wokeNow, "destroyJoint wakes a sleeping body");
    expectTrue(cFall > 0.5f, "a body released by destroyJoint falls");
    expectTrue(staleRejected, "stale joint handles are rejected");
    expectTrue(reused.valid() && reused.index == jc.index && !manager.isJointValid(jc) && manager.isJointValid(reused),
               "a recycled joint slot invalidates the old handle");
}

void testManagerBreakThreshold() {
    Registry reg;
    reg.init(64);
    const vec3 half{0.1f, 0.25f, 0.1f};
    const vec3 pivot{0.f, 5.f, 0.f};
    const EntityID hanging = spawnBox(reg, pivot - vec3{0.f, 0.25f, 0.f}, half, 1.f);
    // A plank hinged about a vertical axis at one end: gravity loads the hinge's axis alignment
    // with a torque m g d.
    const vec3 plankHalf{0.5f, 0.05f, 0.2f};
    const vec3 hingePoint{3.f, 5.f, 0.f};
    const EntityID plank = spawnBox(reg, hingePoint + vec3{0.5f, 0.f, 0.f}, plankHalf, 1.f);
    PhysicsManager manager;
    manager.init(managerDesc());
    PhysicsStreamManager streams;
    JointDesc ropeDesc = JointDesc::ballSocket(hanging, EntityID{}, pivot);
    ropeDesc.breakForce = 30.f;
    const JointHandle force = manager.createJoint(reg, ropeDesc);
    JointDesc hingeDesc = JointDesc::hinge(plank, EntityID{}, hingePoint, {0.f, 1.f, 0.f});
    hingeDesc.breakTorque = 10.f;
    const JointHandle torque = manager.createJoint(reg, hingeDesc);

    u32 callbackBreaks = 0;
    u32 eventBreaks = 0;
    u32 wrongHandle = 0;
    const auto onEvent = [&](const CollisionEvent& event) {
        if (event.type == CollisionEventType::JointBreak) {
            ++callbackBreaks;
        }
    };
    manager.collisionEvents().registerCallback(hanging, onEvent);
    manager.collisionEvents().registerCallback(plank, onEvent);
    const auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            manager.step(reg, kDt, streams);
            for (const CollisionEvent& event : manager.lastEvents()) {
                if (event.type != CollisionEventType::JointBreak) {
                    continue;
                }
                ++eventBreaks;
                const JointHandle h{event.jointIndex, event.jointGeneration};
                wrongHandle += (h == force && event.entityA == hanging) || (h == torque && event.entityA == plank) ? 0u : 1u;
            }
        }
    };
    run(20); // read the holding loads while the bodies are awake (sleeping joints are not solved)
    const f32 holdForce = manager.jointForce(force);
    const f32 holdTorque = manager.jointTorque(torque);
    run(40); // both fall asleep; the mass edit below must wake them
    const u32 breaksWhileHolding = eventBreaks;
    // Heavier loads: 5 kg (49 N > 30 N) and 4 kg (19.6 N m > 10 N m).
    reg.get<fuse::ecs::RigidBody>(hanging)->mass = 5.f;
    reg.get<fuse::ecs::RigidBody>(plank)->mass = 4.f;
    const f32 hangingY = entityPosition(reg, hanging).y;
    const f32 plankY = entityPosition(reg, plank).y;
    run(120);
    const f32 fall = hangingY - entityPosition(reg, hanging).y;
    const f32 plankFall = plankY - entityPosition(reg, plank).y;
    std::printf("break thresholds: holding force %.3f N (m g = %.3f), holding torque %.3f N m (m g d = %.3f); after "
                "overload %u JointBreak events (%u callbacks, %u mismatched), broken %d/%d, bodies fell %.2f / %.2f m\n",
                holdForce, kG, holdTorque, 0.5f * kG, eventBreaks, callbackBreaks, wrongHandle,
                manager.isJointBroken(force), manager.isJointBroken(torque), fall, plankFall);
    expectTrue(std::fabs(holdForce - kG) < 0.05f * kG, "joint force reads m g for a hanging body");
    expectTrue(std::fabs(holdTorque - 0.5f * kG) < 0.1f * kG, "hinge torque reads m g d for a cantilevered plank");
    expectTrue(breaksWhileHolding == 0u, "joints below their thresholds do not break");
    expectTrue(eventBreaks == 2u && callbackBreaks == 2u && wrongHandle == 0u,
               "each break threshold raises exactly one JointBreak event");
    expectTrue(manager.isJointBroken(force) && manager.isJointBroken(torque) && manager.isJointValid(force),
               "broken joints stay valid handles flagged broken");
    expectTrue(fall > 1.f && plankFall > 1.f, "broken joints release their bodies");
    expectTrue(manager.destroyJoint(force) && manager.jointCount() == 1u, "a broken joint can be destroyed");
}

void testManagerOtherJointTypes() {
    // Fixed weld, rigid distance rod, rope and damped spring, each hung from the world.
    Registry reg;
    reg.init(64);
    const vec3 half{0.1f, 0.1f, 0.1f};
    const EntityID welded = spawnBox(reg, {0.f, 5.f, 0.f}, half, 1.f, quatFromAxisAngle({0.f, 0.f, 1.f}, 0.3f));
    const EntityID rodEnd = spawnBox(reg, {3.f, 4.f, 0.f}, half, 1.f);
    const EntityID roped = spawnBox(reg, {6.f, 4.f, 0.f}, half, 1.f);
    const EntityID sprung = spawnBox(reg, {9.f, 4.f, 0.f}, half, 1.f);
    PhysicsManager manager;
    PhysicsManagerDesc desc = managerDesc();
    desc.solver.linearDamping = 0.98f;
    desc.solver.angularDamping = 0.95f;
    manager.init(desc);
    PhysicsStreamManager streams;
    // Weld at an off-centre point: gravity torques it, the weld must hold pose.
    manager.createJoint(reg, JointDesc::fixed(welded, EntityID{}, {0.3f, 5.f, 0.f}));
    // Rod 1 m long, kicked sideways.
    manager.createJoint(reg, JointDesc::distance(rodEnd, EntityID{}, {3.f, 4.f, 0.f}, {3.f, 5.f, 0.f}));
    reg.get<fuse::ecs::RigidBody>(rodEnd)->velocity = {2.f, 0.f, 0.f, 0.f};
    // Rope: 1 m slack now, taut at 2 m.
    manager.createJoint(reg, JointDesc::rope(roped, EntityID{}, {6.f, 4.f, 0.f}, {6.f, 5.f, 0.f}, 2.f));
    // Spring k = 200 N/m, damping 10 N s/m, rest length 1 m: hangs at 1 + m g / k.
    manager.createJoint(reg, JointDesc::spring(sprung, EntityID{}, {9.f, 4.f, 0.f}, {9.f, 5.f, 0.f}, 200.f, 10.f));
    f32 weldDrift = 0.f;
    f32 rodError = 0.f;
    f32 ropeMax = 0.f;
    f32 ropeMinEarly = 10.f;
    for (int frame = 1; frame <= 300; ++frame) {
        manager.step(reg, kDt, streams);
        const Transform* tw = reg.get<Transform>(welded);
        const quat qw{tw->rotation.x, tw->rotation.y, tw->rotation.z, tw->rotation.w};
        const quat start = quatFromAxisAngle({0.f, 0.f, 1.f}, 0.3f);
        const f32 angle = 2.f * std::acos(std::min(std::fabs(quatMul(qw, quatConjugate(start)).w), 1.f));
        weldDrift = std::max(weldDrift, (entityPosition(reg, welded) - vec3{0.f, 5.f, 0.f}).length() + angle);
        rodError = std::max(rodError, std::fabs((entityPosition(reg, rodEnd) - vec3{3.f, 5.f, 0.f}).length() - 1.f));
        const f32 rope = (entityPosition(reg, roped) - vec3{6.f, 5.f, 0.f}).length();
        ropeMax = std::max(ropeMax, rope);
        if (frame == 10) {
            ropeMinEarly = rope; // still falling freely inside the slack
        }
    }
    const f32 stretch = (entityPosition(reg, sprung) - vec3{9.f, 5.f, 0.f}).length();
    const f32 expectedStretch = 1.f + kG / 200.f;
    std::printf("other joints: weld drift %.2e (m + rad), rod length error %.5f m, rope length at 1/6 s %.3f m and max "
                "%.4f m (limit 2), spring length %.4f m (analytic %.4f m)\n",
                weldDrift, rodError, ropeMinEarly, ropeMax, stretch, expectedStretch);
    expectTrue(weldDrift < 2e-3f, "fixed joint holds its body's pose");
    expectTrue(rodError < 2e-3f, "distance joint holds its length");
    expectTrue(ropeMinEarly > 1.05f && ropeMinEarly < 1.95f, "rope is slack below its length");
    expectTrue(ropeMax < 2.f + 2e-3f && ropeMax > 1.99f, "rope goes taut at its length");
    expectTrue(std::fabs(stretch - expectedStretch) < 3e-3f, "spring settles at rest length + m g / k");
}

void testRagdollChain() {
    // Ten links (long axis x) laid out horizontally from a world pivot 2.5 m up, joined
    // alternately by coned + twist-limited ball-sockets and angle-limited hinges; it falls, drapes
    // over the ground and must settle without jitter, drift or limit violations for 10 s.
    constexpr u32 kLinks = 10u;
    const vec3 half{0.2f, 0.07f, 0.07f};
    const vec3 pivot{0.f, 2.5f, 0.f};
    constexpr f32 kCone = 0.6f;
    constexpr f32 kTwist = 0.3f;
    constexpr f32 kHinge = 0.9f;
    Registry reg;
    reg.init(64);
    spawnGroundPlane(reg);
    std::vector<EntityID> links;
    std::vector<JointHandle> joints;
    std::vector<JointType> types;
    PhysicsManager manager;
    PhysicsManagerDesc desc = managerDesc();
    desc.solver.linearDamping = 0.98f;
    desc.solver.angularDamping = 0.95f;
    manager.init(desc);
    PhysicsStreamManager streams;
    for (u32 i = 0; i < kLinks; ++i) {
        links.push_back(spawnBox(reg, pivot + vec3{half.x * (2.f * static_cast<f32>(i) + 1.f), 0.f, 0.f}, half, 1.f));
        const vec3 joint = pivot + vec3{2.f * half.x * static_cast<f32>(i), 0.f, 0.f};
        const EntityID parent = i == 0u ? EntityID{} : links[i - 1u];
        JointDesc d;
        if (i % 2u == 0u) {
            d = JointDesc::ballSocket(links[i], parent, joint, {1.f, 0.f, 0.f});
            d.normal = {0.f, 1.f, 0.f};
            d.swingLimit = kCone;
            d.twistLimit = true;
            d.minTwist = -kTwist;
            d.maxTwist = kTwist;
        } else {
            d = JointDesc::hinge(links[i], parent, joint, {0.f, 0.f, 1.f});
            d.normal = {1.f, 0.f, 0.f};
            d.hingeLimit = true;
            d.minAngle = -kHinge;
            d.maxAngle = kHinge;
        }
        joints.push_back(manager.createJoint(reg, d));
        types.push_back(d.type);
    }
    // A sideways kick on the free end.
    reg.get<fuse::ecs::RigidBody>(links.back())->velocity = {0.f, 0.f, 3.f, 0.f};

    const auto energy = [&]() {
        f32 e = 0.f;
        for (const EntityID id : links) {
            const fuse::ecs::RigidBody* rb = reg.get<fuse::ecs::RigidBody>(id);
            const vec3 v{rb->velocity.x, rb->velocity.y, rb->velocity.z};
            const vec3 w{rb->angular_velocity.x, rb->angular_velocity.y, rb->angular_velocity.z};
            const f32 inertia = (half.x * half.x + half.y * half.y) / 3.f; // loose upper bound, per axis
            e += 0.5f * v.dot(v) + 0.5f * inertia * w.dot(w) + kG * entityPosition(reg, id).y;
        }
        return e;
    };
    const f32 startEnergy = energy();
    f32 peakEnergy = startEnergy;
    f32 worstSeparation = 0.f;
    f32 worstLateSeparation = 0.f;
    f32 worstLimit = 0.f;
    f32 lowest = 10.f;
    bool finite = true;
    f32 lateSpeed = 0.f;
    for (int frame = 1; frame <= 600; ++frame) {
        manager.step(reg, kDt, streams);
        for (u32 i = 0; i < kLinks; ++i) {
            const JointConstraint* joint = manager.joint(joints[i]);
            const vec3 pa = entityPosition(reg, links[i]);
            const Transform* ta = reg.get<Transform>(links[i]);
            const quat qa{ta->rotation.x, ta->rotation.y, ta->rotation.z, ta->rotation.w};
            const vec3 anchorA = pa + rotate(qa, joint->localAnchorA);
            vec3 anchorB = joint->localAnchorB;
            if (i > 0u) {
                const Transform* tb = reg.get<Transform>(links[i - 1u]);
                const quat qb{tb->rotation.x, tb->rotation.y, tb->rotation.z, tb->rotation.w};
                anchorB = entityPosition(reg, links[i - 1u]) + rotate(qb, joint->localAnchorB);
            }
            const f32 separation = (anchorA - anchorB).length();
            worstSeparation = std::max(worstSeparation, separation);
            if (frame > 300) {
                worstLateSeparation = std::max(worstLateSeparation, separation);
            }
            f32 hingeAngle = 0.f;
            f32 swing = 0.f;
            f32 twist = 0.f;
            manager.jointAngles(joints[i], hingeAngle, swing, twist);
            const f32 violation = types[i] == JointType::Hinge
                                      ? std::max(std::fabs(hingeAngle) - kHinge, 0.f)
                                      : std::max(std::max(swing - kCone, std::fabs(twist) - kTwist), 0.f);
            worstLimit = std::max(worstLimit, violation);
            finite = finite && std::isfinite(pa.x) && std::isfinite(pa.y) && std::isfinite(pa.z);
            lowest = std::min(lowest, pa.y);
            if (frame > 540) {
                const fuse::ecs::RigidBody* rb = reg.get<fuse::ecs::RigidBody>(links[i]);
                const vec3 v{rb->velocity.x, rb->velocity.y, rb->velocity.z};
                lateSpeed = std::max(lateSpeed, v.length());
            }
        }
        peakEnergy = std::max(peakEnergy, energy());
    }
    u32 asleep = 0;
    for (const EntityID id : links) {
        asleep += manager.isSleeping(id) ? 1u : 0u;
    }
    std::printf("ragdoll chain (10 limited joints, 10 s): worst joint separation %.4f m (last 5 s %.4f m), worst limit "
                "violation %.3f deg, lowest link y %.3f, speed in the last second <= %.4f m/s, %u/%u links asleep, "
                "energy gain %.4f J\n",
                worstSeparation, worstLateSeparation, degrees(worstLimit), lowest, lateSpeed, asleep, kLinks,
                peakEnergy - startEnergy);
    expectTrue(finite, "ragdoll chain stays finite");
    expectTrue(worstSeparation < 0.02f && worstLateSeparation < 0.005f, "ragdoll joints stay together");
    expectTrue(worstLimit < 2.f * kPi / 180.f, "ragdoll joint limits hold within 2 deg");
    expectTrue(lowest > half.y - 0.02f && lowest < 0.5f, "ragdoll drapes onto the ground without sinking");
    expectTrue(lateSpeed < 0.05f, "ragdoll chain comes to rest (no jitter)");
    expectTrue(peakEnergy - startEnergy < 0.01f * std::fabs(startEnergy) + 0.05f, "ragdoll chain never gains energy");
}

} // namespace

int main() {
    fuse::core::initialize();
    testPointMassPendulum();
    testRodPendulum(0.1f, true);
    testRodPendulum(1.2f, false);
    testChainUnderLoad();
    testHinge();
    testHingeLimits();
    testSwingCone();
    testTwistLimit();
    testManagerMatchesSolver();
    testManagerEntityDestroy();
    testManagerBreakThreshold();
    testManagerOtherJointTypes();
    testRagdollChain();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all joint gates passed\n");
    return EXIT_SUCCESS;
}
