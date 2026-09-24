#pragma once

// WP-8.2 Hillaire atmosphere: CPU reference of every LUT and of the sampling helpers
// (S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020).
// The compute kernels (shaders/atmosphere/at_*.comp / .slang) and the includable sampling helpers
// (shaders/atmosphere/at_sample.glsl / .slang) are line-for-line twins of the functions below, on the same
// AtParams record, in f32.
//
//   transmittance LUT   (r, mu) -> transmittance to the top of the atmosphere; Bruneton's (x_mu, x_r)
//                       parametrisation (mu from the zenith down to the horizon), log-linear density segments
//   multi-scattering    (r, mu_s) -> Psi_ms = L_2nd / (1 - f_ms): isotropic second-order luminance from
//                       msDirSqrt^2 uniform sphere directions (with the Lambertian ground) summed as the
//                       geometric series of all higher orders (Hillaire 2020 §5.5); alpha = max_c f_ms
//   sky-view LUT        (azimuth to the sun, view zenith) at the camera radius; u = sqrt((1 - cos phi) / 2),
//                       v non-linear around the horizon (Hillaire 2020 §5.3); single scattering + Psi_ms
//   aerial perspective  camera froxels (x, y over the screen, slice z at distance maxDist ((z + 1) / D)^2):
//                       in-scattered radiance and rgb view transmittance, one incremental march per column
//
// Texel convention of the (x_mu, x_r), (mu_s, h) and sky-view LUTs: texel i of N sits at unit coordinate
// i / (N - 1), so the table edges are exact (the horizon column of the transmittance LUT, the zenith / nadir
// rows of the sky view) and bilinear filtering is fetch(f = x (N - 1)). The froxel volume uses screen
// texel centres ((x + 0.5) / W) and the slice mapping above.
//
// The medium is the B5 model of AtmosphereParams (Rayleigh + Mie, Mie single-scattering albedo
// kMieSingleScatteringAlbedo, no ozone), plus optional ozone absorption and ground albedo for the
// multi-scattering LUT. The LUTs hold radiance per unit solar irradiance; the helpers scale by
// AtParams::sunIlluminance.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_lut_types.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::atmosphere {

struct AtmosphereLutSizes {
    u32 transWidth = 256;
    u32 transHeight = 64;
    u32 msWidth = 32;  ///< mu_s
    u32 msHeight = 32; ///< altitude
    u32 skyWidth = 192;
    u32 skyHeight = 108;
    u32 apWidth = 32;
    u32 apHeight = 32;
    u32 apDepth = 32;
};

struct AtmosphereLutSampling {
    u32 transSteps = 128;
    u32 msSteps = 20;
    u32 msDirSqrt = 8;
    u32 skySteps = 32;
    u32 apStepsPerSlice = 2;
};

/// Static part: changing any of it rebuilds the transmittance and multi-scattering LUTs.
struct AtmosphereLutSettings {
    AtmosphereParams atmosphere{};
    math::Vec3 ozoneAbsorption{0.f, 0.f, 0.f}; ///< Hillaire's Earth: {0.650e-6, 1.881e-6, 0.085e-6}
    f32 ozoneCenter = 25000.f;
    f32 ozoneHalfWidth = 15000.f;
    math::Vec3 groundAlbedo{0.3f, 0.3f, 0.3f};
    f32 multiScatterFactor = 1.f;
    bool multiScattering = true; ///< false: sky view / aerial perspective are single scattering only
    AtmosphereLutSizes sizes{};
    AtmosphereLutSampling sampling{};
};

/// Per-frame part.
struct AtmosphereLutView {
    math::Vec3 cameraPosition{0.f, 1.f, 0.f}; ///< world metres; sea level under the origin at y = 0
    math::Vec3 sunDirection{0.f, 0.5f, 1.f};  ///< towards the sun (normalised on resolve)
    math::Vec3 sunIlluminance{1.f, 1.f, 1.f};
    // Camera basis of the aerial-perspective froxels (normalised on resolve; screen y points down).
    math::Vec3 forward{0.f, 0.f, 1.f};
    math::Vec3 right{1.f, 0.f, 0.f};
    math::Vec3 up{0.f, 1.f, 0.f};
    f32 tanHalfFovX = 1.f;
    f32 tanHalfFovY = 0.5625f;
    f32 aerialMaxDistance = 32000.f;
};

/// Fills the record (LUT addresses stay 0; AtmosphereGpu sets them). False for an invalid setting
/// (a LUT edge < 2, a zero sample count, top <= bottom radius, zero vectors, non-positive fov / distance).
bool resolve_at_params(const AtmosphereLutSettings& settings, const AtmosphereLutView& view, AtParams& out);

/// True when a and b build the same transmittance / multi-scattering LUTs (medium, sizes, sample counts).
bool at_static_equal(const AtParams& a, const AtParams& b);

/// CPU mirror of the device sections (any pointer may be null when unused).
struct AtLutView {
    const AtTexel* transmittance = nullptr;
    const AtTexel* multiscatter = nullptr;
    const AtTexel* skyView = nullptr;
    const AtTexel* aerialScatter = nullptr;
    const AtTexel* aerialTransmittance = nullptr;
};

// --- geometry -------------------------------------------------------------------------------------------
/// Height above the ground at distance t from radius r0 along cos(zenith) mu (cancellation-free).
f32 at_height(const AtParams& p, f32 r0, f32 mu, f32 t);
/// Distance to the top of the atmosphere (0 when outside and not entering).
f32 at_distance_to_top(const AtParams& p, f32 r0, f32 mu);
/// Distance to the ground, or < 0 when the ray misses it.
f32 at_distance_to_ground(const AtParams& p, f32 r0, f32 mu);

// --- parametrisations -----------------------------------------------------------------------------------
/// Transmittance LUT unit coordinates -> (r, mu).
void at_transmittance_r_mu(const AtParams& p, f32 xMu, f32 xR, f32& r, f32& mu);
/// (r, mu above the horizon) -> transmittance LUT unit coordinates.
void at_transmittance_uv(const AtParams& p, f32 r, f32 mu, f32& xMu, f32& xR);
/// Sky-view unit coordinates -> (cos view zenith, cos of the azimuth to the sun) at radius r.
void at_sky_view_dir(const AtParams& p, f32 r, f32 u, f32 v, f32& mu, f32& lightViewCos);
/// (cos view zenith, cos azimuth to the sun) at radius r -> sky-view unit coordinates.
void at_sky_view_uv(const AtParams& p, f32 r, f32 mu, f32 lightViewCos, f32& u, f32& v);

// --- texel kernels (the compute kernels' twins) ----------------------------------------------------------
AtTexel transmittance_texel(const AtParams& p, u32 x, u32 y);
AtTexel multiscatter_texel(const AtParams& p, const AtLutView& lut, u32 x, u32 y);
/// The two integrals behind a multi-scattering texel (per channel): L_2nd and f_ms.
void multiscatter_integrals(const AtParams& p, const AtLutView& lut, f32 r, f32 muS, math::Vec3& l2, math::Vec3& fms);
AtTexel sky_view_texel(const AtParams& p, const AtLutView& lut, u32 x, u32 y);
/// One froxel column: apDepth texels each, strided by apWidth * apHeight (the volume layout).
void aerial_column(const AtParams& p, const AtLutView& lut, u32 x, u32 y, AtTexel* scatter, AtTexel* transmittance);
/// Single + (flag) multi-scattered in-scatter along a ray from radius r0 (per unit irradiance), and its
/// transmittance: the sky-view integrator for an arbitrary ray (tests: sky view at any direction, no LUT).
math::Vec3 at_integrate_sky(const AtParams& p, const AtLutView& lut, f32 r0, f32 mu, f32 nu, f32 muS0, u32 steps,
                            math::Vec3* transmittance = nullptr);
/// As at_integrate_sky over [0, tEnd] (the sky-view kernel decides the ground test by its row).
math::Vec3 at_integrate_sky_to(const AtParams& p, const AtLutView& lut, f32 r0, f32 mu, f32 nu, f32 muS0, f32 tEnd,
                               u32 steps, math::Vec3* transmittance = nullptr);

// --- builders --------------------------------------------------------------------------------------------
void build_transmittance_lut(const AtParams& p, std::vector<AtTexel>& out);
void build_multiscatter_lut(const AtParams& p, const AtLutView& lut, std::vector<AtTexel>& out);
void build_sky_view_lut(const AtParams& p, const AtLutView& lut, std::vector<AtTexel>& out);
void build_aerial_perspective(const AtParams& p, const AtLutView& lut, std::vector<AtTexel>& scatter,
                              std::vector<AtTexel>& transmittance);

/// All four LUTs on the CPU.
struct AtCpuLuts {
    std::vector<AtTexel> transmittance;
    std::vector<AtTexel> multiscatter;
    std::vector<AtTexel> skyView;
    std::vector<AtTexel> aerialScatter;
    std::vector<AtTexel> aerialTransmittance;

    void build(const AtParams& p);
    AtLutView view() const;
};

// --- sampling helpers (at_sample.glsl / .slang twins) ----------------------------------------------------
/// Bilinear fetch of a W x H f32x4 table at texel coordinates (fx, fy) (clamped; W, H >= 2).
AtTexel at_bilinear(const AtTexel* table, u32 w, u32 h, f32 fx, f32 fy);
/// Transmittance from radius r along mu to the top (0 when the planet occludes the ray).
math::Vec3 at_transmittance(const AtParams& p, const AtLutView& lut, f32 r, f32 mu);
/// Sun transmittance at radius r for sun cos(zenith) muS, times the visible fraction of the 0.5 degree disk
/// above the horizon (continuous planet-shadow penumbra, as atmosphere_sun_transmittance).
math::Vec3 at_sun_transmittance(const AtParams& p, const AtLutView& lut, f32 r, f32 muS);
/// Psi_ms at radius r for sun cos(zenith) muS.
math::Vec3 at_multiscatter(const AtParams& p, const AtLutView& lut, f32 r, f32 muS);
/// Sky-view LUT for a world direction from the camera (per unit irradiance, no sun disk).
math::Vec3 at_sky_view(const AtParams& p, const AtLutView& lut, const math::Vec3& dir);
/// Aerial perspective at screen uv in [0, 1]^2 (y down) and view distance (metres).
void at_aerial(const AtParams& p, const AtLutView& lut, f32 u, f32 v, f32 distance, math::Vec3& scatter,
               math::Vec3& transmittance);
/// Sky radiance (x sunIlluminance) for a world direction, + the transmitted limb-darkened sun disk.
math::Vec3 at_sky_radiance(const AtParams& p, const AtLutView& lut, const math::Vec3& dir, bool sunDisk);
/// Sun transmittance (x sunIlluminance: the sun illuminance reaching a world point) for lighting.
math::Vec3 at_sun_illuminance_at(const AtParams& p, const AtLutView& lut, const math::Vec3& worldPos);

} // namespace fuse::renderer::atmosphere
