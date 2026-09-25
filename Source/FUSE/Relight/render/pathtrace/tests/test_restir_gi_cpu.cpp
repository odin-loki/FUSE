// FUSE Relight RL-5.3 CPU gates: ReSTIR GI (restir_gi.hpp, the core restir_gi_core.h) on the RL-5.1 CPU reference path
// tracer's scenes.
//
//   unbiased     unbiased mode (temporal + spatial reuse with visibility-tested pairwise MIS) driving the path
//                tracer's indirect light: over 24 independent runs of 6 frames the image mean (per channel) lies within
//                3 sigma of the RL-5.1 reference (plain path tracing, 2048 spp) and 4x4 block means within 5 sigma, on
//                the Cornell scene (emissive triangles, sphere light, metal, mirror PSR, alpha-tested panel), Cornell +
//                glass (dirac lobes at x1 / x2: the residual), Cornell open to a sky with a blended unlit layer
//                (escape-direction samples, the pass-through residual) and Cornell with ReSTIR DI (RL-5.2) on too.
//   variance     pixel centres: the per-pixel variance (across 24 runs) of one frame's GI estimate after 5 frames of
//                reuse, summed over the image, is below the same estimator without reuse (= plain path tracing of the
//                indirect light: one continuation per pixel); the full image's variance is below plain path tracing
//                at 1 spp too.
//   fast         biased mode runs, stays finite and within 15% of the reference mean (bias reported).
//   determinism  RestirGiCpu on CpuReference == CpuParallel bit for bit (surfaces, reservoirs, output) over two
//                frames; the path tracer's output with the hook equal too.
//   zero_alloc   steady-state host work (params packing, settings) makes no heap allocation; the stage kernels'
//                count is reported (the WP-6.0 CPU BVH oracle allocates per ray, as in rl_pt_zero_alloc).
//   options      rtx.useReSTIRGI / rtx.restirGI.* are registered and RestirGiSettings::fromOptions reads them.
//   layout       the push-constant blocks of restir_gi.{slang,comp} match RestirGiGpu::Push field for field, and
//                rl_pt_trace.{slang,comp} end with restirDi, restirGi (PathTracerGpu::TracePush).
//
// Usage: fuse_relight_restir_gi_tests <suite>|all. Exit 0 pass, 1 fail.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/restir_di.hpp>
#include <fuse/relight/render/pathtrace/restir_gi.hpp>

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

/// Cornell (no glass) open to a blue sky, with a blended unlit layer (vertex alpha 0.4) in the box.
pt::PtScene skyLayer() {
    pt::PtScene s = cornell(false);
    s.sky[0] = 0.25f;
    s.sky[1] = 0.35f;
    s.sky[2] = 0.6f;
    pt::PtMaterial layer = unlit(0.9f, 0.5f, 0.2f);
    layer.flags |= pt::kPtMatVertexColor | pt::kPtMatAlphaBlend;
    s.materials.push_back(layer);
    const float c[3] = {0.6f, -1.2f, 0.6f}, u[3] = {0.f, 0.f, 0.6f}, v[3] = {0.f, 0.6f, 0.f};
    pt::PtMesh m = quad(c, u, v, static_cast<u32>(s.materials.size() - 1u));
    m.colors = {1.f, 1.f, 1.f, 0.4f, 1.f, 1.f, 1.f, 0.4f, 1.f, 1.f, 1.f, 0.4f, 1.f, 1.f, 1.f, 0.4f};
    pt::PtInstance inst;
    inst.mesh = static_cast<u32>(s.meshes.size());
    inst.objectToWorld = pt::identity34();
    s.meshes.push_back(std::move(m));
    s.instances.push_back(inst);
    return s;
}

pt::RestirGiSettings giSettings() {
    pt::RestirGiSettings g;
    g.enabled = true;
    g.spatialSamples = 4;
    g.spatialIterations = 1;
    g.spatialRadius = 8.f;
    return g;
}

pt::RestirDiSettings diSettings() {
    pt::RestirDiSettings d;
    d.enabled = true;
    d.tileCount = 16;
    d.tileSize = 128;
    d.spatialRadius = 8.f;
    return d;
}

double lum3(const double* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

/// R independent ReSTIR GI runs of F frames (1 path per frame, frame f = sample index f): per pixel and channel the
/// mean over runs of each run's mean, the variance of the run means, (lastVar) the variance over runs of the last
/// frame's single-sample estimate and (giVar) of the last frame's GI output.
struct RunStats {
    std::vector<double> mean, var, lastMean, lastVar, giMean, giVar;
    bool ok = true;
};

RunStats runRestir(const pt::PtCompiledScene& c, const pt::PtSettings& st, const pt::RestirGiSettings& gi, u32 w,
                   u32 h, u32 runs, u32 frames, u32 seedBase, bool withDi = false) {
    RunStats rs;
    const std::size_t n = std::size_t(w) * h * 3u;
    std::vector<double> s1(n, 0.0), s2(n, 0.0), l1(n, 0.0), l2(n, 0.0), g1(n, 0.0), g2(n, 0.0);
    pt::PtReferenceImage img, last;
    img.resize(w, h);
    last.resize(w, h);
    const pt::PtSettings stGi = withDi ? pt::withRestirDi(pt::withRestirGi(st)) : pt::withRestirGi(st);
    const pt::PtSettings stFrame = withDi ? pt::withRestirDi(st) : st;
    const pt::RestirDiSettings di = diSettings();
    for (u32 r = 0; r < runs && rs.ok; ++r) {
        pt::RestirGiCpu restir;
        pt::RestirDiCpu restirDi;
        img.clear();
        const u32 seed = seedBase + r * 7919u;
        for (u32 f = 0; f < frames && rs.ok; ++f) {
            if (withDi) {
                rs.ok = restirDi.frame(c, st, w, h, seed, f, di);
            }
            rs.ok = rs.ok && restir.frame(c, stFrame, w, h, seed, f, gi);
            pt::PtReferenceImage& target = f + 1u == frames ? last : img;
            if (f + 1u == frames) {
                last.clear();
            }
            rs.ok = rs.ok && pt::renderReference(c, stGi, w, h, seed, f, 1u, target, kernel::Backend::CpuParallel,
                                                 nullptr, withDi ? restirDi.hook() : nullptr, restir.hook());
        }
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const std::size_t p = std::size_t(y) * w + x;
                const pt::Word o = restir.output()[p * 2u + 1u];
                const double gv[3] = {o.x, o.y, o.z};
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const std::size_t i = p * 3u + ch;
                    const double lv = last.pixel(x, y).sum[ch];
                    const double m = (img.pixel(x, y).sum[ch] + lv) / double(frames);
                    s1[i] += m;
                    s2[i] += m * m;
                    l1[i] += lv;
                    l2[i] += lv * lv;
                    g1[i] += gv[ch];
                    g2[i] += gv[ch] * gv[ch];
                }
            }
        }
    }
    const double R = double(runs);
    auto finish = [&](const std::vector<double>& a, const std::vector<double>& b, std::vector<double>& mean,
                      std::vector<double>& var) {
        mean.assign(n, 0.0);
        var.assign(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            mean[i] = a[i] / R;
            var[i] = std::max(0.0, (b[i] - R * mean[i] * mean[i]) / (R - 1.0));
        }
    };
    finish(s1, s2, rs.mean, rs.var);
    finish(l1, l2, rs.lastMean, rs.lastVar);
    finish(g1, g2, rs.giMean, rs.giVar);
    return rs;
}

/// Image-mean comparison per channel: |mean_a - mean_b| <= k sigma (the standard errors of the two image means).
bool imageMeansAgree(const char* name, u32 w, u32 h, const std::vector<double>& meanA, const std::vector<double>& varA,
                     double nA, const std::vector<double>& meanB, const std::vector<double>& varB, double nB,
                     double k) {
    bool ok = true;
    const double P = double(w) * h;
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
        std::printf("    %-16s ch %u: restir %.5f reference %.5f (se %.5f, z %.2f)\n", name, ch, ma, mb, se, z);
        ok = ok && z <= k;
    }
    return ok;
}

// --- unbiased ---------------------------------------------------------------------------------------------------------
void unbiasedCase(const char* name, const pt::PtScene& s, u32 maxBounces, bool withDi, bool reuse = true,
                  u32 kRuns = 24) {
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    constexpr u32 W = 24, H = 24, kFrames = 6, kRefSpp = 2048;
    pt::PtSettings st;
    st.maxBounces = maxBounces;
    pt::RestirGiSettings gi = giSettings();
    if (!reuse) {
        gi.temporal = false;
        gi.spatialIterations = 0;
    }
    const RunStats rs = runRestir(c, st, gi, W, H, kRuns, kFrames, 11u, withDi);
    check(rs.ok, std::string("unbiased ") + name + ": frames");
    pt::PtReferenceImage ref;
    ref.resize(W, H);
    check(pt::renderReference(c, st, W, H, 777u, 0u, kRefSpp, ref), "unbiased: reference");
    std::vector<double> meanR, varR;
    referenceStats(ref, meanR, varR);
    std::printf("  unbiased %s (maxBounces %u%s, %u runs x %u frames vs %u spp):\n", name, maxBounces,
                withDi ? ", + ReSTIR DI" : "", kRuns, kFrames, kRefSpp);
    const bool agree = imageMeansAgree(name, W, H, rs.mean, rs.var, kRuns, meanR, varR, kRefSpp, 3.0);
    const BlockResult b = compareBlocks(W, H, 4, rs.mean, rs.var, kRuns, meanR, varR, kRefSpp, 5.0, 1e-4);
    std::printf("    4x4 blocks: %u, worst z %.2f, failing (5 sigma) %u\n", b.blocks, b.worstZ, b.failing);
    if (std::getenv("RGI_DEBUG") != nullptr) {
        for (u32 by = 0; by < H; by += 4) {
            for (u32 bx = 0; bx < W; bx += 4) {
                double ma = 0, mb = 0, va = 0, vb = 0;
                for (u32 y = by; y < by + 4; ++y) {
                    for (u32 x = bx; x < bx + 4; ++x) {
                        const std::size_t i = (std::size_t(y) * W + x) * 3u + 1u;
                        ma += rs.mean[i] / 16;
                        mb += meanR[i] / 16;
                        va += rs.var[i] / kRuns / 256;
                        vb += varR[i] / kRefSpp / 256;
                    }
                }
                const double z = (ma - mb) / std::sqrt(va + vb);
                if (std::fabs(z) > 3.0) {
                    std::printf("      block (%u,%u) g: restir %.5f ref %.5f z %.2f\n", bx, by, ma, mb, z);
                }
            }
        }
    }
    bool finite = true;
    for (double v : rs.mean) {
        finite = finite && std::isfinite(v);
    }
    check(finite, std::string("unbiased ") + name + ": finite");
    check(agree, std::string("unbiased ") + name + ": image mean within 3 sigma of the RL-5.1 reference");
    check(b.failing == 0u, std::string("unbiased ") + name + ": 4x4 block means within 5 sigma");
}

/// The split alone (no reuse: the initial sample + residual is the path tracer's own estimator of the indirect light).
void suiteSplit() {
    unbiasedCase("split cornell", cornell(false), 3u, false, false, 96u);
    unbiasedCase("split glass", cornell(true), 4u, false, false, 96u);
}

/// Diagnostic (not a ctest gate): the unbiased reuse at 4x the runs.
void suiteDeep() {
    unbiasedCase("deep cornell", cornell(false), 3u, false, true, 96u);
}

void suiteUnbiased() {
    unbiasedCase("cornell", cornell(false), 3u, false);
    unbiasedCase("cornell+glass", cornell(true), 4u, false);
    unbiasedCase("sky+layer", skyLayer(), 3u, false);
    unbiasedCase("cornell+di", cornell(false), 3u, true);
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
    st.maxBounces = 3;
    st.flags &= ~pt::kPtFlagJitter; // pixel centres: the variance is the lighting estimator's, not the footprint's
    const pt::RestirGiSettings gi = giSettings();
    const RunStats full = runRestir(c, st, gi, W, H, kRuns, kFrames, 101u);
    pt::RestirGiSettings none = gi;
    none.temporal = false;
    none.spatialIterations = 0;
    const RunStats plainGi = runRestir(c, st, none, W, H, kRuns, 1u, 301u);
    pt::PtReferenceImage plain;
    plain.resize(W, H);
    check(pt::renderReference(c, st, W, H, 55u, 0u, 256u, plain), "variance: plain path tracing");
    std::vector<double> meanP, varP;
    referenceStats(plain, meanP, varP);
    const std::size_t pixels = std::size_t(W) * H;
    const double vGi = sumLumVar(full.giVar, pixels);
    const double vGiPlain = sumLumVar(plainGi.giVar, pixels);
    const double vImg = sumLumVar(full.lastVar, pixels);
    const double vImgPlain = sumLumVar(varP, pixels);
    std::printf("  variance %-8s (1 path, summed luminance variance): indirect estimate: no reuse (plain path "
                "tracing) %.4f, ReSTIR GI (frame %u) %.4f -> %.2fx lower; image: plain PT %.4f, ReSTIR GI %.4f -> "
                "%.2fx lower\n",
                name, vGiPlain, kFrames, vGi, vGiPlain / std::max(vGi, 1e-12), vImgPlain, vImg,
                vImgPlain / std::max(vImg, 1e-12));
    check(full.ok && plainGi.ok, std::string("variance ") + name + ": frames");
    check(vGi < vGiPlain, std::string("variance ") + name + ": indirect variance below plain path tracing");
    check(vImg < vImgPlain, std::string("variance ") + name + ": image variance below plain path tracing");
}

void suiteVariance() {
    varianceCase("cornell", cornell(false));
    varianceCase("sky", skyLayer());
}

// --- fast -------------------------------------------------------------------------------------------------------------
void suiteFast() {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    constexpr u32 W = 24, H = 24, kRuns = 6, kFrames = 6;
    pt::PtSettings st;
    st.maxBounces = 3;
    pt::RestirGiSettings gi = giSettings();
    gi.unbiased = false;
    const RunStats rs = runRestir(c, st, gi, W, H, kRuns, kFrames, 21u);
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
    if (!compile(cornell(true), c)) {
        return;
    }
    constexpr u32 W = 16, H = 12;
    pt::PtSettings st;
    st.maxBounces = 3;
    const pt::RestirGiSettings gi = giSettings();
    pt::RestirGiCpu a, b;
    bool same = true;
    u32 surfaces = 0, samples = 0;
    for (u32 f = 0; f < 2u; ++f) {
        const bool ra = a.frame(c, st, W, H, 5u, f, gi, kernel::Backend::CpuReference);
        const bool rb = b.frame(c, st, W, H, 5u, f, gi, kernel::Backend::CpuParallel);
        check(ra && rb, "determinism: frames");
        auto eq = [](const std::vector<pt::Word>& x, const std::vector<pt::Word>& y) {
            return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(pt::Word)) == 0;
        };
        same = same && eq(a.surfaces(), b.surfaces()) && eq(a.reservoirs(), b.reservoirs()) &&
               eq(a.output(), b.output()) && eq(a.initialReservoirs(), b.initialReservoirs());
        surfaces = a.stats().surfaces;
        samples = a.stats().samples;
    }
    pt::PtReferenceImage ia, ib;
    ia.resize(W, H);
    ib.resize(W, H);
    pt::renderReference(c, pt::withRestirGi(st), W, H, 5u, 1u, 1u, ia, kernel::Backend::CpuReference, nullptr, nullptr,
                        a.hook());
    pt::renderReference(c, pt::withRestirGi(st), W, H, 5u, 1u, 1u, ib, kernel::Backend::CpuParallel, nullptr, nullptr,
                        b.hook());
    bool img = true;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            img = img && std::memcmp(ia.pixel(x, y).sum, ib.pixel(x, y).sum, sizeof(double) * 3u) == 0;
        }
    }
    std::printf("determinism: CpuReference vs CpuParallel %s (%u GI surfaces, %u reservoirs with a sample), path "
                "tracer output %s\n",
                same ? "bit-identical" : "DIFFER", surfaces, samples, img ? "bit-identical" : "DIFFER");
    check(same, "determinism: ReSTIR GI buffers CpuReference == CpuParallel");
    check(img, "determinism: path tracer with the hook CpuReference == CpuParallel");
    check(surfaces > W * H / 2u && samples > W * H / 2u, "determinism: most pixels carry a GI surface / sample");
}

// --- zero_alloc -------------------------------------------------------------------------------------------------------
void suiteZeroAlloc() {
    pt::PtScene s = cornell(false);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    pt::PtSettings st;
    st.maxBounces = 3;
    const pt::RestirGiSettings gi = giSettings();
    pt::RestirGiCpu r;
    bool ok = true;
    for (u32 f = 0; f < 3u; ++f) {
        ok = ok && r.frame(c, st, 12, 12, 1u, f, gi);
    }
    unsigned long long hostAllocs = 0, frameAllocs = 0;
    for (u32 f = 3; f < 11u; ++f) {
        s.lights[0].position.x = 0.9f - 0.02f * float(f % 3u);
        ok = ok && c.update(s);
        g_allocations.store(0);
        g_count.store(true);
        pt::Word params[pt::kRgiParamWords];
        pt::packRestirGiParams(gi, 7u, f, params);
        const pt::RestirGiSettings fromOptions = pt::RestirGiSettings::fromOptions();
        (void)fromOptions;
        g_count.store(false);
        hostAllocs += g_allocations.load();
        g_allocations.store(0);
        g_count.store(true);
        ok = r.frame(c, st, 12, 12, 1u, f, gi) && ok;
        g_count.store(false);
        frameAllocs += g_allocations.load();
    }
    std::printf("zero_alloc: 8 steady-state frames: params + settings %llu operator-new calls; RestirGiCpu::frame "
                "%llu (informational: the WP-6.0 CPU BVH oracle allocates per ray)\n",
                hostAllocs, frameAllocs);
    check(ok, "zero_alloc: frames");
    check(hostAllocs == 0u, "zero_alloc: steady-state ReSTIR GI host work makes no heap allocation");
}

// --- layout ---------------------------------------------------------------------------------------------------------
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
    const std::string dir = FUSE_RL_PT_SHADER_DIR;
    const std::vector<std::string> rgi = {"params",  "instances", "triangles", "materials", "portals",  "lightMap",
                                          "lightTable", "lightTree", "tlas",   "lut",       "restirDi", "block",
                                          "src",     "dst",       "width",     "height",    "lightCount", "stage"};
    const std::vector<std::string> a = pushFields(dir + "/restir_gi.slang", "struct RgiPush");
    const std::vector<std::string> b = pushFields(dir + "/restir_gi.comp", "uniform RgiPush");
    const std::vector<std::string> ta = pushFields(dir + "/rl_pt_trace.slang", "struct PtTracePush");
    const std::vector<std::string> tb = pushFields(dir + "/rl_pt_trace.comp", "uniform PtTracePush");
    std::printf("layout: RgiPush slang %zu / glsl %zu fields (C++ RestirGiGpu::Push: 14 u64 + 4 u32 = 128 "
                "bytes); PtTracePush slang %zu / glsl %zu fields, last %s\n",
                a.size(), b.size(), ta.size(), tb.size(), tb.empty() ? "-" : tb.back().c_str());
    check(a == rgi && b == rgi, "layout: RgiPush (restir_gi.slang / .comp) == RestirGiGpu::Push field order");
    check(ta.size() == 18u && ta == tb && ta[16] == "restirDi" && ta.back() == "restirGi",
          "layout: PtTracePush (rl_pt_trace.slang / .comp) ends with restirDi, restirGi (TracePush, 128 bytes)");
}

// --- options ----------------------------------------------------------------------------------------------------------
void suiteOptions() {
    const pt::RestirGiSettings d = pt::RestirGiSettings::fromOptions();
    const char* names[] = {"rtx.useReSTIRGI",
                           "rtx.restirGI.useTemporalReuse",
                           "rtx.restirGI.useSpatialReuse",
                           "rtx.restirGI.biasCorrectionMode",
                           "rtx.restirGI.temporalHistoryLength",
                           "rtx.restirGI.spatialSamples",
                           "rtx.restirGI.spatialIterations",
                           "rtx.restirGI.spatialRadius"};
    u32 found = 0;
    for (const char* n : names) {
        found += opt::OptionManager::findOption(n) != nullptr ? 1u : 0u;
    }
    std::printf("options: %u/%zu registered; defaults: enabled %d, temporal %d, spatial %u x %u (r %.0f), history "
                "%.0f, unbiased %d\n",
                found, sizeof(names) / sizeof(names[0]), d.enabled ? 1 : 0, d.temporal ? 1 : 0, d.spatialIterations,
                d.spatialSamples, d.spatialRadius, d.maxHistory, d.unbiased ? 1 : 0);
    check(found == sizeof(names) / sizeof(names[0]), "options: rtx.useReSTIRGI / rtx.restirGI.* registered");
    check(!d.enabled && d.unbiased && d.temporal && d.spatialSamples == 4u && d.spatialIterations == 1u,
          "options: defaults");
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    (void)pt::RestirGiOptions::useReSTIRGIObject();
    (void)pt::RestirDiOptions::useRTXDIObject();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"split", suiteSplit},             {"deep", suiteDeep},
                            {"unbiased", suiteUnbiased},
                            {"variance", suiteVariance},
                            {"fast", suiteFast},               {"determinism", suiteDeterminism},
                            {"zero_alloc", suiteZeroAlloc},    {"options", suiteOptions},
                            {"layout", suiteLayout}};
    bool ran = false;
    for (const Suite& s : suites) {
        if ((suite == "all" && std::strcmp(s.name, "deep") != 0) || suite == s.name) {
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
    std::printf("PASS: rl_restir_gi %s\n", suite.c_str());
    return 0;
}
