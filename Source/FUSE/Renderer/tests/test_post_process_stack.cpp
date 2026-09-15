#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/auto_exposure.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#include <fuse/renderer/postprocess/tonemap_curve.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

void testBloomBlackFrameProducesZeroContribution() {
    const fuse::renderer::BloomParams params{};
    const fuse::math::Vec3 black{0.f, 0.f, 0.f};
    const fuse::math::Vec3 bright = fuse::renderer::Bloom::extractBright(black, params);
    expectTrue(bright.x == 0.f && bright.y == 0.f && bright.z == 0.f,
               "black frame produces zero bloom contribution");
}

void testAcesTonemapClamps() {
    const fuse::math::Vec3 hot{100.f, 50.f, 10.f};
    const fuse::math::Vec3 mapped = fuse::renderer::aces_tonemap(hot);
    expectTrue(mapped.x >= 0.f && mapped.x <= 1.f, "aces tonemap clamps red");
    expectTrue(mapped.y >= 0.f && mapped.y <= 1.f, "aces tonemap clamps green");
    expectTrue(mapped.z >= 0.f && mapped.z <= 1.f, "aces tonemap clamps blue");
}

void testNeutralCalibrationGrey() {
    fuse::renderer::ColorGradeParams params{};
    params.tone_mapper = fuse::renderer::ToneMapper::Neutral;
    params.output_srgb = false;
    params.exposure = 0.f;
    params.contrast = 1.f;
    params.saturation = 1.f;

    const fuse::math::Vec3 midGrey{0.18f, 0.18f, 0.18f};
    const fuse::math::Vec3 graded = fuse::renderer::apply_color_grade(midGrey, params);
    expectNear(graded.x, 0.18f, 1e-4f, "neutral grade preserves 0.18 grey");
    expectNear(graded.y, 0.18f, 1e-4f, "neutral grade preserves 0.18 grey g");
    expectNear(graded.z, 0.18f, 1e-4f, "neutral grade preserves 0.18 grey b");
}

void testPostStackStageChain() {
    expectTrue(fuse::renderer::PostStack::stageCount() == 3u, "post stack exposes three stages");
    expectTrue(std::string(fuse::renderer::PostStack::stageName(
                   fuse::renderer::PostProcessStage::Bloom)) == "bloom",
               "bloom stage name");
    expectTrue(std::string(fuse::renderer::PostStack::stageName(
                   fuse::renderer::PostProcessStage::ToneMap)) == "tonemap",
               "tonemap stage name");
    expectTrue(std::string(fuse::renderer::PostStack::stageName(
                   fuse::renderer::PostProcessStage::ColorGrade)) == "color_grade",
               "color grade stage name");
}

void testPostStackProcessPixel() {
    fuse::renderer::PostStack stack{};
    fuse::renderer::PostStackDesc desc{};
    desc.width = 1920;
    desc.height = 1080;
    stack.init(desc);

    expectTrue(stack.isReady(), "post stack ready after init");
    expectTrue(stack.bloom().isReady(), "bloom stage ready");
    expectTrue(stack.toneMap().isReady(), "tonemap stage ready");
    expectTrue(stack.tonemapCurve().isReady(), "tonemap curve stage ready");
    expectTrue(stack.autoExposure().isReady(), "auto exposure stage ready");
    expectTrue(stack.colorGrade().isReady(), "color grade stage ready");

    fuse::renderer::ColorGradeParams gradeParams{};
    gradeParams.tone_mapper = fuse::renderer::ToneMapper::ACES;
    gradeParams.output_srgb = false;
    stack.setColorGradeParams(gradeParams);

    const fuse::math::Vec3 hdr{0.5f, 0.25f, 0.1f};
    const fuse::math::Vec3 ldr = stack.processPixel(hdr, 42u);
    const fuse::renderer::PostStackStats stats = stack.lastStats();
    expectTrue(stats.bloom_ran && stats.tonemap_ran && stats.color_grade_ran,
               "post stack runs bloom → tonemap → color grade");
    expectTrue(ldr.x >= 0.f && ldr.x <= 1.f, "post stack output in range");

    stack.destroy();
    expectTrue(!stack.isReady(), "post stack not ready after destroy");
}

void testTonemapCurveDisabledIsIdentity() {
    fuse::renderer::TonemapCurveParams params{};
    params.enabled = false;
    const fuse::math::Vec3 input{0.4f, 0.2f, 0.1f};
    const fuse::math::Vec3 output = fuse::renderer::apply_tonemap_curve(input, params);
    expectNear(output.x, input.x, 1e-6f, "disabled curve preserves red");
    expectNear(output.y, input.y, 1e-6f, "disabled curve preserves green");
    expectNear(output.z, input.z, 1e-6f, "disabled curve preserves blue");
}

void testTonemapCurveEnabledCompressesHighlights() {
    fuse::renderer::TonemapCurveParams params{};
    params.enabled = true;
    params.shoulder_strength = 0.5f;
    params.shoulder_length = 0.4f;
    const fuse::math::Vec3 hot{8.f, 6.f, 4.f};
    const fuse::math::Vec3 curved = fuse::renderer::apply_tonemap_curve(hot, params);
    expectTrue(curved.x < hot.x, "enabled curve rolls off hot red");
    expectTrue(curved.x >= 0.f && curved.x <= 1.f, "curved red stays in range");
}

void testTonemapCurveReinhardAcesClamp() {
    const fuse::renderer::TonemapCurveParams reinhard = fuse::renderer::make_reinhard_curve_params();
    const fuse::renderer::TonemapCurveParams aces = fuse::renderer::make_aces_curve_params();
    const fuse::math::Vec3 extreme{128.f, 64.f, 32.f};

    const fuse::math::Vec3 reinhardCurved = fuse::renderer::apply_tonemap_curve(extreme, reinhard);
    const fuse::math::Vec3 acesCurved = fuse::renderer::apply_tonemap_curve(extreme, aces);

    expectTrue(reinhardCurved.x >= 0.f && reinhardCurved.x <= 1.f, "reinhard curve clamps red");
    expectTrue(reinhardCurved.y >= 0.f && reinhardCurved.y <= 1.f, "reinhard curve clamps green");
    expectTrue(reinhardCurved.z >= 0.f && reinhardCurved.z <= 1.f, "reinhard curve clamps blue");
    expectTrue(acesCurved.x >= 0.f && acesCurved.x <= 1.f, "aces curve clamps red");
    expectTrue(acesCurved.y >= 0.f && acesCurved.y <= 1.f, "aces curve clamps green");
    expectTrue(acesCurved.z >= 0.f && acesCurved.z <= 1.f, "aces curve clamps blue");
    expectTrue(reinhard.kind == fuse::renderer::TonemapCurveKind::Reinhard, "reinhard preset kind");
    expectTrue(aces.kind == fuse::renderer::TonemapCurveKind::ACES, "aces preset kind");
}

void testTonemapOperators() {
    const fuse::math::Vec3 grey{0.18f, 0.18f, 0.18f};
    const fuse::math::Vec3 aces = fuse::renderer::apply_tone_map(grey, fuse::renderer::ToneMapper::ACES);
    const fuse::math::Vec3 reinhard = fuse::renderer::apply_tone_map(grey, fuse::renderer::ToneMapper::Reinhard);
    const fuse::math::Vec3 neutral = fuse::renderer::apply_tone_map(grey, fuse::renderer::ToneMapper::Neutral);
    expectTrue(aces.x > 0.f && aces.x < 1.f, "aces maps mid grey into display range");
    expectTrue(reinhard.x > 0.f && reinhard.x < neutral.x, "reinhard compresses mid grey below neutral");
    expectNear(neutral.x, 0.18f, 1e-4f, "neutral preserves 0.18 grey");
    expectTrue(std::string(fuse::renderer::tone_mapper_name(fuse::renderer::ToneMapper::Filmic)) == "filmic",
               "filmic mapper name");
}

void testLuminanceToEvCalibration() {
    expectNear(fuse::renderer::luminance_to_ev(0.18f, 0.18f), 0.f, 1e-5f, "0.18 grey is 0 EV offset");
    expectTrue(fuse::renderer::luminance_to_ev(0.36f, 0.18f) > 0.f, "brighter scene yields positive EV");
    expectTrue(fuse::renderer::luminance_to_ev(0.09f, 0.18f) < 0.f, "darker scene yields negative EV");
}

void testAutoExposureEmaConverges() {
    fuse::renderer::AutoExposureParams params{};
    params.use_ema_adaptation = true;
    params.ema_alpha_up = 0.35f;
    params.ema_alpha_down = 0.35f;
    params.adaptation_speed_up = 12.f;
    params.adaptation_speed_down = 12.f;
    params.target_luminance = 0.18f;

    fuse::renderer::AutoExposureState state{};
    const fuse::f32 targetEv = fuse::renderer::luminance_to_ev(0.72f, 0.18f);
    fuse::f32 previousEv = fuse::renderer::update_auto_exposure_ema(state, 0.72f, params, 0.1f);
    for (int i = 0; i < 24; ++i) {
        const fuse::f32 nextEv = fuse::renderer::update_auto_exposure_ema(state, 0.72f, params, 0.1f);
        expectTrue(nextEv >= previousEv - 1e-4f, "ema exposure adapts upward without undershoot");
        previousEv = nextEv;
    }

    expectNear(previousEv, targetEv, 0.15f, "ema exposure settles near target EV");
    expectNear(state.smoothed_luminance, 0.72f, 0.05f, "ema smoothed luminance converges");
}

void testLuminanceHistogramEmpty() {
    fuse::renderer::LuminanceHistogram histogram{};
    fuse::renderer::LuminanceHistogramParams params{};
    histogram.init(params);

    expectTrue(histogram.sampleCount() == 0u, "empty histogram has zero samples");
    expectTrue(histogram.occupiedBinCount() == 0u, "empty histogram has no occupied bins");
    expectNear(histogram.averageLuminance(), 0.f, 1e-6f, "empty histogram average is zero");
    expectNear(histogram.percentileLuminance(0.5f), 0.f, 1e-6f, "empty histogram percentile is zero");
    expectNear(histogram.meteringLuminance(), 0.f, 1e-6f, "empty histogram metering is zero");
    expectNear(fuse::renderer::LuminanceHistogram::measureFromSamples(nullptr, 0u, params), 0.f, 1e-6f,
               "empty sample buffer yields zero metering");
}

void testAutoExposureClampsAndAdapts() {
    fuse::renderer::AutoExposureParams params{};
    params.min_ev = -2.f;
    params.max_ev = 2.f;
    params.target_luminance = 0.18f;
    params.adaptation_speed_up = 4.f;
    params.adaptation_speed_down = 4.f;

    fuse::renderer::AutoExposureState state{};
    const fuse::f32 first = fuse::renderer::update_auto_exposure(state, 0.72f, params, 0.25f);
    expectTrue(first > 0.f && first <= params.max_ev, "bright frame adapts upward within clamp");
    expectNear(state.measured_luminance, 0.72f, 1e-6f, "state stores measured luminance");

    const fuse::f32 second = fuse::renderer::update_auto_exposure(state, 0.045f, params, 0.25f);
    expectTrue(second < first, "dark frame pulls exposure down");
    expectTrue(second >= params.min_ev, "exposure respects minimum clamp");
}

void testExposureMeterAverage() {
    const fuse::math::Vec3 samples[] = {{0.18f, 0.18f, 0.18f}, {0.36f, 0.36f, 0.36f}};
    const fuse::f32 average = fuse::renderer::ExposureMeter::measureAverage(samples, 2u);
    expectNear(average, 0.27f, 1e-4f, "meter averages rec709 luminance");
}

void testPostStackAutoExposureIntegration() {
    fuse::renderer::PostStack stack{};
    stack.init({});

    fuse::renderer::AutoExposureParams autoParams{};
    autoParams.enabled = true;
    autoParams.adaptation_speed_up = 8.f;
    autoParams.adaptation_speed_down = 8.f;
    stack.setAutoExposureParams(autoParams);

    const fuse::math::Vec3 brightFrame[] = {{1.f, 1.f, 1.f}, {0.8f, 0.8f, 0.8f}};
    const fuse::f32 ev = stack.updateAutoExposure(brightFrame, 2u, 0.5f);
    expectTrue(ev > 0.f, "post stack auto exposure adapts to bright samples");

    const fuse::math::Vec3 ldr = stack.processPixel({0.5f, 0.5f, 0.5f}, 0u);
    const fuse::renderer::PostStackStats stats = stack.lastStats();
    expectNear(stats.auto_exposure_ev, ev, 1e-5f, "process pixel reports active auto EV");
    expectTrue(ldr.x >= 0.f && ldr.x <= 1.f, "auto exposure path stays in display range");

    stack.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();

    testBloomBlackFrameProducesZeroContribution();
    testAcesTonemapClamps();
    testNeutralCalibrationGrey();
    testPostStackStageChain();
    testPostStackProcessPixel();
    testTonemapCurveDisabledIsIdentity();
    testTonemapCurveEnabledCompressesHighlights();
    testTonemapCurveReinhardAcesClamp();
    testTonemapOperators();
    testLuminanceToEvCalibration();
    testAutoExposureEmaConverges();
    testLuminanceHistogramEmpty();
    testAutoExposureClampsAndAdapts();
    testExposureMeterAverage();
    testPostStackAutoExposureIntegration();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_post_process_b510: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_post_process_b510: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
