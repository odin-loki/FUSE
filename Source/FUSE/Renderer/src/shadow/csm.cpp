#include <fuse/renderer/shadow/csm.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265f;

f32 splitBlendParameter(u32 cascadeIndex, u32 cascadeCount) {
    if (cascadeCount == 0u) {
        return 0.f;
    }
    return static_cast<f32>(cascadeIndex + 1u) / static_cast<f32>(cascadeCount);
}

f32 computeUniformSplitFraction(f32 splitT) {
    return splitT;
}

f32 computeLogarithmicSplitFraction(f32 splitT, f32 nearPlane, f32 farPlane) {
    if (farPlane <= nearPlane || nearPlane <= 0.f) {
        return 0.f;
    }

    const f32 depthRatio = farPlane / nearPlane;
    const f32 logDistance = nearPlane * std::pow(depthRatio, splitT);
    return (logDistance - nearPlane) / (farPlane - nearPlane);
}

f32 computePracticalSplitFraction(f32 splitT, f32 nearPlane, f32 farPlane, f32 lambda) {
    const f32 uniform = computeUniformSplitFraction(splitT);
    const f32 logarithmic = computeLogarithmicSplitFraction(splitT, nearPlane, farPlane);
    const f32 blend = std::clamp(lambda, 0.f, 1.f);
    return blend * uniform + (1.f - blend) * logarithmic;
}

bool splitFractionNearOne(f32 splitFraction) {
    return std::fabs(splitFraction - 1.f) <= 1e-5f;
}

bool splitDistanceNearFarPlane(f32 splitDistance, f32 farPlane) {
    return std::fabs(splitDistance - farPlane) <= 1e-4f;
}

fuse::math::Vec3 buildCameraBasis(const ShadowCameraParams& camera,
                                  fuse::math::Vec3& outRight,
                                  fuse::math::Vec3& outUp) {
    const fuse::math::Vec3 forward = camera.forward.normalized();
    const fuse::math::Vec3 worldUp{0.f, 1.f, 0.f};
    outRight = fuse::math::cross(forward, worldUp).normalized();
    outUp = fuse::math::cross(outRight, forward);
    return forward;
}

} // namespace

ShadowMat4 ShadowMat4::identity() {
    ShadowMat4 matrix{};
    matrix.data[0] = 1.f;
    matrix.data[5] = 1.f;
    matrix.data[10] = 1.f;
    matrix.data[15] = 1.f;
    return matrix;
}

void ShadowMat4::clear() {
    *this = identity();
}

bool ShadowMat4::isIdentity() const {
    const ShadowMat4 identity = ShadowMat4::identity();
    for (u32 element = 0; element < 16u; ++element) {
        if (data[element] != identity.data[element]) {
            return false;
        }
    }
    return true;
}

CascadeSplitParams CascadeSplitParams::clampParams(const CascadeSplitParams& raw) {
    CascadeSplitParams clamped = raw;
    clamped.cascadeCount = CascadedShadowMapLayout::clampCascadeCount(raw.cascadeCount);
    clamped.lambda = CascadedShadowMapLayout::clampSplitLambda(raw.lambda);
    return clamped;
}

const char* CascadedShadowMapLayout::debugName(u32 cascadeIndex) {
    switch (cascadeIndex) {
    case 0:
        return "shadow_cascade_0";
    case 1:
        return "shadow_cascade_1";
    case 2:
        return "shadow_cascade_2";
    case 3:
        return "shadow_cascade_3";
    default:
        return "shadow_cascade_unknown";
    }
}

u32 CascadedShadowMapLayout::clampCascadeCount(u32 requestedCount) {
    if (requestedCount < kMinCascadeCount) {
        return kMinCascadeCount;
    }
    if (requestedCount > kMaxCascadeCount) {
        return kMaxCascadeCount;
    }
    return requestedCount;
}

u32 CascadedShadowMapLayout::clampCascadeIndex(u32 cascadeIndex, u32 cascadeCount) {
    const u32 clampedCount = clampCascadeCount(cascadeCount);
    if (clampedCount == 0u) {
        return 0u;
    }
    if (cascadeIndex >= clampedCount) {
        return clampedCount - 1u;
    }
    return cascadeIndex;
}

f32 CascadedShadowMapLayout::clampSplitLambda(f32 lambda) {
    return std::clamp(lambda, 0.f, 1.f);
}

f32 CascadedShadowMapLayout::clampSplitFraction(f32 fraction) {
    return std::clamp(fraction, 0.f, 1.f);
}

bool CascadedShadowMapLayout::isEmptyCameraDepthRange(const ShadowCameraParams& camera) {
    return camera.farPlane <= camera.nearPlane;
}

f32 CascadedShadowMapLayout::computeSplitFraction(u32 cascadeIndex,
                                                  const CascadeSplitParams& params,
                                                  const ShadowCameraParams& camera) {
    if (camera.farPlane <= camera.nearPlane) {
        return 0.f;
    }

    const f32 distance = computeSplitDistance(cascadeIndex, params, camera);
    return (distance - camera.nearPlane) / (camera.farPlane - camera.nearPlane);
}

f32 CascadedShadowMapLayout::computeSplitDistance(u32 cascadeIndex,
                                                  const CascadeSplitParams& params,
                                                  const ShadowCameraParams& camera) {
    const u32 cascadeCount = clampCascadeCount(params.cascadeCount);
    if (cascadeCount == 0u || camera.farPlane <= camera.nearPlane) {
        return camera.nearPlane;
    }

    const u32 clampedIndex = clampCascadeIndex(cascadeIndex, cascadeCount);
    const f32 splitT = splitBlendParameter(clampedIndex, cascadeCount);

    switch (params.scheme) {
    case CascadeSplitScheme::Uniform:
        return camera.nearPlane + computeUniformSplitFraction(splitT) * (camera.farPlane - camera.nearPlane);
    case CascadeSplitScheme::Logarithmic:
        return computeLogarithmicSplitFraction(splitT, camera.nearPlane, camera.farPlane) *
                   (camera.farPlane - camera.nearPlane) +
               camera.nearPlane;
    case CascadeSplitScheme::Practical:
        return camera.nearPlane +
               computePracticalSplitFraction(splitT, camera.nearPlane, camera.farPlane, params.lambda) *
                   (camera.farPlane - camera.nearPlane);
    default:
        return camera.nearPlane + computeUniformSplitFraction(splitT) * (camera.farPlane - camera.nearPlane);
    }
}

void CascadedShadowMapLayout::computeSplitFractions(const CascadeSplitParams& params,
                                                    const ShadowCameraParams& camera,
                                                    f32 outFractions[kMaxCascadeCount]) {
    const u32 cascadeCount = clampCascadeCount(params.cascadeCount);
    for (u32 cascade = 0; cascade < kMaxCascadeCount; ++cascade) {
        if (cascade < cascadeCount) {
            outFractions[cascade] = computeSplitFraction(cascade, params, camera);
        } else {
            outFractions[cascade] = 1.f;
        }
    }

    if (cascadeCount > 0u) {
        outFractions[cascadeCount - 1u] = 1.f;
    }
}

void CascadedShadowMapLayout::computeSplitDistances(const CascadeSplitParams& params,
                                                    const ShadowCameraParams& camera,
                                                    f32 outDistances[kMaxCascadeCount]) {
    const u32 cascadeCount = clampCascadeCount(params.cascadeCount);
    for (u32 cascade = 0; cascade < kMaxCascadeCount; ++cascade) {
        if (cascade < cascadeCount) {
            outDistances[cascade] = computeSplitDistance(cascade, params, camera);
        } else {
            outDistances[cascade] = camera.farPlane;
        }
    }
}

f32 CascadedShadowMapLayout::computeSplitNearDistance(u32 cascadeIndex,
                                                      const CascadeSplitParams& params,
                                                      const ShadowCameraParams& camera) {
    if (cascadeIndex == 0u) {
        return camera.nearPlane;
    }

    const u32 cascadeCount = clampCascadeCount(params.cascadeCount);
    if (cascadeIndex >= cascadeCount) {
        return camera.farPlane;
    }

    return computeSplitDistance(cascadeIndex - 1u, params, camera);
}

void CascadedShadowMapLayout::computeSplitNearDistances(const CascadeSplitParams& params,
                                                        const ShadowCameraParams& camera,
                                                        f32 outNearDistances[kMaxCascadeCount]) {
    const u32 cascadeCount = clampCascadeCount(params.cascadeCount);
    for (u32 cascade = 0; cascade < kMaxCascadeCount; ++cascade) {
        if (cascade < cascadeCount) {
            outNearDistances[cascade] = computeSplitNearDistance(cascade, params, camera);
        } else {
            outNearDistances[cascade] = camera.farPlane;
        }
    }
}

u32 CascadedShadowMapLayout::countNonEmptyCascadeFrustums(const CascadedShadowMapDesc& desc,
                                                          const ShadowCameraParams& camera,
                                                          u32 cascadeCount) {
    const u32 activeCount = clampCascadeCount(cascadeCount);
    u32 nonEmptyCount = 0u;
    for (u32 cascade = 0; cascade < activeCount; ++cascade) {
        if (!isEmptyCascadeFrustum(cascade, desc, camera)) {
            ++nonEmptyCount;
        }
    }
    return nonEmptyCount;
}

void CascadedShadowMapLayout::populateCascadeSplits(const CascadeSplitParams& params,
                                                    const ShadowCameraParams& camera,
                                                    CascadedShadowMapDesc& desc) {
    populateCascadeSplitsClamped(CascadeSplitParams::clampParams(params), camera, desc);
}

void CascadedShadowMapLayout::populateCascadeSplitsClamped(const CascadeSplitParams& params,
                                                           const ShadowCameraParams& camera,
                                                           CascadedShadowMapDesc& desc) {
    const u32 cascadeCount = clampCascadeCount(params.cascadeCount);
    computeSplitFractions(params, camera, desc.cascadeSplits);

    if (cascadeCount > 0u) {
        desc.cascadeSplits[cascadeCount - 1u] = 1.f;
    }

    for (u32 cascade = cascadeCount; cascade < kMaxCascadeCount; ++cascade) {
        desc.cascadeSplits[cascade] = 1.f;
    }
}

bool CascadedShadowMapLayout::validateSplitMonotonicity(const f32 splitFractions[], u32 cascadeCount) {
    const u32 clampedCount = clampCascadeCount(cascadeCount);
    if (clampedCount == 0u || splitFractions == nullptr) {
        return false;
    }

    f32 previousSplit = -1.f;
    for (u32 cascade = 0; cascade < clampedCount; ++cascade) {
        const f32 split = splitFractions[cascade];
        if (split < previousSplit) {
            return false;
        }
        previousSplit = split;
    }

    return splitFractionNearOne(splitFractions[clampedCount - 1u]);
}

bool CascadedShadowMapLayout::validateSplitDistances(const f32 splitDistances[],
                                                     u32 cascadeCount,
                                                     const ShadowCameraParams& camera) {
    const u32 clampedCount = clampCascadeCount(cascadeCount);
    if (clampedCount == 0u || splitDistances == nullptr || camera.farPlane <= camera.nearPlane) {
        return false;
    }

    f32 previousDistance = camera.nearPlane - 1.f;
    for (u32 cascade = 0; cascade < clampedCount; ++cascade) {
        const f32 distance = splitDistances[cascade];
        if (distance < previousDistance) {
            return false;
        }
        previousDistance = distance;
    }

    return splitDistanceNearFarPlane(splitDistances[clampedCount - 1u], camera.farPlane);
}

bool CascadedShadowMapLayout::isEmptyCascadeFrustum(u32 cascadeIndex,
                                                    const CascadedShadowMapDesc& desc,
                                                    const ShadowCameraParams& camera) {
    if (camera.farPlane <= camera.nearPlane) {
        return true;
    }

    const CascadeRange range = computeCascadeRange(cascadeIndex, desc, camera);
    return range.nearZ >= range.farZ;
}

f32 CascadedShadowMapLayout::computeCascadeNearZ(u32 cascadeIndex,
                                                 const CascadedShadowMapDesc& desc,
                                                 const ShadowCameraParams& camera) {
    if (cascadeIndex == 0u) {
        return camera.nearPlane;
    }
    if (cascadeIndex >= kCascadeCount) {
        return camera.farPlane;
    }

    return computeCascadeFarZ(cascadeIndex - 1u, desc, camera);
}

f32 CascadedShadowMapLayout::computeCascadeFarZ(u32 cascadeIndex,
                                                const CascadedShadowMapDesc& desc,
                                                const ShadowCameraParams& camera) {
    if (cascadeIndex >= kCascadeCount) {
        return camera.farPlane;
    }

    const f32 splitFraction = desc.cascadeSplits[cascadeIndex];
    return camera.nearPlane + splitFraction * (camera.farPlane - camera.nearPlane);
}

CascadeRange CascadedShadowMapLayout::computeCascadeRange(u32 cascadeIndex,
                                                        const CascadedShadowMapDesc& desc,
                                                        const ShadowCameraParams& camera) {
    return {computeCascadeNearZ(cascadeIndex, desc, camera),
            computeCascadeFarZ(cascadeIndex, desc, camera)};
}

void CascadedShadowMapLayout::computeCascadeNearZs(const CascadedShadowMapDesc& desc,
                                                   const ShadowCameraParams& camera,
                                                   f32 outNearZ[kCascadeCount]) {
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        outNearZ[cascade] = computeCascadeNearZ(cascade, desc, camera);
    }
}

void CascadedShadowMapLayout::computeCascadeFarZs(const CascadedShadowMapDesc& desc,
                                                const ShadowCameraParams& camera,
                                                f32 outFarZ[kCascadeCount]) {
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        outFarZ[cascade] = computeCascadeFarZ(cascade, desc, camera);
    }
}

void CascadedShadowMapLayout::computeCascadeRanges(const CascadedShadowMapDesc& desc,
                                                   const ShadowCameraParams& camera,
                                                   CascadeRange outRanges[kCascadeCount]) {
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        outRanges[cascade] = computeCascadeRange(cascade, desc, camera);
    }
}

bool CascadedShadowMapLayout::validateCascadeSplits(const CascadedShadowMapDesc& desc) {
    f32 previousSplit = -1.f;
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        const f32 split = desc.cascadeSplits[cascade];
        if (split < previousSplit) {
            return false;
        }
        previousSplit = split;
    }

    return splitFractionNearOne(desc.cascadeSplits[kCascadeCount - 1u]);
}

bool CascadedShadowMapLayout::validateCascadeRanges(const CascadedShadowMapDesc& desc,
                                                    const ShadowCameraParams& camera) {
    if (!validateCascadeSplits(desc)) {
        return false;
    }

    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        const CascadeRange range = computeCascadeRange(cascade, desc, camera);
        if (range.nearZ >= range.farZ) {
            return false;
        }
    }

    return true;
}

CascadeFrustumCorners CascadedShadowMapLayout::buildCascadeFrustumCorners(
    u32 cascadeIndex, const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera) {
    const f32 nearZ = computeCascadeNearZ(cascadeIndex, desc, camera);
    const f32 farZ = computeCascadeFarZ(cascadeIndex, desc, camera);

    fuse::math::Vec3 right{};
    fuse::math::Vec3 up{};
    const fuse::math::Vec3 forward = buildCameraBasis(camera, right, up);

    const f32 tanHalfFov = std::tan(camera.fovDegrees * 0.5f * kPi / 180.f);
    const f32 nearHalfHeight = nearZ * tanHalfFov;
    const f32 nearHalfWidth = nearHalfHeight * camera.aspect;
    const f32 farHalfHeight = farZ * tanHalfFov;
    const f32 farHalfWidth = farHalfHeight * camera.aspect;

    const fuse::math::Vec3 nearCenter = camera.position + forward * nearZ;
    const fuse::math::Vec3 farCenter = camera.position + forward * farZ;

    CascadeFrustumCorners corners{};
    corners.corners[0] = nearCenter - right * nearHalfWidth - up * nearHalfHeight;
    corners.corners[1] = nearCenter + right * nearHalfWidth - up * nearHalfHeight;
    corners.corners[2] = nearCenter + right * nearHalfWidth + up * nearHalfHeight;
    corners.corners[3] = nearCenter - right * nearHalfWidth + up * nearHalfHeight;
    corners.corners[4] = farCenter - right * farHalfWidth - up * farHalfHeight;
    corners.corners[5] = farCenter + right * farHalfWidth - up * farHalfHeight;
    corners.corners[6] = farCenter + right * farHalfWidth + up * farHalfHeight;
    corners.corners[7] = farCenter - right * farHalfWidth + up * farHalfHeight;
    return corners;
}

void CascadeShadowDataLayout::clearCascadeSlot(u32 cascadeIndex, CascadedShadowMapData& data) {
    if (cascadeIndex >= kCascadeCount) {
        return;
    }

    data.lightViewProj[cascadeIndex].clear();
    data.cascadeFarZ[cascadeIndex] = 0.f;
}

void CascadeShadowDataLayout::clearAllCascadeSlots(CascadedShadowMapData& data) {
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        clearCascadeSlot(cascade, data);
    }
}

u32 CascadeShadowDataLayout::countPopulatedCascadeMatrices(const CascadedShadowMapData& data,
                                                           u32 cascadeCount) {
    const u32 activeCount = CascadedShadowMapLayout::clampCascadeCount(cascadeCount);
    u32 populatedCount = 0u;
    for (u32 cascade = 0; cascade < activeCount; ++cascade) {
        if (CascadeLightSpaceLayout::shadowMat4IsPopulated(data.lightViewProj[cascade])) {
            ++populatedCount;
        }
    }
    return populatedCount;
}

u32 CascadeShadowDataLayout::populateCascadeShadowData(const CascadedShadowMapDesc& desc,
                                                       const ShadowCameraParams& camera,
                                                       const fuse::math::Vec3& lightDirection,
                                                       u32 cascadeCount,
                                                       CascadedShadowMapData& outData) {
    if (CascadeLightSpaceLayout::isEmptyLightDirection(lightDirection) ||
        CascadedShadowMapLayout::isEmptyCameraDepthRange(camera)) {
        clearAllCascadeSlots(outData);
        return 0u;
    }

    const u32 activeCount = CascadedShadowMapLayout::clampCascadeCount(cascadeCount);
    u32 populatedCount = 0u;
    for (u32 cascade = 0; cascade < activeCount; ++cascade) {
        if (CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(cascade, desc, camera, lightDirection)) {
            clearCascadeSlot(cascade, outData);
            continue;
        }

        const CascadeLightSpaceMatrices matrices =
            CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(cascade, desc, camera, lightDirection);
        outData.cascadeFarZ[cascade] =
            CascadedShadowMapLayout::computeCascadeFarZ(cascade, desc, camera);
        if (matrices.valid) {
            outData.lightViewProj[cascade] = matrices.lightViewProj;
            ++populatedCount;
        } else {
            clearCascadeSlot(cascade, outData);
        }
    }

    for (u32 cascade = activeCount; cascade < kCascadeCount; ++cascade) {
        clearCascadeSlot(cascade, outData);
    }

    return populatedCount;
}

fuse::math::Vec3 CascadeLightSpaceLayout::computeCascadeFocus(u32 cascadeIndex,
                                                              const CascadedShadowMapDesc& desc,
                                                              const ShadowCameraParams& camera) {
    const CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(cascadeIndex, desc, camera);
    const f32 midDistance = (range.nearZ + range.farZ) * 0.5f;
    const fuse::math::Vec3 forward = camera.forward.normalized();
    return camera.position + forward * midDistance;
}

bool CascadeLightSpaceLayout::isDegenerateCascadeRange(const CascadeRange& range,
                                                       const ShadowCameraParams& camera) {
    return range.nearZ >= range.farZ || camera.farPlane <= camera.nearPlane;
}

bool CascadeLightSpaceLayout::isDegenerateLightDirection(const fuse::math::Vec3& lightDirection) {
    return lightDirection.length() < 1e-8f;
}

bool CascadeLightSpaceLayout::isEmptyLightDirection(const fuse::math::Vec3& lightDirection) {
    return isDegenerateLightDirection(lightDirection);
}

bool CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(u32 cascadeIndex,
                                                           const CascadedShadowMapDesc& desc,
                                                           const ShadowCameraParams& camera,
                                                           const fuse::math::Vec3& lightDirection) {
    if (isEmptyLightDirection(lightDirection) || CascadedShadowMapLayout::isEmptyCameraDepthRange(camera)) {
        return true;
    }

    if (CascadedShadowMapLayout::isEmptyCascadeFrustum(cascadeIndex, desc, camera)) {
        return true;
    }

    const CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(cascadeIndex, desc, camera);
    return isDegenerateCascadeRange(range, camera);
}

u32 CascadeLightSpaceLayout::countValidCascadeMatrixSlots(const CascadedShadowMapDesc& desc,
                                                        const ShadowCameraParams& camera,
                                                        const fuse::math::Vec3& lightDirection,
                                                        u32 cascadeCount) {
    if (isEmptyLightDirection(lightDirection) || CascadedShadowMapLayout::isEmptyCameraDepthRange(camera)) {
        return 0u;
    }

    const u32 activeCount = CascadedShadowMapLayout::clampCascadeCount(cascadeCount);
    u32 validCount = 0u;
    for (u32 cascade = 0; cascade < activeCount; ++cascade) {
        if (shouldSkipCascadeShadowBuild(cascade, desc, camera, lightDirection)) {
            continue;
        }
        ++validCount;
    }
    return validCount;
}

bool CascadeLightSpaceLayout::validateOrthoBounds(const CascadeOrthoBounds& bounds) {
    const f32 width = bounds.right - bounds.left;
    const f32 height = bounds.top - bounds.bottom;
    const f32 depth = bounds.farPlane - bounds.nearPlane;
    return width > 1e-8f && height > 1e-8f && depth > 1e-8f;
}

fuse::math::Mat4 CascadeLightSpaceLayout::buildLightView(const fuse::math::Vec3& focus,
                                                         const fuse::math::Vec3& lightDirection) {
    const fuse::math::Vec3 lightDir = lightDirection.normalized();
    const fuse::math::Vec3 lightPos = focus - lightDir * 100.f;
    return fuse::math::lookAt(lightPos, focus, {0.f, 1.f, 0.f});
}

bool CascadeLightSpaceLayout::isEmptyLightSpaceAabb(const fuse::math::AABB& aabb) {
    return aabb.min.x > aabb.max.x || aabb.min.y > aabb.max.y || aabb.min.z > aabb.max.z;
}

fuse::math::AABB CascadeLightSpaceLayout::computeLightSpaceAabbFromWorldCorners(
    const fuse::math::Vec3 worldCorners[8],
    const fuse::math::Mat4& lightView) {
    if (worldCorners == nullptr) {
        return {{1.f, 1.f, 1.f}, {-1.f, -1.f, -1.f}};
    }

    fuse::math::AABB aabb = fuse::math::AABB::fromCenterExtents(
        fuse::math::transformPoint(lightView, worldCorners[0]), {});

    for (u32 cornerIdx = 1; cornerIdx < 8u; ++cornerIdx) {
        const fuse::math::Vec3 lightSpace = fuse::math::transformPoint(lightView, worldCorners[cornerIdx]);
        aabb.min.x = std::min(aabb.min.x, lightSpace.x);
        aabb.min.y = std::min(aabb.min.y, lightSpace.y);
        aabb.min.z = std::min(aabb.min.z, lightSpace.z);
        aabb.max.x = std::max(aabb.max.x, lightSpace.x);
        aabb.max.y = std::max(aabb.max.y, lightSpace.y);
        aabb.max.z = std::max(aabb.max.z, lightSpace.z);
    }

    return aabb;
}

fuse::math::AABB CascadeLightSpaceLayout::computeLightSpaceAabb(const CascadeFrustumCorners& corners,
                                                                const fuse::math::Mat4& lightView) {
    return computeLightSpaceAabbFromWorldCorners(corners.corners, lightView);
}

fuse::math::AABB CascadeLightSpaceLayout::computeCascadeLightSpaceAabb(
    u32 cascadeIndex,
    const CascadedShadowMapDesc& desc,
    const ShadowCameraParams& camera,
    const fuse::math::Vec3& lightDirection) {
    const CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(cascadeIndex, desc, camera);
    if (range.nearZ >= range.farZ || camera.farPlane <= camera.nearPlane) {
        return {{1.f, 1.f, 1.f}, {-1.f, -1.f, -1.f}};
    }

    const fuse::math::Vec3 forward = camera.forward.normalized();
    const f32 midDistance = (range.nearZ + range.farZ) * 0.5f;
    const fuse::math::Vec3 focus = camera.position + forward * midDistance;

    const CascadeFrustumCorners corners =
        CascadedShadowMapLayout::buildCascadeFrustumCorners(cascadeIndex, desc, camera);
    const fuse::math::Mat4 lightView = buildLightView(focus, lightDirection);
    return computeLightSpaceAabb(corners, lightView);
}

CascadeOrthoBounds CascadeLightSpaceLayout::fitOrthoBoundsFromLightSpaceAabb(const fuse::math::AABB& aabb) {
    if (isEmptyLightSpaceAabb(aabb)) {
        return {};
    }

    return {aabb.min.x, aabb.max.x, aabb.min.y, aabb.max.y, -aabb.max.z, -aabb.min.z};
}

CascadeOrthoBounds CascadeLightSpaceLayout::fitOrthoBoundsFromCascadeFrustum(
    u32 cascadeIndex,
    const CascadedShadowMapDesc& desc,
    const ShadowCameraParams& camera,
    const fuse::math::Vec3& lightDirection) {
    const fuse::math::AABB lightAabb = computeCascadeLightSpaceAabb(cascadeIndex, desc, camera, lightDirection);
    return fitOrthoBoundsFromLightSpaceAabb(lightAabb);
}

bool CascadeLightSpaceLayout::orthoBoundsContainsLightSpaceAabb(const CascadeOrthoBounds& bounds,
                                                                const fuse::math::AABB& aabb) {
    if (isEmptyLightSpaceAabb(aabb) || !validateOrthoBounds(bounds)) {
        return false;
    }

    return aabb.min.x >= bounds.left && aabb.max.x <= bounds.right && aabb.min.y >= bounds.bottom &&
           aabb.max.y <= bounds.top && -aabb.max.z >= bounds.nearPlane && -aabb.min.z <= bounds.farPlane;
}

CascadeOrthoBounds CascadeLightSpaceLayout::stabiliseOrthoExtents(const CascadeOrthoBounds& bounds,
                                                                  u32 shadowMapResolution,
                                                                  bool enableStabilisation) {
    if (!enableStabilisation || shadowMapResolution == 0u) {
        return bounds;
    }

    CascadeOrthoBounds stabilised = bounds;
    const f32 resolution = static_cast<f32>(shadowMapResolution);
    const f32 texelWorldSizeX = (bounds.right - bounds.left) / resolution;
    const f32 texelWorldSizeY = (bounds.top - bounds.bottom) / resolution;
    if (texelWorldSizeX <= 1e-8f || texelWorldSizeY <= 1e-8f) {
        return stabilised;
    }

    const f32 centerX = (bounds.left + bounds.right) * 0.5f;
    const f32 centerY = (bounds.bottom + bounds.top) * 0.5f;
    const f32 halfExtentX =
        std::ceil((bounds.right - bounds.left) * 0.5f / texelWorldSizeX + 0.5f) * texelWorldSizeX;
    const f32 halfExtentY =
        std::ceil((bounds.top - bounds.bottom) * 0.5f / texelWorldSizeY + 0.5f) * texelWorldSizeY;
    const f32 snappedCenterX = std::floor(centerX / texelWorldSizeX + 0.5f) * texelWorldSizeX;
    const f32 snappedCenterY = std::floor(centerY / texelWorldSizeY + 0.5f) * texelWorldSizeY;
    stabilised.left = snappedCenterX - halfExtentX;
    stabilised.right = snappedCenterX + halfExtentX;
    stabilised.bottom = snappedCenterY - halfExtentY;
    stabilised.top = snappedCenterY + halfExtentY;
    return stabilised;
}

ShadowMat4 CascadeLightSpaceLayout::buildOrthographicShadowProjection(const CascadeOrthoBounds& bounds) {
    ShadowMat4 projection = ShadowMat4::identity();
    const f32 width = bounds.right - bounds.left;
    const f32 height = bounds.top - bounds.bottom;
    const f32 depth = bounds.farPlane - bounds.nearPlane;
    if (width <= 1e-8f || height <= 1e-8f || depth <= 1e-8f) {
        return projection;
    }

    projection.data[0] = 2.f / width;
    projection.data[5] = 2.f / height;
    projection.data[10] = -1.f / depth;
    projection.data[12] = -(bounds.right + bounds.left) / width;
    projection.data[13] = -(bounds.top + bounds.bottom) / height;
    projection.data[14] = -bounds.nearPlane / depth;
    return projection;
}

bool CascadeLightSpaceLayout::shadowMat4IsPopulated(const ShadowMat4& matrix) {
    const ShadowMat4 identity = ShadowMat4::identity();
    for (u32 element = 0; element < 16u; ++element) {
        if (matrix.data[element] != identity.data[element]) {
            return true;
        }
    }
    return false;
}

ShadowMat4 CascadeLightSpaceLayout::multiplyShadowMatrices(const ShadowMat4& a, const ShadowMat4& b) {
    ShadowMat4 out{};
    for (u32 column = 0; column < 4u; ++column) {
        for (u32 row = 0; row < 4u; ++row) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4u; ++k) {
                sum += a.data[k * 4u + row] * b.data[column * 4u + k];
            }
            out.data[column * 4u + row] = sum;
        }
    }
    return out;
}

ShadowMat4 CascadeLightSpaceLayout::shadowMat4FromMat4(const fuse::math::Mat4& matrix) {
    ShadowMat4 out{};
    out.data = matrix.data;
    return out;
}

CascadeLightSpaceMatrices CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(
    u32 cascadeIndex,
    const CascadedShadowMapDesc& desc,
    const ShadowCameraParams& camera,
    const fuse::math::Vec3& lightDirection) {
    CascadeLightSpaceMatrices matrices{};
    const CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(cascadeIndex, desc, camera);
    if (isDegenerateCascadeRange(range, camera)) {
        return matrices;
    }

    if (isDegenerateLightDirection(lightDirection)) {
        return matrices;
    }

    const fuse::math::Vec3 focus = computeCascadeFocus(cascadeIndex, desc, camera);
    const fuse::math::AABB lightAabb = computeCascadeLightSpaceAabb(cascadeIndex, desc, camera, lightDirection);
    if (isEmptyLightSpaceAabb(lightAabb)) {
        return matrices;
    }

    matrices.lightView = shadowMat4FromMat4(buildLightView(focus, lightDirection));
    matrices.lightSpaceAabb = lightAabb;
    matrices.orthoBounds = stabiliseOrthoExtents(fitOrthoBoundsFromLightSpaceAabb(lightAabb),
                                                 desc.resolution,
                                                 desc.stabilise);
    matrices.lightProjection = buildOrthographicShadowProjection(matrices.orthoBounds);
    matrices.lightViewProj = multiplyShadowMatrices(matrices.lightProjection, matrices.lightView);
    matrices.valid = true;
    return matrices;
}

u32 CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(
    const CascadedShadowMapDesc& desc,
    const ShadowCameraParams& camera,
    const fuse::math::Vec3& lightDirection,
    CascadeLightSpaceMatrices outMatrices[kCascadeCount]) {
    return buildAllCascadeLightSpaceMatrices(desc, camera, lightDirection, kCascadeCount, outMatrices);
}

u32 CascadeLightSpaceLayout::buildAllCascadeLightSpaceMatrices(
    const CascadedShadowMapDesc& desc,
    const ShadowCameraParams& camera,
    const fuse::math::Vec3& lightDirection,
    u32 cascadeCount,
    CascadeLightSpaceMatrices outMatrices[kCascadeCount]) {
    const u32 activeCount = CascadedShadowMapLayout::clampCascadeCount(cascadeCount);
    u32 validCount = 0u;
    for (u32 cascade = 0; cascade < activeCount; ++cascade) {
        outMatrices[cascade] = buildCascadeLightSpaceMatrices(cascade, desc, camera, lightDirection);
        if (outMatrices[cascade].valid) {
            ++validCount;
        }
    }

    for (u32 cascade = activeCount; cascade < kCascadeCount; ++cascade) {
        outMatrices[cascade] = {};
    }

    return validCount;
}

} // namespace fuse::renderer
