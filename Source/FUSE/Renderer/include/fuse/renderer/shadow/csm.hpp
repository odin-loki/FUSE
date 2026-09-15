#pragma once

#include <fuse/math/aabb.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <array>

namespace fuse::renderer {

/// Cascaded shadow map constants (B5.5 — P5 §5.5).
static constexpr u32 kMinCascadeCount = 1u;
static constexpr u32 kMaxCascadeCount = 4u;
static constexpr u32 kCascadeCount = kMaxCascadeCount;

/// PSSM-style cascade split distribution (CPU stub).
enum class CascadeSplitScheme : u8 {
    Uniform = 0,
    Logarithmic = 1,
    Practical = 2,
};

/// Inputs for automatic cascade split generation.
struct CascadeSplitParams {
    CascadeSplitScheme scheme = CascadeSplitScheme::Uniform;
    f32 lambda = 0.5f;
    u32 cascadeCount = kCascadeCount;
};

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

/// View-space depth range for one cascade slice.
struct CascadeRange {
    f32 nearZ = 0.f;
    f32 farZ = 0.f;
};

/// Eight world-space corners of a camera sub-frustum slice (near quad + far quad).
struct CascadeFrustumCorners {
    fuse::math::Vec3 corners[8]{};
};

/// Static cascade layout helpers.
struct CascadedShadowMapLayout {
    static u32 cascadeCount() { return kCascadeCount; }
    static u32 clampCascadeCount(u32 requestedCount);
    static u32 clampCascadeIndex(u32 cascadeIndex, u32 cascadeCount);
    static GpuFormat depthFormat() { return GpuFormat::R32Sfloat; }
    static const char* debugName(u32 cascadeIndex);
    static f32 computeSplitFraction(u32 cascadeIndex,
                                    const CascadeSplitParams& params,
                                    const ShadowCameraParams& camera);
    static f32 computeSplitDistance(u32 cascadeIndex,
                                    const CascadeSplitParams& params,
                                    const ShadowCameraParams& camera);
    static void computeSplitFractions(const CascadeSplitParams& params,
                                      const ShadowCameraParams& camera,
                                      f32 outFractions[kMaxCascadeCount]);
    static void computeSplitDistances(const CascadeSplitParams& params,
                                      const ShadowCameraParams& camera,
                                      f32 outDistances[kMaxCascadeCount]);
    static void populateCascadeSplits(const CascadeSplitParams& params,
                                      const ShadowCameraParams& camera,
                                      CascadedShadowMapDesc& desc);
    static bool validateSplitMonotonicity(const f32 splitFractions[], u32 cascadeCount);
    static f32 computeCascadeNearZ(u32 cascadeIndex, const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera);
    static f32 computeCascadeFarZ(u32 cascadeIndex, const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera);
    static CascadeRange computeCascadeRange(u32 cascadeIndex,
                                            const CascadedShadowMapDesc& desc,
                                            const ShadowCameraParams& camera);
    static void computeCascadeNearZs(const CascadedShadowMapDesc& desc,
                                     const ShadowCameraParams& camera,
                                     f32 outNearZ[kCascadeCount]);
    static void computeCascadeFarZs(const CascadedShadowMapDesc& desc,
                                    const ShadowCameraParams& camera,
                                    f32 outFarZ[kCascadeCount]);
    static void computeCascadeRanges(const CascadedShadowMapDesc& desc,
                                     const ShadowCameraParams& camera,
                                     CascadeRange outRanges[kCascadeCount]);
    static bool validateCascadeSplits(const CascadedShadowMapDesc& desc);
    static bool validateCascadeRanges(const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera);
    static CascadeFrustumCorners buildCascadeFrustumCorners(u32 cascadeIndex,
                                                            const CascadedShadowMapDesc& desc,
                                                            const ShadowCameraParams& camera);
};

/// Light-space fitting helpers for orthographic shadow projections (CPU stub).
struct CascadeLightSpaceLayout {
    static fuse::math::Mat4 buildLightView(const fuse::math::Vec3& focus, const fuse::math::Vec3& lightDirection);
    static bool isEmptyLightSpaceAabb(const fuse::math::AABB& aabb);
    static fuse::math::AABB computeLightSpaceAabbFromWorldCorners(const fuse::math::Vec3 worldCorners[8],
                                                                  const fuse::math::Mat4& lightView);
    static fuse::math::AABB computeLightSpaceAabb(const CascadeFrustumCorners& corners,
                                                  const fuse::math::Mat4& lightView);
    static fuse::math::AABB computeCascadeLightSpaceAabb(u32 cascadeIndex,
                                                        const CascadedShadowMapDesc& desc,
                                                        const ShadowCameraParams& camera,
                                                        const fuse::math::Vec3& lightDirection);
};

} // namespace fuse::renderer
