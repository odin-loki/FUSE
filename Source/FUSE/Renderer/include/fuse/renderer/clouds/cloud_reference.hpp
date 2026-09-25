#pragma once

// WP-8.3 volumetric clouds: settings, the per-frame record resolve and the CPU reference of every kernel
// (A. Schneider, "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015 / GPU Pro 7;
// S. Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016).
// The compute kernels (shaders/clouds/cl_*.comp / .slang, helpers in cl_common.{glsl,slang}) are line-for-line
// twins of the functions below, on the same CloudParams record, in f32.
//
//   noise volumes   baked once on the GPU (clouds.noise.shape / .detail / clouds.weather): integer-hash
//                   (PCG) tileable Perlin gradient noise and Worley F1 noise; shape r = Perlin-Worley
//                   (remap(perlin fBm, 0, 1, Worley fBm, 1)), g / b / a = Worley fBm at 2, 4, 8 x the base
//                   frequency; detail r / g / b = Worley fBm at 1, 2, 4 x; weather r coverage, g density
//                   multiplier, b cloud type. Sizes and frequencies are powers of two (texel centres never
//                   land on a cell edge, so floor() agrees on every device).
//   density         spherical shell [cloudBottom, cloudTop] above the planet; weather (bilinear, wrapped) ->
//                   coverage and type; type-blended trapezoid height gradient; base shape =
//                   remap(perlinWorley, lowFbm - 1, 1, 0, 1) x gradient, coverage remap; detail erosion
//                   (Worley fBm, inverted towards the top); x weather.g x densityScale = extinction (1/m).
//   single ray      first contiguous segment of the ray inside the shell (clipped at maxDistance, blocked by
//                   the ground); primarySteps samples at (i + jitter) dt; per sample with density: a light
//                   march of lightSteps midpoint samples to the layer top (capped at lightDistance), sun
//                   transfer = sum over octaves n of a^n phase(g c^n) exp(-b^n tau) (dual-lobe Henyey-
//                   Greenstein, the Wrenninge / Hillaire multiple-scattering approximation) x the view-
//                   dependent Beer-Powder factor; sun illuminance from the WP-8.2 atmosphere at the sample
//                   (at_sun_illuminance_at), ambient from its sky (5-direction upper-hemisphere average of
//                   at_sky_radiance) scaled from ambientBottom to 1 over the layer; energy-conserving step
//                   integration L += T albedo X (1 - exp(-sigma dt)); T-weighted mean depth.
//   temporal        one pixel of every block x block tile marched per frame (Bayer order; block 4 = 1/16);
//                   reconstruction reprojects every pixel into last frame's image through its block's cloud
//                   depth and the wind delta, takes a static pixel's history as is and a moving one through a
//                   ringing-clamped Catmull-Rom fetch (no blur accumulation), clamps a history whose depth
//                   disagrees with the fresh block's by more than depthRejection x (a disocclusion) or that
//                   comes from beyond the screen edge to the fresh 3 x 3 block neighbourhood,
//                   and blends a marched pixel as a running mean (maxHistoryCount static,
//                   motionHistoryCount moving); history outside the screen is rejected (the block's fresh
//                   sample).
//   composite       out = background x T + aerialT x L + aerialS x E_sun x (1 - T) at the cloud depth (WP-8.2
//                   at_aerial); background = a caller buffer or the atmosphere sky; opaque scene depth in
//                   front of the clouds keeps the background.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_lut_types.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/renderer/clouds/cloud_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::clouds {

struct CloudNoiseSettings {
    u32 shapeSize = 128; ///< power of two
    u32 detailSize = 32;
    u32 weatherSize = 128;
    u32 shapeFrequency = 4; ///< power of two, <= size
    u32 detailFrequency = 4;
    u32 weatherFrequency = 4;
    u32 seed = 1;
};

struct CloudResolution {
    u32 width = 256;     ///< cloud (reconstruction) resolution; multiples of block
    u32 height = 128;
    u32 block = 4;       ///< 1 (every pixel every frame), 2 (1/4) or 4 (1/16)
    u32 outWidth = 512;  ///< composite resolution
    u32 outHeight = 256;
};

struct CloudMedium {
    f32 planetRadius = 6360000.f; ///< used without an atmosphere (else AtParams::bottomRadius)
    f32 cloudBottom = 1500.f;
    f32 cloudTop = 4000.f;
    f32 maxDistance = 60000.f;
    f32 shapeScale = 1.f / 9000.f;
    f32 detailScale = 1.f / 1800.f;
    f32 weatherScale = 1.f / 45000.f;
    f32 coverageScale = 1.f;
    f32 coverageBias = 0.f;
    f32 densityScale = 0.03f;
    f32 detailStrength = 0.35f;
    f32 albedo = 0.99f;
    bool homogeneous = false; ///< analytic mode (density = densityScale inside the layer)
};

struct CloudLighting {
    f32 phaseForward = 0.8f;
    f32 phaseBackward = -0.3f;
    f32 phaseBlend = 0.3f;
    u32 octaves = 3;
    f32 msAttenuation = 0.5f;
    f32 msExtinction = 0.5f;
    f32 msEccentricity = 0.5f;
    f32 powderStrength = 0.5f;
    f32 ambientScale = 1.f;
    f32 ambientBottom = 0.5f;
};

struct CloudSampling {
    u32 primarySteps = 64;
    u32 lightSteps = 6;
    f32 lightDistance = 3000.f;
    f32 transmittanceCutoff = 0.005f;
};

struct CloudTemporalSettings {
    bool enabled = true;  ///< false: history ignored (every pixel must then be marched: block 1)
    bool jitter = true;
    bool reproject = true; ///< false: same-pixel history, never clamped (ghosting baseline)
    f32 staticThreshold = 0.01f;
    f32 maxHistoryCount = 8.f;
    f32 motionHistoryCount = 1.f;
    /// A reprojected history whose cloud depth and the fresh block's differ by more than this ratio (a
    /// disocclusion: a cloud against the far sky / horizon) is clamped to the fresh 3 x 3 block neighbourhood;
    /// so is a history fetched from beyond the screen edge. 1 = clamp every moving pixel.
    f32 depthRejection = 4.f;
};

/// Everything but the camera and the lighting inputs. Changing `noise` re-bakes the noise volumes;
/// changing `resolution` reallocates the frame buffer and drops the history.
struct CloudSettings {
    CloudNoiseSettings noise{};
    CloudResolution resolution{};
    CloudMedium medium{};
    CloudLighting lighting{};
    CloudSampling sampling{};
    CloudTemporalSettings temporal{};
};

struct CloudCamera {
    math::Vec3 position{0.f, 1.f, 0.f};
    math::Vec3 forward{0.f, 0.f, 1.f}; ///< normalised on resolve
    math::Vec3 right{1.f, 0.f, 0.f};
    math::Vec3 up{0.f, 1.f, 0.f};      ///< screen up
    f32 tanHalfFovX = 1.f;
    f32 tanHalfFovY = 0.5f;
};

/// Per-frame inputs.
struct CloudFrame {
    CloudCamera camera{};
    math::Vec3 windOffset{0.f, 0.f, 0.f}; ///< integrated wind displacement (metres); wrap it by the tile sizes
    /// WP-8.2 atmosphere (AtmosphereGpu::params() / frameAddress()): planet radius, sun direction, sun
    /// illuminance at the clouds, sky ambient, aerial perspective. Null / 0: the fallbacks below. The aerial
    /// perspective is read from the atmosphere's froxels at the screen uv: give AtmosphereLutView the same camera.
    const atmosphere::AtParams* atmosphere = nullptr;
    u64 atmosphereAddress = 0;
    bool aerialPerspective = true;
    bool sunDisk = true;
    math::Vec3 sunDirection{0.f, 0.5f, 1.f}; ///< without an atmosphere
    math::Vec3 sunIlluminance{1.f, 1.f, 1.f};
    math::Vec3 ambient{0.f, 0.f, 0.f};
    u64 backgroundAddress = 0; ///< optional f32x4 per output pixel (else the sky)
    u64 depthAddress = 0;      ///< optional f32 view distance per output pixel
};

/// Temporal state carried from frame to frame (VolumetricClouds keeps it; the CPU gates pass their own).
struct CloudHistoryState {
    CloudCamera previousCamera{};
    math::Vec3 previousWind{0.f, 0.f, 0.f};
    u32 frameIndex = 0;
    bool historyValid = false;
};

/// True when every size / count / scale is usable (powers of two, block in {1, 2, 4} dividing the
/// resolution, positive layer and steps, block 1 without temporal accumulation is fine).
bool cloud_settings_valid(const CloudSettings& settings);

/// Fills the record (buffer addresses stay 0 except atmosphere / background / depth). False for invalid
/// settings or a degenerate camera.
bool resolve_cloud_params(const CloudSettings& settings, const CloudFrame& frame, const CloudHistoryState& history,
                          CloudParams& out);

/// True when a and b bake the same noise volumes.
bool cloud_noise_equal(const CloudParams& a, const CloudParams& b);

/// Marched pixel inside the block for frame `index` (Bayer order: 4 x 4 -> every pixel once per 16 frames).
void cloud_block_offset(u32 block, u32 index, u32& x, u32& y);

// --- noise (the noise kernels' twins) ----------------------------------------------------------------------
u32 cl_hash(u32 v);
u32 cl_hash4(u32 x, u32 y, u32 z, u32 seed);
f32 cl_unit(u32 h);
/// Tileable Worley F1, inverted: 1 at feature points, 0 at >= 1 cell. q >= 0 in cells, period in cells.
f32 cl_worley(const math::Vec3& q, u32 period, u32 seed);
/// Tileable improved-Perlin gradient noise in about [-1, 1].
f32 cl_perlin(const math::Vec3& q, u32 period, u32 seed);
/// 3 octaves (x2 frequency, x0.5 amplitude), normalised to about [-1, 1].
f32 cl_perlin_fbm(const math::Vec3& q, u32 period, u32 seed);
/// 0.625 w(q) + 0.25 w(2q) + 0.125 w(4q).
f32 cl_worley_fbm(const math::Vec3& q, u32 period, u32 seed);

ClTexel shape_noise_texel(const CloudParams& p, u32 x, u32 y, u32 z);
ClTexel detail_noise_texel(const CloudParams& p, u32 x, u32 y, u32 z);
ClTexel weather_texel(const CloudParams& p, u32 x, u32 y);
void build_shape_noise(const CloudParams& p, std::vector<ClTexel>& out);
void build_detail_noise(const CloudParams& p, std::vector<ClTexel>& out);
void build_weather(const CloudParams& p, std::vector<ClTexel>& out);

/// CPU mirror of the noise sections.
struct ClNoiseView {
    const ClTexel* shape = nullptr;
    const ClTexel* detail = nullptr;
    const ClTexel* weather = nullptr;
};

/// The atmosphere as the cloud kernels see it (null members: no atmosphere).
struct ClAtmosphereView {
    const atmosphere::AtParams* params = nullptr;
    const atmosphere::AtLutView* luts = nullptr;
};

// --- sampling ---------------------------------------------------------------------------------------------
/// Trilinear fetch of an n^3 volume at unit tile coordinates (wrapped; texel centres at (i + 0.5) / n).
ClTexel cl_sample3(const ClTexel* volume, u32 n, f32 cx, f32 cy, f32 cz);
/// Bilinear fetch of an n^2 table at unit tile coordinates (wrapped).
ClTexel cl_sample2(const ClTexel* table, u32 n, f32 cx, f32 cy);

// --- density field ------------------------------------------------------------------------------------------
f32 cl_height_gradient(f32 h, f32 type);
/// Extinction (1/m) at world position `pos` whose height fraction in the layer is h.
f32 cl_density(const CloudParams& p, const ClNoiseView& noise, const math::Vec3& pos, f32 h);
/// Altitude above sea level of a world point (cancellation-free); r = planetRadius + altitude, up = local up.
f32 cl_altitude(const CloudParams& p, const math::Vec3& pos, f32& r, math::Vec3& up);
/// Extinction at a world point (the altitude computed from it).
f32 cl_density_at(const CloudParams& p, const ClNoiseView& noise, const math::Vec3& pos);

// --- geometry -----------------------------------------------------------------------------------------------
/// Roots of |o + t d - c| = planetRadius + H for a point at altitude alt (radius r), cos(zenith) mu.
bool cl_sphere(const CloudParams& p, f32 alt, f32 r, f32 mu, f32 H, f32& t0, f32& t1);
/// Altitude at distance t along (r, mu) from altitude alt (cancellation-free).
f32 cl_height(const CloudParams& p, f32 alt, f32 r, f32 mu, f32 t);
/// First segment of the ray inside the cloud layer, clipped to maxDistance.
bool cl_segment(const CloudParams& p, f32 alt, f32 r, f32 mu, f32& tStart, f32& tEnd);

// --- lighting and integration -------------------------------------------------------------------------------
f32 cl_hg(f32 cosTheta, f32 g);
f32 cl_phase(const CloudParams& p, f32 cosTheta, f32 eccentricity);
/// Sun transfer at light optical depth tau: multiple-scattering octaves x Beer-Powder.
f32 cl_sun_transfer(const CloudParams& p, f32 cosTheta, f32 tau);
/// Optical depth of the light march from a sample (altitude alt, local up) towards the sun.
f32 cl_light_depth(const CloudParams& p, const ClNoiseView& noise, const math::Vec3& pos, f32 alt, const math::Vec3& up);
/// Sky ambient at the layer top (x ambientScale), or CloudParams::ambient without an atmosphere.
math::Vec3 cl_ambient_top(const CloudParams& p, const ClAtmosphereView& atm);

struct ClRay {
    math::Vec3 scatter{0.f, 0.f, 0.f}; ///< in-scattered radiance
    f32 transmittance = 1.f;
    f32 depth = 0.f;    ///< transmittance-weighted mean distance, or the segment middle / maxDistance
    f32 hasCloud = 0.f; ///< 1 when any extinction was met
};

/// One ray (the single-ray integration of the march and probe kernels).
ClRay cl_integrate(const CloudParams& p, const ClNoiseView& noise, const ClAtmosphereView& atm, const math::Vec3& origin,
                   const math::Vec3& dir, f32 jitter);

/// View ray of screen uv (normalised) for the current / previous camera.
math::Vec3 cl_view_dir(const CloudParams& p, f32 u, f32 v);
/// Jitter of the pixel marched at (x, y) this frame.
f32 cl_jitter(const CloudParams& p, u32 x, u32 y);

// --- texel kernels -------------------------------------------------------------------------------------------
/// clouds.march: the fresh texel pair of block (fx, fy).
void march_texel(const CloudParams& p, const ClNoiseView& noise, const ClAtmosphereView& atm, u32 fx, u32 fy,
                 ClTexel out[2]);
/// clouds.reconstruct: the history texel pair of pixel (x, y).
void reconstruct_texel(const CloudParams& p, const ClTexel* fresh, const ClTexel* historyIn, u32 x, u32 y,
                       ClTexel out[2]);
/// clouds.composite: output pixel (x, y).
ClTexel composite_texel(const CloudParams& p, const ClTexel* history, const ClAtmosphereView& atm,
                        const ClTexel* background, const f32* depth, u32 x, u32 y);

} // namespace fuse::renderer::clouds
