#include <fuse/core/init.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/shadow/csm.hpp>
#include <fuse/renderer/shadow/directional_shadow.hpp>
#include <fuse/renderer/shadow/shadow_atlas.hpp>
#include <fuse/renderer/shadow/shadow_pass.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void testCascadeLayout() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::GpuFormat;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::kCascadeCount;

    expectTrue(CascadedShadowMapLayout::cascadeCount() == 4u, "four cascades");
    expectTrue(CascadedShadowMapLayout::depthFormat() == GpuFormat::R32Sfloat, "R32F depth maps");

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.f;

    expectTrue(CascadedShadowMapLayout::validateCascadeSplits(desc), "default splits monotonic");

    expectNear(CascadedShadowMapLayout::computeCascadeNearZ(0u, desc, camera), 0.1f, 0.001f,
               "first cascade near equals camera near");
    expectNear(CascadedShadowMapLayout::computeCascadeNearZ(2u, desc, camera),
               CascadedShadowMapLayout::computeCascadeFarZ(1u, desc, camera), 0.001f,
               "cascade near chains from previous far");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(0u, desc, camera), 5.095f, 0.01f,
               "first cascade split");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(1u, desc, camera), 15.085f, 0.01f,
               "second cascade split");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(2u, desc, camera), 40.06f, 0.01f,
               "third cascade split");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(3u, desc, camera), 100.f, 0.01f,
               "last cascade reaches far plane");
    expectTrue(desc.cascadeSplits[kCascadeCount - 1u] == 1.0f, "final split reaches far plane fraction");
}

void testCascadeSplitValidation() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;

    CascadedShadowMapDesc validDesc{};
    expectTrue(CascadedShadowMapLayout::validateCascadeSplits(validDesc), "default splits valid");

    CascadedShadowMapDesc invalidDesc{};
    invalidDesc.cascadeSplits[1] = 0.01f;
    expectTrue(!CascadedShadowMapLayout::validateCascadeSplits(invalidDesc), "non-monotonic splits rejected");

    CascadedShadowMapDesc truncatedDesc{};
    truncatedDesc.cascadeSplits[3] = 0.9f;
    expectTrue(!CascadedShadowMapLayout::validateCascadeSplits(truncatedDesc),
               "final split must reach 1.0");
}

void testBatchCascadeFarZs() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 50.f;

    fuse::f32 farZs[fuse::renderer::kCascadeCount]{};
    CascadedShadowMapLayout::computeCascadeFarZs(desc, camera, farZs);

    expectNear(farZs[0], CascadedShadowMapLayout::computeCascadeFarZ(0u, desc, camera), 0.001f,
               "batch far z matches scalar helper");
    expectNear(farZs[3], 50.f, 0.001f, "batch last cascade reaches far plane");
    expectTrue(farZs[1] > farZs[0] && farZs[2] > farZs[1] && farZs[3] > farZs[2],
               "batch far zs monotonic");
}

void testBatchCascadeNearZsAndRanges() {
    using fuse::renderer::CascadeRange;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 200.f;

    fuse::f32 nearZs[fuse::renderer::kCascadeCount]{};
    CascadedShadowMapLayout::computeCascadeNearZs(desc, camera, nearZs);
    expectNear(nearZs[0], 1.f, 0.001f, "batch first near equals camera near");
    expectNear(nearZs[2], CascadedShadowMapLayout::computeCascadeFarZ(1u, desc, camera), 0.001f,
               "batch near chains from previous far");

    CascadeRange ranges[fuse::renderer::kCascadeCount]{};
    CascadedShadowMapLayout::computeCascadeRanges(desc, camera, ranges);
    expectTrue(CascadedShadowMapLayout::validateCascadeRanges(desc, camera), "default cascade ranges valid");
    expectTrue(ranges[0].nearZ < ranges[0].farZ, "first cascade range ordered");
    expectNear(ranges[1].nearZ, ranges[0].farZ, 0.001f, "cascade ranges chain without gaps");
}

void testCascadeFrustumCorners() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {0.f, 0.f, 0.f};
    camera.forward = {0.f, 0.f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.f;
    camera.fovDegrees = 90.f;
    camera.aspect = 1.f;

    const auto corners = CascadedShadowMapLayout::buildCascadeFrustumCorners(0u, desc, camera);
    const fuse::f32 nearZ = CascadedShadowMapLayout::computeCascadeNearZ(0u, desc, camera);
    expectNear(corners.corners[0].z, -nearZ, 0.01f, "near corner depth matches cascade near");
    expectTrue(corners.corners[0].x < corners.corners[1].x, "near corners span horizontal axis");
    expectTrue(corners.corners[4].z < corners.corners[0].z, "far corners deeper than near corners");
}

void testCascadeSplitSchemes() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 500.f;

    CascadeSplitParams uniformParams{};
    uniformParams.scheme = CascadeSplitScheme::Uniform;
    uniformParams.cascadeCount = 4u;

    fuse::f32 uniformFractions[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitFractions(uniformParams, camera, uniformFractions);
    expectTrue(CascadedShadowMapLayout::validateSplitMonotonicity(uniformFractions, 4u),
               "uniform split fractions monotonic");
    expectNear(uniformFractions[0], 0.25f, 0.001f, "uniform first split at 25%");
    expectNear(uniformFractions[3], 1.f, 0.001f, "uniform last split reaches far");

    CascadeSplitParams logParams{};
    logParams.scheme = CascadeSplitScheme::Logarithmic;
    logParams.cascadeCount = 4u;

    fuse::f32 logFractions[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitFractions(logParams, camera, logFractions);
    expectTrue(CascadedShadowMapLayout::validateSplitMonotonicity(logFractions, 4u),
               "logarithmic split fractions monotonic");
    expectTrue(logFractions[0] < uniformFractions[0], "log first split closer than uniform");

    CascadeSplitParams practicalParams{};
    practicalParams.scheme = CascadeSplitScheme::Practical;
    practicalParams.lambda = 0.75f;
    practicalParams.cascadeCount = 4u;

    fuse::f32 practicalFractions[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitFractions(practicalParams, camera, practicalFractions);
    expectTrue(CascadedShadowMapLayout::validateSplitMonotonicity(practicalFractions, 4u),
               "practical split fractions monotonic");
    expectTrue(practicalFractions[0] > logFractions[0] && practicalFractions[0] < uniformFractions[0],
               "practical first split between log and uniform");

    CascadedShadowMapDesc populatedDesc{};
    CascadedShadowMapLayout::populateCascadeSplits(practicalParams, camera, populatedDesc);
    expectTrue(CascadedShadowMapLayout::validateCascadeSplits(populatedDesc),
               "populated practical splits pass cascade validation");
}

void testCascadeCountClamp() {
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::kMaxCascadeCount;
    using fuse::renderer::kMinCascadeCount;

    expectTrue(CascadedShadowMapLayout::clampCascadeCount(0u) == kMinCascadeCount, "zero cascades clamped up");
    expectTrue(CascadedShadowMapLayout::clampCascadeCount(99u) == kMaxCascadeCount, "excess cascades clamped down");
    expectTrue(CascadedShadowMapLayout::clampCascadeCount(3u) == 3u, "in-range cascade count preserved");
    expectTrue(CascadedShadowMapLayout::clampCascadeIndex(9u, 3u) == 2u, "cascade index clamped to count-1");
}

void testBatchSplitDistances() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    CascadeSplitParams params{};
    params.scheme = CascadeSplitScheme::Uniform;
    params.cascadeCount = 4u;

    fuse::f32 distances[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitDistances(params, camera, distances);

    expectNear(distances[0], CascadedShadowMapLayout::computeSplitDistance(0u, params, camera), 0.001f,
               "batch split distance matches scalar helper");
    expectNear(distances[3], 100.f, 0.001f, "batch last split reaches far plane");
    expectTrue(distances[1] > distances[0] && distances[2] > distances[1] && distances[3] > distances[2],
               "batch split distances monotonic");
}

void testVariableCascadeCount() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 0.25f;
    camera.farPlane = 80.f;

    CascadeSplitParams twoCascadeParams{};
    twoCascadeParams.scheme = CascadeSplitScheme::Uniform;
    twoCascadeParams.cascadeCount = 2u;

    fuse::f32 twoFractions[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitFractions(twoCascadeParams, camera, twoFractions);
    expectTrue(CascadedShadowMapLayout::validateSplitMonotonicity(twoFractions, 2u),
               "two-cascade split fractions monotonic");
    expectNear(twoFractions[0], 0.5f, 0.001f, "two-cascade first split at midpoint");
    expectNear(twoFractions[1], 1.f, 0.001f, "two-cascade last split reaches far");

    CascadedShadowMapDesc populatedDesc{};
    CascadedShadowMapLayout::populateCascadeSplits(twoCascadeParams, camera, populatedDesc);
    const fuse::renderer::CascadeRange firstRange =
        CascadedShadowMapLayout::computeCascadeRange(0u, populatedDesc, camera);
    const fuse::renderer::CascadeRange secondRange =
        CascadedShadowMapLayout::computeCascadeRange(1u, populatedDesc, camera);
    expectTrue(firstRange.nearZ < firstRange.farZ, "two-cascade first range ordered");
    expectNear(secondRange.nearZ, firstRange.farZ, 0.001f, "two-cascade ranges chain without gaps");

    CascadeSplitParams threeCascadeParams{};
    threeCascadeParams.scheme = CascadeSplitScheme::Logarithmic;
    threeCascadeParams.cascadeCount = 3u;

    fuse::f32 threeDistances[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitDistances(threeCascadeParams, camera, threeDistances);
    expectTrue(threeDistances[2] > threeDistances[1] && threeDistances[1] > threeDistances[0],
               "three-cascade split distances monotonic");
    expectNear(threeDistances[2], 80.f, 0.001f, "three-cascade last split reaches far plane");
}

void testValidateSplitDistances() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 200.f;

    CascadeSplitParams params{};
    params.scheme = CascadeSplitScheme::Practical;
    params.lambda = 0.5f;
    params.cascadeCount = 4u;

    fuse::f32 distances[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitDistances(params, camera, distances);
    expectTrue(CascadedShadowMapLayout::validateSplitDistances(distances, 4u, camera),
               "practical split distances pass distance validation");

    fuse::f32 invalidDistances[fuse::renderer::kMaxCascadeCount]{50.f, 40.f, 120.f, 200.f};
    expectTrue(!CascadedShadowMapLayout::validateSplitDistances(invalidDistances, 4u, camera),
               "non-monotonic split distances rejected");

    fuse::f32 truncatedDistances[fuse::renderer::kMaxCascadeCount]{50.f, 100.f, 150.f, 180.f};
    expectTrue(!CascadedShadowMapLayout::validateSplitDistances(truncatedDistances, 4u, camera),
               "split distances must reach far plane");
}

void testSingleCascadeCount() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 2.f;
    camera.farPlane = 60.f;

    CascadeSplitParams singleParams{};
    singleParams.scheme = CascadeSplitScheme::Uniform;
    singleParams.cascadeCount = 1u;

    fuse::f32 fractions[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitFractions(singleParams, camera, fractions);
    expectTrue(CascadedShadowMapLayout::validateSplitMonotonicity(fractions, 1u),
               "single-cascade split fractions valid");
    expectNear(fractions[0], 1.f, 0.001f, "single cascade covers full depth range");

    fuse::f32 distances[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitDistances(singleParams, camera, distances);
    expectTrue(CascadedShadowMapLayout::validateSplitDistances(distances, 1u, camera),
               "single-cascade split distances valid");
    expectNear(distances[0], 60.f, 0.001f, "single cascade split reaches far plane");

    CascadedShadowMapDesc populatedDesc{};
    CascadedShadowMapLayout::populateCascadeSplits(singleParams, camera, populatedDesc);
    const fuse::renderer::CascadeRange range =
        CascadedShadowMapLayout::computeCascadeRange(0u, populatedDesc, camera);
    expectNear(range.nearZ, 2.f, 0.001f, "single cascade near equals camera near");
    expectNear(range.farZ, 60.f, 0.001f, "single cascade far equals camera far");
    expectTrue(!CascadedShadowMapLayout::isEmptyCascadeFrustum(0u, populatedDesc, camera),
               "single-cascade frustum is non-empty");
}

void testIsEmptyCascadeFrustum() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 5.f;
    camera.farPlane = 100.f;

    expectTrue(!CascadedShadowMapLayout::isEmptyCascadeFrustum(0u, desc, camera),
               "default cascade frustum is non-empty");

    ShadowCameraParams invertedCamera = camera;
    invertedCamera.nearPlane = 50.f;
    invertedCamera.farPlane = 10.f;
    expectTrue(CascadedShadowMapLayout::isEmptyCascadeFrustum(0u, desc, invertedCamera),
               "inverted camera planes yield empty frustum");

    CascadedShadowMapDesc flatDesc{};
    flatDesc.cascadeSplits[0] = 0.5f;
    flatDesc.cascadeSplits[1] = 0.5f;
    flatDesc.cascadeSplits[2] = 1.f;
    flatDesc.cascadeSplits[3] = 1.f;
    expectTrue(CascadedShadowMapLayout::isEmptyCascadeFrustum(1u, flatDesc, camera),
               "zero-thickness cascade slice flagged empty");
}

void testSplitNearDistances() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 200.f;

    CascadeSplitParams params{};
    params.scheme = CascadeSplitScheme::Uniform;
    params.cascadeCount = 4u;

    fuse::f32 nearDistances[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitNearDistances(params, camera, nearDistances);

    expectNear(nearDistances[0], camera.nearPlane, 0.001f, "first split near equals camera near");
    expectNear(nearDistances[1], CascadedShadowMapLayout::computeSplitDistance(0u, params, camera), 0.001f,
               "second split near chains from first far");
    expectNear(nearDistances[3], CascadedShadowMapLayout::computeSplitDistance(2u, params, camera), 0.001f,
               "last split near chains from previous far");

    fuse::f32 splitDistances[fuse::renderer::kMaxCascadeCount]{};
    CascadedShadowMapLayout::computeSplitDistances(params, camera, splitDistances);
    for (fuse::u32 cascade = 1; cascade < 4u; ++cascade) {
        expectNear(nearDistances[cascade], splitDistances[cascade - 1u], 0.001f,
                   "split near distance matches previous split far");
    }
}

void testCountNonEmptyCascadeFrustums() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    expectTrue(CascadedShadowMapLayout::countNonEmptyCascadeFrustums(desc, camera, 4u) == 4u,
               "default four cascades are non-empty");

    ShadowCameraParams invertedCamera = camera;
    invertedCamera.nearPlane = 50.f;
    invertedCamera.farPlane = 10.f;
    expectTrue(CascadedShadowMapLayout::countNonEmptyCascadeFrustums(desc, invertedCamera, 4u) == 0u,
               "inverted camera yields zero non-empty cascades");

    CascadedShadowMapDesc flatDesc{};
    flatDesc.cascadeSplits[0] = 0.5f;
    flatDesc.cascadeSplits[1] = 0.5f;
    flatDesc.cascadeSplits[2] = 1.f;
    flatDesc.cascadeSplits[3] = 1.f;
    expectTrue(CascadedShadowMapLayout::countNonEmptyCascadeFrustums(flatDesc, camera, 4u) == 2u,
               "zero-thickness cascades excluded from non-empty count");
}

void testOrthoBoundsContainLightSpaceAabb() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeOrthoBounds;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {0.f, 3.f, 10.f};
    camera.forward = {0.f, -0.2f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 120.f;
    camera.fovDegrees = 70.f;
    camera.aspect = 1.5f;

    const fuse::math::Vec3 sunDirection{-0.3f, -1.f, -0.2f};
    const auto fitted =
        CascadeLightSpaceLayout::fitOrthoBoundsFromCascadeFrustum(0u, desc, camera, sunDirection);
    const auto aabb = CascadeLightSpaceLayout::computeCascadeLightSpaceAabb(0u, desc, camera, sunDirection);

    expectTrue(CascadeLightSpaceLayout::validateOrthoBounds(fitted), "fitted ortho bounds valid");
    expectTrue(CascadeLightSpaceLayout::orthoBoundsContainsLightSpaceAabb(fitted, aabb),
               "fitted ortho bounds contain source light-space aabb");

    const CascadeOrthoBounds stabilised = CascadeLightSpaceLayout::stabiliseOrthoExtents(fitted, 1024u, true);
    expectTrue(CascadeLightSpaceLayout::orthoBoundsContainsLightSpaceAabb(stabilised, aabb),
               "stabilised ortho bounds still contain source light-space aabb");

    const auto corners = CascadedShadowMapLayout::buildCascadeFrustumCorners(0u, desc, camera);
    const fuse::renderer::CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(0u, desc, camera);
    const fuse::math::Vec3 focus =
        camera.position + camera.forward.normalized() * ((range.nearZ + range.farZ) * 0.5f);
    const fuse::math::Mat4 lightView = CascadeLightSpaceLayout::buildLightView(focus, sunDirection);
    const auto tightAabb = CascadeLightSpaceLayout::computeLightSpaceAabb(corners, lightView);
    const CascadeOrthoBounds tooSmall{tightAabb.min.x + 1.f, tightAabb.max.x - 1.f, tightAabb.min.y + 1.f,
                                      tightAabb.max.y - 1.f, -tightAabb.max.z, -tightAabb.min.z};
    expectTrue(!CascadeLightSpaceLayout::orthoBoundsContainsLightSpaceAabb(tooSmall, tightAabb),
               "undersized ortho bounds rejected");
}

void testShadowMat4IsPopulated() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::ShadowMat4;

    expectTrue(!CascadeLightSpaceLayout::shadowMat4IsPopulated(ShadowMat4::identity()),
               "identity shadow matrix is not populated");

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {1.f, 5.f, 12.f};
    camera.forward = {0.f, -0.25f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 150.f;

    const auto matrices =
        CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(0u, desc, camera, {-0.2f, -1.f, -0.1f});
    expectTrue(matrices.valid, "cascade matrices built for population check");
    expectTrue(CascadeLightSpaceLayout::shadowMat4IsPopulated(matrices.lightViewProj),
               "valid cascade view-projection is populated");
}

void testVariableCascadeCountBatchMatrices() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::kCascadeCount;

    ShadowCameraParams camera{};
    camera.position = {0.f, 4.f, 8.f};
    camera.forward = {0.f, -0.1f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 120.f;
    camera.fovDegrees = 65.f;
    camera.aspect = 1.333f;

    CascadeSplitParams twoCascadeParams{};
    twoCascadeParams.scheme = CascadeSplitScheme::Uniform;
    twoCascadeParams.cascadeCount = 2u;

    CascadedShadowMapDesc desc{};
    CascadedShadowMapLayout::populateCascadeSplits(twoCascadeParams, camera, desc);

    const fuse::math::Vec3 sunDirection{-0.25f, -1.f, -0.2f};
    fuse::renderer::CascadeLightSpaceMatrices matrices[kCascadeCount]{};
    const fuse::u32 validCount =
        CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(desc, camera, sunDirection, 2u, matrices);

    expectTrue(validCount == 2u, "two-cascade batch builds two valid matrices");
    expectTrue(matrices[0].valid && matrices[1].valid, "first two cascade slots valid");
    expectTrue(!matrices[2].valid && !matrices[3].valid, "inactive cascade slots cleared");
    expectTrue(CascadeLightSpaceLayout::shadowMat4IsPopulated(matrices[0].lightViewProj),
               "two-cascade batch populates view-projection");
}

void testBuildAllCascadeLightSpaceMatrices() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::kCascadeCount;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {0.f, 4.f, 8.f};
    camera.forward = {0.f, -0.1f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 120.f;
    camera.fovDegrees = 65.f;
    camera.aspect = 1.333f;

    const fuse::math::Vec3 sunDirection{-0.25f, -1.f, -0.2f};
    fuse::renderer::CascadeLightSpaceMatrices matrices[fuse::renderer::kCascadeCount]{};
    const fuse::u32 validCount =
        CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(desc, camera, sunDirection, matrices);

    expectTrue(validCount == kCascadeCount, "all default cascades produce valid matrices");
    for (fuse::u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        expectTrue(matrices[cascade].valid, "batch-built cascade matrix valid");
        expectTrue(CascadeLightSpaceLayout::validateOrthoBounds(matrices[cascade].orthoBounds),
                   "batch-built ortho bounds valid");
    }

    const fuse::u32 degenerateCount = CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(
        desc, camera, {0.f, 0.f, 0.f}, matrices);
    expectTrue(degenerateCount == 0u, "zero light direction yields no valid matrices");
}

void testValidateOrthoBounds() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeOrthoBounds;

    const CascadeOrthoBounds valid{-5.f, 5.f, -3.f, 3.f, 1.f, 10.f};
    expectTrue(CascadeLightSpaceLayout::validateOrthoBounds(valid), "positive ortho extents valid");

    const CascadeOrthoBounds flat{0.f, 0.f, -1.f, 1.f, 0.f, 5.f};
    expectTrue(!CascadeLightSpaceLayout::validateOrthoBounds(flat), "zero-width ortho bounds rejected");

    const fuse::math::AABB emptyAabb{{1.f, 1.f, 1.f}, {-1.f, -1.f, -1.f}};
    const CascadeOrthoBounds emptyFit = CascadeLightSpaceLayout::fitOrthoBoundsFromLightSpaceAabb(emptyAabb);
    expectTrue(!CascadeLightSpaceLayout::validateOrthoBounds(emptyFit), "empty aabb fit rejected");
}

void testLightSpaceAabbContainsCorners() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {2.f, 8.f, 12.f};
    camera.forward = {0.f, -0.3f, -1.f};
    camera.nearPlane = 0.2f;
    camera.farPlane = 150.f;
    camera.fovDegrees = 75.f;
    camera.aspect = 1.777f;

    const auto corners = CascadedShadowMapLayout::buildCascadeFrustumCorners(1u, desc, camera);
    const fuse::renderer::CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(1u, desc, camera);
    const fuse::math::Vec3 focus =
        camera.position + camera.forward.normalized() * ((range.nearZ + range.farZ) * 0.5f);
    const fuse::math::Vec3 sunDirection{-0.4f, -1.f, -0.1f};
    const fuse::math::Mat4 lightView = CascadeLightSpaceLayout::buildLightView(focus, sunDirection);
    const auto aabb = CascadeLightSpaceLayout::computeLightSpaceAabbFromWorldCorners(corners.corners, lightView);

    expectTrue(!CascadeLightSpaceLayout::isEmptyLightSpaceAabb(aabb), "valid frustum yields non-empty aabb");
    for (fuse::u32 cornerIdx = 0; cornerIdx < 8u; ++cornerIdx) {
        const fuse::math::Vec3 lightSpace = fuse::math::transformPoint(lightView, corners.corners[cornerIdx]);
        expectTrue(aabb.contains(lightSpace), "light-space aabb contains transformed frustum corner");
    }
}

void testEmptyFrustumLightSpaceAabb() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 10.f;
    camera.farPlane = 5.f;

    const auto aabb = CascadeLightSpaceLayout::computeCascadeLightSpaceAabb(0u, desc, camera, {0.f, -1.f, 0.f});
    expectTrue(CascadeLightSpaceLayout::isEmptyLightSpaceAabb(aabb), "inverted near/far yields empty aabb");

    fuse::math::Vec3 collapsedCorners[8]{};
    for (fuse::u32 i = 0; i < 8u; ++i) {
        collapsedCorners[i] = {3.f, 4.f, 5.f};
    }
    const fuse::math::Mat4 lightView = CascadeLightSpaceLayout::buildLightView({3.f, 4.f, 5.f}, {0.f, -1.f, 0.f});
    const auto pointAabb =
        CascadeLightSpaceLayout::computeLightSpaceAabbFromWorldCorners(collapsedCorners, lightView);
    expectTrue(!CascadeLightSpaceLayout::isEmptyLightSpaceAabb(pointAabb), "degenerate point frustum is valid");
    expectNear(pointAabb.min.x, pointAabb.max.x, 0.001f, "degenerate frustum aabb has zero extent");
}

void testOrthoBoundsFitAndStabilisation() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeOrthoBounds;

    const fuse::math::AABB aabb{{-4.f, -2.f, -8.f}, {6.f, 3.f, -1.f}};
    const CascadeOrthoBounds fitted = CascadeLightSpaceLayout::fitOrthoBoundsFromLightSpaceAabb(aabb);
    expectNear(fitted.left, -4.f, 0.001f, "fitted ortho left from aabb");
    expectNear(fitted.right, 6.f, 0.001f, "fitted ortho right from aabb");
    expectNear(fitted.nearPlane, 1.f, 0.001f, "fitted ortho near from aabb z");
    expectNear(fitted.farPlane, 8.f, 0.001f, "fitted ortho far from aabb z");

    const CascadeOrthoBounds stabilised =
        CascadeLightSpaceLayout::stabiliseOrthoExtents(fitted, 1024u, true);
    expectTrue(stabilised.right >= fitted.right, "stabilised ortho expands or preserves right bound");
    expectTrue(stabilised.top >= fitted.top, "stabilised ortho expands or preserves top bound");

    const CascadeOrthoBounds unchanged =
        CascadeLightSpaceLayout::stabiliseOrthoExtents(fitted, 1024u, false);
    expectNear(unchanged.left, fitted.left, 0.001f, "stabilisation disabled preserves left bound");
}

void testLightSpaceMatrixBookkeeping() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {1.f, 6.f, 14.f};
    camera.forward = {0.f, -0.25f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 200.f;
    camera.fovDegrees = 70.f;
    camera.aspect = 1.6f;

    const fuse::math::Vec3 sunDirection{-0.2f, -1.f, -0.15f};
    const auto matrices =
        CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(0u, desc, camera, sunDirection);

    expectTrue(matrices.valid, "valid cascade yields light-space matrices");
    expectTrue(!CascadeLightSpaceLayout::isEmptyLightSpaceAabb(matrices.lightSpaceAabb),
               "matrix bookkeeping retains light-space aabb");
    expectTrue(matrices.orthoBounds.right > matrices.orthoBounds.left, "ortho bounds have horizontal extent");
    expectTrue(matrices.lightViewProj.data[15] != 0.f, "view-projection matrix populated");

    const fuse::math::Vec3 focus = CascadeLightSpaceLayout::computeCascadeFocus(0u, desc, camera);
    const fuse::renderer::CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(0u, desc, camera);
    const fuse::math::Vec3 expectedFocus =
        camera.position + camera.forward.normalized() * ((range.nearZ + range.farZ) * 0.5f);
    expectNear(focus.x, expectedFocus.x, 0.001f, "cascade focus matches midpoint");
    expectNear(focus.z, expectedFocus.z, 0.001f, "cascade focus z matches midpoint");
}

void testDegenerateCascadeMatrices() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadeOrthoBounds;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 20.f;
    camera.farPlane = 5.f;

    const fuse::renderer::CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(0u, desc, camera);
    expectTrue(CascadeLightSpaceLayout::isDegenerateCascadeRange(range, camera),
               "inverted camera planes flagged degenerate");

    const auto matrices =
        CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(0u, desc, camera, {0.f, -1.f, 0.f});
    expectTrue(!matrices.valid, "degenerate cascade range yields invalid matrices");

    const CascadeOrthoBounds emptyFit =
        CascadeLightSpaceLayout::fitOrthoBoundsFromLightSpaceAabb({{1.f, 1.f, 1.f}, {-1.f, -1.f, -1.f}});
    const auto emptyProjection = CascadeLightSpaceLayout::buildOrthographicShadowProjection(emptyFit);
    expectNear(emptyProjection.data[0], 1.f, 0.001f, "empty aabb yields identity projection stub");
}

void testLightSpaceAabb() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {0.f, 5.f, 10.f};
    camera.forward = {0.f, -0.2f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 200.f;
    camera.fovDegrees = 60.f;
    camera.aspect = 16.f / 9.f;

    const fuse::math::Vec3 sunDirection{-0.3f, -1.f, -0.2f};
    const auto aabb = CascadeLightSpaceLayout::computeCascadeLightSpaceAabb(0u, desc, camera, sunDirection);

    expectTrue(aabb.min.x < aabb.max.x, "light-space aabb has horizontal extent");
    expectTrue(aabb.min.y < aabb.max.y, "light-space aabb has vertical extent");
    expectTrue(aabb.min.z < aabb.max.z, "light-space aabb has depth extent");

    const auto corners = CascadedShadowMapLayout::buildCascadeFrustumCorners(0u, desc, camera);
    const fuse::renderer::CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(0u, desc, camera);
    const fuse::math::Vec3 focus =
        camera.position + camera.forward.normalized() * ((range.nearZ + range.farZ) * 0.5f);
    const fuse::math::Mat4 lightView = CascadeLightSpaceLayout::buildLightView(focus, sunDirection);
    const auto rebuilt = CascadeLightSpaceLayout::computeLightSpaceAabb(corners, lightView);
    expectNear(rebuilt.min.x, aabb.min.x, 0.001f, "rebuilt light-space min x matches");
    expectNear(rebuilt.max.z, aabb.max.z, 0.001f, "rebuilt light-space max z matches");
}

void testClampSplitParams() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadedShadowMapLayout;

    CascadeSplitParams raw{};
    raw.lambda = 2.5f;
    raw.cascadeCount = 99u;

    const CascadeSplitParams clamped = CascadeSplitParams::clampParams(raw);
    expectTrue(clamped.cascadeCount == fuse::renderer::kMaxCascadeCount, "cascade count clamped to max");
    expectNear(clamped.lambda, 1.f, 0.001f, "lambda clamped to unit interval");
    expectNear(CascadedShadowMapLayout::clampSplitLambda(-0.25f), 0.f, 0.001f, "negative lambda clamped to zero");
    expectNear(CascadedShadowMapLayout::clampSplitFraction(1.5f), 1.f, 0.001f, "split fraction clamped to one");
}

void testShadowMat4ClearAndIdentity() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::ShadowMat4;

    ShadowMat4 matrix = ShadowMat4::identity();
    expectTrue(matrix.isIdentity(), "fresh identity reports identity");
    matrix.data[3] = 0.5f;
    expectTrue(!matrix.isIdentity(), "modified matrix is not identity");
    matrix.clear();
    expectTrue(matrix.isIdentity(), "cleared matrix returns to identity");
    expectTrue(!CascadeLightSpaceLayout::shadowMat4IsPopulated(matrix),
               "cleared matrix is not populated for shadow upload");
}

void testShouldSkipCascadeShadowBuild() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::math::Vec3 sunDirection{0.f, -1.f, 0.f};
    expectTrue(!CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(0u, desc, camera, sunDirection),
               "default cascade is not skipped");

    expectTrue(CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(0u, desc, camera, {0.f, 0.f, 0.f}),
               "zero light direction skips cascade build");
    expectTrue(CascadeLightSpaceLayout::isEmptyLightDirection({0.f, 0.f, 0.f}),
               "zero light direction flagged empty");

    ShadowCameraParams invertedCamera = camera;
    invertedCamera.nearPlane = 50.f;
    invertedCamera.farPlane = 10.f;
    expectTrue(CascadedShadowMapLayout::isEmptyCameraDepthRange(invertedCamera),
               "inverted camera depth range flagged empty");
    expectTrue(CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(0u, desc, invertedCamera, sunDirection),
               "inverted camera skips cascade build");

    CascadedShadowMapDesc flatDesc{};
    flatDesc.cascadeSplits[0] = 0.5f;
    flatDesc.cascadeSplits[1] = 0.5f;
    flatDesc.cascadeSplits[2] = 1.f;
    flatDesc.cascadeSplits[3] = 1.f;
    expectTrue(CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(1u, flatDesc, camera, sunDirection),
               "zero-thickness cascade slice skipped");
}

void testCountValidCascadeMatrixSlots() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::math::Vec3 sunDirection{0.f, -1.f, 0.f};
    expectTrue(CascadeLightSpaceLayout::countValidCascadeMatrixSlots(desc, camera, sunDirection, 4u) == 4u,
               "default four cascades all eligible");

    expectTrue(CascadeLightSpaceLayout::countValidCascadeMatrixSlots(desc, camera, {0.f, 0.f, 0.f}, 4u) == 0u,
               "empty light direction yields zero eligible cascades");

    CascadedShadowMapDesc flatDesc{};
    flatDesc.cascadeSplits[0] = 0.5f;
    flatDesc.cascadeSplits[1] = 0.5f;
    flatDesc.cascadeSplits[2] = 1.f;
    flatDesc.cascadeSplits[3] = 1.f;
    expectTrue(CascadeLightSpaceLayout::countValidCascadeMatrixSlots(flatDesc, camera, sunDirection, 4u) == 2u,
               "zero-thickness cascade excluded from eligible count");
    expectTrue(CascadedShadowMapLayout::countNonEmptyCascadeFrustums(flatDesc, camera, 4u) == 2u,
               "non-empty cascade count matches eligible matrix slots");
}

void testPopulateCascadeShadowData() {
    using fuse::renderer::CascadeShadowDataLayout;
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapData;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.position = {0.f, 5.f, 10.f};
    camera.forward = {0.f, -0.2f, -1.f};
    camera.nearPlane = 0.1f;
    camera.farPlane = 150.f;

    CascadedShadowMapData data{};
    const fuse::math::Vec3 sunDirection{-0.2f, -1.f, -0.1f};
    const fuse::u32 populatedCount =
        CascadeShadowDataLayout::populateCascadeShadowData(desc, camera, sunDirection, 4u, data);

    expectTrue(populatedCount == 4u, "populate fills all default cascades");
    expectTrue(CascadeShadowDataLayout::countPopulatedCascadeMatrices(data, 4u) == 4u,
               "populated matrix count matches batch result");
    expectTrue(data.cascadeFarZ[0] > camera.nearPlane, "populated far z written for first cascade");
    expectTrue(CascadeLightSpaceLayout::shadowMat4IsPopulated(data.lightViewProj[0]),
               "populated view-projection is non-identity");

    const fuse::u32 emptyLightCount =
        CascadeShadowDataLayout::populateCascadeShadowData(desc, camera, {0.f, 0.f, 0.f}, 4u, data);
    expectTrue(emptyLightCount == 0u, "empty light direction clears populated data");
    expectTrue(CascadeShadowDataLayout::countPopulatedCascadeMatrices(data, 4u) == 0u,
               "cleared data has zero populated matrices");
    expectTrue(data.lightViewProj[0].isIdentity(), "cleared cascade slot returns identity matrix");
}

void testClearCascadeShadowDataSlots() {
    using fuse::renderer::CascadeShadowDataLayout;
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapData;
    using fuse::renderer::ShadowMat4;

    CascadedShadowMapData data{};
    data.lightViewProj[0].data[0] = 2.f;
    data.cascadeFarZ[0] = 42.f;

    CascadeShadowDataLayout::clearCascadeSlot(0u, data);
    expectTrue(data.lightViewProj[0].isIdentity(), "clearCascadeSlot resets matrix");
    expectNear(data.cascadeFarZ[0], 0.f, 0.001f, "clearCascadeSlot resets far z");

    data.lightViewProj[1].data[5] = 3.f;
    data.lightViewProj[2].data[10] = 4.f;
    CascadeShadowDataLayout::clearAllCascadeSlots(data);
    expectTrue(data.lightViewProj[1].isIdentity() && data.lightViewProj[2].isIdentity(),
               "clearAllCascadeSlots resets every slot");
    expectTrue(!CascadeLightSpaceLayout::shadowMat4IsPopulated(data.lightViewProj[1]),
               "cleared slots are not populated");
    expectTrue(ShadowMat4::identity().isIdentity(), "identity helper remains identity");
}

void testEmptyLightDirectionDirectionalShadowUpdate() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for empty-light guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready for empty-light guard");

    fuse::renderer::DirectionalShadow shadows;
    expectTrue(shadows.init(resources, {}), "directional shadow initialized for empty-light guard");

    fuse::renderer::ShadowCameraParams camera{};
    camera.farPlane = 200.f;
    shadows.update(camera, {-0.3f, -1.f, -0.2f});
    expectTrue(shadows.data().lightViewProj[0].data[15] != 0.f, "valid light direction populates matrix");

    shadows.update(camera, {0.f, 0.f, 0.f});
    expectTrue(shadows.data().lightViewProj[0].isIdentity(), "empty light direction clears cascade matrix");
    expectTrue(shadows.stats().framesUpdated == 2u, "empty-light update still advances stats");

    shadows.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testSanitizeCascadeSplits() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;

    CascadedShadowMapDesc desc{};
    desc.cascadeSplits[0] = -0.2f;
    desc.cascadeSplits[1] = 1.5f;
    desc.cascadeSplits[2] = 0.3f;
    desc.cascadeSplits[3] = 0.8f;

    CascadedShadowMapLayout::sanitizeCascadeSplits(desc);
    expectTrue(CascadedShadowMapLayout::validateClampedCascadeSplits(desc), "sanitized splits are clamped and valid");
    expectNear(desc.cascadeSplits[0], 0.f, 0.001f, "negative split clamped to zero");
    expectNear(desc.cascadeSplits[1], 1.f, 0.001f, "oversized split clamped to one");
    expectTrue(desc.cascadeSplits[2] >= desc.cascadeSplits[1], "non-monotonic split repaired");
    expectNear(desc.cascadeSplits[3], 1.f, 0.001f, "last split pinned to far plane");
}

void testClampedCascadeFarZ() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    desc.cascadeSplits[0] = 2.f;

    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 101.f;

    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(0u, desc, camera), 101.f, 0.001f,
               "out-of-range split fraction clamped before far-z compute");
}

void testPopulateCascadeSplitsClamped() {
    using fuse::renderer::CascadeSplitParams;
    using fuse::renderer::CascadeSplitScheme;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    ShadowCameraParams camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 200.f;

    CascadeSplitParams rawParams{};
    rawParams.scheme = CascadeSplitScheme::Practical;
    rawParams.lambda = 9.f;
    rawParams.cascadeCount = 0u;

    CascadedShadowMapDesc desc{};
    CascadedShadowMapLayout::populateCascadeSplitsClamped(CascadeSplitParams::clampParams(rawParams), camera, desc);
    expectTrue(CascadedShadowMapLayout::validateClampedCascadeSplits(desc),
               "clamped populate yields valid split fractions");
    expectNear(desc.cascadeSplits[0], 1.f, 0.001f, "single clamped cascade reaches far plane");
}

void testCountSkippedCascadeShadowBuilds() {
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::math::Vec3 sunDirection{0.f, -1.f, 0.f};
    expectTrue(CascadeLightSpaceLayout::countSkippedCascadeShadowBuilds(desc, camera, sunDirection, 4u) == 0u,
               "default cascades are not skipped");

    expectTrue(CascadeLightSpaceLayout::countSkippedCascadeShadowBuilds(desc, camera, {0.f, 0.f, 0.f}, 4u) == 4u,
               "empty light direction skips every cascade");

    CascadedShadowMapDesc flatDesc{};
    flatDesc.cascadeSplits[0] = 0.5f;
    flatDesc.cascadeSplits[1] = 0.5f;
    flatDesc.cascadeSplits[2] = 1.f;
    flatDesc.cascadeSplits[3] = 1.f;
    expectTrue(CascadeLightSpaceLayout::countSkippedCascadeShadowBuilds(flatDesc, camera, sunDirection, 4u) == 2u,
               "zero-thickness cascades counted as skipped");
    expectTrue(CascadeLightSpaceLayout::countValidCascadeMatrixSlots(flatDesc, camera, sunDirection, 4u) +
                       CascadeLightSpaceLayout::countSkippedCascadeShadowBuilds(flatDesc, camera, sunDirection, 4u) ==
                   4u,
               "skipped + valid cascade counts sum to active count");
}

void testIsCascadeSlotPopulated() {
    using fuse::renderer::CascadeShadowDataLayout;
    using fuse::renderer::CascadeLightSpaceLayout;
    using fuse::renderer::CascadedShadowMapData;
    using fuse::renderer::ShadowMat4;

    CascadedShadowMapData data{};
    CascadeShadowDataLayout::clearAllCascadeSlots(data);
    expectTrue(!CascadeShadowDataLayout::isCascadeSlotPopulated(data, 0u), "cleared slot is not populated");

    data.lightViewProj[0].data[7] = 0.25f;
    expectTrue(CascadeShadowDataLayout::isCascadeSlotPopulated(data, 0u), "non-identity slot is populated");
    expectTrue(!CascadeShadowDataLayout::isCascadeSlotPopulated(data, 99u), "oob slot is not populated");

    CascadeShadowDataLayout::clearCascadeSlot(0u, data);
    expectTrue(!CascadeShadowDataLayout::isCascadeSlotPopulated(data, 0u),
               "cleared slot is not populated");
    expectTrue(ShadowMat4::identity().isIdentity(), "identity helper still valid after slot clear");
    expectTrue(!CascadeLightSpaceLayout::shadowMat4IsPopulated(data.lightViewProj[0]),
               "cleared slot matrix is not populated");
}

void testDirectionalShadowEmptyCameraGuard() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for empty-camera guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready for empty-camera guard");

    fuse::renderer::DirectionalShadow shadows;
    expectTrue(shadows.init(resources, {}), "directional shadow initialized for empty-camera guard");

    fuse::renderer::ShadowCameraParams camera{};
    camera.nearPlane = 80.f;
    camera.farPlane = 10.f;
    shadows.update(camera, {-0.3f, -1.f, -0.2f});
    expectTrue(shadows.data().lightViewProj[0].isIdentity(), "inverted camera clears cascade matrix");
    expectTrue(shadows.stats().framesUpdated == 1u, "empty-camera update still advances stats");

    shadows.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testShadowAtlasLayout() {
    using fuse::renderer::ShadowAtlas;
    using fuse::renderer::ShadowAtlasDesc;

    ShadowAtlasDesc desc{};
    desc.cascadeResolution = 1024;
    desc.cascadeCount = 4;
    desc.paddingTexels = 1;

    const auto layout = ShadowAtlas::computeLayout(desc);
    expectTrue(layout.atlasWidth == 2050u, "atlas width tiles cascades horizontally");
    expectTrue(layout.atlasHeight == 2050u, "atlas height tiles cascades vertically");
    expectTrue(layout.cascadeViewports[0].width == 1024u, "cascade 0 viewport width");
    expectTrue(layout.cascadeViewports[3].x == 1025u, "cascade 3 viewport x offset");
}

void testDirectionalShadowAllocation() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shadow test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::DirectionalShadowDesc desc{};
    desc.csm.resolution = 256;
    desc.atlas.cascadeResolution = 256;

    fuse::renderer::DirectionalShadow shadows;
    expectTrue(shadows.init(resources, desc), "directional shadow initialized");
    expectTrue(shadows.isReady(), "directional shadow ready");
    expectTrue(shadows.atlas().isReady(), "shadow atlas ready");
    expectTrue(shadows.atlas().texture().isValid(), "atlas texture allocated");

    for (fuse::u32 i = 0; i < fuse::renderer::kCascadeCount; ++i) {
        expectTrue(shadows.data().shadowMaps[i].isValid(), "cascade depth map allocated");
    }

    fuse::renderer::ShadowCameraParams camera{};
    camera.position = {0.f, 10.f, 20.f};
    camera.forward = {0.f, -0.2f, -1.f};
    camera.farPlane = 500.f;

    shadows.update(camera, {-0.3f, -1.f, -0.2f});
    expectTrue(shadows.stats().framesUpdated == 1u, "shadow update recorded");
    expectTrue(shadows.data().cascadeFarZ[0] > camera.nearPlane, "first cascade far z computed");
    expectTrue(shadows.data().lightViewProj[0].data[15] != 0.f, "cascade matrix populated");

    shadows.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testShadowPassGraph() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shadow pass test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::DirectionalShadow shadows;
    shadows.init(resources, {});

    auto pass = fuse::renderer::ShadowPass::create({});
    expectTrue(pass->isReady(), "shadow pass ready");

    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);
    expectTrue(pass->recordFrame(shadows, graph), "shadow pass recorded into graph");
    graph.compile();
    expectTrue(graph.compileInfo().passCount == 1u, "one shadow pass node");
    expectTrue(pass->lastStats().framesRecorded == 1u, "shadow pass stats updated");

    shadows.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testCascadeLayout();
    testCascadeSplitValidation();
    testCascadeSplitSchemes();
    testCascadeCountClamp();
    testValidateSplitDistances();
    testSingleCascadeCount();
    testIsEmptyCascadeFrustum();
    testBatchSplitDistances();
    testSplitNearDistances();
    testCountNonEmptyCascadeFrustums();
    testVariableCascadeCount();
    testVariableCascadeCountBatchMatrices();
    testBatchCascadeFarZs();
    testBatchCascadeNearZsAndRanges();
    testCascadeFrustumCorners();
    testLightSpaceAabbContainsCorners();
    testEmptyFrustumLightSpaceAabb();
    testOrthoBoundsFitAndStabilisation();
    testOrthoBoundsContainLightSpaceAabb();
    testShadowMat4IsPopulated();
    testLightSpaceMatrixBookkeeping();
    testBuildAllCascadeLightSpaceMatrices();
    testValidateOrthoBounds();
    testDegenerateCascadeMatrices();
    testLightSpaceAabb();
    testClampSplitParams();
    testShadowMat4ClearAndIdentity();
    testShouldSkipCascadeShadowBuild();
    testCountValidCascadeMatrixSlots();
    testPopulateCascadeShadowData();
    testClearCascadeShadowDataSlots();
    testEmptyLightDirectionDirectionalShadowUpdate();
    testSanitizeCascadeSplits();
    testClampedCascadeFarZ();
    testPopulateCascadeSplitsClamped();
    testCountSkippedCascadeShadowBuilds();
    testIsCascadeSlotPopulated();
    testDirectionalShadowEmptyCameraGuard();
    testShadowAtlasLayout();
    testDirectionalShadowAllocation();
    testShadowPassGraph();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_shadow_system: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_shadow_system: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
