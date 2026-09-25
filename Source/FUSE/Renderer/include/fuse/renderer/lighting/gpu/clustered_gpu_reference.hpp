#pragma once

// WP-2.1 CPU references of the GPU clustered lighting:
//
//  * light lists: the B5 oracle (ClusteredLightCuller / cluster_math, the single-source clustered
//    kernels) takes point lights (WP-2.2: and area lights, by their range sphere) then spot lights as two arrays and names a light by its index in
//    that order; the GPU names a light by its GpuScene slot. makeOracleLights() builds the oracle's
//    inputs from the scene's light table (point slots ascending, then spot slots ascending; free
//    slots and directional lights are not clustered) plus the index -> slot map, and
//    translateToSlots() rewrites an oracle grid in slot names. When every point slot is below every
//    spot slot the map is monotone, so the oracle's ascending-index lists (and its "keep the lowest
//    indices" overflow rule) are exactly the GPU's ascending-slot lists: the GPU lists must equal the
//    translated oracle lists entry for entry, offsets included.
//  * shading: shadeReferenceFrame() runs the "light.shade" reference kernel (clustered_gpu_kernel.hpp)
//    on decoded G-buffer texels, the scene light table and a light grid.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::lighting_gpu {

/// Oracle inputs built from a scene light table.
struct OracleLights {
    std::vector<PointLightInput> points;
    std::vector<SpotLightInput> spots;
    std::vector<u32> slotOfIndex; ///< oracle light index (points, then spots) -> scene slot
    std::vector<u32> directional; ///< directional slots, ascending (the GPU's directional list)
    /// Every point slot is below every spot slot: oracle order == slot order.
    bool monotone = true;
};

/// Point / spot rows -> PointLightInput / SpotLightInput (position, range as radius, colour,
/// intensity, direction, cone angles from cosInner / cosOuter).
void makeOracleLights(const gpu_scene::GpuLight* lights, u32 count, OracleLights& out);

/// Rewrites `oracle` (light indices) as `out` (scene slots); offsets and counts are unchanged.
void translateToSlots(const ClusterGridSoA& oracle, const OracleLights& lights, ClusterGridSoA& out);

/// The oracle's light assignment exactly as ClusteredLightCuller::cullLights runs it (clamped desc,
/// buildClusterAabbs, cullLightsToClusterLists, compactClusterLists), without the culler's
/// ResourceManager buffers; returns the oracle grid in light indices.
ClusterCullResult oracleLightGrid(const ClusterDesc& desc,
                                  const ClusterCameraDesc& camera,
                                  const OracleLights& lights,
                                  ClusterGridSoA& outGrid,
                                  kernel::Backend backend = kernel::Backend::CpuParallel);

struct ShadeReferenceDesc {
    u32 width = 0;
    u32 height = 0;
    ClusterDesc desc{};
    ClusterCameraDesc camera{};
    math::Vec3 ambient{};
    GBufferTexels gbuffer{};
    const gpu_scene::GpuLight* lights = nullptr;
    u32 lightCount = 0;
    const ClusterGridSoA* grid = nullptr;      ///< in scene slots
    const std::vector<u32>* directional = nullptr;
    /// WP-2.2: ltc::kLutWords f32 (ltc::BrdfLut::data(), ClusteredLighting::brdfLut()): compensated
    /// BRDF + area lights, as the GPU with a LUT; null = the WP-2.1 lobe.
    const f32* brdfLut = nullptr;
    /// WP-3.2: ShadeParams::shadow / shadow_user (null = unshadowed).
    f32 (*shadow)(const void* user, u32 slot, const SurfaceSample& s) = nullptr;
    const void* shadowUser = nullptr;
    /// WP-6.2: ShadeParams::rt_shadow / rt_shadow_user (null = no ray-traced shadows).
    f32 (*rtShadow)(const void* user, u32 slot, u32 px, u32 py) = nullptr;
    const void* rtShadowUser = nullptr;
    /// LightingFrameDesc::skipAreaLights (kFlagSkipAreaLights): ShadeParams::skip_area_lights.
    bool skipAreaLights = false;
};

/// Shades every pixel ("light.shade" kernel on `backend`; CpuReference and CpuParallel are
/// bit-identical). `out` gets width * height RGBA (alpha 1 = shaded, 0 = sky / out of range).
/// Returns the number of shaded pixels.
u32 shadeReferenceFrame(const ShadeReferenceDesc& desc, std::vector<math::Vec4>& out,
                        kernel::Backend backend = kernel::Backend::CpuParallel);

/// ShadeParams for `desc` writing into `out` (width * height entries).
ShadeParams makeShadeParams(const ShadeReferenceDesc& desc, math::Vec4* out);

} // namespace fuse::renderer::lighting_gpu
