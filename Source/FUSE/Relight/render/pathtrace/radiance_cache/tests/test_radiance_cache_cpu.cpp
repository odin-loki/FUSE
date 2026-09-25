// FUSE Relight RL-5.4 CPU gates: the world-space hash-grid radiance cache (radiance_cache.hpp; the single-source core
// kernels/radiance_cache_core.h / radiance_cache_path.h) on the RL-5.1 CPU reference path tracer's scenes.
//
//   cells        cell ids: the adaptive level makes cells at least cellPixels pixels and less than twice that wide at
//                their distance (above the finest size); points of one cell share its id, neighbours and opposite
//                normal bins do not; checksums are never 0, slots below the capacity and well spread.
//   update       closed forms of update + resolve on synthetic records: the frame mean (fixed point, 1e-4), the
//                temporal window (sample count capped at maxSamples), aging and eviction after maxAge frames, the
//                radiance clamp, drops when a probe sequence is full; the per-key result is independent of the record
//                order and of the backend (CpuReference == CpuParallel).
//   converge     cache-terminated path tracing of the Cornell scene (paths end at the first eligible vertex after the
//                G-buffer vertex) converges within the documented bias bound of the RL-5.1 reference (plain path
//                tracing, 1024 spp): image mean within kBiasImage relative per channel, 4x4 block means within
//                kBiasBlock relative (RMS over blocks); the measured values are next to the constants below.
//   variance     pixel centres, a warm cache: the per-pixel variance of cache-terminated paths, summed over the image,
//                is below plain path tracing's at equal spp (factor reported; gate: ratio < kVarianceRatio).
//   hitrate      cache hit-rate metrics per frame (queries, hits, live cells, inserts, drops): after warm-up the hit
//                rate is above kHitRate and nothing is dropped.
//   determinism  two runs (CpuReference / CpuParallel) give the same records bit for bit, the same cells per key and
//                the same cache-terminated render.
//   zero_alloc   steady-state host work (params packing, update, resolve, the query hook) makes no heap allocation;
//                the training stage's count is reported (the WP-6.0 CPU BVH oracle allocates per ray).
//   options      rtx.radianceCache.* are registered and RadianceCacheSettings::fromOptions reads them.
//   layout       the push blocks of radiance_cache.{slang,comp} match RadianceCacheGpu::Push field for field; the
//                path tracer's PtTracePush is unchanged (RL-5.3 layout); PtParams ends with rcTableLo, rcTableHi.
//
// Usage: fuse_relight_radiance_cache_tests <suite>|all. Exit 0 pass, 1 fail.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/radiance_cache.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include "pt_reference_kernels.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> g_count{false};
std::atomic<unsigned long long> g_allocations{0};
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (g_count.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace pt_test;
using fuse::u32;
using fuse::u64;
namespace opt = fuse::relight::options;
namespace kernel = fuse::kernel;

// Documented bias bound of the hash-grid cache: cache-terminated vs the RL-5.1 reference on the Cornell scenes (with
// and without the glass sphere) at 32 x 32, cellPixels 2, maxBounces 6. Measured (Linux and MinGW / Wine alike):
// Cornell image 0.57% / 0.16% / 0.04% (r / g / b), 4x4 blocks rms 2.4% (max 9.1%); + glass 0.66% / 0.65% / 0.23%,
// rms 2.8% (max 9.7%). The reference's image noise is ~0.17%; the block metric is noise-limited (~2% combined).
constexpr double kBiasImage = 0.02;  ///< |image mean - reference| / reference, per channel
constexpr double kBiasBlock = 0.06;  ///< RMS over 4x4 blocks of |block mean - reference| / reference
constexpr double kVarianceRatio = 0.8;
constexpr double kHitRate = 0.9;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
void check(bool condition, const std::string& message) { check(condition, message.c_str()); }

struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

bool compile(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    check(ok, "compile: " + error);
    return ok;
}

/// The cache settings of the image gates (32 x 32: a pixel is ~0.1 world units on the Cornell back wall).
pt::RadianceCacheSettings imageSettings() {
    pt::RadianceCacheSettings s;
    s.enabled = true;
    s.capacity = 1u << 16;
    s.cellSize = 0.05f;
    s.cellPixels = 2.f;
    s.trainStride = 2;
    s.minSamples = 4.f;
    s.maxSamples = 512.f;
    s.maxAge = 16;
    return s;
}

pt::PtSettings imagePt() {
    pt::PtSettings st;
    st.maxBounces = 6;
    st.rrStart = 3;
    return st;
}

void record(std::vector<pt::Word>& out, const float p[3], const float n[3], float r, float g, float b) {
    out.push_back(pt::Word(p[0], p[1], p[2], 1.f));
    out.push_back(pt::Word(n[0], n[1], n[2], 0.f));
    out.push_back(pt::Word(r, g, b, 0.f));
}

void cornellParams(const pt::RadianceCacheSettings& s, u32 w, u32 h, pt::Word* rp) {
    static pt::PtCompiledScene c;
    static bool compiled = false;
    if (!compiled) {
        compiled = compile(cornell(false), c);
    }
    pt::Word ptw[pt::kPtParamWords];
    c.packParams(pt::PtSettings{}, w, h, 1u, 0u, ptw);
    pt::packRadianceCacheParams(s, ptw, 0u, rp);
}

u32 lcg(u32& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}
float uniform(u32& state) { return float(lcg(state)) * (1.f / 16777216.f); }

// --- cells ------------------------------------------------------------------------------------------------------------
void suiteCells() {
    pt::RadianceCacheSettings s;
    s.capacity = 1u << 16;
    s.cellSize = 0.05f;
    s.cellPixels = 4.f;
    pt::Word rp[pt::kRcParamWords];
    cornellParams(s, 64, 64, rp);
    const float cam[3] = {rp[0].x, rp[0].y, rp[0].z};
    const float pixelAngle = rp[0].w;
    const float expectAngle = 2.f * std::tan(0.5f * 38.f * kPi / 180.f) / 64.f;
    check(std::fabs(pixelAngle - expectAngle) <= 1e-5f * expectAngle + 1e-7f, "cells: pixel angle from the camera");
    const float n[3] = {0.f, 0.f, 1.f};
    u32 prevLevel = 0;
    bool sizeOk = true, monotone = true;
    for (float d : {0.05f, 0.2f, 0.5f, 1.f, 2.f, 4.f, 9.f, 16.f, 40.f, 100.f, 400.f}) {
        const float p[3] = {cam[0] + 0.3f * d, cam[1] - 0.2f * d, cam[2] - 0.93f * d};
        const float dist = std::sqrt(0.09f + 0.04f + 0.93f * 0.93f) * d;
        const pt::RadianceCacheCell cell = pt::radianceCacheCell(rp, p, n);
        const float size = s.cellSize * std::ldexp(1.f, int(cell.level));
        const float target = dist * pixelAngle * s.cellPixels;
        sizeOk = sizeOk && size >= target * (1.f - 1e-4f) && (cell.level == 0u || 0.5f * size < target * (1.f + 1e-4f));
        monotone = monotone && cell.level >= prevLevel;
        prevLevel = cell.level;
        std::printf("  distance %7.2f: level %2u, cell %.4f (target %.4f)\n", dist, cell.level, size, target);
    }
    check(sizeOk, "cells: cell edge in [target, 2 x target) at the cell's distance (adaptive level)");
    check(monotone, "cells: level non-decreasing with distance");

    // One cell: points inside share the id; the neighbour and the opposite bin differ.
    const float base[3] = {0.31f, -1.7f, -2.2f};
    const pt::RadianceCacheCell c0 = pt::radianceCacheCell(rp, base, n);
    const float size = s.cellSize * std::ldexp(1.f, int(c0.level));
    float centre[3];
    for (int a = 0; a < 3; ++a) {
        centre[a] = (std::floor(base[a] / size) + 0.5f) * size;
    }
    const pt::RadianceCacheCell cc = pt::radianceCacheCell(rp, centre, n);
    bool inside = true;
    u32 st = 7u;
    for (u32 i = 0; i < 200; ++i) {
        const float q[3] = {centre[0] + (uniform(st) - 0.5f) * 0.8f * size, centre[1] + (uniform(st) - 0.5f) * 0.8f * size,
                            centre[2] + (uniform(st) - 0.5f) * 0.8f * size};
        const float nq[3] = {0.1f * (uniform(st) - 0.5f), 0.1f * (uniform(st) - 0.5f), 1.f};
        const pt::RadianceCacheCell ci = pt::radianceCacheCell(rp, q, nq);
        inside = inside && (ci.level != cc.level || (ci.check == cc.check && ci.slot == cc.slot));
    }
    check(inside, "cells: points of one cell (and normals of one bin) share its id");
    const float nb[3] = {centre[0] + size, centre[1], centre[2]};
    const float nOpp[3] = {0.f, 0.f, -1.f};
    const float nSide[3] = {0.f, 1.f, 0.f};
    check(pt::radianceCacheCell(rp, nb, n).check != cc.check, "cells: the neighbouring cell has another id");
    check(pt::radianceCacheCell(rp, centre, nOpp).check != cc.check &&
              pt::radianceCacheCell(rp, centre, nSide).check != cc.check,
          "cells: other normal bins have other ids");

    // Spread: 20000 random points of the box.
    std::vector<u32> slots, checks;
    slots.reserve(20000);
    checks.reserve(20000);
    bool nonZero = true, inRange = true, repeat = true;
    for (u32 i = 0; i < 20000; ++i) {
        const float q[3] = {(uniform(st) - 0.5f) * 5.f, (uniform(st) - 0.5f) * 5.f, (uniform(st) - 0.5f) * 5.f};
        const pt::RadianceCacheCell a = pt::radianceCacheCell(rp, q, n);
        const pt::RadianceCacheCell b = pt::radianceCacheCell(rp, q, n);
        nonZero = nonZero && a.check != 0u;
        inRange = inRange && a.slot < s.capacity;
        repeat = repeat && a.check == b.check && a.slot == b.slot && a.level == b.level;
        slots.push_back(a.slot);
        checks.push_back(a.check);
    }
    std::sort(slots.begin(), slots.end());
    std::sort(checks.begin(), checks.end());
    const std::size_t distinct = static_cast<std::size_t>(std::unique(slots.begin(), slots.end()) - slots.begin());
    const std::size_t cells = static_cast<std::size_t>(std::unique(checks.begin(), checks.end()) - checks.begin());
    const double expected = double(s.capacity) * (1.0 - std::exp(-double(cells) / double(s.capacity)));
    std::printf("cells: 20000 random points of the box -> %zu cells, %zu distinct start slots (uniform hashing: %.0f)\n",
                cells, distinct, expected);
    check(nonZero && inRange && repeat, "cells: checksums non-zero, slots in range, repeatable");
    check(cells > 1000u && double(distinct) > 0.9 * expected, "cells: start slots spread like uniform hashing");
}

// --- update -------------------------------------------------------------------------------------------------------------
std::vector<pt::RadianceCacheEntry> entriesOf(const std::vector<u32>& table, u32 capacity) {
    std::vector<pt::RadianceCacheEntry> e;
    pt::radianceCacheEntries(table.data(), capacity, e);
    return e;
}

bool sameEntries(const std::vector<pt::RadianceCacheEntry>& a, const std::vector<pt::RadianceCacheEntry>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].check != b[i].check || std::memcmp(a[i].words, b[i].words, sizeof(a[i].words)) != 0) {
            return false;
        }
    }
    return true;
}

void suiteUpdate() {
    pt::RadianceCacheSettings s;
    s.capacity = 1024;
    s.maxSamples = 64.f;
    s.maxAge = 3;
    s.minSamples = 1.f;
    s.maxRadiance = 8.f;
    pt::Word rp[pt::kRcParamWords];
    cornellParams(s, 64, 64, rp);
    pt::RadianceCacheCpu rc;
    rc.configure(s);
    const u32 cap = s.capacity;
    std::vector<u32> table(pt::radianceCacheTableWords(s), 0u);
    const float A[3] = {0.2f, -2.4f, 0.4f};
    const float up[3] = {0.f, 1.f, 0.f};
    std::vector<pt::Word> rec;
    double mean[3] = {0, 0, 0};
    for (u32 i = 0; i < 100; ++i) {
        const float r = 0.05f + 0.1f * float(i % 7), g = 0.1f + 0.013f * float(i), b = 0.15f;
        record(rec, A, up, r, g, b);
        mean[0] += r / 100.0;
        mean[1] += g / 100.0;
        mean[2] += b / 100.0;
    }
    check(rc.replayUpdate(rp, rec.data(), 100u, table) && rc.replayResolve(rp, table), "update: replay");
    float L[3];
    float n = 0.f;
    bool hit = pt::radianceCacheLookup(rp, table.data(), A, up, L, &n);
    const double err1 = std::max({std::fabs(L[0] - mean[0]), std::fabs(L[1] - mean[1]), std::fabs(L[2] - mean[2])});
    std::printf("update: frame 1: 100 records -> (%.5f %.5f %.5f), expected (%.5f %.5f %.5f), samples %.0f\n", L[0], L[1],
                L[2], mean[0], mean[1], mean[2], n);
    check(hit && err1 <= 1e-4 && n == 64.f, "update: resolved mean of the frame (fixed point 1e-4), samples capped");
    const pt::RadianceCacheStats c1 = pt::radianceCacheCounters(table.data());
    check(c1.inserts == 100u && c1.dropped == 0u && c1.live == 1u && c1.fresh == 1u, "update: counters");

    // Frame 2: 50 records of 1.0 (and a clamped / NaN one): window = (old x 64 + sum) / (64 + n).
    rec.clear();
    for (u32 i = 0; i < 50; ++i) {
        record(rec, A, up, 1.f, 1.f, 1.f);
    }
    record(rec, A, up, 1e6f, std::nanf(""), -3.f); // -> (maxRadiance, 0, 0)
    check(rc.replayUpdate(rp, rec.data(), 51u, table) && rc.replayResolve(rp, table), "update: replay 2");
    hit = pt::radianceCacheLookup(rp, table.data(), A, up, L, &n);
    const double e0 = (mean[0] * 64.0 + 50.0 + 8.0) / 115.0;
    const double e1 = (mean[1] * 64.0 + 50.0) / 115.0;
    const double e2 = (mean[2] * 64.0 + 50.0) / 115.0;
    std::printf("update: frame 2 -> (%.5f %.5f %.5f), expected (%.5f %.5f %.5f)\n", L[0], L[1], L[2], e0, e1, e2);
    check(hit && std::fabs(L[0] - e0) <= 1e-4 && std::fabs(L[1] - e1) <= 1e-4 && std::fabs(L[2] - e2) <= 1e-4 &&
              n == 64.f,
          "update: temporal window, radiance clamp, NaN / negative records count as 0");
    // Aging: alive for maxAge frames without samples, evicted after.
    bool alive = true;
    for (u32 f = 0; f < 3; ++f) {
        rc.replayResolve(rp, table);
        alive = alive && pt::radianceCacheLookup(rp, table.data(), A, up, L, &n);
    }
    rc.replayResolve(rp, table);
    const bool evicted = !pt::radianceCacheLookup(rp, table.data(), A, up, L, &n);
    check(alive && evicted && pt::radianceCacheCounters(table.data()).evicted == 1u,
          "update: a cell lives maxAge frames without samples, then is evicted");
    check(entriesOf(table, cap).empty(), "update: the evicted slot is empty");

    // Drops: a full probe sequence.
    pt::RadianceCacheSettings tiny = s;
    tiny.capacity = 64;
    tiny.maxProbe = 1;
    pt::Word rt[pt::kRcParamWords];
    cornellParams(tiny, 64, 64, rt);
    std::vector<u32> small(pt::radianceCacheTableWords(tiny), 0u);
    rec.clear();
    u32 st = 3u;
    for (u32 i = 0; i < 500; ++i) {
        const float q[3] = {(uniform(st) - 0.5f) * 5.f, (uniform(st) - 0.5f) * 5.f, (uniform(st) - 0.5f) * 5.f};
        record(rec, q, up, 0.5f, 0.5f, 0.5f);
    }
    rc.replayUpdate(rt, rec.data(), 500u, small);
    const pt::RadianceCacheStats cd = pt::radianceCacheCounters(small.data());
    std::printf("update: 500 records into 64 slots (probe 1): %u accumulated, %u dropped\n", cd.inserts, cd.dropped);
    check(cd.dropped > 0u && cd.inserts + cd.dropped == 500u, "update: records of a full probe sequence are dropped");

    // Order and backend independence (per key), with collisions: ~1100 cells in 4096 slots.
    pt::RadianceCacheSettings fine = s;
    fine.capacity = 4096;
    fine.cellPixels = 0.25f;
    cornellParams(fine, 64, 64, rp);
    const u32 capFine = fine.capacity;
    table.assign(pt::radianceCacheTableWords(fine), 0u);
    rec.clear();
    for (u32 i = 0; i < 4000; ++i) {
        const float q[3] = {(uniform(st) - 0.5f) * 1.2f, -2.45f + 0.1f * uniform(st), (uniform(st) - 0.5f) * 1.2f};
        const float nn[3] = {uniform(st) - 0.5f, 1.f, uniform(st) - 0.5f};
        record(rec, q, nn, uniform(st), uniform(st) * 2.f, uniform(st) * 0.5f);
    }
    std::vector<pt::Word> shuffled = rec;
    for (u32 i = 3999; i > 0; --i) {
        const u32 j = lcg(st) % (i + 1u);
        for (u32 k = 0; k < 3; ++k) {
            std::swap(shuffled[i * 3u + k], shuffled[j * 3u + k]);
        }
    }
    std::vector<u32> ta(table.size(), 0u), tb(table.size(), 0u), tc(table.size(), 0u);
    rc.replayUpdate(rp, rec.data(), 4000u, ta, kernel::Backend::CpuReference);
    rc.replayUpdate(rp, shuffled.data(), 4000u, tb, kernel::Backend::CpuReference);
    rc.replayUpdate(rp, rec.data(), 4000u, tc, kernel::Backend::CpuParallel);
    const bool sameUpdate = sameEntries(entriesOf(ta, capFine), entriesOf(tb, capFine)) &&
                            sameEntries(entriesOf(ta, capFine), entriesOf(tc, capFine));
    const bool relocated = std::memcmp(ta.data(), tb.data(), ta.size() * sizeof(u32)) != 0;
    rc.replayResolve(rp, ta, kernel::Backend::CpuReference);
    rc.replayResolve(rp, tb, kernel::Backend::CpuParallel);
    rc.replayResolve(rp, tc, kernel::Backend::CpuParallel);
    const auto ea = entriesOf(ta, capFine);
    std::printf("update: 4000 records -> %zu cells (%u dropped); shuffled order %s the slot layout, identical per key\n",
                ea.size(), pt::radianceCacheCounters(ta.data()).dropped, relocated ? "changes" : "keeps");
    check(ea.size() > 500u && pt::radianceCacheCounters(ta.data()).dropped == 0u, "update: order test has many cells");
    check(sameUpdate && sameEntries(ea, entriesOf(tb, capFine)) && sameEntries(ea, entriesOf(tc, capFine)),
          "update: per-key cells independent of record order and backend (update and resolve)");
}

// --- image gates ----------------------------------------------------------------------------------------------------
struct CacheRun {
    pt::PtReferenceImage img;
    std::vector<pt::RadianceCacheStats> stats; ///< per frame (after the frame's render)
    bool ok = true;
};

/// `warm` frames of cache updates, then `frames` frames of (update + cache-terminated render at `spp`), accumulated.
CacheRun runCache(const pt::PtCompiledScene& c, const pt::PtSettings& st, const pt::RadianceCacheSettings& s, u32 W,
                  u32 H, u32 warm, u32 frames, u32 spp, u32 seed, bool freeze = false) {
    CacheRun r;
    r.img.resize(W, H);
    pt::RadianceCacheCpu rc;
    rc.configure(s);
    pt::PtReferenceImage scratch;
    scratch.resize(W, H);
    for (u32 f = 0; f < warm + frames; ++f) {
        if (!(freeze && f >= warm)) {
            r.ok = r.ok && rc.frame(c, st, W, H, seed, f * 64u);
        }
        const bool keep = f >= warm;
        pt::PtReferenceImage& target = keep ? r.img : scratch;
        r.ok = r.ok && pt::renderReference(c, pt::withRadianceCache(st), W, H, seed + 1000u, f * 64u, keep ? spp : 1u,
                                           target, kernel::Backend::CpuParallel, nullptr, nullptr, nullptr,
                                           rc.hook(W, H));
        r.stats.push_back(rc.stats());
    }
    return r;
}

struct Bias {
    double image[3] = {0, 0, 0};
    double blockRms = 0.0;
    double blockMax = 0.0;
};

Bias measureBias(const pt::PtReferenceImage& a, const pt::PtReferenceImage& ref, u32 W, u32 H) {
    Bias b;
    double sa[3] = {0, 0, 0}, sr[3] = {0, 0, 0};
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            for (u32 k = 0; k < 3; ++k) {
                sa[k] += a.mean(x, y, k);
                sr[k] += ref.mean(x, y, k);
            }
        }
    }
    for (u32 k = 0; k < 3; ++k) {
        b.image[k] = std::fabs(sa[k] - sr[k]) / std::max(sr[k], 1e-12);
    }
    double sq = 0.0;
    u32 n = 0;
    for (u32 by = 0; by < H; by += 4) {
        for (u32 bx = 0; bx < W; bx += 4) {
            double ma = 0, mr = 0;
            for (u32 y = by; y < by + 4; ++y) {
                for (u32 x = bx; x < bx + 4; ++x) {
                    for (u32 k = 0; k < 3; ++k) {
                        ma += a.mean(x, y, k);
                        mr += ref.mean(x, y, k);
                    }
                }
            }
            if (mr > 1e-3) {
                const double e = std::fabs(ma - mr) / mr;
                sq += e * e;
                b.blockMax = std::max(b.blockMax, e);
                ++n;
            }
        }
    }
    b.blockRms = n != 0u ? std::sqrt(sq / n) : 0.0;
    return b;
}

void convergeCase(const char* name, const pt::PtScene& scene) {
    constexpr u32 W = 32, H = 32, kRefSpp = 1024;
    pt::PtCompiledScene c;
    if (!compile(scene, c)) {
        return;
    }
    const pt::PtSettings st = imagePt();
    pt::PtReferenceImage ref;
    ref.resize(W, H);
    check(pt::renderReference(c, st, W, H, 4242u, 0u, kRefSpp, ref), "converge: reference");
    const CacheRun run = runCache(c, st, imageSettings(), W, H, 16, 64, 16, 7u);
    check(run.ok, "converge: cache frames");
    const Bias b = measureBias(run.img, ref, W, H);
    // The reference's own noise at 1024 spp (the bound must be well above it).
    double v = 0.0, m = 0.0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            v += ref.variance(x, y, 1) / kRefSpp;
            m += ref.mean(x, y, 1);
        }
    }
    const double noise = std::sqrt(v) / std::max(m, 1e-12);
    const pt::RadianceCacheStats& last = run.stats.back();
    std::printf("converge %s: cache (16 warm-up + 64 frames x 16 spp) vs reference (%u spp): image bias r %.4f g %.4f "
                "b %.4f (bound %.3f; reference noise %.5f), 4x4 blocks rms %.4f max %.4f (bound rms %.3f); hit rate "
                "%.3f\n",
                name, kRefSpp, b.image[0], b.image[1], b.image[2], kBiasImage, noise, b.blockRms, b.blockMax, kBiasBlock,
                last.hitRate());
    check(b.image[0] <= kBiasImage && b.image[1] <= kBiasImage && b.image[2] <= kBiasImage,
          std::string("converge ") + name + ": image mean within the documented bias bound of the RL-5.1 reference");
    check(b.blockRms <= kBiasBlock,
          std::string("converge ") + name + ": 4x4 block means within the documented bias bound (rms)");
    check(last.hits > 0u, std::string("converge ") + name + ": paths ended in the cache");
}

void suiteConverge() {
    convergeCase("cornell", cornell(false));
    convergeCase("cornell + glass", cornell(true));
}

double varianceSum(const pt::PtReferenceImage& img, u32 W, u32 H) {
    double v = 0.0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            for (u32 k = 0; k < 3; ++k) {
                v += img.variance(x, y, k);
            }
        }
    }
    return v;
}

void suiteVariance() {
    constexpr u32 W = 32, H = 32, kSpp = 64;
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    pt::PtSettings st = imagePt();
    st.flags &= ~pt::kPtFlagJitter; // pixel centres: the lighting estimator's variance, not the footprint's
    pt::PtReferenceImage plain;
    plain.resize(W, H);
    check(pt::renderReference(c, st, W, H, 99u, 0u, kSpp, plain), "variance: plain path tracing");
    const CacheRun run = runCache(c, st, imageSettings(), W, H, 32, 1, kSpp, 5u, true);
    check(run.ok, "variance: cache frames");
    const double vp = varianceSum(plain, W, H);
    const double vc = varianceSum(run.img, W, H);
    std::printf("variance: per-pixel variance summed over the image at %u spp: plain %.4f, cache-terminated %.4f (ratio "
                "%.3f, gate < %.2f)\n",
                kSpp, vp, vc, vc / vp, kVarianceRatio);
    check(vc < kVarianceRatio * vp, "variance: cache-terminated paths below plain path tracing at equal spp");
}

void suiteHitRate() {
    constexpr u32 W = 32, H = 32;
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const CacheRun run = runCache(c, imagePt(), imageSettings(), W, H, 0, 24, 1, 3u);
    check(run.ok, "hitrate: frames");
    std::printf("hitrate: frame  queries  hits  rate   live  fresh  inserts  dropped  evicted\n");
    for (std::size_t f = 0; f < run.stats.size(); ++f) {
        const pt::RadianceCacheStats& s = run.stats[f];
        if (f < 4 || f % 4 == 3) {
            std::printf("         %5zu  %7u  %5u  %.3f  %5u  %5u  %7u  %7u  %7u\n", f, s.queries, s.hits, s.hitRate(),
                        s.live, s.fresh, s.inserts, s.dropped, s.evicted);
        }
    }
    const pt::RadianceCacheStats& last = run.stats.back();
    u32 dropped = 0;
    for (const pt::RadianceCacheStats& s : run.stats) {
        dropped += s.dropped;
    }
    check(run.stats.front().hits == 0u || run.stats.front().hitRate() < last.hitRate(), "hitrate: warms up");
    check(last.queries > 0u && last.hitRate() >= kHitRate, "hitrate: above the gate after warm-up");
    check(dropped == 0u && last.live > 0u, "hitrate: no drops, live cells");
}

// --- determinism ------------------------------------------------------------------------------------------------------
void suiteDeterminism() {
    constexpr u32 W = 16, H = 16;
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const pt::PtSettings st = imagePt();
    pt::RadianceCacheSettings s = imageSettings();
    pt::RadianceCacheCpu a, b;
    a.configure(s);
    b.configure(s);
    bool recordsSame = true;
    for (u32 f = 0; f < 3; ++f) {
        a.frame(c, st, W, H, 3u, f, kernel::Backend::CpuReference);
        b.frame(c, st, W, H, 3u, f, kernel::Backend::CpuParallel);
        recordsSame = recordsSame && a.records().size() == b.records().size() &&
                      std::memcmp(a.records().data(), b.records().data(), a.records().size() * sizeof(pt::Word)) == 0;
    }
    check(recordsSame, "determinism: training records bit-identical (CpuReference / CpuParallel)");
    check(sameEntries(entriesOf(a.table(), s.capacity), entriesOf(b.table(), s.capacity)),
          "determinism: cells identical per key");
    pt::PtReferenceImage ia, ib;
    ia.resize(W, H);
    ib.resize(W, H);
    pt::renderReference(c, pt::withRadianceCache(st), W, H, 9u, 0u, 4u, ia, kernel::Backend::CpuReference, nullptr,
                        nullptr, nullptr, a.hook(W, H));
    pt::renderReference(c, pt::withRadianceCache(st), W, H, 9u, 0u, 4u, ib, kernel::Backend::CpuParallel, nullptr,
                        nullptr, nullptr, b.hook(W, H));
    bool same = true;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            for (u32 k = 0; k < 3; ++k) {
                same = same && ia.pixel(x, y).sum[k] == ib.pixel(x, y).sum[k];
            }
        }
    }
    check(same, "determinism: cache-terminated render bit-identical");
    std::printf("determinism: %zu cells, records / cells / render identical\n", entriesOf(a.table(), s.capacity).size());
}

// --- zero_alloc -------------------------------------------------------------------------------------------------------
void suiteZeroAlloc() {
    constexpr u32 W = 16, H = 16;
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const pt::PtSettings st = imagePt();
    pt::RadianceCacheCpu rc;
    rc.configure(imageSettings());
    unsigned long long host = 0, trainAllocs = 0;
    bool ok = true;
    for (u32 f = 0; f < 10; ++f) {
        const bool measure = f >= 2;
        g_allocations.store(0);
        g_count.store(measure);
        ok = ok && rc.train(c, st, W, H, 1u, f, kernel::Backend::CpuReference);
        g_count.store(false);
        trainAllocs += g_allocations.load();
        g_allocations.store(0);
        g_count.store(measure);
        pt::Word ptw[pt::kPtParamWords];
        c.packParams(st, W, H, 1u, f, ptw);
        pt::Word rp[pt::kRcParamWords];
        pt::packRadianceCacheParams(rc.settings(), ptw, f, rp);
        ok = ok && rc.update(kernel::Backend::CpuReference) && rc.resolve(kernel::Backend::CpuReference);
        const fuse::relight::ptk::PtRcHook* h = rc.hook(W, H);
        ok = ok && h != nullptr;
        const pt::RadianceCacheStats s = rc.stats();
        (void)s;
        g_count.store(false);
        host += g_allocations.load();
    }
    std::printf("zero_alloc: 8 steady-state frames: params + update + resolve + hook + stats %llu operator-new calls; "
                "train (CPU BVH oracle, per ray) %llu\n",
                host, trainAllocs);
    check(ok, "zero_alloc: frames");
    check(host == 0u, "zero_alloc: steady-state radiance cache host work makes no heap allocation");
}

// --- options ----------------------------------------------------------------------------------------------------------
void suiteOptions() {
    const pt::RadianceCacheSettings d = pt::RadianceCacheSettings::fromOptions();
    const char* names[] = {"rtx.radianceCache.enable",       "rtx.radianceCache.capacityLog2",
                           "rtx.radianceCache.cellSize",     "rtx.radianceCache.cellPixels",
                           "rtx.radianceCache.trainStride",  "rtx.radianceCache.minSamples",
                           "rtx.radianceCache.maxSamples",   "rtx.radianceCache.maxAge",
                           "rtx.radianceCache.minRoughness", "rtx.radianceCache.spreadThreshold"};
    u32 found = 0;
    for (const char* n : names) {
        found += opt::OptionManager::findOption(n) != nullptr ? 1u : 0u;
    }
    std::printf("options: %u/%zu registered; defaults: enabled %d, capacity %u, cell %.3f (%.1f px), stride %u, "
                "samples %.0f..%.0f, age %u, roughness %.2f\n",
                found, sizeof(names) / sizeof(names[0]), d.enabled ? 1 : 0, d.capacity, d.cellSize, d.cellPixels,
                d.trainStride, d.minSamples, d.maxSamples, d.maxAge, d.minRoughness);
    check(found == sizeof(names) / sizeof(names[0]), "options: rtx.radianceCache.* registered");
    check(!d.enabled && d.capacity == (1u << 18) && d.trainStride == 4u && d.maxAge == 32u, "options: defaults");
}

// --- layout -----------------------------------------------------------------------------------------------------------
std::string readText(const std::string& path) {
    std::string text;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return text;
    }
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
    return text;
}

std::vector<std::string> blockFields(const std::string& path, const char* name) {
    std::vector<std::string> out;
    const std::string text = readText(path);
    const std::size_t at = text.find(name);
    const std::size_t open = at == std::string::npos ? at : text.find('{', at);
    const std::size_t close = open == std::string::npos ? open : text.find('}', open);
    if (close == std::string::npos) {
        return out;
    }
    std::size_t p = open + 1u;
    while (p < close) {
        const std::size_t semi = text.find(';', p);
        if (semi == std::string::npos || semi > close) {
            break;
        }
        const std::string decl = text.substr(p, semi - p);
        std::size_t end = decl.size();
        while (end > 0 && decl[end - 1] == ' ') {
            --end;
        }
        std::size_t begin = end;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(decl[begin - 1])) || decl[begin - 1] == '_')) {
            --begin;
        }
        out.push_back(decl.substr(begin, end - begin));
        p = text.find('\n', semi);
        p = p == std::string::npos ? close : p + 1u;
    }
    return out;
}

void suiteLayout() {
    const std::string rc = FUSE_RL_RC_SHADER_DIR;
    const std::string ptDir = FUSE_RL_PT_SHADER_DIR;
    const std::string kDir = FUSE_RL_KERNELS_DIR;
    const std::vector<std::string> want = {"params",   "instances", "triangles", "materials", "portals",
                                           "lightMap", "lightTable", "lightTree", "tlas",      "lut",
                                           "restirDi", "block",     "width",     "height",    "lightCount",
                                           "stage",    "count",     "pitch"};
    const std::vector<std::string> a = blockFields(rc + "/radiance_cache.slang", "struct RcPush");
    const std::vector<std::string> b = blockFields(rc + "/radiance_cache.comp", "uniform RcPush");
    const std::vector<std::string> ta = blockFields(ptDir + "/rl_pt_trace.slang", "struct PtTracePush");
    const std::vector<std::string> tb = blockFields(ptDir + "/rl_pt_trace.comp", "uniform PtTracePush");
    const std::vector<std::string> pp = blockFields(kDir + "/pt_reference_types.h", "struct PtParams");
    std::printf("layout: RcPush slang %zu / glsl %zu fields (C++ RadianceCacheGpu::Push: 12 u64 + 6 u32 = 120 bytes); "
                "PtTracePush %zu fields (last %s); PtParams %zu fields (last %s)\n",
                a.size(), b.size(), ta.size(), ta.empty() ? "-" : ta.back().c_str(), pp.size(),
                pp.empty() ? "-" : pp.back().c_str());
    check(a == want && b == want, "layout: RcPush (radiance_cache.slang / .comp) == RadianceCacheGpu::Push field order");
    check(ta.size() == 18u && ta == tb && ta.back() == "restirGi",
          "layout: PtTracePush unchanged (the table address travels in PtParams w10.zw)");
    check(pp.size() >= 2u && pp[pp.size() - 2u] == "rcTableLo" && pp.back() == "rcTableHi",
          "layout: PtParams ends with rcTableLo, rcTableHi (ptParamsUnpack w10.z / w10.w)");
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    (void)pt::RadianceCacheOptions::enableObject();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"cells", suiteCells},       {"update", suiteUpdate},     {"converge", suiteConverge},
                            {"variance", suiteVariance}, {"hitrate", suiteHitRate},   {"determinism", suiteDeterminism},
                            {"zero_alloc", suiteZeroAlloc}, {"options", suiteOptions}, {"layout", suiteLayout}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            s.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_radiance_cache %s\n", suite.c_str());
    return 0;
}
