#pragma once

// WP-6.2 offline reference: a CPU path tracer over the WP-6.0 reference scene (rt::RtReferenceScene, the
// two-level fuse::spatial::BVH with double-precision triangles) that integrates exactly the quantities the
// ray-traced shadow and reflection kernels estimate (rt_effects_kernel.hpp):
//
//   shadow(light, surface)       fraction of the light visible from the ray origin, by stratified jittered
//                                sampling of the light (rectangle / disk by area, sun by solid angle) with an
//                                independent generator (std::mt19937_64), f64 throughout, plus the mean
//                                occluder distance
//   reflection(surface, view)    lobe-averaged incoming radiance: stratified GGX visible-normal samples,
//                                each ray traced and its hit shaded with shadeHit()
//   shadeHit(ray, hit)           the kernels' first-bounce hit shading: geometric normal of the hit triangle
//                                (facing the ray), material row GpuInstance::material (base colour,
//                                metallic, emission; kRtfxDefaultAlbedo without a row), irradiance =
//                                pi x ambient + the hit lights' N.L x irradiance (x a shadow ray when
//                                `shadowed`), radiance = emissive + albedo (1 - metallic) / pi x irradiance;
//                                misses return the sky radiance
//
// The surfaces come from the G-buffer texels the GPU read (surfaceFromGBuffer, f64 reconstruction), so the
// comparison isolates the tracing and the estimators. Every estimate also returns its per-sample variance,
// from which the gates derive their statistical tolerance.

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/renderer/rt_effects/rt_effects_kernel.hpp>
#include <fuse/renderer/rt_effects/rt_effects_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer::rt_effects {

/// Albedo of reflection hits on instances without a material row (rtfx_common kFuseRtfxDefaultAlbedo).
inline constexpr f32 kRtfxDefaultAlbedo = 0.8f;

struct RtfxSurface {
    bool valid = false;   ///< false: sky pixel (depth >= 1) or not finite
    RtfxVec3<f64> position{};
    RtfxVec3<f64> normal{};
    RtfxVec3<f64> origin{}; ///< ray origin (rtfxRayOrigin in f64)
    RtfxVec3<f64> view{};   ///< unit vector to the camera
    f64 roughness = 1.0;
};

struct RtfxEstimate {
    f64 mean[3] = {0.0, 0.0, 0.0};     ///< visibility in [0] (shadows) / radiance (reflections)
    f64 variance[3] = {0.0, 0.0, 0.0}; ///< per-sample variance of the integrand
    f64 hitDistance = kRtfxNoHit;      ///< mean over the samples that hit (kRtfxNoHit: none)
    f64 hitDistanceVariance = 0.0;     ///< per-sample variance of the hit distance over the samples that hit
    u32 samples = 0;
    u32 hits = 0;
};

class RtEffectsReference {
public:
    /// `materials` (GpuScene material rows, `materialCount` of them) may be null.
    RtEffectsReference(const rt::RtReferenceScene& bvh, const gpu_scene::GpuScene& scene, const Material::GPUMaterial* materials,
                       u32 materialCount);

    /// The surface of pixel (px, py) from its RT4 depth, RT0 oct normal and RT2 roughness texels.
    static RtfxSurface surfaceFromGBuffer(const RtfxFrameConstants& c, u32 px, u32 py, f32 depth, f32 octX, f32 octY,
                                          f32 roughness);

    /// Converged shadow visibility of `light` (sqrtSamples^2 stratified samples; 1 ray for hard kinds).
    RtfxEstimate shadow(const RtfxFrameConstants& c, const RtfxShadowLight& light, const RtfxSurface& s, u32 sqrtSamples,
                        u64 seed) const;
    /// Converged lobe-averaged reflected radiance (sqrtSamples^2 stratified samples; 1 for a mirror).
    RtfxEstimate reflection(const RtfxFrameConstants& c, const RtfxSurface& s, u32 sqrtSamples, u64 seed) const;

    /// Radiance arriving along `ray` whose closest hit (cull mask c.reflectionCullMask) is `hit`. With
    /// `secondaryRobust`, the hit lights' shadow rays are classified (rt::RtReferenceScene::traceClassified at
    /// 1e-4): false when one of them lies within the edge band (its answer may differ on the GPU).
    RtfxVec3<f64> shadeHit(const RtfxFrameConstants& c, const RtfxVec3<f64>& origin, const RtfxVec3<f64>& dir,
                           const rt::RtRefHit& hit, bool* secondaryRobust = nullptr) const;

    const rt::RtReferenceScene& bvh() const { return m_bvh; }

    /// Radiance of a miss along unit `dir` when the frame has kRtfxFlagAtmosphereSky (the kernels'
    /// at_sky_radiance(atmosphere, dir, false)); without it (or without the flag) misses return c.sky.
    using SkyRadianceFn = RtfxVec3<f64> (*)(const void* user, const RtfxVec3<f64>& dir);
    void setSkyRadiance(SkyRadianceFn fn, const void* user) {
        m_sky = fn;
        m_skyUser = user;
    }

private:
    rt::RtRefHit trace(const RtfxVec3<f64>& origin, const RtfxVec3<f64>& dir, f64 tMax, u32 cullMask) const;

    const rt::RtReferenceScene& m_bvh;
    const gpu_scene::GpuScene& m_scene;
    const Material::GPUMaterial* m_materials = nullptr;
    u32 m_materialCount = 0;
    SkyRadianceFn m_sky = nullptr;
    const void* m_skyUser = nullptr;
};

} // namespace fuse::renderer::rt_effects
