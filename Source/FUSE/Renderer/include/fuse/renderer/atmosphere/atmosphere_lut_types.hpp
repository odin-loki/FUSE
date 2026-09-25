#pragma once

// WP-8.2 Hillaire atmosphere (renderer plan P8 "Physically based sky and atmosphere (Hillaire-style LUTs)";
// execution doc WP-8.2): the record shared by the C++ host code, the CPU reference and the compute kernels.
// shaders/atmosphere/at_sample.{glsl,slang} declare `struct AtParams` with the same fields in the same order;
// fuse_rp_atmosphere_gpu_layout checks names, order and offsets. Vulkan-free: builds in the stub backend.
//
// Every LUT is an array of f32x4 texels (row-major, x fastest) in one persistent device buffer reached
// through buffer device addresses, so the kernels compute in the CPU reference's f32 and every consumer
// samples with the same manual bilinear (trilinear for the froxel volume) filter as the CPU helpers.
//
// Frame: world space, +Y up, metres. The planet centre is at (0, -bottomRadius, 0), so y = 0 is sea level
// under the world origin. The LUTs hold radiance per unit solar irradiance (sunIlluminance scales it).

#include <fuse/types.hpp>

namespace fuse::renderer::atmosphere {

/// Workgroup edge of the 2D kernels (8 x 8 threads, one texel / froxel column each).
inline constexpr u32 kAtTile = 8u;

/// AtParams::flags
enum AtFlag : u32 {
    kAtFlagMultiScatter = 1u << 0, ///< sky-view / aerial perspective add the multi-scattering LUT term
};

/// Per-frame record (one host-visible ring slot per frame in flight, read through BDA). Consumers of the
/// sampling helpers (at_sample.glsl / .slang) get its address from AtmosphereGpu::frameAddress().
struct AtParams {
    // --- LUT sections (BDA; f32x4 texels) ----------------------------------------------------------
    u64 transmittance = 0;       ///< transWidth x transHeight: rgb transmittance to the top, a = 0
    u64 multiscatter = 0;        ///< msWidth x msHeight: rgb Psi_ms (x msFactor), a = f_ms (grey transfer)
    u64 skyView = 0;             ///< skyWidth x skyHeight: rgb in-scattered radiance, a = 0
    u64 aerialScatter = 0;       ///< apWidth x apHeight x apDepth: rgb in-scattered radiance, a = 0
    u64 aerialTransmittance = 0; ///< same extent: rgb view transmittance, a = 0
    u64 reserved0 = 0;
    // --- extents and sample counts -----------------------------------------------------------------
    u32 transWidth = 0;
    u32 transHeight = 0;
    u32 msWidth = 0;
    u32 msHeight = 0;
    u32 skyWidth = 0;
    u32 skyHeight = 0;
    u32 apWidth = 0;
    u32 apHeight = 0;
    u32 apDepth = 0;
    u32 flags = 0;          ///< AtFlag
    u32 transSteps = 0;     ///< log-linear segments per transmittance ray
    u32 msSteps = 0;        ///< march steps per multi-scattering direction
    u32 msDirSqrt = 0;      ///< msDirSqrt^2 uniform sphere directions per multi-scattering texel
    u32 skySteps = 0;       ///< march steps per sky-view texel
    u32 apStepsPerSlice = 0;///< march steps per froxel slice
    u32 reserved1 = 0;
    // --- medium -------------------------------------------------------------------------------------
    f32 bottomRadius = 0.f; ///< metres (AtmosphereParams::earth_radius)
    f32 topRadius = 0.f;    ///< metres (AtmosphereParams::atmo_radius)
    f32 rayleighHeight = 0.f;
    f32 mieHeight = 0.f;
    f32 rayleighScattering[4] = {}; ///< 1/m at sea level (rgb); Rayleigh does not absorb
    f32 mieScattering = 0.f;        ///< 1/m at sea level
    f32 mieExtinction = 0.f;        ///< mieScattering / kMieSingleScatteringAlbedo
    f32 mieG = 0.f;                 ///< Cornette-Shanks asymmetry
    f32 msFactor = 0.f;             ///< multi-scattering LUT scale (1 = physical)
    f32 ozoneAbsorption[4] = {};    ///< 1/m at the tent peak (rgb); 0 = no ozone (the B5 reference has none)
    f32 ozoneCenter = 0.f;          ///< metres
    f32 ozoneHalfWidth = 0.f;       ///< metres (tent: max(0, 1 - |h - centre| / halfWidth))
    f32 sunAngularRadius = 0.f;     ///< radians
    f32 sunDiskNorm = 0.f;          ///< 1 / (limb-darkened solid angle): disk radiance per unit irradiance
    f32 groundAlbedo[4] = {};       ///< Lambertian ground seen by the multi-scattering LUT
    // --- frame --------------------------------------------------------------------------------------
    f32 sunIlluminance[4] = {};     ///< rgb scale applied by the sampling helpers (LUTs are per unit irradiance)
    f32 sunDir[4] = {};             ///< unit direction towards the sun; w = cos(sun zenith) at the camera
    f32 up[4] = {};                 ///< unit local up at the camera; w = camera radius (clamped into the shell)
    f32 cameraPos[4] = {};          ///< world camera position; w = aerial-perspective max distance (metres)
    f32 camForward[4] = {};         ///< unit; w = tan(horizontal half fov)
    f32 camRight[4] = {};           ///< unit; w = tan(vertical half fov)
    f32 camUp[4] = {};              ///< unit (screen up); w = 0
};
static_assert(sizeof(AtParams) == 320u, "AtParams layout (mirrored by at_sample.glsl / .slang)");

/// Push constants of every kernel.
struct AtPush {
    u64 params = 0; ///< BDA of this frame's AtParams
    u64 src = 0;    ///< at_probe: probe inputs (3 f32x4 per probe)
    u64 dst = 0;    ///< at_probe: probe outputs (kAtProbeOutputs f32x4 per probe)
    u32 mode = 0;   ///< at_probe: probe count
    u32 reserved = 0;
};
static_assert(sizeof(AtPush) == 32u, "AtPush layout");

/// at_probe (the sampling helpers run on the device, for gates and tools): per probe the inputs are
/// {world direction xyz, -}, {screen u, v, view distance, -}, {world position xyz, -} and the outputs
///   0 at_sky_radiance(dir, sun disk)   1 at_sky_radiance(dir, no disk)   2 at_sun_illuminance_at(pos)
///   3 at_aerial scatter                4 at_aerial transmittance          5 at_multiscatter(camera r, sun)
inline constexpr u32 kAtProbeInputs = 3u;
inline constexpr u32 kAtProbeOutputs = 6u;

/// One LUT texel as stored on the device.
struct AtTexel {
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    f32 a = 0.f;
};
static_assert(sizeof(AtTexel) == 16u, "AtTexel is f32x4");

} // namespace fuse::renderer::atmosphere
