// TAAU gates — native single-source temporal upsampling ("taau") on the upscaler reference scenes.
//
// Every scene (Showcase, ThinFastObject, MoireFlight; fuse/renderer/upscale/reference_scene.hpp) is rendered as a
// deterministic sequence: jittered render-resolution frames (UpscaleInputs) at 1.5x and 2x, and a native
// display-resolution ground truth (4x4 stratified box supersampling). TAAU is compared with the spatial baselines
// (bilinear, Catmull-Rom of the same jittered frame) against the ground truth after a warm-up:
//   - PSNR / SSIM / FLIP above calibrated floors and better than both spatial baselines;
//   - temporal flicker (tPSNR vs the ground-truth sequence) above a floor and better than Catmull-Rom;
//   - ghosting: residual on the trail the thin fast rod left over the last 6 frames below a ceiling;
//   - disocclusion: error on pixels not visible in the previous frame below a ceiling;
//   - reactive mask lowers the error on particle pixels; no NaN / Inf anywhere.
// Plus the input contract (jitter phases, mip bias, jittered projection, motion vectors of static / dynamic / sky
// pixels vs the camera matrices, UI composite), CpuReference == CpuParallel parity (0/2/4 workers), kernel stats,
// GPU-request fallback and LoadScale-aware performance numbers (reported, not gated).

#include <fuse/compute_kernel/load_scale.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/renderer/quality/image_metrics.hpp>
#include <fuse/renderer/taa/taau.hpp>
#include <fuse/renderer/upscale/reference_scene.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec2;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace kernel = fuse::kernel;
namespace q = fuse::renderer::quality;
namespace rs = fuse::renderer::refscene;
using fuse::renderer::TaauUpscaler;
using fuse::renderer::UpscaleInputs;
using fuse::renderer::UpscaleResolution;
namespace tk = fuse::renderer::taau_kernel;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectGreater(f64 value, f64 limit, const char* message) {
    if (!(value > limit)) {
        std::fprintf(stderr, "FAIL: %s (got %.5f, must exceed %.5f)\n", message, value, limit);
        ++g_failures;
    }
}

void expectLess(f64 value, f64 limit, const char* message) {
    if (!(value < limit)) {
        std::fprintf(stderr, "FAIL: %s (got %.5f, limit %.5f)\n", message, value, limit);
        ++g_failures;
    }
}

constexpr u32 kDisplayW = 240;
constexpr u32 kDisplayH = 144;
constexpr u32 kGtSamples = 4;   // 4x4 stratified samples per display pixel
constexpr u32 kFrames = 40;
constexpr u32 kWarmup = 16;     // metrics over frames [kWarmup, kFrames)
constexpr u32 kTrailFrames = 6; // ghost trail = rod coverage over the last 6 frames

f64 elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------------------
// Input contract
// ---------------------------------------------------------------------------------------------------------

void testInputContract() {
    using namespace fuse::renderer;
    const UpscaleResolution q15 = makeUpscaleResolution(kDisplayW, kDisplayH, 1.5f);
    const UpscaleResolution p20 = makeUpscaleResolution(kDisplayW, kDisplayH, UpscaleQualityMode::Performance);
    expectTrue(q15.render_width == 160u && q15.render_height == 96u && p20.render_width == 120u && p20.render_height == 72u,
               "render resolution = display / ratio");
    expectTrue(upscaleJitterPhaseCount(q15) == 18u && upscaleJitterPhaseCount(p20) == 32u &&
                   upscaleJitterPhaseCount(makeUpscaleResolution(kDisplayW, kDisplayH, 1.f)) == 8u,
               "jitter phase count = ceil(8 * ratio^2): 8 / 18 / 32");
    expectTrue(std::fabs(upscaleTextureMipBias(q15) - (std::log2(1.f / 1.5f) - 1.f)) < 1e-5f &&
                   std::fabs(upscaleTextureMipBias(p20) + 2.f) < 1e-5f,
               "mip bias = log2(render / display) - 1 (-1.585 at 1.5x, -2 at 2x)");

    // Halton(2, 3) jitter: in [-0.5, 0.5), cycle wraps, the cycle's mean is ~0.
    Vec2 mean{};
    bool inRange = true;
    for (u32 i = 0; i < 32u; ++i) {
        const Vec2 j = upscaleJitterOffset(i, 32u);
        inRange = inRange && j.x >= -0.5f && j.x < 0.5f && j.y >= -0.5f && j.y < 0.5f;
        mean = mean + j * (1.f / 32.f);
    }
    const Vec2 first = upscaleJitterOffset(0u, 32u);
    const Vec2 wrapped = upscaleJitterOffset(32u, 32u);
    expectTrue(inRange && std::fabs(mean.x) < 0.05f && std::fabs(mean.y) < 0.05f && first.x == wrapped.x &&
                   first.y == wrapped.y && std::fabs(first.x) < 1e-6f && std::fabs(first.y + 1.f / 6.f) < 1e-6f,
               "Halton(2,3) jitter: range, wrap, zero mean, first = (0, -1/6)");

    // Jittered projection shifts NDC by exactly the jitter NDC offset for any depth.
    const fuse::math::Mat4 proj = fuse::math::perspective(55.f, 240.f / 144.f, 0.05f, 200.f);
    const Vec2 jndc = upscaleJitterNdc({0.25f, -0.375f}, 120u, 72u);
    const fuse::math::Mat4 jproj = jitterProjection(proj, jndc);
    bool shiftOk = true;
    for (f32 z : {-0.5f, -3.f, -40.f}) {
        const Vec3 p{0.3f, -0.2f, z};
        const auto ndc = [&](const fuse::math::Mat4& m) {
            const auto& d = m.data;
            const f32 cx = d[0] * p.x + d[4] * p.y + d[8] * p.z + d[12];
            const f32 cy = d[1] * p.x + d[5] * p.y + d[9] * p.z + d[13];
            const f32 cw = d[3] * p.x + d[7] * p.y + d[11] * p.z + d[15];
            return Vec2{cx / cw, cy / cw};
        };
        const Vec2 delta = ndc(jproj) - ndc(proj);
        shiftOk = shiftOk && std::fabs(delta.x - jndc.x) < 1e-5f && std::fabs(delta.y - jndc.y) < 1e-5f;
    }
    expectTrue(shiftOk && std::fabs(jndc.x - 2.f * 0.25f / 120.f) < 1e-7f && std::fabs(jndc.y - 2.f * 0.375f / 72.f) < 1e-7f,
               "jitterProjection: NDC shift = (2 jx / w, -2 jy / h) at every depth");

    UpscaleInputs bad{};
    expectTrue(validateUpscaleInputs(bad) == UpscaleInputsError::InvalidResolution, "validate: invalid resolution");
    bad.resolution = p20;
    expectTrue(validateUpscaleInputs(bad) == UpscaleInputsError::MissingColor, "validate: missing colour");

    // Motion vectors from the reference renderer vs the camera matrices (static, sky, dynamic).
    const rs::ReferenceScene scene(rs::ReferenceSceneKind::Showcase);
    rs::ReferenceFrame frame{};
    expectTrue(scene.renderFrame(21u, p20, frame, kernel::Backend::CpuParallel, true), "render Showcase frame 21 at 2x");
    const UpscaleInputs in = frame.inputs();
    expectTrue(validateUpscaleInputs(in) == UpscaleInputsError::None && in.ui != nullptr && in.reactive != nullptr &&
                   in.transparency_composition != nullptr && in.jitter_phase_count == 32u && in.jitter_phase == 21u,
               "reference frame -> valid UpscaleInputs with masks, UI, jitter phase");
    u32 staticChecked = 0;
    u32 staticOk = 0;
    u32 skyChecked = 0;
    u32 skyOk = 0;
    u32 thinPixels = 0;
    u32 reactivePixels = 0;
    const f32 tanY = std::tan(0.5f * in.camera.vertical_fov_rad);
    const fuse::math::Mat4 invView = fuse::math::inverseAffine(in.camera.view);
    for (u32 y = 0; y < p20.render_height; ++y) {
        for (u32 x = 0; x < p20.render_width; ++x) {
            const u32 i = y * p20.render_width + x;
            reactivePixels += frame.reactive[i] > 0.05f ? 1u : 0u;
            // World position of the sample: jittered pixel ray at linear depth.
            const f32 ndcX = 2.f * (static_cast<f32>(x) + 0.5f + in.jitter_px.x) / static_cast<f32>(p20.render_width) - 1.f;
            const f32 ndcY = 1.f - 2.f * (static_cast<f32>(y) + 0.5f + in.jitter_px.y) / static_cast<f32>(p20.render_height);
            const Vec3 viewDir{ndcX * tanY * in.camera.aspect, ndcY * tanY, -1.f};
            const u32 id = frame.object_id[i];
            if (id == rs::kObjectThin) {
                ++thinPixels;
            }
            if ((x % 7u) != 0u || (y % 5u) != 0u) {
                continue;
            }
            if (id == rs::kObjectSky) {
                const Vec3 dir = fuse::math::transformDirection(invView, viewDir).normalized();
                Vec2 mv{};
                if (upscaleSkyMotion(in.camera, in.previous_camera, dir, mv)) {
                    ++skyChecked;
                    skyOk += (mv - frame.motion[i]).length() < 2e-4f ? 1u : 0u;
                }
            } else if (id == rs::kObjectGround || (id >= 16u && id < 32u)) {
                const Vec3 world = fuse::math::transformPoint(invView, viewDir * frame.depth[i]);
                Vec2 mv{};
                if (upscaleStaticMotion(in.camera, in.previous_camera, world, mv)) {
                    ++staticChecked;
                    staticOk += (mv - frame.motion[i]).length() < 2e-4f ? 1u : 0u;
                }
            }
        }
    }
    std::printf("  motion vectors: static %u/%u, sky %u/%u match the camera matrices; %u rod pixels, %u reactive pixels\n",
                staticOk, staticChecked, skyOk, skyChecked, thinPixels, reactivePixels);
    expectTrue(staticChecked > 50u && staticOk >= staticChecked * 98u / 100u, "static motion == matrix reprojection");
    expectTrue(skyChecked > 20u && skyOk >= skyChecked * 98u / 100u, "sky motion == camera-rotation-only reprojection");
    expectTrue(thinPixels > 3u && reactivePixels > 20u, "frame contains the thin rod and particle (reactive) pixels");
    // Dynamic object: rod pixels move by the rod displacement (plus camera), clearly more than the background.
    f32 rodMotion = 0.f;
    f32 bgMotion = 0.f;
    u32 rodCount = 0;
    u32 bgCount = 0;
    for (usize i = 0; i < frame.motion.size(); ++i) {
        const f32 m = frame.motion[i].length() * static_cast<f32>(kDisplayW);
        if (frame.object_id[i] == rs::kObjectThin) {
            rodMotion += m;
            ++rodCount;
        } else if (frame.object_id[i] == rs::kObjectGround) {
            bgMotion += m;
            ++bgCount;
        }
    }
    rodMotion /= static_cast<f32>(std::max(1u, rodCount));
    bgMotion /= static_cast<f32>(std::max(1u, bgCount));
    std::printf("  rod motion %.2f display px/frame, ground %.2f\n", rodMotion, bgMotion);
    expectGreater(rodMotion, 6.0, "dynamic rod motion vectors carry the object motion (> 6 display px / frame)");

    // UI composite: opaque HUD pixels replace the scene, transparent ones keep it.
    std::vector<Vec3> sceneImg(p20.display_width * p20.display_height, Vec3{0.2f, 0.3f, 0.4f});
    std::vector<Vec3> composed(sceneImg.size());
    compositeUpscaledWithUi(sceneImg.data(), frame.ui.data(), p20.display_width, p20.display_height, composed.data());
    u32 opaque = 0;
    u32 clear = 0;
    bool uiOk = true;
    for (usize i = 0; i < composed.size(); ++i) {
        const Vec4 u = frame.ui[i];
        if (u.w == 0.f) {
            ++clear;
            uiOk = uiOk && composed[i].x == sceneImg[i].x && composed[i].z == sceneImg[i].z;
        } else if (u.w >= 1.f) {
            ++opaque;
            uiOk = uiOk && composed[i].x == u.x && composed[i].y == u.y;
        }
    }
    expectTrue(uiOk && opaque > 50u && clear > composed.size() / 2u, "UI composited after the upscale (premultiplied over)");
}

// ---------------------------------------------------------------------------------------------------------
// Quality gates
// ---------------------------------------------------------------------------------------------------------

struct Scores {
    f64 psnr = 0.0;
    f64 ssim = 0.0;
    f64 flip = 0.0;
    f64 tpsnr = 0.0;
    f64 tflip = 0.0;
    f64 ghost = 0.0;       // mean |luma error| on the rod trail
    f64 ghost_rms = 0.0;
    f64 ghost_frac = 0.0;  // share of trail pixels with |luma error| > 0.1
    f64 disocclusion = 0.0; // mean |luma error| on disoccluded pixels
    f64 particle = 0.0;     // mean |luma error| on particle (reactive) pixels
    u64 ghost_pixels = 0;
    u64 disocclusion_pixels = 0;
    bool finite = true;
};

struct GroundTruthSequence {
    std::vector<rs::GroundTruthFrame> frames;
};

struct Thresholds {
    f64 psnr;
    f64 ssim;
    f64 flip;
    f64 tpsnr;
    f64 ghost;
    f64 disocclusion;
};

// kTaauNoRejection is the negative control of the ghosting / disocclusion metrics: the same TAAU with clipping,
// depth and velocity rejection disabled must show a clearly larger trail residual.
enum Method : u32 { kTaau = 0, kBilinear = 1, kCatmullRom = 2, kTaauNoReactive = 3, kTaauNoRejection = 4, kMethodCount = 5 };
const char* kMethodNames[kMethodCount] = {"TAAU", "bilinear", "Catmull-Rom", "TAAU(no reactive)", "TAAU(no rejection)"};

void evaluateSequence(const rs::ReferenceScene& scene, const GroundTruthSequence& gt, f32 ratio, Scores out[kMethodCount]) {
    const UpscaleResolution res = fuse::renderer::makeUpscaleResolution(kDisplayW, kDisplayH, ratio);
    const usize dn = static_cast<usize>(kDisplayW) * kDisplayH;
    TaauUpscaler taau{};
    TaauUpscaler taauNoReactive{};
    tk::Settings noRejection{};
    noRejection.clamp_gamma = 1.0e6f;
    noRejection.depth_rejection = 0.f;
    noRejection.velocity_rejection_px = 0.f;
    TaauUpscaler taauNoRejection{noRejection};
    std::vector<Vec3> outputs[kMethodCount];
    std::vector<Vec3> previous[kMethodCount];
    for (u32 m = 0; m < kMethodCount; ++m) {
        outputs[m].resize(dn);
        previous[m].resize(dn);
    }
    q::ImageQualityEvaluator eval{{q::QualityEncoding::Linear, 1.f, kernel::Backend::CpuParallel}};
    std::vector<u8> trail(dn);
    std::vector<u8> trailDilated(dn);
    std::vector<u8> disocc(dn);
    std::vector<u8> particles(dn);
    rs::ReferenceFrame frame{};
    const u32 evalFrames = kFrames - kWarmup;
    u64 ghostCount[kMethodCount] = {};
    u64 disoccCount[kMethodCount] = {};
    u64 particleCount[kMethodCount] = {};
    for (u32 f = 0; f < kFrames; ++f) {
        expectTrue(scene.renderFrame(f, res, frame), "render reference frame");
        UpscaleInputs in = frame.inputs();
        expectTrue(taau.upscale(in, outputs[kTaau].data()), "taau upscale");
        UpscaleInputs noReactive = in;
        noReactive.reactive = nullptr;
        noReactive.transparency_composition = nullptr;
        expectTrue(taauNoReactive.upscale(noReactive, outputs[kTaauNoReactive].data()), "taau upscale (no reactive)");
        expectTrue(taauNoRejection.upscale(in, outputs[kTaauNoRejection].data()), "taau upscale (no rejection)");
        expectTrue(fuse::renderer::spatialUpscale(in, tk::SpatialFilter::Bilinear, outputs[kBilinear].data()) &&
                       fuse::renderer::spatialUpscale(in, tk::SpatialFilter::CatmullRom, outputs[kCatmullRom].data()),
                   "spatial upscale baselines");
        if (f >= kWarmup) {
            const rs::GroundTruthFrame& g = gt.frames[f];
            const rs::GroundTruthFrame& gp = gt.frames[f - 1u];
            const q::QualityImage ref{g.color.data(), kDisplayW, kDisplayH};
            const q::QualityImage refPrev{gp.color.data(), kDisplayW, kDisplayH};
            // Ghost trail: rod coverage over the previous kTrailFrames frames, not now (dilated by 1 px).
            const u32* history[kTrailFrames];
            for (u32 k = 0; k < kTrailFrames; ++k) {
                history[k] = gt.frames[f - 1u - k].object_id.data();
            }
            q::buildTrailMask(history, kTrailFrames, g.object_id.data(), rs::kObjectThin, kDisplayW, kDisplayH, trail.data());
            q::dilateMask(trail.data(), kDisplayW, kDisplayH, 1u, trailDilated.data());
            for (usize i = 0; i < dn; ++i) {
                trailDilated[i] = (trailDilated[i] != 0u && g.object_id[i] != rs::kObjectThin) ? 1u : 0u;
            }
            // Particle pixels: where the ground truth render is covered by particles (render-res reactive, nearest).
            for (u32 y = 0; y < kDisplayH; ++y) {
                for (u32 x = 0; x < kDisplayW; ++x) {
                    const u32 rx = std::min(res.render_width - 1u, x * res.render_width / kDisplayW);
                    const u32 ry = std::min(res.render_height - 1u, y * res.render_height / kDisplayH);
                    particles[y * kDisplayW + x] = frame.reactive[ry * res.render_width + rx] > 0.2f ? 1u : 0u;
                }
            }
            for (usize i = 0; i < dn; ++i) {
                disocc[i] = g.disoccluded[i];
            }
            for (u32 m = 0; m < kMethodCount; ++m) {
                const q::QualityImage test{outputs[m].data(), kDisplayW, kDisplayH};
                const q::QualityImage testPrev{previous[m].data(), kDisplayW, kDisplayH};
                Scores& s = out[m];
                s.finite = s.finite && !q::imageHasNonFinite(outputs[m].data(), dn);
                q::PsnrResult pr{};
                eval.psnr(test, ref, pr);
                s.psnr += pr.psnr / evalFrames;
                s.ssim += eval.ssim(test, ref) / evalFrames;
                s.flip += eval.flip(test, ref) / evalFrames;
                if (f > kWarmup) {
                    q::TemporalResult tr{};
                    eval.temporal(testPrev, test, refPrev, ref, true, tr);
                    s.tpsnr += tr.tpsnr / (evalFrames - 1u);
                    s.tflip += tr.flip_excess / (evalFrames - 1u);
                }
                q::MaskedErrorResult mr{};
                eval.maskedError(test, ref, trailDilated.data(), mr);
                s.ghost += mr.mean_abs_luma * static_cast<f64>(mr.pixels);
                s.ghost_rms += mr.rms * mr.rms * static_cast<f64>(mr.pixels);
                s.ghost_frac += mr.fraction_above * static_cast<f64>(mr.pixels);
                ghostCount[m] += mr.pixels;
                eval.maskedError(test, ref, disocc.data(), mr);
                s.disocclusion += mr.mean_abs_luma * static_cast<f64>(mr.pixels);
                disoccCount[m] += mr.pixels;
                eval.maskedError(test, ref, particles.data(), mr);
                s.particle += mr.mean_abs_luma * static_cast<f64>(mr.pixels);
                particleCount[m] += mr.pixels;
            }
        }
        for (u32 m = 0; m < kMethodCount; ++m) {
            previous[m].swap(outputs[m]);
        }
    }
    for (u32 m = 0; m < kMethodCount; ++m) {
        Scores& s = out[m];
        s.ghost_pixels = ghostCount[m];
        s.disocclusion_pixels = disoccCount[m];
        s.ghost = ghostCount[m] > 0u ? s.ghost / static_cast<f64>(ghostCount[m]) : 0.0;
        s.ghost_rms = ghostCount[m] > 0u ? std::sqrt(s.ghost_rms / static_cast<f64>(ghostCount[m])) : 0.0;
        s.ghost_frac = ghostCount[m] > 0u ? s.ghost_frac / static_cast<f64>(ghostCount[m]) : 0.0;
        s.disocclusion = disoccCount[m] > 0u ? s.disocclusion / static_cast<f64>(disoccCount[m]) : 0.0;
        s.particle = particleCount[m] > 0u ? s.particle / static_cast<f64>(particleCount[m]) : 0.0;
    }
}

void testQualityGates() {
    struct SceneGate {
        rs::ReferenceSceneKind kind;
        Thresholds t15;
        Thresholds t20;
    };
    // Calibrated floors / ceilings: measured TAAU values (printed table; CpuParallel == CpuReference, so they are
    // deterministic on one toolchain) minus ~5% PSNR / ~1.5% SSIM / plus ~10-15% FLIP, ghost and disocclusion
    // margins for other compilers' float contraction. Order: PSNR, SSIM, FLIP, tPSNR, ghost, disocclusion.
    //   measured 1.5x: Showcase 27.99 / 0.927 / 0.059 / 27.6 / 0.031 / 0.048   2x: 26.28 / 0.896 / 0.074 / 26.2 / 0.040 / 0.058
    //                  ThinFast 32.80 / 0.982 / 0.037 / 36.1 / 0.023 / 0.038       31.12 / 0.973 / 0.044 / 34.3 / 0.030 / 0.048
    //                  Moire    26.80 / 0.898 / 0.053 / 25.9 /   -   / 0.030       25.55 / 0.872 / 0.063 / 24.8 /   -   / 0.033
    const SceneGate gates[] = {
        {rs::ReferenceSceneKind::Showcase, {26.5, 0.910, 0.065, 26.0, 0.036, 0.055}, {25.0, 0.880, 0.081, 24.5, 0.045, 0.065}},
        {rs::ReferenceSceneKind::ThinFastObject, {31.0, 0.975, 0.042, 34.0, 0.027, 0.045}, {29.5, 0.965, 0.050, 32.5, 0.035, 0.056}},
        {rs::ReferenceSceneKind::MoireFlight, {25.3, 0.885, 0.059, 24.3, 1.0, 0.060}, {24.2, 0.855, 0.070, 23.3, 1.0, 0.060}},
    };
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(3);
    std::printf("  %-15s %-5s %-18s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n", "scene", "ratio", "method", "PSNR", "SSIM",
                "FLIP", "tPSNR", "tFLIP+", "ghost", "ghost>.1", "disocc", "partic");
    for (const SceneGate& gate : gates) {
        const rs::ReferenceScene scene(gate.kind);
        const auto t0 = std::chrono::steady_clock::now();
        GroundTruthSequence gt{};
        gt.frames.resize(kFrames);
        for (u32 f = 0; f < kFrames; ++f) {
            expectTrue(scene.renderGroundTruth(f, kDisplayW, kDisplayH, kGtSamples, gt.frames[f]), "render ground truth");
        }
        const f64 gtMs = elapsedMs(t0);
        for (f32 ratio : {1.5f, 2.f}) {
            const auto t1 = std::chrono::steady_clock::now();
            Scores s[kMethodCount];
            evaluateSequence(scene, gt, ratio, s);
            for (u32 m = 0; m < kMethodCount; ++m) {
                std::printf("  %-15s %-5.1f %-18s %8.3f %8.4f %8.4f %8.3f %8.4f %8.4f %8.4f %8.4f %8.4f\n",
                            rs::referenceSceneLabel(gate.kind), ratio, kMethodNames[m], s[m].psnr, s[m].ssim, s[m].flip,
                            s[m].tpsnr, s[m].tflip, s[m].ghost, s[m].ghost_frac, s[m].disocclusion, s[m].particle);
            }
            std::printf("  %-15s %-5.1f (%llu trail px, %llu disoccluded px; GT %.0f ms, eval %.0f ms)\n",
                        rs::referenceSceneLabel(gate.kind), ratio, static_cast<unsigned long long>(s[kTaau].ghost_pixels),
                        static_cast<unsigned long long>(s[kTaau].disocclusion_pixels), gtMs, elapsedMs(t1));
            const Thresholds& t = ratio < 1.75f ? gate.t15 : gate.t20;
            char label[192];
            const char* name = rs::referenceSceneLabel(gate.kind);
            const auto gateLabel = [&](const char* what) {
                std::snprintf(label, sizeof(label), "%s %.1fx: %s", name, ratio, what);
                return label;
            };
            expectTrue(s[kTaau].finite && s[kBilinear].finite && s[kTaauNoReactive].finite, gateLabel("no NaN / Inf"));
            expectGreater(s[kTaau].psnr, t.psnr, gateLabel("TAAU PSNR floor"));
            expectGreater(s[kTaau].ssim, t.ssim, gateLabel("TAAU SSIM floor"));
            expectLess(s[kTaau].flip, t.flip, gateLabel("TAAU FLIP ceiling"));
            expectGreater(s[kTaau].tpsnr, t.tpsnr, gateLabel("TAAU temporal PSNR (flicker) floor"));
            expectLess(s[kTaau].ghost, t.ghost, gateLabel("TAAU ghosting residual ceiling"));
            expectLess(s[kTaau].disocclusion, t.disocclusion, gateLabel("TAAU disocclusion error ceiling"));
            for (u32 b : {static_cast<u32>(kBilinear), static_cast<u32>(kCatmullRom)}) {
                std::snprintf(label, sizeof(label), "%s %.1fx: TAAU beats %s on PSNR, SSIM and FLIP", name, ratio,
                              kMethodNames[b]);
                expectTrue(s[kTaau].psnr > s[b].psnr && s[kTaau].ssim > s[b].ssim && s[kTaau].flip < s[b].flip, label);
            }
            expectGreater(s[kTaau].tpsnr, s[kCatmullRom].tpsnr, gateLabel("TAAU flickers less than Catmull-Rom (tPSNR)"));
            expectGreater(s[kTaau].tpsnr, s[kBilinear].tpsnr, gateLabel("TAAU flickers less than bilinear (tPSNR)"));
            if (gate.kind == rs::ReferenceSceneKind::Showcase) {
                expectLess(s[kTaau].particle, s[kTaauNoReactive].particle, gateLabel("reactive mask lowers particle error"));
            }
            if (gate.kind != rs::ReferenceSceneKind::MoireFlight) {
                // Negative control: without clipping / depth / velocity rejection the same TAAU ghosts and smears
                // disocclusions — the metrics must see it, and the rejection must remove most of it.
                expectLess(s[kTaau].ghost_frac, 0.8 * s[kTaauNoRejection].ghost_frac,
                           gateLabel("rejection removes >= 20% of the ghost-trail pixels (vs no-rejection control)"));
                expectLess(s[kTaau].disocclusion, 0.85 * s[kTaauNoRejection].disocclusion,
                           gateLabel("rejection lowers the disocclusion error >= 15% (vs no-rejection control)"));
            }
        }
    }
    scheduler.shutdown();
}

// ---------------------------------------------------------------------------------------------------------
// Parity, stats, fallback, performance
// ---------------------------------------------------------------------------------------------------------

void testParityAndStats() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    const UpscaleResolution res = fuse::renderer::makeUpscaleResolution(107u, 61u, 2.f); // partial 8x8 tiles
    const rs::ReferenceScene scene(rs::ReferenceSceneKind::Showcase);
    constexpr u32 kParityFrames = 5;
    const usize dn = static_cast<usize>(res.display_width) * res.display_height;

    scheduler.shutdown();
    kernel::reset_kernel_stats();
    std::vector<rs::ReferenceFrame> frames(kParityFrames);
    std::vector<std::vector<Vec3>> reference(kParityFrames, std::vector<Vec3>(dn));
    TaauUpscaler taau{};
    for (u32 f = 0; f < kParityFrames; ++f) {
        expectTrue(scene.renderFrame(10u + f, res, frames[f], kernel::Backend::CpuReference), "CpuReference scene render");
        expectTrue(taau.upscale(frames[f].inputs(), reference[f].data(), kernel::Backend::CpuReference), "CpuReference taau");
    }
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(tk::kName, stats) && stats.launches == kParityFrames &&
                   stats.items == static_cast<u64>(dn) * kParityFrames && stats.last_backend == kernel::Backend::CpuReference,
               "\"taau\" stats: one launch per frame, one item per display pixel");
    expectTrue(kernel::find_kernel_stats("upscale_ref_scene", stats) &&
                   stats.items == static_cast<u64>(res.render_width) * res.render_height * kParityFrames,
               "\"upscale_ref_scene\" stats: one item per render pixel");

    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        TaauUpscaler par{};
        bool same = true;
        std::vector<Vec3> out(dn);
        for (u32 f = 0; f < kParityFrames; ++f) {
            rs::ReferenceFrame pf{};
            scene.renderFrame(10u + f, res, pf, kernel::Backend::CpuParallel);
            same = same && std::memcmp(pf.color.data(), frames[f].color.data(), pf.color.size() * sizeof(Vec3)) == 0 &&
                   std::memcmp(pf.motion.data(), frames[f].motion.data(), pf.motion.size() * sizeof(Vec2)) == 0 &&
                   std::memcmp(pf.depth.data(), frames[f].depth.data(), pf.depth.size() * sizeof(f32)) == 0;
            par.upscale(pf.inputs(), out.data(), kernel::Backend::CpuParallel);
            same = same && std::memcmp(out.data(), reference[f].data(), dn * sizeof(Vec3)) == 0;
        }
        same = same && std::memcmp(par.history().data(), taau.history().data(), dn * sizeof(Vec4)) == 0;
        char label[128];
        std::snprintf(label, sizeof(label), "CpuReference == CpuParallel bit-exact scene + TAAU (%u workers)", workers);
        expectTrue(same, label);
    }
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        TaauUpscaler gpu{};
        std::vector<Vec3> out(dn);
        expectTrue(gpu.upscale(frames[0].inputs(), out.data(), kernel::Backend::Cuda) &&
                       gpu.lastStats().backend == kernel::Backend::CpuParallel &&
                       std::memcmp(out.data(), reference[0].data(), dn * sizeof(Vec3)) == 0,
                   "Cuda request without a device falls back to CpuParallel (same image)");
    }
    // Reset drops history; invalid inputs are rejected.
    UpscaleInputs in = frames[1].inputs();
    in.reset_history = true;
    std::vector<Vec3> out(dn);
    expectTrue(taau.upscale(in, out.data()) && !taau.lastStats().history_used, "reset_history drops history");
    in.depth = nullptr;
    expectTrue(!taau.upscale(in, out.data()), "missing depth rejected");
    scheduler.shutdown();
}

void reportPerformance() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(3);
    const kernel::LoadScale scale = kernel::load_scale();
    const u32 dw = kernel::scaled_extent(1280u, scale.resolution);
    const u32 dh = kernel::scaled_extent(720u, scale.resolution);
    for (f32 ratio : {1.5f, 2.f}) {
        const UpscaleResolution res = fuse::renderer::makeUpscaleResolution(dw, dh, ratio);
        const usize rn = static_cast<usize>(res.render_width) * res.render_height;
        std::vector<Vec3> color(rn);
        std::vector<f32> depth(rn);
        std::vector<Vec2> motion(rn);
        std::vector<f32> reactive(rn, 0.f);
        for (usize i = 0; i < rn; ++i) {
            const f32 v = static_cast<f32>((i * 2654435761u) >> 24) / 255.f;
            color[i] = {v, 0.5f * v, 0.25f + 0.5f * v};
            depth[i] = 1.f + 10.f * v;
            motion[i] = {0.002f * v, -0.001f};
        }
        UpscaleInputs in{};
        in.resolution = res;
        in.color = color.data();
        in.depth = depth.data();
        in.motion = motion.data();
        in.reactive = reactive.data();
        std::vector<Vec3> out(static_cast<usize>(dw) * dh);
        for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
            TaauUpscaler taau{};
            kernel::reset_kernel_stats();
            for (u32 f = 0; f < 6u; ++f) {
                in.jitter_px = fuse::renderer::upscaleJitterOffset(f, 32u);
                taau.upscale(in, out.data(), b);
            }
            kernel::KernelStats st{};
            kernel::find_kernel_stats(tk::kName, st);
            const f64 avgMs = static_cast<f64>(st.total_ns) / static_cast<f64>(std::max<u64>(1u, st.launches)) * 1e-6;
            std::printf("  perf taau %ux%u -> %ux%u (LoadScale res %.2f) %-12s: avg %.2f ms, min %.2f ms, %.1f Mpix/s (%llu launches)\n",
                        res.render_width, res.render_height, dw, dh, scale.resolution, kernel::backend_name(b), avgMs,
                        static_cast<f64>(st.min_ns) * 1e-6, static_cast<f64>(dw) * dh / (avgMs * 1e3),
                        static_cast<unsigned long long>(st.launches));
        }
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    kernel::load_scale_from_env();
    testInputContract();
    testParityAndStats();
    testQualityGates();
    reportPerformance();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d TAAU gate check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("TAAU gates passed\n");
    return EXIT_SUCCESS;
}
