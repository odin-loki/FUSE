#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <array>

namespace fuse::renderer {

/// Cascaded shadow map constants (B5.5 — P5 §5.5).
static constexpr u32 kCascadeCount = 4;

/// Column-major 4×4 matrix for light view-projection (Vulkan / GLSL convention).
struct ShadowMat4 {
    std::array<f32, 16> data{};

    static ShadowMat4 identity();
};

/// Per-cascade configuration — mirrors P5 CSMDesc.
struct CascadedShadowMapDesc {
    u32 resolution = 2048;
    f32 cascadeSplits[kCascadeCount] = {0.05f, 0.15f, 0.4f, 1.0f};
    f32 depthBias = 0.005f;
    f32 normalOffsetBias = 0.01f;
    bool stabilise = true;
};

/// GPU-side cascade payload uploaded for deferred shading.
struct CascadedShadowMapData {
    ShadowMat4 lightViewProj[kCascadeCount]{};
    f32 cascadeFarZ[kCascadeCount]{};
    TextureHandle shadowMaps[kCascadeCount]{};
};

/// Minimal camera inputs for cascade split computation.
struct ShadowCameraParams {
    fuse::math::Vec3 position{};
    fuse::math::Vec3 forward{0.f, 0.f, -1.f};
    f32 nearPlane = 0.1f;
    f32 farPlane = 1000.f;
    f32 fovDegrees = 60.f;
    f32 aspect = 16.f / 9.f;
};

/// Static cascade layout helpers.
struct CascadedShadowMapLayout {
    static u32 cascadeCount() { return kCascadeCount; }
    static GpuFormat depthFormat() { return GpuFormat::R32Sfloat; }
    static const char* debugName(u32 cascadeIndex);
    static f32 computeCascadeFarZ(u32 cascadeIndex, const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera);
};

} // namespace fuse::renderer
