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

    /// Clamp cascade count and lambda to CPU stub limits.
    static CascadeSplitParams clampParams(const CascadeSplitParams& raw);
};

/// Column-major 4×4 matrix for light view-projection (Vulkan / GLSL convention).
struct ShadowMat4 {
    std::array<f32, 16> data{};

    static ShadowMat4 identity();
    void clear();
    bool isIdentity() const;
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

/// Why a cascade shadow build would be skipped (B5.5 deepen).
enum class CascadeShadowSkipReason : u8 {
    None = 0,
    EmptyLightDirection = 1,
    EmptyCameraDepthRange = 2,
    EmptyCascadeFrustum = 3,
    DegenerateCascadeRange = 4,
};

/// Per-reason skipped-cascade breakdown for CPU bookkeeping (B5.5 deepen).
struct CascadeShadowSkipCounts {
    u32 total = 0;
    u32 emptyLight = 0;
    u32 emptyCamera = 0;
    u32 emptyFrustum = 0;
    u32 degenerateRange = 0;
};

/// Static cascade layout helpers.
struct CascadedShadowMapLayout {
    static u32 cascadeCount() { return kCascadeCount; }
    static u32 clampCascadeCount(u32 requestedCount);
    static u32 clampCascadeIndex(u32 cascadeIndex, u32 cascadeCount);
    static f32 clampSplitLambda(f32 lambda);
    static f32 clampSplitFraction(f32 fraction);
    static bool isEmptyCameraDepthRange(const ShadowCameraParams& camera);
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
    static f32 computeSplitNearDistance(u32 cascadeIndex,
                                        const CascadeSplitParams& params,
                                        const ShadowCameraParams& camera);
    static void computeSplitNearDistances(const CascadeSplitParams& params,
                                          const ShadowCameraParams& camera,
                                          f32 outNearDistances[kMaxCascadeCount]);
    static u32 countNonEmptyCascadeFrustums(const CascadedShadowMapDesc& desc,
                                            const ShadowCameraParams& camera,
                                            u32 cascadeCount);
    static void populateCascadeSplits(const CascadeSplitParams& params,
                                      const ShadowCameraParams& camera,
                                      CascadedShadowMapDesc& desc);
    /// Write clamped split params into `desc.cascadeSplits` for the first `cascadeCount` slots.
    static void populateCascadeSplitsClamped(const CascadeSplitParams& params,
                                             const ShadowCameraParams& camera,
                                             CascadedShadowMapDesc& desc);
    static bool validateSplitMonotonicity(const f32 splitFractions[], u32 cascadeCount);
    static bool validateSplitDistances(const f32 splitDistances[],
                                       u32 cascadeCount,
                                       const ShadowCameraParams& camera);
    static bool isEmptyCascadeFrustum(u32 cascadeIndex,
                                      const CascadedShadowMapDesc& desc,
                                      const ShadowCameraParams& camera);
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
    /// True when every split lies in [0, 1] and `validateCascadeSplits` passes.
    static bool validateClampedCascadeSplits(const CascadedShadowMapDesc& desc);
    /// True when any split fraction lies outside [0, 1] (B5.5 deepen follow-up).
    static bool needsCascadeSplitFractionClamp(const CascadedShadowMapDesc& desc);
    /// True when split fractions are not monotonically non-decreasing (B5.5 deepen follow-up).
    static bool needsCascadeSplitMonotonicityRepair(const CascadedShadowMapDesc& desc);
    /// True when the last cascade split is not pinned to 1.0 (B5.5 deepen follow-up).
    static bool needsLastCascadeSplitPin(const CascadedShadowMapDesc& desc);
    /// True when any sanitize step would mutate `desc` (B5.5 deepen follow-up).
    static bool needsCascadeSplitSanitize(const CascadedShadowMapDesc& desc);
    /// Clamp each split fraction to [0, 1] without enforcing monotonicity.
    static void clampCascadeSplitFractions(CascadedShadowMapDesc& desc);
    /// Repair non-monotonic split fractions in ascending order.
    static void enforceCascadeSplitMonotonicity(CascadedShadowMapDesc& desc);
    /// Pin the last cascade split to the far-plane fraction (1.0).
    static void pinLastCascadeSplit(CascadedShadowMapDesc& desc);
    /// Clamp each split to [0, 1], enforce monotonicity, and pin the last slot to 1.0.
    static void sanitizeCascadeSplits(CascadedShadowMapDesc& desc);
    static bool validateCascadeRanges(const CascadedShadowMapDesc& desc, const ShadowCameraParams& camera);
    static CascadeFrustumCorners buildCascadeFrustumCorners(u32 cascadeIndex,
                                                            const CascadedShadowMapDesc& desc,
                                                            const ShadowCameraParams& camera);
};

/// Orthographic projection extents in light view space.
struct CascadeOrthoBounds {
    f32 left = 0.f;
    f32 right = 0.f;
    f32 bottom = 0.f;
    f32 top = 0.f;
    f32 nearPlane = 0.f;
    f32 farPlane = 0.f;
};

/// GPU cascade payload population and slot clearing (CPU stub).
struct CascadeShadowDataLayout {
    static void clearCascadeSlot(u32 cascadeIndex, CascadedShadowMapData& data);
    static void clearAllCascadeSlots(CascadedShadowMapData& data);
    static u32 countPopulatedCascadeMatrices(const CascadedShadowMapData& data, u32 cascadeCount);
    static bool isCascadeSlotPopulated(const CascadedShadowMapData& data, u32 cascadeIndex);
    /// Populate far-Z and light view-projection slots; returns count of populated matrices.
    static u32 populateCascadeShadowData(const CascadedShadowMapDesc& desc,
                                         const ShadowCameraParams& camera,
                                         const fuse::math::Vec3& lightDirection,
                                         u32 cascadeCount,
                                         CascadedShadowMapData& outData);
};

/// Per-cascade light-space matrix bookkeeping (CPU stub).
struct CascadeLightSpaceMatrices {
    ShadowMat4 lightView{};
    ShadowMat4 lightProjection{};
    ShadowMat4 lightViewProj{};
    CascadeOrthoBounds orthoBounds{};
    fuse::math::AABB lightSpaceAabb{};
    bool valid = false;
};

/// Light-space fitting helpers for orthographic shadow projections (CPU stub).
struct CascadeLightSpaceLayout {
    static fuse::math::Vec3 computeCascadeFocus(u32 cascadeIndex,
                                                const CascadedShadowMapDesc& desc,
                                                const ShadowCameraParams& camera);
    static bool isDegenerateCascadeRange(const CascadeRange& range, const ShadowCameraParams& camera);
    static bool isDegenerateLightDirection(const fuse::math::Vec3& lightDirection);
    static bool isEmptyLightDirection(const fuse::math::Vec3& lightDirection);
    /// Bypass guard — empty light direction clears every cascade before fitting (B5.5 deepen follow-up).
    static bool shouldBypassEmptyLightDirection(const fuse::math::Vec3& lightDirection);
    /// Bypass guard — empty camera depth range clears every cascade before fitting (B5.5 deepen follow-up).
    static bool shouldBypassEmptyCameraDepthRange(const ShadowCameraParams& camera);
    /// True when every cascade should be bypassed before per-cascade fitting.
    static bool shouldBypassAllCascadeShadowBuilds(const ShadowCameraParams& camera,
                                                 const fuse::math::Vec3& lightDirection);
    /// Per-check skip guards routed by `classifyCascadeShadowSkip` (B5.5 deepen follow-up).
    static bool shouldSkipEmptyLightDirection(const fuse::math::Vec3& lightDirection);
    static bool shouldSkipEmptyCameraDepthRange(const ShadowCameraParams& camera);
    static bool shouldSkipEmptyCascadeFrustum(u32 cascadeIndex,
                                              const CascadedShadowMapDesc& desc,
                                              const ShadowCameraParams& camera);
    static bool shouldSkipDegenerateCascadeRange(u32 cascadeIndex,
                                                 const CascadedShadowMapDesc& desc,
                                                 const ShadowCameraParams& camera);
    /// Classify why a cascade build would be skipped — same ordering as `shouldSkipCascadeShadowBuild`.
    static CascadeShadowSkipReason classifyCascadeShadowSkip(u32 cascadeIndex,
                                                             const CascadedShadowMapDesc& desc,
                                                             const ShadowCameraParams& camera,
                                                             const fuse::math::Vec3& lightDirection);
    static bool shouldSkipCascadeShadowBuild(u32 cascadeIndex,
                                             const CascadedShadowMapDesc& desc,
                                             const ShadowCameraParams& camera,
                                             const fuse::math::Vec3& lightDirection);
    static u32 countValidCascadeMatrixSlots(const CascadedShadowMapDesc& desc,
                                            const ShadowCameraParams& camera,
                                            const fuse::math::Vec3& lightDirection,
                                            u32 cascadeCount);
    static u32 countSkippedCascadeShadowBuilds(const CascadedShadowMapDesc& desc,
                                               const ShadowCameraParams& camera,
                                               const fuse::math::Vec3& lightDirection,
                                               u32 cascadeCount);
    /// Per-reason skipped-cascade breakdown for the first `cascadeCount` slots.
    static CascadeShadowSkipCounts countCascadeShadowSkipsByKind(const CascadedShadowMapDesc& desc,
                                                                 const ShadowCameraParams& camera,
                                                                 const fuse::math::Vec3& lightDirection,
                                                                 u32 cascadeCount);
    static bool validateOrthoBounds(const CascadeOrthoBounds& bounds);
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
    static CascadeOrthoBounds fitOrthoBoundsFromLightSpaceAabb(const fuse::math::AABB& aabb);
    static CascadeOrthoBounds fitOrthoBoundsFromCascadeFrustum(u32 cascadeIndex,
                                                               const CascadedShadowMapDesc& desc,
                                                               const ShadowCameraParams& camera,
                                                               const fuse::math::Vec3& lightDirection);
    static bool orthoBoundsContainsLightSpaceAabb(const CascadeOrthoBounds& bounds,
                                                  const fuse::math::AABB& aabb);
    static CascadeOrthoBounds stabiliseOrthoExtents(const CascadeOrthoBounds& bounds,
                                                    u32 shadowMapResolution,
                                                    bool enableStabilisation);
    static ShadowMat4 buildOrthographicShadowProjection(const CascadeOrthoBounds& bounds);
    static bool shadowMat4IsPopulated(const ShadowMat4& matrix);
    static ShadowMat4 multiplyShadowMatrices(const ShadowMat4& a, const ShadowMat4& b);
    static ShadowMat4 shadowMat4FromMat4(const fuse::math::Mat4& matrix);
    static CascadeLightSpaceMatrices buildCascadeLightSpaceMatrices(u32 cascadeIndex,
                                                                    const CascadedShadowMapDesc& desc,
                                                                    const ShadowCameraParams& camera,
                                                                    const fuse::math::Vec3& lightDirection);
    /// Build per-cascade light-space matrices for all slots; returns count of valid cascades.
    static u32 buildAllCascadeLightSpaceMatrices(const CascadedShadowMapDesc& desc,
                                                 const ShadowCameraParams& camera,
                                                 const fuse::math::Vec3& lightDirection,
                                                 CascadeLightSpaceMatrices outMatrices[kCascadeCount]);
    /// Build light-space matrices for the first `cascadeCount` slots; inactive slots are cleared.
    static u32 buildAllCascadeLightSpaceMatrices(const CascadedShadowMapDesc& desc,
                                                 const ShadowCameraParams& camera,
                                                 const fuse::math::Vec3& lightDirection,
                                                 u32 cascadeCount,
                                                 CascadeLightSpaceMatrices outMatrices[kCascadeCount]);
};

/// True when a cascade shadow skip reason blocks matrix population.
bool cascadeShadowSkipReasonIsBlocking(CascadeShadowSkipReason reason);

/// Human-readable label for skip reasons (logging / tests).
const char* cascadeShadowSkipReasonLabel(CascadeShadowSkipReason reason);

/// Record one skip reason into per-kind counters (B5.5 deepen follow-up).
void recordCascadeShadowSkip(CascadeShadowSkipCounts& counts, CascadeShadowSkipReason reason);

} // namespace fuse::renderer
