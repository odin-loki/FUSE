#include <fuse/renderer/shadow/csm.hpp>

#include <cmath>

namespace fuse::renderer {

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

void CascadedShadowMapLayout::computeCascadeFarZs(const CascadedShadowMapDesc& desc,
                                                const ShadowCameraParams& camera,
                                                f32 outFarZ[kCascadeCount]) {
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        outFarZ[cascade] = computeCascadeFarZ(cascade, desc, camera);
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

} // namespace fuse::renderer
