// Gate for the single-source SVO ray cast kernel (fuse/scene/svo_ray_kernel.hpp, "svo_ray_cast"):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical hit records for a ray batch
//     not a multiple of the 64-ray workgroup, over an SVO holding every brick form (sparse, masked,
//     dense with an sdf plane, uniform, emptied) plus axis-parallel and degenerate rays.
//   - The batched kernel equals the scalar SVO::rayCast (one implementation) and a brute-force
//     slab test over every solid voxel.
//   - Launches record "svo_ray_cast" stats; Cuda / Auto / VulkanCompute without a device fall back to
//     CpuParallel with the same records; invalid requests are rejected.
//   - Prints the 1M-ray CPU timing (reference vs parallel) for the "SVO 1M rays" row.

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
#include <vector>

namespace {

using fuse::f32;
using fuse::s32;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
using fuse::scene::ivec3;
using fuse::scene::SVO;
using fuse::scene::SVODesc;
using fuse::scene::SvoRay;
using fuse::scene::SvoRayHit;
using fuse::scene::vec3;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// Depth-8 SVO over [-10, 90]^3-ish with every brick payload form.
void buildScene(SVO& svo) {
    SVODesc desc{};
    desc.rootSize = 100.f;
    desc.maxDepth = 8;
    desc.origin = vec3{-10.f, 3.f, -50.f};
    svo.init(desc);
    svo.fill({0, 0, 0}, {255, 40, 255}, 2u);     // uniform bricks + masked edges
    svo.fill({30, 41, 30}, {37, 60, 37}, 3u);    // a pillar
    svo.fill({100, 0, 100}, {140, 30, 140}, 0u); // emptied region (empty uniform / masked bricks)
    std::mt19937 rng(7u);
    std::uniform_int_distribution<int> c(0, 255);
    for (int i = 0; i < 20000; ++i) {
        svo.set({c(rng), c(rng), c(rng)}, 1u + static_cast<u32>(i % 3)); // sparse + dense (mixed)
    }
    svo.carve(vec3{20.f, 18.f, 0.f}, 9.f); // dense bricks with sdf planes
    svo.carve(vec3{60.f, 10.f, -20.f}, 6.f);
}

std::vector<SvoRay> makeRays(u32 count, u32 seed, f32 lo, f32 hi) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> pos(lo, hi);
    std::uniform_real_distribution<f32> unit(-1.f, 1.f);
    std::uniform_int_distribution<int> kind(0, 9);
    std::vector<SvoRay> rays(count);
    for (u32 i = 0; i < count; ++i) {
        SvoRay& r = rays[i];
        r = SvoRay{{pos(rng), pos(rng), pos(rng)}, {unit(rng), unit(rng), unit(rng)}, (i & 1u) ? 1e9f : 90.f};
        switch (kind(rng)) {
        case 0: r.dir[0] = 0.f; break;                  // axis-parallel planes
        case 1: r.dir[1] = r.dir[2] = 0.f; break;       // axis-aligned
        case 2: r.dir[0] = r.dir[1] = r.dir[2] = 0.f; break; // degenerate
        default: break;
        }
    }
    return rays;
}

bool sameHits(const std::vector<SvoRayHit>& a, const std::vector<SvoRayHit>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(SvoRayHit)) == 0;
}

std::vector<SvoRayHit> cast(const SVO& svo, kernel::Backend backend, const std::vector<SvoRay>& rays) {
    SvoRayHit poison{};
    poison.hit = 0xCDCDCDCDu; // every record must be written
    poison.distance = -7.f;
    std::vector<SvoRayHit> hits(rays.size(), poison);
    expectTrue(svo.rayCastBatch(backend, rays.data(), hits.data(), static_cast<u32>(rays.size())),
               "rayCastBatch succeeds");
    return hits;
}

void testBackendParity() {
    SVO svo;
    buildScene(svo);
    constexpr u32 kRays = 100'003; // not a multiple of the 64-ray workgroup
    const std::vector<SvoRay> rays = makeRays(kRays, 11u, -30.f, 110.f);

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const std::vector<SvoRayHit> reference = cast(svo, kernel::Backend::CpuReference, rays);
    u32 hits = 0;
    for (const SvoRayHit& h : reference) {
        hits += h.hit;
    }
    std::printf("svo_ray_cast parity: %u rays, %u hits\n", kRays, hits);
    expectTrue(hits > kRays / 10u && hits < kRays - kRays / 10u, "ray batch has both hits and misses");

    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        const std::vector<SvoRayHit> parallel = cast(svo, kernel::Backend::CpuParallel, rays);
        char label[128];
        std::snprintf(label, sizeof(label), "CpuReference == CpuParallel bit-exact hit records (%u workers)", workers);
        expectTrue(sameHits(reference, parallel), label);
    }

    // Batched kernel == scalar SVO::rayCast, bit for bit (same function, one implementation).
    u32 scalarMismatch = 0;
    for (u32 i = 0; i < kRays; ++i) {
        const SvoRay& r = rays[i];
        ivec3 voxel{};
        vec3 normal{};
        f32 t = 0.f;
        const bool hit = svo.rayCast(vec3{r.origin[0], r.origin[1], r.origin[2]}, vec3{r.dir[0], r.dir[1], r.dir[2]},
                                     r.max_distance, voxel, normal, t);
        SvoRayHit expected{};
        if (hit) {
            expected = SvoRayHit{{voxel.x, voxel.y, voxel.z}, {normal.x, normal.y, normal.z}, t, 1u};
        }
        scalarMismatch += std::memcmp(&expected, &reference[i], sizeof(SvoRayHit)) != 0 ? 1u : 0u;
    }
    expectTrue(scalarMismatch == 0u, "batched svo_ray_cast == scalar SVO::rayCast");
    scheduler.shutdown();
}

/// Brute force over every solid voxel's AABB (slab test) — the b3 gate's reference, on a small SVO.
void testBruteForce() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 32.f;
    desc.maxDepth = 5;
    svo.init(desc);
    std::mt19937 rng(5u);
    std::uniform_int_distribution<int> coord(0, 31);
    std::vector<ivec3> solid;
    for (int i = 0; i < 600; ++i) {
        const ivec3 c{coord(rng), coord(rng), coord(rng)};
        if (svo.get(c) == 0u) {
            svo.set(c, 1u);
            solid.push_back(c);
        }
    }
    std::uniform_real_distribution<f32> pos(-10.f, 42.f);
    std::uniform_real_distribution<f32> unit(-1.f, 1.f);
    std::vector<SvoRay> rays;
    for (int r = 0; r < 2000; ++r) {
        vec3 d{unit(rng), unit(rng), unit(rng)};
        if (d.length() < 1e-3f) {
            continue;
        }
        d = d.normalized();
        rays.push_back(SvoRay{{pos(rng), pos(rng), pos(rng)}, {d.x, d.y, d.z}, 100.f});
    }
    fuse::jobs::JobScheduler::instance().initialize(2);
    const std::vector<SvoRayHit> hits = cast(svo, kernel::Backend::CpuParallel, rays);
    fuse::jobs::JobScheduler::instance().shutdown();
    int mismatches = 0;
    for (fuse::usize i = 0; i < rays.size(); ++i) {
        const SvoRay& r = rays[i];
        f32 best = std::numeric_limits<f32>::max();
        for (const ivec3& v : solid) {
            const f32 mn[3] = {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
            f32 t0 = 0.f;
            f32 t1 = std::numeric_limits<f32>::max();
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                f32 ta = (mn[a] - r.origin[a]) / r.dir[a];
                f32 tb = (mn[a] + 1.f - r.origin[a]) / r.dir[a];
                if (ta > tb) {
                    std::swap(ta, tb);
                }
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                miss = t0 > t1;
            }
            if (!miss && t0 <= r.max_distance) {
                best = std::min(best, t0);
            }
        }
        const bool expected = best != std::numeric_limits<f32>::max();
        if ((hits[i].hit != 0u) != expected || (expected && std::fabs(hits[i].distance - best) > 1e-3f)) {
            ++mismatches;
        }
    }
    expectTrue(mismatches == 0, "svo_ray_cast kernel matches brute-force traversal");
}

void testStatsFallbackAndInvalid() {
    SVO svo;
    buildScene(svo);
    const std::vector<SvoRay> rays = makeRays(1000u, 3u, -30.f, 110.f);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.initialize(2);
    kernel::reset_kernel_stats();
    const std::vector<SvoRayHit> reference = cast(svo, kernel::Backend::CpuReference, rays);
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(fuse::scene::svo_kernel::kName, stats) && stats.launches == 1u &&
                   stats.items == 1000u && stats.workgroups == 16u &&
                   stats.last_backend == kernel::Backend::CpuReference,
               "launch records svo_ray_cast stats (items, 64-ray workgroups, backend)");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            const std::vector<SvoRayHit> fallback = cast(svo, gpu, rays);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(sameHits(reference, fallback), "fallback hits == CpuReference hits");
        }
    } else {
        // Device present: the DDA is plain IEEE arithmetic (no transcendental ulp drift) — exact.
        // nvcc contracts some DDA divisions into fma, so hit distance can differ by ~1 ulp.
        // Voxel, face normal and hit/miss stay exact.
        const std::vector<SvoRayHit> gpu = cast(svo, kernel::Backend::Cuda, rays);
        bool cudaOk = gpu.size() == reference.size();
        for (u32 i = 0; cudaOk && i < reference.size(); ++i) {
            cudaOk = gpu[i].hit == reference[i].hit;
            if (!cudaOk || gpu[i].hit == 0u) {
                continue;
            }
            cudaOk = std::memcmp(gpu[i].voxel, reference[i].voxel, sizeof(gpu[i].voxel)) == 0 &&
                     std::memcmp(gpu[i].normal, reference[i].normal, sizeof(gpu[i].normal)) == 0 &&
                     std::fabs(gpu[i].distance - reference[i].distance) <=
                         1e-5f * std::max(1.f, std::fabs(reference[i].distance));
        }
        expectTrue(cudaOk, "CUDA hits match CpuReference (distance within 1e-5 relative)");
    }

    std::vector<SvoRayHit> hits(rays.size());
    for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel, kernel::Backend::Cuda}) {
        expectTrue(!svo.rayCastBatch(b, nullptr, hits.data(), 4u), "null rays rejected");
        expectTrue(!svo.rayCastBatch(b, rays.data(), nullptr, 4u), "null hits rejected");
    }
    expectTrue(svo.rayCastBatch(kernel::Backend::CpuParallel, nullptr, nullptr, 0u), "empty batch is a no-op");

    SVO empty;
    std::vector<SvoRayHit> emptyHits(rays.size());
    expectTrue(empty.rayCastBatch(kernel::Backend::CpuParallel, rays.data(), emptyHits.data(), 1000u) &&
                   std::all_of(emptyHits.begin(), emptyHits.end(), [](const SvoRayHit& h) { return h.hit == 0u; }),
               "uninitialised SVO: every ray misses");
    scheduler.shutdown();
}

/// "SVO 1M rays" row: CPU timing of the same kernel (the < 10 ms target is for CUDA on an RTX 3090).
void timeMillionRays() {
    SVO svo;
    SVODesc desc{};
    desc.rootSize = 1024.f;
    desc.maxDepth = 10;
    svo.init(desc);
    std::mt19937 rng(99u);
    std::uniform_int_distribution<int> c(0, 1023);
    for (int i = 0; i < 300000; ++i) {
        svo.set({c(rng), c(rng), c(rng)}, 1u);
    }
    svo.fill({0, 0, 0}, {1023, 63, 1023}, 2u); // ground slab
    std::vector<SvoRay> rays(1u << 20u);
    std::uniform_real_distribution<f32> unit(-1.f, 1.f);
    std::uniform_real_distribution<f32> pos(0.f, 1024.f);
    std::uniform_real_distribution<f32> down(-1.f, -0.25f);
    for (SvoRay& r : rays) { // camera-like: from above the volume, looking down through it at the slab
        r = SvoRay{{pos(rng), 1100.f, pos(rng)}, {unit(rng), down(rng), unit(rng)}, 4000.f};
    }
    std::vector<SvoRayHit> a(rays.size());
    std::vector<SvoRayHit> b(rays.size());
    const u32 n = static_cast<u32>(rays.size());
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    const u32 workers = std::max(1u, std::thread::hardware_concurrency()) - 1u;
    scheduler.initialize(workers);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    expectTrue(svo.rayCastBatch(kernel::Backend::CpuReference, rays.data(), a.data(), n), "1M reference");
    const auto t1 = clock::now();
    expectTrue(svo.rayCastBatch(kernel::Backend::CpuParallel, rays.data(), b.data(), n), "1M parallel");
    const auto t2 = clock::now();
    scheduler.shutdown();
    expectTrue(sameHits(a, b), "1M rays: reference == parallel");
    u32 hits = 0;
    for (const SvoRayHit& h : a) {
        hits += h.hit;
    }
    const double refMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double parMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("svo_ray_cast 1M rays (%u hits): CpuReference %.1f ms, CpuParallel (%u workers) %.1f ms (%.2fx)\n",
                hits, refMs, workers, parMs, refMs / std::max(parMs, 1e-3));
    if (kernel::backend_available(kernel::Backend::Cuda)) {
        expectTrue(svo.rayCastBatch(kernel::Backend::Cuda, rays.data(), a.data(), n), "1M cuda warmup");
        kernel::reset_kernel_stats();
        const auto c0 = clock::now();
        expectTrue(svo.rayCastBatch(kernel::Backend::Cuda, rays.data(), b.data(), n), "1M cuda");
        const auto c1 = clock::now();
        kernel::KernelStats ks{};
        const bool have = kernel::find_kernel_stats(fuse::scene::svo_kernel::kName, ks);
        const double wallMs = std::chrono::duration<double, std::milli>(c1 - c0).count();
        std::printf("svo_ray_cast 1M rays CUDA: wall %.2f ms (upload+launch+download), kernel last %.2f ms\n",
                    wallMs, have ? static_cast<double>(ks.last_ns) / 1e6 : -1.0);
        expectTrue(sameHits(a, b), "1M rays: CUDA == CpuReference");
    }
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testBruteForce();
    testStatsFallbackAndInvalid();
    timeMillionRays();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d svo_ray_cast kernel check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("SVO ray cast kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
