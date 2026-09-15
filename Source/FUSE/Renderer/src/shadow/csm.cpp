#include <fuse/renderer/shadow/csm.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265f;

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

    return desc.cascadeSplits[kCascadeCount - 1u] == 1.0f;
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

fuse::math::Mat4 CascadeLightSpaceLayout::buildLightView(const fuse::math::Vec3& focus,
                                                         const fuse::math::Vec3& lightDirection) {
    const fuse::math::Vec3 lightDir = lightDirection.normalized();
    const fuse::math::Vec3 lightPos = focus - lightDir * 100.f;
    return fuse::math::lookAt(lightPos, focus, {0.f, 1.f, 0.f});
}

fuse::math::AABB CascadeLightSpaceLayout::computeLightSpaceAabb(const CascadeFrustumCorners& corners,
                                                                const fuse::math::Mat4& lightView) {
    fuse::math::AABB aabb = fuse::math::AABB::fromCenterExtents(
        fuse::math::transformPoint(lightView, corners.corners[0]), {});

    for (u32 cornerIdx = 1; cornerIdx < 8u; ++cornerIdx) {
        const fuse::math::Vec3 lightSpace = fuse::math::transformPoint(lightView, corners.corners[cornerIdx]);
        aabb.min.x = std::min(aabb.min.x, lightSpace.x);
        aabb.min.y = std::min(aabb.min.y, lightSpace.y);
        aabb.min.z = std::min(aabb.min.z, lightSpace.z);
        aabb.max.x = std::max(aabb.max.x, lightSpace.x);
        aabb.max.y = std::max(aabb.max.y, lightSpace.y);
        aabb.max.z = std::max(aabb.max.z, lightSpace.z);
    }

    return aabb;
}

fuse::math::AABB CascadeLightSpaceLayout::computeCascadeLightSpaceAabb(
    u32 cascadeIndex,
    const CascadedShadowMapDesc& desc,
    const ShadowCameraParams& camera,
    const fuse::math::Vec3& lightDirection) {
    const CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(cascadeIndex, desc, camera);
    const fuse::math::Vec3 forward = camera.forward.normalized();
    const f32 midDistance = (range.nearZ + range.farZ) * 0.5f;
    const fuse::math::Vec3 focus = camera.position + forward * midDistance;

    const CascadeFrustumCorners corners =
        CascadedShadowMapLayout::buildCascadeFrustumCorners(cascadeIndex, desc, camera);
    const fuse::math::Mat4 lightView = buildLightView(focus, lightDirection);
    return computeLightSpaceAabb(corners, lightView);
}

} // namespace fuse::renderer
