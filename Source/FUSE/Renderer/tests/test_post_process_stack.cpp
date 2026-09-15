#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>

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

} // namespace

int main() {
    fuse::core::initialize();

    testBloomBlackFrameProducesZeroContribution();
    testAcesTonemapClamps();
    testNeutralCalibrationGrey();
    testPostStackStageChain();
    testPostStackProcessPixel();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_post_process_b510: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_post_process_b510: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
