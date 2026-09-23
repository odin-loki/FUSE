// B4 joint gate rows (anchored distance constraints with rotational terms):
//  - a point-mass pendulum swings with the analytic small-angle period 2 pi sqrt(L / g)
//  - a rod pinned off-centre (ball-socket at its end) swings with the physical pendulum period
//    2 pi sqrt(I_pivot / (m g d)) and keeps its pivot
//  - a chain of 10 linked boxes carrying a heavy load holds every link length to +-0.01 m
//  - pendulums and the chain never gain energy (the position solve dissipates like implicit Euler:
//    a 1 m pendulum at 4 substeps / 60 Hz keeps ~2/3 of its swing energy after 10 s)
//  - a hinge (two ball-sockets on the axis) turns about its axis only
#include <fuse/core/init.hpp>
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

} // namespace

int main() {
    fuse::core::initialize();
    testPointMassPendulum();
    testRodPendulum(0.1f, true);
    testRodPendulum(1.2f, false);
    testChainUnderLoad();
    testHinge();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all joint gates passed\n");
    return EXIT_SUCCESS;
}
