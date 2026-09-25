// Gate for the physics single-source kernel ports (docs/compute-kernels.md):
//   - physics_scan / physics_broadphase_sort: exclusive scan and stable radix sort equal
//     std::exclusive_scan / std::stable_sort on CpuReference and CpuParallel (0/2/4 workers), sizes
//     with partial tiles and multi-level scans; KernelRadixSorter as SpatialHashParams::entrySorter
//     leaves the legacy broadphase output unchanged.
//   - runBroadphaseKernels == runBroadphaseIntoBuffer / runBroadphase2DIntoBuffer bit for bit (pairs,
//     order, counters) on 3D / 2D / rotated-box / plane / layer / capacity-clamp / occupancy scenes.
//   - runNarrowphaseKernels == runNarrowphaseIntoBuffer (contact SoA over activeCount + counters).
//   - PhysicsPipeline Legacy vs Kernels: identical per-frame state hashes over 60 frames.
//   - Colored solve (ConstraintSolveMode::ColoredKernel): valid colouring, CpuReference ==
//     CpuParallel (0/2/4 workers) bit for bit, repeatable, stacks stay up, close to the island path.
//   - Kernel stats names / item counts; GPU requests without a device fall back to CpuParallel.
//   - Prints CPU timings (legacy vs kernel reference vs kernel parallel) at 1000 and 10000 bodies.

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/physics/broadphase/broadphase_kernel.hpp>
#include <fuse/physics/broadphase/broadphase_kernels.hpp>
#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/narrowphase_kernels.hpp>
#include <fuse/physics/physics_pipeline.hpp>
#include <fuse/physics/rotation.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace {

using namespace fuse::physics;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
namespace bp = fuse::physics::broadphase;
namespace np = fuse::physics::narrowphase;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

fuse::jobs::JobScheduler& scheduler() { return fuse::jobs::JobScheduler::instance(); }

void setWorkers(u32 workers) {
    scheduler().shutdown();
    scheduler().initialize(workers);
}

template <typename T>
bool sameBytes(const std::vector<T>& a, const std::vector<T>& b, size_t count) {
    return a.size() >= count && b.size() >= count && (count == 0 || std::memcmp(a.data(), b.data(), count * sizeof(T)) == 0);
}

u64 fnv(u64 h, const void* data, size_t bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------------------------

struct Scene {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
};

/// Jittered 3D grid of spheres / boxes (some rotated) + a ground plane; ~spacing 0.9 so neighbours overlap.
Scene makeScene3D(u32 count, u32 seed, bool withPlane = true, bool rotated = true, bool layers = false) {
    Scene s;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> jitter(-0.15f, 0.15f);
    if (withPlane) {
        const u32 ground = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
        s.shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    }
    const u32 side = static_cast<u32>(std::ceil(std::cbrt(static_cast<double>(count))));
    for (u32 i = 0; i < count; ++i) {
        const vec3 p{static_cast<float>(i % side) * 0.9f + jitter(rng),
                     0.45f + static_cast<float>((i / side) % side) * 0.9f + jitter(rng),
                     static_cast<float>(i / (side * side)) * 0.9f + jitter(rng)};
        const u32 layer = layers ? (1u << (i % 3u)) : 1u;
        const u32 mask = layers ? (i % 5u == 0u ? 0x1u : 0xFFFFFFFFu) : 0xFFFFFFFFu;
        const u32 body = s.bodies.addBody(p, 1.f, i % 17u == 0u ? RB_STATIC : 0u, layer, mask);
        if (i % 3u == 0u) {
            s.shapes.addShape(CollisionShapeType::Box, body, {0.45f, 0.4f, 0.35f});
            if (rotated && i % 2u == 0u) {
                s.bodies.orientations[body] = quatFromAxisAngle({0.3f, 1.f, 0.2f}, 0.1f * static_cast<float>(i % 13u));
            }
        } else {
            s.shapes.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
        }
    }
    return s;
}

Scene makeScene2D(u32 count, u32 seed) {
    Scene s;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> jitter(-0.2f, 0.2f);
    for (u32 i = 0; i < count; ++i) {
        const vec3 p{static_cast<float>(i % 40u) * 0.9f + jitter(rng), static_cast<float>(i / 40u) * 0.9f + jitter(rng),
                     0.f};
        const u32 body = s.bodies.addBody(p, 1.f);
        if (i % 3u == 0u) {
            s.shapes.addShape(CollisionShapeType::Box, body, {0.45f, 0.45f, 0.f});
        } else {
            s.shapes.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
        }
    }
    return s;
}

bool samePairBuffers(const bp::PairBufferSoA& a, const bp::PairBufferSoA& b) {
    return a.activeCount == b.activeCount && a.pairSlotCount == b.pairSlotCount && a.droppedCount == b.droppedCount &&
           a.bodyA.size() == b.bodyA.size() && sameBytes(a.bodyA, b.bodyA, a.bodyA.size()) &&
           sameBytes(a.bodyB, b.bodyB, a.bodyB.size()) && sameBytes(a.validFlags, b.validFlags, a.validFlags.size());
}

bool sameContactBuffers(const np::ContactBufferSoA& a, const np::ContactBufferSoA& b) {
    const size_t n = a.activeCount;
    const size_t points = n * np::kMaxContactPointsPerManifold;
    return a.activeCount == b.activeCount && a.pairSlotCount == b.pairSlotCount && a.droppedCount == b.droppedCount &&
           sameBytes(a.contactPoints, b.contactPoints, n) && sameBytes(a.contactNormals, b.contactNormals, n) &&
           sameBytes(a.penetrationDepths, b.penetrationDepths, n) && sameBytes(a.minSeparations, b.minSeparations, n) &&
           sameBytes(a.bodyA, b.bodyA, n) && sameBytes(a.bodyB, b.bodyB, n) &&
           sameBytes(a.validFlags, b.validFlags, a.pairSlotCount) && sameBytes(a.pointCounts, b.pointCounts, a.pairSlotCount) &&
           sameBytes(a.pointSlots, b.pointSlots, points) && sameBytes(a.pointPenetrations, b.pointPenetrations, points) &&
           sameBytes(a.warmNormalImpulses, b.warmNormalImpulses, n) &&
           sameBytes(a.warmTangentImpulses, b.warmTangentImpulses, n) && sameBytes(a.tangent1, b.tangent1, n) &&
           sameBytes(a.tangent2, b.tangent2, n);
}

// ---------------------------------------------------------------------------------------------
// Scan + radix sort
// ---------------------------------------------------------------------------------------------

void testScanAndSort() {
    std::mt19937 rng(7);
    for (u32 workers : {0u, 2u, 4u}) {
        setWorkers(workers);
        for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
            bool scanOk = true;
            bp::KernelScanScratch scanScratch;
            for (u32 n : {1u, 5u, 1023u, 1024u, 1025u, 4097u, 300000u, 1100000u}) {
                std::vector<u32> data(n);
                for (u32& v : data) {
                    v = rng() % 7u;
                }
                std::vector<u32> expected(n);
                std::exclusive_scan(data.begin(), data.end(), expected.begin(), 0u);
                const u32 total = std::accumulate(data.begin(), data.end(), 0u);
                const u32 got = bp::exclusiveScanKernel(backend, data.data(), n, scanScratch);
                scanOk = scanOk && got == total && data == expected;
            }
            char label[160];
            std::snprintf(label, sizeof(label), "physics_scan == std::exclusive_scan (%s, %u workers, multi-level)",
                          kernel::backend_name(backend), workers);
            expectTrue(scanOk, label);

            bool sortOk = true;
            bp::KernelRadixSortScratch sortScratch;
            for (u32 n : {2u, 255u, 256u, 257u, 5000u, 70001u}) {
                for (u32 bits : {5u, 13u, 28u, 32u}) {
                    std::vector<u32> keys(n);
                    std::vector<u32> values(n);
                    for (u32 i = 0; i < n; ++i) {
                        keys[i] = bits == 32u ? static_cast<u32>(rng()) : static_cast<u32>(rng()) & ((1u << bits) - 1u);
                        values[i] = i;
                    }
                    std::vector<u32> order(n);
                    std::iota(order.begin(), order.end(), 0u);
                    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return keys[a] < keys[b]; });
                    std::vector<u32> k2 = keys;
                    bp::radixSortKernel(backend, keys.data(), values.data(), n, bits, sortScratch);
                    for (u32 i = 0; i < n && sortOk; ++i) {
                        sortOk = values[i] == order[i] && keys[i] == k2[order[i]];
                    }
                    // Keys-only sort.
                    bp::radixSortKernel(backend, k2.data(), nullptr, n, bits, sortScratch);
                    sortOk = sortOk && k2 == keys;
                }
            }
            std::snprintf(label, sizeof(label), "physics_broadphase_sort == std::stable_sort (%s, %u workers)",
                          kernel::backend_name(backend), workers);
            expectTrue(sortOk, label);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Broadphase / narrowphase parity
// ---------------------------------------------------------------------------------------------

struct BroadphaseCase {
    const char* name;
    Scene scene;
    bp::SpatialHashParams params;
    bool use2D;
    u32 maxCapacity;
};

void runLegacy(const BroadphaseCase& c, bp::PairBufferSoA& out, const bp::BroadphaseKeyValueSorter* sorter = nullptr) {
    bp::SpatialHashParams params = c.params;
    params.entrySorter = sorter;
    bp::BroadphaseScratch scratch;
    out.setMaxCapacity(c.maxCapacity);
    if (c.use2D) {
        bp::runBroadphase2DIntoBuffer(c.scene.bodies, c.scene.shapes, params, out, scratch);
    } else {
        bp::runBroadphaseIntoBuffer(c.scene.bodies, c.scene.shapes, params, out, scratch);
    }
}

std::vector<BroadphaseCase> broadphaseCases() {
    std::vector<BroadphaseCase> cases;
    bp::SpatialHashParams base{};
    base.cellSize = 1.f;
    base.tableSize = 4096;
    base.bodyCount = 0;

    cases.push_back({"3D spheres+boxes+plane", makeScene3D(1000, 1), base, false, 0u});
    cases.push_back({"3D layers", makeScene3D(700, 2, true, true, true), base, false, 0u});
    cases.push_back({"3D clamp", makeScene3D(600, 3), base, false, 500u});
    bp::SpatialHashParams occupancy = base;
    occupancy.maxCellOccupancy = 4u;
    occupancy.cellSize = 0.4f;
    cases.push_back({"3D occupancy budget", makeScene3D(500, 4, false), occupancy, false, 0u});
    bp::SpatialHashParams coarse = base;
    coarse.cellSize = 3.f;
    coarse.tableSize = 97u;
    cases.push_back({"3D coarse cells, small table", makeScene3D(800, 5), coarse, false, 0u});
    cases.push_back({"2D circles+boxes", makeScene2D(1000, 6), base, true, 0u});
    cases.push_back({"2D clamp", makeScene2D(400, 7), base, true, 300u});
    cases.push_back({"singleton", makeScene3D(1, 8, false), base, false, 0u});
    return cases;
}

void testBroadphaseParity() {
    const std::vector<BroadphaseCase> cases = broadphaseCases();
    for (const BroadphaseCase& c : cases) {
        setWorkers(2);
        bp::PairBufferSoA reference;
        runLegacy(c, reference);

        bp::KernelRadixSorter sorter;
        sorter.backend = kernel::Backend::CpuParallel;
        const bp::BroadphaseKeyValueSorter hook = sorter.sorter();
        bp::PairBufferSoA viaSorter;
        runLegacy(c, viaSorter, &hook);
        char label[200];
        std::snprintf(label, sizeof(label), "[%s] KernelRadixSorter entrySorter == legacy counting sort", c.name);
        expectTrue(samePairBuffers(reference, viaSorter), label);

        for (u32 workers : {0u, 2u, 4u}) {
            setWorkers(workers);
            for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
                bp::BroadphaseKernelContext context;
                context.backend = backend;
                bp::PairBufferSoA out;
                out.setMaxCapacity(c.maxCapacity);
                for (int repeat = 0; repeat < 2; ++repeat) { // second run reuses the warm scratch
                    bp::runBroadphaseKernels(c.scene.bodies, c.scene.shapes, c.params, c.use2D, out, context);
                }
                std::snprintf(label, sizeof(label), "[%s] runBroadphaseKernels == legacy (%s, %u workers, %u pairs)",
                              c.name, kernel::backend_name(backend), workers, reference.activeCount);
                expectTrue(context.stats.usedKernels && samePairBuffers(reference, out), label);
            }
        }
    }
}

void testNarrowphaseParity() {
    for (u32 maxCapacity : {0u, 300u}) {
        setWorkers(2);
        const Scene scene = makeScene3D(1000, 11);
        bp::SpatialHashParams params{};
        params.cellSize = 1.f;
        params.tableSize = 4096;
        bp::PairBufferSoA pairBuffer;
        bp::runBroadphaseIntoBuffer(scene.bodies, scene.shapes, params, pairBuffer);
        std::vector<bp::CandidatePair> pairs;
        pairBuffer.copyTo(pairs);

        np::ContactBufferSoA reference;
        reference.setMaxCapacity(maxCapacity);
        np::runNarrowphaseIntoBuffer(pairs, scene.bodies, scene.shapes, reference);
        expectTrue(reference.activeCount > 100u, "narrowphase scene produces contacts");

        for (u32 workers : {0u, 2u, 4u}) {
            setWorkers(workers);
            for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
                np::NarrowphaseKernelContext context;
                context.backend = backend;
                np::ContactBufferSoA out;
                out.setMaxCapacity(maxCapacity);
                np::runNarrowphaseKernels(pairs, scene.bodies, scene.shapes, out, context);
                np::runNarrowphaseKernels(pairs, scene.bodies, scene.shapes, out, context);
                char label[200];
                std::snprintf(label, sizeof(label),
                              "runNarrowphaseKernels == runNarrowphaseIntoBuffer (%s, %u workers, clamp %u, %u contacts)",
                              kernel::backend_name(backend), workers, maxCapacity, reference.activeCount);
                expectTrue(sameContactBuffers(reference, out), label);
            }
        }
        np::NarrowphaseKernelContext context;
        np::ContactBufferSoA empty;
        np::runNarrowphaseKernels({}, scene.bodies, scene.shapes, empty, context);
        expectTrue(empty.activeCount == 0u && empty.pairSlotCount == 0u, "no pairs -> empty contact buffer");
    }
}

u64 pipelineHash(const PhysicsPipeline& p) {
    u64 h = 1469598103934665603ull;
    const RigidBodySoA& b = p.bodies();
    h = fnv(h, b.positions.data(), b.positions.size() * sizeof(vec3));
    h = fnv(h, b.linearVelocities.data(), b.linearVelocities.size() * sizeof(vec3));
    for (const bp::CandidatePair& pair : p.candidatePairs()) {
        h = fnv(h, &pair, sizeof(pair));
    }
    for (const np::ContactManifold& m : p.contacts()) {
        h = fnv(h, &m.contactNormal, sizeof(m.contactNormal));
        h = fnv(h, &m.bodyA, sizeof(u32) * 3u);
        h = fnv(h, &m.penetrationDepth, sizeof(m.penetrationDepth));
    }
    return h;
}

void fillPipeline(PhysicsPipeline& p, u32 count, bool twoD) {
    if (!twoD) {
        p.addStaticPlane({0.f, 1.f, 0.f}, 0.f);
    }
    const u32 side = twoD ? 40u : static_cast<u32>(std::ceil(std::cbrt(static_cast<double>(count))));
    for (u32 i = 0; i < count; ++i) {
        const vec3 pos = twoD ? vec3{static_cast<float>(i % side) * 0.9f, static_cast<float>(i / side) * 0.9f, 0.f}
                              : vec3{static_cast<float>(i % side) * 0.9f, 1.f + static_cast<float>((i / side) % side) * 0.9f,
                                     static_cast<float>(i / (side * side)) * 0.9f};
        if (i % 4u == 0u) {
            p.addBoxBody(pos, {0.45f, 0.45f, twoD ? 0.f : 0.45f});
        } else {
            p.addSphereBody(pos, 0.5f);
        }
    }
}

void testPipelineParity() {
    setWorkers(2);
    for (bool twoD : {false, true}) {
        PhysicsPipelineDesc legacyDesc{};
        legacyDesc.broadphaseMode = twoD ? BroadphaseMode::SpatialHash2D : BroadphaseMode::SpatialHash3D;
        PhysicsPipelineDesc kernelDesc = legacyDesc;
        kernelDesc.computePath = PhysicsComputePath::Kernels;
        PhysicsPipeline legacy;
        PhysicsPipeline kernels;
        legacy.init(legacyDesc);
        kernels.init(kernelDesc);
        fillPipeline(legacy, 1000, twoD);
        fillPipeline(kernels, 1000, twoD);
        bool same = true;
        u32 frames = 0;
        for (; frames < 60 && same; ++frames) {
            legacy.step(1.f / 60.f);
            kernels.step(1.f / 60.f);
            same = pipelineHash(legacy) == pipelineHash(kernels) &&
                   samePairBuffers(legacy.pairBuffer(), kernels.pairBuffer()) &&
                   sameContactBuffers(legacy.contactBuffer(), kernels.contactBuffer());
        }
        char label[200];
        std::snprintf(label, sizeof(label),
                      "PhysicsPipeline %s Kernels == Legacy state hash every frame (%u frames, %zu pairs, %u contacts)",
                      twoD ? "2D" : "3D", frames, legacy.candidatePairs().size(), legacy.contactCount());
        expectTrue(same && legacy.candidatePairs().size() > 1000u, label);
    }
}

// ---------------------------------------------------------------------------------------------
// Stats + fallback
// ---------------------------------------------------------------------------------------------

void testStatsAndFallback() {
    setWorkers(2);
    const Scene scene = makeScene3D(300, 21);
    bp::SpatialHashParams params{};
    params.cellSize = 1.f;
    params.tableSize = 1024;
    bp::PairBufferSoA reference;
    bp::runBroadphaseIntoBuffer(scene.bodies, scene.shapes, params, reference);

    kernel::reset_kernel_stats();
    bp::BroadphaseKernelContext context;
    context.backend = kernel::Backend::CpuParallel;
    bp::PairBufferSoA out;
    bp::runBroadphaseKernels(scene.bodies, scene.shapes, params, false, out, context);
    namespace bk = fuse::physics::broadphase_kernel;
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(bk::kKeysName, stats) && stats.launches == 1u &&
                   stats.items == scene.shapes.count() && stats.last_backend == kernel::Backend::CpuParallel,
               "physics_broadphase_keys stats: one launch, one item per shape");
    expectTrue(kernel::find_kernel_stats(bk::kCountName, stats) && stats.items == scene.shapes.count(),
               "physics_broadphase_count stats: one item per shape");
    expectTrue(kernel::find_kernel_stats(bk::kSortName, stats) && stats.launches >= 2u &&
                   stats.items >= 2u * kernel::div_up(context.stats.entries, bk::kSortTile) * bk::kSortThreads,
               "physics_broadphase_sort stats: >= 2 passes, 256 threads per 1024-entry tile");
    expectTrue(kernel::find_kernel_stats(bk::kCellsName, stats) && stats.items == context.stats.cells,
               "physics_broadphase_cells stats: one item per occupied cell");
    expectTrue(kernel::find_kernel_stats(bk::kPairsName, stats) && stats.items == context.stats.cells,
               "physics_broadphase_pairs stats: one item per occupied cell");
    expectTrue(kernel::find_kernel_stats(bk::kScanName, stats) && stats.launches > 0u, "physics_scan launched");

    std::vector<bp::CandidatePair> pairs;
    out.copyTo(pairs);
    np::NarrowphaseKernelContext npContext;
    np::ContactBufferSoA contacts;
    np::runNarrowphaseKernels(pairs, scene.bodies, scene.shapes, contacts, npContext);
    expectTrue(kernel::find_kernel_stats(np::kNarrowphaseKernelName, stats) && stats.items == pairs.size(),
               "physics_narrowphase stats: one item per candidate pair");
    expectTrue(kernel::find_kernel_stats(np::kNarrowphaseWriteKernelName, stats) && stats.items == pairs.size(),
               "physics_narrowphase_write stats: one item per candidate pair");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            bp::BroadphaseKernelContext gpuContext;
            gpuContext.backend = gpu;
            bp::PairBufferSoA gpuOut;
            bp::runBroadphaseKernels(scene.bodies, scene.shapes, params, false, gpuOut, gpuContext);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "broadphase GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(samePairBuffers(reference, gpuOut), "fallback broadphase == legacy");

            np::NarrowphaseKernelContext gpuNp;
            gpuNp.backend = gpu;
            np::ContactBufferSoA gpuContacts;
            np::runNarrowphaseKernels(pairs, scene.bodies, scene.shapes, gpuContacts, gpuNp);
            const kernel::LaunchRecord npLast = kernel::last_launch();
            expectTrue(npLast.ok && npLast.requested == gpu && npLast.backend == kernel::Backend::CpuParallel,
                       "narrowphase GPU request falls back to CpuParallel (recorded)");
            expectTrue(sameContactBuffers(contacts, gpuContacts), "fallback narrowphase == CpuParallel");
        }
    } else {
        bp::KernelRadixSorter sorter;
        sorter.backend = kernel::Backend::Cuda;
        const bp::BroadphaseKeyValueSorter hook = sorter.sorter();
        bp::SpatialHashParams withSorter = params;
        withSorter.entrySorter = &hook;
        bp::PairBufferSoA cudaOut;
        bp::runBroadphaseIntoBuffer(scene.bodies, scene.shapes, withSorter, cudaOut);
        expectTrue(samePairBuffers(reference, cudaOut), "CUDA radix sort entrySorter == legacy (integer: exact)");
    }
}

// ---------------------------------------------------------------------------------------------
// Colored solver
// ---------------------------------------------------------------------------------------------

struct SolverScene {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::vector<u32> tops;
};

/// Columns of stacked boxes and spheres on a ground plane (+ a few distance constraints).
SolverScene makeSolverScene(u32 columnsPerSide, u32 height) {
    SolverScene s;
    const u32 ground = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    s.shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    for (u32 cx = 0; cx < columnsPerSide; ++cx) {
        for (u32 cz = 0; cz < columnsPerSide; ++cz) {
            // Box columns stack; sphere "columns" are a single sphere resting on the ground (stacked
            // spheres are unstable and would roll off in either solver).
            const bool boxes = ((cx + cz) % 2u) == 0u;
            const u32 levels = boxes ? height : 1u;
            for (u32 level = 0; level < levels; ++level) {
                const vec3 p{static_cast<float>(cx) * 1.5f, 0.5f + static_cast<float>(level) * 1.001f,
                             static_cast<float>(cz) * 1.5f};
                const u32 body = s.bodies.addBody(p, 1.f);
                if (boxes) {
                    s.shapes.addShape(CollisionShapeType::Box, body, {0.5f, 0.5f, 0.5f});
                } else {
                    s.shapes.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
                }
                if (boxes && level + 1u == height) {
                    s.tops.push_back(body);
                }
            }
        }
    }
    return s;
}

SolverParams solverParams(ConstraintSolveMode mode, kernel::Backend backend) {
    SolverParams params;
    params.substeps = 4;
    params.iterations = 8;
    params.broadphase.cellSize = 2.f;
    params.broadphase.tableSize = 4096;
    params.solveMode = mode;
    params.kernelBackend = backend;
    params.enableCcd = false;
    return params;
}

u64 bodyHash(const RigidBodySoA& b) {
    u64 h = 1469598103934665603ull;
    h = fnv(h, b.positions.data(), b.positions.size() * sizeof(vec3));
    h = fnv(h, b.orientations.data(), b.orientations.size() * sizeof(quat));
    h = fnv(h, b.linearVelocities.data(), b.linearVelocities.size() * sizeof(vec3));
    h = fnv(h, b.angularVelocities.data(), b.angularVelocities.size() * sizeof(vec3));
    h = fnv(h, b.flags.data(), b.flags.size() * sizeof(u32));
    return h;
}

struct SolveRun {
    u64 hash = 0;
    SolverScene scene;
    bool coloringValid = true;
    u32 colors = 0;
    u32 constraints = 0;
};

SolveRun runSolver(ConstraintSolveMode mode, kernel::Backend backend, u32 frames, u32 columns = 8, u32 height = 6) {
    SolveRun run;
    run.scene = makeSolverScene(columns, height);
    std::vector<DistanceConstraint> links;
    // Soft links between neighbouring boxes of the same column (already at rest length).
    for (u32 i = 1; i + 1u < run.scene.bodies.count(); i += 13u) {
        DistanceConstraint link{};
        link.bodyA = i;
        link.bodyB = i + 1u;
        const vec3 d = run.scene.bodies.positions[i] - run.scene.bodies.positions[i + 1u];
        link.restLength = d.length();
        link.compliance = 1e-4f;
        links.push_back(link);
    }
    PBDSolver solver;
    solver.init(run.scene.bodies.count(), 8192, static_cast<u32>(links.size()));
    solver.setDistanceConstraints(links);
    const SolverParams params = solverParams(mode, backend);
    for (u32 f = 0; f < frames; ++f) {
        solver.step(run.scene.bodies, run.scene.shapes, params, 1.f / 60.f);
        if (mode == ConstraintSolveMode::ColoredKernel) {
            // Every colour: no dynamic body written by two constraints.
            const ConstraintColoring& coloring = solver.constraintColoring();
            const std::vector<np::ContactManifold>& contacts = solver.workBuffers().contactManifolds();
            std::vector<u32> seen(run.scene.bodies.count(), ~0u);
            for (u32 c = 0; c < coloring.colorCount; ++c) {
                for (u32 i = 0; i < coloring.colorSize(c); ++i) {
                    const u32 ref = coloring.colorItems(c)[i];
                    u32 a = 0;
                    u32 b = 0;
                    if ((ref & ConstraintColoring::kDistanceBit) != 0u) {
                        a = links[ref & ~ConstraintColoring::kDistanceBit].bodyA;
                        b = links[ref & ~ConstraintColoring::kDistanceBit].bodyB;
                    } else {
                        a = contacts[ref].bodyA;
                        b = contacts[ref].bodyB;
                    }
                    for (u32 body : {a, b}) {
                        if (!solverBodyIsDynamic(run.scene.bodies, body)) {
                            continue;
                        }
                        run.coloringValid = run.coloringValid && seen[body] != c;
                        seen[body] = c;
                    }
                }
            }
            run.colors = std::max(run.colors, coloring.colorCount);
            run.constraints = std::max(run.constraints, coloring.constraintCount());
        }
    }
    run.hash = bodyHash(run.scene.bodies);
    return run;
}

void testColoredSolver() {
    constexpr u32 kFrames = 90;
    setWorkers(2);
    const SolveRun reference = runSolver(ConstraintSolveMode::ColoredKernel, kernel::Backend::CpuReference, kFrames);
    expectTrue(reference.coloringValid, "colouring: no dynamic body shared inside a colour");
    expectTrue(reference.colors >= 2u && reference.colors < ConstraintColoring::kMaxColors && reference.constraints > 100u,
               "colouring uses several colours for the stacked scene");
    std::printf("colored solver: %u colours for up to %u constraints\n", reference.colors, reference.constraints);

    for (u32 workers : {0u, 2u, 4u}) {
        setWorkers(workers);
        const SolveRun parallel = runSolver(ConstraintSolveMode::ColoredKernel, kernel::Backend::CpuParallel, kFrames);
        char label[160];
        std::snprintf(label, sizeof(label), "colored solve CpuParallel == CpuReference bit for bit (%u workers)", workers);
        expectTrue(parallel.hash == reference.hash, label);
    }
    setWorkers(2);
    const SolveRun again = runSolver(ConstraintSolveMode::ColoredKernel, kernel::Backend::CpuParallel, kFrames);
    expectTrue(again.hash == reference.hash, "colored solve is repeatable (same hash on a second run)");

    kernel::reset_kernel_stats();
    (void)runSolver(ConstraintSolveMode::ColoredKernel, kernel::Backend::Cuda, 2);
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(kSolveColorKernelName, stats) && stats.launches > 0u,
               "physics_solve_color launches recorded");
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        expectTrue(stats.launches_by_backend[static_cast<u32>(kernel::Backend::CpuParallel)] == stats.launches,
                   "colored solve GPU request falls back to CpuParallel");
    }

    // Physical sanity against the default island solver: stacks stay up, no NaN, similar heights.
    const SolveRun island = runSolver(ConstraintSolveMode::IslandGaussSeidel, kernel::Backend::CpuParallel, kFrames);
    bool finite = true;
    f32 maxTopDelta = 0.f;
    f32 maxTopDrop = 0.f;
    for (u32 i = 0; i < reference.scene.tops.size(); ++i) {
        const u32 body = reference.scene.tops[i];
        const vec3 p = reference.scene.bodies.positions[body];
        finite = finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        maxTopDelta = std::max(maxTopDelta, std::fabs(p.y - island.scene.bodies.positions[body].y));
        maxTopDrop = std::max(maxTopDrop, (0.5f + 5.f * 1.001f) - p.y);
    }
    std::printf("colored solver vs island: max top-body height difference %.4f m, max top drop %.4f m\n", maxTopDelta,
                maxTopDrop);
    expectTrue(finite, "colored solve stays finite");
    expectTrue(maxTopDrop < 0.1f, "colored solve: stacks stay standing (top body within 0.1 m of rest height)");
    expectTrue(maxTopDelta < 0.05f, "colored solve rest heights match the island solver within 5 cm");
    expectTrue(island.hash != 0u, "island solver ran");
}

// ---------------------------------------------------------------------------------------------
// Timings
// ---------------------------------------------------------------------------------------------

template <typename Fn>
double medianUs(u32 runs, Fn&& fn) {
    std::vector<double> t;
    fn(); // warm-up (scratch growth)
    for (u32 i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2u];
}

void printTimings() {
    fuse::core::shutdown();
    fuse::core::initialize();
    std::printf("timings (%u job workers; median us):\n", scheduler().workerCount());
    for (u32 count : {1000u, 10000u}) {
        const Scene scene = makeScene3D(count, 99);
        bp::SpatialHashParams params{};
        params.cellSize = 2.f;
        params.tableSize = count * 2u;
        bp::PairBufferSoA pairs;
        bp::BroadphaseScratch scratch;
        const double legacyBp = medianUs(15, [&] { bp::runBroadphaseIntoBuffer(scene.bodies, scene.shapes, params, pairs, scratch); });
        double kernelBp[2]{};
        for (u32 i = 0; i < 2; ++i) {
            bp::BroadphaseKernelContext context;
            context.backend = i == 0 ? kernel::Backend::CpuReference : kernel::Backend::CpuParallel;
            bp::PairBufferSoA out;
            kernel::reset_kernel_stats();
            kernelBp[i] = medianUs(15, [&] { bp::runBroadphaseKernels(scene.bodies, scene.shapes, params, false, out, context); });
            if (i == 1) {
                std::printf("  %5u bodies  broadphase kernels (CpuParallel, per call):", count);
                for (u32 k = 0; k < kernel::kernel_stats_count(); ++k) {
                    kernel::KernelStats ks{};
                    if (kernel::kernel_stats_at(k, ks) && ks.launches > 0u) {
                        std::printf(" %s %.0f", ks.name, static_cast<double>(ks.total_ns) / 16e3);
                    }
                }
                std::printf(" us (%u entries, %u cells, %u cell pairs)\n", context.stats.entries, context.stats.cells,
                            context.stats.cellPairs);
            }
        }
        std::vector<bp::CandidatePair> candidates;
        pairs.copyTo(candidates);
        np::ContactBufferSoA contacts;
        contacts.reserve(static_cast<u32>(candidates.size()));
        const double legacyNp = medianUs(15, [&] { np::runNarrowphaseIntoBuffer(candidates, scene.bodies, scene.shapes, contacts); });
        double kernelNp[2]{};
        for (u32 i = 0; i < 2; ++i) {
            np::NarrowphaseKernelContext context;
            context.backend = i == 0 ? kernel::Backend::CpuReference : kernel::Backend::CpuParallel;
            np::ContactBufferSoA out;
            kernelNp[i] = medianUs(15, [&] { np::runNarrowphaseKernels(candidates, scene.bodies, scene.shapes, out, context); });
        }
        std::printf("  %5u bodies  broadphase: legacy %8.1f | kernels ref %8.1f | kernels par %8.1f   (%u pairs)\n", count,
                    legacyBp, kernelBp[0], kernelBp[1], pairs.activeCount);
        std::printf("  %5u bodies  narrowphase: legacy %8.1f | kernels ref %8.1f | kernels par %8.1f   (%u contacts)\n",
                    count, legacyNp, kernelNp[0], kernelNp[1], contacts.activeCount);

        // Solver: one PBDSolver::step (4 substeps x 8 iterations) on stacked columns.
        const u32 height = 10u;
        const u32 columns = static_cast<u32>(std::sqrt(static_cast<double>(count / height)));
        double solve[3]{};
        const ConstraintSolveMode modes[3] = {ConstraintSolveMode::IslandGaussSeidel, ConstraintSolveMode::ColoredKernel,
                                              ConstraintSolveMode::ColoredKernel};
        const kernel::Backend backends[3] = {kernel::Backend::CpuParallel, kernel::Backend::CpuReference,
                                             kernel::Backend::CpuParallel};
        for (u32 i = 0; i < 3; ++i) {
            SolverScene s = makeSolverScene(columns, height);
            PBDSolver solver;
            solver.init(s.bodies.count(), 65536, 0);
            SolverParams params = solverParams(modes[i], backends[i]);
            params.sleepTimeRequired = 1e9f; // keep every body awake: steady per-step cost
            for (u32 f = 0; f < 10; ++f) {
                solver.step(s.bodies, s.shapes, params, 1.f / 60.f);
            }
            solve[i] = medianUs(9, [&] { solver.step(s.bodies, s.shapes, params, 1.f / 60.f); });
        }
        std::printf("  %5u bodies  solver step: island %8.1f | colored ref %8.1f | colored par %8.1f   (%u bodies)\n",
                    count, solve[0], solve[1], solve[2], columns * columns * height);
        if (kernel::backend_available(kernel::Backend::Cuda)) {
            bp::BroadphaseKernelContext cudaCtx;
            cudaCtx.backend = kernel::Backend::Cuda;
            bp::PairBufferSoA cudaOut;
            const double cudaBp = medianUs(7, [&] {
                bp::runBroadphaseKernels(scene.bodies, scene.shapes, params, false, cudaOut, cudaCtx);
            });
            std::vector<bp::CandidatePair> cudaPairs;
            cudaOut.copyTo(cudaPairs);
            np::NarrowphaseKernelContext cudaNp;
            cudaNp.backend = kernel::Backend::Cuda;
            np::ContactBufferSoA cudaContacts;
            const double cudaNpUs = medianUs(7, [&] {
                np::runNarrowphaseKernels(cudaPairs, scene.bodies, scene.shapes, cudaContacts, cudaNp);
            });
            SolverScene cudaScene = makeSolverScene(columns, height);
            PBDSolver cudaSolver;
            cudaSolver.init(cudaScene.bodies.count(), 65536, 0);
            SolverParams cudaParams = solverParams(ConstraintSolveMode::ColoredKernel, kernel::Backend::Cuda);
            cudaParams.sleepTimeRequired = 1e9f;
            for (u32 f = 0; f < 4; ++f) {
                cudaSolver.step(cudaScene.bodies, cudaScene.shapes, cudaParams, 1.f / 60.f);
            }
            const double cudaSolve = medianUs(5, [&] {
                cudaSolver.step(cudaScene.bodies, cudaScene.shapes, cudaParams, 1.f / 60.f);
            });
            std::printf("  %5u bodies  CUDA wall us: broadphase %8.1f | narrowphase %8.1f (%u contacts) | solver step %8.1f\n",
                        count, cudaBp, cudaNpUs, cudaContacts.activeCount, cudaSolve);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();
    testScanAndSort();
    testBroadphaseParity();
    testNarrowphaseParity();
    testPipelineParity();
    testStatsAndFallback();
    testColoredSolver();
    const bool timings = argc < 2 || std::strcmp(argv[1], "--no-timings") != 0;
    if (timings) {
        printTimings();
    }
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_physics_kernel_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_physics_kernel_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
