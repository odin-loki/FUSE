// FUSE Relight RL-5.7 CPU gates: post and upscale wiring (see ../include/fuse/relight/render/post/post_pipeline.hpp).
//
//   composite       remodulated channels == the path tracer's radiance with the denoiser off (the CPU reference path
//                   tracer's channels of the Cornell scene at 1 spp; float round trip of the albedo division only),
//                   through compositeRadiance and through PostPipeline::process on a PathTracerGpu-layout buffer
//   mip_bias        log2(render / output) (+ rtx.upscalingMipBias; rtx.nativeMipBias at 1:1) for every preset
//   options         rtx.upscalerType / presets / relight.post.* -> PostConfig
//   upscaler_switch every in-tree backend (none, native_taau, fsr1, nis, cas) runs at its render extent and produces
//                   finite, lit output; fsr3 without a device and the plugin ids (dlss, dlss_rr, xess) fall back with a
//                   reason; a runtime switch between backends keeps working
//   nan_guard       a non-finite input pixel is counted and never reaches the 8-bit output as garbage
//   ltm             local tone map node: uniform stays uniform, dynamic range compressed, Look placement validated
//   determinism     two pipelines, same frames -> identical bytes (every in-tree backend)
//   zero_alloc      steady-state frames of every in-tree backend make no heap allocation
#include "pt_test_scenes.hpp"

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/post/composite.hpp>
#include <fuse/relight/render/post/local_tonemap.hpp>
#include <fuse/relight/render/post/post_config.hpp>
#include <fuse/relight/render/post/post_pipeline.hpp>

#include "pt_reference_kernels.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// --- allocation counter (every thread) ------------------------------------------------------------------------------
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

namespace opt = fuse::relight::options;
namespace post = fuse::relight::render::post;
namespace pt = fuse::relight::render::pathtrace;
namespace math = fuse::math;
using fuse::u32;
using fuse::u64;
using fuse::u8;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

constexpr u32 kW = 128u, kH = 96u;

/// A PathTracerGpu-layout output buffer (kPtOutSections sections of n * 16 bytes) from the CPU reference's channels.
struct PtBuffer {
    std::vector<float> data;
    u64 stride = 0;
    u32 w = 0, h = 0;
    float* section(u32 s) { return data.data() + s * (stride / sizeof(float)); }
};

PtBuffer bufferFromReference(const pt::PtReferenceImage& img) {
    PtBuffer b;
    b.w = img.width();
    b.h = img.height();
    const std::size_t n = std::size_t(b.w) * b.h;
    b.stride = n * 16u;
    b.data.assign(pt::kPtOutSections * n * 4u, 0.f);
    for (u32 y = 0; y < b.h; ++y) {
        for (u32 x = 0; x < b.w; ++x) {
            const std::size_t i = std::size_t(y) * b.w + x;
            const auto& p = img.pixel(x, y);
            const double inv = 1.0 / std::max<u32>(p.samples, 1u);
            for (u32 c = 0; c < 3u; ++c) {
                b.section(pt::kPtOutRadiance)[i * 4u + c] = float(p.sum[c] * inv);
                b.section(pt::kPtOutAccum)[i * 4u + c] = float(p.sum[c]);
                b.section(pt::kPtOutEmissive)[i * 4u + c] = float(p.emissive[c] * inv);
                b.section(pt::kPtOutDiffuse)[i * 4u + c] = float(p.diffuse[c] * inv);
                b.section(pt::kPtOutSpecular)[i * 4u + c] = float(p.specular[c] * inv);
                b.section(pt::kPtOutAlbedoD)[i * 4u + c] = p.albedoD[c];
                b.section(pt::kPtOutAlbedoS)[i * 4u + c] = p.albedoS[c];
            }
            b.section(pt::kPtOutAccum)[i * 4u + 3u] = float(p.samples);
            b.section(pt::kPtOutMotion)[i * 2u] = p.motion[0];
            b.section(pt::kPtOutMotion)[i * 2u + 1u] = p.motion[1];
            b.section(pt::kPtOutDepth)[i] = p.depth;
        }
    }
    return b;
}

bool referenceCornell(u32 w, u32 h, pt::PtReferenceImage& img) {
    pt::PtCompiledScene cs;
    std::string error;
    if (!cs.compile(pt_test::cornell(), {}, &error)) {
        check(false, "compile cornell: " + error);
        return false;
    }
    pt::PtSettings st;
    st.maxBounces = 4;
    img.resize(w, h);
    const bool ok = pt::renderReference(cs, st, w, h, 0x5EEDu, 0u, 1u, img);
    check(ok, "renderReference");
    return ok;
}

double relErr(float a, float b) { return std::fabs(double(a) - double(b)) / std::max(std::fabs(double(b)), 1e-3); }

void suiteComposite() {
    pt::PtReferenceImage img;
    if (!referenceCornell(64u, 48u, img)) {
        return;
    }
    PtBuffer b = bufferFromReference(img);
    const std::size_t n = std::size_t(b.w) * b.h;
    std::vector<math::Vec3> out(n);
    post::CompositeInputs ci;
    ci.emissive = b.section(pt::kPtOutEmissive);
    ci.diffuse = b.section(pt::kPtOutDiffuse);
    ci.specular = b.section(pt::kPtOutSpecular);
    ci.albedoD = b.section(pt::kPtOutAlbedoD);
    ci.albedoS = b.section(pt::kPtOutAlbedoS);
    ci.count = n;
    check(post::compositeRadiance(ci, out.data()), "compositeRadiance");
    double worst = 0.0, sum = 0.0;
    u32 lit = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float* r = b.section(pt::kPtOutRadiance) + i * 4u;
        const float c[3] = {out[i].x, out[i].y, out[i].z};
        for (u32 k = 0; k < 3u; ++k) {
            worst = std::max(worst, relErr(c[k], r[k]));
            sum += r[k];
        }
        lit += r[0] + r[1] + r[2] > 0.f ? 1u : 0u;
    }
    std::printf("composite: %zu pixels (%u lit, mean %.4f): max relative error to the radiance %.3g\n", n, lit,
                sum / double(3u * n), worst);
    check(lit > n / 2u, "cornell mostly lit");
    check(worst <= 2e-6, "composite == undemodulated radiance (float round trip), max rel " + std::to_string(worst));
    // Denoised channels given as the raw channels: identical.
    std::vector<math::Vec3> out2(n);
    ci.denoisedDiffuse = ci.diffuse;
    ci.denoisedSpecular = ci.specular;
    post::compositeRadiance(ci, out2.data());
    check(std::memcmp(out.data(), out2.data(), n * sizeof(math::Vec3)) == 0, "denoised == raw channels -> identical");
    // Through the pipeline (PathTracerGpu layout, sections read in place).
    post::PostPipeline p;
    post::PostConfig cfg;
    cfg.upscaler = "none";
    cfg.toneMapping = post::ToneMappingMode::Global;
    check(p.configure(cfg, b.w, b.h), "configure none");
    std::vector<u8> px(n * 4u);
    post::PtPostInput in;
    in.outputs = b.data.data();
    in.stride = b.stride;
    in.renderWidth = b.w;
    in.renderHeight = b.h;
    check(p.process(in, {px.data(), b.w, b.h, false}), std::string("process: ") + p.error());
    check(std::memcmp(p.composite().data(), out.data(), n * sizeof(math::Vec3)) == 0,
          "pipeline composite == compositeRadiance");
    // Accumulated frames read the accumulated mean.
    in.accumulated = true;
    check(p.process(in, {px.data(), b.w, b.h, false}), "process accumulated");
    double worstAcc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const float* r = b.section(pt::kPtOutRadiance) + i * 4u;
        worstAcc = std::max({worstAcc, relErr(p.composite()[i].x, r[0]), relErr(p.composite()[i].y, r[1]),
                             relErr(p.composite()[i].z, r[2])});
    }
    check(worstAcc <= 1e-6, "accumulated input == accumulated mean");
}

void suiteMipBias() {
    check(post::textureMipBias(64u, 128u, 0.f, 0.f) == -1.f, "2x -> -1");
    check(std::fabs(post::textureMipBias(96u, 128u, 0.f, 0.f) - float(std::log2(0.75))) < 1e-7f, "0.75 -> log2 0.75");
    check(post::textureMipBias(128u, 128u, 0.25f, -0.5f) == 0.25f, "native -> rtx.nativeMipBias");
    check(post::textureMipBias(64u, 128u, 0.25f, -0.5f) == -1.5f, "upscaling -> log2 + rtx.upscalingMipBias");
    // Every preset through the pipeline: selection().mipBias == log2(render / output).
    const float scales[] = {0.33f, 0.5f, 0.58f, 0.66f, 0.667f, 0.75f, 1.f};
    for (float s : scales) {
        post::PostPipeline p;
        post::PostConfig c;
        c.upscaler = "native_taau";
        c.resolutionScale = s;
        c.toneMapping = post::ToneMappingMode::Global;
        if (!p.configure(c, 1920u, 1080u)) {
            check(false, "configure 1080p");
            continue;
        }
        const auto& sel = p.selection();
        const float want = sel.renderWidth == 1920u ? 0.f : float(std::log2(double(sel.renderWidth) / 1920.0));
        std::printf("mip_bias: scale %.3f -> render %ux%u, bias %.4f\n", double(s), sel.renderWidth, sel.renderHeight,
                    double(sel.mipBias));
        check(sel.mipBias == want, "pipeline mip bias == log2(render / output) at scale " + std::to_string(s));
        check(sel.renderWidth == u32(std::lround(1920.0 * s)), "render width = round(output x scale)");
    }
}

bool loadRtxConf(const char* name, const char* text) {
    opt::OptionSystem::shutdown();
    {
        FILE* f = std::fopen(name, "wb");
        if (f == nullptr) {
            check(false, std::string("write ") + name);
            return false;
        }
        std::fputs(text, f);
        std::fclose(f);
    }
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, name);
    opt::OptionSystemDesc desc;
    desc.loadEnvironmentVariables = false;
    (void)opt::OptionSystem::initialize(desc);
    opt::OptionManager::applyPendingValues(nullptr, false);
    return true;
}

void suiteOptions() {
    check(std::string(post::upscalerNameForRemixType(0)) == "none" &&
              std::string(post::upscalerNameForRemixType(1)) == "dlss" &&
              std::string(post::upscalerNameForRemixType(2)) == "nis" &&
              std::string(post::upscalerNameForRemixType(3)) == "native_taau" &&
              std::string(post::upscalerNameForRemixType(4)) == "xess",
          "rtx.upscalerType mapping");
    post::PostConfig c = post::PostConfig::fromOptions(1080u);
    check(c.upscaler == "dlss" && std::fabs(c.resolutionScale - 0.667f) < 1e-6f,
          "defaults: DLSS, Auto at 1080p = MaxQuality");
    check(c.toneMapping == post::ToneMappingMode::Local, "default tone mapping: local");
    check(post::dlssPresetScale(5, 2160u) == 0.5f && post::dlssPresetScale(5, 1440u) == 0.58f, "DLSS Auto");
    // rtx.conf files, through the option system's rtx.conf layer.
    if (loadRtxConf("rl_post_a.rtx.conf", "rtx.upscalerType = 2\nrtx.nisPreset = 0\n")) {
        c = post::PostConfig::fromOptions(1080u);
        std::printf("options: rtx.conf A -> %s scale %.3f\n", c.upscaler.c_str(), double(c.resolutionScale));
        check(c.upscaler == "nis" && c.resolutionScale == 0.5f, "rtx.conf: NIS Performance");
    }
    if (loadRtxConf("rl_post_b.rtx.conf", "rtx.upscalerType = 3\nrtx.taauPreset = 5\nrtx.resolutionScale = 0.6\n"
                                          "rtx.tonemappingMode = 0\nrtx.upscalingMipBias = -0.5\n"
                                          "rtx.localtonemap.mip = 5\n")) {
        c = post::PostConfig::fromOptions(1080u);
        std::printf("options: rtx.conf B -> %s scale %.3f tm %d mip bias %.2f ltm mip %u\n", c.upscaler.c_str(),
                    double(c.resolutionScale), int(c.toneMapping), double(c.upscalingMipBias), c.localToneMap.mip);
        check(c.upscaler == "native_taau" && c.resolutionScale == 0.6f, "rtx.conf: TAA-U custom scale");
        check(c.toneMapping == post::ToneMappingMode::Global && c.upscalingMipBias == -0.5f && c.localToneMap.mip == 5u,
              "rtx.conf: global TM / mip bias / localtonemap");
    }
    if (loadRtxConf("rl_post_c.rtx.conf", "rtx.upscalerType = 3\nrelight.post.upscaler = FSR1\n"
                                          "relight.post.resolutionScale = 0.5\n")) {
        c = post::PostConfig::fromOptions(1080u);
        check(c.upscaler == "fsr1" && c.resolutionScale == 0.5f, "relight.post.upscaler / resolutionScale override");
    }
    loadRtxConf("rl_post_d.rtx.conf", "");
    c = post::PostConfig::fromOptions(1080u);
    check(c.upscaler == "dlss", "empty rtx.conf: defaults again");
}

/// Synthetic HDR frame `f` at w x h: a sky gradient, a bright moving disc (emitter), a dark band; depth and motion.
void syntheticFrame(u32 f, u32 w, u32 h, std::vector<math::Vec3>& c, std::vector<float>& d, std::vector<math::Vec2>& m) {
    c.resize(std::size_t(w) * h);
    d.resize(c.size());
    m.resize(c.size());
    const float cx = 0.3f + 0.02f * float(f), cy = 0.5f;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const std::size_t i = std::size_t(y) * w + x;
            const float u = (float(x) + 0.5f) / float(w), v = (float(y) + 0.5f) / float(h);
            math::Vec3 col(0.2f + 0.6f * v, 0.3f + 0.3f * u, 0.8f);
            float depth = 0.f;
            math::Vec2 mv(0.f, 0.f);
            const float dx = (u - cx) * float(w) / float(h), dy = v - cy;
            if (dx * dx + dy * dy < 0.04f) {
                col = math::Vec3(20.f, 16.f, 9.f);
                depth = 4.f;
                mv = math::Vec2(0.02f, 0.f);
            } else if (v > 0.75f) {
                col = math::Vec3(0.01f, 0.012f, 0.015f) * (1.f + float((x / 4u + y / 4u) % 2u));
                depth = 2.f + v;
            }
            c[i] = col;
            d[i] = depth;
            m[i] = mv;
        }
    }
}

struct RunResult {
    std::vector<u8> bytes;
    bool ok = false;
    u32 nonFinite = 0;
    u32 lit = 0;
};

RunResult runFrames(post::PostPipeline& p, u32 frames, bool nanPixel = false) {
    RunResult r;
    const auto& s = p.selection();
    std::vector<math::Vec3> c;
    std::vector<float> d;
    std::vector<math::Vec2> m;
    r.bytes.assign(std::size_t(s.displayWidth) * s.displayHeight * 4u, 0u);
    r.ok = true;
    for (u32 f = 0; f < frames; ++f) {
        syntheticFrame(f, s.renderWidth, s.renderHeight, c, d, m);
        if (nanPixel) {
            c[c.size() / 2u].x = std::nanf("");
            c[c.size() / 3u].y = INFINITY;
        }
        r.ok = r.ok && p.processHdr(c.data(), d.data(), m.data(), {r.bytes.data(), s.displayWidth, s.displayHeight, true});
        r.nonFinite += p.stats().nonFinite;
    }
    for (std::size_t i = 0; i < r.bytes.size(); i += 4u) {
        r.lit += (r.bytes[i] | r.bytes[i + 1u] | r.bytes[i + 2u]) > 8u ? 1u : 0u;
    }
    return r;
}

struct Case {
    const char* requested;
    const char* expected; ///< backend that must run
    bool fallback;
};
const Case kCases[] = {
    {"none", "none", false},           {"native_taau", "native_taau", false}, {"fsr1", "fsr1", false},
    {"nis", "nis", false},             {"cas", "cas", false},                 {"fsr3", "native_taau", true},
    {"dlss", "native_taau", true},     {"dlss_rr", "native_taau", true},      {"xess", "native_taau", true},
    {"bogus", "native_taau", true},
};

bool configureCase(post::PostPipeline& p, const char* requested, float scale = 0.5f,
                   post::ToneMappingMode tm = post::ToneMappingMode::Local) {
    post::PostConfig c;
    c.upscaler = requested;
    c.resolutionScale = scale;
    c.toneMapping = tm;
    return p.configure(c, kW, kH);
}

void suiteUpscalerSwitch() {
    for (const Case& k : kCases) {
        post::PostPipeline p;
        if (!configureCase(p, k.requested)) {
            check(false, std::string("configure ") + k.requested + ": " + p.error());
            continue;
        }
        const auto& s = p.selection();
        const RunResult r = runFrames(p, 4u);
        std::printf("switch: %-11s -> %-11s render %3ux%-3u mip %.3f status %s nonfinite %u lit %u/%u%s%s\n",
                    k.requested, s.backend.c_str(), s.renderWidth, s.renderHeight, double(s.mipBias),
                    fuse::renderer::upscale::upscale_status_name(p.stats().status), r.nonFinite, r.lit, kW * kH,
                    s.reason.empty() ? "" : "  reason: ", s.reason.c_str());
        check(r.ok, std::string(k.requested) + ": frames ran (" + p.error() + ")");
        check(s.backend == k.expected, std::string(k.requested) + ": backend " + s.backend);
        check(s.fallback == k.fallback, std::string(k.requested) + ": fallback flag");
        if (k.fallback) {
            check(s.reason.find(std::string(k.requested) + ":") == 0u, std::string(k.requested) + ": reason names it");
        }
        check(r.nonFinite == 0u, std::string(k.requested) + ": finite output");
        check(r.lit > kW * kH / 2u, std::string(k.requested) + ": lit output");
        const bool upscales = std::string(k.expected) != "none" && std::string(k.expected) != "cas";
        check(upscales ? s.renderWidth < kW : s.renderWidth == kW, std::string(k.requested) + ": render extent");
        for (const math::Vec3& v : p.displayHdr()) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
                check(false, std::string(k.requested) + ": non-finite display HDR");
                break;
            }
        }
    }
    // Runtime switch on one pipeline (re-configure between frames), global TM this time.
    post::PostPipeline p;
    for (const char* name : {"native_taau", "fsr1", "none", "nis", "native_taau"}) {
        check(configureCase(p, name, 0.66f, post::ToneMappingMode::Global), std::string("switch to ") + name);
        const RunResult r = runFrames(p, 2u);
        check(r.ok && r.nonFinite == 0u && r.lit > kW * kH / 2u, std::string("switched ") + name + " runs");
    }
}

void suiteNanGuard() {
    for (const char* name : {"none", "native_taau", "fsr1"}) {
        post::PostPipeline p;
        check(configureCase(p, name, 0.5f, post::ToneMappingMode::Global), "configure");
        const RunResult r = runFrames(p, 2u, true);
        std::printf("nan_guard: %s: %u non-finite values caught\n", name, r.nonFinite);
        check(r.ok, std::string("nan_guard ") + name + " ran");
    }
}

void suiteLtm() {
    namespace look = fuse::renderer::look;
    post::LocalToneMapper ltm;
    post::LocalToneMapConfig cfg;
    check(ltm.init(kW, kH, cfg), "ltm init");
    check(ltm.levels() == 4u, "mip 3 -> 4 levels");
    const std::size_t n = std::size_t(kW) * kH;
    std::vector<math::Vec3> in(n, math::Vec3(0.5f, 0.5f, 0.5f)), out(n);
    ltm.process(in.data(), out.data());
    float lo = 1e30f, hi = -1e30f;
    for (const auto& v : out) {
        lo = std::min(lo, v.x);
        hi = std::max(hi, v.x);
    }
    check(hi - lo <= 1e-5f * hi && std::isfinite(hi) && hi > 0.f, "uniform input -> uniform output");
    // Bright left half (x 64), dark right half: the local tone map compresses the ratio.
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            in[std::size_t(y) * kW + x] = x < kW / 2u ? math::Vec3(16.f, 16.f, 16.f) : math::Vec3(0.25f, 0.25f, 0.25f);
        }
    }
    ltm.process(in.data(), out.data());
    const float before = 16.f / 0.25f;
    const float after = out[std::size_t(kH / 2u) * kW + 8u].x / out[std::size_t(kH / 2u) * kW + kW - 8u].x;
    std::printf("ltm: bright / dark ratio %.1f -> %.2f\n", double(before), double(after));
    check(after < before * 0.5f && after > 1.f, "local tone map compresses the dynamic range, keeps the order");
    for (const auto& v : out) {
        if (!std::isfinite(v.x)) {
            check(false, "ltm finite");
            break;
        }
    }
    // Look placement: legal before a scene-referred chain, illegal without a tone map.
    look::LookEffectGraph g = look::LookEffectGraph::makeDefault();
    check(post::validateLocalToneMapPlacement(g), "placement before the default Look graph");
    look::LookEffectGraph bad;
    bad.push(look::LookEffect::Exposure);
    bad.push(look::LookEffect::OutputTransform);
    check(!post::validateLocalToneMapPlacement(bad), "no toneMap: rejected");
    check(post::kLocalToneMapStage == look::LookStage::PostUpscaleHdr, "stage");
}

void suiteDeterminism() {
    for (const char* name : {"none", "native_taau", "fsr1", "nis", "cas"}) {
        post::PostPipeline a, b;
        check(configureCase(a, name) && configureCase(b, name), "configure");
        const RunResult ra = runFrames(a, 3u), rb = runFrames(b, 3u);
        check(ra.bytes == rb.bytes, std::string("determinism ") + name);
    }
}

void suiteZeroAlloc() {
    for (const char* name : {"none", "native_taau", "fsr1", "nis", "cas"}) {
        for (post::ToneMappingMode tm : {post::ToneMappingMode::Local, post::ToneMappingMode::Global}) {
            post::PostPipeline p;
            check(configureCase(p, name, 0.5f, tm), "configure");
            const auto& s = p.selection();
            std::vector<math::Vec3> c;
            std::vector<float> d;
            std::vector<math::Vec2> m;
            std::vector<u8> bytes(std::size_t(s.displayWidth) * s.displayHeight * 4u);
            syntheticFrame(0, s.renderWidth, s.renderHeight, c, d, m);
            const post::PostTarget t{bytes.data(), s.displayWidth, s.displayHeight, false};
            p.processHdr(c.data(), d.data(), m.data(), t); // warm-up (first-frame history)
            p.processHdr(c.data(), d.data(), m.data(), t);
            g_allocations.store(0);
            g_count.store(true);
            for (u32 f = 0; f < 3u; ++f) {
                p.processHdr(c.data(), d.data(), m.data(), t);
            }
            g_count.store(false);
            const unsigned long long a = g_allocations.load();
            std::printf("zero_alloc: %-11s %s: %llu allocations in 3 steady-state frames\n", name,
                        tm == post::ToneMappingMode::Local ? "local " : "global", a);
            check(a == 0u, std::string("zero steady-state allocations: ") + name);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    post::registerPostOptions();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"composite", suiteComposite},     {"mip_bias", suiteMipBias},
                            {"options", suiteOptions},         {"upscaler_switch", suiteUpscalerSwitch},
                            {"nan_guard", suiteNanGuard},      {"ltm", suiteLtm},
                            {"determinism", suiteDeterminism}, {"zero_alloc", suiteZeroAlloc}};
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
    std::printf("PASS: rl_post %s\n", suite.c_str());
    return 0;
}
