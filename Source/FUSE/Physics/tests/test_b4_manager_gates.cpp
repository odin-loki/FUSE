// B4.11 integration gate rows (master plan), on the PhysicsManager ECS bridge (B4.9/B4.10):
//  - ECS Transform components reflect physics positions every frame
//  - apply_impulse produces the velocity change impulse / mass
//  - CollisionEventSystem dispatches Enter/Exit correctly — no missed or spurious callbacks
//  - kinematic body moves along its programmed path and pushes dynamic bodies
//  - PhysicsManager::step < 8 ms for 1000 active rigid bodies (optimised builds)
//  - 1000 dynamic bodies, full pipeline (broad + narrow + 10 PBD iterations + integrate) < 4 ms
//  - 10k sleeping bodies < 0.5 ms (sleep check only)
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/physics_manager.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::u32;
using fuse::usize;
using fuse::ecs::Collider;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;
using fuse::physics::CollisionEvent;
using fuse::physics::CollisionEventType;
using fuse::physics::PhysicsManager;
using fuse::physics::PhysicsManagerDesc;
using fuse::physics::PhysicsStreamManager;
using pvec3 = fuse::physics::vec3;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;

EntityID spawn(Registry& reg, pvec3 position, u32 shape, pvec3 params, bool isStatic = false, f32 mass = 1.f) {
    const EntityID id = reg.create();
    Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.is_static = isStatic;
    rb.mass = mass;
    rb.restitution = 0.f;
    reg.add(id, rb);
    Collider c{};
    c.shape = shape;
    c.params = {params.x, params.y, params.z, 0.f};
    reg.add(id, c);
    return id;
}

EntityID spawnGround(Registry& reg) {
    return spawn(reg, {}, Collider::Plane, {0.f, 1.f, 0.f}, true);
}

pvec3 positionOf(Registry& reg, EntityID id) {
    const Transform* t = reg.get<Transform>(id);
    return {t->position.x, t->position.y, t->position.z};
}

void testTransformsTrackPhysics() {
    Registry reg;
    reg.init(256);
    spawnGround(reg);
    std::vector<EntityID> bodies;
    std::mt19937 rng(9u);
    std::uniform_real_distribution<f32> xz(-3.f, 3.f);
    std::uniform_real_distribution<f32> y(1.f, 8.f);
    for (int i = 0; i < 60; ++i) {
        const bool box = i % 3 == 0;
        bodies.push_back(spawn(reg, {xz(rng), y(rng), xz(rng)}, box ? Collider::Box : Collider::Sphere,
                               box ? pvec3{0.4f, 0.4f, 0.4f} : pvec3{0.4f, 0.f, 0.f}));
    }
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    int mismatches = 0;
    int staleDirty = 0;
    for (int frame = 0; frame < 180; ++frame) {
        std::vector<pvec3> before;
        for (const EntityID id : bodies) {
            reg.get<Transform>(id)->dirty = false;
            before.push_back(positionOf(reg, id));
        }
        manager.step(reg, kDt, streams);
        for (usize i = 0; i < bodies.size(); ++i) {
            const u32 body = manager.bodyIndex(bodies[i]);
            const pvec3 physics = manager.bodies().positions[body];
            const Transform* t = reg.get<Transform>(bodies[i]);
            const bool same = t->position.x == physics.x && t->position.y == physics.y && t->position.z == physics.z;
            mismatches += same ? 0 : 1;
            const bool moved = physics.x != before[i].x || physics.y != before[i].y || physics.z != before[i].z;
            staleDirty += (moved && !t->dirty) ? 1 : 0;
        }
    }
    std::printf("transform sync: %d position mismatches, %d moved-but-not-dirty over 180 frames x 60 bodies\n",
                mismatches, staleDirty);
    expectTrue(mismatches == 0, "every Transform equals its body position after every step");
    expectTrue(staleDirty == 0, "moved bodies flag their Transform dirty for the transform system");
}

void testImpulseVelocityChange() {
    Registry reg;
    reg.init(16);
    const EntityID heavy = spawn(reg, {0.f, 5.f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f}, false, 2.f);
    const EntityID wall = spawn(reg, {5.f, 5.f, 0.f}, Collider::Box, {0.5f, 0.5f, 0.5f}, true);
    PhysicsManagerDesc desc{};
    desc.solver.gravity = {};
    desc.solver.linearDamping = 1.f;
    PhysicsManager manager;
    manager.init(desc);
    PhysicsStreamManager streams{};
    manager.step(reg, kDt, streams);

    manager.applyImpulse(heavy, {4.f, -2.f, 6.f});
    manager.applyImpulse(wall, {100.f, 0.f, 0.f});
    manager.step(reg, kDt, streams);
    const fuse::ecs::vec3 v = reg.get<fuse::ecs::RigidBody>(heavy)->velocity;
    std::printf("impulse: J = (4, -2, 6) on m = 2 -> v = (%.5f, %.5f, %.5f)\n", v.x, v.y, v.z);
    // PBD derives velocity from position deltas, so allow float quantisation (~1e-5 at y = 5).
    expectTrue(std::fabs(v.x - 2.f) < 1e-4f && std::fabs(v.y + 1.f) < 1e-4f && std::fabs(v.z - 3.f) < 1e-4f,
               "apply_impulse changes velocity by impulse / mass");
    expectTrue(positionOf(reg, wall).x == 5.f, "impulse on a static body is ignored");
}

struct EventTally {
    int enter = 0;
    int stay = 0;
    int exit = 0;
    int trigger = 0;
    void add(const CollisionEvent& e) {
        enter += e.type == CollisionEventType::Enter ? 1 : 0;
        stay += e.type == CollisionEventType::Stay ? 1 : 0;
        exit += e.type == CollisionEventType::Exit ? 1 : 0;
        trigger += e.type == CollisionEventType::Trigger ? 1 : 0;
    }
};

void testCollisionEvents() {
    Registry reg;
    reg.init(16);
    const EntityID ground = spawnGround(reg);
    const EntityID ball = spawn(reg, {0.f, 3.f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f});
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    EventTally viaCallback;
    EventTally groundCallback;
    manager.collisionEvents().registerCallback(ball, [&](const CollisionEvent& e) { viaCallback.add(e); });
    manager.collisionEvents().registerCallback(ground, [&](const CollisionEvent& e) { groundCallback.add(e); });

    EventTally total;
    bool slept = false;
    for (int frame = 0; frame < 240; ++frame) { // land, settle, fall asleep
        manager.step(reg, kDt, streams);
        for (const CollisionEvent& e : manager.lastEvents()) {
            total.add(e);
        }
        slept = slept || manager.isSleeping(ball);
    }
    std::printf("events while landing/resting: enter %d stay %d exit %d (asleep %d)\n", total.enter, total.stay,
                total.exit, slept ? 1 : 0);
    expectTrue(total.enter == 1 && total.exit == 0, "one Enter on landing, no Exit while resting");
    expectTrue(slept && total.stay > 100, "contact persists (Stay) through sleep");

    Transform* t = reg.get<Transform>(ball);
    t->position.y = 5.f; // game code lifts the ball off the ground
    manager.step(reg, kDt, streams);
    EventTally lifted;
    for (const CollisionEvent& e : manager.lastEvents()) {
        lifted.add(e);
    }
    expectTrue(lifted.exit == 1 && lifted.enter == 0 && lifted.stay == 0, "exactly one Exit when separated");
    for (int frame = 0; frame < 120; ++frame) {
        manager.step(reg, kDt, streams);
        for (const CollisionEvent& e : manager.lastEvents()) {
            lifted.add(e);
        }
    }
    expectTrue(lifted.enter == 1 && lifted.exit == 1, "lands again: one more Enter, no spurious Exit");
    expectTrue(viaCallback.enter == 2 && viaCallback.exit == 1 && groundCallback.enter == 2 &&
                   groundCallback.exit == 1,
               "callbacks on both entities see every Enter/Exit");

    // Trigger volume on the fall path: reported, never resolved.
    auto drop = [](bool withTrigger, EventTally& tally, f32& landedY) {
        Registry r;
        r.init(16);
        spawnGround(r);
        const EntityID falling = spawn(r, {0.f, 6.f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f});
        if (withTrigger) {
            const EntityID zone = spawn(r, {0.f, 3.f, 0.f}, Collider::Sphere, {0.6f, 0.f, 0.f}, true);
            r.get<Collider>(zone)->is_trigger = true;
        }
        PhysicsManager m;
        m.init({});
        PhysicsStreamManager s{};
        for (int frame = 0; frame < 90; ++frame) {
            m.step(r, kDt, s);
            for (const CollisionEvent& e : m.lastEvents()) {
                tally.add(e);
            }
        }
        landedY = positionOf(r, falling).y;
    };
    EventTally withTrigger;
    EventTally without;
    f32 yWith = 0.f;
    f32 yWithout = 0.f;
    drop(true, withTrigger, yWith);
    drop(false, without, yWithout);
    std::printf("trigger: trigger %d exit %d enter %d, landed y %.4f vs %.4f without trigger\n", withTrigger.trigger,
                withTrigger.exit, withTrigger.enter, yWith, yWithout);
    expectTrue(withTrigger.trigger == 1 && withTrigger.exit == 1, "one Trigger on entering, one Exit on leaving");
    expectTrue(withTrigger.enter == 1 && without.enter == 1, "only the ground raises an Enter");
    expectTrue(yWith == yWithout, "trigger volumes do not deflect bodies");
}

void testKinematicPush() {
    Registry reg;
    reg.init(16);
    spawnGround(reg);
    const EntityID pusher = spawn(reg, {-3.f, 0.5f, 0.f}, Collider::Box, {0.5f, 0.5f, 0.5f});
    reg.add<fuse::ecs::TagKinematic>(pusher);
    const EntityID ball = spawn(reg, {0.f, 0.5f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f});
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    f32 worstPathError = 0.f;
    f32 worstOverlap = 0.f;
    for (int frame = 1; frame <= 150; ++frame) {
        const f32 x = -3.f + 2.f * static_cast<f32>(frame) * kDt; // programmed path: 2 m/s along +x
        reg.get<Transform>(pusher)->position.x = x;
        manager.step(reg, kDt, streams);
        worstPathError = std::max(worstPathError, std::fabs(positionOf(reg, pusher).x - x));
        worstOverlap = std::max(worstOverlap, (positionOf(reg, pusher).x + 1.f) - positionOf(reg, ball).x);
    }
    const pvec3 ballPos = positionOf(reg, ball);
    std::printf("kinematic: path error %.2e m, ball pushed to x %.3f (pusher %.3f), worst overlap %.4f m\n",
                worstPathError, ballPos.x, positionOf(reg, pusher).x, worstOverlap);
    expectTrue(worstPathError < 1e-5f, "kinematic body follows its programmed path exactly");
    expectTrue(ballPos.x > positionOf(reg, pusher).x + 0.95f && ballPos.x > 1.5f, "kinematic body pushes the ball");
    expectTrue(worstOverlap < 0.05f, "pushed ball does not sink into the kinematic body");

    // setKinematicTarget drives a body the same way.
    manager.setKinematicTarget(pusher, {3.f, 0.5f, 0.f}, {});
    manager.step(reg, kDt, streams);
    expectTrue(std::fabs(positionOf(reg, pusher).x - 3.f) < 1e-5f, "setKinematicTarget reaches the target this step");
}

void testStackedBoxesRestOnFaces() {
    Registry reg;
    reg.init(16);
    spawnGround(reg);
    const EntityID base = spawn(reg, {0.f, 0.5f, 0.f}, Collider::Box, {0.5f, 0.5f, 0.5f}, true);
    const EntityID top = spawn(reg, {0.1f, 2.f, 0.f}, Collider::Box, {0.5f, 0.5f, 0.5f});
    const EntityID ball = spawn(reg, {2.f, 3.f, 0.f}, Collider::Sphere, {0.5f, 0.f, 0.f});
    const EntityID crate = spawn(reg, {2.f, 0.5f, 0.f}, Collider::Box, {0.6f, 0.5f, 0.6f}, true);
    (void)base;
    (void)crate;
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    for (int frame = 0; frame < 180; ++frame) {
        manager.step(reg, kDt, streams);
    }
    std::printf("resting heights: box on box %.4f (expect 1.5), sphere on box %.4f (expect 1.5)\n",
                positionOf(reg, top).y, positionOf(reg, ball).y);
    expectTrue(std::fabs(positionOf(reg, top).y - 1.5f) < 0.02f, "box rests on the box below (face contact)");
    expectTrue(std::fabs(positionOf(reg, ball).y - 1.5f) < 0.02f, "sphere rests on the box top, not inside it");
}

double medianMs(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void testThousandBodiesBudget() {
    Registry reg;
    reg.init(2048);
    spawnGround(reg);
    std::mt19937 rng(1000u);
    std::uniform_real_distribution<f32> xz(-12.f, 12.f);
    std::uniform_real_distribution<f32> y(1.f, 20.f);
    for (int i = 0; i < 1000; ++i) {
        const bool box = i % 4 == 0;
        spawn(reg, {xz(rng), y(rng), xz(rng)}, box ? Collider::Box : Collider::Sphere,
              box ? pvec3{0.4f, 0.4f, 0.4f} : pvec3{0.4f, 0.f, 0.f});
    }
    PhysicsManager manager;
    manager.init({});
    PhysicsStreamManager streams{};
    std::vector<double> samples;
    u32 active = 0;
    for (int frame = 0; frame < 90; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        manager.step(reg, kDt, streams);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        active = std::max(active, manager.solver().activeBodyCount());
    }
    const double stepMs = medianMs(samples);

    // Raw pipeline: one pass of broad + narrow + 10 PBD iterations + integrate.
    fuse::physics::RigidBodySoA bodies = manager.bodies();
    const fuse::physics::CollisionShapeSoA shapes = manager.shapes();
    for (u32 i = 0; i < bodies.count(); ++i) {
        bodies.flags[i] &= ~fuse::physics::RB_SLEEPING;
    }
    fuse::physics::PBDSolver solver;
    solver.init(bodies.count(), 16384, 0);
    fuse::physics::SolverParams params{};
    params.substeps = 1;
    params.iterations = 10;
    std::vector<double> pipeline;
    for (int frame = 0; frame < 31; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        solver.step(bodies, shapes, params, kDt);
        pipeline.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const double pipelineMs = medianMs(pipeline);
    std::printf("1000 bodies: manager step median %.3f ms (peak active %u, 4 substeps x 10 iterations); "
                "single-pass pipeline median %.3f ms (%u contacts)\n",
                stepMs, active, pipelineMs, solver.contactCount());
    expectTrue(active >= 900u, "the scene keeps ~1000 bodies active");
#if defined(NDEBUG)
    expectTrue(stepMs < 8.0, "PhysicsManager::step < 8 ms for 1000 active rigid bodies");
    expectTrue(pipelineMs < 4.0, "broad + narrow + 10 PBD iterations + integrate < 4 ms for 1000 bodies");
#endif
}

void testTenThousandSleepingBodies() {
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    const u32 ground = bodies.addBody({}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(fuse::physics::CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f});
    for (int i = 0; i < 10'000; ++i) {
        const u32 body = bodies.addBody({static_cast<f32>(i % 100) * 1.1f, 0.5f, static_cast<f32>(i / 100) * 1.1f},
                                        1.f, fuse::physics::RB_SLEEPING);
        shapes.addShape(fuse::physics::CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
    }
    fuse::physics::PBDSolver solver;
    solver.init(bodies.count(), 1024, 0);
    std::vector<double> samples;
    for (int frame = 0; frame < 101; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        solver.step(bodies, shapes, fuse::physics::SolverParams{}, kDt);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    bool stillAsleep = true;
    for (u32 i = 1; i < bodies.count(); ++i) {
        stillAsleep = stillAsleep && (bodies.flags[i] & fuse::physics::RB_SLEEPING) != 0u;
    }
    const double ms = medianMs(samples);
    std::printf("10k sleeping bodies: solver step median %.4f ms\n", ms);
    expectTrue(stillAsleep && solver.activeBodyCount() == 0u, "sleeping bodies stay asleep and inactive");
#if defined(NDEBUG)
    expectTrue(ms < 0.5, "10k sleeping bodies < 0.5 ms");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    testTransformsTrackPhysics();
    testImpulseVelocityChange();
    testCollisionEvents();
    testKinematicPush();
    testStackedBoxesRestOnFaces();
    testThousandBodiesBudget();
    testTenThousandSleepingBodies();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_manager_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_manager_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
