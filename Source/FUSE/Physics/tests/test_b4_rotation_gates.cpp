// B4 rigid-body rotation gate rows (follow-up to the B4.11 solver gates):
//  - an off-centre impulse changes angular velocity by exactly I_world^-1 (r x J)
//  - a box dropped on an edge / corner tips over and comes to rest flat on a face
//  - a box on an incline rests when mu_s > tan(theta) and slides at g (sin - mu cos) otherwise
//  - a torque-free spinning sphere keeps its angular velocity (angular momentum) with no damping
//  - a torque-free tumbling box never gains kinetic energy and keeps its angular momentum
//  - a stack of five yawed (oriented) boxes stays stable
//  - a tilted capsule falls over and rests on its side; a spinning box sleeps only once it stops
//  - oriented box narrowphase: OBB-OBB face manifolds clip to four points, OBB-plane corners
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/physics/rotation.hpp>
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
constexpr f32 kPi = 3.14159265f;

/// Row-major 3x3 matrix, built independently of the solver's quaternion helpers.
struct Mat3 {
    f32 m[3][3]{};

    vec3 operator*(vec3 v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};

Mat3 rotationMatrix(const quat& q) {
    const f32 x = q.x, y = q.y, z = q.z, w = q.w;
    Mat3 r{};
    r.m[0][0] = 1.f - 2.f * (y * y + z * z);
    r.m[0][1] = 2.f * (x * y - z * w);
    r.m[0][2] = 2.f * (x * z + y * w);
    r.m[1][0] = 2.f * (x * y + z * w);
    r.m[1][1] = 1.f - 2.f * (x * x + z * z);
    r.m[1][2] = 2.f * (y * z - x * w);
    r.m[2][0] = 2.f * (x * z - y * w);
    r.m[2][1] = 2.f * (y * z + x * w);
    r.m[2][2] = 1.f - 2.f * (x * x + y * y);
    return r;
}

/// World inertia tensor R diag(I) R^T.
Mat3 worldInertia(const quat& q, vec3 bodyInertia) {
    const Mat3 r = rotationMatrix(q);
    const f32 d[3] = {bodyInertia.x, bodyInertia.y, bodyInertia.z};
    Mat3 out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                out.m[i][j] += r.m[i][k] * d[k] * r.m[j][k];
            }
        }
    }
    return out;
}

/// Solves M x = b by Cramer's rule.
vec3 solve(const Mat3& a, vec3 b) {
    const vec3 c0{a.m[0][0], a.m[1][0], a.m[2][0]};
    const vec3 c1{a.m[0][1], a.m[1][1], a.m[2][1]};
    const vec3 c2{a.m[0][2], a.m[1][2], a.m[2][2]};
    const f32 det = c0.dot(c1.cross(c2));
    return {b.dot(c1.cross(c2)) / det, c0.dot(b.cross(c2)) / det, c0.dot(c1.cross(b)) / det};
}

vec3 boxInertia(f32 mass, vec3 h) {
    return {mass * (h.y * h.y + h.z * h.z) / 3.f, mass * (h.x * h.x + h.z * h.z) / 3.f,
            mass * (h.x * h.x + h.y * h.y) / 3.f};
}

f32 relativeError(vec3 got, vec3 expected) {
    return (got - expected).length() / std::max(expected.length(), 1e-12f);
}

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

u32 addPlane(RigidBodySoA& bodies, CollisionShapeSoA& shapes, vec3 normal = {0.f, 1.f, 0.f}) {
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Plane, ground, normal, 0.f);
    return ground;
}

u32 addBox(RigidBodySoA& bodies, CollisionShapeSoA& shapes, vec3 position, vec3 halfExtents, quat orientation = {},
           f32 invMass = 1.f) {
    const u32 body = bodies.addBody(position, invMass);
    shapes.addShape(CollisionShapeType::Box, body, halfExtents);
    bodies.orientations[body] = orientation;
    bodies.predictedOrientations[body] = orientation;
    return body;
}

void setFriction(RigidBodySoA& bodies, u32 body, f32 mu) {
    bodies.frictionStatic[body] = mu;
    bodies.frictionDynamic[body] = mu;
}

/// Largest |local axis . world up|: 1 when a box face lies flat.
f32 flatness(const quat& q) {
    return std::max({std::fabs(rotate(q, {1.f, 0.f, 0.f}).y), std::fabs(rotate(q, {0.f, 1.f, 0.f}).y),
                     std::fabs(rotate(q, {0.f, 0.f, 1.f}).y)});
}

void testOffCentreImpulse() {
    using fuse::ecs::Collider;
    fuse::ecs::Registry reg;
    reg.init(8);
    const fuse::ecs::EntityID box = reg.create();
    const quat orientation = quatFromAxisAngle({1.f, 2.f, 0.5f}, 0.7f);
    fuse::ecs::Transform transform{};
    transform.position = {1.f, 3.f, -2.f, 1.f};
    transform.rotation = {orientation.x, orientation.y, orientation.z, orientation.w};
    reg.add(box, transform);
    fuse::ecs::RigidBody rb{};
    rb.mass = 2.f;
    reg.add(box, rb);
    Collider collider{};
    collider.shape = Collider::Box;
    const vec3 half{0.5f, 0.25f, 1.f};
    collider.params = {half.x, half.y, half.z, 0.f};
    reg.add(box, collider);

    PhysicsManagerDesc desc{};
    desc.solver.gravity = {};
    desc.solver.linearDamping = 1.f;
    desc.solver.angularDamping = 1.f;
    PhysicsManager manager;
    manager.init(desc);
    PhysicsStreamManager streams{};
    manager.step(reg, kDt, streams); // registers the body

    const u32 body = manager.bodyIndex(box);
    const vec3 centre = manager.bodies().positions[body];
    const quat q = manager.bodies().orientations[body];
    const vec3 impulse{3.f, -1.f, 2.f};
    const vec3 point = centre + vec3{0.4f, 0.2f, -0.9f};
    const vec3 v0 = manager.bodies().linearVelocities[body];
    const vec3 w0 = manager.bodies().angularVelocities[body];
    manager.applyImpulse(box, impulse, point);
    const vec3 dv = manager.bodies().linearVelocities[body] - v0;
    const vec3 dw = manager.bodies().angularVelocities[body] - w0;

    const vec3 expectedDv = impulse * 0.5f;
    const vec3 expectedDw = solve(worldInertia(q, boxInertia(2.f, half)), (point - centre).cross(impulse));
    const f32 dvError = relativeError(dv, expectedDv);
    const f32 dwError = relativeError(dw, expectedDw);
    std::printf("off-centre impulse: dv err %.2e, dw = (%.4f, %.4f, %.4f) expected (%.4f, %.4f, %.4f), err %.2e\n",
                dvError, dw.x, dw.y, dw.z, expectedDw.x, expectedDw.y, expectedDw.z, dwError);
    expectTrue(dvError < 1e-5f, "off-centre impulse: dv = J / m");
    expectTrue(dwError < 1e-4f, "off-centre impulse: dw = I_world^-1 (r x J)");

    // Through the centre: no spin.
    const vec3 before = manager.bodies().angularVelocities[body];
    manager.applyImpulse(box, impulse);
    expectTrue((manager.bodies().angularVelocities[body] - before).length() == 0.f,
               "centre impulse adds no angular velocity");

    // The ECS components carry the spin and the turning pose after a step.
    const quat qBefore = manager.bodies().orientations[body];
    const vec3 spin = manager.bodies().angularVelocities[body];
    manager.step(reg, kDt, streams);
    const fuse::ecs::RigidBody* after = reg.get<fuse::ecs::RigidBody>(box);
    const fuse::ecs::Transform* pose = reg.get<fuse::ecs::Transform>(box);
    const vec3 ecsSpin{after->angular_velocity.x, after->angular_velocity.y, after->angular_velocity.z};
    const quat ecsPose{pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w};
    const f32 turned = angularVelocityBetween(qBefore, ecsPose, kDt).length();
    std::printf("off-centre impulse: |w| %.4f -> ECS %.4f rad/s, pose turned at %.4f rad/s\n", spin.length(),
                ecsSpin.length(), turned);
    expectTrue(std::fabs(ecsSpin.length() - spin.length()) < 0.02f * spin.length(),
               "RigidBody.angular_velocity reflects the spin after a step");
    expectTrue(std::fabs(turned - ecsSpin.length()) < 0.02f * spin.length(),
               "Transform.rotation turns at the body's angular speed");
}

void testKinematicTargetOrientation() {
    using fuse::ecs::Collider;
    fuse::ecs::Registry reg;
    reg.init(8);
    const fuse::ecs::EntityID paddle = reg.create();
    reg.add(paddle, fuse::ecs::Transform{});
    reg.add(paddle, fuse::ecs::RigidBody{});
    Collider collider{};
    collider.shape = Collider::Box;
    collider.params = {1.f, 0.1f, 0.5f, 0.f};
    reg.add(paddle, collider);
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    manager.step(reg, kDt, streams);
    const quat target = quatFromAxisAngle({0.f, 1.f, 0.f}, 0.6f);
    manager.setKinematicTarget(paddle, {0.f, 2.f, 0.f}, target);
    manager.step(reg, kDt, streams);
    const fuse::ecs::Transform* pose = reg.get<fuse::ecs::Transform>(paddle);
    const quat reached{pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w};
    const f32 error = angularVelocityBetween(target, reached, 1.f).length();
    const fuse::ecs::RigidBody* rb = reg.get<fuse::ecs::RigidBody>(paddle);
    std::printf("kinematic target: orientation error %.2e rad, angular velocity %.3f rad/s (expect %.3f)\n", error,
                rb->angular_velocity.y, 0.6f / kDt);
    expectTrue(error < 1e-4f && std::fabs(pose->position.y - 2.f) < 1e-5f,
               "setKinematicTarget reaches the target pose (position and orientation) this step");
    expectTrue(std::fabs(rb->angular_velocity.y - 0.6f / kDt) < 1e-2f * 0.6f / kDt,
               "the kinematic body sweeps to the target orientation at the matching angular velocity");
}

void testBoxTipsOverAndRestsFlat() {
    struct Drop {
        const char* name;
        quat orientation;
    };
    // Off-balance edge (40 deg about z) and corner (tilted about a skew axis) landings.
    const Drop drops[] = {
        {"edge", quatFromAxisAngle({0.f, 0.f, 1.f}, 40.f * kPi / 180.f)},
        {"corner", quatMul(quatFromAxisAngle({1.f, 0.f, 0.f}, 35.f * kPi / 180.f),
                           quatFromAxisAngle({0.f, 0.f, 1.f}, 40.f * kPi / 180.f))},
    };
    for (const Drop& drop : drops) {
        RigidBodySoA bodies;
        CollisionShapeSoA shapes;
        const u32 box = addBox(bodies, shapes, {0.f, 1.5f, 0.f}, {0.5f, 0.5f, 0.5f}, drop.orientation);
        const u32 ground = addPlane(bodies, shapes);
        setFriction(bodies, box, 0.6f);
        setFriction(bodies, ground, 0.6f);
        bodies.restitutions[box] = 0.1f;
        PBDSolver solver;
        solver.init(2, 8, 0);
        const SolverParams params = undampedParams();
        f32 peakSpeed = 0.f;
        for (int frame = 0; frame < 300; ++frame) { // 5 s
            solver.step(bodies, shapes, params, kDt);
            peakSpeed = std::max(peakSpeed, bodies.linearVelocities[box].length());
        }
        const f32 flat = flatness(bodies.orientations[box]);
        const f32 tiltDeg = std::acos(std::min(flat, 1.f)) * 180.f / kPi;
        const f32 height = bodies.positions[box].y;
        std::printf("tip-over (%s): face tilt %.3f deg, rest height %.4f (expect 0.5), |v| %.5f |w| %.5f, asleep %d\n",
                    drop.name, tiltDeg, height, bodies.linearVelocities[box].length(),
                    bodies.angularVelocities[box].length(), (bodies.flags[box] & RB_SLEEPING) != 0u ? 1 : 0);
        expectTrue(tiltDeg < 1.f, "dropped box tips over onto a face (orientation aligned within 1 deg)");
        expectTrue(std::fabs(height - 0.5f) < 0.01f, "box rests at its half extent above the ground");
        expectTrue(bodies.linearVelocities[box].length() < 0.02f && bodies.angularVelocities[box].length() < 0.05f,
                   "tipped box comes to rest");
        expectTrue(peakSpeed < 8.f, "no explosive velocity while tipping");
    }
}

void testBoxOnIncline() {
    const f32 theta = 20.f * kPi / 180.f;
    const vec3 normal{std::sin(theta), std::cos(theta), 0.f};    // surface descends towards +x
    const vec3 downhill{std::cos(theta), -std::sin(theta), 0.f}; // unit, along the surface
    const quat onSlope = quatFromAxisAngle({0.f, 0.f, 1.f}, -theta);
    struct Case {
        f32 mu;
        bool slides;
    };
    const Case cases[] = {{0.8f, false}, {0.15f, true}};
    for (const Case& c : cases) {
        RigidBodySoA bodies;
        CollisionShapeSoA shapes;
        const u32 box = addBox(bodies, shapes, normal * 0.5f, {0.5f, 0.5f, 0.5f}, onSlope);
        const u32 slope = addPlane(bodies, shapes, normal);
        setFriction(bodies, box, c.mu);
        setFriction(bodies, slope, c.mu);
        bodies.restitutions[box] = 0.f;
        PBDSolver solver;
        solver.init(2, 8, 0);
        SolverParams params = undampedParams();
        params.sleepTimeRequired = 1e9f;
        const vec3 start = bodies.positions[box];
        const f32 time = 1.5f;
        const int frames = static_cast<int>(time / kDt + 0.5f);
        f32 worstTilt = 0.f;
        for (int frame = 0; frame < frames; ++frame) {
            solver.step(bodies, shapes, params, kDt);
            const f32 align = rotate(bodies.orientations[box], {0.f, 1.f, 0.f}).dot(normal);
            worstTilt = std::max(worstTilt, std::acos(std::min(align, 1.f)) * 180.f / kPi);
        }
        const f32 travelled = (bodies.positions[box] - start).dot(downhill);
        const f32 offSurface = std::fabs(bodies.positions[box].dot(normal) - 0.5f);
        if (c.slides) {
            const f32 accel = kG * (std::sin(theta) - c.mu * std::cos(theta));
            const f32 expected = 0.5f * accel * time * time;
            std::printf("incline mu %.2f < tan %.3f: slid %.4f m (analytic %.4f m), tilt %.3f deg, off-surface %.4f\n",
                        c.mu, std::tan(theta), travelled, expected, worstTilt, offSurface);
            expectTrue(std::fabs(travelled - expected) < 0.05f * expected,
                       "box slides down the incline at g (sin - mu cos) within 5%");
        } else {
            std::printf("incline mu %.2f > tan %.3f: moved %.5f m, tilt %.3f deg, off-surface %.4f\n", c.mu,
                        std::tan(theta), travelled, worstTilt, offSurface);
            expectTrue(std::fabs(travelled) < 0.01f, "static friction holds the box on the incline");
        }
        expectTrue(worstTilt < 1.f, "box stays flat on the incline (no spurious tumbling)");
        expectTrue(offSurface < 0.01f, "box stays on the incline surface");
    }
}

void testSpinningSphereConservesMomentum() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 sphere = bodies.addBody({0.f, 5.f, 0.f}, 0.5f, RB_NO_GRAVITY);
    shapes.addShape(CollisionShapeType::Sphere, sphere, {0.5f, 0.f, 0.f});
    const vec3 omega{1.5f, 12.f, -4.f};
    bodies.angularVelocities[sphere] = omega;
    PBDSolver solver;
    solver.init(1, 4, 0);
    SolverParams params = undampedParams();
    params.gravity = {};
    const quat start = bodies.orientations[sphere];
    for (int frame = 0; frame < 120; ++frame) {
        solver.step(bodies, shapes, params, kDt);
    }
    const f32 error = relativeError(bodies.angularVelocities[sphere], omega);
    // After 2 s the pose has turned by exactly omega * 2 s about the fixed axis.
    const quat expectedPose = applyRotationVector(start, omega * 2.f);
    const f32 poseError = angularVelocityBetween(expectedPose, bodies.orientations[sphere], 1.f).length();
    std::printf("spinning sphere: |w| %.5f -> %.5f (rel err %.2e), pose error %.2e rad after 2 s\n", omega.length(),
                bodies.angularVelocities[sphere].length(), error, poseError);
    expectTrue(error < 1e-4f, "torque-free sphere keeps its angular velocity (angular momentum) undamped");
    expectTrue(poseError < 1e-3f, "sphere orientation advances by w t");
    expectTrue((bodies.flags[sphere] & RB_SLEEPING) == 0u, "a spinning body never sleeps");
}

void testTumblingBoxEnergy() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const vec3 half{0.2f, 0.5f, 1.f};
    const u32 box = addBox(bodies, shapes, {0.f, 5.f, 0.f}, half, quatFromAxisAngle({1.f, 1.f, 0.f}, 0.4f));
    bodies.flags[box] |= RB_NO_GRAVITY;
    bodies.angularVelocities[box] = {0.4f, 6.f, 0.3f}; // near the intermediate axis: tumbles
    const vec3 inertia = boxInertia(1.f, half);
    const auto energy = [&]() {
        const vec3 w = inverseRotate(bodies.orientations[box], bodies.angularVelocities[box]);
        return 0.5f * (inertia.x * w.x * w.x + inertia.y * w.y * w.y + inertia.z * w.z * w.z);
    };
    const auto momentum = [&]() {
        return worldInertia(bodies.orientations[box], inertia) * bodies.angularVelocities[box];
    };
    PBDSolver solver;
    solver.init(1, 4, 0);
    SolverParams params = undampedParams();
    params.gravity = {};
    const f32 e0 = energy();
    const vec3 l0 = momentum();
    f32 peak = e0;
    f32 worstMomentum = 0.f;
    for (int frame = 0; frame < 300; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        peak = std::max(peak, energy());
        worstMomentum = std::max(worstMomentum, relativeError(momentum(), l0));
    }
    std::printf("tumbling box: energy %.5f -> %.5f (peak %.5f), worst |L - L0| / |L0| %.4f over 5 s\n", e0, energy(),
                peak, worstMomentum);
    expectTrue(peak <= e0 * 1.0001f, "torque-free kinetic energy never increases");
    expectTrue(energy() > 0.9f * e0, "torque-free tumbling is not over-damped");
    expectTrue(worstMomentum < 0.1f, "torque-free tumbling keeps its world angular momentum");
}

void testOrientedBoxStack() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 ground = addPlane(bodies, shapes);
    setFriction(bodies, ground, 0.6f);
    u32 first = 0;
    for (u32 i = 0; i < 5u; ++i) {
        const quat yaw = quatFromAxisAngle({0.f, 1.f, 0.f}, 0.3f * static_cast<f32>(i));
        const u32 box = addBox(bodies, shapes, {0.f, 0.5f + static_cast<f32>(i), 0.f}, {0.5f, 0.5f, 0.5f}, yaw);
        setFriction(bodies, box, 0.6f);
        bodies.restitutions[box] = 0.f;
        first = i == 0u ? box : first;
    }
    PBDSolver solver;
    solver.init(8, 64, 0);
    SolverParams params = undampedParams();
    f32 peakSpeed = 0.f;
    for (int frame = 0; frame < 300; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        for (u32 i = 0; i < 5u; ++i) {
            peakSpeed = std::max(peakSpeed, bodies.linearVelocities[first + i].length());
        }
    }
    f32 worstLateral = 0.f;
    f32 worstVertical = 0.f;
    f32 worstTilt = 0.f;
    f32 worstYawDrift = 0.f;
    for (u32 i = 0; i < 5u; ++i) {
        const vec3 p = bodies.positions[first + i];
        const quat q = bodies.orientations[first + i];
        worstLateral = std::max(worstLateral, std::sqrt(p.x * p.x + p.z * p.z));
        worstVertical = std::max(worstVertical, std::fabs(p.y - (0.5f + static_cast<f32>(i))));
        worstTilt = std::max(worstTilt, std::acos(std::min(rotate(q, {0.f, 1.f, 0.f}).y, 1.f)) * 180.f / kPi);
        const vec3 xAxis = rotate(q, {1.f, 0.f, 0.f});
        const f32 yaw = std::atan2(-xAxis.z, xAxis.x);
        worstYawDrift = std::max(worstYawDrift, std::fabs(yaw - 0.3f * static_cast<f32>(i)));
    }
    std::printf("oriented stack of 5: lateral %.5f m, vertical %.4f m, tilt %.4f deg, yaw drift %.4f rad, peak "
                "speed %.4f m/s\n",
                worstLateral, worstVertical, worstTilt, worstYawDrift, peakSpeed);
    bool asleep = true;
    for (u32 i = 0; i < 5u; ++i) {
        asleep = asleep && (bodies.flags[first + i] & RB_SLEEPING) != 0u;
    }
    expectTrue(worstLateral < 0.005f, "yawed box stack does not drift sideways");
    expectTrue(worstVertical < 0.005f, "yawed box stack holds its rest heights");
    expectTrue(worstTilt < 0.5f, "stacked boxes stay upright");
    expectTrue(worstYawDrift < 0.01f, "stacked boxes keep their yaw (no spurious spin)");
    expectTrue(peakSpeed < 0.05f, "resting stack does not pop or jitter");
    expectTrue(asleep, "the stack settles and sleeps");
}

void testCapsuleFallsOnItsSide() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 capsule = bodies.addBody({0.f, 1.3f, 0.f}, 1.f);
    shapes.addShape(CollisionShapeType::Capsule, capsule, {0.25f, 0.75f, 0.f});
    bodies.orientations[capsule] = quatFromAxisAngle({0.f, 0.f, 1.f}, 0.15f);
    bodies.restitutions[capsule] = 0.f;
    const u32 ground = addPlane(bodies, shapes);
    setFriction(bodies, capsule, 0.6f);
    setFriction(bodies, ground, 0.6f);
    PBDSolver solver;
    solver.init(2, 8, 0);
    SolverParams params = undampedParams();
    params.angularDamping = 0.98f; // rolling on its round side needs some drag to settle
    for (int frame = 0; frame < 360; ++frame) {
        solver.step(bodies, shapes, params, kDt);
    }
    const f32 axisUp = std::fabs(rotate(bodies.orientations[capsule], {0.f, 1.f, 0.f}).y);
    std::printf("capsule: axis . up %.4f (0 = lying), rest height %.4f (expect radius 0.25)\n", axisUp,
                bodies.positions[capsule].y);
    expectTrue(axisUp < 0.02f, "tilted capsule falls over and lies on its side");
    expectTrue(std::fabs(bodies.positions[capsule].y - 0.25f) < 0.01f, "capsule rests at its radius");
}

void testSpinningBoxSleepsOnlyWhenStopped() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 box = addBox(bodies, shapes, {0.f, 0.5f, 0.f}, {0.5f, 0.5f, 0.5f});
    const u32 ground = addPlane(bodies, shapes);
    setFriction(bodies, box, 0.3f);
    setFriction(bodies, ground, 0.3f);
    bodies.restitutions[box] = 0.f;
    bodies.angularVelocities[box] = {0.f, 6.f, 0.f}; // spins in place about the vertical axis
    PBDSolver solver;
    solver.init(2, 8, 0);
    SolverParams params = undampedParams();
    params.sleepTimeRequired = 0.5f;
    int framesToStop = -1;
    bool sleptWhileSpinning = false;
    for (int frame = 1; frame <= 240; ++frame) {
        solver.step(bodies, shapes, params, kDt);
        const f32 spin = bodies.angularVelocities[box].length();
        if ((bodies.flags[box] & RB_SLEEPING) == 0u && spin > params.sleepAngularThreshold) {
            framesToStop = -1;
        } else if (framesToStop < 0) {
            framesToStop = frame;
        }
        sleptWhileSpinning = sleptWhileSpinning || ((bodies.flags[box] & RB_SLEEPING) != 0u && frame < 10);
    }
    // Friction torque of a uniform pressure square: mu m g * (2/3 of the mean arm); stop time ~ I w / tau.
    std::printf("spinning box: stopped after %d frames, asleep %d, yaw rate %.5f, |v| %.5f\n", framesToStop,
                (bodies.flags[box] & RB_SLEEPING) != 0u ? 1 : 0, bodies.angularVelocities[box].length(),
                bodies.linearVelocities[box].length());
    expectTrue(!sleptWhileSpinning, "a spinning body is not put to sleep");
    expectTrue(framesToStop > 10, "friction takes time to stop the spin");
    expectTrue((bodies.flags[box] & RB_SLEEPING) != 0u, "the box sleeps once friction has stopped the spin");
    expectTrue(std::fabs(bodies.positions[box].y - 0.5f) < 0.01f && bodies.positions[box].x * bodies.positions[box].x +
                                                                             bodies.positions[box].z * bodies.positions[box].z <
                                                                         1e-4f,
               "spinning in place does not walk the box");
}

void testOrientedNarrowphase() {
    using namespace fuse::physics::narrowphase;
    // A box yawed 45 deg resting on an axis-aligned box: the incident face clips to an octagon,
    // reduced to four points, all at the penetration depth, normal straight up.
    const quat yaw = quatFromAxisAngle({0.f, 1.f, 0.f}, kPi / 4.f);
    ContactManifold m = collideOrientedBoxBox({0.f, 0.98f, 0.f}, yaw, {0.5f, 0.5f, 0.5f}, {}, {}, {0.5f, 0.5f, 0.5f},
                                              0u, 1u);
    f32 worstDepth = 0.f;
    for (u32 i = 0; i < m.pointCount; ++i) {
        worstDepth = std::max(worstDepth, std::fabs(m.points[i].penetration - 0.02f));
    }
    std::printf("OBB-OBB yawed face: valid %d, %u points, normal (%.3f %.3f %.3f), depth error %.2e\n", m.valid ? 1 : 0,
                m.pointCount, m.contactNormal.x, m.contactNormal.y, m.contactNormal.z, worstDepth);
    expectTrue(m.valid && m.pointCount == 4u, "yawed box on box: four clipped face points");
    expectTrue(std::fabs(m.contactNormal.y - 1.f) < 1e-5f, "yawed box on box: normal from B to A (up)");
    expectTrue(worstDepth < 1e-4f, "yawed box on box: every point at the face penetration");

    // Edge-on-edge: two boxes crossed at 90 deg, each rolled 45 deg so edges meet.
    const quat rollA = quatFromAxisAngle({1.f, 0.f, 0.f}, kPi / 4.f);
    const quat rollB = quatFromAxisAngle({0.f, 0.f, 1.f}, kPi / 4.f);
    const f32 reach = 0.5f * std::sqrt(2.f);
    m = collideOrientedBoxBox({0.f, 2.f * reach - 0.03f, 0.f}, rollA, {0.5f, 0.5f, 0.5f}, {}, rollB,
                              {0.5f, 0.5f, 0.5f}, 0u, 1u);
    std::printf("OBB-OBB crossed edges: valid %d, %u points, normal (%.3f %.3f %.3f), depth %.4f\n", m.valid ? 1 : 0,
                m.pointCount, m.contactNormal.x, m.contactNormal.y, m.contactNormal.z, m.penetrationDepth);
    expectTrue(m.valid && m.pointCount == 1u && std::fabs(m.contactNormal.y - 1.f) < 1e-3f &&
                   std::fabs(m.penetrationDepth - 0.03f) < 1e-3f,
               "crossed edges: one edge-edge point, normal up, depth 0.03");
    m = collideOrientedBoxBox({0.f, 2.f * reach + 0.03f, 0.f}, rollA, {0.5f, 0.5f, 0.5f}, {}, rollB,
                              {0.5f, 0.5f, 0.5f}, 0u, 1u);
    expectTrue(!m.valid, "separated crossed edges: no contact");

    // Box balanced on a corner: exactly one plane contact at the lowest corner.
    const quat corner = quatMul(quatFromAxisAngle({1.f, 0.f, 0.f}, std::atan(1.f / std::sqrt(2.f))),
                                quatFromAxisAngle({0.f, 0.f, 1.f}, kPi / 4.f));
    const f32 cornerReach = 0.5f * std::sqrt(3.f);
    m = collideOrientedBoxPlane({0.f, cornerReach - 0.01f, 0.f}, corner, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, 0.f, 0u,
                                1u);
    std::printf("OBB-plane corner: %u points, depth %.4f\n", m.pointCount, m.penetrationDepth);
    expectTrue(m.valid && m.pointCount == 1u && std::fabs(m.penetrationDepth - 0.01f) < 1e-4f,
               "box on a corner: one contact at the lowest corner");
    m = collideOrientedBoxPlane({0.f, 0.49f, 0.f}, {}, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, 0.f, 0u, 1u);
    expectTrue(m.valid && m.pointCount == 4u, "flat box on a plane: four corner contacts");

    // Sphere against a rotated box face: the normal follows the face.
    const quat tilt = quatFromAxisAngle({0.f, 0.f, 1.f}, 0.5f);
    const vec3 faceNormal = rotate(tilt, {0.f, 1.f, 0.f});
    m = collideOrientedBoxSphere(faceNormal * 0.9f, 0.5f, {}, tilt, {0.5f, 0.5f, 0.5f}, 0u, 1u);
    expectTrue(m.valid && (m.contactNormal - faceNormal).length() < 1e-4f && std::fabs(m.penetrationDepth - 0.1f) < 1e-4f,
               "sphere on a rotated box face: normal along the face, depth 0.1");
}

} // namespace

int main() {
    fuse::core::initialize();
    testOrientedNarrowphase();
    testOffCentreImpulse();
    testKinematicTargetOrientation();
    testSpinningSphereConservesMomentum();
    testTumblingBoxEnergy();
    testBoxTipsOverAndRestsFlat();
    testBoxOnIncline();
    testOrientedBoxStack();
    testCapsuleFallsOnItsSide();
    testSpinningBoxSleepsOnlyWhenStopped();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_rotation_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_rotation_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
