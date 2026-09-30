// G11 (GAP-WORLD2D-GAMEPLAY, first half): 2D rigid-body gates for PhysicsWorld2D.
//   - mass / inertia of circles, boxes and hulls vs analytic; torque -> angular acceleration
//   - a tumbling box settles flat on a one-sided edge chain; a frictionless box slides over chain
//     vertices without snagging (ghost vertices)
//   - a box stack on a chain stays upright (sequential impulses + warm starting)
//   - revolute and distance pendulum periods match the analytic physical pendulum within 1%
//   - revolute limit, prismatic limit + motor
//   - contact begin / end events fire exactly once per touch (spans and listener), sensor events
//     fire once and sensors do not respond
//   - BVH ray casts (closest / all) and AABB / point queries == brute force over every shape
//   - deterministic stepping: two worlds fed the same commands end bit-identical
//   - a steady-state step makes zero heap allocations

#include <fuse/physics/physics_world_2d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <vector>

namespace {
bool g_counting = false;
unsigned long g_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    if (g_counting) {
        ++g_allocations;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse;
using namespace fuse::physics;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("  ok: %s\n", message);
    }
}

constexpr f32 kPi = 3.14159265358979f;

PhysicsWorld2D makeWorld() {
    PhysicsWorld2D world;
    world.init();
    return world;
}

BodyId2D staticBody(PhysicsWorld2D& world, vec2 p = {0.f, 0.f}) {
    BodyDef2D def;
    def.type = BodyType2D::Static;
    def.position = p;
    return world.createBody(def);
}

BodyId2D dynamicBody(PhysicsWorld2D& world, vec2 p, f32 angle = 0.f) {
    BodyDef2D def;
    def.position = p;
    def.angle = angle;
    return world.createBody(def);
}

/// Ground chain along y = 0 from x = +20 to x = -20 (right-to-left so the collision side faces up),
/// with vertices every 2 m.
ShapeId2D addGroundChain(PhysicsWorld2D& world, BodyId2D ground, f32 friction = 0.6f) {
    std::vector<vec2> points;
    for (int i = 10; i >= -10; --i) {
        points.push_back({2.f * static_cast<f32>(i), 0.f});
    }
    ShapeDef2D def;
    def.friction = friction;
    return world.addChainShape(ground, points, false, def);
}

// ---------------------------------------------------------------------------------------------------

void testMassAndTorque() {
    std::printf("mass / inertia / torque\n");
    PhysicsWorld2D world = makeWorld();
    world.setGravity({0.f, 0.f});
    const BodyId2D box = dynamicBody(world, {0.f, 0.f});
    ShapeDef2D def;
    def.density = 2.f;
    world.addBoxShape(box, 1.f, 0.5f, {0.f, 0.f}, 0.f, def);
    const f32 m = 2.f * 2.f * 1.f;
    const f32 inertia = m * (2.f * 2.f + 1.f * 1.f) / 12.f;
    check(std::fabs(world.bodyMass(box) - m) < 1e-4f, "box mass = density * area");
    check(std::fabs(world.bodyInertia(box) - inertia) < 1e-4f, "box inertia = m (w^2 + h^2) / 12");

    const BodyId2D disc = dynamicBody(world, {5.f, 0.f});
    world.addCircleShape(disc, 0.5f, {0.3f, 0.f}, def);
    const f32 md = 2.f * kPi * 0.25f;
    check(std::fabs(world.bodyMass(disc) - md) < 1e-4f, "circle mass = density * pi r^2");
    check(std::fabs(world.bodyInertia(disc) - 0.5f * md * 0.25f) < 1e-4f, "offset circle inertia about its centre");
    check(std::fabs(world.bodyWorldCenter(disc).x - 5.3f) < 1e-5f, "world centre follows the offset circle");

    const BodyId2D tri = dynamicBody(world, {-5.f, 0.f});
    const vec2 triPts[] = {{0.f, 0.f}, {3.f, 0.f}, {0.f, 3.f}, {1.f, 1.f}};
    const ShapeId2D triShape = world.addPolygonShape(tri, triPts, def);
    check(triShape != kInvalidId2D, "hull of 4 points with one interior point is a triangle");
    check(std::fabs(world.bodyMass(tri) - 2.f * 4.5f) < 1e-4f, "triangle mass");
    check(std::fabs(world.bodyWorldCenter(tri).x - (-4.f)) < 1e-4f && std::fabs(world.bodyWorldCenter(tri).y - 1.f) < 1e-4f,
          "triangle centroid");
    const vec2 degenerate[] = {{0.f, 0.f}, {1.f, 1.f}, {2.f, 2.f}};
    check(world.addPolygonShape(tri, degenerate, def) == kInvalidId2D, "collinear points are rejected");

    // Torque: w(t) = tau / I * t, integrated with the step.
    const f32 tau = 3.f;
    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 60; ++i) {
        world.applyTorque(box, tau);
        world.stepRigid(dt);
    }
    const f32 expectedW = tau / inertia * 1.f;
    check(std::fabs(world.bodyAngularVelocity(box) - expectedW) < 1e-3f * expectedW, "torque -> angular velocity");
    // Semi-implicit Euler: angle = sum_{k=1..n} k * dt * alpha * dt = alpha dt^2 n(n+1)/2.
    const f32 alpha = tau / inertia;
    const f32 expectedAngle = alpha * dt * dt * 60.f * 61.f * 0.5f;
    check(std::fabs(world.bodyAngle(box) - expectedAngle) < 1e-3f * expectedAngle, "angular velocity -> angle");

    // Off-centre impulse spins and translates.
    world.applyLinearImpulse(disc, {0.f, 1.f}, world.bodyWorldCenter(disc) + vec2{0.5f, 0.f});
    check(world.bodyAngularVelocity(disc) > 0.f && world.bodyLinearVelocity(disc).y > 0.f,
          "off-centre impulse adds linear and angular velocity");
}

void testBoxSettlesOnChain() {
    std::printf("tumbling box settles on an edge chain\n");
    PhysicsWorld2D world = makeWorld();
    const BodyId2D ground = staticBody(world);
    const ShapeId2D chain = addGroundChain(world, ground);
    check(chain != kInvalidId2D && world.shapeCount() == 20u, "chain of 21 points = 20 edges");

    const BodyId2D box = dynamicBody(world, {0.3f, 3.f}, 0.7f);
    world.addBoxShape(box, 0.5f, 0.5f);
    world.setBodyLinearVelocity(box, {3.f, 0.f});
    world.setBodyAngularVelocity(box, 4.f);

    const f32 dt = 1.f / 60.f;
    bool touchedChain = false;
    for (int i = 0; i < 60 * 6; ++i) {
        world.step(dt);
        touchedChain = touchedChain || world.bodyContactCount(box) > 0u;
    }
    const vec2 p = world.bodyPosition(box);
    const vec2 v = world.bodyLinearVelocity(box);
    const f32 w = world.bodyAngularVelocity(box);
    f32 a = std::fmod(std::fabs(world.bodyAngle(box)), 0.5f * kPi);
    a = std::min(a, 0.5f * kPi - a);
    std::printf("    rest: p=(%.4f, %.4f) angle-mod=%.5f |v|=%.5f w=%.5f\n", static_cast<double>(p.x),
                static_cast<double>(p.y), static_cast<double>(a), static_cast<double>(std::hypot(v.x, v.y)),
                static_cast<double>(w));
    check(touchedChain, "box touched the chain");
    check(std::hypot(v.x, v.y) < 0.02f && std::fabs(w) < 0.02f, "box is at rest");
    check(a < 0.01f, "box rests on a face");
    check(std::fabs(p.y - 0.5f) < 0.03f, "box rests at half-height above the chain");
    check(p.x > 0.3f, "box travelled over chain vertices");
}

void testNoSnagOnChainVertices() {
    std::printf("frictionless box slides over chain vertices\n");
    PhysicsWorld2D world = makeWorld();
    const BodyId2D ground = staticBody(world);
    addGroundChain(world, ground, 0.f);
    const BodyId2D box = dynamicBody(world, {-15.f, 0.515f});
    ShapeDef2D def;
    def.friction = 0.f;
    world.addBoxShape(box, 0.5f, 0.5f, {0.f, 0.f}, 0.f, def);
    world.setBodyLinearVelocity(box, {5.f, 0.f});
    const f32 dt = 1.f / 60.f;
    f32 minVx = 1e9f;
    f32 maxAbsVy = 0.f;
    f32 maxAbsW = 0.f;
    for (int i = 0; i < 60 * 5; ++i) {
        world.step(dt);
        if (i > 10) {
            minVx = std::min(minVx, world.bodyLinearVelocity(box).x);
            maxAbsVy = std::max(maxAbsVy, std::fabs(world.bodyLinearVelocity(box).y));
            maxAbsW = std::max(maxAbsW, std::fabs(world.bodyAngularVelocity(box)));
        }
    }
    std::printf("    min vx=%.4f max|vy|=%.4f max|w|=%.4f x=%.3f\n", static_cast<double>(minVx),
                static_cast<double>(maxAbsVy), static_cast<double>(maxAbsW),
                static_cast<double>(world.bodyPosition(box).x));
    check(minVx > 4.95f, "no horizontal snag at interior vertices");
    check(maxAbsVy < 0.05f && maxAbsW < 0.05f, "no bump or spin at interior vertices");
    check(world.bodyPosition(box).x > 8.f, "box crossed many vertices");

    // A circle below a one-sided chain passes up through it.
    PhysicsWorld2D w2 = makeWorld();
    const BodyId2D g2 = staticBody(w2);
    addGroundChain(w2, g2);
    const BodyId2D ball = dynamicBody(w2, {0.5f, -1.f});
    w2.addCircleShape(ball, 0.25f);
    w2.setBodyLinearVelocity(ball, {0.f, 8.f});
    for (int i = 0; i < 30; ++i) {
        w2.step(dt);
    }
    check(w2.bodyPosition(ball).y > 0.5f, "one-sided chain lets bodies through from behind");
}

void testStack() {
    std::printf("box stack on a chain\n");
    PhysicsWorld2D world = makeWorld();
    const BodyId2D ground = staticBody(world);
    addGroundChain(world, ground);
    BodyId2D boxes[6];
    for (int i = 0; i < 6; ++i) {
        boxes[i] = dynamicBody(world, {0.f, 0.5f + 1.02f * static_cast<f32>(i)});
        world.addBoxShape(boxes[i], 0.5f, 0.5f);
    }
    for (int i = 0; i < 60 * 5; ++i) {
        world.step(1.f / 60.f);
    }
    f32 maxDx = 0.f;
    f32 maxSpeed = 0.f;
    for (int i = 0; i < 6; ++i) {
        maxDx = std::max(maxDx, std::fabs(world.bodyPosition(boxes[i]).x));
        const vec2 v = world.bodyLinearVelocity(boxes[i]);
        maxSpeed = std::max(maxSpeed, std::hypot(v.x, v.y));
    }
    const f32 topY = world.bodyPosition(boxes[5]).y;
    std::printf("    max|dx|=%.5f max speed=%.5f top y=%.4f\n", static_cast<double>(maxDx),
                static_cast<double>(maxSpeed), static_cast<double>(topY));
    check(maxDx < 0.02f, "stack stays upright");
    check(maxSpeed < 0.05f, "stack at rest");
    check(std::fabs(topY - 5.5f) < 0.15f, "stack height preserved");
}

/// Period from upward zero crossings of the bob's x (linear interpolation), averaged over cycles.
f32 measurePeriod(PhysicsWorld2D& world, BodyId2D bob, f32 dt, int steps) {
    f32 prevX = world.bodyWorldCenter(bob).x;
    f32 t = 0.f;
    std::vector<f32> crossings;
    for (int i = 0; i < steps; ++i) {
        world.step(dt);
        t += dt;
        const f32 x = world.bodyWorldCenter(bob).x;
        if (prevX < 0.f && x >= 0.f) {
            crossings.push_back(t - dt + dt * (-prevX) / (x - prevX));
        }
        prevX = x;
    }
    if (crossings.size() < 2u) {
        return 0.f;
    }
    return (crossings.back() - crossings.front()) / static_cast<f32>(crossings.size() - 1u);
}

void testPendulums() {
    std::printf("pendulum periods\n");
    const f32 length = 2.f;
    const f32 radius = 0.1f;
    const f32 theta0 = 0.15f;
    const f32 g = 10.f;
    // Physical pendulum (disc bob): I_pivot = I_cm + m L^2; finite-amplitude series correction.
    const f32 iOverM = 0.5f * radius * radius + length * length;
    const f32 t0 = 2.f * kPi * std::sqrt(iOverM / (g * length));
    const f32 analytic = t0 * (1.f + theta0 * theta0 / 16.f + 11.f * std::pow(theta0, 4.f) / 3072.f);
    const f32 dt = 1.f / 240.f;

    {
        PhysicsWorld2D world = makeWorld();
        const BodyId2D pivot = staticBody(world);
        const vec2 bobPos{length * std::sin(theta0), -length * std::cos(theta0)};
        const BodyId2D bob = dynamicBody(world, bobPos);
        world.addCircleShape(bob, radius);
        RevoluteJointDef2D jd;
        jd.initialize(world, pivot, bob, {0.f, 0.f});
        const JointId2D joint = world.createRevoluteJoint(jd);
        check(joint != kInvalidId2D, "revolute joint created");
        const f32 period = measurePeriod(world, bob, dt, 240 * 30);
        const f32 err = std::fabs(period - analytic) / analytic;
        std::printf("    revolute: period=%.5f analytic=%.5f err=%.4f%%\n", static_cast<double>(period),
                    static_cast<double>(analytic), static_cast<double>(err * 100.f));
        check(err < 0.01f, "revolute pendulum period within 1% of analytic");
        const vec2 c = world.bodyWorldCenter(bob);
        check(std::fabs(std::hypot(c.x, c.y) - length) < 0.01f, "revolute keeps the bob on the circle");
    }
    {
        // Distance joint: the bob can spin freely, so the bob's own inertia does not enter the period
        // (rotation about its centre decouples): T = 2 pi sqrt(L / g) with the amplitude correction.
        PhysicsWorld2D world = makeWorld();
        const BodyId2D pivot = staticBody(world);
        const vec2 bobPos{length * std::sin(theta0), -length * std::cos(theta0)};
        const BodyId2D bob = dynamicBody(world, bobPos);
        world.addCircleShape(bob, radius);
        DistanceJointDef2D jd;
        jd.initialize(world, pivot, bob, {0.f, 0.f}, bobPos);
        world.createDistanceJoint(jd);
        const f32 pointAnalytic = 2.f * kPi * std::sqrt(length / g) *
                                  (1.f + theta0 * theta0 / 16.f + 11.f * std::pow(theta0, 4.f) / 3072.f);
        const f32 period = measurePeriod(world, bob, dt, 240 * 30);
        const f32 err = std::fabs(period - pointAnalytic) / pointAnalytic;
        std::printf("    distance: period=%.5f analytic=%.5f err=%.4f%%\n", static_cast<double>(period),
                    static_cast<double>(pointAnalytic), static_cast<double>(err * 100.f));
        check(err < 0.01f, "distance pendulum period within 1% of analytic");
    }
}

void testJointLimitsAndMotors() {
    std::printf("joint limits / motors\n");
    const f32 dt = 1.f / 60.f;
    {
        PhysicsWorld2D world = makeWorld();
        const BodyId2D pivot = staticBody(world);
        const BodyId2D arm = dynamicBody(world, {1.f, 0.f});
        world.addBoxShape(arm, 1.f, 0.1f);
        RevoluteJointDef2D jd;
        jd.initialize(world, pivot, arm, {0.f, 0.f});
        jd.enableLimit = true;
        jd.lowerAngle = -0.5f;
        jd.upperAngle = 0.5f;
        const JointId2D j = world.createRevoluteJoint(jd);
        f32 minAngle = 0.f;
        for (int i = 0; i < 180; ++i) {
            world.step(dt);
            minAngle = std::min(minAngle, world.revoluteJointAngle(j));
        }
        std::printf("    revolute limit: min angle=%.4f final=%.4f\n", static_cast<double>(minAngle),
                    static_cast<double>(world.revoluteJointAngle(j)));
        check(minAngle > -0.5f - 0.05f, "revolute lower limit holds");
        check(std::fabs(world.revoluteJointAngle(j) + 0.5f) < 0.02f, "arm rests on the lower limit");

        world.setJointMotor(j, true, 1.f, 1000.f);
        for (int i = 0; i < 20; ++i) {
            world.step(dt);
        }
        check(std::fabs(world.bodyAngularVelocity(arm) - 1.f) < 0.02f, "revolute motor drives the set speed");
    }
    {
        PhysicsWorld2D world = makeWorld();
        const BodyId2D base = staticBody(world);
        const BodyId2D slider = dynamicBody(world, {0.f, 0.f});
        world.addBoxShape(slider, 0.25f, 0.25f);
        PrismaticJointDef2D jd;
        jd.initialize(world, base, slider, {0.f, 0.f}, {0.f, 1.f});
        jd.enableLimit = true;
        jd.lowerTranslation = -1.f;
        jd.upperTranslation = 1.f;
        const JointId2D j = world.createPrismaticJoint(jd);
        world.applyLinearImpulse(slider, {3.f, 0.f}, {0.f, 0.3f}); // sideways kick: axis + angle locked
        for (int i = 0; i < 120; ++i) {
            world.step(dt);
        }
        std::printf("    prismatic: translation=%.4f x=%.5f angle=%.5f\n",
                    static_cast<double>(world.prismaticJointTranslation(j)),
                    static_cast<double>(world.bodyPosition(slider).x), static_cast<double>(world.bodyAngle(slider)));
        check(std::fabs(world.prismaticJointTranslation(j) + 1.f) < 0.02f, "prismatic lower limit stops the fall");
        check(std::fabs(world.bodyPosition(slider).x) < 0.01f && std::fabs(world.bodyAngle(slider)) < 0.01f,
              "prismatic keeps the body on the axis without rotating");
        world.setJointMotor(j, true, 2.f, 1000.f);
        for (int i = 0; i < 20; ++i) {
            world.step(dt);
        }
        check(std::fabs(world.bodyLinearVelocity(slider).y - 2.f) < 0.05f, "prismatic motor drives the set speed");
    }
    {
        // Distance joint keeps the length between two dynamic bodies; spring version oscillates.
        PhysicsWorld2D world = makeWorld();
        world.setGravity({0.f, 0.f});
        const BodyId2D a = dynamicBody(world, {0.f, 0.f});
        const BodyId2D b = dynamicBody(world, {3.f, 0.f});
        world.addCircleShape(a, 0.2f);
        world.addCircleShape(b, 0.2f);
        DistanceJointDef2D jd;
        jd.initialize(world, a, b, {0.f, 0.f}, {3.f, 0.f});
        world.createDistanceJoint(jd);
        world.setBodyLinearVelocity(b, {0.f, 4.f});
        for (int i = 0; i < 240; ++i) {
            world.step(dt);
        }
        const vec2 pa = world.bodyWorldCenter(a);
        const vec2 pb = world.bodyWorldCenter(b);
        check(std::fabs(std::hypot(pb.x - pa.x, pb.y - pa.y) - 3.f) < 0.01f, "rigid distance joint keeps its length");
    }
}

struct CountingListener : ContactListener2D {
    int begins = 0;
    int ends = 0;
    int sensorBegins = 0;
    int sensorEnds = 0;
    void beginContact(const ContactEvent2D&) override { ++begins; }
    void endContact(const ContactEvent2D&) override { ++ends; }
    void beginSensor(const SensorEvent2D&) override { ++sensorBegins; }
    void endSensor(const SensorEvent2D&) override { ++sensorEnds; }
};

void testEvents() {
    std::printf("contact / sensor events\n");
    PhysicsWorld2D world = makeWorld();
    CountingListener listener;
    world.setContactListener(&listener);
    const BodyId2D ground = staticBody(world);
    const ShapeId2D groundShape = world.addBoxShape(ground, 10.f, 0.5f, {0.f, -0.5f});
    const BodyId2D box = dynamicBody(world, {0.f, 1.5f});
    const ShapeId2D boxShape = world.addBoxShape(box, 0.5f, 0.5f);

    int begins = 0;
    int ends = 0;
    f32 approach = 0.f;
    vec2 normal{0.f, 0.f};
    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 180; ++i) {
        world.step(dt);
        for (const ContactEvent2D& e : world.contactBeginEvents()) {
            if (e.shapeA == groundShape && e.shapeB == boxShape) {
                ++begins;
                approach = e.approachSpeed;
                normal = e.normal;
            }
        }
        ends += static_cast<int>(world.contactEndEvents().size());
    }
    std::printf("    begins=%d ends=%d approach=%.3f normal=(%.3f, %.3f)\n", begins, ends,
                static_cast<double>(approach), static_cast<double>(normal.x), static_cast<double>(normal.y));
    check(begins == 1 && ends == 0, "resting contact begins exactly once and does not end");
    check(approach > 3.f && approach < 5.f, "begin event reports the approach speed (~sqrt(2 g h))");
    check(normal.y > 0.99f, "begin normal points from shapeA (ground) to shapeB (box)");

    world.setBodyTransform(box, {0.f, 10.f}, 0.f);
    world.setBodyLinearVelocity(box, {0.f, 0.f});
    int endsAfter = 0;
    for (int i = 0; i < 5; ++i) {
        world.step(dt);
        endsAfter += static_cast<int>(world.contactEndEvents().size());
    }
    check(endsAfter == 1, "separation ends the contact exactly once");
    check(listener.begins == 1 && listener.ends == 1, "listener saw the same begin / end");

    // Sensor: a ball falls through a sensor box; begin and end once, trajectory unaffected.
    const BodyId2D sensorBody = staticBody(world, {5.f, 3.f});
    ShapeDef2D sensorDef;
    sensorDef.isSensor = true;
    const ShapeId2D sensor = world.addBoxShape(sensorBody, 1.f, 0.5f, {0.f, 0.f}, 0.f, sensorDef);
    const BodyId2D ball = dynamicBody(world, {5.f, 6.f});
    const ShapeId2D ballShape = world.addCircleShape(ball, 0.2f);
    int sBegin = 0;
    int sEnd = 0;
    bool pairOk = true;
    for (int i = 0; i < 60; ++i) {
        world.step(dt);
        for (const SensorEvent2D& e : world.sensorBeginEvents()) {
            ++sBegin;
            pairOk = pairOk && e.sensorShape == sensor && e.visitorShape == ballShape;
        }
        sEnd += static_cast<int>(world.sensorEndEvents().size());
    }
    // Free fall for 1 s from rest: v = g t (sensor applied no impulse).
    const f32 vy = world.bodyLinearVelocity(ball).y;
    std::printf("    sensor begins=%d ends=%d ball vy=%.4f\n", sBegin, sEnd, static_cast<double>(vy));
    check(sBegin == 1 && sEnd == 1 && pairOk, "sensor begin / end fire once for the visitor");
    check(std::fabs(vy + 10.f) < 1e-3f, "sensor does not push the visitor");
    check(listener.sensorBegins == 1 && listener.sensorEnds == 1, "listener saw the sensor events");

    // Destroying a touching body reports the end on the next step.
    const BodyId2D box2 = dynamicBody(world, {-3.f, 0.5f});
    world.addBoxShape(box2, 0.5f, 0.5f);
    for (int i = 0; i < 10; ++i) {
        world.step(dt);
    }
    const int endsBefore = listener.ends;
    world.destroyBody(box2);
    world.step(dt);
    check(listener.ends == endsBefore + 1, "destroying a touching body ends its contact once");
    world.setContactListener(nullptr);
}

struct Scene {
    PhysicsWorld2D world;
    std::vector<ShapeId2D> shapes;
};

void buildRandomScene(Scene& scene, u32 seed) {
    scene.world.init();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> pos(-50.f, 50.f);
    std::uniform_real_distribution<f32> ang(-kPi, kPi);
    std::uniform_real_distribution<f32> size(0.2f, 2.f);
    std::uniform_int_distribution<int> kind(0, 3);
    for (int i = 0; i < 400; ++i) {
        BodyDef2D def;
        def.type = (i % 3 == 0) ? BodyType2D::Static : BodyType2D::Dynamic;
        def.position = {pos(rng), pos(rng)};
        def.angle = ang(rng);
        const BodyId2D body = scene.world.createBody(def);
        ShapeDef2D sd;
        sd.filter.categoryBits = (i % 5 == 0) ? 2u : 1u;
        switch (kind(rng)) {
        case 0:
            scene.shapes.push_back(scene.world.addCircleShape(body, size(rng), {size(rng) * 0.3f, 0.f}, sd));
            break;
        case 1:
            scene.shapes.push_back(scene.world.addBoxShape(body, size(rng), size(rng), {0.f, 0.f}, ang(rng), sd));
            break;
        case 2: {
            vec2 pts[7];
            for (vec2& p : pts) {
                p = {size(rng) * std::cos(ang(rng)), size(rng) * std::sin(ang(rng))};
            }
            const ShapeId2D s = scene.world.addPolygonShape(body, pts, sd);
            if (s != kInvalidId2D) {
                scene.shapes.push_back(s);
            }
            break;
        }
        default: {
            const vec2 pts[4] = {{0.f, 0.f}, {size(rng), size(rng)}, {2.f * size(rng), 0.f}, {3.f * size(rng), size(rng)}};
            const bool asChain = (i % 2) == 0;
            if (asChain) {
                const ShapeId2D first = scene.world.addChainShape(body, pts, false, sd);
                for (u32 k = 0; k < 3u; ++k) {
                    scene.shapes.push_back(first + k);
                }
            } else {
                scene.shapes.push_back(scene.world.addEdgeShape(body, pts[0], pts[1], sd));
            }
            break;
        }
        }
    }
}

void testQueriesVsBruteForce() {
    std::printf("ray / AABB / point queries vs brute force\n");
    Scene scene;
    buildRandomScene(scene, 1234u);
    PhysicsWorld2D& world = scene.world;
    // Move things a bit so the query tree is built from simulated poses.
    for (int i = 0; i < 10; ++i) {
        world.step(1.f / 60.f);
    }
    std::mt19937 rng(99u);
    std::uniform_real_distribution<f32> pos(-60.f, 60.f);
    std::vector<RayHit2D> hits;
    std::vector<ShapeId2D> ids;
    int closestMismatch = 0;
    int allMismatch = 0;
    int aabbMismatch = 0;
    int pointMismatch = 0;
    int hitCount = 0;
    const u32 masks[2] = {0xFFFFFFFFu, 2u};
    for (int r = 0; r < 3000; ++r) {
        const vec2 p1{pos(rng), pos(rng)};
        const vec2 p2{pos(rng), pos(rng)};
        const u32 mask = masks[r % 2];
        RayHit2D brute;
        std::vector<RayHit2D> filtered;
        for (ShapeId2D shape : scene.shapes) {
            RayHit2D h;
            if ((world.shapeFilter(shape).categoryBits & mask) == 0u || !world.rayCastShape(shape, p1, p2, h)) {
                continue;
            }
            filtered.push_back(h);
            if (!brute.hit || h.fraction < brute.fraction || (h.fraction == brute.fraction && h.shape < brute.shape)) {
                brute = h;
            }
        }
        const RayHit2D fast = world.rayCastClosest(p1, p2, mask);
        if (fast.hit != brute.hit || (fast.hit && (fast.shape != brute.shape || fast.fraction != brute.fraction))) {
            ++closestMismatch;
        }
        hitCount += fast.hit ? 1 : 0;
        const u32 n = world.rayCastAll(p1, p2, hits, mask);
        if (n != filtered.size()) {
            ++allMismatch;
        }

        const vec2 c{pos(rng), pos(rng)};
        const Aabb2D box{{c.x - 3.f, c.y - 2.f}, {c.x + 3.f, c.y + 2.f}};
        world.queryAabb(box, ids, 0xFFFFFFFFu);
        std::vector<ShapeId2D> bruteIds;
        for (ShapeId2D s : scene.shapes) {
            const Aabb2D sb = world.shapeAabb(s);
            if (sb.lower.x <= box.upper.x && box.lower.x <= sb.upper.x && sb.lower.y <= box.upper.y &&
                box.lower.y <= sb.upper.y) {
                bruteIds.push_back(s);
            }
        }
        std::sort(bruteIds.begin(), bruteIds.end());
        if (ids != bruteIds) {
            ++aabbMismatch;
        }
        world.queryPoint(c, ids, 0xFFFFFFFFu);
        for (ShapeId2D s : ids) {
            const Aabb2D sb = world.shapeAabb(s);
            if (!(sb.lower.x <= c.x && c.x <= sb.upper.x && sb.lower.y <= c.y && c.y <= sb.upper.y)) {
                ++pointMismatch;
            }
        }
    }
    std::printf("    rays with hits=%d closest mismatches=%d all-count mismatches=%d aabb mismatches=%d point=%d\n",
                hitCount, closestMismatch, allMismatch, aabbMismatch, pointMismatch);
    check(hitCount > 500, "ray set exercises many hits");
    check(closestMismatch == 0, "BVH closest ray hit == brute force (shape and fraction)");
    check(allMismatch == 0, "BVH all-hits count == brute force");
    check(aabbMismatch == 0, "BVH AABB query == brute force");
    check(pointMismatch == 0, "point query results contain the point's AABB");

    // Point query exactness on a known shape.
    PhysicsWorld2D w = makeWorld();
    const BodyId2D b = staticBody(w, {1.f, 1.f});
    const ShapeId2D s = w.addBoxShape(b, 1.f, 1.f, {0.f, 0.f}, 0.785398f);
    w.queryPoint({1.f, 2.3f}, ids, 0xFFFFFFFFu);
    check(ids.size() == 1u && ids[0] == s, "point inside the rotated box is found");
    w.queryPoint({1.9f, 1.9f}, ids, 0xFFFFFFFFu);
    check(ids.empty(), "point in the rotated box's AABB but outside the box is rejected");
    const RayHit2D h = w.rayCastClosest({-5.f, 1.f}, {5.f, 1.f});
    check(h.hit && std::fabs(h.point.x - (1.f - std::sqrt(2.f))) < 1e-4f, "ray hits the rotated box corner");
}

std::vector<f32> snapshotState(const PhysicsWorld2D& world, u32 bodies) {
    std::vector<f32> out;
    for (u32 b = 0; b < bodies; ++b) {
        const vec2 p = world.bodyPosition(b);
        const vec2 v = world.bodyLinearVelocity(b);
        out.insert(out.end(), {p.x, p.y, world.bodyAngle(b), v.x, v.y, world.bodyAngularVelocity(b)});
    }
    return out;
}

void buildPile(PhysicsWorld2D& world) {
    world.init();
    const BodyId2D ground = staticBody(world);
    addGroundChain(world, ground);
    std::mt19937 rng(7u);
    std::uniform_real_distribution<f32> jitter(-0.3f, 0.3f);
    for (int i = 0; i < 60; ++i) {
        const BodyId2D b = dynamicBody(world, {jitter(rng) * 10.f, 1.f + 1.1f * static_cast<f32>(i % 20)}, jitter(rng));
        if (i % 3 == 0) {
            world.addCircleShape(b, 0.4f);
        } else if (i % 3 == 1) {
            world.addBoxShape(b, 0.4f, 0.3f);
        } else {
            const vec2 pts[5] = {{-0.4f, -0.3f}, {0.4f, -0.3f}, {0.5f, 0.1f}, {0.f, 0.5f}, {-0.5f, 0.1f}};
            world.addPolygonShape(b, pts);
        }
    }
    // A small chain of bodies linked by revolute joints.
    BodyId2D prev = staticBody(world, {12.f, 8.f});
    for (int i = 0; i < 6; ++i) {
        const BodyId2D link = dynamicBody(world, {12.5f + static_cast<f32>(i), 8.f});
        world.addBoxShape(link, 0.5f, 0.1f);
        RevoluteJointDef2D jd;
        jd.initialize(world, prev, link, {12.f + static_cast<f32>(i), 8.f});
        world.createRevoluteJoint(jd);
        prev = link;
    }
}

void testDeterminism() {
    std::printf("deterministic stepping\n");
    PhysicsWorld2D a;
    PhysicsWorld2D b;
    buildPile(a);
    buildPile(b);
    for (int i = 0; i < 400; ++i) {
        a.step(1.f / 60.f);
        b.step(1.f / 60.f);
        if (i == 200) {
            a.applyLinearImpulse(5u, {3.f, 1.f}, a.bodyWorldCenter(5u));
            b.applyLinearImpulse(5u, {3.f, 1.f}, b.bodyWorldCenter(5u));
        }
    }
    const u32 n = a.rigidBodyCount();
    const std::vector<f32> sa = snapshotState(a, n);
    const std::vector<f32> sb = snapshotState(b, n);
    check(n == b.rigidBodyCount() && sa.size() == sb.size() &&
              std::memcmp(sa.data(), sb.data(), sa.size() * sizeof(f32)) == 0,
          "two worlds with the same commands are bit-identical after 400 steps");

    // reset() + rebuild reproduces the same trajectory too.
    a.reset();
    buildPile(a);
    for (int i = 0; i < 400; ++i) {
        a.step(1.f / 60.f);
        if (i == 200) {
            a.applyLinearImpulse(5u, {3.f, 1.f}, a.bodyWorldCenter(5u));
        }
    }
    const std::vector<f32> sc = snapshotState(a, n);
    check(std::memcmp(sa.data(), sc.data(), sa.size() * sizeof(f32)) == 0, "reset + rebuild replays bit-identically");

    // Steady state: no heap allocations per step once buffers reached their working size.
    std::vector<RayHit2D> hits;
    hits.reserve(64);
    std::vector<ShapeId2D> ids;
    ids.reserve(256);
    for (int i = 0; i < 30; ++i) {
        a.step(1.f / 60.f);
        (void)a.rayCastClosest({-20.f, 5.f}, {20.f, 0.f});
        a.rayCastAll({-20.f, 5.f}, {20.f, 0.f}, hits);
        a.queryAabb({{-5.f, 0.f}, {5.f, 5.f}}, ids);
    }
    g_allocations = 0;
    g_counting = true;
    for (int i = 0; i < 60; ++i) {
        a.step(1.f / 60.f);
        (void)a.rayCastClosest({-20.f, 5.f}, {20.f, 0.f});
        a.rayCastAll({-20.f, 5.f}, {20.f, 0.f}, hits);
        a.queryAabb({{-5.f, 0.f}, {5.f, 5.f}}, ids);
    }
    g_counting = false;
    std::printf("    allocations over 60 steady steps + queries: %lu\n", g_allocations);
    check(g_allocations == 0u, "steady-state step and queries make zero heap allocations");
}

} // namespace

int main() {
    testMassAndTorque();
    testBoxSettlesOnChain();
    testNoSnagOnChainVertices();
    testStack();
    testPendulums();
    testJointLimitsAndMotors();
    testEvents();
    testQueriesVsBruteForce();
    testDeterminism();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d G11 physics2d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all G11 physics2d gates passed\n");
    return EXIT_SUCCESS;
}
