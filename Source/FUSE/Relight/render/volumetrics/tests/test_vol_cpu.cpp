// FUSE Relight RL-5.6 CPU gates: volumetrics and the particle composite (VolumetricsCpu, the single-source core
// shaders/rl_vol_core.h). Suites (argv[1]; "all" runs every one):
//
//   fog_d3d      D3D fog -> medium: EXP with no lights composites to D3D range fog lerp(fogColour, colour, e^{-density d})
//                per pixel (<= 2e-3 relative, froxel interpolation); EXP2 / LINEAR equal D3D at their match distance.
//   convergence  RIS over the RL-4.4 light set (tree selection, 4 candidates, no reuse) at every froxel's jittered point
//                vs an independent per-light Monte Carlo reference at the same point: mean error within 4 sigma on
//                >= 98% of the froxels and the image-mean within 1%; RMSE of the running mean falls >= 2x from 32 to
//                512 frames; 4 candidates have lower variance than 1.
//   reuse        temporal reservoir reuse (static camera): per-frame error variance below no-reuse; bias of the mean
//                within 5% (the documented bias bound of the Remix-style reuse).
//   ghosting     temporal filter metrics: a light switched off lags as (1 - alpha)^n (within 1e-3) and a history reset
//                removes it in one frame; a sideways camera move with reprojection leaves a smaller relative L1 error
//                against the converged field than without (and below 5%).
//   shadows      shadow rays on the path tracer's CPU reference BVH: froxels fully behind an occluder receive exactly
//                no light-set in-scattering, lit ones do.
//   particles    the composite of RL-3.6 billboard quads vs an independent painter's rasteriser (quads sorted back to
//                front, point-in-quad coverage, D3D blending) for alpha / additive / premultiplied / multiply systems
//                (<= 1e-5); submission order does not matter; opaque depth clips; the 16 nearest layers; particles in
//                EXP fog match the closed form (<= 3e-3).
//   determinism  two runs bit-identical.
//   zero_alloc   steady-state frames (lights, reuse, fog, particles): no operator-new call (CPU shadow rays through the
//                WP-6.0 RtReferenceScene::trace allocate per call: reported only).
//   layout       rl_vol.{comp,slang} push blocks == VolumetricsGpu::Push; rl_vol_types.h constants == the C++ enums.
#include "vol_test_common.hpp"

#include "vol_cpp.hpp"

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

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

using namespace vol_test;
using fuse::u32;
namespace opt = fuse::relight::options;
namespace volk = fuse::relight::volk;

struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

bool buildLights(rl::RelightLightSet& set) {
    set.reserve(8, 4);
    set.beginFrame();
    const char* only = std::getenv("VOL_DEBUG_LIGHT"); // debugging aid: one of the test lights (0, 1, 2)
    if (only == nullptr) {
        addTestLights(set);
    } else {
        rl::RelightLightSet all;
        all.reserve(8, 4);
        all.beginFrame();
        addTestLights(all);
        all.build();
        set.addLight(all.light(u32(std::atoi(only))), 1u);
    }
    return set.build();
}

/// The core's params for a desc (the jittered points the kernels use).
volk::VolParams unpacked(const vol::VolFrameDesc& d, u32 lightCount) {
    vol::Word w[vol::kVolParamWordCount];
    vol::VolUint4 iw[vol::kVolIntWordCount];
    vol::packVolParams(d, lightCount, w, iw);
    volk::uint4 u[vol::kVolIntWordCount];
    for (u32 k = 0; k < vol::kVolIntWordCount; ++k) {
        u[k] = volk::uint4{iw[k].x, iw[k].y, iw[k].z, iw[k].w};
    }
    return volk::volParamsUnpack(w, u);
}

/// The froxel's jittered point and view direction of frame P.frame (rl_vol_core.h volInject).
void froxelPoint(const volk::VolParams& P, u32 x, u32 y, u32 z, lk::float3& p, lk::float3& v) {
    const u32 i = volk::volFroxelIndex(P, x, y, z);
    const float jx = volk::volRandom(i, P.frame, 0u);
    const float jy = volk::volRandom(i, P.frame, 1u);
    const float jz = volk::volRandom(i, P.frame, 2u);
    const lk::float3 ray = volk::volRay(P, (float(x) + jx) / float(P.gx), (float(y) + jy) / float(P.gy));
    p = P.camPos + ray * volk::volDepthAt(P, float(z) + jz);
    v = lk::normalize(ray);
}

/// Light-set in-scattering of the froxel (current = albedo x L x sigma_t, ambient 0).
lk::float3 lightPart(const vol::Word& cur, const vol::VolFrameDesc& d) {
    const float s = cur.w > 0.f ? 1.f / cur.w : 0.f;
    return lk::float3(cur.x * s / d.medium.albedo[0], cur.y * s / d.medium.albedo[1], cur.z * s / d.medium.albedo[2]);
}

double lum(const lk::float3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

// ---- fog_d3d ----------------------------------------------------------------------------------------------------------

void suiteFogD3d() {
    const u32 w = 24, h = 16;
    vol::VolFrameDesc d = baseDesc(w, h, 12, 8, 64);
    vol::D3dFogState fog;
    fog.mode = vol::D3dFogMode::Exp;
    fog.density = 0.2f;
    fog.color[0] = 0.5f;
    fog.color[1] = 0.6f;
    fog.color[2] = 0.7f;
    d.medium = vol::mediumFromD3dFog(fog);
    d.flags = vol::kVolFog; // no lights
    std::vector<vol::Word> color(w * h);
    std::vector<float> depth(w * h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            color[y * w + x] = vol::Word(float(x) / w, float(y) / h, 0.25f, 1.f);
            depth[y * w + x] = (x + y) % 5u == 4u ? 0.f : 0.5f + 11.f * float((x * 7u + y * 3u) % 23u) / 23.f;
        }
    }
    vol::VolumetricsCpu cpu;
    cpu.resize(d.gridX, d.gridY, d.gridZ, w, h);
    vol::VolCpuInputs in;
    in.color = color.data();
    in.depth = depth.data();
    check(cpu.run(d, in), "fog_d3d: run");
    const volk::VolParams P = unpacked(d, 0u);
    double worst = 0.0;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            const lk::float3 ray = volk::volRay(P, (x + 0.5f) / w, (y + 0.5f) / h);
            const float z = depth[i] > 0.f ? depth[i] : d.farZ;
            const double dist = double(z) * std::sqrt(double(lk::dot(ray, ray)));
            const double f = std::exp(-double(fog.density) * dist);
            const double e[3] = {fog.color[0] + (color[i].x - fog.color[0]) * f, fog.color[1] + (color[i].y - fog.color[1]) * f,
                                 fog.color[2] + (color[i].z - fog.color[2]) * f};
            const vol::Word& o = cpu.buffer(vol::kVolColorOut)[i];
            const double g[3] = {o.x, o.y, o.z};
            for (int k = 0; k < 3; ++k) {
                worst = std::max(worst, std::fabs(g[k] - e[k]) / std::max(e[k], 0.05));
            }
        }
    }
    std::printf("fog_d3d: EXP density %.2f: worst relative error vs D3D range fog %.2e over %u pixels\n", fog.density,
                worst, w * h);
    check(worst <= 2e-3, "fog_d3d: EXP composite == D3D range fog (<= 2e-3)");
    // EXP2 / LINEAR match distances.
    vol::D3dFogState e2 = fog;
    e2.mode = vol::D3dFogMode::Exp2;
    const vol::VolMedium m2 = vol::mediumFromD3dFog(e2);
    const double dd = 1.0 / fog.density;
    const double d3dExp2 = std::exp(-(fog.density * dd) * (fog.density * dd));
    check(std::fabs(std::exp(-m2.density * dd) - d3dExp2) < 1e-6, "fog_d3d: EXP2 equal at 1 / density");
    vol::D3dFogState lin = fog;
    lin.mode = vol::D3dFogMode::Linear;
    lin.start = 2.f;
    lin.end = 10.f;
    const vol::VolMedium ml = vol::mediumFromD3dFog(lin);
    check(std::fabs(std::exp(-ml.density * 6.0) - 0.5) < 1e-6, "fog_d3d: LINEAR equal at the midpoint");
    vol::D3dFogState none = fog;
    none.mode = vol::D3dFogMode::None;
    check(vol::mediumFromD3dFog(none).density == 0.f, "fog_d3d: NONE -> no medium");
    check(std::fabs(vol::mediumFromD3dFog(fog, 2.f).density - 0.1f) < 1e-7f, "fog_d3d: sceneScale divides");
}

// ---- convergence ------------------------------------------------------------------------------------------------------

struct ConvergenceResult {
    double passFraction = 0.0;
    double meanRel = 0.0;
    double rmse32 = 0.0;
    double rmseAll = 0.0;
    double variance = 0.0; ///< mean per-frame luminance error variance
};

ConvergenceResult convergenceRun(const rl::RelightLightSet& set, u32 candidates, u32 frames, bool reuse, u32 refSamples) {
    const u32 gx = 4, gy = 3, gz = 8;
    vol::VolFrameDesc d = baseDesc(16, 12, gx, gy, gz);
    d.candidates = candidates;
    d.flags = vol::kVolLights | (reuse ? vol::kVolReuse : 0u);
    d.mCap = 8.f;
    vol::VolumetricsCpu cpu;
    cpu.resize(gx, gy, gz, 16, 12);
    std::vector<vol::Word> color(16 * 12, vol::Word(0.f, 0.f, 0.f, 1.f));
    vol::VolCpuInputs in;
    in.lights = &set;
    in.color = color.data();
    const u32 n = gx * gy * gz;
    std::vector<double> sumE(n, 0.0), sumE2(n, 0.0), sumEst(n, 0.0), sumRef(n, 0.0), sumE32(n, 0.0);
    for (u32 f = 0; f < frames; ++f) {
        d.frame = f;
        cpu.run(d, in);
        const volk::VolParams P = unpacked(d, set.lightCount());
        for (u32 y = 0; y < gy; ++y) {
            for (u32 x = 0; x < gx; ++x) {
                for (u32 z = 0; z < gz; ++z) {
                    const u32 i = volk::volFroxelIndex(P, x, y, z);
                    lk::float3 p, v;
                    froxelPoint(P, x, y, z, p, v);
                    const lk::float3 ref = vol::referenceInScatter(set, p, v, d.medium.anisotropy, refSamples, nullptr);
                    const double est = lum(lightPart(cpu.buffer(vol::kVolCurrent)[i], d));
                    const double e = est - lum(ref);
                    sumE[i] += e;
                    sumE2[i] += e * e;
                    sumEst[i] += est;
                    sumRef[i] += lum(ref);
                    if (f < 32u) {
                        sumE32[i] += e;
                    }
                }
            }
        }
    }
    ConvergenceResult r;
    u32 pass = 0;
    double est = 0.0, ref = 0.0, var = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const double mean = sumE[i] / frames;
        const double v = std::max(sumE2[i] / frames - mean * mean, 1e-30);
        const double se = std::sqrt(v / frames);
        pass += std::fabs(mean) <= 4.0 * se + 1e-6 * (sumRef[i] / frames) ? 1u : 0u;
        est += sumEst[i];
        ref += sumRef[i];
        var += v;
        r.rmse32 += (sumE32[i] / 32.0) * (sumE32[i] / 32.0);
        r.rmseAll += mean * mean;
    }
    r.passFraction = double(pass) / n;
    r.meanRel = std::fabs(est - ref) / std::max(ref, 1e-30);
    r.rmse32 = std::sqrt(r.rmse32 / n);
    r.rmseAll = std::sqrt(r.rmseAll / n);
    r.variance = var / n;
    return r;
}

void suiteConvergence() {
    rl::RelightLightSet set;
    check(buildLights(set), "convergence: light set build");
    const ConvergenceResult r4 = convergenceRun(set, 4, 512, false, 64);
    const ConvergenceResult r1 = convergenceRun(set, 1, 128, false, 16);
    std::printf("convergence: 4 candidates, 512 frames: %.1f%% froxels within 4 sigma, image mean rel error %.3e, "
                "RMSE of the running mean 32 frames %.4f -> 512 frames %.4f; per-frame variance 4 cand %.4e vs "
                "1 cand %.4e\n",
                r4.passFraction * 100.0, r4.meanRel, r4.rmse32, r4.rmseAll, r4.variance, r1.variance);
    check(r4.passFraction >= 0.98, "convergence: >= 98% of froxels within 4 sigma of the reference");
    check(r4.meanRel <= 0.01, "convergence: image mean within 1%");
    check(r4.rmseAll * 2.0 <= r4.rmse32, "convergence: RMSE falls >= 2x from 32 to 512 frames");
    check(r4.variance < r1.variance, "convergence: 4 RIS candidates < 1 candidate variance");
}

// ---- reuse ------------------------------------------------------------------------------------------------------------

void suiteReuse() {
    rl::RelightLightSet set;
    check(buildLights(set), "reuse: light set build");
    const ConvergenceResult off = convergenceRun(set, 2, 256, false, 32);
    const ConvergenceResult on = convergenceRun(set, 2, 256, true, 32);
    std::printf("reuse: per-frame error variance %.4e (reuse) vs %.4e (none); image-mean bias %.3e (reuse) / %.3e\n",
                on.variance, off.variance, on.meanRel, off.meanRel);
    check(on.variance < off.variance, "reuse: temporal reservoir reuse lowers the per-frame variance");
    check(on.meanRel <= 0.05, "reuse: bias bound (image mean within 5%)");
}

// ---- ghosting ---------------------------------------------------------------------------------------------------------

double fieldLum(const std::vector<vol::Word>& b) {
    double s = 0.0;
    for (const vol::Word& w : b) {
        s += 0.2126 * w.x + 0.7152 * w.y + 0.0722 * w.z;
    }
    return s;
}

void suiteGhosting() {
    rl::RelightLightSet lit, dark;
    check(buildLights(lit), "ghosting: light set");
    dark.reserve(4, 4);
    dark.beginFrame();
    dark.build();
    const u32 w = 16, h = 12;
    vol::VolFrameDesc d = baseDesc(w, h, 8, 6, 16);
    d.flags = vol::kVolLights | vol::kVolFog;
    d.temporalAlpha = 0.2f;
    std::vector<vol::Word> color(w * h, vol::Word(0.f, 0.f, 0.f, 1.f));
    vol::VolumetricsCpu cpu;
    cpu.resize(d.gridX, d.gridY, d.gridZ, w, h);
    vol::VolCpuInputs in;
    in.color = color.data();
    in.lights = &lit;
    for (u32 f = 0; f < 64; ++f) {
        d.frame = f;
        cpu.run(d, in);
    }
    const double lit0 = fieldLum(cpu.buffer(vol::kVolHistPrev));
    in.lights = &dark;
    double worst = 0.0;
    for (u32 n = 1; n <= 8; ++n) {
        d.frame = 64 + n;
        cpu.run(d, in);
        const double expect = lit0 * std::pow(1.0 - d.temporalAlpha, double(n));
        worst = std::max(worst, std::fabs(fieldLum(cpu.buffer(vol::kVolHistPrev)) - expect) / lit0);
    }
    std::printf("ghosting: light off: history energy follows (1 - alpha)^n within %.2e of the lit field", worst);
    check(worst <= 1e-3, "ghosting: lag == (1 - alpha)^n");
    cpu.resetHistory();
    d.frame = 80;
    cpu.run(d, in);
    const double after = fieldLum(cpu.buffer(vol::kVolHistPrev));
    std::printf("; after a history reset %.2e\n", after / lit0);
    check(after == 0.0, "ghosting: history reset removes the lag in one frame");

    // Camera move: converged field at the new position vs the history right after the move.
    auto converge = [&](const pt::PtCamera& cam, u32 frames, vol::VolumetricsCpu& c) {
        d.camera = cam;
        d.prevCamera = cam;
        c.resetHistory();
        for (u32 f = 0; f < frames; ++f) {
            d.frame = 1000 + f;
            c.run(d, in);
        }
    };
    // A deterministic, spatially varying field (height fog lit by ambient light only) isolates the ghosting error
    // from the estimator's noise; the camera moves up across the height gradient.
    in.lights = nullptr;
    d.medium.falloff = 0.6f;
    d.medium.baseHeight = -1.5f;
    d.medium.ambient[0] = d.medium.ambient[1] = d.medium.ambient[2] = 1.f;
    const pt::PtCamera a = camera(0.f, 0.f, 4.f, 0.f, 0.f, -1.f, 50.f);
    const pt::PtCamera b = camera(0.f, 0.6f, 4.f, 0.f, 0.f, -1.f, 50.f);
    vol::VolumetricsCpu truth;
    truth.resize(d.gridX, d.gridY, d.gridZ, w, h);
    converge(b, 96, truth);
    double err[2] = {0.0, 0.0};
    for (int mode = 0; mode < 2; ++mode) {
        vol::VolumetricsCpu c;
        c.resize(d.gridX, d.gridY, d.gridZ, w, h);
        d.flags = vol::kVolLights | vol::kVolFog | (mode == 1 ? vol::kVolReproject : 0u);
        converge(a, 96, c);
        d.camera = b;
        d.prevCamera = a;
        d.frame = 2000;
        c.run(d, in);
        const std::vector<vol::Word>& g = c.buffer(vol::kVolHistPrev);
        const std::vector<vol::Word>& t = truth.buffer(vol::kVolHistPrev);
        double num = 0.0, den = 0.0;
        for (std::size_t i = 0; i < g.size(); ++i) {
            num += std::fabs(g[i].x - t[i].x) + std::fabs(g[i].y - t[i].y) + std::fabs(g[i].z - t[i].z);
            den += t[i].x + t[i].y + t[i].z;
        }
        err[mode] = num / std::max(den, 1e-30);
    }
    std::printf("ghosting: camera moved 0.6 up: relative L1 error of the history %.4f without / %.4f with "
                "reprojection\n",
                err[0], err[1]);
    check(err[1] < err[0], "ghosting: reprojection reduces the ghosting error");
    check(err[1] < 0.05, "ghosting: reprojected history error < 5%");
}

// ---- shadows ----------------------------------------------------------------------------------------------------------

void suiteShadows() {
    pt::PtCompiledScene c;
    std::string error;
    check(c.compile(shadowScene(), {}, &error), "shadows: compile " + error);
    vol::VolFrameDesc d = baseDesc(16, 12, 8, 6, 16);
    d.camera = shadowScene().camera;
    d.prevCamera = d.camera;
    d.flags = vol::kVolLights | vol::kVolShadows;
    d.candidates = 2;
    vol::VolumetricsCpu cpu;
    cpu.resize(d.gridX, d.gridY, d.gridZ, 16, 12);
    std::vector<vol::Word> color(16 * 12, vol::Word(0.f, 0.f, 0.f, 1.f));
    vol::VolCpuInputs in;
    in.lights = &c.lightSet();
    in.scene = &c.reference();
    in.color = color.data();
    const lk::float3 light(0.f, 3.f, -2.f);
    u32 dark = 0, darkBad = 0, lit = 0, litNonZero = 0;
    for (u32 f = 0; f < 16; ++f) {
        d.frame = f;
        cpu.run(d, in);
        const volk::VolParams P = unpacked(d, c.lightSet().lightCount());
        for (u32 y = 0; y < d.gridY; ++y) {
            for (u32 x = 0; x < d.gridX; ++x) {
                for (u32 z = 0; z < d.gridZ; ++z) {
                    lk::float3 p, v;
                    froxelPoint(P, x, y, z, p, v);
                    const vol::Word& cur = cpu.buffer(vol::kVolCurrent)[volk::volFroxelIndex(P, x, y, z)];
                    const double l = lum(lightPart(cur, d));
                    if (!(p.y < 0.8f)) {
                        continue;
                    }
                    // Where the segment to the light centre crosses y = 1 (the occluder covers x < 0).
                    const float t = (1.f - p.y) / (light.y - p.y);
                    const float cx = p.x + (light.x - p.x) * t;
                    const float cz = p.z + (light.z - p.z) * t;
                    if (cx < -0.4f && std::fabs(cz) < 9.f) {
                        ++dark;
                        darkBad += l != 0.0 ? 1u : 0u;
                    } else if (cx > 0.4f) {
                        ++lit;
                        litNonZero += l > 0.0 ? 1u : 0u;
                    }
                }
            }
        }
    }
    std::printf("shadows: %u shadowed froxel samples (%u lit by mistake), %u lit samples (%u with light)\n", dark,
                darkBad, lit, litNonZero);
    check(dark > 100u && darkBad == 0u, "shadows: occluded froxels receive no light-set in-scattering");
    check(lit > 100u && litNonZero >= lit * 9u / 10u, "shadows: unoccluded froxels are lit");
}

// ---- particles --------------------------------------------------------------------------------------------------------

struct PainterQuad {
    float cx, cy, cz, s;
    float rgba[4];
    u32 blend;
};

/// Independent painter's reference: quads (camera at the origin looking down -z, axis-aligned quads) sorted back to
/// front (stable in submission order), coverage by the pixel centre ray, D3D blending.
std::vector<vol::Word> painter(const vol::VolFrameDesc& d, const std::vector<PainterQuad>& quads,
                               const std::vector<vol::Word>& color, const std::vector<float>& depth, u32 maxLayers) {
    const volk::VolParams P = unpacked(d, 0u);
    std::vector<vol::Word> out(color);
    for (u32 y = 0; y < d.height; ++y) {
        for (u32 x = 0; x < d.width; ++x) {
            const u32 i = y * d.width + x;
            const lk::float3 ray = volk::volRay(P, (x + 0.5f) / d.width, (y + 0.5f) / d.height);
            // ray per unit depth: the point at view depth z is z * ray (forward = -z world).
            struct Hit {
                float depth;
                u32 order;
            };
            std::vector<Hit> hits;
            for (u32 q = 0; q < quads.size(); ++q) {
                const PainterQuad& Q = quads[q];
                const float z = -Q.cz; // view depth
                if (depth[i] > 0.f && !(z < depth[i])) {
                    continue;
                }
                const float px = ray.x * z, py = ray.y * z;
                if (std::fabs(px - Q.cx) <= Q.s && std::fabs(py - Q.cy) <= Q.s) {
                    hits.push_back({z, q});
                }
            }
            std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.depth < b.depth; });
            if (hits.size() > maxLayers) {
                hits.resize(maxLayers);
            }
            double c[3] = {color[i].x, color[i].y, color[i].z};
            for (std::size_t n = hits.size(); n-- > 0;) {
                const PainterQuad& Q = quads[hits[n].order];
                for (int k = 0; k < 3; ++k) {
                    const double s = Q.rgba[k], a = Q.rgba[3];
                    switch (Q.blend) {
                    case vol::kVolAdditive: c[k] = c[k] + s * a; break;
                    case vol::kVolPremultiplied: c[k] = s + c[k] * (1.0 - a); break;
                    case vol::kVolMultiply: c[k] = c[k] * s; break;
                    default: c[k] = s * a + c[k] * (1.0 - a); break;
                    }
                }
            }
            out[i] = vol::Word(float(c[0]), float(c[1]), float(c[2]), color[i].w);
        }
    }
    return out;
}

/// Quantised like the vertex colour (8 bits).
float q8(float v) { return float(std::lround(std::min(std::max(v, 0.f), 1.f) * 255.f)) / 255.f; }

double compositeError(const std::vector<vol::Word>& a, const std::vector<vol::Word>& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max({worst, double(std::fabs(a[i].x - b[i].x)), double(std::fabs(a[i].y - b[i].y)),
                          double(std::fabs(a[i].z - b[i].z))});
    }
    return worst;
}

void suiteParticles() {
    const u32 w = 40, h = 30;
    vol::VolFrameDesc d = baseDesc(w, h, 8, 6, 32);
    d.camera = camera(0.f, 0.f, 0.f, 0.f, 0.f, -1.f, 60.f);
    d.prevCamera = d.camera;
    d.flags = vol::kVolParticles;
    std::vector<vol::Word> color(w * h);
    std::vector<float> depth(w * h, 0.f);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            color[y * w + x] = vol::Word(0.2f + 0.5f * x / w, 0.3f, 0.6f - 0.4f * y / h, 1.f);
        }
    }
    // Four systems (one per blend mode), overlapping quads at several depths, submitted in scrambled order.
    std::vector<PainterQuad> quads;
    u32 seed = 12345u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / 16777216.f;
    };
    for (u32 sys = 0; sys < 4; ++sys) {
        for (u32 k = 0; k < 6; ++k) {
            PainterQuad q;
            q.cz = -(2.f + 6.f * rnd());
            q.cx = (rnd() - 0.5f) * 1.2f * -q.cz * 0.5f;
            q.cy = (rnd() - 0.5f) * 0.9f * -q.cz * 0.5f;
            q.s = 0.15f * -q.cz + 0.05f;
            q.rgba[0] = q8(rnd());
            q.rgba[1] = q8(rnd());
            q.rgba[2] = q8(rnd());
            q.rgba[3] = q8(0.2f + 0.7f * rnd());
            q.blend = sys;
            quads.push_back(q);
        }
    }
    auto build = [&](const std::vector<u32>& order, std::vector<pa::GpuParticleVertex>& verts, vol::VolFrameDesc& dd) {
        verts.clear();
        dd.systemCount = 4;
        for (u32 sys = 0; sys < 4; ++sys) {
            dd.systems[sys].firstQuad = static_cast<u32>(verts.size() / 4u);
            u32 n = 0;
            for (u32 idx : order) {
                const PainterQuad& q = quads[idx];
                if (q.blend != sys) {
                    continue;
                }
                addQuad(verts, q.cx, q.cy, q.cz, q.s, packColor(q.rgba[0], q.rgba[1], q.rgba[2], q.rgba[3]));
                ++n;
            }
            dd.systems[sys].quadCount = n;
            dd.systems[sys].blend = sys;
            dd.systems[sys].flags = 0;
        }
    };
    std::vector<u32> order(quads.size());
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    // Reference order: systems in order, quads by index; the painter sorts by depth (distinct depths).
    std::vector<PainterQuad> sysOrdered;
    for (u32 sys = 0; sys < 4; ++sys) {
        for (const PainterQuad& q : quads) {
            if (q.blend == sys) {
                sysOrdered.push_back(q);
            }
        }
    }
    std::vector<pa::GpuParticleVertex> verts;
    build(order, verts, d);
    vol::VolumetricsCpu cpu;
    cpu.resize(d.gridX, d.gridY, d.gridZ, w, h);
    vol::VolCpuInputs in;
    in.color = color.data();
    in.depth = depth.data();
    in.vertices = verts;
    cpu.run(d, in);
    const std::vector<vol::Word> ref = painter(d, sysOrdered, color, depth, vol::kVolLayerCount);
    const double e0 = compositeError(cpu.buffer(vol::kVolColorOut), ref);
    u32 changed = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        changed += std::fabs(ref[i].x - color[i].x) > 1e-3f ? 1u : 0u;
    }
    // Scrambled submission.
    for (u32 i = static_cast<u32>(order.size()); i > 1; --i) {
        std::swap(order[i - 1], order[u32(rnd() * i) % i]);
    }
    std::vector<pa::GpuParticleVertex> verts2;
    vol::VolFrameDesc d2 = d;
    build(order, verts2, d2);
    in.vertices = verts2;
    vol::VolumetricsCpu cpu2;
    cpu2.resize(d.gridX, d.gridY, d.gridZ, w, h);
    cpu2.run(d2, in);
    const double e1 = compositeError(cpu2.buffer(vol::kVolColorOut), ref);
    // The unsorted painter (submission order, no depth sort) differs: sorting matters for the alpha systems.
    std::printf("particles: %zu quads in 4 systems (alpha / additive / premultiplied / multiply), %u pixels covered: "
                "max error vs sorted painter %.2e (scrambled submission %.2e)\n",
                quads.size(), changed, e0, e1);
    check(changed > w * h / 4u, "particles: the quads cover the image");
    check(e0 <= 1e-5, "particles: composite == back-to-front painter with D3D blending");
    check(e1 <= 1e-5, "particles: submission order does not matter");

    // Opaque depth clips: a wall at view depth 3 over the left half.
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w / 2u; ++x) {
            depth[y * w + x] = 3.f;
        }
    }
    in.vertices = verts;
    cpu.run(d, in);
    const std::vector<vol::Word> refClip = painter(d, sysOrdered, color, depth, vol::kVolLayerCount);
    const double e2 = compositeError(cpu.buffer(vol::kVolColorOut), refClip);
    std::printf("particles: opaque depth clip max error %.2e\n", e2);
    check(e2 <= 1e-5, "particles: particles behind the opaque surface are hidden");
    std::fill(depth.begin(), depth.end(), 0.f);

    // Layer cap: 24 stacked quads at one pixel region, the 16 nearest kept.
    std::vector<PainterQuad> stack;
    for (u32 k = 0; k < 24; ++k) {
        PainterQuad q{0.f, 0.f, -(2.f + 0.25f * float((k * 7u) % 24u)), 2.f, {q8(0.1f * (k % 10)), q8(0.5f), q8(1.f - 0.04f * k), q8(0.3f)}, vol::kVolAlpha};
        stack.push_back(q);
    }
    std::vector<pa::GpuParticleVertex> sv;
    for (const PainterQuad& q : stack) {
        addQuad(sv, q.cx, q.cy, q.cz, q.s, packColor(q.rgba[0], q.rgba[1], q.rgba[2], q.rgba[3]));
    }
    vol::VolFrameDesc ds = d;
    ds.systemCount = 1;
    ds.systems[0] = vol::VolParticleSystem{0u, 24u, vol::kVolAlpha, 0u};
    in.vertices = sv;
    cpu.run(ds, in);
    const std::vector<vol::Word> refCap = painter(ds, stack, color, depth, vol::kVolLayerCount);
    const double e3 = compositeError(cpu.buffer(vol::kVolColorOut), refCap);
    std::printf("particles: 24 stacked layers, 16 nearest composited: max error %.2e\n", e3);
    check(e3 <= 1e-5, "particles: the 16 nearest layers");

    // Particles in EXP fog: one alpha quad at depth 4 over a background at depth 10.
    vol::D3dFogState fog;
    fog.mode = vol::D3dFogMode::Exp;
    fog.density = 0.12f;
    fog.color[0] = 0.6f;
    fog.color[1] = 0.65f;
    fog.color[2] = 0.7f;
    vol::VolFrameDesc df = d;
    df.gridZ = 96;
    df.medium = vol::mediumFromD3dFog(fog);
    df.flags = vol::kVolParticles | vol::kVolFog;
    df.systemCount = 1;
    df.systems[0] = vol::VolParticleSystem{0u, 1u, vol::kVolAlpha, 0u};
    std::vector<pa::GpuParticleVertex> one;
    const float pc[4] = {q8(0.9f), q8(0.2f), q8(0.1f), q8(0.6f)};
    addQuad(one, 0.f, 0.f, -4.f, 1.5f, packColor(pc[0], pc[1], pc[2], pc[3]));
    std::fill(depth.begin(), depth.end(), 10.f);
    in.vertices = one;
    vol::VolumetricsCpu cf;
    cf.resize(df.gridX, df.gridY, df.gridZ, w, h);
    cf.run(df, in);
    const volk::VolParams P = unpacked(df, 0u);
    double worst = 0.0;
    u32 covered = 0;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            const lk::float3 ray = volk::volRay(P, (x + 0.5f) / w, (y + 0.5f) / h);
            const double len = std::sqrt(double(lk::dot(ray, ray)));
            const bool hit = std::fabs(ray.x * 4.f) <= 1.5f && std::fabs(ray.y * 4.f) <= 1.5f;
            const double t1 = std::exp(-fog.density * 4.0 * len), t2 = std::exp(-fog.density * 6.0 * len);
            const double tb = std::exp(-fog.density * 10.0 * len);
            const vol::Word& o = cf.buffer(vol::kVolColorOut)[i];
            const double got[3] = {o.x, o.y, o.z};
            const double bg[3] = {color[i].x, color[i].y, color[i].z};
            for (int k = 0; k < 3; ++k) {
                double e;
                if (hit) {
                    const double behind = bg[k] * t2 + fog.color[k] * (1.0 - t2);
                    const double blended = pc[k] * pc[3] + behind * (1.0 - pc[3]);
                    e = blended * t1 + fog.color[k] * (1.0 - t1);
                } else {
                    e = bg[k] * tb + fog.color[k] * (1.0 - tb);
                }
                worst = std::max(worst, std::fabs(got[k] - e));
            }
            covered += hit ? 1u : 0u;
        }
    }
    std::printf("particles: alpha quad in EXP fog (%u pixels covered): max error vs closed form %.2e\n", covered, worst);
    check(covered > 50u && worst <= 3e-3, "particles: fog between the layers == closed form");
}

// ---- determinism / zero_alloc -----------------------------------------------------------------------------------------

struct FullFrame {
    pt::PtCompiledScene scene;
    vol::VolFrameDesc desc;
    std::vector<vol::Word> color;
    std::vector<float> depth;
    std::vector<pa::GpuParticleVertex> verts;
    vol::VolumetricsCpu cpu;
    vol::VolCpuInputs in;
};

bool setupFull(FullFrame& f) {
    std::string error;
    if (!f.scene.compile(shadowScene(), {}, &error)) {
        return false;
    }
    const u32 w = 24, h = 16;
    f.desc = baseDesc(w, h, 8, 6, 16);
    f.desc.camera = shadowScene().camera;
    f.desc.prevCamera = f.desc.camera;
    f.desc.flags = vol::kVolLights | vol::kVolShadows | vol::kVolReuse | vol::kVolFog | vol::kVolReproject |
                   vol::kVolParticles;
    f.color.assign(w * h, vol::Word(0.1f, 0.2f, 0.3f, 1.f));
    f.depth.assign(w * h, 6.f);
    addQuad(f.verts, 0.2f, 0.1f, -1.f, 0.8f, packColor(0.9f, 0.5f, 0.2f, 0.5f));
    addQuad(f.verts, -0.3f, 0.f, 0.f, 1.f, packColor(0.2f, 0.5f, 0.9f, 0.4f));
    f.desc.systemCount = 1;
    f.desc.systems[0] = vol::VolParticleSystem{0u, 2u, vol::kVolAdditive, vol::kVolSoftDisc};
    f.cpu.resize(8, 6, 16, w, h);
    f.in.lights = &f.scene.lightSet();
    f.in.scene = &f.scene.reference();
    f.in.color = f.color.data();
    f.in.depth = f.depth.data();
    f.in.vertices = f.verts;
    return true;
}

void suiteDeterminism() {
    FullFrame a, b;
    check(setupFull(a) && setupFull(b), "determinism: setup");
    for (u32 f = 0; f < 6; ++f) {
        a.desc.frame = b.desc.frame = f;
        a.cpu.run(a.desc, a.in);
        b.cpu.run(b.desc, b.in);
    }
    bool same = true;
    for (u32 buf = 0; buf < vol::kVolBufferCount; ++buf) {
        const auto& x = a.cpu.buffer(buf);
        const auto& y = b.cpu.buffer(buf);
        same = same && x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(vol::Word)) == 0;
    }
    std::printf("determinism: two runs of 6 frames %s\n", same ? "bit-identical" : "DIFFER");
    check(same, "determinism: bit-identical");
}

void suiteZeroAlloc() {
    // The shadow rays of the CPU reference go through the WP-6.0 RtReferenceScene::trace, which allocates per call (its
    // traversal stack; not RL-5.6 code): the gate runs the reference without the scene (shadows skipped) and reports
    // the count with it. The GPU runner's shadow rays are ray queries (rl_vol_vk zero_alloc covers them).
    unsigned long long counts[2] = {0, 0};
    for (int withScene = 0; withScene < 2; ++withScene) {
        FullFrame f;
        check(setupFull(f), "zero_alloc: setup");
        if (withScene == 0) {
            f.in.scene = nullptr;
        }
        for (u32 k = 0; k < 4; ++k) {
            f.desc.frame = k;
            f.cpu.run(f.desc, f.in);
        }
        g_allocations = 0;
        g_count = true;
        for (u32 k = 4; k < 36; ++k) {
            f.desc.frame = k;
            f.desc.camera.origin[0] = 0.01f * float(k);
            f.cpu.run(f.desc, f.in);
            f.desc.prevCamera = f.desc.camera;
        }
        g_count = false;
        counts[withScene] = g_allocations.load();
    }
    std::printf("zero_alloc: 32 steady-state frames (lights, reuse, fog, particles): %llu operator-new calls; with CPU "
                "shadow rays through RtReferenceScene::trace (WP-6.0): %llu (reported, not RL-5.6's)\n",
                counts[0], counts[1]);
    check(counts[0] == 0u, "zero_alloc: no heap allocation in steady-state frames");
}

// ---- layout -----------------------------------------------------------------------------------------------------------

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

std::vector<std::string> pushFields(const std::string& text, const char* name) {
    std::vector<std::string> out;
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
        std::size_t begin = end;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(decl[begin - 1])) || decl[begin - 1] == '_')) {
            --begin;
        }
        out.push_back(decl.substr(begin, end - begin));
        p = semi + 1u;
    }
    return out;
}

/// Value of `VOL_CONST uint <name> = <v>u;` in the shader text (~0u when absent).
u32 shaderConst(const std::string& text, const std::string& name) {
    const std::size_t at = text.find("uint " + name + " = ");
    if (at == std::string::npos) {
        return ~0u;
    }
    return static_cast<u32>(std::strtoul(text.c_str() + at + name.size() + 8u, nullptr, 0));
}

void suiteLayout() {
    const std::string dir = FUSE_RL_VOL_SHADER_DIR;
    const std::vector<std::string> expect = {"block", "tlas", "lightTable", "lightTree", "stage", "count",
                                             "reserved0", "reserved1"};
    const std::vector<std::string> a = pushFields(readText(dir + "/rl_vol.slang"), "struct VolPush");
    const std::vector<std::string> b = pushFields(readText(dir + "/rl_vol.comp"), "uniform VolPush");
    std::printf("layout: VolPush slang %zu / glsl %zu fields (C++ VolumetricsGpu::Push: 4 u64 + 4 u32 = 48 bytes)\n",
                a.size(), b.size());
    check(a == expect && b == expect, "layout: VolPush (rl_vol.slang / .comp) == VolumetricsGpu::Push field order");
    const std::string t = readText(dir + "/rl_vol_types.h");
    struct C {
        const char* name;
        u32 value;
    };
    const C consts[] = {{"kVolParamWords", vol::kVolParamWordCount}, {"kVolIntWords", vol::kVolIntWordCount},
                        {"kVolMaxSystems", vol::kVolSystemCount},    {"kVolMaxLayers", vol::kVolLayerCount},
                        {"kVolBufCurrent", vol::kVolCurrent},        {"kVolBufHistPrev", vol::kVolHistPrev},
                        {"kVolBufHistCur", vol::kVolHistCur},        {"kVolBufResPrev", vol::kVolResPrev},
                        {"kVolBufResCur", vol::kVolResCur},          {"kVolBufIntegrated", vol::kVolIntegrated},
                        {"kVolBufColorIn", vol::kVolColorIn},        {"kVolBufColorOut", vol::kVolColorOut},
                        {"kVolStageInject", vol::kVolInject},        {"kVolStageTemporal", vol::kVolTemporal},
                        {"kVolStageIntegrate", vol::kVolIntegrate},  {"kVolStageApply", vol::kVolApply},
                        {"kVolFlagHistory", vol::kVolHistory},       {"kVolFlagReproject", vol::kVolReproject},
                        {"kVolFlagReuse", vol::kVolReuse},           {"kVolFlagShadows", vol::kVolShadows},
                        {"kVolFlagLights", vol::kVolLights},         {"kVolFlagFog", vol::kVolFog},
                        {"kVolFlagParticles", vol::kVolParticles},   {"kVolBlendAlpha", vol::kVolAlpha},
                        {"kVolBlendAdditive", vol::kVolAdditive},    {"kVolBlendPremultiplied", vol::kVolPremultiplied},
                        {"kVolBlendMultiply", vol::kVolMultiply},    {"kVolSystemSoftDisc", vol::kVolSoftDisc}};
    u32 ok = 0;
    for (const C& c : consts) {
        const u32 v = shaderConst(t, c.name);
        if (v == c.value) {
            ++ok;
        } else {
            std::fprintf(stderr, "  %s: shader %u, C++ %u\n", c.name, v, c.value);
        }
    }
    std::printf("layout: %u/%zu rl_vol_types.h constants == C++\n", ok, sizeof(consts) / sizeof(consts[0]));
    check(ok == sizeof(consts) / sizeof(consts[0]), "layout: rl_vol_types.h constants == volumetrics.hpp");
    // The core's local layer arrays are sized 16 literally.
    check(vol::kVolLayerCount == 16u, "layout: kVolMaxLayers == the core's 16-entry layer arrays");
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
    const Suite suites[] = {{"fog_d3d", suiteFogD3d},         {"convergence", suiteConvergence},
                            {"reuse", suiteReuse},             {"ghosting", suiteGhosting},
                            {"shadows", suiteShadows},         {"particles", suiteParticles},
                            {"determinism", suiteDeterminism}, {"zero_alloc", suiteZeroAlloc},
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
    std::printf("PASS: rl_vol %s\n", suite.c_str());
    return 0;
}
