// FUSE Relight RL-5.2 CPU gates: ReSTIR DI (restir_di.hpp, the core restir_di_core.h) on the RL-5.1 CPU reference path
// tracer's scenes.
//
//   presample    the tiles' power table: pmf sums to 1 exactly, CDF monotone ending at 1, the pmf is the CDF's exact
//                24-bit bin width, and a frame's tile entries follow the pmf (chi-square over the lights).
//   unbiased     unbiased mode (initial RIS over tiles + tree + BSDF, temporal + spatial reuse with visibility-tested
//                pairwise MIS) driving the path tracer's direct light: over 24 independent runs of 6 frames, the image
//                mean (per channel) lies within 3 sigma of the RL-5.1 reference (plain light-tree NEE + MIS, 2048 spp),
//                on the Cornell scene (emissive triangles, sphere light, metal, mirror PSR, alpha-tested panel),
//                Cornell + glass (maxBounces 3: the ReSTIR vertex inside full paths) and a many-light scene (rect,
//                disk, cylinder, shaped sphere, delta distant; direct only); 4x4 block means within 5 sigma.
//   variance     direct lighting only, pixel centres (no jitter): the per-pixel variance of one ReSTIR frame (after
//                warm-up, across 24 runs) summed over the image is below plain light-tree NEE (1 spp) and below
//                RIS-only (reuse off, the same candidates).
//   fast         biased mode runs, stays finite and within 15% of the reference mean (bias reported).
//   determinism  RestirDiCpu on CpuReference == CpuParallel bit for bit (surfaces, reservoirs, output) over two
//                frames; the path tracer's output with the hook equal too.
//   zero_alloc   steady-state host work (light table build, params packing) makes no heap allocation; the stage
//                kernels' count is reported (the WP-6.0 CPU BVH oracle allocates per ray, as in rl_pt_zero_alloc).
//   options      rtx.useRTXDI / rtx.di.* are registered and RestirDiSettings::fromOptions reads them.
//   layout       the push-constant blocks of restir_di.{slang,comp} match RestirDiGpu::Push field for field (its size
//                is a static_assert), rl_pt_trace.{slang,comp} end with restirDi (PathTracerGpu::TracePush); the
//                C++ mirrors of the core's constants are static_asserts in restir_di.cpp.
//
// Usage: fuse_relight_restir_di_tests <suite>|all. Exit 0 pass, 1 fail.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/restir_di.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include "pt_reference_kernels.hpp"

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

/// Cornell box without glass plus one light of every analytic kind (one-sided rect, disk, cylinder, shaped sphere, delta
/// distant through the open front).
pt::PtScene manyLights() {
    pt::PtScene s = cornell(false);
    s.lights.push_back(rl::makeRectLight(lk::float3(-1.2f, 2.3f, 1.0f), lk::float3(0.4f, 0.f, 0.f),
                                         lk::float3(0.f, 0.f, -0.3f), lk::float3(4.f, 4.f, 5.f)));
    s.lights.push_back(rl::makeDiskLight(lk::float3(2.3f, 0.8f, 0.5f), lk::float3(0.f, 0.f, 0.3f),
                                         lk::float3(0.f, 0.3f, 0.f), lk::float3(3.f, 5.f, 3.f), true));
    s.lights.push_back(rl::makeCylinderLight(lk::float3(0.f, -1.9f, -1.8f), lk::float3(0.8f, 0.f, 0.f), 0.08f,
                                             lk::float3(8.f, 3.f, 2.f)));
    lk::RlLight spot = rl::makeSphereLight(lk::float3(-1.5f, 1.8f, -1.5f), 0.1f, lk::float3(40.f, 40.f, 30.f));
    rl::setShaping(spot, lk::float3(0.3f, -1.f, 0.4f), 0.5f, 0.1f, 1.f);
    s.lights.push_back(spot);
    s.lights.push_back(rl::makeDistantLight(lk::float3(0.2f, -0.35f, -1.f), 0.f, lk::float3(1.5f, 1.4f, 1.2f)));
    return s;
}

pt::RestirDiSettings diSettings() {
    pt::RestirDiSettings d;
    d.enabled = true;
    d.tileCount = 16;
    d.tileSize = 128;
    d.tileCandidates = 8;
    d.treeCandidates = 1;
    d.spatialSamples = 4;
    d.spatialIterations = 1;
    d.spatialRadius = 8.f;
    return d;
}

double lum3(const double* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

/// R independent ReSTIR runs of F frames (1 path per frame, frame f = sample index f): per pixel and channel the
/// mean over runs of each run's mean, the variance of the run means, and (lastVar) the variance over runs of the
/// last frame's single-sample estimate.
struct RunStats {
    std::vector<double> mean, var, lastMean, lastVar;
    bool ok = true;
};

RunStats runRestir(const pt::PtCompiledScene& c, const pt::PtSettings& st, const pt::RestirDiSettings& di, u32 w,
                   u32 h, u32 runs, u32 frames, u32 seedBase) {
    RunStats rs;
    const std::size_t n = std::size_t(w) * h * 3u;
    std::vector<double> s1(n, 0.0), s2(n, 0.0), l1(n, 0.0), l2(n, 0.0);
    pt::PtReferenceImage img, last;
    img.resize(w, h);
    last.resize(w, h);
    for (u32 r = 0; r < runs && rs.ok; ++r) {
        pt::RestirDiCpu restir;
        img.clear();
        const u32 seed = seedBase + r * 7919u;
        for (u32 f = 0; f < frames && rs.ok; ++f) {
            rs.ok = restir.frame(c, st, w, h, seed, f, di);
            pt::PtReferenceImage& target = f + 1u == frames ? last : img;
            if (f + 1u == frames) {
                last.clear();
            }
            rs.ok = rs.ok && pt::renderReference(c, pt::withRestirDi(st), w, h, seed, f, 1u, target,
                                                 kernel::Backend::CpuParallel, nullptr, restir.hook());
        }
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const std::size_t i = (std::size_t(y) * w + x) * 3u + ch;
                    const double lv = last.pixel(x, y).sum[ch];
                    const double m = (img.pixel(x, y).sum[ch] + lv) / double(frames);
                    s1[i] += m;
                    s2[i] += m * m;
                    l1[i] += lv;
                    l2[i] += lv * lv;
                }
            }
        }
    }
    rs.mean.assign(n, 0.0);
    rs.var.assign(n, 0.0);
    rs.lastMean.assign(n, 0.0);
    rs.lastVar.assign(n, 0.0);
    const double R = double(runs);
    for (std::size_t i = 0; i < n; ++i) {
        rs.mean[i] = s1[i] / R;
        rs.var[i] = std::max(0.0, (s2[i] - R * rs.mean[i] * rs.mean[i]) / (R - 1.0));
        rs.lastMean[i] = l1[i] / R;
        rs.lastVar[i] = std::max(0.0, (l2[i] - R * rs.lastMean[i] * rs.lastMean[i]) / (R - 1.0));
    }
    return rs;
}

/// Image-mean comparison per channel: |mean_a - mean_b| <= k sigma (the standard errors of the two image means;
/// `varA` / `varB` per pixel of one estimate, with nA / nB estimates).
bool imageMeansAgree(const char* name, u32 w, u32 h, const std::vector<double>& meanA, const std::vector<double>& varA,
                     double nA, const std::vector<double>& meanB, const std::vector<double>& varB, double nB,
                     double k, double* worstZ = nullptr) {
    bool ok = true;
    const double P = double(w) * h;
    double worst = 0.0;
    for (u32 ch = 0; ch < 3u; ++ch) {
        double ma = 0.0, mb = 0.0, va = 0.0, vb = 0.0;
        for (std::size_t p = 0; p < std::size_t(w) * h; ++p) {
            ma += meanA[p * 3u + ch];
            mb += meanB[p * 3u + ch];
            va += varA[p * 3u + ch] / nA;
            vb += varB[p * 3u + ch] / nB;
        }
        ma /= P;
        mb /= P;
        const double se = std::sqrt((va + vb) / (P * P));
        const double z = std::fabs(ma - mb) / std::max(se, 1e-12);
        worst = std::max(worst, z);
        std::printf("    %-16s ch %u: restir %.5f reference %.5f (se %.5f, z %.2f)\n", name, ch, ma, mb, se, z);
        ok = ok && z <= k;
    }
    if (worstZ != nullptr) {
        *worstZ = worst;
    }
    return ok;
}

// --- presample ------------------------------------------------------------------------------------------------------
void suitePresample() {
    pt::PtCompiledScene c;
    if (!compile(manyLights(), c)) {
        return;
    }
    pt::RestirDiLightTable t;
    check(t.build(c, 16.f), "presample: table usable");
    double sum = 0.0;
    bool monotone = true;
    u64 bins = 0;
    for (u32 l = 0; l < t.lightCount(); ++l) {
        sum += t.pmf()[l];
        monotone = monotone && (l == 0u || t.cdf()[l] >= t.cdf()[l - 1u]);
        const double lo = l == 0u ? 0.0 : std::ceil(double(t.cdf()[l - 1u]) * 16777216.0);
        const double hi = std::ceil(double(t.cdf()[l]) * 16777216.0);
        bins += u64(hi - lo);
        check(double(t.pmf()[l]) == (hi - lo) / 16777216.0, "presample: pmf is the exact 24-bit bin width");
    }
    std::printf("presample: %u lights, pmf sum %.9f, bins %llu\n", t.lightCount(), sum,
                static_cast<unsigned long long>(bins));
    check(sum == 1.0 && bins == 16777216ull, "presample: pmf sums to 1 exactly");
    check(monotone && t.cdf().back() == 1.f, "presample: CDF monotone, ends at 1");
    // One frame's tiles vs the pmf (chi-square over lights with an expected count >= 5, merged remainder).
    pt::RestirDiSettings di = diSettings();
    di.tileCount = 256;
    di.tileSize = 256;
    pt::RestirDiCpu r;
    pt::PtSettings st;
    st.maxBounces = 1;
    check(r.frame(c, st, 8, 8, 3u, 0u, di), "presample: frame");
    std::vector<double> counts(t.lightCount(), 0.0);
    for (u32 e : r.tiles()) {
        if (e < counts.size()) {
            counts[e] += 1.0;
        }
    }
    const double total = double(r.tiles().size());
    double chi2 = 0.0, restObs = 0.0, restExp = 0.0;
    u32 dof = 0;
    for (u32 l = 0; l < t.lightCount(); ++l) {
        const double e = total * t.pmf()[l];
        if (e >= 5.0) {
            chi2 += (counts[l] - e) * (counts[l] - e) / e;
            ++dof;
        } else {
            restObs += counts[l];
            restExp += e;
        }
    }
    if (restExp >= 5.0) {
        chi2 += (restObs - restExp) * (restObs - restExp) / restExp;
        ++dof;
    }
    // chi-square(dof - 1): mean dof - 1, sd sqrt(2 (dof - 1)); 5 sd.
    const double limit = double(dof - 1u) + 5.0 * std::sqrt(2.0 * double(dof - 1u));
    std::printf("presample: %u tile entries, chi2 %.1f over %u bins (limit %.1f)\n", u32(total), chi2, dof, limit);
    check(dof >= 2u && chi2 <= limit, "presample: tile entries follow the pmf");
}

// --- unbiased ---------------------------------------------------------------------------------------------------------
void unbiasedCase(const char* name, const pt::PtScene& s, u32 maxBounces) {
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    constexpr u32 W = 24, H = 24, kRuns = 24, kFrames = 6, kRefSpp = 2048;
    pt::PtSettings st;
    st.maxBounces = maxBounces;
    const pt::RestirDiSettings di = diSettings();
    const RunStats rs = runRestir(c, st, di, W, H, kRuns, kFrames, 11u);
    check(rs.ok, std::string("unbiased ") + name + ": frames");
    pt::PtReferenceImage ref;
    ref.resize(W, H);
    check(pt::renderReference(c, st, W, H, 777u, 0u, kRefSpp, ref), "unbiased: reference");
    std::vector<double> meanR, varR;
    referenceStats(ref, meanR, varR);
    std::printf("  unbiased %s (maxBounces %u, %u runs x %u frames vs %u spp):\n", name, maxBounces, kRuns, kFrames,
                kRefSpp);
    double worst = 0.0;
    const bool agree = imageMeansAgree(name, W, H, rs.mean, rs.var, kRuns, meanR, varR, kRefSpp, 3.0, &worst);
    const BlockResult b = compareBlocks(W, H, 4, rs.mean, rs.var, kRuns, meanR, varR, kRefSpp, 5.0, 1e-4);
    std::printf("    4x4 blocks: %u, worst z %.2f, failing (5 sigma) %u\n", b.blocks, b.worstZ, b.failing);
    bool finite = true;
    for (double v : rs.mean) {
        finite = finite && std::isfinite(v);
    }
    check(finite, std::string("unbiased ") + name + ": finite");
    check(agree, std::string("unbiased ") + name + ": image mean within 3 sigma of the RL-5.1 reference");
    check(b.failing == 0u, std::string("unbiased ") + name + ": 4x4 block means within 5 sigma");
}

void suiteUnbiased() {
    unbiasedCase("cornell", cornell(false), 3u);
    unbiasedCase("cornell+glass", cornell(true), 3u);
    unbiasedCase("many-lights", manyLights(), 1u);
}

// --- variance ---------------------------------------------------------------------------------------------------------
double sumLumVar(const std::vector<double>& var, std::size_t pixels) {
    double s = 0.0;
    for (std::size_t p = 0; p < pixels; ++p) {
        s += lum3(&var[p * 3u]);
    }
    return s;
}

void varianceCase(const char* name, const pt::PtScene& s) {
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    constexpr u32 W = 24, H = 24, kRuns = 24, kFrames = 5;
    pt::PtSettings st;
    st.maxBounces = 1;                // direct lighting only
    st.flags &= ~pt::kPtFlagJitter;   // pixel centres: the variance is the lighting estimator's, not the footprint's
    const pt::RestirDiSettings di = diSettings();
    const RunStats full = runRestir(c, st, di, W, H, kRuns, kFrames, 101u);
    pt::RestirDiSettings ris = di;
    ris.temporal = false;
    ris.spatialIterations = 0;
    const RunStats risOnly = runRestir(c, st, ris, W, H, kRuns, 1u, 301u);
    pt::PtReferenceImage plain;
    plain.resize(W, H);
    check(pt::renderReference(c, st, W, H, 55u, 0u, 256u, plain), "variance: plain NEE");
    std::vector<double> meanP, varP;
    referenceStats(plain, meanP, varP);
    const std::size_t pixels = std::size_t(W) * H;
    const double vRestir = sumLumVar(full.lastVar, pixels);
    const double vRis = sumLumVar(risOnly.lastVar, pixels);
    const double vPlain = sumLumVar(varP, pixels);
    std::printf("  variance %-12s (1 path, direct only; summed luminance variance): plain light-tree NEE %.4f, "
                "RIS-only %.4f, ReSTIR (frame %u) %.4f -> %.2fx / %.2fx lower\n",
                name, vPlain, vRis, kFrames, vRestir, vPlain / std::max(vRestir, 1e-12),
                vRis / std::max(vRestir, 1e-12));
    check(full.ok && risOnly.ok, std::string("variance ") + name + ": frames");
    check(vRestir < vPlain, std::string("variance ") + name + ": ReSTIR below plain light-tree sampling");
    check(vRestir < vRis, std::string("variance ") + name + ": ReSTIR below RIS-only at equal candidates");
}

void suiteVariance() {
    varianceCase("cornell", cornell(false));
    varianceCase("many-lights", manyLights());
}

// --- fast -------------------------------------------------------------------------------------------------------------
void suiteFast() {
    pt::PtCompiledScene c;
    if (!compile(manyLights(), c)) {
        return;
    }
    constexpr u32 W = 24, H = 24, kRuns = 6, kFrames = 6;
    pt::PtSettings st;
    st.maxBounces = 1;
    pt::RestirDiSettings di = diSettings();
    di.unbiased = false;
    const RunStats rs = runRestir(c, st, di, W, H, kRuns, kFrames, 21u);
    pt::PtReferenceImage ref;
    ref.resize(W, H);
    pt::renderReference(c, st, W, H, 99u, 0u, 256u, ref);
    double a = 0.0, b = 0.0;
    bool finite = true;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const std::size_t i = (std::size_t(y) * W + x) * 3u;
            a += lum3(&rs.mean[i]);
            b += 0.2126 * ref.mean(x, y, 0) + 0.7152 * ref.mean(x, y, 1) + 0.0722 * ref.mean(x, y, 2);
            finite = finite && std::isfinite(rs.mean[i]) && std::isfinite(rs.mean[i + 1u]) &&
                     std::isfinite(rs.mean[i + 2u]);
        }
    }
    const double rel = (a - b) / std::max(b, 1e-12);
    std::printf("fast (biased) mode: image luminance %.5f vs reference %.5f (relative bias %+.2f%%)\n", a / (W * H),
                b / (W * H), 100.0 * rel);
    check(rs.ok && finite, "fast: frames finite");
    check(std::fabs(rel) < 0.15, "fast: within 15% of the reference");
}

// --- determinism ------------------------------------------------------------------------------------------------------
void suiteDeterminism() {
    pt::PtCompiledScene c;
    if (!compile(manyLights(), c)) {
        return;
    }
    constexpr u32 W = 16, H = 12;
    pt::PtSettings st;
    st.maxBounces = 2;
    const pt::RestirDiSettings di = diSettings();
    pt::RestirDiCpu a, b;
    bool same = true;
    u32 surfaces = 0, samples = 0;
    for (u32 f = 0; f < 2u; ++f) {
        const bool ra = a.frame(c, st, W, H, 5u, f, di, kernel::Backend::CpuReference);
        const bool rb = b.frame(c, st, W, H, 5u, f, di, kernel::Backend::CpuParallel);
        check(ra && rb, "determinism: frames");
        auto eq = [](const std::vector<pt::Word>& x, const std::vector<pt::Word>& y) {
            return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(pt::Word)) == 0;
        };
        same = same && eq(a.surfaces(), b.surfaces()) && eq(a.reservoirs(), b.reservoirs()) &&
               eq(a.output(), b.output()) && a.tiles() == b.tiles();
        surfaces = a.stats().surfaces;
        samples = a.stats().samples;
    }
    pt::PtReferenceImage ia, ib;
    ia.resize(W, H);
    ib.resize(W, H);
    pt::renderReference(c, pt::withRestirDi(st), W, H, 5u, 1u, 1u, ia, kernel::Backend::CpuReference, nullptr, a.hook());
    pt::renderReference(c, pt::withRestirDi(st), W, H, 5u, 1u, 1u, ib, kernel::Backend::CpuParallel, nullptr, b.hook());
    bool img = true;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            img = img && std::memcmp(ia.pixel(x, y).sum, ib.pixel(x, y).sum, sizeof(double) * 3u) == 0;
        }
    }
    std::printf("determinism: CpuReference vs CpuParallel %s (%u ReSTIR surfaces, %u reservoirs with a sample), path "
                "tracer output %s\n",
                same ? "bit-identical" : "DIFFER", surfaces, samples, img ? "bit-identical" : "DIFFER");
    check(same, "determinism: ReSTIR buffers CpuReference == CpuParallel");
    check(img, "determinism: path tracer with the hook CpuReference == CpuParallel");
    check(surfaces > W * H / 2u && samples > W * H / 2u, "determinism: most pixels carry a ReSTIR surface / sample");
}

// --- zero_alloc -------------------------------------------------------------------------------------------------------
void suiteZeroAlloc() {
    pt::PtScene s = manyLights();
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    pt::PtSettings st;
    st.maxBounces = 1;
    const pt::RestirDiSettings di = diSettings();
    pt::RestirDiCpu r;
    pt::RestirDiLightTable table;
    bool ok = true;
    for (u32 f = 0; f < 3u; ++f) {
        ok = ok && r.frame(c, st, 12, 12, 1u, f, di) && table.build(c, 16.f);
    }
    unsigned long long hostAllocs = 0, frameAllocs = 0;
    for (u32 f = 3; f < 11u; ++f) {
        s.lights[0].position.x = 0.9f - 0.02f * float(f % 3u);
        ok = ok && c.update(s);
        g_allocations.store(0);
        g_count.store(true);
        ok = table.build(c, 16.f) && ok;
        pt::Word params[pt::kRdiParamWords];
        pt::packRestirDiParams(di, 7u, di.tileCount, f, params);
        g_count.store(false);
        hostAllocs += g_allocations.load();
        g_allocations.store(0);
        g_count.store(true);
        ok = r.frame(c, st, 12, 12, 1u, f, di) && ok;
        g_count.store(false);
        frameAllocs += g_allocations.load();
    }
    std::printf("zero_alloc: 8 steady-state frames: light table + params %llu operator-new calls; RestirDiCpu::frame "
                "%llu (informational: the WP-6.0 CPU BVH oracle allocates per ray)\n",
                hostAllocs, frameAllocs);
    check(ok, "zero_alloc: frames");
    check(hostAllocs == 0u, "zero_alloc: steady-state ReSTIR host work makes no heap allocation");
}

// --- layout ---------------------------------------------------------------------------------------------------------
/// Field names of the push-constant block `name` in a shader source, in order.
std::vector<std::string> pushFields(const std::string& path, const char* name) {
    std::vector<std::string> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return out;
    }
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
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
        std::string decl = text.substr(p, semi - p);
        const std::size_t nl = decl.rfind('\n');
        std::size_t end = decl.size();
        while (end > 0 && (decl[end - 1] == ' ')) {
            --end;
        }
        std::size_t begin = end;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(decl[begin - 1])) || decl[begin - 1] == '_')) {
            --begin;
        }
        (void)nl;
        out.push_back(decl.substr(begin, end - begin));
        p = text.find('\n', semi);
        p = p == std::string::npos ? close : p + 1u;
    }
    return out;
}

void suiteLayout() {
    const std::string dir = FUSE_RL_PT_SHADER_DIR;
    const std::vector<std::string> rdi = {"params",  "instances", "triangles", "materials", "portals",  "lightMap",
                                          "lightTable", "lightTree", "tlas",   "lut",       "restirDi", "block",
                                          "src",     "dst",       "width",     "height",    "lightCount", "stage"};
    const std::vector<std::string> a = pushFields(dir + "/restir_di.slang", "struct RdiPush");
    const std::vector<std::string> b = pushFields(dir + "/restir_di.comp", "uniform RdiPush");
    const std::vector<std::string> ta = pushFields(dir + "/rl_pt_trace.slang", "struct PtTracePush");
    const std::vector<std::string> tb = pushFields(dir + "/rl_pt_trace.comp", "uniform PtTracePush");
    std::printf("layout: RdiPush slang %zu / glsl %zu fields (C++ RestirDiGpu::Push: 14 u64 + 4 u32 = 128 bytes); "
                "PtTracePush slang %zu / glsl %zu fields, last %s\n",
                a.size(), b.size(), ta.size(), tb.size(), tb.empty() ? "-" : tb.back().c_str());
    check(a == rdi && b == rdi, "layout: RdiPush (restir_di.slang / .comp) == RestirDiGpu::Push field order");
    check(ta.size() == 17u && ta == tb && ta.back() == "restirDi",
          "layout: PtTracePush (rl_pt_trace.slang / .comp) ends with restirDi (PathTracerGpu::TracePush, 120 bytes)");
}

// --- options ----------------------------------------------------------------------------------------------------------
void suiteOptions() {
    const pt::RestirDiSettings d = pt::RestirDiSettings::fromOptions();
    const char* names[] = {"rtx.useRTXDI",
                           "rtx.di.initialSampleCount",
                           "rtx.di.lightTreeSampleCount",
                           "rtx.di.bsdfSampleCount",
                           "rtx.di.spatialSamples",
                           "rtx.di.spatialIterations",
                           "rtx.di.spatialRadius",
                           "rtx.di.maxHistoryLength",
                           "rtx.di.enableTemporalReuse",
                           "rtx.di.enableInitialVisibility",
                           "rtx.di.enableRayTracedBiasCorrection",
                           "rtx.di.lightTileCount",
                           "rtx.di.lightTileSize"};
    u32 found = 0;
    for (const char* n : names) {
        found += opt::OptionManager::findOption(n) != nullptr ? 1u : 0u;
    }
    std::printf("options: %u/%zu registered; defaults: enabled %d, tiles %u x %u, candidates %u + %u, spatial %u x %u "
                "(r %.0f), history %.0f, unbiased %d\n",
                found, sizeof(names) / sizeof(names[0]), d.enabled ? 1 : 0, d.tileCount, d.tileSize, d.tileCandidates,
                d.treeCandidates, d.spatialIterations, d.spatialSamples, d.spatialRadius, d.maxHistory,
                d.unbiased ? 1 : 0);
    check(found == sizeof(names) / sizeof(names[0]), "options: rtx.useRTXDI / rtx.di.* registered");
    check(!d.enabled && d.unbiased && d.tileCandidates == 8u && d.spatialSamples == 4u, "options: defaults");
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    (void)pt::RestirDiOptions::useRTXDIObject();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"presample", suitePresample},     {"unbiased", suiteUnbiased}, {"variance", suiteVariance},
                            {"fast", suiteFast},               {"determinism", suiteDeterminism},
                            {"zero_alloc", suiteZeroAlloc},    {"options", suiteOptions},
                            {"layout", suiteLayout}};
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
    std::printf("PASS: rl_restir_di %s\n", suite.c_str());
    return 0;
}
