// FUSE Relight RL-5.5 CPU gates (native; PE through the Wine emulator in the MinGW tree): the path tracer's denoiser on
// the RL-5.1 test scenes, with the CPU reference path tracer as the producer (PtDenoiseCpu: the same packing as the GPU
// output sections, the gradient producer's C++ dialect, RdnReference).
//
//   options      rtx.useDenoiser / relight.denoise.* defaults and PtDenoiseConfig::fromOptions (rtx.conf layers).
//   select       the plugin switch: disabled -> none; dlss_rr upscaler -> the upscaler denoises; "nrd" without a
//                provider -> in-tree fallback with the registry's reason; a mock plugin registered for REBLUR ->
//                selectable but not bindable here (fallback, named in the reason); unknown ids -> fallback.
//   quality      Cornell box (mirror, rough gold sphere, glass, alpha panel), static camera, 1 spp per frame with a fresh
//                seed, 8 independent 16-frame sequences: over the last 4 frames of each the denoised demodulated
//                diffuse and specular channels and the remodulated (non-emissive) radiance reduce the squared error
//                against the 1024-spp reference >= 4x (the plan's rl_denoise_* bar); |bias| of the denoised mean
//                against the reference <= kMaxBias (the input's own bias over the same frames is printed: the metric's
//                sampling noise).
//   gradient     the A-SVGF producer: static scene -> every valid sample re-shades to exactly its previous value; after
//                the light is scaled by 0.25 -> dCur = 0.25 dPrev (same paths) on >= 95% of the lit samples.
//   asvgf        the light x 0.25 at frame 12: A-SVGF's mean relative error over frames 12..15 against the new reference
//                is at most half of the gradient-free run's.
//   determinism  two runs bit-identical (gradient records and denoised output).
//   zero_alloc   steady-state denoiser frames on the path tracer's inputs: 0 heap allocations (the CPU path tracer's
//                own allocations - the WP-6.0 CPU BVH oracle - are reported, informational).
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/denoise/pt_denoise.hpp>

#include "pt_reference_kernels.hpp"

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
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
using namespace pt_test;
namespace dn = fuse::relight::render::denoise;
namespace rdn = fuse::renderer::denoise;
namespace ptk = fuse::relight::ptk;
namespace opt = fuse::relight::options;

/// rtx.sceneScale belongs to scene/instances (linked in the runtime); the light set reads it by name.
struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kW = 64u, kH = 48u;
constexpr u32 kRefSpp = 1024u;
constexpr f64 kMinReduction = 4.0;
constexpr f64 kMaxBias = 0.05;

f64 lum3(f64 r, f64 g, f64 b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }

pt::PtSettings frameSettings() {
    pt::PtSettings st;
    st.maxBounces = 4;
    st.samplesPerPixel = 1;
    return st;
}

bool compileScene(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    if (!ok) {
        std::fprintf(stderr, "FAIL: compile: %s\n", error.c_str());
        ++g_failures;
    }
    return ok;
}

/// The Cornell box lit by its sphere light only (the emissive panel dark): a light change scales everything.
pt::PtScene sphereLitCornell(float scale) {
    pt::PtScene s = cornell(true);
    s.materials[3].bsdf.emission = bk::float3(0.f, 0.f, 0.f);
    s.lights[0] =
        rl::makeSphereLight(lk::float3(0.9f, 1.4f, 0.6f), 0.25f, lk::float3(40.f * scale, 34.f * scale, 28.f * scale));
    return s;
}

bool loadRtxConf(const char* name, const char* text) {
    opt::OptionSystem::shutdown();
    FILE* f = std::fopen(name, "wb");
    if (f == nullptr) {
        check(false, "write rtx.conf");
        return false;
    }
    std::fputs(text, f);
    std::fclose(f);
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, name);
    opt::OptionSystemDesc desc;
    desc.loadEnvironmentVariables = false;
    (void)opt::OptionSystem::initialize(desc);
    opt::OptionManager::applyPendingValues(nullptr, false);
    return true;
}

// --- options / select ------------------------------------------------------------------------------------------------
void suiteOptions() {
    dn::registerDenoiseOptions();
    check(dn::RemixDenoiseOptions::useDenoiser() && !dn::DenoiseOptions::enable() &&
              dn::DenoiseOptions::backend().empty() && dn::DenoiseOptions::gradients() &&
              dn::DenoiseOptions::virtualMotion() && dn::DenoiseOptions::atrousIterations() == 5,
          "option defaults");
    dn::PtDenoiseConfig c = dn::PtDenoiseConfig::fromOptions();
    check(!c.enabled, "denoiser off by default (relight.denoise.enable)");
    check(c.settings.motionScale == 1.f && c.settings.instanceTest && c.settings.gradients,
          "path-tracer settings: previous - current motion, instance test, gradients");
    if (loadRtxConf("rl_denoise_a.rtx.conf", "relight.denoise.enable = True\nrelight.denoise.atrousIterations = 9\n"
                                             "relight.denoise.gradients = False\n")) {
        c = dn::PtDenoiseConfig::fromOptions();
        check(c.enabled && c.settings.atrousIterations == 5u && !c.settings.gradients, "options applied and clamped");
    }
    if (loadRtxConf("rl_denoise_b.rtx.conf", "relight.denoise.enable = True\nrtx.useDenoiser = False\n")) {
        check(!dn::PtDenoiseConfig::fromOptions().enabled, "rtx.useDenoiser = False disables it");
    }
    loadRtxConf("rl_denoise_c.rtx.conf", "\n");
    std::printf("options: defaults and fromOptions ok\n");
}

std::unique_ptr<rdn::IDenoiser> mockFactory(const rdn::DenoiserCreateInfo&) { return nullptr; }

void suiteSelect() {
    rdn::DenoiserRegistry reg;
    reg.register_builtin_denoisers();
    dn::PtDenoiseConfig c;
    c.enabled = false;
    check(dn::selectPtDenoiser(c, "native_taau", reg).backend == "none", "disabled -> none");
    c.enabled = true;
    dn::PtDenoiseSelection s = dn::selectPtDenoiser(c, "native_taau", reg);
    check(s.backend == "rdn" && s.active && !s.fallback, "default -> in-tree rdn");
    s = dn::selectPtDenoiser(c, "dlss_rr", reg);
    check(s.backend == "dlss_rr" && !s.active, "dlss_rr upscaler replaces the denoiser");
    c.backend = "nrd";
    s = dn::selectPtDenoiser(c, "fsr1", reg);
    std::printf("select: nrd without a provider -> %s (%s)\n", s.backend.c_str(), s.reason.c_str());
    check(s.backend == "rdn" && s.fallback && s.active && !s.reason.empty(), "nrd unavailable -> in-tree fallback");
    rdn::DenoiserCaps mock{};
    mock.name = "nrd";
    mock.display_name = "mock NRD";
    mock.license = "LicenseRef-mock";
    mock.methods = rdn::denoiser_method_bit(rdn::DenoiserMethod::Reblur);
    mock.signals = rdn::kAllDenoiseSignals;
    mock.hit_distance_methods = mock.methods;
    mock.needs_native_frame = false;
    check(reg.register_backend(mock, &mockFactory), "mock plugin registered");
    s = dn::selectPtDenoiser(c, "fsr1", reg);
    std::printf("select: mock nrd plugin -> %s (%s)\n", s.backend.c_str(), s.reason.c_str());
    check(s.backend == "rdn" && s.fallback && s.reason.find("nrd") != std::string::npos,
          "a selectable plugin is named; the path tracer has no plugin binding: in-tree fallback");
    c.backend = "bogus";
    s = dn::selectPtDenoiser(c, "fsr1", reg);
    check(s.backend == "rdn" && s.fallback, "unknown backend -> fallback");
}

// --- quality ---------------------------------------------------------------------------------------------------------
struct Err {
    f64 noisy = 0.0, dn = 0.0, dnSum = 0.0, refSum = 0.0, noisySum = 0.0;
    f64 noisyBias() const { return refSum > 0.0 ? (noisySum - refSum) / refSum : 0.0; }
    f64 reduction() const { return dn > 0.0 ? noisy / dn : 1e30; }
    f64 bias() const { return refSum > 0.0 ? (dnSum - refSum) / refSum : 0.0; }
};

void suiteQuality() {
    const pt::PtScene s = cornell(true);
    pt::PtCompiledScene c;
    if (!compileScene(s, c)) {
        return;
    }
    const pt::PtSettings st = frameSettings();
    pt::PtReferenceImage ref;
    ref.resize(kW, kH);
    pt::PtSettings rs = st;
    rs.samplesPerPixel = kRefSpp;
    pt::PtReferenceStats stats{};
    check(pt::renderReference(c, rs, kW, kH, 0xC0FFEEu, 0u, kRefSpp, ref, kernel::Backend::CpuParallel, &stats),
          "reference render");
    std::printf("quality: %u x %u reference at %u spp: %.1f s\n", kW, kH, kRefSpp, stats.seconds);
    dn::PtDenoiseCpu cpu;
    rdn::RdnSettings qs = dn::ptDenoiseSettings();
    Err ed, es, er;
    constexpr u32 kFrames = 16u;
    constexpr u32 kRuns = 8u; // independent sequences: the bias statistic needs the ensemble (heavy-tailed 1-spp input)
    for (u32 t = 0; t < kFrames * kRuns; ++t) {
        if (t % kFrames == 0u) {
            cpu.init(kW, kH, qs);
        }
        check(cpu.runFrame(c, st, 1000u + t, 0u), "PtDenoiseCpu frame");
        if (t % kFrames + 4u < kFrames) {
            continue;
        }
        const rdn::rdnk::float4* dd = cpu.denoisedDiffuse();
        const rdn::rdnk::float4* ds = cpu.denoisedSpecular();
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const usize i = static_cast<usize>(y) * kW + x;
                if (!(cpu.depth()[i] > 0.f)) {
                    continue;
                }
                const f64 rd = lum3(ref.channel(x, y, 1, 0), ref.channel(x, y, 1, 1), ref.channel(x, y, 1, 2));
                const f64 rsp = lum3(ref.channel(x, y, 2, 0), ref.channel(x, y, 2, 1), ref.channel(x, y, 2, 2));
                const rdn::rdnk::float4& nd = cpu.noisyDiffuse()[i];
                const rdn::rdnk::float4& ns = cpu.noisySpecular()[i];
                const f64 a = lum3(nd.x, nd.y, nd.z) - rd;
                const f64 b = lum3(dd[i].x, dd[i].y, dd[i].z) - rd;
                ed.noisy += a * a;
                ed.dn += b * b;
                ed.dnSum += lum3(dd[i].x, dd[i].y, dd[i].z);
                ed.noisySum += lum3(nd.x, nd.y, nd.z);
                ed.refSum += rd;
                const f64 a2 = lum3(ns.x, ns.y, ns.z) - rsp;
                const f64 b2 = lum3(ds[i].x, ds[i].y, ds[i].z) - rsp;
                es.noisy += a2 * a2;
                es.dn += b2 * b2;
                es.dnSum += lum3(ds[i].x, ds[i].y, ds[i].z);
                es.noisySum += lum3(ns.x, ns.y, ns.z);
                es.refSum += rsp;
                // Remodulated radiance (the post composite: emissive + D albedoD + S
                // albedoS) without the emissive term, which the denoiser does not touch
                // (light sources seen directly, the PSR chain's emission).
                const ptk::PtReferencePixel& p = cpu.image().pixel(x, y);
                const f64 inv = 1.0 / std::max<u32>(p.samples, 1u);
                const rdn::rdnk::float4& aD = cpu.albedoD()[i];
                const rdn::rdnk::float4& aS = cpu.albedoS()[i];
                const f64 comp = lum3(dd[i].x * aD.x + ds[i].x * aS.x, dd[i].y * aD.y + ds[i].y * aS.y,
                                      dd[i].z * aD.z + ds[i].z * aS.z);
                const f64 noisyRad = lum3((p.sum[0] - p.emissive[0]) * inv, (p.sum[1] - p.emissive[1]) * inv,
                                          (p.sum[2] - p.emissive[2]) * inv);
                const f64 refRad =
                    lum3(ref.mean(x, y, 0) - ref.channel(x, y, 0, 0), ref.mean(x, y, 1) - ref.channel(x, y, 0, 1),
                         ref.mean(x, y, 2) - ref.channel(x, y, 0, 2));
                er.noisy += (noisyRad - refRad) * (noisyRad - refRad);
                er.dn += (comp - refRad) * (comp - refRad);
                er.dnSum += comp;
                er.noisySum += noisyRad;
                er.refSum += refRad;
            }
        }
    }
    std::printf("quality (Cornell, 1 spp, last 4 of 16 frames x 8 sequences vs "
                "%u spp): diffuse reduction %.2fx bias %+.4f | "
                "specular reduction %.2fx bias %+.4f | radiance reduction %.2fx bias %+.4f\n",
                kRefSpp, ed.reduction(), ed.bias(), es.reduction(), es.bias(), er.reduction(), er.bias());
    std::printf("quality: input bias over the same frames (the metric's own noise): diffuse %+.4f specular %+.4f "
                "radiance %+.4f\n",
                ed.noisyBias(), es.noisyBias(), er.noisyBias());
    check(ed.reduction() >= kMinReduction, "diffuse error reduction >= 4x");
    check(es.reduction() >= kMinReduction, "specular error reduction >= 4x");
    check(er.reduction() >= kMinReduction, "radiance error reduction >= 4x");
    // Bias: the denoised mean against the converged reference (the input's own
    // mean over the same frames is printed as the metric's sampling noise: 1-spp
    // path-traced input is heavy-tailed - caustics through the glass sphere).
    check(std::fabs(ed.bias()) <= kMaxBias, "diffuse |bias| <= kMaxBias");
    check(std::fabs(es.bias()) <= kMaxBias, "specular |bias| <= kMaxBias");
    check(std::fabs(er.bias()) <= kMaxBias, "radiance |bias| <= kMaxBias");
}

// --- gradient --------------------------------------------------------------------------------------------------------
void suiteGradient() {
    pt::PtScene s = sphereLitCornell(1.f);
    pt::PtCompiledScene c;
    if (!compileScene(s, c)) {
        return;
    }
    const pt::PtSettings st = frameSettings();
    dn::PtDenoiseCpu cpu;
    cpu.init(kW, kH, dn::ptDenoiseSettings());
    u32 valid = 0, exact = 0, lit = 0, scaled = 0;
    for (u32 t = 0; t < 4u; ++t) {
        check(cpu.runFrame(c, st, 50u + t, 0u), "frame");
        if (t == 0u) {
            continue;
        }
        for (const pt::Word& g : cpu.gradient()) {
            if (g.y < 0.f) {
                continue;
            }
            ++valid;
            exact += g.x == g.y && g.z == g.w ? 1u : 0u;
        }
    }
    const u32 strata = static_cast<u32>(cpu.gradient().size());
    s = sphereLitCornell(0.25f);
    check(c.update(s), "light update");
    check(cpu.runFrame(c, st, 60u, 0u), "frame after the light change");
    for (const pt::Word& g : cpu.gradient()) {
        if (g.y > 1e-3f) {
            ++lit;
            scaled += std::fabs(g.x - 0.25f * g.y) <= 1e-3f * g.y + 1e-6f ? 1u : 0u;
        }
    }
    std::printf("gradient: static frames: %u valid samples (of %u strata x 3), "
                "%u re-shade exactly; after light x 0.25: %u lit samples, %u at 0.25 x prev\n",
                valid, strata, exact, lit, scaled);
    check(valid > strata, "most strata carry a valid gradient sample");
    check(exact == valid, "static scene: every re-shaded sample equals its previous value");
    check(lit > strata / 4u && scaled * 100u >= lit * 95u, "light x 0.25: dCur = 0.25 dPrev on >= 95% of lit samples");
}

// --- asvgf -----------------------------------------------------------------------------------------------------------
void suiteAsvgf() {
    pt::PtScene s = sphereLitCornell(1.f);
    pt::PtCompiledScene c;
    if (!compileScene(s, c)) {
        return;
    }
    const pt::PtSettings st = frameSettings();
    pt::PtSettings rs = st;
    rs.samplesPerPixel = 256u;
    pt::PtReferenceImage ref[2];
    for (u32 k = 0; k < 2u; ++k) {
        ref[k].resize(kW, kH);
        if (k == 1u) {
            pt::PtScene s2 = sphereLitCornell(0.25f);
            pt::PtCompiledScene c2;
            compileScene(s2, c2);
            pt::renderReference(c2, rs, kW, kH, 0xBEEFu, 0u, rs.samplesPerPixel, ref[k]);
        } else {
            pt::renderReference(c, rs, kW, kH, 0xBEEFu, 0u, rs.samplesPerPixel, ref[k]);
        }
    }
    f64 after[2] = {0.0, 0.0};
    for (u32 g = 0; g < 2u; ++g) {
        pt::PtScene sc = sphereLitCornell(1.f);
        pt::PtCompiledScene cc;
        compileScene(sc, cc);
        rdn::RdnSettings set = dn::ptDenoiseSettings();
        set.gradients = g == 1u;
        dn::PtDenoiseCpu cpu;
        cpu.init(kW, kH, set);
        for (u32 t = 0; t < 16u; ++t) {
            if (t == 12u) {
                sc = sphereLitCornell(0.25f);
                check(cc.update(sc), "light update");
            }
            check(cpu.runFrame(cc, st, 300u + t, 0u), "frame");
            if (t < 12u) {
                continue;
            }
            f64 e = 0.0, tot = 0.0;
            for (u32 y = 0; y < kH; ++y) {
                for (u32 x = 0; x < kW; ++x) {
                    const usize i = static_cast<usize>(y) * kW + x;
                    if (!(cpu.depth()[i] > 0.f)) {
                        continue;
                    }
                    const rdn::rdnk::float4& d = cpu.denoisedDiffuse()[i];
                    const rdn::rdnk::float4& sp = cpu.denoisedSpecular()[i];
                    const f64 r =
                        lum3(ref[1].channel(x, y, 1, 0), ref[1].channel(x, y, 1, 1), ref[1].channel(x, y, 1, 2)) +
                        lum3(ref[1].channel(x, y, 2, 0), ref[1].channel(x, y, 2, 1), ref[1].channel(x, y, 2, 2));
                    e += std::fabs(lum3(d.x, d.y, d.z) + lum3(sp.x, sp.y, sp.z) - r);
                    tot += r;
                }
            }
            after[g] += e / tot / 4.0;
        }
    }
    std::printf("asvgf (Cornell, light x 0.25 at frame 12): mean relative error "
                "over frames 12..15: SVGF temporal %.3f, A-SVGF %.3f (ratio %.3f)\n",
                after[0], after[1], after[1] / after[0]);
    check(after[1] <= 0.5 * after[0], "A-SVGF at most half the lag error after a light change");
}

// --- determinism / zero_alloc ----------------------------------------------------------------------------------------
void suiteDeterminism() {
    const pt::PtScene s = cornell(true);
    pt::PtCompiledScene c;
    if (!compileScene(s, c)) {
        return;
    }
    const pt::PtSettings st = frameSettings();
    dn::PtDenoiseCpu a, b;
    a.init(48u, 32u, dn::ptDenoiseSettings());
    b.init(48u, 32u, dn::ptDenoiseSettings());
    for (u32 t = 0; t < 5u; ++t) {
        a.runFrame(c, st, 7u + t, 0u);
    }
    for (u32 t = 0; t < 5u; ++t) {
        b.runFrame(c, st, 7u + t, 0u);
    }
    const usize n = 48u * 32u;
    const bool same = std::memcmp(a.denoisedDiffuse(), b.denoisedDiffuse(), n * 16u) == 0 &&
                      std::memcmp(a.denoisedSpecular(), b.denoisedSpecular(), n * 16u) == 0 &&
                      std::memcmp(a.gradient().data(), b.gradient().data(), a.gradient().size() * 16u) == 0;
    std::printf("determinism: two 5-frame runs bit-identical: %s\n", same ? "yes" : "NO");
    check(same, "determinism");
}

void suiteZeroAlloc() {
    const pt::PtScene s = cornell(false);
    pt::PtCompiledScene c;
    if (!compileScene(s, c)) {
        return;
    }
    const pt::PtSettings st = frameSettings();
    dn::PtDenoiseCpu cpu;
    cpu.init(32u, 24u, dn::ptDenoiseSettings());
    cpu.runFrame(c, st, 1u, 0u);
    cpu.runFrame(c, st, 2u, 0u);
    // The denoiser on the path tracer's inputs (the runtime part; the GPU twin is gated by rl_denoise_vk).
    rdn::RdnReference ref;
    ref.init(32u, 24u, dn::ptDenoiseSettings());
    rdn::RdnReferenceInputs in{};
    in.diffuse = cpu.noisyDiffuse().data();
    in.specular = cpu.noisySpecular().data();
    in.normal = cpu.normal().data();
    in.depth = cpu.depth().data();
    in.motion = cpu.motion().data();
    in.instance = cpu.instance().data();
    in.gradient = reinterpret_cast<const rdn::rdnk::float4*>(cpu.gradient().data());
    rdn::RdnCamera cam{};
    ref.runFrame(in, cam, cam);
    t_allocations = 0;
    t_count = true;
    for (u32 t = 0; t < 3u; ++t) {
        ref.runFrame(in, cam, cam);
    }
    t_count = false;
    const unsigned long long denoiser = t_allocations;
    t_allocations = 0;
    t_count = true;
    for (u32 t = 3; t < 6u; ++t) {
        cpu.runFrame(c, st, t, 0u);
    }
    t_count = false;
    std::printf("zero_alloc: 3 steady-state denoiser frames on the path tracer's inputs: %llu heap allocations; whole "
                "PtDenoiseCpu frames (CPU path tracer + producer + denoiser): %llu (informational: the WP-6.0 CPU BVH "
                "oracle passes its per-ray callbacks as std::function)\n",
                denoiser, t_allocations);
    check(denoiser == 0u, "the denoiser makes no heap allocation per steady-state frame");
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    dn::registerDenoiseOptions();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    struct Entry {
        const char* name;
        void (*fn)();
    };
    const Entry entries[] = {{"options", suiteOptions},     {"select", suiteSelect}, {"quality", suiteQuality},
                             {"gradient", suiteGradient},   {"asvgf", suiteAsvgf},   {"determinism", suiteDeterminism},
                             {"zero_alloc", suiteZeroAlloc}};
    bool ran = false;
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
