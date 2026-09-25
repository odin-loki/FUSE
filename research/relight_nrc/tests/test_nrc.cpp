// FUSE Relight RL-5.4 research track, CPU runs: the online-trained neural radiance cache (NeuralRadianceCache) against
// the hash-grid radiance cache (RadianceCacheCpu) on the RL-5.1 Cornell scene. Reports only (plan: "neural track
// reports only (HW)"): the gates below check that the prototype works, not that it wins.
//
//   train        64 frames of online training on the hash grid's training records: at frame 4's record positions the
//                network's RMS error vs the converged hash grid (>= 64 samples per cell) falls below 0.7x its
//                frame-4 value; finite parameters.
//   report       bias and variance of cache-terminated path tracing with each cache (same training paths, same
//                termination decisions) vs the RL-5.1 reference: image bias per channel, 4x4 block error (rms / max),
//                per-pixel variance at 64 spp vs plain path tracing, hit rate, CPU time per frame; printed and written to
//                nrc_report.json (the RL-5.4 hand-off report quotes it). Gate: finite results only.
//   zero_alloc   steady-state trainFrame + queries make no heap allocation.
//
// Usage: fuse_relight_nrc_tests <suite>|all. Exit 0 pass, 1 fail.
#include "pt_test_scenes.hpp"

#include <relight_nrc/neural_radiance_cache.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/render/pathtrace/radiance_cache.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
namespace opt = fuse::relight::options;
namespace kernel = fuse::kernel;
namespace rs = fuse::relight::research;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

bool compile(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    check(ok, "compile");
    return ok;
}

pt::RadianceCacheSettings cacheSettings() {
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

pt::PtSettings ptSettings() {
    pt::PtSettings st;
    st.maxBounces = 6;
    st.rrStart = 3;
    return st;
}

rs::NrcSettings nrcSettings() {
    rs::NrcSettings s = rs::NrcSettings::defaults();
    for (int a = 0; a < 3; ++a) { // the Cornell box (5 x 5 x 5 around the origin) with a margin
        s.boundsMin[a] = -2.6f;
        s.boundsMax[a] = 2.6f;
    }
    return s;
}

constexpr u32 W = 32, H = 32;

enum class Mode { Grid, Neural };

struct Run {
    pt::PtReferenceImage img;
    double seconds = 0.0;   ///< cache work per frame (train + update / NN training)
    double hitRate = 0.0;   ///< last frame
    float loss = 0.f;
    bool ok = true;
};

Run runCache(const pt::PtCompiledScene& c, const pt::PtSettings& st, Mode mode, u32 warm, u32 frames, u32 spp,
             u32 seed, bool freeze) {
    Run r;
    r.img.resize(W, H);
    pt::RadianceCacheCpu rc;
    rc.configure(cacheSettings());
    rs::NeuralRadianceCache nrc;
    if (mode == Mode::Neural) {
        r.ok = nrc.init(nrcSettings());
        rc.setLookup(nrc.lookup());
    }
    pt::PtReferenceImage scratch;
    scratch.resize(W, H);
    double seconds = 0.0;
    u32 timed = 0;
    for (u32 f = 0; f < warm + frames && r.ok; ++f) {
        if (!(freeze && f >= warm)) {
            const auto t0 = std::chrono::steady_clock::now();
            r.ok = rc.frame(c, st, W, H, seed, f * 64u);
            if (mode == Mode::Neural) {
                r.ok = r.ok && nrc.trainFrame(rc.records().data(), u32(rc.records().size() / pt::kRcRecordWords));
            }
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ++timed;
        }
        const bool keep = f >= warm;
        pt::PtReferenceImage& target = keep ? r.img : scratch;
        r.ok = r.ok && pt::renderReference(c, pt::withRadianceCache(st), W, H, seed + 1000u, f * 64u, keep ? spp : 1u,
                                           target, kernel::Backend::CpuParallel, nullptr, nullptr, nullptr,
                                           rc.hook(W, H));
        r.hitRate = rc.stats().hitRate();
    }
    r.seconds = timed != 0u ? seconds / timed : 0.0;
    r.loss = nrc.valid() ? nrc.stats().loss : 0.f;
    return r;
}

struct Bias {
    double image[3] = {0, 0, 0};
    double blockRms = 0.0;
    double blockMax = 0.0;
};

Bias measureBias(const pt::PtReferenceImage& a, const pt::PtReferenceImage& ref) {
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
        b.image[k] = (sa[k] - sr[k]) / std::max(sr[k], 1e-12);
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

double varianceSum(const pt::PtReferenceImage& img) {
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

// --- train --------------------------------------------------------------------------------------------------------
void suiteTrain() {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    pt::RadianceCacheCpu rc;
    rc.configure(cacheSettings());
    rs::NeuralRadianceCache nrc;
    check(nrc.init(nrcSettings()), "train: init");
    // Probe points: frame 4's training records. The network's answer there after 4 and after 64 frames is compared
    // with the converged hash grid (64 frames: a many-sample, low-noise estimate of the same cells).
    std::vector<float> probes;
    std::vector<float> early;
    bool ok = true;
    for (u32 f = 0; f < 64 && ok; ++f) {
        ok = rc.frame(c, ptSettings(), W, H, 3u, f) &&
             nrc.trainFrame(rc.records().data(), u32(rc.records().size() / pt::kRcRecordWords));
        if (f == 4u) {
            const std::vector<pt::Word>& rec = rc.records();
            for (std::size_t i = 0; i + 2u < rec.size(); i += pt::kRcRecordWords) {
                if (rec[i].w > 0.5f) {
                    const float q[6] = {rec[i].x, rec[i].y, rec[i].z, rec[i + 1u].x, rec[i + 1u].y, rec[i + 1u].z};
                    probes.insert(probes.end(), q, q + 6);
                    float out[3];
                    nrc.query(q, q + 3, out);
                    early.insert(early.end(), out, out + 3);
                }
            }
        }
    }
    double eEarly = 0.0, eLate = 0.0;
    u32 n = 0;
    for (std::size_t i = 0; i < probes.size() / 6u; ++i) {
        const float* q = &probes[i * 6u];
        float grid[3], late[3], samples = 0.f;
        if (!pt::radianceCacheLookup(rc.params(), rc.table().data(), q, q + 3, grid, &samples) || samples < 64.f) {
            continue;
        }
        nrc.query(q, q + 3, late);
        for (u32 k = 0; k < 3u; ++k) {
            eEarly += (early[i * 3u + k] - grid[k]) * (early[i * 3u + k] - grid[k]);
            eLate += (late[k] - grid[k]) * (late[k] - grid[k]);
        }
        ++n;
    }
    eEarly = n != 0u ? std::sqrt(eEarly / (3.0 * n)) : 0.0;
    eLate = n != 0u ? std::sqrt(eLate / (3.0 * n)) : 0.0;
    bool finite = true;
    for (float p : nrc.net().params()) {
        finite = finite && std::isfinite(p);
    }
    std::printf("train: %u frames, %u Adam steps, %u samples / frame, %u parameters; last batch loss %.5f (one-sample "
                "targets: mostly their variance); RMS error vs the converged hash grid at %u probe points: after 4 "
                "frames %.4f, after 64 frames %.4f\n",
                nrc.stats().frames, nrc.stats().steps, nrc.stats().samples, nrc.net().paramCount(), nrc.stats().loss, n,
                eEarly, eLate);
    check(ok, "train: frames");
    check(finite, "train: parameters finite");
    check(n > 50u && eLate < 0.7 * eEarly, "train: the online fit converges towards the cells' means");
}

// --- report -------------------------------------------------------------------------------------------------------
void suiteReport() {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const pt::PtSettings st = ptSettings();
    pt::PtReferenceImage ref;
    ref.resize(W, H);
    check(pt::renderReference(c, st, W, H, 4242u, 0u, 1024u, ref), "report: reference");
    pt::PtSettings centres = st;
    centres.flags &= ~pt::kPtFlagJitter;
    pt::PtReferenceImage plain;
    plain.resize(W, H);
    check(pt::renderReference(c, centres, W, H, 99u, 0u, 64u, plain), "report: plain");
    const double vPlain = varianceSum(plain);
    struct Row {
        const char* name;
        Bias bias;
        double varRatio;
        double hitRate;
        double seconds;
        float loss;
    };
    Row rows[2];
    const Mode modes[2] = {Mode::Grid, Mode::Neural};
    const char* names[2] = {"hash grid", "neural (MLP)"};
    bool finite = true;
    for (int m = 0; m < 2; ++m) {
        const Run conv = runCache(c, st, modes[m], 16, 64, 16, 7u, false);
        const Run var = runCache(c, centres, modes[m], 32, 1, 64, 5u, true);
        check(conv.ok && var.ok, "report: runs");
        rows[m].name = names[m];
        rows[m].bias = measureBias(conv.img, ref);
        rows[m].varRatio = varianceSum(var.img) / vPlain;
        rows[m].hitRate = conv.hitRate;
        rows[m].seconds = conv.seconds;
        rows[m].loss = conv.loss;
        finite = finite && std::isfinite(rows[m].varRatio) && std::isfinite(rows[m].bias.blockRms);
    }
    std::printf("report (Cornell 32 x 32, maxBounces 6; bias: 16 warm-up + 64 frames x 16 spp vs 1024 spp reference; "
                "variance: pixel centres, 64 spp, frozen cache after 32 frames):\n");
    std::printf("| cache | image bias r / g / b | 4x4 block error rms / max | variance vs plain PT | hit rate | "
                "CPU ms / frame | loss |\n|---|---|---|---|---|---|---|\n");
    for (const Row& r : rows) {
        std::printf("| %s | %+.4f / %+.4f / %+.4f | %.4f / %.4f | %.3f | %.3f | %.1f | %.4f |\n", r.name,
                    r.bias.image[0], r.bias.image[1], r.bias.image[2], r.bias.blockRms, r.bias.blockMax, r.varRatio,
                    r.hitRate, r.seconds * 1e3, r.loss);
    }
    if (std::FILE* f = std::fopen("nrc_report.json", "wb")) {
        std::fprintf(f, "{\n  \"scene\": \"cornell 32x32\",\n  \"rows\": [\n");
        for (int m = 0; m < 2; ++m) {
            const Row& r = rows[m];
            std::fprintf(f,
                         "    {\"cache\": \"%s\", \"imageBias\": [%.6f, %.6f, %.6f], \"blockRms\": %.6f, \"blockMax\": "
                         "%.6f, \"varianceRatio\": %.6f, \"hitRate\": %.6f, \"msPerFrame\": %.3f, \"loss\": %.6f}%s\n",
                         r.name, r.bias.image[0], r.bias.image[1], r.bias.image[2], r.bias.blockRms, r.bias.blockMax,
                         r.varRatio, r.hitRate, r.seconds * 1e3, double(r.loss), m == 0 ? "," : "");
        }
        std::fprintf(f, "  ]\n}\n");
        std::fclose(f);
    }
    check(finite, "report: finite results");
}

// --- zero_alloc ---------------------------------------------------------------------------------------------------
void suiteZeroAlloc() {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    pt::RadianceCacheCpu rc;
    rc.configure(cacheSettings());
    rs::NeuralRadianceCache nrc;
    check(nrc.init(nrcSettings()), "zero_alloc: init");
    unsigned long long allocs = 0;
    bool ok = true;
    for (u32 f = 0; f < 8 && ok; ++f) {
        ok = rc.frame(c, ptSettings(), W, H, 1u, f);
        const bool measure = f >= 2;
        g_allocations.store(0);
        g_count.store(measure);
        ok = ok && nrc.trainFrame(rc.records().data(), u32(rc.records().size() / pt::kRcRecordWords));
        float out[3];
        for (u32 i = 0; i < 256; ++i) {
            const float p[3] = {-2.f + 0.015f * float(i), -2.4f, 0.1f};
            const float n[3] = {0.f, 1.f, 0.f};
            nrc.query(p, n, out);
        }
        g_count.store(false);
        allocs += g_allocations.load();
    }
    std::printf("zero_alloc: 6 steady-state frames of trainFrame + 256 queries: %llu operator-new calls\n", allocs);
    check(ok && allocs == 0u, "zero_alloc: steady-state neural cache training and queries make no heap allocation");
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"train", suiteTrain}, {"report", suiteReport}, {"zero_alloc", suiteZeroAlloc}};
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
    std::printf("PASS: rl_nrc %s\n", suite.c_str());
    return 0;
}
