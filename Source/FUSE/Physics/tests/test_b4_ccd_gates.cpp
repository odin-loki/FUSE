// B4.11 CCD gate rows (master plan):
//  - high-velocity sphere (100 m/s) does not tunnel through a 0.1 m wall — discrete misses, CCD catches
//  - a fast-spinning long thin box does not tunnel through a thin post in its sweep — discrete
//    misses, CCD (conservative advancement with the |w| r_max rotation bound) catches
//  - CCD introduces < 1 ms overhead per frame for 100 fast-moving bodies (optimised builds)
//  - TOI search converges in < 8 iterations for all test cases: sphere/plane/box sweeps are closed
//    form (1 iteration); the iterative solver is conservative advancement (rotating/oriented boxes
//    and capsules). Its iteration count is instrumented (`TOIResult::iterations`,
//    `ccdIterationStats()`) and bounded over every gate case above plus a seeded sweep of hard
//    cases (fast spin, grazing, thin posts, tumbling onto planes), each checked against a dense
//    brute-force reference so the bound is never bought by skipping an impact.
//  - hull, triangle mesh, SDF and voxel pairs sweep the origin-centred bounding box stored in
//    params (half extents x, y, z) instead of tunneling; a zero bound is a point box
#include <fuse/core/sanitizer.hpp>
#include <fuse/core/init.hpp>
#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/ccd_margin.hpp>
#include <fuse/physics/ccd/toi_range.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

namespace {

int g_failures = 0;

using namespace fuse::physics;
using fuse::u32;
using fuse::u64;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;
constexpr u32 kToiIterationBudget = 8u; // plan row: converges in < 8 iterations

/// Asserts the TOI iteration counters accumulated since the last reset are inside the budget.
void expectIterationBudget(const char* gate) {
    const CcdIterationStats stats = ccdIterationStats();
    std::printf("CCD TOI iterations [%s]: %llu sweeps (%llu iterative), max %u, mean %.2f\n", gate,
                static_cast<unsigned long long>(stats.sweeps), static_cast<unsigned long long>(stats.iterativeSweeps),
                stats.maxIterations,
                stats.sweeps > 0u ? static_cast<double>(stats.totalIterations) / static_cast<double>(stats.sweeps) : 0.0);
    expectTrue(stats.sweeps > 0u, "gate exercised the TOI solver");
    expectTrue(stats.maxIterations < kToiIterationBudget, "TOI search converges in < 8 iterations");
    resetCcdIterationStats();
}
constexpr f32 kRadius = 0.1f;
constexpr f32 kWallHalf = 0.05f; // 0.1 m thick

SolverParams params() {
    SolverParams p;
    p.substeps = 4;
    p.iterations = 10;
    p.broadphase.cellSize = 2.f;
    p.broadphase.tableSize = 1024;
    return p;
}

/// Fires a sphere at 100 m/s along +z at a static wall at z = 0; returns the final z.
f32 fireAtWall(f32 startZ, bool ccd, u32& ccdHits) {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 wall = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Box, wall, {5.f, 5.f, kWallHalf});
    const u32 bullet = bodies.addBody({0.f, 0.f, startZ}, 1.f, RB_NO_GRAVITY | (ccd ? RB_CCD : 0u));
    shapes.addShape(CollisionShapeType::Sphere, bullet, {kRadius, 0.f, 0.f});
    bodies.linearVelocities[bullet] = {0.f, 0.f, 100.f};

    PBDSolver solver;
    solver.init(2, 8, 0);
    ccdHits = 0;
    for (int frame = 0; frame < 20; ++frame) {
        solver.step(bodies, shapes, params(), kDt);
        ccdHits += solver.lastCcdHitCount();
    }
    return bodies.positions[bullet].z;
}

void testNoTunnellingThroughThinWall() {
    // Start so every discrete substep sample (100 m/s * dt/4 = 0.4167 m apart) straddles
    // the wall: one sample at z = -0.2, the next at z = +0.217.
    const f32 stride = 100.f * kDt / 4.f;
    const f32 straddleStart = -0.2f - 5.f * stride;
    u32 hits = 0;
    const f32 discreteZ = fireAtWall(straddleStart, false, hits);
    std::printf("CCD: discrete-only final z %.3f (wall at |z| <= %.2f)\n", discreteZ, kWallHalf);
    expectTrue(discreteZ > kWallHalf + kRadius, "discrete stepping alone tunnels through the 0.1 m wall");

    int tunnelled = 0;
    u32 totalHits = 0;
    std::mt19937 rng(100u);
    std::uniform_real_distribution<f32> offset(0.f, stride);
    for (int shot = 0; shot < 50; ++shot) {
        const f32 start = shot == 0 ? straddleStart : -3.f - offset(rng);
        const f32 z = fireAtWall(start, true, hits);
        tunnelled += z > -kWallHalf ? 1 : 0;
        totalHits += hits;
    }
    std::printf("CCD: 50 shots at 100 m/s, %d tunnelled, %u CCD clamps\n", tunnelled, totalHits);
    expectTrue(tunnelled == 0, "CCD catches every 100 m/s shot at the 0.1 m wall");
    expectTrue(totalHits >= 50u, "each shot is clamped by CCD");
}

void testFastSpheresCollideHeadOn() {
    for (int ccd = 0; ccd < 2; ++ccd) {
        RigidBodySoA bodies;
        CollisionShapeSoA shapes;
        const u32 flags = RB_NO_GRAVITY | (ccd != 0 ? RB_CCD : 0u);
        const u32 a = bodies.addBody({0.f, 0.f, -1.1f}, 1.f, flags);
        const u32 b = bodies.addBody({0.f, 0.f, 1.3f}, 1.f, flags);
        shapes.addShape(CollisionShapeType::Sphere, a, {kRadius, 0.f, 0.f});
        shapes.addShape(CollisionShapeType::Sphere, b, {kRadius, 0.f, 0.f});
        bodies.linearVelocities[a] = {0.f, 0.f, 100.f};
        bodies.linearVelocities[b] = {0.f, 0.f, -100.f};
        PBDSolver solver;
        solver.init(2, 8, 0);
        for (int frame = 0; frame < 5; ++frame) {
            solver.step(bodies, shapes, params(), kDt);
        }
        const bool passed = bodies.positions[a].z > bodies.positions[b].z;
        std::printf("CCD head-on (%s): a.z %.3f b.z %.3f\n", ccd != 0 ? "ccd" : "discrete", bodies.positions[a].z,
                    bodies.positions[b].z);
        if (ccd != 0) {
            expectTrue(!passed, "CCD stops fast spheres passing through each other");
        }
    }
}

/// A 2 m bar (0.04 m thick) spinning at 60 rad/s about z around its centre; a 0.04 m post stands in
/// the sweep 0.8 m from the centre. The swing angle phi (unwrapped) must never pass +-pi/2, where
/// either end of the bar meets the post. Returns the largest |phi| reached.
f32 spinBarPastPost(bool ccd, u32& ccdHits) {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 post = bodies.addBody({0.f, 0.8f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Box, post, {0.02f, 0.02f, 1.f});
    const u32 bar = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_NO_GRAVITY | (ccd ? RB_CCD : 0u));
    shapes.addShape(CollisionShapeType::Box, bar, {1.f, 0.02f, 0.02f});
    bodies.angularVelocities[bar] = {0.f, 0.f, 60.f};
    bodies.restitutions[bar] = 0.8f;
    bodies.restitutions[post] = 0.8f;

    PBDSolver solver;
    solver.init(2, 8, 0);
    SolverParams p = params();
    p.linearDamping = 1.f;
    p.angularDamping = 1.f;
    ccdHits = 0;
    f32 phi = 0.f;
    f32 worst = 0.f;
    for (int frame = 0; frame < 120; ++frame) {
        solver.step(bodies, shapes, p, kDt);
        ccdHits += solver.lastCcdHitCount();
        const vec3 axis = rotate(bodies.orientations[bar], {1.f, 0.f, 0.f});
        f32 delta = std::atan2(axis.y, axis.x) - std::fmod(phi, 2.f * 3.14159265f);
        while (delta > 3.14159265f) {
            delta -= 2.f * 3.14159265f;
        }
        while (delta < -3.14159265f) {
            delta += 2.f * 3.14159265f;
        }
        phi += delta;
        worst = std::max(worst, std::fabs(phi));
    }
    return worst;
}

void testSpinningBarDoesNotTunnel() {
    constexpr f32 kHalfPi = 0.5f * 3.14159265f;
    // The post spans about +-0.05 rad around phi = pi/2 (bar and post half thickness over 0.8 m).
    constexpr f32 kPostSpan = 0.06f;
    u32 hits = 0;
    const f32 discrete = spinBarPastPost(false, hits);
    std::printf("CCD spin: discrete-only bar swept to |phi| %.3f rad (post at %.3f rad)\n", discrete, kHalfPi);
    expectTrue(discrete > kHalfPi + 0.5f, "discrete stepping alone lets the spinning bar pass through the post");
    const f32 swept = spinBarPastPost(true, hits);
    std::printf("CCD spin: with CCD bar swept to |phi| %.3f rad, %u CCD clamps\n", swept, hits);
    expectTrue(swept < kHalfPi + kPostSpan, "CCD stops the spinning bar at the post");
    expectTrue(hits >= 1u, "the bar's impact with the post is clamped by CCD");
}

void testCcdOverheadHundredBodies() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::mt19937 rng(1000u);
    std::uniform_real_distribution<f32> pos(-40.f, 40.f);
    std::uniform_real_distribution<f32> dir(-1.f, 1.f);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    for (int i = 0; i < 1000; ++i) { // scenery the fast bodies sweep past
        const u32 body = bodies.addBody({pos(rng), 1.f + std::fabs(pos(rng)) * 0.2f, pos(rng)}, 1.f);
        shapes.addShape(i % 2 == 0 ? CollisionShapeType::Sphere : CollisionShapeType::Box, body,
                        i % 2 == 0 ? vec3{0.5f, 0.f, 0.f} : vec3{0.5f, 0.5f, 0.5f});
    }
    for (int i = 0; i < 100; ++i) {
        const u32 body = bodies.addBody({pos(rng), 5.f, pos(rng)}, 1.f, RB_CCD);
        shapes.addShape(CollisionShapeType::Sphere, body, {0.2f, 0.f, 0.f});
        bodies.linearVelocities[body] = vec3{dir(rng), dir(rng), dir(rng)}.normalized() * 100.f;
    }

    PBDSolver solver;
    solver.init(bodies.count(), 4096, 0);
    std::vector<double> samples;
    for (int i = 0; i < 21; ++i) {
        RigidBodySoA copy = bodies; // same frame each time
        const auto start = std::chrono::steady_clock::now();
        solver.applyContinuousCollision(copy, shapes, kDt);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("CCD overhead: 100 fast bodies among %u, median %.3f ms, %u clamps\n", bodies.count(), samples[10],
                solver.lastCcdHitCount());
#if defined(NDEBUG)
    if (fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(samples[10] < 1.0, "CCD < 1 ms per frame for 100 fast-moving bodies");
    }
#endif
}

/// One hard TOI case: shapes at their frame-start poses with per-frame displacement and rotation
/// vectors (world axis, as the solver integrates them).
struct HardToiCase {
    narrowphase::ShapeInstance a;
    vec3 moveA{};
    vec3 spinA{};
    narrowphase::ShapeInstance b;
    vec3 moveB{};
    vec3 spinB{};
};

narrowphase::ShapeInstance poseAt(const narrowphase::ShapeInstance& shape, vec3 move, vec3 spin, f32 t) {
    narrowphase::ShapeInstance moved = shape;
    moved.position = shape.position + move * t;
    moved.orientation = applyRotationVector(shape.orientation, spin * t);
    return moved;
}

bool overlapsAt(const HardToiCase& c, f32 t) {
    const narrowphase::ContactManifold m =
        narrowphase::collideShapes(poseAt(c.a, c.moveA, c.spinA, t), poseAt(c.b, c.moveB, c.spinB, t), 0u, 1u, 0.f);
    return m.valid && m.pointCount > 0u;
}

/// Brute-force first-contact time: the first of 4000 uniform samples where the shapes overlap, or
/// -1 when none does. The true first contact is at or before it.
f32 referenceFirstContact(const HardToiCase& c) {
    constexpr int kSamples = 4000;
    for (int k = 0; k <= kSamples; ++k) {
        const f32 t = static_cast<f32>(k) / static_cast<f32>(kSamples);
        if (overlapsAt(c, t)) {
            return t;
        }
    }
    return -1.f;
}

narrowphase::ShapeInstance makeShape(CollisionShapeType type, vec3 params, vec3 position, quat orientation, f32 scalar = 0.f) {
    narrowphase::ShapeInstance shape;
    shape.type = type;
    shape.params = params;
    shape.scalar = scalar;
    shape.position = position;
    shape.orientation = orientation;
    return shape;
}

void testToiIterationsOnHardCases() {
    std::mt19937 rng(0xC0DEu);
    std::uniform_real_distribution<f32> unit(0.f, 1.f);
    const auto randomAxis = [&]() {
        vec3 v{};
        do {
            v = {unit(rng) * 2.f - 1.f, unit(rng) * 2.f - 1.f, unit(rng) * 2.f - 1.f};
        } while (v.length() < 0.1f || v.length() > 1.f);
        return v.normalized();
    };
    const auto randomOrientation = [&]() { return quatFromAxisAngle(randomAxis(), unit(rng) * 6.2831853f); };
    const quat identity{0.f, 0.f, 0.f, 1.f};

    const char* names[] = {"fast spin bar vs post", "grazing box vs box", "thin post vs spinning capsule",
                           "tumbling box onto plane", "grazing capsule vs capsule", "spinning box vs capsule"};
    constexpr int kFamilies = 6;
    constexpr int kPerFamily = 400;
    u32 worstOverall = 0;
    int missedImpacts = 0;
    int lateImpacts = 0;
    for (int family = 0; family < kFamilies; ++family) {
        u32 worst = 0;
        u64 total = 0;
        int hits = 0;
        int cases = 0;
        for (int i = 0; i < kPerFamily; ++i) {
            HardToiCase c;
            switch (family) {
            case 0: { // bar up to 1.5 m long spinning up to 150 rad/s (2.5 rad/frame) past a 2 cm post
                const f32 half = 0.5f + unit(rng);
                c.a = makeShape(CollisionShapeType::Box, {half, 0.02f, 0.02f}, {0.f, 0.f, 0.f},
                                quatFromAxisAngle({0.f, 0.f, 1.f}, unit(rng) * 6.2831853f));
                c.spinA = {0.f, 0.f, (unit(rng) < 0.5f ? -1.f : 1.f) * (0.5f + 2.f * unit(rng))};
                c.moveA = {unit(rng) * 0.4f - 0.2f, unit(rng) * 0.4f - 0.2f, 0.f};
                const f32 angle = unit(rng) * 6.2831853f;
                const f32 radius = 0.1f + unit(rng) * (half + 0.2f);
                c.b = makeShape(CollisionShapeType::Box, {0.02f, 0.02f, 1.f},
                                {radius * std::cos(angle), radius * std::sin(angle), 0.f}, identity);
                break;
            }
            case 1: { // oriented boxes flying past each other at 180 m/s (3 m/frame), offsets across the graze line
                c.b = makeShape(CollisionShapeType::Box, {0.5f, 0.3f, 0.4f}, {0.f, 0.f, 0.f}, randomOrientation());
                c.a = makeShape(CollisionShapeType::Box, {0.1f, 0.2f, 0.05f},
                                {-1.5f, unit(rng) * 1.4f - 0.7f, unit(rng) * 1.4f - 0.7f}, randomOrientation());
                c.moveA = {3.f, unit(rng) * 0.2f - 0.1f, unit(rng) * 0.2f - 0.1f};
                c.spinA = randomAxis() * (unit(rng) * 0.5f);
                break;
            }
            case 2: { // spinning, translating thin capsule vs a 1 cm post
                c.b = makeShape(CollisionShapeType::Box, {0.01f, 0.01f, 1.f}, {0.f, 0.f, 0.f}, identity);
                c.a = makeShape(CollisionShapeType::Capsule, {0.01f, 0.3f + unit(rng) * 0.5f, 0.f},
                                {-1.2f, unit(rng) * 1.2f - 0.6f, unit(rng) * 0.4f - 0.2f},
                                quatFromAxisAngle({0.f, 0.f, 1.f}, unit(rng) * 6.2831853f));
                c.moveA = {1.5f + unit(rng) * 1.f, unit(rng) * 0.4f - 0.2f, 0.f};
                c.spinA = {0.f, 0.f, (unit(rng) < 0.5f ? -1.f : 1.f) * (1.f + 2.f * unit(rng))};
                break;
            }
            case 3: { // box tumbling fast onto the ground plane, some glancing
                c.b = makeShape(CollisionShapeType::Plane, {0.f, 1.f, 0.f}, {}, identity, 0.f);
                c.a = makeShape(CollisionShapeType::Box, {0.5f, 0.1f + unit(rng) * 0.4f, 0.3f},
                                {0.f, 1.f + unit(rng) * 1.f, 0.f}, randomOrientation());
                c.moveA = {unit(rng) * 2.f - 1.f, -(unit(rng) * 2.5f), unit(rng) * 2.f - 1.f};
                c.spinA = randomAxis() * (unit(rng) * 2.f);
                break;
            }
            case 4: { // capsules crossing near-tangentially
                c.b = makeShape(CollisionShapeType::Capsule, {0.05f, 0.6f, 0.f}, {0.f, 0.f, 0.f}, randomOrientation());
                c.a = makeShape(CollisionShapeType::Capsule, {0.03f, 0.4f, 0.f},
                                {-2.f, unit(rng) * 1.2f - 0.6f, unit(rng) * 1.2f - 0.6f}, randomOrientation());
                c.moveA = {4.f, unit(rng) * 0.1f - 0.05f, unit(rng) * 0.1f - 0.05f};
                c.spinA = randomAxis() * (unit(rng) * 1.f);
                c.spinB = randomAxis() * (unit(rng) * 1.f);
                break;
            }
            default: { // both spinning: box vs thin capsule, head-on and glancing
                c.b = makeShape(CollisionShapeType::Capsule, {0.02f, 0.8f, 0.f}, {0.f, 0.f, 0.f}, randomOrientation());
                c.a = makeShape(CollisionShapeType::Box, {0.4f, 0.05f, 0.2f},
                                {-1.5f, unit(rng) * 1.f - 0.5f, unit(rng) * 1.f - 0.5f}, randomOrientation());
                c.moveA = {2.5f, 0.f, 0.f};
                c.spinA = randomAxis() * (unit(rng) * 2.f);
                c.spinB = randomAxis() * (unit(rng) * 1.f);
                break;
            }
            }
            if (overlapsAt(c, 0.f)) {
                continue; // resting contact is not a sweep
            }
            ++cases;
            const TOIResult toi = conservativeAdvancementToi(c.a, c.moveA, c.spinA, c.b, c.moveB, c.spinB);
            const f32 reference = referenceFirstContact(c);
            worst = std::max(worst, toi.iterations);
            total += toi.iterations;
            hits += toi.valid ? 1 : 0;
            if (reference >= 0.f && !toi.valid) {
                ++missedImpacts;
                std::fprintf(stderr, "  %s case %d: missed impact at t=%.4f (%u iterations)\n", names[family], i,
                             reference, toi.iterations);
            } else if (reference >= 0.f && toi.toi > reference) {
                ++lateImpacts;
                std::fprintf(stderr, "  %s case %d: toi %.4f after reference contact %.4f\n", names[family], i, toi.toi,
                             reference);
            }
            if (toi.iterations >= kToiIterationBudget) {
                std::fprintf(stderr, "  %s case %d: %u iterations (valid %d toi %.4f ref %.4f)\n", names[family], i,
                             toi.iterations, toi.valid ? 1 : 0, toi.toi, reference);
            }
        }
        std::printf("CCD TOI hard cases [%s]: %d sweeps, %d hits, max %u iterations, mean %.2f\n", names[family], cases,
                    hits, worst, cases > 0 ? static_cast<double>(total) / cases : 0.0);
        expectTrue(cases > kPerFamily / 2 && hits > 0 && hits < cases, "hard-case family mixes hits and misses");
        worstOverall = std::max(worstOverall, worst);
    }
    std::printf("CCD TOI hard cases: worst %u iterations (budget < %u), %d missed impacts, %d late TOIs\n", worstOverall,
                kToiIterationBudget, missedImpacts, lateImpacts);
    expectTrue(worstOverall < kToiIterationBudget, "iterative TOI converges in < 8 iterations on every hard case");
    expectTrue(missedImpacts == 0, "no hard case tunnels (every brute-force impact is caught)");
    expectTrue(lateImpacts == 0, "no hard-case TOI lands after the brute-force first contact");
}

/// One swept pair. `dt` is 1, so linear and angular velocities are the frame displacements.
/// Pooled shapes are not registered in the shape pool: CCD reads type plus `params`.
struct PooledSweep {
    CollisionShapeType typeA = CollisionShapeType::Box;
    vec3 paramsA{};
    vec3 posA{};
    vec3 velA{};
    vec3 spinA{};
    quat orientA{};
    f32 scalarA = 0.f;
    bool ccdA = true;
    CollisionShapeType typeB = CollisionShapeType::Box;
    vec3 paramsB{};
    vec3 posB{};
    vec3 velB{};
    vec3 spinB{};
    quat orientB{};
    f32 scalarB = 0.f;
    bool ccdB = false;
};

TOIResult sweepPair(const PooledSweep& s) {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const u32 a = bodies.addBody(s.posA, 1.f, s.ccdA ? RB_CCD : 0u);
    const u32 b = bodies.addBody(s.posB, 1.f, s.ccdB ? RB_CCD : 0u);
    bodies.linearVelocities[a] = s.velA;
    bodies.linearVelocities[b] = s.velB;
    bodies.angularVelocities[a] = s.spinA;
    bodies.angularVelocities[b] = s.spinB;
    bodies.orientations[a] = s.orientA;
    bodies.orientations[b] = s.orientB;
    shapes.addShape(s.typeA, a, s.paramsA, s.scalarA);
    shapes.addShape(s.typeB, b, s.paramsB, s.scalarB);

    std::vector<broadphase::CandidatePair> pairs{{a, b}};
    std::vector<TOIResult> results;
    CcdPipeline pipeline;
    pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    return results.empty() ? TOIResult{} : results[0];
}

bool finiteVec(vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void expectMatchesBox(const char* label, const TOIResult& pooled, const TOIResult& box) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "%s matches box sweep (pooled valid=%d toi=%.5f, box valid=%d toi=%.5f)", label,
                  pooled.valid ? 1 : 0, pooled.toi, box.valid ? 1 : 0, box.toi);
    const bool finite = std::isfinite(pooled.toi) && finiteVec(pooled.contactPoint) && finiteVec(pooled.contactNormal);
    const bool sameHit = pooled.valid == box.valid && (!pooled.valid || std::fabs(pooled.toi - box.toi) <= 1e-4f);
    const f32 align = (pooled.valid && box.valid) ? pooled.contactNormal.dot(box.contactNormal) : 1.f;
    expectTrue(finite && sameHit && align > 0.99f, message);
}

void expectCaught(const char* label, const TOIResult& result, f32 latest) {
    char message[192];
    std::snprintf(message, sizeof(message), "%s caught before t=%.3f (valid=%d toi=%.5f)", label, latest,
                  result.valid ? 1 : 0, result.toi);
    expectTrue(result.valid && std::isfinite(result.toi) && result.toi > 0.f && result.toi < latest &&
                   finiteVec(result.contactPoint) && finiteVec(result.contactNormal),
               message);
}

const char* pooledShapeName(CollisionShapeType type) {
    switch (type) {
    case CollisionShapeType::ConvexHull:
        return "ConvexHull";
    case CollisionShapeType::TriMesh:
        return "TriMesh";
    case CollisionShapeType::SdfMesh:
        return "SdfMesh";
    case CollisionShapeType::Voxel:
        return "Voxel";
    default:
        return "shape";
    }
}

void testPooledShapesSweepBoundingBoxes() {
    resetCcdIterationStats();
    const CollisionShapeType pooledTypes[] = {
        CollisionShapeType::ConvexHull,
        CollisionShapeType::TriMesh,
        CollisionShapeType::SdfMesh,
        CollisionShapeType::Voxel,
    };

    for (CollisionShapeType type : pooledTypes) {
        const char* name = pooledShapeName(type);
        char label[96];

        // Translating bound into a static box. Gap along z is 2.5 m at 12 m/frame.
        PooledSweep intoBox;
        intoBox.typeA = type;
        intoBox.paramsA = {0.5f, 0.4f, 0.3f};
        intoBox.posA = {0.f, 0.f, -3.f};
        intoBox.velA = {0.f, 0.f, 12.f};
        intoBox.typeB = CollisionShapeType::Box;
        intoBox.paramsB = {1.f, 1.f, 0.2f};
        PooledSweep intoBoxAsBox = intoBox;
        intoBoxAsBox.typeA = CollisionShapeType::Box;
        std::snprintf(label, sizeof(label), "%s vs box", name);
        const TOIResult intoBoxToi = sweepPair(intoBox);
        expectMatchesBox(label, intoBoxToi, sweepPair(intoBoxAsBox));
        expectCaught(label, intoBoxToi, 0.5f);

        // Falling onto a plane, and the same pair with the plane on the other side.
        PooledSweep ontoPlane;
        ontoPlane.typeA = type;
        ontoPlane.paramsA = {0.4f, 0.2f, 0.5f};
        ontoPlane.posA = {0.f, 2.f, 0.f};
        ontoPlane.velA = {0.f, -8.f, 0.f};
        ontoPlane.typeB = CollisionShapeType::Plane;
        ontoPlane.paramsB = {0.f, 1.f, 0.f};
        PooledSweep ontoPlaneAsBox = ontoPlane;
        ontoPlaneAsBox.typeA = CollisionShapeType::Box;
        std::snprintf(label, sizeof(label), "%s vs plane", name);
        const TOIResult ontoPlaneToi = sweepPair(ontoPlane);
        expectMatchesBox(label, ontoPlaneToi, sweepPair(ontoPlaneAsBox));
        expectCaught(label, ontoPlaneToi, 0.5f);

        PooledSweep planeFirst;
        planeFirst.typeA = CollisionShapeType::Plane;
        planeFirst.paramsA = {0.f, 1.f, 0.f};
        planeFirst.ccdA = false;
        planeFirst.typeB = type;
        planeFirst.paramsB = {0.4f, 0.2f, 0.5f};
        planeFirst.posB = {0.f, 2.f, 0.f};
        planeFirst.velB = {0.f, -8.f, 0.f};
        planeFirst.ccdB = true;
        PooledSweep planeFirstAsBox = planeFirst;
        planeFirstAsBox.typeB = CollisionShapeType::Box;
        std::snprintf(label, sizeof(label), "plane vs %s", name);
        const TOIResult planeFirstToi = sweepPair(planeFirst);
        expectMatchesBox(label, planeFirstToi, sweepPair(planeFirstAsBox));
        expectCaught(label, planeFirstToi, 0.5f);

        // A fully collapsed bound is a point. It still has to hit, and it must stay finite.
        PooledSweep pointOntoPlane = ontoPlane;
        pointOntoPlane.paramsA = {};
        pointOntoPlane.posA = {0.f, 1.5f, 0.f};
        pointOntoPlane.velA = {0.f, -6.f, 0.f};
        PooledSweep pointAsBox = pointOntoPlane;
        pointAsBox.typeA = CollisionShapeType::Box;
        std::snprintf(label, sizeof(label), "%s point bound vs plane", name);
        const TOIResult pointToi = sweepPair(pointOntoPlane);
        expectMatchesBox(label, pointToi, sweepPair(pointAsBox));
        expectCaught(label, pointToi, 0.5f);
    }

    // Two pooled shapes, both substituted, closing on each other.
    PooledSweep both;
    both.typeA = CollisionShapeType::ConvexHull;
    both.paramsA = {0.25f, 0.25f, 0.25f};
    both.posA = {0.f, 0.f, -2.f};
    both.velA = {0.f, 0.f, 6.f};
    both.typeB = CollisionShapeType::TriMesh;
    both.paramsB = {0.25f, 0.25f, 0.25f};
    both.posB = {0.f, 0.f, 2.f};
    both.velB = {0.f, 0.f, -6.f};
    both.ccdB = true;
    PooledSweep bothAsBox = both;
    bothAsBox.typeA = CollisionShapeType::Box;
    bothAsBox.typeB = CollisionShapeType::Box;
    const TOIResult bothToi = sweepPair(both);
    expectMatchesBox("ConvexHull vs TriMesh", bothToi, sweepPair(bothAsBox));
    expectCaught("ConvexHull vs TriMesh", bothToi, 0.6f);

    PooledSweep sdfVoxel;
    sdfVoxel.typeA = CollisionShapeType::SdfMesh;
    sdfVoxel.paramsA = {0.4f, 0.3f, 0.2f};
    sdfVoxel.posA = {-2.5f, 0.f, 0.f};
    sdfVoxel.velA = {7.f, 0.f, 0.f};
    sdfVoxel.typeB = CollisionShapeType::Voxel;
    sdfVoxel.paramsB = {0.3f, 0.3f, 0.3f};
    PooledSweep sdfVoxelAsBox = sdfVoxel;
    sdfVoxelAsBox.typeA = CollisionShapeType::Box;
    sdfVoxelAsBox.typeB = CollisionShapeType::Box;
    const TOIResult sdfVoxelToi = sweepPair(sdfVoxel);
    expectMatchesBox("SdfMesh vs Voxel", sdfVoxelToi, sweepPair(sdfVoxelAsBox));
    expectCaught("SdfMesh vs Voxel", sdfVoxelToi, 0.6f);

    // Two degenerate points meeting. Gap 1.5 m at 4 m/frame.
    PooledSweep points;
    points.typeA = CollisionShapeType::ConvexHull;
    points.paramsA = {};
    points.posA = {};
    points.velA = {4.f, 0.f, 0.f};
    points.typeB = CollisionShapeType::TriMesh;
    points.paramsB = {};
    points.posB = {1.5f, 0.f, 0.f};
    PooledSweep pointsAsBox = points;
    pointsAsBox.typeA = CollisionShapeType::Box;
    pointsAsBox.typeB = CollisionShapeType::Box;
    const TOIResult pointsToi = sweepPair(points);
    expectMatchesBox("point hull vs point mesh", pointsToi, sweepPair(pointsAsBox));
    expectCaught("point hull vs point mesh", pointsToi, 0.6f);

    // A zero thickness on one axis stays that flat box (a ground-like mesh is not a point).
    // Front face is 0.5 m from the wall at 4 m/frame, so the hit is well before a point's.
    PooledSweep flat;
    flat.typeA = CollisionShapeType::ConvexHull;
    flat.paramsA = {2.f, 0.f, 2.f};
    flat.posA = {-3.f, 0.f, 0.f};
    flat.velA = {4.f, 0.f, 0.f};
    flat.paramsB = {0.5f, 0.5f, 0.5f};
    PooledSweep flatAsBox = flat;
    flatAsBox.typeA = CollisionShapeType::Box;
    const TOIResult flatToi = sweepPair(flat);
    expectMatchesBox("flat hull vs box", flatToi, sweepPair(flatAsBox));
    expectCaught("flat hull vs box", flatToi, 0.35f);

    // Oriented and spinning bounds take the same non-analytic box sweep, orientation included.
    const quat tilt = quatFromAxisAngle({0.f, 1.f, 0.f}, 0.6f);
    PooledSweep oriented;
    oriented.typeA = CollisionShapeType::ConvexHull;
    oriented.paramsA = {0.3f, 0.2f, 0.35f};
    oriented.posA = {0.f, 0.f, -2.5f};
    oriented.velA = {0.f, 0.f, 6.f};
    oriented.orientA = tilt;
    oriented.paramsB = {1.f, 1.f, 0.25f};
    PooledSweep orientedAsBox = oriented;
    orientedAsBox.typeA = CollisionShapeType::Box;
    const TOIResult orientedToi = sweepPair(oriented);
    expectMatchesBox("oriented hull vs box", orientedToi, sweepPair(orientedAsBox));
    expectCaught("oriented hull vs box", orientedToi, 0.6f);

    PooledSweep spinning;
    spinning.typeA = CollisionShapeType::Voxel;
    spinning.paramsA = {1.f, 0.05f, 0.05f};
    spinning.spinA = {0.f, 0.f, 2.f};
    spinning.typeB = CollisionShapeType::SdfMesh;
    spinning.paramsB = {1.2f, 0.05f, 0.4f};
    spinning.posB = {0.f, 0.7f, 0.f};
    PooledSweep spinningAsBox = spinning;
    spinningAsBox.typeA = CollisionShapeType::Box;
    spinningAsBox.typeB = CollisionShapeType::Box;
    const TOIResult spinningToi = sweepPair(spinning);
    expectMatchesBox("spinning voxel vs sdf", spinningToi, sweepPair(spinningAsBox));
    expectCaught("spinning voxel vs sdf", spinningToi, 0.85f);

    // Sphere versus an axis-aligned bound uses the closed-form sphere-box sweep.
    PooledSweep sphereHull;
    sphereHull.typeA = CollisionShapeType::Sphere;
    sphereHull.paramsA = {0.25f, 0.f, 0.f};
    sphereHull.posA = {0.f, 0.f, -4.f};
    sphereHull.velA = {0.f, 0.f, 15.f};
    sphereHull.typeB = CollisionShapeType::ConvexHull;
    sphereHull.paramsB = {0.5f, 0.5f, 0.5f};
    PooledSweep sphereBox = sphereHull;
    sphereBox.typeB = CollisionShapeType::Box;
    const TOIResult sphereHullToi = sweepPair(sphereHull);
    expectMatchesBox("sphere vs hull", sphereHullToi, sweepPair(sphereBox));
    expectCaught("sphere vs hull", sphereHullToi, 0.4f);

    PooledSweep sphereOriented = oriented;
    sphereOriented.typeA = CollisionShapeType::Sphere;
    sphereOriented.paramsA = {0.2f, 0.f, 0.f};
    sphereOriented.posA = {0.f, 0.f, -3.f};
    sphereOriented.velA = {0.f, 0.f, 8.f};
    sphereOriented.orientA = {};
    sphereOriented.typeB = CollisionShapeType::TriMesh;
    sphereOriented.orientB = tilt;
    sphereOriented.posB = {};
    sphereOriented.velB = {};
    sphereOriented.paramsB = {0.3f, 0.2f, 0.35f};
    PooledSweep sphereOrientedBox = sphereOriented;
    sphereOrientedBox.typeB = CollisionShapeType::Box;
    const TOIResult sphereOrientedToi = sweepPair(sphereOriented);
    expectMatchesBox("sphere vs oriented mesh", sphereOrientedToi, sweepPair(sphereOrientedBox));
    expectCaught("sphere vs oriented mesh", sphereOrientedToi, 0.6f);

    PooledSweep capsule;
    capsule.typeA = CollisionShapeType::Capsule;
    capsule.paramsA = {0.2f, 0.5f, 0.f};
    capsule.posA = {-2.f, 0.f, 0.f};
    capsule.velA = {8.f, 0.f, 0.f};
    capsule.typeB = CollisionShapeType::Voxel;
    capsule.paramsB = {0.3f, 0.3f, 0.3f};
    PooledSweep capsuleBox = capsule;
    capsuleBox.typeB = CollisionShapeType::Box;
    const TOIResult capsuleToi = sweepPair(capsule);
    expectMatchesBox("capsule vs voxel", capsuleToi, sweepPair(capsuleBox));
    expectCaught("capsule vs voxel", capsuleToi, 0.5f);

    // A point bound against a sphere stays on the analytic AABB path and stays finite.
    PooledSweep pointSphere;
    pointSphere.typeA = CollisionShapeType::Sphere;
    pointSphere.paramsA = {0.5f, 0.f, 0.f};
    pointSphere.posA = {0.f, 2.f, 0.f};
    pointSphere.velA = {0.f, -5.f, 0.f};
    pointSphere.typeB = CollisionShapeType::SdfMesh;
    pointSphere.paramsB = {};
    PooledSweep pointSphereBox = pointSphere;
    pointSphereBox.typeB = CollisionShapeType::Box;
    const TOIResult pointSphereToi = sweepPair(pointSphere);
    expectMatchesBox("sphere vs point sdf", pointSphereToi, sweepPair(pointSphereBox));
    expectCaught("sphere vs point sdf", pointSphereToi, 0.5f);

    PooledSweep away;
    away.typeA = CollisionShapeType::ConvexHull;
    away.paramsA = {0.5f, 0.4f, 0.3f};
    away.posA = {0.f, 0.f, -3.f};
    away.velA = {0.f, 0.f, -12.f};
    away.paramsB = {1.f, 1.f, 0.2f};
    PooledSweep awayBox = away;
    awayBox.typeA = CollisionShapeType::Box;
    const TOIResult awayToi = sweepPair(away);
    expectMatchesBox("separating hull", awayToi, sweepPair(awayBox));
    expectTrue(!awayToi.valid, "a hull moving away from a box does not report a hit");

    PooledSweep unflagged = away;
    unflagged.velA = {0.f, 0.f, 12.f};
    unflagged.ccdA = false;
    expectTrue(!sweepPair(unflagged).valid, "a pooled pair still requires RB_CCD");

    // Primitive closed forms are untouched.
    PooledSweep spheres;
    spheres.typeA = CollisionShapeType::Sphere;
    spheres.paramsA = {0.5f, 0.f, 0.f};
    spheres.velA = {10.f, 0.f, 0.f};
    spheres.typeB = CollisionShapeType::Sphere;
    spheres.paramsB = {0.5f, 0.f, 0.f};
    spheres.posB = {5.f, 0.f, 0.f};
    const TOIResult sphereToi = sweepPair(spheres);
    expectTrue(sphereToi.valid && std::fabs(sphereToi.toi - 0.4f) <= 1e-3f, "sphere-sphere TOI is unchanged");

    PooledSweep sphereWall;
    sphereWall.typeA = CollisionShapeType::Sphere;
    sphereWall.paramsA = {0.5f, 0.f, 0.f};
    sphereWall.velA = {10.f, 0.f, 0.f};
    sphereWall.typeB = CollisionShapeType::Box;
    sphereWall.paramsB = {1.f, 1.f, 1.f};
    sphereWall.posB = {5.f, 0.f, 0.f};
    const TOIResult sphereWallToi = sweepPair(sphereWall);
    expectTrue(sphereWallToi.valid && std::fabs(sphereWallToi.toi - 0.35f) <= 1e-3f, "sphere-box TOI is unchanged");

    PooledSweep sphereGround;
    sphereGround.typeA = CollisionShapeType::Sphere;
    sphereGround.paramsA = {1.f, 0.f, 0.f};
    sphereGround.posA = {0.f, 5.f, 0.f};
    sphereGround.velA = {0.f, -10.f, 0.f};
    sphereGround.typeB = CollisionShapeType::Plane;
    sphereGround.paramsB = {0.f, 1.f, 0.f};
    const TOIResult sphereGroundToi = sweepPair(sphereGround);
    expectTrue(sphereGroundToi.valid && std::fabs(sphereGroundToi.toi - 0.4f) <= 1e-3f, "sphere-plane TOI is unchanged");

    std::printf("CCD pooled bounds: hull/mesh/sdf/voxel sweeps match their bounding boxes\n");
}

void testCcdMarginOk() {
    expectTrue(ccd_margin_ok(0.001f), "CCD margin 0.001 is in range");
    expectTrue(ccd_margin_ok(1.f), "CCD margin 1 is in range");
    expectTrue(!ccd_margin_ok(0.f), "a zero CCD margin is rejected");
    expectTrue(!ccd_margin_ok(-0.1f), "a negative CCD margin is rejected");
    expectTrue(!ccd_margin_ok(1.1f), "a CCD margin past 1 is rejected");
    expectTrue(!ccd_margin_ok(std::numeric_limits<float>::quiet_NaN()), "a non-finite CCD margin is rejected");
}

void testToiInRange() {
    expectTrue(toi_in_range(0.f), "TOI 0 is in range");
    expectTrue(toi_in_range(0.25f), "TOI 0.25 is in range");
    expectTrue(toi_in_range(1.f), "TOI 1 is in range");
    expectTrue(!toi_in_range(-0.01f), "a negative TOI is rejected");
    expectTrue(!toi_in_range(1.01f), "a TOI past 1 is rejected");
}

} // namespace

int main() {
    testCcdMarginOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b4_ccd_gates: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testToiInRange();
    fuse::core::initialize();
    resetCcdIterationStats();
    testNoTunnellingThroughThinWall();
    expectIterationBudget("thin wall");
    testFastSpheresCollideHeadOn();
    expectIterationBudget("head-on spheres");
    testSpinningBarDoesNotTunnel();
    expectIterationBudget("spinning bar vs post");
    testCcdOverheadHundredBodies();
    expectIterationBudget("100 fast bodies");
    testToiIterationsOnHardCases();
    testPooledShapesSweepBoundingBoxes();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_ccd_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_ccd_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
