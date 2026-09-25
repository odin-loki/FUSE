// WP-7.3 path-tracing mode, CPU gates (stub-safe; the Lavapipe gates are test_rp_pathtrace.cpp).
//
//   layout          PtFrame / PtPush / PtHit / PtLight of pt_common.{glsl,slang} == the C++ records (names, order,
//                   offsets, sizes); PtBufferLayout sections 256-aligned and disjoint
//   bsdf            pt_bsdf.hpp: Lambert importance weight == albedo exactly; every lobe's pdf integrates (uniform
//                   sphere MC) to the fraction of valid samples; importance-sampled albedo == uniform-MC albedo
//                   within 5 sigma; GGX weak white furnace (integral of G1 D (wo.h)+ / wo.z == 1); reciprocity;
//                   no configuration reflects more than it receives
//   furnace         CPU reference, Furnace scene: every box pixel == albedo (to 1e-12), sky pixels == 1; with Russian
//                   roulette from bounce 0 the mean stays within 5 sigma of the albedo
//   analytic        CPU reference, Analytic scene: MIS, NEE-only and BSDF-only estimates of the ground below a square
//                   emitter == rho Le F (point-to-parallel-rectangle form factor) within 4 sigma and 1 %
//   strategies      CPU reference, Converge scene (24 x 16): MIS == NEE-only per pixel (4 sigma, >= 99 % of the
//                   channel tests; per-channel image mean within 4 sigma), BSDF-only == MIS with the point light dark (98 %), Russian
//                   roulette on == off; MIS has the lowest noise
//   determinism     CPU reference render 1 thread == 4 threads == again, bit for bit
//   api             settings sanitised, frame-constant flags per strategy, emitter map (build / lookup / rejects),
//                   SBT layouts for several device property sets, the T2 / T3 capability rule on synthetic caps,
//                   PathTracerGpu without a device fails cleanly
//   reconstruction  selectPtReconstruction over fresh registries with mock backends: DLSS RR (mock upscaler) first,
//                   an NRD RELAX mock (IDenoiser) when it can serve the path tracer's inputs, else the in-tree SVGF,
//                   else accumulation; the chosen IDenoiser is created through the registry factory
//   zero_alloc      steady-state reference pixels, frame constants and emitter lookups make no heap allocation
#include "test_rp_pathtrace_scene.hpp"

#include <fuse/renderer/pathtrace/pt_gpu.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
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

using namespace fuse::renderer;
using namespace fuse::renderer::pathtrace;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

#if defined(NDEBUG)
constexpr u32 kStrategySamples = 2048;
constexpr u32 kAnalyticSamples = 1u << 18;
constexpr u32 kBsdfSamples = 400000;
#else
constexpr u32 kStrategySamples = 1024;
constexpr u32 kAnalyticSamples = 1u << 16;
constexpr u32 kBsdfSamples = 100000;
#endif
constexpr u32 kThreads = 4;

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define PT_FIELD(T, n) Field{#n, offsetof(T, n)}
const Field kFrameFields[] = {
    PT_FIELD(PtFrameConstants, tlas),          PT_FIELD(PtFrameConstants, scene),          PT_FIELD(PtFrameConstants, lightTree),
    PT_FIELD(PtFrameConstants, lights),        PT_FIELD(PtFrameConstants, emitterMap),     PT_FIELD(PtFrameConstants, accum),
    PT_FIELD(PtFrameConstants, accumSq),       PT_FIELD(PtFrameConstants, mean),           PT_FIELD(PtFrameConstants, signal),
    PT_FIELD(PtFrameConstants, depth),         PT_FIELD(PtFrameConstants, normal),         PT_FIELD(PtFrameConstants, albedo),
    PT_FIELD(PtFrameConstants, invViewProj),   PT_FIELD(PtFrameConstants, cameraPosition), PT_FIELD(PtFrameConstants, width),
    PT_FIELD(PtFrameConstants, cameraForward), PT_FIELD(PtFrameConstants, height),         PT_FIELD(PtFrameConstants, sky),
    PT_FIELD(PtFrameConstants, flags),         PT_FIELD(PtFrameConstants, frameIndex),     PT_FIELD(PtFrameConstants, seed),
    PT_FIELD(PtFrameConstants, samplesPerFrame), PT_FIELD(PtFrameConstants, maxBounces),   PT_FIELD(PtFrameConstants, rrStartBounce),
    PT_FIELD(PtFrameConstants, lightCount),    PT_FIELD(PtFrameConstants, cullMask),       PT_FIELD(PtFrameConstants, emitterMapSlots),
    PT_FIELD(PtFrameConstants, rayTMin),       PT_FIELD(PtFrameConstants, normalBias),     PT_FIELD(PtFrameConstants, viewBias),
    PT_FIELD(PtFrameConstants, clampRadiance), PT_FIELD(PtFrameConstants, farDistance),    PT_FIELD(PtFrameConstants, minRoughness),
    PT_FIELD(PtFrameConstants, sampleBase),    PT_FIELD(PtFrameConstants, reserved),
};
const Field kPushFields[] = {PT_FIELD(PtPush, frame), PT_FIELD(PtPush, pass), PT_FIELD(PtPush, reserved)};
const Field kHitFields[] = {PT_FIELD(PtHit, position), PT_FIELD(PtHit, t),        PT_FIELD(PtHit, normal),   PT_FIELD(PtHit, instance),
                            PT_FIELD(PtHit, albedo),   PT_FIELD(PtHit, primitive), PT_FIELD(PtHit, emission), PT_FIELD(PtHit, emitter),
                            PT_FIELD(PtHit, roughness), PT_FIELD(PtHit, metallic), PT_FIELD(PtHit, flags),    PT_FIELD(PtHit, reserved)};
const Field kLightFields[] = {PT_FIELD(restir::RestirLight, radiance), PT_FIELD(restir::RestirLight, kind),
                              PT_FIELD(restir::RestirLight, cosInner), PT_FIELD(restir::RestirLight, cosOuter),
                              PT_FIELD(restir::RestirLight, flags),    PT_FIELD(restir::RestirLight, reserved)};

std::string readText(const std::string& path) {
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// Parses `struct <name> {...};` of a shader source: (name, std430 / scalar offset) per field.
bool parseShaderStruct(const std::string& text, const std::string& name, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    const std::string key = "struct " + name + " {";
    const size_t begin = text.find(key);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + key.size(), end - begin - key.size()));
    std::string line;
    size_t offset = 0;
    size_t align = 4;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        size_t count = 1;
        const size_t bracket = decl.find('[');
        if (bracket != std::string::npos) {
            count = static_cast<size_t>(std::stoul(decl.substr(bracket + 1)));
            decl = decl.substr(0, bracket);
        }
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        align = std::max(align, bytes);
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = (offset + align - 1u) / align * align;
    return true;
}

template <size_t N>
void checkStruct(const std::string& text, const char* lang, const char* shaderName, const Field (&fields)[N], size_t cppSize) {
    std::vector<size_t> offsets;
    std::vector<std::string> names;
    size_t size = 0;
    const bool parsed = parseShaderStruct(text, shaderName, offsets, size, names);
    expect(parsed, "shader struct parsed");
    if (!parsed) {
        std::fprintf(stderr, "  %s: struct %s missing\n", lang, shaderName);
        return;
    }
    bool same = offsets.size() == N && size == cppSize;
    for (size_t i = 0; same && i < N; ++i) {
        same = names[i] == fields[i].name && offsets[i] == fields[i].offset;
        if (!same) {
            std::fprintf(stderr, "  %s %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, shaderName, i, names[i].c_str(), offsets[i],
                         fields[i].name, fields[i].offset);
        }
    }
    if (offsets.size() != N || size != cppSize) {
        std::fprintf(stderr, "  %s %s: %zu fields / %zu bytes vs C++ %zu / %zu\n", lang, shaderName, offsets.size(), size, N, cppSize);
    }
    std::printf("layout: %s %s %zu fields, %zu bytes\n", lang, shaderName, offsets.size(), size);
    expect(same, "shader struct == C++ record (names, order, offsets, size)");
}

void testLayout() {
    const std::string dir = FUSE_RP_PATHTRACE_SHADER_DIR;
    for (const char* lang : {"glsl", "slang"}) {
        const std::string text = readText(dir + "/pt_common." + lang);
        expect(!text.empty(), "pt_common source readable");
        checkStruct(text, lang, "PtFrame", kFrameFields, sizeof(PtFrameConstants));
        checkStruct(text, lang, "PtPush", kPushFields, sizeof(PtPush));
        checkStruct(text, lang, "PtHit", kHitFields, sizeof(PtHit));
        checkStruct(text, lang, "PtLight", kLightFields, sizeof(restir::RestirLight));
    }
    // The payload wraps PtHit + 4 words in both languages.
    const std::string glslPayload = readText(dir + "/pt_payload.glsl");
    const std::string slang = readText(dir + "/pt_common.slang");
    expect(glslPayload.find("PtHit hit;") != std::string::npos && slang.find("struct PtPayload {\n    PtHit hit;") != std::string::npos,
           "PtPayload = PtHit + origin words in both languages");
    for (const auto& ext : {std::pair<u32, u32>{37u, 23u}, std::pair<u32, u32>{1u, 1u}, std::pair<u32, u32>{1920u, 1080u}}) {
        const PtBufferLayout l = PtBufferLayout::compute(ext.first, ext.second);
        const u64 px = static_cast<u64>(ext.first) * ext.second;
        const u64 state[2][2] = {{l.accum, px * 16u}, {l.accumSq, px * 16u}};
        const u64 out[5][2] = {{l.output, px * 16u}, {l.signal, px * 16u}, {l.depth, px * 4u}, {l.normal, px * 16u}, {l.albedo, px * 16u}};
        bool ok = state[0][0] + state[0][1] <= state[1][0] && state[1][0] + state[1][1] <= l.stateBytes;
        for (u32 i = 0; i < 5u; ++i) {
            ok = ok && out[i][0] % 256u == 0u && out[i][0] + out[i][1] <= (i + 1u < 5u ? out[i + 1u][0] : l.outputBytes);
        }
        ok = ok && l.accumSq % 256u == 0u;
        expect(ok, "PtBufferLayout sections 256-aligned and disjoint");
    }
    std::printf("layout: PtBufferLayout 1080p state %.1f MiB, output %.1f MiB\n",
                static_cast<f64>(PtBufferLayout::compute(1920, 1080).stateBytes) / 1048576.0,
                static_cast<f64>(PtBufferLayout::compute(1920, 1080).outputBytes) / 1048576.0);
}

// --- BSDF ------------------------------------------------------------------------------------------
u64 g_rng = 0x1234567ull;
f64 urand() {
    g_rng += 0x9E3779B97F4A7C15ull;
    u64 z = g_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<f64>(z >> 11) * (1.0 / 9007199254740992.0);
}

PtV3d dirFromCos(f64 cosTheta) {
    const f64 s = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
    return PtV3d{s, 0.0, cosTheta};
}

void testBsdf() {
    const f64 minRough = 0.05;
    // Lambert: the weight is the albedo exactly.
    {
        PtMaterialT<f64> m{};
        m.albedo = PtV3d{0.3, 0.6, 0.9};
        m.metallic = 0.0;
        f64 worst = 0.0;
        for (u32 i = 0; i < 20000u; ++i) {
            const PtBsdfSample<f64> s = ptBsdfSample(m, minRough, dirFromCos(0.05 + 0.95 * urand()), urand(), urand(), urand());
            if (s.valid) {
                worst = std::max(worst, std::fabs(s.weight.y - 0.6));
            }
        }
        std::printf("bsdf: Lambert |weight - albedo| max %.2e\n", worst);
        expect(worst < 1e-12, "Lambert importance weight == albedo");
    }
    struct Case {
        f64 metallic;
        f64 roughness;
        f64 cosO;
    };
    const Case cases[] = {{1.0, 0.45, 0.8}, {1.0, 0.2, 0.5}, {1.0, 0.9, 0.2}, {0.5, 0.3, 0.7}, {0.5, 0.6, 0.15}, {0.25, 0.1, 0.9}};
    for (const Case& c : cases) {
        PtMaterialT<f64> m{};
        m.albedo = PtV3d{0.95, 0.64, 0.54};
        m.metallic = c.metallic;
        m.roughness = c.roughness;
        const PtV3d wo = dirFromCos(c.cosO);
        // Importance-sampled albedo (y channel) and the valid-sample fraction.
        f64 sum = 0.0;
        f64 sum2 = 0.0;
        u32 valid = 0;
        for (u32 i = 0; i < kBsdfSamples; ++i) {
            const PtBsdfSample<f64> s = ptBsdfSample(m, minRough, wo, urand(), urand(), urand());
            const f64 w = s.valid ? s.weight.y : 0.0;
            valid += s.valid ? 1u : 0u;
            sum += w;
            sum2 += w * w;
        }
        const f64 n = kBsdfSamples;
        const f64 isMean = sum / n;
        const f64 isSe = std::sqrt(std::max(sum2 / n - isMean * isMean, 0.0) / n);
        // Uniform-hemisphere MC of the albedo and of the pdf's integral.
        f64 u = 0.0;
        f64 u2 = 0.0;
        f64 p = 0.0;
        f64 p2 = 0.0;
        f64 recip = 0.0;
        for (u32 i = 0; i < kBsdfSamples; ++i) {
            const f64 z = urand();
            const f64 phi = 2.0 * 3.14159265358979323846 * urand();
            const f64 r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const PtV3d wi{r * std::cos(phi), r * std::sin(phi), z};
            const PtV3d f = ptBsdfEval(m, minRough, wo, wi);
            const f64 v = f.y * wi.z * 2.0 * 3.14159265358979323846;
            const f64 q = ptBsdfPdf(m, minRough, wo, wi) * 2.0 * 3.14159265358979323846;
            u += v;
            u2 += v * v;
            p += q;
            p2 += q * q;
            const PtV3d fr = ptBsdfEval(m, minRough, wi, wo);
            recip = std::max(recip, std::fabs(fr.y - f.y) / std::max(1e-12, f.y));
        }
        const f64 uMean = u / n;
        const f64 uSe = std::sqrt(std::max(u2 / n - uMean * uMean, 0.0) / n);
        const f64 pMean = p / n;
        const f64 pSe = std::sqrt(std::max(p2 / n - pMean * pMean, 0.0) / n);
        const f64 validFraction = valid / n;
        const f64 zAlbedo = (isMean - uMean) / std::sqrt(isSe * isSe + uSe * uSe);
        const f64 zPdf = (pMean - validFraction) / std::sqrt(pSe * pSe + validFraction * (1.0 - validFraction) / n + 1e-30);
        // GGX weak white furnace: integral over h of G1(wo) D(h) max(0, wo.h) / wo.z == 1 (uniform h hemisphere).
        const f64 alpha = ptAlpha(c.roughness, minRough);
        f64 weak = 0.0;
        f64 weak2 = 0.0;
        for (u32 i = 0; i < kBsdfSamples; ++i) {
            const f64 z = urand();
            const f64 phi = 2.0 * 3.14159265358979323846 * urand();
            const f64 r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const PtV3d h{r * std::cos(phi), r * std::sin(phi), z};
            const f64 g1 = 1.0 / (1.0 + ptGgxLambda(alpha, wo.z));
            const f64 v = g1 * ptGgxD(alpha, h.z) * std::max(0.0, ptDot(wo, h)) / wo.z * 2.0 * 3.14159265358979323846;
            weak += v;
            weak2 += v * v;
        }
        weak /= n;
        const f64 weakSe = std::sqrt(std::max(weak2 / n - weak * weak, 0.0) / n);
        std::printf("bsdf: m %.2f r %.2f cos %.2f: albedo IS %.5f +- %.5f vs uniform %.5f +- %.5f (z %+.2f); pdf integral %.5f vs "
                    "valid %.5f (z %+.2f); weak furnace %.4f +- %.4f; reciprocity %.1e\n",
                    c.metallic, c.roughness, c.cosO, isMean, isSe, uMean, uSe, zAlbedo, pMean, validFraction, zPdf, weak, weakSe, recip);
        expect(std::fabs(zAlbedo) < 5.0, "importance-sampled albedo == uniform MC albedo (5 sigma)");
        expect(std::fabs(zPdf) < 5.0, "pdf integrates to the valid-sample fraction (5 sigma)");
        expect(std::fabs(weak - 1.0) < 5.0 * weakSe + 0.005, "GGX weak white furnace == 1 (5 sigma + 0.5 %)");
        expect(recip < 1e-9, "BSDF reciprocal");
        expect(uMean < 1.0 + 5.0 * uSe, "no energy gain");
    }
}

// --- CPU reference scenes ------------------------------------------------------------------------
struct CpuWorld {
    gpu_scene::GpuScene scene;
    pt_test::World world;
};

bool makeWorld(CpuWorld& w, pt_test::SceneKind kind, u32 width, u32 height, f32 albedo = 1.f) {
    gpu_scene::GpuSceneDesc d{};
    if (!w.scene.init(d)) {
        return false;
    }
    w.scene.beginFrame(1);
    if (!pt_test::buildWorld(w.world, w.scene, kind, width, height, albedo)) {
        return false;
    }
    return w.scene.commit().ok;
}

void testFurnace() {
    for (const f32 albedo : {1.f, 0.6f}) {
        CpuWorld w;
        expect(makeWorld(w, pt_test::SceneKind::Furnace, 16, 12, albedo), "furnace world");
        PtSettings s = pt_test::baseSettings(w.world);
        s.maxBounces = 8;
        PtReference ref;
        expect(ref.setup(s, w.world.camera, 16, 12), "reference setup");
        // Every path returns exactly albedo (box: one diffuse bounce, then the sky) or 1 (sky).
        u32 box = 0;
        u32 sky = 0;
        u32 other = 0;
        f64 worst = 0.0;
        for (u32 y = 0; y < 12u; ++y) {
            for (u32 x = 0; x < 16u; ++x) {
                u64 rng = 0x51ull + y * 16u + x;
                for (u32 i = 0; i < 32u; ++i) {
                    const PtV3d l = ref.path(w.world.cpu, x, y, rng);
                    const f64 db = std::max(std::fabs(l.x - albedo), std::max(std::fabs(l.y - albedo), std::fabs(l.z - albedo)));
                    const f64 ds = std::max(std::fabs(l.x - 1.0), std::max(std::fabs(l.y - 1.0), std::fabs(l.z - 1.0)));
                    if (albedo != 1.f && ds < 1e-12) {
                        ++sky;
                    } else if (db < 1e-9) {
                        ++box;
                        worst = std::max(worst, db);
                    } else {
                        ++other;
                        worst = std::max(worst, std::min(db, ds));
                    }
                }
            }
        }
        std::printf("furnace (albedo %.1f): %u box paths == albedo, %u sky paths == 1, %u other (max |L - expected| %.2e)\n", albedo, box, sky,
                    other, worst);
        expect(other == 0u, "furnace: every path returns albedo x sky or the sky");
        expect(albedo == 1.f || (box > 200u && sky > 200u), "furnace: box and sky both visible");
        if (albedo != 1.f) {
            continue;
        }
        // Russian roulette from the first bounce: unbiased, not exact (albedo 1: every pixel's expectation is 1).
        s.rrStartBounce = 0;
        expect(ref.setup(s, w.world.camera, 16, 12), "reference setup");
        PtReferenceImage img;
        expect(ref.render(w.world.cpu, 256, 11, kThreads, img), "render");
        f64 sum = 0.0;
        f64 var = 0.0;
        u32 count = 0;
        f64 worstZ = 0.0;
        for (usize p = 0; p < 16u * 12u; ++p) {
            if (img.variance[p * 3u] == 0.0) {
                continue; // sky (no roulette on a camera ray that escapes)
            }
            sum += img.mean[p * 3u] - 1.0;
            var += img.variance[p * 3u];
            worstZ = std::max(worstZ, std::fabs(img.mean[p * 3u] - 1.0) / std::sqrt(img.variance[p * 3u]));
            ++count;
        }
        const f64 z = count > 0u ? sum / std::sqrt(var) : 0.0;
        std::printf("furnace + RR from bounce 0 (albedo 1): %u noisy pixels, image z %+.2f, worst pixel %.2f sigma\n", count, z, worstZ);
        expect(count > 0u && std::fabs(z) < 5.0, "furnace with Russian roulette: mean == 1 (5 sigma)");
    }
}

void testAnalytic() {
    CpuWorld w;
    expect(makeWorld(w, pt_test::SceneKind::Analytic, 1, 1), "analytic world");
    for (const PtStrategy strategy : {PtStrategy::Mis, PtStrategy::NeeOnly, PtStrategy::BsdfOnly}) {
        PtSettings s = pt_test::baseSettings(w.world);
        s.strategy = strategy;
        s.maxBounces = 3;
        PtReference ref;
        expect(ref.setup(s, w.world.camera, 1, 1), "reference setup");
        PtReferenceImage img;
        expect(ref.render(w.world.cpu, kAnalyticSamples, 3, 1, img), "render");
        const f64 se = std::sqrt(img.variance[0]);
        const f64 z = (img.mean[0] - w.world.analytic) / se;
        std::printf("analytic (%s): L %.6f +- %.6f vs rho Le F %.6f (z %+.2f, rel %+.3f %%)\n",
                    strategy == PtStrategy::Mis ? "MIS" : (strategy == PtStrategy::NeeOnly ? "NEE" : "BSDF"), img.mean[0], se,
                    w.world.analytic, z, 100.0 * (img.mean[0] / w.world.analytic - 1.0));
        expect(std::fabs(z) < 4.0 && std::fabs(img.mean[0] / w.world.analytic - 1.0) < 0.01, "analytic irradiance (4 sigma, 1 %)");
    }
}

struct Compare {
    u32 tests = 0;
    u32 within = 0;
    f64 worst = 0.0;
    f64 imageZ = 0.0;
};

Compare compareImages(const PtReferenceImage& a, const PtReferenceImage& b, f64 k) {
    Compare c{};
    f64 diff[3] = {0.0, 0.0, 0.0};
    f64 var[3] = {0.0, 0.0, 0.0};
    for (usize i = 0; i < a.mean.size(); ++i) {
        const f64 v = a.variance[i] + b.variance[i];
        const f64 d = a.mean[i] - b.mean[i];
        diff[i % 3u] += d;
        var[i % 3u] += v;
        if (v == 0.0) {
            ++c.tests;
            c.within += std::fabs(d) < 1e-9 ? 1u : 0u;
            continue;
        }
        const f64 z = std::fabs(d) / std::sqrt(v);
        ++c.tests;
        c.within += z <= k ? 1u : 0u;
        c.worst = std::max(c.worst, z);
    }
    // Per channel (the channels of one pixel share their paths; pixels are independent).
    for (u32 ch = 0; ch < 3u; ++ch) {
        const f64 z = var[ch] > 0.0 ? diff[ch] / std::sqrt(var[ch]) : 0.0;
        c.imageZ = std::fabs(z) > std::fabs(c.imageZ) ? z : c.imageZ;
    }
    return c;
}

f64 meanVariance(const PtReferenceImage& a) {
    f64 v = 0.0;
    for (const f64 x : a.variance) {
        v += x;
    }
    return v / static_cast<f64>(a.variance.size());
}

void testStrategies() {
    CpuWorld w;
    expect(makeWorld(w, pt_test::SceneKind::Converge, 24, 16), "converge world");
    std::printf("strategies: %zu light-tree emitters (%zu traced triangles mapped), %u spp per image\n", w.world.table.size(),
                w.world.emitterRefs.size(), kStrategySamples);
    auto render = [&](PtStrategy strategy, bool rr, u64 seed, PtReferenceImage& img) {
        PtSettings s = pt_test::baseSettings(w.world);
        s.strategy = strategy;
        s.russianRoulette = rr;
        PtReference ref;
        expect(ref.setup(s, w.world.camera, 24, 16), "reference setup");
        expect(ref.render(w.world.cpu, kStrategySamples, seed, kThreads, img), "render");
    };
    PtReferenceImage mis, nee, misRr;
    render(PtStrategy::Mis, false, 101, mis);
    render(PtStrategy::NeeOnly, false, 202, nee);
    render(PtStrategy::Mis, true, 303, misRr);
    const Compare a = compareImages(mis, nee, 4.0);
    const Compare r = compareImages(mis, misRr, 4.0);
    std::printf("strategies: MIS vs NEE-only: %u / %u channel tests within 4 sigma (worst %.2f), image z %+.2f\n", a.within, a.tests,
                a.worst, a.imageZ);
    std::printf("strategies: RR on vs off:   %u / %u within 4 sigma (worst %.2f), image z %+.2f\n", r.within, r.tests, r.worst, r.imageZ);
    expect(a.within >= a.tests * 99u / 100u && std::fabs(a.imageZ) < 4.0, "MIS == NEE-only");
    expect(r.within >= r.tests * 99u / 100u && std::fabs(r.imageZ) < 4.0, "Russian roulette unbiased");
    // BSDF-only cannot see the point light: compare with MIS on the table with the point light dark.
    const u32 point = static_cast<u32>(w.world.table.size()) - 1u;
    restir::RestirLight saved = w.world.table[point];
    w.world.table[point].radiance[0] = w.world.table[point].radiance[1] = w.world.table[point].radiance[2] = 0.f;
    w.world.cpu.setLights(w.world.tree.emitters().data(), w.world.table.data(), static_cast<u32>(w.world.table.size()),
                          w.world.emitterMap.data(), static_cast<u32>(w.world.emitterMap.size()), w.scene.instanceHighWater());
    PtReferenceImage misDark, bsdf;
    render(PtStrategy::Mis, false, 404, misDark);
    render(PtStrategy::BsdfOnly, false, 505, bsdf);
    w.world.table[point] = saved;
    const Compare b = compareImages(misDark, bsdf, 4.0);
    std::printf("strategies: MIS vs BSDF-only (point light dark): %u / %u within 4 sigma (worst %.2f), image z %+.2f\n", b.within, b.tests,
                b.worst, b.imageZ);
    // BSDF-only is heavy-tailed (rare BSDF hits of the small panel after a glossy bounce): its per-pixel sample variance
    // underestimates the spread at low sample counts, so this comparison allows 2 % outliers.
    expect(b.within >= b.tests * 98u / 100u && std::fabs(b.imageZ) < 4.0, "MIS == BSDF-only");
    const f64 vMis = meanVariance(misDark);
    const f64 vNee = meanVariance(nee);
    const f64 vBsdf = meanVariance(bsdf);
    std::printf("strategies: mean variance of the mean: MIS %.3e (dark point light), BSDF-only %.3e (x%.1f), MIS (lit) %.3e, NEE-only "
                "%.3e\n",
                vMis, vBsdf, vBsdf / vMis, meanVariance(mis), vNee);
    expect(vMis < vBsdf, "MIS less noisy than BSDF-only");
}

void testDeterminism() {
    CpuWorld w;
    expect(makeWorld(w, pt_test::SceneKind::Converge, 12, 8), "converge world");
    PtSettings s = pt_test::baseSettings(w.world);
    PtReference ref;
    expect(ref.setup(s, w.world.camera, 12, 8), "reference setup");
    PtReferenceImage a, b, c;
    expect(ref.render(w.world.cpu, 32, 9, 1, a) && ref.render(w.world.cpu, 32, 9, 4, b) && ref.render(w.world.cpu, 32, 9, 3, c), "renders");
    const bool same = std::memcmp(a.mean.data(), b.mean.data(), a.mean.size() * 8u) == 0 &&
                      std::memcmp(a.mean.data(), c.mean.data(), a.mean.size() * 8u) == 0 &&
                      std::memcmp(a.variance.data(), b.variance.data(), a.variance.size() * 8u) == 0;
    PtReferenceImage d;
    expect(ref.render(w.world.cpu, 32, 10, 4, d), "render");
    const bool differs = std::memcmp(a.mean.data(), d.mean.data(), a.mean.size() * 8u) != 0;
    std::printf("determinism: 1 / 3 / 4 threads bit-identical: %s; another seed differs: %s\n", same ? "yes" : "no", differs ? "yes" : "no");
    expect(same, "reference render independent of the thread count");
    expect(differs, "the seed changes the samples");
}

// --- API -----------------------------------------------------------------------------------------
void testApi() {
    // Settings.
    PtSettings bad{};
    bad.maxBounces = 1000;
    bad.samplesPerFrame = 0;
    bad.minRoughness = 0.f;
    bad.cullMask = 0x80u;
    bad.sky[1] = -3.f;
    const PtSettings s = ptSanitize(bad);
    expect(s.maxBounces == kPtMaxBounces && s.samplesPerFrame == 1u && s.minRoughness >= 1e-3f && s.cullMask == 0x3u && s.sky[1] == 0.f,
           "ptSanitize clamps");
    // Frame constants.
    pt_test::World w{};
    PtCamera cam{};
    const f64 eye[3] = {0.0, 1.0, 3.0};
    const f64 at[3] = {0.0, 0.0, 0.0};
    pt_test::lookAt(cam, eye, at, 1.0, 1.5, 0.1, 50.0);
    PtFrameParams p{};
    p.width = 30;
    p.height = 20;
    p.lightCount = 3;
    p.lightTree = true;
    p.emitterMap = true;
    p.emitterMapSlots = 7;
    p.sampleBase = 64;
    struct Expect {
        PtStrategy strategy;
        u32 flags;
    };
    for (const Expect e : {Expect{PtStrategy::Mis, kPtFlagNee | kPtFlagEmitterHits}, Expect{PtStrategy::NeeOnly, kPtFlagNee},
                           Expect{PtStrategy::BsdfOnly, kPtFlagEmitterHits}}) {
        PtSettings ss{};
        ss.strategy = e.strategy;
        PtFrameConstants c{};
        expect(buildPtFrameConstants(ss, cam, p, c), "frame constants");
        expect((c.flags & (kPtFlagNee | kPtFlagEmitterHits)) == e.flags, "strategy flags");
        expect((c.flags & kPtFlagAccumulate) != 0u && (c.flags & kPtFlagRussianRoulette) != 0u && c.sampleBase == 64u &&
                   c.emitterMapSlots == 7u && c.width == 30u,
               "frame constant fields");
    }
    {
        PtFrameParams q = p;
        q.lightTree = false;
        PtFrameConstants c{};
        expect(buildPtFrameConstants(PtSettings{}, cam, q, c) && (c.flags & (kPtFlagNee | kPtFlagEmitterHits)) == 0u && c.lightCount == 0u,
               "no light tree: no NEE, no emitter hits");
        PtCamera singular{};
        expect(!buildPtFrameConstants(PtSettings{}, singular, p, c), "singular camera rejected");
    }
    // Emitter map.
    {
        const u32 tris[4] = {2u, 8u, 0u, 5u};
        const PtEmitterRef refs[3] = {{1u, 3u, 10u}, {3u, 4u, 11u}, {1u, 0u, 12u}};
        std::vector<u32> map;
        expect(buildPtEmitterMap(tris, 4u, refs, 3u, map), "emitter map built");
        expect(map.size() == 4u + 8u + 5u, "emitter map words: slots + mapped instances' triangles");
        expect(ptEmitterLookup(map.data(), static_cast<u32>(map.size()), 4u, 1u, 3u) == 10u &&
                   ptEmitterLookup(map.data(), static_cast<u32>(map.size()), 4u, 3u, 4u) == 11u &&
                   ptEmitterLookup(map.data(), static_cast<u32>(map.size()), 4u, 1u, 0u) == 12u &&
                   ptEmitterLookup(map.data(), static_cast<u32>(map.size()), 4u, 1u, 1u) == kPtInvalid &&
                   ptEmitterLookup(map.data(), static_cast<u32>(map.size()), 4u, 0u, 0u) == kPtInvalid &&
                   ptEmitterLookup(map.data(), static_cast<u32>(map.size()), 4u, 9u, 0u) == kPtInvalid,
               "emitter lookup");
        const PtEmitterRef dup[2] = {{1u, 3u, 1u}, {1u, 3u, 2u}};
        const PtEmitterRef range[1] = {{2u, 0u, 1u}};
        expect(!buildPtEmitterMap(tris, 4u, dup, 2u, map) && map.empty(), "duplicate triangle rejected");
        expect(!buildPtEmitterMap(tris, 4u, range, 1u, map), "triangle out of range rejected");
    }
    // Scene-built map: the Converge scene's panel triangles all map to their emitters.
    {
        CpuWorld cw;
        expect(makeWorld(cw, pt_test::SceneKind::Converge, 8, 8), "converge world");
        u32 mapped = 0;
        for (const PtEmitterRef& r : cw.world.emitterRefs) {
            mapped += ptEmitterLookup(cw.world.emitterMap.data(), static_cast<u32>(cw.world.emitterMap.size()), cw.scene.instanceHighWater(),
                                      r.instance, r.triangle) == r.emitter
                          ? 1u
                          : 0u;
        }
        std::printf("api: Converge scene: %zu emitters, %u / %zu panel triangles mapped, map %zu words\n", cw.world.table.size(), mapped,
                    cw.world.emitterRefs.size(), cw.world.emitterMap.size());
        expect(mapped == 8u && cw.world.emitterRefs.size() == 8u, "every panel triangle mapped to its emitter");
    }
    // SBT layouts.
    struct Props {
        u32 handle, handleAlign, baseAlign, maxStride;
        const char* name;
    };
    for (const Props pr : {Props{32, 32, 64, 4096, "NVIDIA-like"}, Props{32, 32, 64, 4096, "AMD-like"}, Props{32, 32, 32, 4096, "llvmpipe-like"},
                           Props{16, 16, 256, 4096, "wide base"}, Props{48, 16, 64, 4096, "odd handle"}}) {
        const PtSbtLayout l = computePtSbtLayout(pr.handle, pr.handleAlign, pr.baseAlign, pr.maxStride);
        const bool ok = l.valid && l.raygen.stride == l.raygen.size && l.raygen.offset % pr.baseAlign == 0u && l.miss.offset % pr.baseAlign == 0u &&
                        l.hit.offset % pr.baseAlign == 0u && l.miss.stride % pr.handleAlign == 0u && l.hit.stride % pr.handleAlign == 0u &&
                        l.miss.stride >= pr.handle && l.raygen.offset + l.raygen.size <= l.miss.offset &&
                        l.miss.offset + l.miss.size <= l.hit.offset && l.hit.offset + l.hit.size <= l.bytes &&
                        l.miss.size >= kPtMissRecords * l.miss.stride && l.hit.size >= kPtHitRecords * l.hit.stride;
        std::printf("api: SBT %-13s raygen %llu/%llu, miss @%llu stride %llu, hit @%llu stride %llu, %llu bytes\n", pr.name,
                    static_cast<unsigned long long>(l.raygen.offset), static_cast<unsigned long long>(l.raygen.size),
                    static_cast<unsigned long long>(l.miss.offset), static_cast<unsigned long long>(l.miss.stride),
                    static_cast<unsigned long long>(l.hit.offset), static_cast<unsigned long long>(l.hit.stride),
                    static_cast<unsigned long long>(l.bytes));
        expect(ok, "SBT regions aligned, disjoint, raygen size == stride");
    }
    expect(!computePtSbtLayout(32, 24, 64).valid && !computePtSbtLayout(32, 32, 48).valid && !computePtSbtLayout(0, 32, 64).valid &&
               !computePtSbtLayout(64, 32, 64, 32).valid,
           "invalid SBT properties rejected");
    // Capability gate on synthetic caps.
    {
        RendererCaps caps{};
        expect(!evaluatePtCapabilities(caps).rayQuery, "no device caps: nothing");
        caps.valid = true;
        caps.meetsT0 = true;
        caps.tier = RenderTier::T2;
        caps.tierCap = RenderTier::T3;
        caps.bufferDeviceAddress = true;
        caps.accelerationStructure = true;
        caps.rayQuery = true;
        PtCapabilities pc = evaluatePtCapabilities(caps);
        expect(pc.rayQuery && !pc.rayTracingPipeline, "T2 device: ray query only");
        caps.rayTracingPipeline = true;
        pc = evaluatePtCapabilities(caps);
        expect(pc.rayQuery && pc.rayTracingPipeline, "RT pipeline enabled (cap >= T3): T3 variant");
        std::printf("api: T2 without RT pipeline -> rayQuery; + VK_KHR_ray_tracing_pipeline enabled -> rt_pipeline\n");
        caps.rayTracingPipeline = false;
        caps.tierCap = RenderTier::T2;
        pc = evaluatePtCapabilities(caps);
        expect(pc.rayQuery && !pc.rayTracingPipeline && std::strstr(pc.pipelineReason, "T3") != nullptr, "capped below T3: reason");
        caps.rayQuery = false;
        expect(!evaluatePtCapabilities(caps).rayQuery, "no ray query: nothing");
    }
    // No device.
    PathTracerGpu gpu;
    expect(!gpu.init(PathTracerGpuDesc{}) && !gpu.valid(), "PathTracerGpu without a device fails");
    std::printf("api: PathTracerGpu without a device: %s\n", gpu.reason());
    PtCapabilities none = queryPtCapabilities(nullptr);
    expect(!none.rayQuery && !none.rayTracingPipeline, "queryPtCapabilities(nullptr)");
}

// --- reconstruction (mock plugins) -----------------------------------------------------------------
class MockDenoiser final : public denoise::IDenoiser {
public:
    const denoise::DenoiserCaps& caps() const override { return s_caps; }
    bool configure(const denoise::DenoiserSettings& settings) override {
        m_settings = settings;
        return true;
    }
    const denoise::DenoiserSettings& settings() const override { return m_settings; }
    void reset() override {}
    bool beginFrame(u64, const denoise::DenoiseFrameDesc&) override { return true; }
    denoise::DenoiseGraphRefs importInto(rg::Graph&) override { return {}; }
    void addPasses(rg::Graph&, const denoise::DenoiseGraphRefs&, const denoise::DenoiseGraphInputs&) override {}
    u32 collectRetired(u64) override { return 0; }
    static denoise::DenoiserCaps s_caps;
    static u32 s_created;

private:
    denoise::DenoiserSettings m_settings{};
};
denoise::DenoiserCaps MockDenoiser::s_caps{};
u32 MockDenoiser::s_created = 0;

std::unique_ptr<denoise::IDenoiser> makeMockDenoiser(const denoise::DenoiserCreateInfo&) {
    ++MockDenoiser::s_created;
    return std::make_unique<MockDenoiser>();
}
std::unique_ptr<upscale::IUpscaler> makeNothing() { return nullptr; }

void testReconstruction() {
    // Nothing registered.
    upscale::UpscalerRegistry up;
    denoise::DenoiserRegistry den;
    PtReconstruction r = selectPtReconstruction(&up, &den, PtReconstructionRequest{});
    expect(r.kind == PtReconstructionKind::Accumulate, "empty registries: accumulate");
    std::printf("reconstruction: empty registries -> accumulate (%s)\n", r.reason);
    // In-tree SVGF only: RELAX not served -> SVGF fallback.
    den.register_builtin_denoisers();
    r = selectPtReconstruction(&up, &den, PtReconstructionRequest{});
    expect(r.kind == PtReconstructionKind::Denoiser && std::strcmp(r.backend, "svgf") == 0 && r.method == denoise::DenoiserMethod::Svgf &&
               r.fallback,
           "SVGF fallback");
    std::printf("reconstruction: svgf only -> %s %s (%s)\n", r.backend, denoise::denoiser_method_name(r.method), r.reason);
    // Mock NRD: RELAX / REBLUR, native frame; RELAX needs no hit distance in this mock (REBLUR does).
    MockDenoiser::s_caps = denoise::DenoiserCaps{};
    MockDenoiser::s_caps.name = "nrd";
    MockDenoiser::s_caps.display_name = "NRD (mock)";
    MockDenoiser::s_caps.methods = denoise::denoiser_method_bit(denoise::DenoiserMethod::Relax) | denoise::denoiser_method_bit(denoise::DenoiserMethod::Reblur);
    MockDenoiser::s_caps.signals = denoise::denoise_signal_bit(denoise::DenoiseSignal::Gi) | denoise::denoise_signal_bit(denoise::DenoiseSignal::Reflection);
    MockDenoiser::s_caps.needs_native_frame = true;
    MockDenoiser::s_caps.hit_distance_methods = denoise::denoiser_method_bit(denoise::DenoiserMethod::Reblur);
    expect(den.register_backend(MockDenoiser::s_caps, &makeMockDenoiser), "mock NRD registered");
    r = selectPtReconstruction(&up, &den, PtReconstructionRequest{});
    expect(r.kind == PtReconstructionKind::Denoiser && std::strcmp(r.backend, "nrd") == 0 && r.method == denoise::DenoiserMethod::Relax,
           "NRD RELAX mock chosen");
    std::printf("reconstruction: + nrd mock -> %s %s\n", r.backend, denoise::denoiser_method_name(r.method));
    PtReconstructionRequest reblur{};
    reblur.denoiserMethod = denoise::DenoiserMethod::Reblur;
    r = selectPtReconstruction(&up, &den, reblur);
    expect(r.kind == PtReconstructionKind::Denoiser && std::strcmp(r.backend, "svgf") == 0,
           "REBLUR without the hit-distance packing: in-tree fallback");
    std::printf("reconstruction: REBLUR requested without hit distance -> %s (%s)\n", r.backend, r.reason);
    PtReconstructionRequest noNative{};
    noNative.haveNativeFrame = false;
    r = selectPtReconstruction(&up, &den, noNative);
    expect(r.kind == PtReconstructionKind::Denoiser && std::strcmp(r.backend, "svgf") == 0, "no native frame: in-tree fallback");
    // Create through the registry and drive the interface.
    MockDenoiser::s_created = 0;
    r = selectPtReconstruction(&up, &den, PtReconstructionRequest{});
    std::unique_ptr<denoise::IDenoiser> dn = den.create(r.backend, denoise::DenoiserCreateInfo{});
    expect(dn != nullptr && MockDenoiser::s_created == 1u, "IDenoiser created through the registry factory");
    if (dn != nullptr) {
        denoise::DenoiserSettings ds{};
        ds.method = r.method;
        ds.signal = denoise::DenoiseSignal::Gi;
        expect(dn->configure(ds) && dn->settings().method == denoise::DenoiserMethod::Relax, "configure");
    }
    // Mock DLSS RR (temporal, Vulkan): first choice.
    upscale::UpscalerCaps rr{};
    rr.name = kPtRayReconstructionBackend;
    rr.display_name = "DLSS RR (mock)";
    rr.kind = upscale::UpscalerKind::Temporal;
    rr.temporal = true;
    rr.apis = upscale::api_bit(upscale::UpscalerApi::Vulkan);
    rr.quality_modes = upscale::kAllQualityModes;
    rr.stub = true;
    expect(up.register_backend(rr, &makeNothing), "mock RR registered");
    r = selectPtReconstruction(&up, &den, PtReconstructionRequest{});
    expect(r.kind == PtReconstructionKind::Denoiser, "stub RR skipped");
    expect(up.unregister_backend(kPtRayReconstructionBackend), "unregister");
    rr.stub = false;
    expect(up.register_backend(rr, &makeNothing), "mock RR registered");
    r = selectPtReconstruction(&up, &den, PtReconstructionRequest{});
    expect(r.kind == PtReconstructionKind::RayReconstruction && std::strcmp(r.backend, kPtRayReconstructionBackend) == 0, "RR first");
    std::printf("reconstruction: + dlss_rr mock -> %s (%s)\n", r.backend, r.reason);
    PtReconstructionRequest noRr{};
    noRr.allowRayReconstruction = false;
    r = selectPtReconstruction(&up, &den, noRr);
    expect(r.kind == PtReconstructionKind::Denoiser && std::strcmp(r.backend, "nrd") == 0, "RR disallowed: NRD");
    PtReconstructionRequest none{};
    none.allowRayReconstruction = false;
    none.allowDenoiser = false;
    expect(selectPtReconstruction(&up, &den, none).kind == PtReconstructionKind::Accumulate, "nothing allowed: accumulate");
    expect(selectPtReconstruction(nullptr, nullptr, PtReconstructionRequest{}).kind == PtReconstructionKind::Accumulate, "null registries");
}

// --- zero allocations ----------------------------------------------------------------------------------
void testZeroAlloc() {
    CpuWorld w;
    expect(makeWorld(w, pt_test::SceneKind::Converge, 12, 8), "converge world");
    PtSettings s = pt_test::baseSettings(w.world);
    PtReference ref;
    expect(ref.setup(s, w.world.camera, 12, 8), "reference setup");
    PtPixelStats st{};
    ref.renderPixel(w.world.cpu, 5, 4, 0, 4, 1, st);
    PtFrameParams p{};
    p.width = 12;
    p.height = 8;
    p.lightTree = true;
    p.lightCount = static_cast<u32>(w.world.table.size());
    PtFrameConstants c{};
    t_allocations = 0;
    t_count = true;
    u32 found = 0;
    for (u32 y = 0; y < 8u; ++y) {
        for (u32 x = 0; x < 12u; ++x) {
            ref.renderPixel(w.world.cpu, x, y, 0, 16, 2, st);
        }
    }
    for (u32 i = 0; i < 64u; ++i) {
        p.frameIndex = i;
        buildPtFrameConstants(s, w.world.camera, p, c);
        found += ptEmitterLookup(w.world.emitterMap.data(), static_cast<u32>(w.world.emitterMap.size()), w.scene.instanceHighWater(), 4u, i % 8u) !=
                         kPtInvalid
                     ? 1u
                     : 0u;
    }
    t_count = false;
    std::printf("zero_alloc: %u reference paths, 64 frame-constant builds + emitter lookups (%u hits): %llu operator new\n", st.samples, found,
                t_allocations);
    expect(t_allocations == 0u, "steady-state reference pixels / frame constants / lookups allocate nothing");
    expect(found == 64u, "panel lookups");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "bsdf") {
        testBsdf();
    }
    if (all || suite == "furnace") {
        testFurnace();
    }
    if (all || suite == "analytic") {
        testAnalytic();
    }
    if (all || suite == "strategies") {
        testStrategies();
    }
    if (all || suite == "determinism") {
        testDeterminism();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (all || suite == "reconstruction") {
        testReconstruction();
    }
    if (all || suite == "zero_alloc") {
        testZeroAlloc();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
