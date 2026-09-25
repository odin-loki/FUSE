// B4.11 broad-phase gate rows (master plan):
//  - spatial hash finds all overlapping pairs for 10k random spheres (vs brute-force O(n^2))
//  - no missed pairs for bodies straddling multiple cells (grid-aligned edge cases)
//  - planes pair with every dynamic body, not just ones sharing the plane body's cell
//  - AABB refine drops separated candidates but keeps every true overlap (spheres and boxes)
#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/physics_data.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::physics::CollisionShapeSoA;
using fuse::physics::CollisionShapeType;
using fuse::physics::RigidBodySoA;
using fuse::physics::broadphase::CandidatePair;
using fuse::physics::broadphase::SpatialHashParams;
using Pair = std::pair<fuse::u32, fuse::u32>;

std::vector<Pair> canonical(const std::vector<CandidatePair>& pairs) {
    std::vector<Pair> out;
    for (const CandidatePair& p : pairs) {
        out.emplace_back(std::min(p.bodyA, p.bodyB), std::max(p.bodyA, p.bodyB));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Pair> bruteForce(const RigidBodySoA& bodies, const std::vector<float>& radii) {
    std::vector<Pair> out;
    for (fuse::u32 i = 0; i < bodies.count(); ++i) {
        for (fuse::u32 j = i + 1; j < bodies.count(); ++j) {
            const auto& a = bodies.positions[i];
            const auto& b = bodies.positions[j];
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            const float r = radii[i] + radii[j];
            if (dx * dx + dy * dy + dz * dz <= r * r) {
                out.emplace_back(i, j);
            }
        }
    }
    return out;
}

/// Every true overlap must be a candidate (the broad phase may add AABB-only extras).
std::size_t missing(const std::vector<Pair>& truth, const std::vector<Pair>& candidates) {
    std::size_t miss = 0;
    for (const Pair& p : truth) {
        miss += std::binary_search(candidates.begin(), candidates.end(), p) ? 0u : 1u;
    }
    return miss;
}

void testTenThousandSpheres() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4);

    std::mt19937 rng(10'000u);
    std::uniform_real_distribution<float> pos(-60.f, 60.f);
    std::uniform_real_distribution<float> rad(0.1f, 1.5f);
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::vector<float> radii;
    for (fuse::u32 i = 0; i < 10'000u; ++i) {
        const fuse::u32 body = bodies.addBody({pos(rng), pos(rng), pos(rng)}, 1.f);
        radii.push_back(rad(rng));
        shapes.addShape(CollisionShapeType::Sphere, body, {radii.back(), 0.f, 0.f});
    }

    SpatialHashParams params{};
    params.cellSize = 3.f;
    params.tableSize = 16384;
    const std::vector<Pair> candidates = canonical(fuse::physics::broadphase::runBroadphase(bodies, shapes, params));
    const std::vector<Pair> truth = bruteForce(bodies, radii);
    const bool unique = std::adjacent_find(candidates.begin(), candidates.end()) == candidates.end();
    const std::size_t missed = missing(truth, candidates);
    std::printf("broadphase 10k spheres: %zu true overlaps, %zu candidates, %zu missed\n", truth.size(),
                candidates.size(), missed);
    expectTrue(truth.size() > 100u, "scene has many real overlaps");
    expectTrue(missed == 0u, "no overlapping pair missed vs brute force");
    expectTrue(unique, "candidate pairs are unique");

    // Grid + refine, as the pipeline runs it.
    fuse::physics::broadphase::PairBufferSoA buffer;
    const auto start = std::chrono::steady_clock::now();
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, buffer);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const std::vector<Pair> refined = canonical(buffer.toVector());
    std::printf("broadphase 10k refined: %zu pairs, %zu missed, %.2f ms\n", refined.size(), missing(truth, refined), ms);
    expectTrue(missing(truth, refined) == 0u, "refine keeps every true overlap");
    expectTrue(refined.size() < candidates.size() / 4u, "refine removes most separated cell-mates");
    scheduler.shutdown();
}

void testGridAlignedStraddlers() {
    // Spheres centred exactly on cell corners/edges/faces, touching across cell boundaries.
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::vector<float> radii;
    const float cell = 2.f;
    auto add = [&](float x, float y, float z, float r) {
        const fuse::u32 body = bodies.addBody({x, y, z}, 1.f);
        radii.push_back(r);
        shapes.addShape(CollisionShapeType::Sphere, body, {r, 0.f, 0.f});
    };
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            add(i * cell, j * cell, 0.f, 1.f);                 // on corners, touching neighbours exactly
            add(i * cell + cell * 0.5f, j * cell, 0.f, 0.25f); // on edges
        }
    }
    add(0.f, 0.f, cell, 1.f);           // straddles a face above the corner sphere
    add(-0.001f, -0.001f, -0.001f, 0.2f); // tiny, sitting across 8 cells
    add(0.f, 0.f, 0.f, 20.f);           // huge: spans many cells, overlaps everything

    SpatialHashParams params{};
    params.cellSize = cell;
    params.tableSize = 4096;
    const std::vector<Pair> candidates = canonical(fuse::physics::broadphase::runBroadphase(bodies, shapes, params));
    const std::vector<Pair> truth = bruteForce(bodies, radii);
    const std::size_t missed = missing(truth, candidates);
    std::printf("broadphase straddlers: %zu true overlaps, %zu missed\n", truth.size(), missed);
    expectTrue(missed == 0u, "grid-aligned and multi-cell bodies never miss a pair");
}

void testBoxRefine() {
    // Long thin boxes (half extents 0.2 x 3 x 0.2) overlapping only along y: a refine that
    // treated boxes as spheres of radius halfExtents.x would reject these.
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    for (int i = 0; i < 8; ++i) {
        const fuse::u32 body = bodies.addBody({0.3f * static_cast<float>(i), 5.f * static_cast<float>(i % 2), 0.f}, 1.f);
        shapes.addShape(CollisionShapeType::Box, body, {0.2f, 3.f, 0.2f});
    }
    SpatialHashParams params{};
    params.cellSize = 1.f;
    fuse::physics::broadphase::PairBufferSoA buffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, buffer);
    fuse::physics::broadphase::refineBroadphasePairsParallel(bodies, shapes, buffer);
    const std::vector<Pair> refined = canonical(buffer.toVector());
    // Neighbours along x (0.3 apart, widths 0.4) overlap; bodies at y 0 and 5 overlap in y (6 tall).
    std::size_t missed = 0;
    for (fuse::u32 i = 0; i + 1 < 8u; ++i) {
        missed += std::binary_search(refined.begin(), refined.end(), Pair{i, i + 1}) ? 0u : 1u;
    }
    std::printf("broadphase box refine: %zu pairs, %zu adjacent pairs missed\n", refined.size(), missed);
    expectTrue(missed == 0u, "box refine uses box extents, not a radius");
}

void testPlanePairsAwayFromPlaneOrigin() {
    // Planes are unbounded: a lone body far from the plane body's origin (sharing no cell
    // with anything) must still be paired with the plane.
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    const fuse::u32 plane = bodies.addBody({0.f, 0.f, 0.f}, 0.f, fuse::physics::RB_STATIC);
    shapes.addShape(CollisionShapeType::Plane, plane, {0.f, 1.f, 0.f}, 0.f);
    const fuse::u32 far = bodies.addBody({250.f, 0.5f, -40.f}, 1.f);
    shapes.addShape(CollisionShapeType::Box, far, {0.5f, 0.5f, 0.5f});
    SpatialHashParams params{};
    params.cellSize = 2.f;
    params.tableSize = 1024;
    const std::vector<Pair> pairs = canonical(fuse::physics::broadphase::runBroadphase(bodies, shapes, params));
    expectTrue(std::binary_search(pairs.begin(), pairs.end(), Pair{plane, far}),
               "body far from the plane origin is paired with the plane");
}

} // namespace

int main() {
    testTenThousandSpheres();
    testGridAlignedStraddlers();
    testBoxRefine();
    testPlanePairsAwayFromPlaneOrigin();

    if (g_failures == 0) {
        std::printf("fuse_b4_broadphase_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_broadphase_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
