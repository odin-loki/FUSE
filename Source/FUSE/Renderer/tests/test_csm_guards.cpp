#include <fuse/core/init.hpp>
#include <fuse/renderer/shadow/csm.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testSanitizeStagePredicates() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;

    CascadedShadowMapDesc validDesc{};
    expectTrue(!CascadedShadowMapLayout::needsCascadeSplitClamp(validDesc), "default splits need no clamp");
    expectTrue(!CascadedShadowMapLayout::needsCascadeSplitMonotonicityRepair(validDesc),
               "default splits need no monotonicity repair");
    expectTrue(!CascadedShadowMapLayout::needsLastCascadeSplitPin(validDesc), "default splits need no pin");
    expectTrue(!CascadedShadowMapLayout::needsCascadeSplitSanitize(validDesc), "default splits need no sanitize");

    CascadedShadowMapDesc outOfRangeDesc{};
    outOfRangeDesc.cascadeSplits[0] = -0.1f;
    outOfRangeDesc.cascadeSplits[1] = 1.2f;
    expectTrue(CascadedShadowMapLayout::needsCascadeSplitClamp(outOfRangeDesc), "out-of-range splits need clamp");
    expectTrue(CascadedShadowMapLayout::needsCascadeSplitSanitize(outOfRangeDesc), "out-of-range splits need sanitize");

    CascadedShadowMapDesc nonMonotonicDesc{};
    nonMonotonicDesc.cascadeSplits[0] = 0.4f;
    nonMonotonicDesc.cascadeSplits[1] = 0.2f;
    nonMonotonicDesc.cascadeSplits[2] = 0.8f;
    nonMonotonicDesc.cascadeSplits[3] = 1.f;
    expectTrue(!CascadedShadowMapLayout::needsCascadeSplitClamp(nonMonotonicDesc),
               "monotonic dip does not require clamp");
    expectTrue(CascadedShadowMapLayout::needsCascadeSplitMonotonicityRepair(nonMonotonicDesc),
               "descending split needs monotonicity repair");
    expectTrue(CascadedShadowMapLayout::needsCascadeSplitSanitize(nonMonotonicDesc),
               "descending split needs sanitize");

    CascadedShadowMapDesc unpinnedDesc{};
    unpinnedDesc.cascadeSplits[3] = 0.85f;
    expectTrue(CascadedShadowMapLayout::needsLastCascadeSplitPin(unpinnedDesc), "unpinned last split needs pin");
    expectTrue(CascadedShadowMapLayout::needsCascadeSplitSanitize(unpinnedDesc), "unpinned last split needs sanitize");
}

void testSanitizeNoOpWhenAlreadyValid() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;

    CascadedShadowMapDesc desc{};
    const CascadedShadowMapDesc before = desc;
    CascadedShadowMapLayout::sanitizeCascadeSplits(desc);

    for (fuse::u32 cascade = 0; cascade < fuse::renderer::kCascadeCount; ++cascade) {
        expectNear(desc.cascadeSplits[cascade], before.cascadeSplits[cascade], 0.0001f,
                   "sanitize is a no-op for already-valid splits");
    }
}

void testSplitBypassGuards() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeShadowBypassReason;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::cascadeShadowBypassReasonIsBlocking;
    using fuse::renderer::cascadeShadowBypassReasonLabel;

    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    expectTrue(!CascadeLightSpaceLayout::shouldBypassForEmptyLightDirection({0.f, -1.f, 0.f}),
               "valid light direction does not trigger empty-light bypass");
    expectTrue(CascadeLightSpaceLayout::shouldBypassForEmptyLightDirection({0.f, 0.f, 0.f}),
               "zero light direction triggers empty-light bypass");
    expectTrue(!CascadeLightSpaceLayout::shouldBypassForEmptyCameraDepthRange(camera),
               "valid camera depth range does not trigger empty-camera bypass");

    ShadowCameraParams invertedCamera = camera;
    invertedCamera.nearPlane = 80.f;
    invertedCamera.farPlane = 10.f;
    expectTrue(CascadedShadowMapLayout::isEmptyCameraDepthRange(invertedCamera),
               "inverted camera flagged empty before bypass guard");
    expectTrue(CascadeLightSpaceLayout::shouldBypassForEmptyCameraDepthRange(invertedCamera),
               "inverted camera triggers empty-camera bypass");

    expectTrue(CascadeLightSpaceLayout::classifyCascadeShadowBypass(camera, {0.f, 0.f, 0.f}) ==
                   CascadeShadowBypassReason::EmptyLightDirection,
               "empty light wins bypass classification");
    expectTrue(CascadeLightSpaceLayout::classifyCascadeShadowBypass(invertedCamera, {0.f, -1.f, 0.f}) ==
                   CascadeShadowBypassReason::EmptyCameraDepthRange,
               "empty camera wins when light direction is valid");
    expectTrue(CascadeLightSpaceLayout::classifyCascadeShadowBypass(camera, {0.f, -1.f, 0.f}) ==
                   CascadeShadowBypassReason::None,
               "valid inputs yield no bypass reason");

    expectTrue(std::strcmp(cascadeShadowBypassReasonLabel(CascadeShadowBypassReason::None), "none") == 0,
               "None bypass reason label");
    expectTrue(std::strcmp(cascadeShadowBypassReasonLabel(CascadeShadowBypassReason::EmptyLightDirection),
                           "empty_light_direction") == 0,
               "EmptyLightDirection bypass reason label");
    expectTrue(std::strcmp(cascadeShadowBypassReasonLabel(CascadeShadowBypassReason::EmptyCameraDepthRange),
                           "empty_camera_depth_range") == 0,
               "EmptyCameraDepthRange bypass reason label");
    expectTrue(!cascadeShadowBypassReasonIsBlocking(CascadeShadowBypassReason::None), "None bypass is not blocking");
    expectTrue(cascadeShadowBypassReasonIsBlocking(CascadeShadowBypassReason::EmptyLightDirection),
               "empty light bypass is blocking");
}

void testPerCascadeSkipGuards() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::math::Vec3 sunDirection{0.f, -1.f, 0.f};
    expectTrue(!CascadeLightSpaceLayout::isEmptyLightCascadeShadowGuard(sunDirection),
               "valid light passes empty-light guard");
    expectTrue(CascadeLightSpaceLayout::isEmptyLightCascadeShadowGuard({0.f, 0.f, 0.f}),
               "zero light fails empty-light guard");
    expectTrue(!CascadeLightSpaceLayout::isEmptyCameraCascadeShadowGuard(camera),
               "valid camera passes empty-camera guard");
    expectTrue(!CascadeLightSpaceLayout::isEmptyFrustumCascadeShadowGuard(0u, desc, camera),
               "default cascade passes empty-frustum guard");
    expectTrue(!CascadeLightSpaceLayout::isDegenerateRangeCascadeShadowGuard(0u, desc, camera),
               "default cascade passes degenerate-range guard");

    CascadedShadowMapDesc flatDesc{};
    flatDesc.cascadeSplits[0] = 0.5f;
    flatDesc.cascadeSplits[1] = 0.5f;
    flatDesc.cascadeSplits[2] = 1.f;
    flatDesc.cascadeSplits[3] = 1.f;
    expectTrue(CascadeLightSpaceLayout::isEmptyFrustumCascadeShadowGuard(1u, flatDesc, camera),
               "zero-thickness slice fails empty-frustum guard");
    expectTrue(CascadedShadowMapLayout::isEmptyCascadeFrustum(1u, flatDesc, camera),
               "empty-frustum guard matches layout helper");
}

void testWouldSkipCascadeShadowBuild() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeShadowSkipReason;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::cascadeShadowSkipReasonLabel;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::math::Vec3 sunDirection{0.f, -1.f, 0.f};
    fuse::renderer::CascadeShadowSkipReason skipReason = CascadeShadowSkipReason::None;
    expectTrue(!CascadeLightSpaceLayout::wouldSkipCascadeShadowBuild(0u, desc, camera, sunDirection, &skipReason),
               "valid cascade does not skip");
    expectTrue(skipReason == CascadeShadowSkipReason::None, "valid cascade reports None skip reason");

    expectTrue(CascadeLightSpaceLayout::wouldSkipCascadeShadowBuild(0u, desc, camera, {0.f, 0.f, 0.f}, &skipReason),
               "empty light direction skips build");
    expectTrue(skipReason == CascadeShadowSkipReason::EmptyLightDirection,
               "empty light skip reason returned by wouldSkip");

    expectTrue(std::strcmp(cascadeShadowSkipReasonLabel(CascadeShadowSkipReason::None), "none") == 0,
               "None skip reason label");
    expectTrue(std::strcmp(cascadeShadowSkipReasonLabel(CascadeShadowSkipReason::EmptyLightDirection),
                           "empty_light_direction") == 0,
               "EmptyLightDirection skip reason label");
    expectTrue(std::strcmp(cascadeShadowSkipReasonLabel(CascadeShadowSkipReason::EmptyCameraDepthRange),
                           "empty_camera_depth_range") == 0,
               "EmptyCameraDepthRange skip reason label");
    expectTrue(std::strcmp(cascadeShadowSkipReasonLabel(CascadeShadowSkipReason::EmptyCascadeFrustum),
                           "empty_cascade_frustum") == 0,
               "EmptyCascadeFrustum skip reason label");
    expectTrue(std::strcmp(cascadeShadowSkipReasonLabel(CascadeShadowSkipReason::DegenerateCascadeRange),
                           "degenerate_cascade_range") == 0,
               "DegenerateCascadeRange skip reason label");
}

} // namespace

int main() {
    fuse::core::initialize();

    testSanitizeStagePredicates();
    testSanitizeNoOpWhenAlreadyValid();
    testSplitBypassGuards();
    testPerCascadeSkipGuards();
    testWouldSkipCascadeShadowBuild();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_csm_guards: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_csm_guards: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
