#pragma once

// WP-6.1 DDGI on Vulkan: records shared by the C++ side (DdgiGpu, the CPU references) and the kernels
// (shaders/ddgi/*: GLSL + Slang twins). docs/unification/RENDERER-EXECUTION.md, renderer plan Phase 6a.
//
// Device-safe (only <fuse/types.hpp>). Layout rules as for the GPU scene (gpu_scene_types.hpp): 4-byte
// scalars, scalar arrays and 8-byte addresses, u64 members 8-byte aligned, sizes multiples of 16, no
// implicit padding (static_asserts below); the kernels read everything through buffer device addresses.
//
// Atlas layout (the CPU oracle's, DdgiCpuVolume::irradianceAtlas / distanceAtlas, byte for byte):
//   irradiance  probe-major bordered tiles of (irradiance_res + 2)^2 texels, 3 x f32 each (E / pi)
//   distance    probe-major bordered tiles of (depth_res + 2)^2 texels, 2 x f32 each (mean, mean^2)
// so a read-back atlas compares with the CPU volume element for element, and the sampling code
// (ddgi_sample.{glsl,slang}) is a line-by-line port of ddgi_kernel::sample_irradiance.

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::gi_gpu {

/// Largest irradiance / depth tile resolutions the blend kernel's shared memory holds.
inline constexpr u32 kMaxIrradianceRes = 16u;
inline constexpr u32 kMaxDepthRes = 32u;
/// Blend: one 64-thread workgroup per scheduled probe (== ddgi_kernel::kBlendThreads).
inline constexpr u32 kBlendThreads = 64u;
/// Ray generation / trace / reset / probe: 64-wide 1D workgroups.
inline constexpr u32 kWorkgroup = 64u;

/// DdgiVolumeView::flags / DdgiFrameConstants::flags.
enum DdgiFlag : u32 {
    kDdgiMultiBounce = 1u << 0,       ///< trace: add the previous volume's irradiance at hits
    kDdgiFrontFaceCcw = 1u << 1,      ///< trace (T2): triangles are counter-clockwise seen from outside
    kDdgiSunEnabled = 1u << 2,        ///< trace: the sun term is on (irradiance > 0)
};

/// What sampling needs (ddgi_sample.{glsl,slang} fuse_ddgi_sample_irradiance): the probe grid and the
/// two atlas addresses. The first 80 bytes of DdgiFrameConstants, so its address is the frame's.
struct DdgiVolumeView {
    f32 origin[3] = {0.f, 0.f, 0.f};
    u32 probeCount = 0;
    f32 spacing[3] = {1.f, 1.f, 1.f};
    u32 irradianceRes = 0;
    u32 dims[3] = {0u, 0u, 0u};
    u32 depthRes = 0;
    f32 normalBias = 0.1f;          ///< DdgiCpuConfig::normal_bias
    f32 weightCrushThreshold = 0.2f; ///< DdgiCpuConfig::weight_crush_threshold
    f32 intensity = 1.f;            ///< scale of the indirect diffuse the lighting shade adds
    u32 flags = 0;
    u64 irradiance = 0;             ///< f32 x 3 per bordered texel
    u64 distance = 0;               ///< f32 x 2 per bordered texel
};
static_assert(sizeof(DdgiVolumeView) == 80u, "DdgiVolumeView layout (ddgi_sample.glsl / .slang)");
static_assert(offsetof(DdgiVolumeView, normalBias) == 48u && offsetof(DdgiVolumeView, irradiance) == 64u,
              "DdgiVolumeView offsets");

/// Per-frame constants (host ring, one slot per frame in flight, read through BDA): 352 bytes.
struct DdgiFrameConstants {
    DdgiVolumeView volume{};
    f32 rotation[3][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}}; ///< ray-set rotation rows
    f32 sunDirection[3] = {0.f, 1.f, 0.f}; ///< unit, surface -> sun
    f32 maxRayDistance = 20.f;
    f32 sunIrradiance[3] = {0.f, 0.f, 0.f};
    f32 backfaceDistanceScale = 0.2f;
    f32 skyRadiance[3] = {0.f, 0.f, 0.f};
    f32 rayEpsilon = 1e-4f; ///< shadow-ray origin offset along the hit normal (T2; ddgi_kernel::kRayEpsilon)
    u32 raysPerProbe = 0;
    u32 scheduled = 0;      ///< probes updated this frame (schedule entries)
    u32 frameIndex = 0;
    u32 flags = 0;          ///< DdgiFlag
    // Blend (ddgi_kernel::BlendParams).
    f32 hysteresis = 0.97f;
    f32 probeChangeHysteresis = 0.f;
    f32 probeChangeThreshold = 0.1f;
    f32 changeThreshold = 0.25f;
    f32 changeHysteresisDrop = 0.75f;
    f32 changeFloor = 1e-3f;
    f32 distancePower = 50.f;
    f32 distanceMinCos = 0.f;
    // T0 sphere tracing.
    f32 sdfMinDistance = 1e-4f;
    u32 sdfMaxSteps = 256;
    u32 sdfCount = 0;
    f32 sdfShadowBias = 1e-3f;
    // T2 ray query.
    u32 traceMask = 0x7Fu;  ///< rt::kRtMaskAll
    u32 shadowMask = 0x2u;  ///< rt::kRtMaskShadow
    f32 initialIrradiance[3] = {0.f, 0.f, 0.f}; ///< ddgi.reset
    u32 sdfSurfaceCount = 0; ///< T0: DdgiSurface rows at sdfSurfaces
    // Addresses.
    u64 schedule = 0;            ///< u32 probe index per scheduled slot
    u64 rayDirs = 0;             ///< f32 x 4 per ray (ddgi.raygen output)
    u64 rays = 0;                ///< f32 x 4 per (slot, ray): radiance xyz, hit distance w (ddgi.trace output)
    u64 updateCounts = 0;        ///< u32 per probe
    u64 irradianceTexelDirs = 0; ///< f32 x 4 per interior irradiance texel
    u64 distanceTexelDirs = 0;   ///< f32 x 4 per interior depth texel
    u64 tlas = 0;                ///< T2: VkAccelerationStructureKHR device address
    u64 scene = 0;               ///< T2: GpuSceneHeader device address
    u64 sdfObjects = 0;          ///< T0: DdgiSdfObject[sdfCount]
    u64 sdfSurfaces = 0;         ///< T0: DdgiSurface per material_id
    u64 slotStats = 0;           ///< u32 per scheduled slot: texels with reduced hysteresis
};
static_assert(sizeof(DdgiFrameConstants) == 352u, "DdgiFrameConstants layout (ddgi_common.glsl / .slang)");
static_assert(offsetof(DdgiFrameConstants, rotation) == 80u && offsetof(DdgiFrameConstants, raysPerProbe) == 176u &&
                  offsetof(DdgiFrameConstants, hysteresis) == 192u && offsetof(DdgiFrameConstants, sdfMinDistance) == 224u &&
                  offsetof(DdgiFrameConstants, traceMask) == 240u && offsetof(DdgiFrameConstants, schedule) == 264u &&
                  offsetof(DdgiFrameConstants, slotStats) == 344u,
              "DdgiFrameConstants offsets");

/// Push constants of every DDGI kernel (32 bytes).
struct DdgiPush {
    u64 frame = 0; ///< DdgiFrameConstants address
    u64 aux = 0;   ///< ddgi.probe: DdgiProbePoint[count]; else 0
    u64 out = 0;   ///< ddgi.probe: f32 x 4 per point; else 0
    u32 count = 0; ///< ddgi.probe points / ddgi.reset texels
    u32 pad = 0;
};
static_assert(sizeof(DdgiPush) == 32u, "DdgiPush layout");

/// T0 SDF primitive: compute::SdfObject (Compute/include/fuse/compute/ray_march.hpp) byte for byte, so
/// the Compute agent's analytic scene uploads as is. type: 0 sphere, 1 box, 2 capsule, 3 torus.
struct DdgiSdfObject {
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 params[3] = {0.f, 0.f, 0.f};
    u32 type = 0;
    f32 alpha = 0.f;       ///< smooth-union width with the objects before it (<= 0: hard union)
    u32 materialId = 0;    ///< DdgiSurface row
    f32 rounding = 0.f;
};
static_assert(sizeof(DdgiSdfObject) == 40u, "DdgiSdfObject == compute::SdfObject (40 bytes)");

/// Lambertian surface of a T0 hit (the oracle's DdgiCpuSurface): 32 bytes.
struct DdgiSurface {
    f32 albedo[3] = {0.8f, 0.8f, 0.8f};
    f32 pad0 = 0.f;
    f32 emissive[3] = {0.f, 0.f, 0.f};
    f32 pad1 = 0.f;
};
static_assert(sizeof(DdgiSurface) == 32u, "DdgiSurface layout");

/// ddgi.probe input: a surface point and its normal (32 bytes).
struct DdgiProbePoint {
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 pad0 = 0.f;
    f32 normal[3] = {0.f, 1.f, 0.f};
    f32 pad1 = 0.f;
};
static_assert(sizeof(DdgiProbePoint) == 32u, "DdgiProbePoint layout");

} // namespace fuse::renderer::gi_gpu
