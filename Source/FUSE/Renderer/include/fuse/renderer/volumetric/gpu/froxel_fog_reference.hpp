#pragma once

// WP-8.1 froxel fog: host-side settings, frame-constant resolution and the CPU reference of the whole
// frame (every pass of froxel_fog_kernel.hpp over the grid / image). Vulkan-free: builds in the stub backend
// and is what the CPU gates (fuse_rp_volumetric_gpu_*) and the Lavapipe parity gates compare against.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_kernel.hpp>
#include <fuse/renderer/volumetric/gpu/froxel_fog_types.hpp>
#include <fuse/renderer/volumetric/volumetric_fog.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::volumetric_gpu {

/// Fog settings of one frame.
struct FroxelFogSettings {
    /// Froxel grid (clamped to kFogMaxGridX / Y, kFogMaxSlices). Aligned with the WP-2.1 clusters when
    /// gridX / gridY are multiples of the cluster tiles and the slices share near / far: every froxel then
    /// lies inside one cluster. Any grid is correct (each sample looks its own cluster up).
    u32 gridX = 160;
    u32 gridY = 90;
    u32 gridZ = 64;
    /// Fog range: slices span [camera near, min(farPlane, camera far)], exponentially (the B5 layout).
    f32 farPlane = 64.f;
    /// Global medium: the B5 VolumetricFogParams (density, anisotropy = HG g, fog_color = scattering albedo,
    /// height_falloff, base_height); march_steps is unused (the grid is the quadrature); receive_shadows
    /// enables the VSM lookup when a shadow address is given.
    VolumetricFogParams medium{};
    f32 ambient[3] = {0.f, 0.f, 0.f}; ///< isotropic ambient radiance
    FogVolume volumes[kFogMaxVolumes] = {};
    u32 volumeCount = 0;
    /// Temporal accumulation: history weight 1 - alpha; jittered samples (Halton 2 / 3 / 5, period 16).
    bool temporal = true;
    bool reproject = true; ///< false = same-froxel history (the ghosting control)
    bool jitter = true;
    f32 temporalAlpha = 0.05f;
};

/// Halton radical inverse of `index` in `base`.
f32 halton(u32 index, u32 base);
/// This frame's in-froxel sample offset ((0.5, 0.5, 0.5) without jitter).
void fogJitter(u32 frameIndex, bool jitter, f32 (&out)[3]);

/// Clamped grid (0 dimensions stay 0).
void clampFogGrid(u32& x, u32& y, u32& z);

/// Resolves the frame constants (addresses and handles left 0). `prev` = the previous frame's camera
/// (null or `historyValid` false: no history this frame). `width` / `height` = the apply extent.
/// False on an empty grid or an invalid camera (near <= 0, far <= near).
bool resolveFogConstants(const FroxelFogSettings& settings, const ClusterCameraDesc& camera,
                         const ClusterCameraDesc* prev, bool historyValid, u32 frameIndex, u32 width, u32 height,
                         FogFrameConstants& out);

/// The fields of the WP-2.1 LightingFrameConstants the fog reads (camera, grid, light count), resolved
/// exactly as ClusteredLighting::beginFrame does (clustered_kernel::make_camera of the clamped desc).
lighting_gpu::LightingFrameConstants makeLightingView(const ClusterDesc& desc, const ClusterCameraDesc& camera,
                                                      u32 lightCount);

/// Light lists as the GPU buffers hold them (u32 pairs grid, flat list, directional slots).
struct FogLightLists {
    std::vector<u32> grid;
    std::vector<u32> lightList;
    std::vector<u32> directional;
};

/// The oracle's lists (lighting_gpu::oracleLightGrid over the scene light table, translated to slots):
/// what WP-2.1's light.* passes produce (identical entry for entry, see the WP-2.1 gates).
void oracleFogLights(const ClusterDesc& desc, const ClusterCameraDesc& camera, const gpu_scene::GpuLight* lights,
                     u32 lightCount, FogLightLists& out);

fog_kernel::LightView makeLightView(const lighting_gpu::LightingFrameConstants* frame, const gpu_scene::GpuLight* lights,
                                    const FogLightLists& lists);

// --- whole-frame CPU references (the GPU passes' oracles) -----------------------------------------------
void injectReference(const FogFrameConstants& c, const fog_kernel::LightView& lights, std::vector<math::Vec4>& out);
void temporalReference(const FogFrameConstants& c, const std::vector<math::Vec4>& current,
                       const std::vector<math::Vec4>& history, std::vector<math::Vec4>& out);
void integrateReference(const FogFrameConstants& c, const std::vector<math::Vec4>& froxels, std::vector<math::Vec4>& out);
/// `depth` = RT4 device depth per pixel, `lit` = the lit image (width x height).
void applyReference(const FogFrameConstants& c, const std::vector<math::Vec4>& integrated, const std::vector<f32>& depth,
                    const std::vector<math::Vec4>& lit, std::vector<math::Vec4>& out);

/// Ground truth of a froxel's content: the mean of n^3 stratified samples (cell centres of an n^3
/// subdivision) of inject_point (convergence / ghosting gates).
void froxelAverageReference(const FogFrameConstants& c, const fog_kernel::LightView& lights, u32 n,
                            std::vector<math::Vec4>& out);
/// The mean over one jitter period (kFogJitterPeriod frames) of inject_froxel: what a converged static
/// history averages to over a period (the history is a linear filter of a periodic input).
void jitterPeriodMeanReference(const FogFrameConstants& c, const fog_kernel::LightView& lights,
                               std::vector<math::Vec4>& out);

} // namespace fuse::renderer::volumetric_gpu
