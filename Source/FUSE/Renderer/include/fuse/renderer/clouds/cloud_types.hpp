#pragma once

// WP-8.3 volumetric clouds (renderer plan P8 "Volumetric clouds (raymarched, temporally amortised)"; execution
// doc WP-8.3): the record shared by the C++ host code, the CPU reference and the compute kernels.
// shaders/clouds/cl_common.{glsl,slang} declare `struct CloudParams` with the same fields in the same order;
// fuse_rp_clouds_layout checks names, order and offsets. Vulkan-free: builds in the stub backend.
//
// Every table is an array of f32x4 texels (x fastest) in persistent device buffers reached through buffer
// device addresses, so the kernels compute in the CPU reference's f32 and sample with the same manual
// (tileable) trilinear / bilinear filters as the CPU twins in cloud_reference.hpp.
//
// Frame: world space, +Y up, metres, the planet centre at (0, -planetRadius, 0) (the WP-8.2 atmosphere frame).
// Screen: u right, v down, uv in [0, 1]^2; the view ray of uv is
// normalize(forward + (2u - 1) tanX right - (2v - 1) tanY up) (the aerial-perspective froxel convention).

#include <fuse/types.hpp>

namespace fuse::renderer::clouds {

/// Workgroup edge of the 2D kernels (8 x 8 threads) and of the 3D noise kernels (4 x 4 x 4).
inline constexpr u32 kClTile = 8u;
inline constexpr u32 kClTile3 = 4u;

/// CloudParams::flags
enum ClFlag : u32 {
    kClFlagHomogeneous = 1u << 0,  ///< analytic mode: density = densityScale everywhere inside the layer
    kClFlagJitter = 1u << 1,       ///< per-pixel, per-cycle start offset of the primary march (else 0.5)
    kClFlagHistory = 1u << 2,      ///< historyIn holds last frame's reconstruction (else it is ignored)
    kClFlagNoReproject = 1u << 3,  ///< ghosting baseline: history fetched at the same pixel, never clamped
    kClFlagAtmosphere = 1u << 4,   ///< `atmosphere` is an AtParams address: sun / sky / aerial perspective
    kClFlagAerial = 1u << 5,       ///< composite applies the atmosphere's aerial perspective at the cloud depth
    kClFlagBackground = 1u << 6,   ///< composite reads `background` (f32x4 per output pixel), else the sky
    kClFlagSceneDepth = 1u << 7,   ///< composite reads `depth` (f32 view distance per output pixel)
    kClFlagSunDisk = 1u << 8,      ///< composite's sky background includes the sun disk
};

/// Per-frame record (one host-visible ring slot per frame in flight, read through BDA).
struct CloudParams {
    // --- addresses (BDA) ---------------------------------------------------------------------------
    u64 shapeNoise = 0;  ///< shapeSize^3 f32x4: r Perlin-Worley, g / b / a Worley fBm at 2, 4, 8 x frequency
    u64 detailNoise = 0; ///< detailSize^3 f32x4: r / g / b Worley fBm at 1, 2, 4 x frequency, a = 0
    u64 weather = 0;     ///< weatherSize^2 f32x4: r coverage, g density multiplier, b cloud type, a = 0
    u64 fresh = 0;       ///< freshWidth x freshHeight x 2 f32x4: this frame's marched pixels
    u64 historyIn = 0;   ///< width x height x 2 f32x4: last frame's reconstruction
    u64 historyOut = 0;  ///< width x height x 2 f32x4: this frame's reconstruction
    u64 result = 0;      ///< outWidth x outHeight f32x4: composited radiance, a = cloud transmittance
    u64 background = 0;  ///< optional f32x4 per output pixel (kClFlagBackground)
    u64 depth = 0;       ///< optional f32 view distance per output pixel (kClFlagSceneDepth)
    u64 atmosphere = 0;  ///< WP-8.2 AtParams address (kClFlagAtmosphere)
    // --- extents, sample counts, frame --------------------------------------------------------------
    u32 shapeSize = 0;
    u32 detailSize = 0;
    u32 weatherSize = 0;
    u32 shapeFrequency = 0;   ///< Perlin / Worley cells per shape tile (base octave)
    u32 detailFrequency = 0;  ///< Worley cells per detail tile (base octave)
    u32 weatherFrequency = 0; ///< Perlin / Worley cells per weather tile
    u32 seed = 0;
    u32 flags = 0; ///< ClFlag
    u32 width = 0;  ///< cloud (reconstruction) resolution
    u32 height = 0;
    u32 freshWidth = 0; ///< width / block
    u32 freshHeight = 0;
    u32 block = 0;   ///< 1, 2 or 4: one pixel of every block x block tile is marched per frame
    u32 offsetX = 0; ///< this frame's marched pixel inside the block (Bayer order)
    u32 offsetY = 0;
    u32 frameIndex = 0;
    u32 outWidth = 0; ///< composite resolution
    u32 outHeight = 0;
    u32 primarySteps = 0;
    u32 lightSteps = 0;
    u32 octaves = 0; ///< multiple-scattering octaves (1 = single scattering)
    u32 cycle = 0;   ///< frameIndex / (block * block): the jitter sequence index of this frame's pixels
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    // --- layer, medium, lighting, temporal -----------------------------------------------------------
    f32 planetRadius = 0.f;  ///< metres
    f32 cloudBottom = 0.f;   ///< layer altitudes above sea level, metres
    f32 cloudTop = 0.f;
    f32 maxDistance = 0.f;   ///< primary march clip distance, metres
    f32 shapeScale = 0.f;    ///< shape tiles per metre
    f32 detailScale = 0.f;   ///< detail tiles per metre
    f32 weatherScale = 0.f;  ///< weather tiles per metre (xz)
    f32 coverageScale = 0.f; ///< coverage = saturate(weather.r * coverageScale + coverageBias)
    f32 coverageBias = 0.f;
    f32 densityScale = 0.f;  ///< extinction (1/m) at density 1
    f32 detailStrength = 0.f;
    f32 albedo = 0.f;        ///< single-scattering albedo
    f32 phaseForward = 0.f;  ///< dual-lobe Henyey-Greenstein: g of the forward lobe
    f32 phaseBackward = 0.f; ///< g of the backward lobe
    f32 phaseBlend = 0.f;    ///< weight of the backward lobe
    f32 msAttenuation = 0.f; ///< octave n: scattering x a^n
    f32 msExtinction = 0.f;  ///< octave n: light optical depth x b^n
    f32 msEccentricity = 0.f;///< octave n: g x c^n
    f32 powderStrength = 0.f;///< Beer-Powder blend (0 = Beer only)
    f32 lightDistance = 0.f; ///< light march length cap, metres (the march stops at the layer top before)
    f32 ambientScale = 0.f;  ///< sky ambient multiplier
    f32 ambientBottom = 0.f; ///< ambient at the layer bottom relative to the top
    f32 transmittanceCutoff = 0.f; ///< the primary march stops below this view transmittance
    f32 staticThreshold = 0.f;     ///< reprojection motion (pixels) below which the history is taken as is
    f32 maxHistoryCount = 0.f;     ///< running-mean length of a static pixel
    f32 motionHistoryCount = 0.f;  ///< running-mean length of a moving pixel
    f32 depthRejection = 0.f;     ///< history whose depth differs by more than this ratio is clamped
    f32 reserved3 = 0.f;
    // --- vectors --------------------------------------------------------------------------------------
    f32 sunDir[4] = {};        ///< unit, towards the sun
    f32 sunIlluminance[4] = {};///< without an atmosphere: rgb illuminance at the clouds
    f32 ambient[4] = {};       ///< without an atmosphere: rgb ambient radiance at the layer top
    f32 windOffset[4] = {};    ///< the noise is sampled at (p - windOffset): clouds move along +windOffset
    f32 windDelta[4] = {};     ///< windOffset - last frame's windOffset
    f32 cameraPos[4] = {};
    f32 camForward[4] = {};    ///< w = tan(horizontal half fov)
    f32 camRight[4] = {};      ///< w = tan(vertical half fov)
    f32 camUp[4] = {};
    f32 prevCameraPos[4] = {};
    f32 prevForward[4] = {};   ///< w = tan(horizontal half fov)
    f32 prevRight[4] = {};     ///< w = tan(vertical half fov)
    f32 prevUp[4] = {};
};
static_assert(sizeof(CloudParams) == 496u, "CloudParams layout (mirrored by cl_common.glsl / .slang)");

/// Push constants of every kernel.
struct CloudPush {
    u64 params = 0; ///< BDA of this frame's CloudParams
    u64 src = 0;    ///< cl_probe: probe inputs (kClProbeInputs f32x4 per probe)
    u64 dst = 0;    ///< cl_probe: probe outputs (kClProbeOutputs f32x4 per probe)
    u32 mode = 0;   ///< cl_probe: probe count
    u32 reserved = 0;
};
static_assert(sizeof(CloudPush) == 32u, "CloudPush layout");

/// cl_probe: per probe the inputs are {ray origin xyz, jitter}, {ray direction xyz, -} and the outputs
///   0 {in-scattered radiance rgb, view transmittance}   1 {cloud depth, has cloud, density at the origin, -}
inline constexpr u32 kClProbeInputs = 2u;
inline constexpr u32 kClProbeOutputs = 2u;

/// One texel as stored on the device.
struct ClTexel {
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    f32 a = 0.f;
};
static_assert(sizeof(ClTexel) == 16u, "ClTexel is f32x4");

} // namespace fuse::renderer::clouds
