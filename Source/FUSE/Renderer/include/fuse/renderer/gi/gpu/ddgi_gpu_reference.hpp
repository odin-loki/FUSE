#pragma once

// WP-6.1 DDGI on Vulkan: CPU references and host helpers (stub-safe).
//
//   * sdf_trace_radiance: the T0 probe-ray trace (sphere tracing of the global SDF) as a single-source
//     FUSE_HOST_DEVICE function. The oracle (ddgi_probe_kernel.hpp) traces analytic boxes and has no SDF
//     path, so this is the reference of shaders/ddgi/ddgi_trace.{comp,slang} (default permutation):
//     the SDF, its gradient and the union rules are the Compute agent's compute::ray_march_kernel
//     (scene_eval / scene_normal / object_sdf, far = kSdfFar), the shading at a hit is the oracle's
//     direct_radiance + multi-bounce term (trace_radiance), the backface rule the oracle's.
//   * runBlendReference: the oracle's ddgi_kernel::BlendKernel (CpuReference) on explicit ray results;
//     the gate feeds it the GPU's own ray set and ray results ("the same ray results").
//   * makeFrameConstants: DdgiFrameConstants from the oracle's settings (DdgiGpu::beginFrame uses it).
//   * sdfSceneFromBoxes: the oracle's analytic box scene as the global SDF (boxes, hard union).

#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::gi_gpu {

/// Union "far" value of the SDF evaluation (compute::RayMarchParams::max_dist) and the escape distance
/// of T0 shadow rays (ddgi_trace: FUSE_DDGI_SDF_FAR / FUSE_DDGI_SDF_SHADOW_MAX).
inline constexpr f32 kSdfFar = 1.0e30f;
inline constexpr f32 kSdfShadowMax = 1.0e4f;

/// Everything one T0 probe ray needs (a POD view; the GPU reads the same values from DdgiFrameConstants).
struct SdfTraceScene {
    const compute::SdfObject* objects = nullptr;
    u32 objectCount = 0;
    const DdgiSurface* surfaces = nullptr;
    u32 surfaceCount = 0;
    math::Vec3 sunDirection{0.f, 1.f, 0.f};
    math::Vec3 sunIrradiance{};
    math::Vec3 skyRadiance{};
    f32 maxDistance = 20.f;
    f32 minDistance = 1e-4f;
    u32 maxSteps = 256;
    f32 shadowBias = 1e-3f;
    f32 backfaceDistanceScale = 0.2f;
    bool multiBounce = true;
    ddgi_kernel::VolumeView volume{}; ///< the PREVIOUS volume (multi-bounce reads)
};

FUSE_HOST_DEVICE inline compute::RayMarchParams sdf_params(const SdfTraceScene& s) {
    compute::RayMarchParams rm{};
    rm.objects = s.objects;
    rm.object_count = s.objectCount;
    rm.max_dist = kSdfFar;
    rm.min_dist = s.minDistance;
    return rm;
}

/// Material row of the closest primitive at `p` (first on ties); UINT32_MAX when none.
FUSE_HOST_DEVICE inline u32 sdf_material(const SdfTraceScene& s, const math::Vec3& p) {
    f32 best = kSdfFar;
    u32 material = 0xFFFFFFFFu;
    for (u32 i = 0; i < s.objectCount; ++i) {
        const compute::SdfObject& o = s.objects[i];
        const f32 d = compute::ray_march_kernel::object_sdf(o, p - o.position, kSdfFar, nullptr);
        if (d < best) {
            best = d;
            material = o.material_id;
        }
    }
    return material;
}

/// T0 shadow ray toward the sun from `position` offset by shadowBias along `normal`.
FUSE_HOST_DEVICE inline bool sdf_sun_occluded(const SdfTraceScene& s, const compute::RayMarchParams& rm,
                                              const math::Vec3& position, const math::Vec3& normal) {
    const math::Vec3 origin = position + normal * s.shadowBias;
    f32 t = 0.f;
    for (u32 step = 0; step < s.maxSteps; ++step) {
        if (t >= kSdfShadowMax) {
            return false;
        }
        const f32 d = compute::ray_march_kernel::scene_eval(rm, origin + s.sunDirection * t, nullptr);
        if (d < s.minDistance) {
            return true;
        }
        t += d;
    }
    return t < kSdfShadowMax;
}

/// Radiance leaving a front-face hit: the oracle's direct_radiance (emissive + shadowed sun) plus its
/// multi-bounce term (albedo x the previous volume's irradiance / pi).
FUSE_HOST_DEVICE inline math::Vec3 sdf_hit_radiance(const SdfTraceScene& s, const compute::RayMarchParams& rm,
                                                    const math::Vec3& position, const math::Vec3& normal,
                                                    const math::Vec3& albedo, const math::Vec3& emissive) {
    math::Vec3 radiance = emissive;
    const f32 cos_sun = normal.dot(s.sunDirection);
    if (cos_sun > 0.f && ddgi_kernel::max_component(s.sunIrradiance) > 0.f && !sdf_sun_occluded(s, rm, position, normal)) {
        radiance = radiance + ddgi_kernel::mul(albedo, s.sunIrradiance) * (cos_sun * ddgi_kernel::kInvPi);
    }
    if (s.multiBounce) {
        radiance = radiance + ddgi_kernel::mul(albedo, ddgi_kernel::sample_irradiance(s.volume, position, normal)) *
                                  ddgi_kernel::kInvPi;
    }
    return radiance;
}

/// One T0 probe ray (`direction` unit): radiance arriving at `origin` and the distance the blend uses.
/// Rules: inside geometry (SDF < 0 at the probe) -> no light, distance = exit distance x backface scale;
/// sphere trace until SDF < minDistance (hit), t >= maxDistance (miss: sky, maxDistance) or maxSteps
/// (exhausted short of maxDistance = a grazing hit at t).
FUSE_HOST_DEVICE inline math::Vec3 sdf_trace_radiance(const SdfTraceScene& s, const math::Vec3& origin,
                                                      const math::Vec3& direction, f32& out_distance) {
    const compute::RayMarchParams rm = sdf_params(s);
    const f32 maxD = s.maxDistance;
    const f32 d0 = compute::ray_march_kernel::scene_eval(rm, origin, nullptr);
    if (d0 < 0.f) {
        f32 t = 0.f;
        for (u32 step = 0; step < s.maxSteps; ++step) {
            const f32 d = compute::ray_march_kernel::scene_eval(rm, origin + direction * t, nullptr);
            if (d >= 0.f || t >= maxD) {
                break;
            }
            t += std::max(-d, s.minDistance);
        }
        out_distance = std::min(t, maxD) * s.backfaceDistanceScale;
        return {};
    }
    f32 t = 0.f;
    bool hit = false;
    for (u32 step = 0; step < s.maxSteps; ++step) {
        if (t >= maxD) {
            break;
        }
        const f32 d = compute::ray_march_kernel::scene_eval(rm, origin + direction * t, nullptr);
        if (d < s.minDistance) {
            hit = true;
            break;
        }
        t += d;
    }
    if (!hit && t < maxD) {
        hit = true;
    }
    if (!hit) {
        out_distance = maxD;
        return s.skyRadiance;
    }
    const math::Vec3 position = origin + direction * t;
    const math::Vec3 normal = compute::ray_march_kernel::scene_normal(rm, position);
    math::Vec3 albedo{0.8f, 0.8f, 0.8f};
    math::Vec3 emissive{};
    const u32 material = sdf_material(s, position);
    if (material != 0xFFFFFFFFu && s.surfaces != nullptr && material < s.surfaceCount) {
        const DdgiSurface& row = s.surfaces[material];
        albedo = {row.albedo[0], row.albedo[1], row.albedo[2]};
        emissive = {row.emissive[0], row.emissive[1], row.emissive[2]};
    }
    out_distance = t;
    return sdf_hit_radiance(s, rm, position, normal, albedo, emissive);
}

/// The oracle's box scene as a global SDF: one sharp box primitive per box (hard union, material = box
/// index) and its surface rows.
void sdfSceneFromBoxes(const DdgiCpuScene& scene, std::vector<compute::SdfObject>& objects, std::vector<DdgiSurface>& surfaces);

/// DdgiFrameConstants from the oracle's settings (addresses left 0; DdgiGpu fills them).
DdgiFrameConstants makeFrameConstants(const DDGIDesc& volume, const DdgiCpuConfig& config, const DdgiGpuTuning& tuning,
                                      const DdgiFrameDesc& frame, u32 scheduled);

/// Explicit ray results of one update, run through the oracle's BlendKernel (CpuReference).
struct BlendReferenceInput {
    const DDGIDesc* volume = nullptr;
    const DdgiCpuConfig* config = nullptr;
    const u32* schedule = nullptr;
    u32 scheduled = 0;
    const math::Vec3* rayDirs = nullptr; ///< volume->rays_per_probe entries
    const math::Vec3* radiance = nullptr; ///< scheduled x rays
    const f32* distance = nullptr;        ///< scheduled x rays
    const math::Vec3* irradianceTexelDirs = nullptr; ///< irradiance_res^2
    const math::Vec3* distanceTexelDirs = nullptr;   ///< depth_res^2
};

/// Volume state the blend updates in place (the DdgiCpuVolume atlas layouts).
struct BlendReferenceState {
    std::vector<math::Vec3> irradiance;
    std::vector<math::Vec2> distance;
    std::vector<u32> updateCounts;
    u32 fastResponseTexels = 0; ///< added by the run
};

bool runBlendReference(const BlendReferenceInput& input, BlendReferenceState& state);

/// The CPU oracle's initial volume (DdgiCpuVolume::init): what ddgi.reset writes.
void initialVolumeState(const DDGIDesc& volume, const DdgiCpuConfig& config, BlendReferenceState& state);

/// ddgi_kernel::VolumeView over a state (for sample_irradiance / trace references).
ddgi_kernel::VolumeView volumeView(const DDGIDesc& volume, const DdgiCpuConfig& config, const BlendReferenceState& state);

} // namespace fuse::renderer::gi_gpu
