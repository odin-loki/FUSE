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

f32 CascadedShadowMapLayout::computeCascadeFarZ(u32 cascadeIndex,
                                                const CascadedShadowMapDesc& desc,
                                                const ShadowCameraParams& camera) {
    if (cascadeIndex >= kCascadeCount) {
        return camera.farPlane;
    }

    const f32 splitFraction = desc.cascadeSplits[cascadeIndex];
    return camera.nearPlane + splitFraction * (camera.farPlane - camera.nearPlane);
}

} // namespace fuse::renderer
