// RL-5.5 radiance denoiser: CPU gates (stub-safe). The Lavapipe gates are test_rp_rdn.cpp; the Relight gates on the
// real path tracer are Relight/render/denoise/tests (rl_denoise_*).
//
// Scene: rdn_synthetic.hpp (analytic moving-camera mirror scene with a converged reference). Metrics over the surface
// pixels of the last kEval frames: noisy MSE / denoised MSE ("error reduction") and the relative bias of the denoised
// mean against the converged mean, per channel (luminance).
//
//   layout        RdnFrame / RdnPush sizes and offsets; rdn.comp / rdn.slang include the single-source core and declare
//                 the push block in RdnPush's order; RdnBufferLayout sections 256-aligned and disjoint.
//   backends      CpuReference == CpuParallel (0, 2, 4 workers) bit for bit over 6 frames (every feature on).
//   quality       error reduction >= kMinReduction (the plan's rl_denoise_* bar: 4x) and |bias| <= kMaxBias for diffuse
//                 and specular, moving camera + moving occluder + A-SVGF, and the static scene.
//   ghosting      moving camera over the mirror floor: the specular error of the reflection with virtual (hit-distance)
//                 motion is at most kMaxGhostRatio x the error with surface motion only (with and without clipping).
//   disocclusion  the occluder's trail restarts its history (length 1 after the temporal pass), stable pixels keep it;
//                 frame 0 and reset() start every pixel over.
//   asvgf         after a light step (x 0.25 at frame 12) A-SVGF's mean relative error over the next 4 frames is at most
//                 half of the gradient-free run's; before the step it is no worse than 1.25 x.
//   firefly       2 per mille x 200 diffuse outliers: the suppression (temporal + spatial) cuts the diffuse error >= 2x.
//   determinism   two runs bit-identical.
//   zero_alloc    steady-state frames (CpuReference and CpuParallel) make no heap allocation.
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/denoise/radiance_denoise.hpp>
#include <fuse/renderer/denoise/rdn_synthetic.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
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

using namespace fuse;
using namespace fuse::renderer::denoise;
using rdnk::float4;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kFrames = 24u;
constexpr u32 kEval = 8u;
constexpr f64 kMinReduction = 4.0; ///< the plan's rl_denoise_* bar
constexpr f64 kMaxBias = 0.06; ///< |mean(denoised) - mean(truth)| / mean(truth)
constexpr f64 kMaxGhostRatio = 0.8; ///< virtual-motion specular error / surface-motion specular error

f64 lum(const float4& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

struct ChannelStats {
    f64 noisySe = 0.0;
    f64 dnSe = 0.0;
    f64 dnSum = 0.0;
    f64 truthSum = 0.0;
    f64 count = 0.0;
    f64 reduction() const { return dnSe > 0.0 ? noisySe / dnSe : 1e30; }
    f64 bias() const { return truthSum > 0.0 ? (dnSum - truthSum) / truthSum : 0.0; }
    f64 rmse() const { return count > 0.0 ? std::sqrt(dnSe / count) : 0.0; }
};

/// `mask` (optional): surface kinds counted (bit per RdnSyntheticSurface).
void accumulate(const RdnSyntheticFrame& f, const float4* dn, const std::vector<float4>& noisy,
                const std::vector<float4>& truth, ChannelStats& st, u32 mask = 0xEu) {
    const usize n = static_cast<usize>(f.width) * f.height;
    for (usize i = 0; i < n; ++i) {
        if (((1u << f.surface[i]) & mask) == 0u) {
            continue;
        }
        const f64 t = lum(truth[i]);
        const f64 a = lum(noisy[i]) - t;
        const f64 b = lum(dn[i]) - t;
        st.noisySe += a * a;
        st.dnSe += b * b;
        st.dnSum += lum(dn[i]);
        st.truthSum += t;
        st.count += 1.0;
    }
}

struct RunResult {
    ChannelStats d;
    ChannelStats s;
};

RdnSettings settingsFor(bool gradients) {
    RdnSettings s = rdn_preset();
    s.motionScale = 1.f; // the synthetic motion is previous - current (the Relight convention)
    s.gradients = gradients;
    s.instanceTest = true;
    return s;
}

RunResult run(const RdnSyntheticDesc& d, const RdnSettings& s, u32 frames, u32 evalFrom, u32 seed, u32 maskS = 0xEu,
              kernel::Backend backend = kernel::Backend::CpuParallel) {
    RdnReference ref;
    ref.init(d.width, d.height, s);
    RdnSyntheticFrame f;
    RunResult r{};
    for (u32 t = 0; t < frames; ++t) {
        rdn_synthetic_frame(d, t, seed, f);
        expect(ref.runFrame(f.inputs(s.gradients, s.instanceTest), f.camera, f.prevCamera, backend), "runFrame");
        if (t >= evalFrom) {
            accumulate(f, ref.outputD(), f.diffuse, f.truthD, r.d);
            accumulate(f, ref.outputS(), f.specular, f.truthS, r.s, maskS);
        }
    }
    return r;
}

// --- layout ----------------------------------------------------------------------------------------------------------
std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testLayout() {
    expect(sizeof(RdnFrame) == 520u, "sizeof(RdnFrame) == 520");
    expect(sizeof(RdnPush) == 64u, "sizeof(RdnPush) == 64");
    expect(offsetof(RdnPush, pass) == 56u && offsetof(RdnPush, step) == 60u, "RdnPush pass / step offsets");
    const std::string dir = FUSE_RP_RDN_SHADER_DIR;
    for (const char* file : {"/rdn.comp", "/rdn.slang"}) {
        const std::string text = readFile(dir + file);
        expect(text.find("#include \"rdn_core.h\"") != std::string::npos, "kernel includes the single-source core");
        expect(text.find("struct RdnFrame") == std::string::npos, "kernel does not redeclare RdnFrame");
        // Push block order: frame, a0..a5, pass, stp.
        const char* fields[] = {"uint64_t frame;", "uint64_t a0;", "uint64_t a1;", "uint64_t a2;", "uint64_t a3;",
                                "uint64_t a4;",    "uint64_t a5;", "uint pass;",   "uint stp;"};
        usize at = 0;
        bool ordered = true;
        for (const char* field : fields) {
            const usize p = text.find(field, at);
            ordered = ordered && p != std::string::npos;
            at = p == std::string::npos ? at : p;
        }
        std::printf("layout: %s push block in RdnPush order: %s\n", file + 1, ordered ? "yes" : "NO");
        expect(ordered, "push block fields in RdnPush order");
    }
    for (const bool keep : {false, true}) {
        const RdnBufferLayout l = RdnBufferLayout::compute(61u, 37u, keep);
        const u64 n = 61u * 37u * 16u;
        const u64 state[] = {l.guide[0], l.guide[1], l.aux[0],   l.aux[1], l.histD[0],
                             l.histD[1], l.histS[0], l.histS[1], l.mom[0], l.mom[1]};
        bool ok = true;
        for (u32 a = 0; a < 10u; ++a) {
            ok = ok && state[a] % 256u == 0u && state[a] + n <= l.stateBytes;
            for (u32 b = a + 1u; b < 10u; ++b) {
                ok = ok && (state[a] + n <= state[b] || state[b] + n <= state[a]);
            }
        }
        const u64 work[] = {l.preD, l.preS, l.blurD, l.blurS, l.accD, l.accS, l.fixD, l.fixS, l.varD, l.varS};
        for (u32 a = 0; a < 10u; ++a) {
            ok = ok && work[a] % 256u == 0u && work[a] + n <= l.workBytes;
            for (u32 b = a + 1u; b < 10u; ++b) {
                ok = ok && (work[a] + n <= work[b] || work[b] + n <= work[a]);
            }
        }
        ok = ok && l.mipW[0] == 31u && l.mipH[0] == 19u && l.mipW[2] == 8u && l.mipH[2] == 5u;
        ok = ok && l.outS >= l.outD + n && l.outS + n <= l.outputBytes;
        if (keep) {
            for (u32 k = 0; k + 1u < kRdnMaxAtrous; ++k) {
                ok = ok && l.atrousD[k] != l.atrousD[k + 1u] && l.atrousS[k] != l.atrousS[k + 1u];
            }
        } else {
            ok = ok && l.atrousD[0] == l.atrousD[2] && l.atrousD[1] != l.atrousD[0];
        }
        std::printf("layout: RdnBufferLayout (keepIntermediates %d) aligned / disjoint: %s\n",
                    keep ? 1 : 0, ok ? "yes" : "NO");
        expect(ok, "RdnBufferLayout sections");
    }
}

// --- backends / determinism ------------------------------------------------------------------------------------------
bool sameBits(const RdnReference& a, const RdnReference& b) {
    const RdnBufferLayout& l = a.layout();
    return std::memcmp(a.outputD(), b.outputD(), static_cast<usize>(l.width) * l.height * 16u) == 0 &&
           std::memcmp(a.outputS(), b.outputS(), static_cast<usize>(l.width) * l.height * 16u) == 0 &&
           std::memcmp(a.stateArena(), b.stateArena(), static_cast<usize>(l.stateBytes)) == 0 &&
           std::memcmp(a.workArena(), b.workArena(), static_cast<usize>(l.workBytes)) == 0;
}

void testBackends() {
    auto& scheduler = jobs::JobScheduler::instance();
    RdnSyntheticDesc d{};
    d.width = 61;
    d.height = 37;
    d.lightStepFrame = 3;
    d.firefliesPerMille = 3;
    RdnSettings s = settingsFor(true);
    s.fireflyRatio = 8.f;
    s.fireflySigma = 6.f;
    s.clampSigmaD = 3.f;
    for (const u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        RdnReference a;
        RdnReference b;
        a.init(d.width, d.height, s);
        b.init(d.width, d.height, s);
        RdnSyntheticFrame f;
        bool same = true;
        for (u32 t = 0; t < 6u; ++t) {
            rdn_synthetic_frame(d, t, 11u, f);
            a.runFrame(f.inputs(true, true), f.camera, f.prevCamera, kernel::Backend::CpuReference);
            b.runFrame(f.inputs(true, true), f.camera, f.prevCamera, kernel::Backend::CpuParallel);
            same = same && sameBits(a, b);
        }
        std::printf("backends: CpuReference == CpuParallel bit for bit (6 frames, every feature, %u workers): %s\n",
                    workers, same ? "yes" : "NO");
        expect(same, "CpuReference == CpuParallel");
    }
    scheduler.shutdown();
}

void testDeterminism() {
    RdnSyntheticDesc d{};
    d.width = 64;
    d.height = 40;
    const RdnSettings s = settingsFor(true);
    RdnReference a;
    RdnReference b;
    a.init(d.width, d.height, s);
    b.init(d.width, d.height, s);
    RdnSyntheticFrame f;
    for (u32 t = 0; t < 8u; ++t) {
        rdn_synthetic_frame(d, t, 5u, f);
        a.runFrame(f.inputs(true, true), f.camera, f.prevCamera, kernel::Backend::CpuParallel);
    }
    for (u32 t = 0; t < 8u; ++t) {
        rdn_synthetic_frame(d, t, 5u, f);
        b.runFrame(f.inputs(true, true), f.camera, f.prevCamera, kernel::Backend::CpuParallel);
    }
    const bool same = sameBits(a, b);
    std::printf("determinism: two 8-frame runs bit-identical: %s\n", same ? "yes" : "NO");
    expect(same, "two runs bit-identical");
}

// --- quality ---------------------------------------------------------------------------------------------------------
void report(const char* name, const RunResult& r) {
    std::printf("quality %-28s diffuse: reduction %7.2fx bias %+7.4f rmse %.4f | "
                "specular: reduction %7.2fx bias %+7.4f rmse %.4f\n",
                name, r.d.reduction(), r.d.bias(), r.d.rmse(), r.s.reduction(), r.s.bias(), r.s.rmse());
}

void testQuality() {
    struct Case {
        const char* name;
        f32 camSpeed;
        f32 occSpeed;
        bool gradients;
        f32 floorRough;
    };
    const Case cases[] = {
        {"dynamic (A-SVGF)", 0.05f, 0.06f, true, 0.f},
        {"dynamic (SVGF temporal)", 0.05f, 0.06f, false, 0.f},
        {"static", 0.f, 0.f, false, 0.f},
        {"static glossy floor 0.4", 0.f, 0.f, true, 0.4f},
    };
    for (const Case& c : cases) {
        RdnSyntheticDesc d{};
        d.camSpeed = c.camSpeed;
        d.occluderSpeed = c.occSpeed;
        d.floorRoughness = c.floorRough;
        const RunResult r = run(d, settingsFor(c.gradients), kFrames, kFrames - kEval, 3u);
        report(c.name, r);
        expect(r.d.reduction() >= kMinReduction, "diffuse error reduction >= 4x");
        expect(r.s.reduction() >= kMinReduction, "specular error reduction >= 4x");
        expect(std::fabs(r.d.bias()) <= kMaxBias, "diffuse |bias| <= kMaxBias");
        expect(std::fabs(r.s.bias()) <= kMaxBias, "specular |bias| <= kMaxBias");
    }
}

// --- ghosting --------------------------------------------------------------------------------------------------------
void testGhosting() {
    RdnSyntheticDesc d{};
    d.camSpeed = 0.12f;
    d.occluder = false;
    d.floorRoughness = 0.f;
    const u32 floorOnly = 1u << kRdnFloor;
    RdnSettings on = settingsFor(false);
    on.clampSigmaS = 2.f; // history clipping on (off by default)
    RdnSettings off = on;
    off.virtualMotion = false;
    const RunResult a = run(d, on, kFrames, kFrames - kEval, 9u, floorOnly);
    const RunResult b = run(d, off, kFrames, kFrames - kEval, 9u, floorOnly);
    // Same with history clipping off: the reprojection alone.
    on.clampSigmaS = 0.f;
    off.clampSigmaS = 0.f;
    const RunResult c = run(d, on, kFrames, kFrames - kEval, 9u, floorOnly);
    const RunResult e = run(d, off, kFrames, kFrames - kEval, 9u, floorOnly);
    std::printf("ghosting: mirror-floor specular RMSE virtual motion %.4f vs surface motion %.4f (ratio %.3f); "
                "without clipping %.4f vs %.4f (ratio %.3f)\n",
                a.s.rmse(), b.s.rmse(), a.s.rmse() / b.s.rmse(), c.s.rmse(), e.s.rmse(), c.s.rmse() / e.s.rmse());
    expect(a.s.rmse() <= kMaxGhostRatio * b.s.rmse(), "virtual motion reduces reflection ghosting (with clipping)");
    expect(c.s.rmse() <= kMaxGhostRatio * e.s.rmse(), "virtual motion reduces reflection ghosting (no clipping)");
}

// --- disocclusion ----------------------------------------------------------------------------------------------------
void testDisocclusion() {
    RdnSyntheticDesc d{};
    d.camSpeed = 0.f;
    d.occluderSpeed = 0.15f;
    RdnSettings s = settingsFor(false);
    RdnReference ref;
    ref.init(d.width, d.height, s);
    RdnSyntheticFrame prev;
    RdnSyntheticFrame f;
    u32 trail = 0, trailReset = 0, stable = 0, stableKept = 0, frame0 = 0;
    for (u32 t = 0; t < 10u; ++t) {
        rdn_synthetic_frame(d, t, 21u, f);
        ref.runFrame(f.inputs(false, true), f.camera, f.prevCamera);
        const float4* accD = ref.work(ref.layout().accD);
        const float4* accS = ref.work(ref.layout().accS);
        const usize n = static_cast<usize>(d.width) * d.height;
        for (usize i = 0; i < n; ++i) {
            if (f.surface[i] == kRdnSky) {
                continue;
            }
            if (t == 0u) {
                frame0 += accD[i].w == 1.f && accS[i].w == 1.f ? 0u : 1u;
                continue;
            }
            // Revealed this frame: the occluder covered the pixel last frame and the
            // pixel is not it any more. The bilinear footprint reaches one pixel
            // further: only pixels whose 3 x 3 neighbourhood was all occluder.
            const u32 x = static_cast<u32>(i % d.width);
            const u32 y = static_cast<u32>(i / d.width);
            bool allOcc = f.surface[i] != kRdnOccluder;
            bool noneOcc = true;
            for (s32 dy = -1; dy <= 1; ++dy) {
                for (s32 dx = -1; dx <= 1; ++dx) {
                    const s32 qx = static_cast<s32>(x) + dx;
                    const s32 qy = static_cast<s32>(y) + dy;
                    if (qx < 0 || qy < 0 || qx >= static_cast<s32>(d.width) || qy >= static_cast<s32>(d.height)) {
                        allOcc = false;
                        continue;
                    }
                    const u8 ps = prev.surface[static_cast<usize>(qy) * d.width + qx];
                    allOcc = allOcc && ps == kRdnOccluder;
                    noneOcc = noneOcc && ps != kRdnOccluder &&
                              f.surface[static_cast<usize>(qy) * d.width + qx] != kRdnOccluder;
                }
            }
            if (allOcc) {
                ++trail;
                trailReset += accD[i].w == 1.f && accS[i].w == 1.f ? 1u : 0u;
            } else if (noneOcc && f.surface[i] != kRdnOccluder) {
                ++stable;
                stableKept += accD[i].w > 1.f ? 1u : 0u;
            }
        }
        prev = f;
    }
    // reset(): every pixel starts over.
    ref.reset();
    rdn_synthetic_frame(d, 10u, 21u, f);
    ref.runFrame(f.inputs(false, true), f.camera, f.prevCamera);
    u32 afterReset = 0;
    const float4* accD = ref.work(ref.layout().accD);
    for (usize i = 0; i < f.surface.size(); ++i) {
        afterReset += f.surface[i] != kRdnSky && accD[i].w != 1.f ? 1u : 0u;
    }
    std::printf("disocclusion: revealed pixels restarted %u / %u, stable pixels kept history %u / %u, frame 0 "
                "non-restarted %u, after reset() %u\n",
                trailReset, trail, stableKept, stable, frame0, afterReset);
    expect(trail > 50u && trailReset == trail, "every revealed pixel restarts its history");
    expect(stable > 1000u && stableKept == stable, "every stable pixel keeps its history");
    expect(frame0 == 0u && afterReset == 0u, "frame 0 and reset() start every pixel over");
}

// --- A-SVGF ----------------------------------------------------------------------------------------------------------
f64 relError(const RdnSyntheticFrame& f, const float4* dn, const std::vector<float4>& truth) {
    f64 e = 0.0, t = 0.0;
    for (usize i = 0; i < truth.size(); ++i) {
        if (f.surface[i] == kRdnSky) {
            continue;
        }
        e += std::fabs(lum(dn[i]) - lum(truth[i]));
        t += lum(truth[i]);
    }
    return e / t;
}

void testAsvgf() {
    RdnSyntheticDesc d{};
    d.camSpeed = 0.03f;
    d.occluderSpeed = 0.f;
    d.lightStepFrame = 12u;
    f64 before[2] = {0, 0}, after[2] = {0, 0};
    for (u32 g = 0; g < 2u; ++g) {
        const RdnSettings s = settingsFor(g == 1u);
        RdnReference ref;
        ref.init(d.width, d.height, s);
        RdnSyntheticFrame f;
        for (u32 t = 0; t < 16u; ++t) {
            rdn_synthetic_frame(d, t, 13u, f);
            ref.runFrame(f.inputs(s.gradients, true), f.camera, f.prevCamera, kernel::Backend::CpuParallel);
            const f64 e = 0.5 * (relError(f, ref.outputD(), f.truthD) + relError(f, ref.outputS(), f.truthS));
            if (t >= 8u && t < 12u) {
                before[g] += e / 4.0;
            } else if (t >= 12u) {
                after[g] += e / 4.0;
            }
        }
    }
    std::printf("asvgf: mean relative error before the light step SVGF %.4f A-SVGF %.4f; 4 frames after SVGF %.4f "
                "A-SVGF %.4f (ratio %.3f)\n",
                before[0], before[1], after[0], after[1], after[1] / after[0]);
    expect(after[1] <= 0.5 * after[0], "A-SVGF: error after the light step <= half of SVGF's");
    expect(before[1] <= 1.25 * before[0], "A-SVGF: no worse than 1.25 x SVGF before the step");
}

// --- firefly ---------------------------------------------------------------------------------------------------------
void testFirefly() {
    RdnSyntheticDesc d{};
    d.firefliesPerMille = 2u;
    RdnSettings on = settingsFor(true);
    on.fireflyRatio = 8.f;
    on.fireflySigma = 6.f;
    RdnSettings off = on;
    off.fireflyRatio = 0.f;
    off.fireflySigma = 0.f;
    RdnSyntheticDesc clean = d;
    clean.firefliesPerMille = 0u;
    const RunResult a = run(d, on, kFrames, kFrames - kEval, 17u);
    const RunResult b = run(d, off, kFrames, kFrames - kEval, 17u);
    const RunResult c = run(clean, on, kFrames, kFrames - kEval, 17u);
    std::printf("firefly: diffuse RMSE with suppression %.4f, without %.4f (ratio %.3f); clean input with suppression "
                "%.4f bias %+.4f\n",
                a.d.rmse(), b.d.rmse(), a.d.rmse() / b.d.rmse(), c.d.rmse(), c.d.bias());
    expect(a.d.rmse() * 2.0 <= b.d.rmse(), "firefly suppression cuts the diffuse error >= 2x");
    expect(std::fabs(c.d.bias()) <= kMaxBias, "suppression on clean input: |bias| <= kMaxBias");
}

// --- zero_alloc ------------------------------------------------------------------------------------------------------
void testZeroAlloc() {
    RdnSyntheticDesc d{};
    d.width = 64;
    d.height = 40;
    const RdnSettings s = settingsFor(true);
    RdnReference ref;
    ref.init(d.width, d.height, s);
    std::vector<RdnSyntheticFrame> frames(4);
    for (u32 t = 0; t < 4u; ++t) {
        rdn_synthetic_frame(d, t, 2u, frames[t]);
    }
    for (const kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
        ref.runFrame(frames[0].inputs(true, true), frames[0].camera, frames[0].prevCamera, b); // warm-up
        t_allocations = 0;
        t_count = true;
        for (u32 t = 1; t < 4u; ++t) {
            ref.runFrame(frames[t].inputs(true, true), frames[t].camera, frames[t].prevCamera, b);
        }
        t_count = false;
        std::printf("zero_alloc: %s steady-state frames: %llu heap allocations\n",
                    b == kernel::Backend::CpuReference ? "CpuReference" : "CpuParallel", t_allocations);
        expect(t_allocations == 0u, "0 heap allocations per steady-state frame");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    bool ran = false;
    struct Entry {
        const char* name;
        void (*fn)();
    };
    const Entry entries[] = {
        {"layout", testLayout},     {"backends", testBackends},         {"quality", testQuality},
        {"ghosting", testGhosting}, {"disocclusion", testDisocclusion}, {"asvgf", testAsvgf},
        {"firefly", testFirefly},   {"determinism", testDeterminism},   {"zero_alloc", testZeroAlloc}};
    for (const Entry& e : entries) {
        if (all || suite == e.name) {
            e.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
