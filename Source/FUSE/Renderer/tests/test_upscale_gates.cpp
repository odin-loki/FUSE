// Upscaler gates (fuse/renderer/upscale/, docs/upscalers.md):
//   1. Interface / registry: quality-mode ratios and render extents, capability queries, selection by
//      requirements, register / unregister / create, input validation, Halton jitter provider (FSR phase
//      count), mip bias, temporal history invalidation (native_taau adapter over the TAAU kernel), fsr1
//      backend == run_easu + run_rcas.
//   2. Constant setup: easu_constants / rcas_constants / cas_constants are bit-identical to the vendored
//      FidelityFX SDK v1.1.4 host helpers (ffxFsrPopulateEasuConstants, FsrRcasCon, ffxCasSetup compiled
//      with FFX_CPU from Engine/lib/fidelityfx).
//   3. Parity: CpuReference vs CpuParallel (0 / 2 / 4 workers) bit-exact for EASU, RCAS, CAS, NIS and
//      bilinear on grids with partial 8x8 edge tiles; kernel stats names / item counts; a Cuda request
//      without a device falls back to CpuParallel with identical output.
//   4. Quality vs bilinear on an analytic scene (box-filtered render-resolution input vs box-filtered
//      display-resolution ground truth) at 1.5x / 1.7x / 2x / 3x: PSNR and SSIM per backend; EASU and
//      FSR1 (EASU + RCAS) beat bilinear on both, NIS beats bilinear on both where supported (<= 2x), CAS
//      improves a blurred image; native_taau (registry adapter) on a static jittered sequence beats every
//      spatial backend. PSNR / SSIM / FLIP from fuse::renderer::quality (quality/image_metrics.hpp).
//      Results are written to FUSE_UPSCALE_REPORT_PATH (JSON) and printed.

#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/quality/image_metrics.hpp>
#include <fuse/renderer/upscale/upscale_passes.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>

#include "upscale_test_images.hpp"

// Vendored FidelityFX host-side constant helpers (CPU mode of the portable FFX headers).
#include <cmath>
#include <cstdint>
#define FFX_CPU 1
#include <FidelityFX/gpu/ffx_core.h>
#include <FidelityFX/gpu/cas/ffx_cas.h>
#include <FidelityFX/gpu/fsr1/ffx_fsr1.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec2;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace quality = fuse::renderer::quality;
namespace kernel = fuse::kernel;
namespace up = fuse::renderer::upscale;
namespace ut = fuse::upscale_test;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f64 value, f64 expected, f64 eps, const char* message) {
    if (!(std::fabs(value - expected) <= eps)) {
        std::fprintf(stderr, "FAIL: %s (got %.6f, expected %.6f)\n", message, value, expected);
        ++g_failures;
    }
}

up::ConstRgbaImage cview(const std::vector<Vec4>& v, u32 w, u32 h) { return {v.data(), w, h}; }
up::RgbaImage view(std::vector<Vec4>& v, u32 w, u32 h) { return {v.data(), w, h}; }

// ---- 1. Interface / registry --------------------------------------------------------------------------

void testQualityModes() {
    expectNear(up::quality_mode_ratio(up::QualityMode::NativeAA), 1.0, 0.0, "NativeAA ratio 1.0");
    expectNear(up::quality_mode_ratio(up::QualityMode::Quality), 1.5, 0.0, "Quality ratio 1.5");
    expectNear(up::quality_mode_ratio(up::QualityMode::Balanced), 1.7, 1e-6, "Balanced ratio 1.7");
    expectNear(up::quality_mode_ratio(up::QualityMode::Performance), 2.0, 0.0, "Performance ratio 2.0");
    expectNear(up::quality_mode_ratio(up::QualityMode::UltraPerformance), 3.0, 0.0, "UltraPerformance ratio 3.0");
    const up::Extent2D display{1920, 1080};
    expectTrue(up::render_extent(display, up::QualityMode::NativeAA) == up::Extent2D{1920, 1080}, "1080p NativeAA");
    expectTrue(up::render_extent(display, up::QualityMode::Quality) == up::Extent2D{1280, 720}, "1080p Quality 1280x720");
    expectTrue(up::render_extent(display, up::QualityMode::Balanced) == up::Extent2D{1129, 635},
               "1080p Balanced 1129x635");
    expectTrue(up::render_extent(display, up::QualityMode::Performance) == up::Extent2D{960, 540},
               "1080p Performance 960x540");
    expectTrue(up::render_extent(display, up::QualityMode::UltraPerformance) == up::Extent2D{640, 360},
               "1080p UltraPerformance 640x360");
    expectTrue(up::render_extent(display, up::QualityMode::UltraQuality) == up::Extent2D{1477, 831},
               "1080p UltraQuality 1477x831");
    expectTrue(up::render_extent({1, 1}, up::QualityMode::UltraPerformance) == up::Extent2D{1, 1},
               "render extent never 0");
    expectNear(up::mip_lod_bias({1280, 720}, display, true), std::log2(1280.0 / 1920.0) - 1.0, 1e-5,
               "temporal mip bias log2(render/display) - 1");
    expectNear(up::mip_lod_bias({960, 540}, display, false), -1.0, 1e-6, "spatial mip bias log2(render/display)");
    expectTrue(std::strcmp(up::quality_mode_name(up::QualityMode::Balanced), "Balanced") == 0, "mode names");
}

void testRegistry() {
    const up::UpscalerRegistry& reg = up::UpscalerRegistry::instance();
    expectTrue(reg.backends().size() == 4u, "four built-in backends (native_taau, fsr1, nis, cas)");
    const up::UpscalerCaps* taau = reg.find(up::kNativeTaauName);
    const up::UpscalerCaps* fsr1 = reg.find(up::kFsr1Name);
    const up::UpscalerCaps* nis = reg.find(up::kNisName);
    const up::UpscalerCaps* cas = reg.find(up::kCasName);
    expectTrue(taau && fsr1 && nis && cas, "built-ins found by name");
    expectTrue(reg.find(up::kFsr3Name) == nullptr, "fsr3 absent without FUSE_UPSCALER_FSR3");
    if (!(taau && fsr1 && nis && cas)) {
        return;
    }
    expectTrue(taau->temporal && taau->kind == up::UpscalerKind::Temporal && taau->needs_motion_vectors &&
                   taau->needs_depth && taau->needs_jitter && taau->accepts_reactive_mask &&
                   taau->accepts_transparency_mask && taau->needs_exposure && taau->hdr_input && !taau->stub &&
                   taau->quality_modes == up::kAllQualityModes && taau->supports_ratio(3.f),
               "native_taau caps: temporal, depth + MV + jitter + exposure, optional reactive / T&C masks, HDR, all modes");
    expectTrue(!fsr1->temporal && fsr1->kind == up::UpscalerKind::Spatial && !fsr1->needs_motion_vectors &&
                   !fsr1->needs_reactive_mask && !fsr1->needs_exposure && !fsr1->hdr_input &&
                   fsr1->built_in_sharpening && !fsr1->supports_mode(up::QualityMode::NativeAA) &&
                   fsr1->supports_mode(up::QualityMode::UltraPerformance) && fsr1->supports_api(up::UpscalerApi::Cpu) &&
                   !fsr1->supports_api(up::UpscalerApi::Vulkan) && std::strcmp(fsr1->license, "MIT") == 0,
               "fsr1 caps: spatial, LDR, RCAS sharpening, UltraQuality..UltraPerformance, CPU, MIT");
    expectTrue(nis->supports_ratio(2.f) && !nis->supports_ratio(3.f) &&
                   !nis->supports_mode(up::QualityMode::UltraPerformance) &&
                   nis->supports_mode(up::QualityMode::Performance),
               "nis caps: up to 2x");
    expectTrue(cas->kind == up::UpscalerKind::Sharpen && cas->supports_ratio(1.f) && !cas->supports_ratio(1.5f) &&
                   cas->quality_modes == up::quality_mode_bit(up::QualityMode::NativeAA),
               "cas caps: 1x sharpener");

    // Selection: preference order (temporal first), input availability, ratio limits, API filter.
    up::UpscalerRequirements req{};
    req.mode = up::QualityMode::Quality;
    const up::UpscalerCaps* sel = reg.select(req);
    expectTrue(sel != nullptr && std::strcmp(sel->name, "native_taau") == 0, "Quality/CPU selects native_taau");
    req.have_motion_vectors = false;
    sel = reg.select(req);
    expectTrue(sel != nullptr && std::strcmp(sel->name, "fsr1") == 0, "no motion vectors -> spatial fsr1");
    req = {};
    req.mode = up::QualityMode::UltraPerformance;
    req.allow_temporal = false;
    sel = reg.select(req);
    expectTrue(sel != nullptr && std::strcmp(sel->name, "fsr1") == 0, "UltraPerformance spatial -> fsr1 (nis <= 2x)");
    req = {};
    req.mode = up::QualityMode::NativeAA;
    sel = reg.select(req);
    expectTrue(sel != nullptr && std::strcmp(sel->name, "native_taau") == 0, "NativeAA -> native_taau (TAA)");
    req.allow_temporal = false;
    sel = reg.select(req);
    expectTrue(sel != nullptr && std::strcmp(sel->name, "cas") == 0, "NativeAA spatial-only -> cas");
    req = {};
    req.have_exposure = false;
    sel = reg.select(req);
    expectTrue(sel != nullptr && std::strcmp(sel->name, "fsr1") == 0, "no exposure -> spatial");
    req = {};
    req.api = up::UpscalerApi::Vulkan;
    expectTrue(reg.select(req) == nullptr, "no Vulkan backend compiled in this pass");
    up::UpscalerCaps stubCaps = *fsr1;
    stubCaps.stub = true;
    req = {};
    req.allow_temporal = false;
    expectTrue(!up::caps_satisfy(stubCaps, req), "stub backends excluded unless allow_stub");
    req.allow_stub = true;
    expectTrue(up::caps_satisfy(stubCaps, req), "allow_stub admits stub backends");

    // A private registry: register / duplicate / unregister / create.
    up::UpscalerRegistry local;
    expectTrue(local.backends().empty(), "fresh registry empty");
    local.register_builtin_backends();
    expectTrue(local.backends().size() == 4u, "register_builtin_backends");
    expectTrue(!local.register_backend(*fsr1, local.backends()[1].factory), "duplicate name rejected");
    up::UpscalerCaps custom = *cas;
    custom.name = "custom";
    expectTrue(!local.register_backend(custom, nullptr), "null factory rejected");
    expectTrue(local.register_backend(custom, local.backends()[3].factory), "custom backend registered");
    expectTrue(local.find("custom") != nullptr && local.create("custom") != nullptr, "custom create");
    expectTrue(local.unregister_backend("custom") && local.find("custom") == nullptr, "unregister");
    expectTrue(local.create("nope") == nullptr, "unknown create -> null");
    for (const up::UpscalerBackendEntry& e : local.backends()) {
        const std::unique_ptr<up::IUpscaler> u = local.create(e.caps.name);
        expectTrue(u != nullptr && std::strcmp(u->caps().name, e.caps.name) == 0, "instance caps match registry");
    }
}

void testJitter() {
    const up::HaltonJitterProvider jitter;
    expectTrue(jitter.phase_count({1280, 720}, {1920, 1080}) == 18u, "phase count 1.5x = ceil(8 * 2.25) = 18");
    expectTrue(jitter.phase_count({960, 540}, {1920, 1080}) == 32u, "phase count 2x = 32");
    expectTrue(jitter.phase_count({640, 360}, {1920, 1080}) == 72u, "phase count 3x = 72");
    expectTrue(jitter.phase_count({1920, 1080}, {1920, 1080}) == 8u, "phase count 1x = 8");
    const Vec2 j0 = jitter.offset_px(0, {960, 540}, {1920, 1080});
    expectNear(j0.x, 0.0, 1e-6, "first jitter x = halton(1, 2) - 0.5 = 0");
    expectNear(j0.y, 1.0 / 3.0 - 0.5, 1e-6, "first jitter y = halton(1, 3) - 0.5");
    f64 sx = 0.0;
    f64 sy = 0.0;
    bool inRange = true;
    const u32 n = jitter.phase_count({960, 540}, {1920, 1080});
    for (u32 i = 0; i < n; ++i) {
        const Vec2 j = jitter.offset_px(i, {960, 540}, {1920, 1080});
        inRange = inRange && j.x > -0.5f && j.x < 0.5f && j.y > -0.5f && j.y < 0.5f;
        sx += j.x;
        sy += j.y;
        const Vec2 again = jitter.offset_px(i + n, {960, 540}, {1920, 1080});
        inRange = inRange && again.x == j.x && again.y == j.y;
    }
    expectTrue(inRange, "jitter offsets in (-0.5, 0.5) and periodic in phase_count");
    expectTrue(std::fabs(sx / n) < 0.05 && std::fabs(sy / n) < 0.05, "jitter is centred over a cycle");
    const Vec2 ndc = jitter.offset_ndc(1, {960, 540}, {1920, 1080});
    const Vec2 px = jitter.offset_px(1, {960, 540}, {1920, 1080});
    expectNear(ndc.x, 2.0 * px.x / 960.0, 1e-7, "ndc x = 2 px / width");
    expectNear(ndc.y, -2.0 * px.y / 540.0, 1e-7, "ndc y = -2 px / height (+y down in pixels)");
    const up::ZeroJitterProvider zero;
    expectTrue(zero.phase_count({1, 1}, {1, 1}) == 1u && zero.offset_px(5, {1, 1}, {1, 1}).x == 0.f, "zero jitter");
}

/// A static frame at render resolution for the TAAU adapter (analytic scene, jittered, zero motion).
struct TemporalFrame {
    std::vector<Vec3> color;
    std::vector<f32> depth;
    std::vector<Vec2> motion;
    up::UpscaleInputs inputs{};

    TemporalFrame(up::Extent2D render, up::Extent2D display, Vec2 jitter, u32 frame, u32 phases) {
        color = ut::to_rgb(ut::render_scene(render.width, render.height, 4u, jitter.x, jitter.y));
        depth.assign(color.size(), 10.f);
        motion.assign(color.size(), Vec2(0.f, 0.f));
        inputs.resolution = up::make_resolution(render, display);
        inputs.color = color.data();
        inputs.depth = depth.data();
        inputs.motion = motion.data();
        inputs.jitter_px = jitter;
        inputs.jitter_phase = frame % phases;
        inputs.jitter_phase_count = phases;
        inputs.frame_index = frame;
    }
};

void testValidationAndTemporal() {
    const up::UpscalerRegistry& reg = up::UpscalerRegistry::instance();
    constexpr u32 kW = 32;
    constexpr u32 kH = 24;
    std::vector<Vec4> color = ut::render_scene(kW, kH, 2u);
    std::vector<Vec4> out(kW * kH);
    std::vector<Vec4> big(3u * kW * 3u * kH);

    // Spatial: ratio limits and missing colour.
    std::unique_ptr<up::IUpscaler> nisBase = reg.create(up::kNisName);
    auto* nis = dynamic_cast<up::ISpatialUpscaler*>(nisBase.get());
    std::unique_ptr<up::IUpscaler> casBase = reg.create(up::kCasName);
    auto* cas = dynamic_cast<up::ISpatialUpscaler*>(casBase.get());
    expectTrue(nis != nullptr && cas != nullptr && dynamic_cast<up::ITemporalUpscaler*>(nisBase.get()) == nullptr,
               "nis / cas implement ISpatialUpscaler only");
    if (nis == nullptr || cas == nullptr) {
        return;
    }
    up::SpatialUpscaleInputs in{};
    in.color = cview(color, kW, kH);
    up::UpscaleOutputs o{};
    o.color = view(big, 3u * kW, 3u * kH);
    expectTrue(nis->evaluate({}, in, o) == up::UpscaleStatus::UnsupportedRatio, "nis rejects 3x");
    expectTrue(!nis->supports({kW, kH}, {3u * kW, 3u * kH}) && nis->supports({kW, kH}, {2u * kW, 2u * kH}),
               "IUpscaler::supports ratio query");
    const up::SpatialUpscaleInputs empty{};
    expectTrue(nis->evaluate({}, empty, o) == up::UpscaleStatus::InvalidInputs, "missing colour rejected");
    expectTrue(cas->evaluate({}, in, o) == up::UpscaleStatus::UnsupportedRatio, "cas rejects 3x");
    o.color = view(out, kW, kH);
    expectTrue(cas->evaluate({}, in, o) == up::UpscaleStatus::Ok, "cas 1x ok");

    // Temporal adapter over the TAAU kernel: requirements, history generation, reset reasons.
    std::unique_ptr<up::IUpscaler> base = reg.create(up::kNativeTaauName);
    auto* taau = dynamic_cast<up::ITemporalUpscaler*>(base.get());
    expectTrue(taau != nullptr, "native_taau implements ITemporalUpscaler");
    if (taau == nullptr) {
        return;
    }
    const up::Extent2D render{kW, kH};
    const up::Extent2D display{2u * kW, 2u * kH};
    const u32 phases = taau->jitter_provider().phase_count(render, display);
    expectTrue(phases == 32u, "native_taau jitter provider: 32 phases at 2x");
    std::vector<Vec3> result(static_cast<size_t>(display.width) * display.height);
    const up::TemporalUpscaleOutputs to{{result.data(), display.width, display.height}};
    TemporalFrame f0(render, display, taau->jitter_provider().offset_px(0, render, display), 0, phases);
    up::UpscaleInputs missing = f0.inputs;
    missing.depth = nullptr;
    expectTrue(taau->evaluate({}, missing, to) == up::UpscaleStatus::InvalidInputs, "native_taau requires depth");
    missing = f0.inputs;
    missing.exposure = 0.f;
    expectTrue(taau->evaluate({}, missing, to) == up::UpscaleStatus::InvalidInputs, "native_taau requires exposure");
    std::vector<Vec3> wrong(10);
    expectTrue(taau->evaluate({}, f0.inputs, {{wrong.data(), 5, 2}}) == up::UpscaleStatus::InvalidInputs,
               "output must be display-sized");
    const u32 gen0 = taau->history_generation();
    expectTrue(taau->evaluate({}, f0.inputs, to) == up::UpscaleStatus::Ok, "native_taau 2x runs (TAAU kernel)");
    expectTrue(taau->history_generation() == gen0 + 1u &&
                   taau->last_reset_reason() == up::HistoryResetReason::ResolutionChange,
               "first frame invalidates history (resolution change)");
    TemporalFrame f1(render, display, taau->jitter_provider().offset_px(1, render, display), 1, phases);
    expectTrue(taau->evaluate({}, f1.inputs, to) == up::UpscaleStatus::Ok && taau->accumulated_frames() == 2u &&
                   taau->history_generation() == gen0 + 1u,
               "second frame accumulates without invalidation");
    f1.inputs.reset_history = true;
    expectTrue(taau->evaluate({}, f1.inputs, to) == up::UpscaleStatus::Ok && taau->history_generation() == gen0 + 2u &&
                   taau->last_reset_reason() == up::HistoryResetReason::CameraCut && taau->accumulated_frames() == 1u,
               "reset_history invalidates (camera cut)");
    taau->invalidate_history(up::HistoryResetReason::Teleport);
    expectTrue(taau->history_generation() == gen0 + 3u && taau->accumulated_frames() == 0u &&
                   taau->last_reset_reason() == up::HistoryResetReason::Teleport,
               "explicit invalidate_history");
    const up::Extent2D display3{3u * kW, 3u * kH};
    std::vector<Vec3> result3(static_cast<size_t>(display3.width) * display3.height);
    TemporalFrame f3(render, display3, Vec2(0.f, 0.f), 0, 72);
    expectTrue(taau->evaluate({}, f3.inputs, {{result3.data(), display3.width, display3.height}}) ==
                       up::UpscaleStatus::Ok &&
                   taau->last_reset_reason() == up::HistoryResetReason::ResolutionChange,
               "3x after 2x: resolution change resets history");
    const up::Extent2D display4{4u * kW, 4u * kH};
    std::vector<Vec3> result4(static_cast<size_t>(display4.width) * display4.height);
    TemporalFrame f4(render, display4, Vec2(0.f, 0.f), 0, 128);
    expectTrue(taau->evaluate({}, f4.inputs, {{result4.data(), display4.width, display4.height}}) ==
                   up::UpscaleStatus::UnsupportedRatio,
               "native_taau rejects 4x (max 3x)");
    expectTrue(std::strcmp(up::upscale_status_name(up::UpscaleStatus::UnsupportedRatio), "UnsupportedRatio") == 0 &&
                   std::strcmp(up::history_reset_reason_name(up::HistoryResetReason::CameraCut), "CameraCut") == 0,
               "status / reason names");
}

void testFsr1BackendComposition() {
    constexpr u32 kW = 40;
    constexpr u32 kH = 30;
    const std::vector<Vec4> src = ut::render_scene(kW, kH, 3u);
    std::vector<Vec4> viaBackend(60 * 45);
    std::vector<Vec4> easu(60 * 45);
    std::vector<Vec4> manual(60 * 45);
    std::unique_ptr<up::IUpscaler> base = up::UpscalerRegistry::instance().create(up::kFsr1Name);
    auto* fsr1 = dynamic_cast<up::ISpatialUpscaler*>(base.get());
    if (fsr1 == nullptr) {
        expectTrue(false, "fsr1 implements ISpatialUpscaler");
        return;
    }
    up::SpatialUpscaleInputs in{};
    in.color = cview(src, kW, kH);
    in.sharpness = 0.6f;
    up::UpscaleOutputs o{};
    o.color = view(viaBackend, 60, 45);
    expectTrue(fsr1->evaluate({kernel::Backend::CpuReference}, in, o) == up::UpscaleStatus::Ok, "fsr1 backend 1.5x");
    expectTrue(up::run_easu(cview(src, kW, kH), view(easu, 60, 45), {kernel::Backend::CpuReference}) &&
                   up::run_rcas(cview(easu, 60, 45), view(manual, 60, 45), up::rcas_stops_from_sharpness(0.6f),
                                {kernel::Backend::CpuReference}),
               "manual EASU + RCAS");
    expectTrue(std::memcmp(viaBackend.data(), manual.data(), manual.size() * sizeof(Vec4)) == 0,
               "fsr1 backend == run_easu + run_rcas (bitwise)");
    expectNear(up::rcas_stops_from_sharpness(1.f), 0.0, 0.0, "sharpness 1 -> 0 stops (max)");
    expectNear(up::rcas_stops_from_sharpness(0.f), 2.0, 0.0, "sharpness 0 -> 2 stops");
    expectTrue(!up::run_rcas(cview(src, kW, kH), view(manual, 60, 45), 0.f), "rcas rejects size mismatch");
    std::vector<Vec4> alias = src;
    expectTrue(!up::run_cas(cview(alias, kW, kH), view(alias, kW, kH), 0.5f), "cas rejects aliasing");
}

// ---- 2. Constant setup vs the vendored SDK host helpers ------------------------------------------------

void testConstantsMatchSdk() {
    const u32 sizes[][4] = {{1280, 720, 1920, 1080}, {1129, 635, 1920, 1080}, {640, 360, 1920, 1080},
                            {61, 43, 97, 71}, {1920, 1080, 1920, 1080}, {997, 541, 2560, 1440}};
    bool easuOk = true;
    bool casOk = true;
    for (const auto& s : sizes) {
        FfxUInt32x4 c0;
        FfxUInt32x4 c1;
        FfxUInt32x4 c2;
        FfxUInt32x4 c3;
        ffxFsrPopulateEasuConstants(c0, c1, c2, c3, static_cast<FfxFloat32>(s[0]), static_cast<FfxFloat32>(s[1]),
                                    static_cast<FfxFloat32>(s[0]), static_cast<FfxFloat32>(s[1]),
                                    static_cast<FfxFloat32>(s[2]), static_cast<FfxFloat32>(s[3]));
        const up::kernels::EasuConstants mine = up::easu_constants(s[0], s[1], s[2], s[3]);
        for (u32 i = 0; i < 4u; ++i) {
            easuOk = easuOk && mine.con0[i] == c0[i] && mine.con1[i] == c1[i] && mine.con2[i] == c2[i] &&
                     mine.con3[i] == c3[i];
        }
        for (const f32 sharp : {0.f, 0.2f, 0.5f, 1.f}) {
            FfxUInt32x4 k0;
            FfxUInt32x4 k1;
            ffxCasSetup(k0, k1, sharp, static_cast<FfxFloat32>(s[0]), static_cast<FfxFloat32>(s[1]),
                        static_cast<FfxFloat32>(s[2]), static_cast<FfxFloat32>(s[3]));
            const up::kernels::CasConstants cc = up::cas_constants(sharp, s[0], s[1], s[2], s[3]);
            for (u32 i = 0; i < 4u; ++i) {
                casOk = casOk && cc.const0[i] == k0[i];
            }
            casOk = casOk && cc.const1[0] == k1[0] && cc.const1[2] == k1[2];
        }
    }
    expectTrue(easuOk, "easu_constants == ffxFsrPopulateEasuConstants (bitwise, 6 size pairs)");
    expectTrue(casOk, "cas_constants == ffxCasSetup const0 / const1.x / const1.z (bitwise)");
    bool rcasOk = true;
    for (const f32 stops : {0.f, 0.2f, 0.5f, 1.f, 1.6f, 2.f}) {
        FfxUInt32x4 con;
        FsrRcasCon(con, stops);
        rcasOk = rcasOk && up::rcas_constants(stops).con[0] == con[0];
    }
    expectTrue(rcasOk, "rcas_constants == FsrRcasCon con.x (bitwise)");

    // NIS: the vendored NVScalerUpdateConfig is used directly; check the range guard and a known value.
    up::kernels::NisConstants k{};
    expectTrue(up::nis_constants(0.5f, 960, 540, 1920, 1080, k) && k.scale_x == 0.5f && k.scale_y == 0.5f &&
                   std::fabs(k.detect_ratio - 2.f * 1127.f / 1024.f) < 1e-6f,
               "nis_constants 2x");
    expectTrue(!up::nis_constants(0.5f, 640, 360, 1920, 1080, k), "nis_constants rejects 3x");
    expectTrue(up::nis_coef_scale().size() == 512u && up::nis_coef_scale()[2] == 1.0f &&
                   up::nis_coef_usm().size() == 512u,
               "NIS coefficient banks exposed (64 x 8)");
}

// ---- 3. Parity ----------------------------------------------------------------------------------------

struct PassRun {
    const char* name;
    u32 srcW;
    u32 srcH;
    u32 dstW;
    u32 dstH;
    bool (*run)(const up::ConstRgbaImage&, const up::RgbaImage&, const up::PassOptions&);
};

bool runEasu(const up::ConstRgbaImage& s, const up::RgbaImage& d, const up::PassOptions& o) { return up::run_easu(s, d, o); }
bool runRcas(const up::ConstRgbaImage& s, const up::RgbaImage& d, const up::PassOptions& o) {
    return up::run_rcas(s, d, 0.3f, o);
}
bool runCas(const up::ConstRgbaImage& s, const up::RgbaImage& d, const up::PassOptions& o) {
    return up::run_cas(s, d, 0.7f, o);
}
bool runCasDiag(const up::ConstRgbaImage& s, const up::RgbaImage& d, const up::PassOptions& o) {
    return up::run_cas(s, d, 0.4f, o, true, true);
}
bool runNis(const up::ConstRgbaImage& s, const up::RgbaImage& d, const up::PassOptions& o) {
    return up::run_nis(s, d, 0.5f, o);
}
bool runBilinear(const up::ConstRgbaImage& s, const up::RgbaImage& d, const up::PassOptions& o) {
    return up::run_bilinear(s, d, o);
}

void testParity() {
    const PassRun runs[] = {
        {"upscale_fsr1_easu", 61, 43, 97, 71, &runEasu},  {"upscale_fsr1_rcas", 97, 71, 97, 71, &runRcas},
        {"upscale_cas", 97, 71, 97, 71, &runCas},         {"upscale_cas", 97, 71, 97, 71, &runCasDiag},
        {"upscale_nis", 61, 43, 110, 80, &runNis},        {"upscale_bilinear", 61, 43, 97, 71, &runBilinear},
    };
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (const PassRun& r : runs) {
        // Scene detail + a noise overlay so every branch (edges, flat, black / white clamps) is hit.
        std::vector<Vec4> src = ut::render_scene(r.srcW, r.srcH, 2u);
        const std::vector<Vec4> noise = ut::noise_image(r.srcW, r.srcH, r.srcW * 31u + r.dstW);
        for (size_t i = 0; i < src.size(); i += 7u) {
            src[i] = noise[i];
        }
        scheduler.shutdown();
        std::vector<Vec4> ref(static_cast<size_t>(r.dstW) * r.dstH);
        kernel::reset_kernel_stats();
        expectTrue(r.run(cview(src, r.srcW, r.srcH), view(ref, r.dstW, r.dstH), {kernel::Backend::CpuReference}),
                   "reference launch");
        kernel::KernelStats stats{};
        expectTrue(kernel::find_kernel_stats(r.name, stats) && stats.items == static_cast<u64>(r.dstW) * r.dstH &&
                       stats.last_backend == kernel::Backend::CpuReference,
                   "kernel stats name / item count");
        bool finite = true;
        for (const Vec4& c : ref) {
            finite = finite && std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.w);
        }
        char label[192];
        std::snprintf(label, sizeof(label), "%s output finite", r.name);
        expectTrue(finite, label);
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            std::vector<Vec4> par(ref.size());
            expectTrue(r.run(cview(src, r.srcW, r.srcH), view(par, r.dstW, r.dstH), {kernel::Backend::CpuParallel}),
                       "parallel launch");
            const kernel::ParityReport report =
                kernel::compare_bitwise(std::span<const Vec4>(ref), std::span<const Vec4>(par));
            std::snprintf(label, sizeof(label), "%s %ux%u->%ux%u CpuReference == CpuParallel bit-exact (%u workers, %llu mismatches)",
                          r.name, r.srcW, r.srcH, r.dstW, r.dstH, workers,
                          static_cast<unsigned long long>(report.mismatches));
            expectTrue(report.ok && report.compared == ref.size(), label);
        }
        // GPU request without a device (or without an entry) falls back to CpuParallel.
        std::vector<Vec4> fb(ref.size());
        expectTrue(r.run(cview(src, r.srcW, r.srcH), view(fb, r.dstW, r.dstH), {kernel::Backend::Cuda}),
                   "cuda request runs");
        expectTrue(kernel::last_launch().backend == kernel::Backend::CpuParallel &&
                       std::memcmp(fb.data(), ref.data(), ref.size() * sizeof(Vec4)) == 0,
                   "Cuda without device falls back to CpuParallel, identical output");
    }
    scheduler.shutdown();
}

// ---- 4. Quality vs bilinear ---------------------------------------------------------------------------

struct QualityRow {
    std::string method;
    f32 ratio;
    f64 psnr;
    f64 ssim;
    f64 flip;
    f64 ms;
};

class Scorer {
public:
    explicit Scorer(const std::vector<Vec4>& truth, u32 width, u32 height)
        : m_truth(ut::to_rgb(truth)), m_width(width), m_height(height) {}

    QualityRow score(const char* method, f32 ratio, const std::vector<Vec3>& image, f64 ms) {
        QualityRow row{method, ratio, 0.0, 0.0, 1.0, ms};
        const quality::QualityImage test{image.data(), m_width, m_height};
        const quality::QualityImage ref{m_truth.data(), m_width, m_height};
        quality::PsnrResult p{};
        if (m_eval.psnr(test, ref, p)) {
            row.psnr = p.psnr;
        }
        row.ssim = m_eval.ssim(test, ref);
        row.flip = m_eval.flip(test, ref);
        return row;
    }
    QualityRow score(const char* method, f32 ratio, const std::vector<Vec4>& image, f64 ms) {
        return score(method, ratio, ut::to_rgb(image), ms);
    }

private:
    std::vector<Vec3> m_truth;
    u32 m_width;
    u32 m_height;
    quality::ImageQualityEvaluator m_eval{};
};

bool beats(const QualityRow& a, const QualityRow& b) { return a.psnr > b.psnr && a.ssim > b.ssim && a.flip < b.flip; }

f64 elapsed_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void testQuality(std::vector<QualityRow>& rows) {
    constexpr u32 kDisplayW = 384;
    constexpr u32 kDisplayH = 256;
    const up::Extent2D display{kDisplayW, kDisplayH};
    const std::vector<Vec4> truth = ut::render_scene(kDisplayW, kDisplayH, 6u);
    Scorer scorer(truth, kDisplayW, kDisplayH);
    const up::UpscalerRegistry& reg = up::UpscalerRegistry::instance();
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4u);
    char label[256];
    for (const up::QualityMode mode : {up::QualityMode::Quality, up::QualityMode::Balanced,
                                       up::QualityMode::Performance, up::QualityMode::UltraPerformance}) {
        const up::Extent2D render = up::render_extent(display, mode);
        const f32 ratio = up::quality_mode_ratio(mode);
        const std::vector<Vec4> input = ut::render_scene(render.width, render.height, 6u);
        const up::ConstRgbaImage in = cview(input, render.width, render.height);

        auto spatial = [&](const char* method, auto&& run) {
            std::vector<Vec4> out(static_cast<size_t>(kDisplayW) * kDisplayH);
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = run(view(out, kDisplayW, kDisplayH));
            const QualityRow row = scorer.score(method, ratio, out, elapsed_ms(t0));
            std::snprintf(label, sizeof(label), "%s %.1fx ran", method, ratio);
            expectTrue(ok, label);
            rows.push_back(row);
            return row;
        };
        auto backend = [&](const char* name, f32 sharpness) {
            return [&reg, &in, name, sharpness](const up::RgbaImage& o) {
                std::unique_ptr<up::IUpscaler> base = reg.create(name);
                auto* u = dynamic_cast<up::ISpatialUpscaler*>(base.get());
                return u != nullptr &&
                       u->evaluate({}, up::SpatialUpscaleInputs{in, sharpness}, up::UpscaleOutputs{o}) ==
                           up::UpscaleStatus::Ok;
            };
        };

        const QualityRow bil = spatial("bilinear", [&](const up::RgbaImage& o) { return up::run_bilinear(in, o); });
        const QualityRow easu = spatial("fsr1_easu", [&](const up::RgbaImage& o) { return up::run_easu(in, o); });
        const QualityRow fsr1 = spatial("fsr1", backend(up::kFsr1Name, 0.2f));
        std::snprintf(label, sizeof(label),
                      "%.1fx EASU beats bilinear (PSNR %.2f vs %.2f dB, SSIM %.4f vs %.4f, FLIP %.4f vs %.4f)", ratio,
                      easu.psnr, bil.psnr, easu.ssim, bil.ssim, easu.flip, bil.flip);
        expectTrue(beats(easu, bil), label);
        std::snprintf(label, sizeof(label),
                      "%.1fx FSR1 (EASU+RCAS) beats bilinear (PSNR %.2f vs %.2f dB, SSIM %.4f vs %.4f, FLIP %.4f vs %.4f)",
                      ratio, fsr1.psnr, bil.psnr, fsr1.ssim, bil.ssim, fsr1.flip, bil.flip);
        expectTrue(beats(fsr1, bil), label);
        QualityRow best = easu.psnr > fsr1.psnr ? easu : fsr1;
        if (ratio <= 2.f) {
            const QualityRow nis = spatial("nis", backend(up::kNisName, 0.5f));
            std::snprintf(label, sizeof(label),
                          "%.1fx NIS beats bilinear (PSNR %.2f vs %.2f dB, SSIM %.4f vs %.4f, FLIP %.4f vs %.4f)", ratio,
                          nis.psnr, bil.psnr, nis.ssim, bil.ssim, nis.flip, bil.flip);
            expectTrue(beats(nis, bil), label);
            best = nis.psnr > best.psnr ? nis : best;
        }

        // native_taau through the registry: a static camera, one jitter cycle of box-filtered jittered frames.
        std::unique_ptr<up::IUpscaler> base = reg.create(up::kNativeTaauName);
        auto* taau = dynamic_cast<up::ITemporalUpscaler*>(base.get());
        if (taau == nullptr) {
            expectTrue(false, "native_taau implements ITemporalUpscaler");
            continue;
        }
        const u32 phases = taau->jitter_provider().phase_count(render, display);
        std::vector<Vec3> out(static_cast<size_t>(kDisplayW) * kDisplayH);
        bool ok = true;
        f64 ms = 0.0;
        for (u32 frame = 0; frame < phases; ++frame) {
            TemporalFrame f(render, display, taau->jitter_provider().offset_px(frame, render, display), frame, phases);
            const auto t0 = std::chrono::steady_clock::now();
            ok = ok && taau->evaluate({}, f.inputs, {{out.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok;
            ms += elapsed_ms(t0);
        }
        const QualityRow t = scorer.score("native_taau", ratio, out, ms / static_cast<f64>(phases));
        rows.push_back(t);
        std::snprintf(label, sizeof(label),
                      "%.1fx native_taau (%u jittered frames) beats the best spatial backend %s (PSNR %.2f vs %.2f dB, "
                      "SSIM %.4f vs %.4f)",
                      ratio, phases, best.method.c_str(), t.psnr, best.psnr, t.ssim, best.ssim);
        expectTrue(ok && t.psnr > best.psnr && t.ssim > best.ssim, label);
    }

    // CAS: sharpening a softened image moves it towards the ground truth.
    std::vector<Vec4> soft(truth.size());
    for (u32 y = 0; y < kDisplayH; ++y) {
        for (u32 x = 0; x < kDisplayW; ++x) {
            f32 acc[3] = {0.f, 0.f, 0.f};
            f32 wsum = 0.f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const u32 sx = static_cast<u32>(std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(kDisplayW) - 1));
                    const u32 sy = static_cast<u32>(std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(kDisplayH) - 1));
                    const f32 w = (dx == 0 ? 2.f : 1.f) * (dy == 0 ? 2.f : 1.f);
                    const Vec4& c = truth[sy * kDisplayW + sx];
                    acc[0] += c.x * w;
                    acc[1] += c.y * w;
                    acc[2] += c.z * w;
                    wsum += w;
                }
            }
            soft[y * kDisplayW + x] = Vec4(acc[0] / wsum, acc[1] / wsum, acc[2] / wsum, 1.f);
        }
    }
    const QualityRow softRow = scorer.score("soft_input", 1.f, soft, 0.0);
    rows.push_back(softRow);
    for (const f32 sharp : {0.5f, 1.0f}) {
        std::vector<Vec4> sharpened(truth.size());
        const auto t0 = std::chrono::steady_clock::now();
        expectTrue(up::run_cas(cview(soft, kDisplayW, kDisplayH), view(sharpened, kDisplayW, kDisplayH), sharp),
                   "cas on soft image");
        rows.push_back(scorer.score(sharp < 1.f ? "cas_0.5" : "cas_1.0", 1.f, sharpened, elapsed_ms(t0)));
        std::snprintf(label, sizeof(label),
                      "CAS %.1f sharpens a softened image towards truth (PSNR %.2f vs %.2f dB, SSIM %.4f vs %.4f)", sharp,
                      rows.back().psnr, softRow.psnr, rows.back().ssim, softRow.ssim);
        expectTrue(rows.back().ssim > softRow.ssim && rows.back().psnr > softRow.psnr, label);
    }
    scheduler.shutdown();
}

void writeReport(const std::vector<QualityRow>& rows) {
    std::printf("\nupscale quality vs box-filtered ground truth (384x256 display, analytic scene)\n");
    std::printf("%-12s %6s %9s %8s %8s %9s\n", "method", "ratio", "PSNR(dB)", "SSIM", "FLIP", "time(ms)");
    for (const QualityRow& r : rows) {
        std::printf("%-12s %6.2f %9.3f %8.4f %8.4f %9.3f\n", r.method.c_str(), r.ratio, r.psnr, r.ssim, r.flip, r.ms);
    }
#ifdef FUSE_UPSCALE_REPORT_PATH
    if (FILE* f = std::fopen(FUSE_UPSCALE_REPORT_PATH, "wb")) {
        std::fprintf(f, "{\n  \"scene\": \"upscale_test_images.hpp analytic scene, 6x6 box filter\",\n");
        std::fprintf(f, "  \"display\": [384, 256],\n  \"rows\": [\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            const QualityRow& r = rows[i];
            std::fprintf(f,
                         "    {\"method\": \"%s\", \"ratio\": %.2f, \"psnr_db\": %.4f, \"ssim\": %.5f, \"flip\": %.5f, "
                         "\"ms\": %.3f}%s\n",
                         r.method.c_str(), r.ratio, r.psnr, r.ssim, r.flip, r.ms, i + 1 < rows.size() ? "," : "");
        }
        std::fprintf(f, "  ],\n  \"kernel_stats\": [\n");
        const u32 n = kernel::kernel_stats_count();
        bool first = true;
        for (u32 i = 0; i < n; ++i) {
            kernel::KernelStats st{};
            if (!kernel::kernel_stats_at(i, st) || st.name == nullptr ||
                (std::strncmp(st.name, "upscale_", 8) != 0 && std::strcmp(st.name, "taau") != 0)) {
                continue;
            }
            std::fprintf(f, "%s    {\"name\": \"%s\", \"launches\": %llu, \"items\": %llu, \"total_ms\": %.3f}",
                         first ? "" : ",\n", st.name, static_cast<unsigned long long>(st.launches),
                         static_cast<unsigned long long>(st.items), static_cast<f64>(st.total_ns) * 1e-6);
            first = false;
        }
        std::fprintf(f, "\n  ]\n}\n");
        std::fclose(f);
        std::printf("report: %s\n", FUSE_UPSCALE_REPORT_PATH);
    }
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    testQualityModes();
    testRegistry();
    testJitter();
    testValidationAndTemporal();
    testFsr1BackendComposition();
    testConstantsMatchSdk();
    testParity();
    std::vector<QualityRow> rows;
    kernel::reset_kernel_stats();
    testQuality(rows);
    writeReport(rows);
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_upscale_gates: all checks passed\n");
    return 0;
}
