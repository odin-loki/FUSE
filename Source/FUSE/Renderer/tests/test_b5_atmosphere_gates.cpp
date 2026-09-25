// B5.12 gate rows — Atmosphere & Sky (B5.8) and volumetric height fog (B5.11).
//
// CI has no GPU, so each row is proven against the CPU implementation of the same math the sky /
// fog kernels evaluate, checked against independent references (closed-form integrals and
// double-precision quadrature written here, not shared with the implementation).
//
// Rows:
//   1. Sky renders correct Rayleigh scattering — blue midday, orange/red at low sun angles
//   2. No banding artifacts in sky gradient — verified at 10-bit output
//   3. Sun disk correct angular size (0.5° apparent diameter)
//   4. Volumetric fog density falloff matches analytic exponential reference

#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/renderer/atmosphere/sky_lut.hpp>
#include <fuse/renderer/atmosphere/sky_output.hpp>
#include <fuse/renderer/atmosphere/sky_scatter.hpp>
#include <fuse/renderer/atmosphere/sun_disk.hpp>
#include <fuse/renderer/atmosphere/transmittance_lut.hpp>
#include <fuse/renderer/volumetric/volumetric_fog.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::math::Vec3;
using fuse::renderer::AtmosphereParams;

constexpr f64 kPiD = 3.14159265358979323846;
constexpr f64 kDeg = kPiD / 180.0;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLe(f64 value, f64 bound, const char* message) {
    if (!(value <= bound)) {
        std::fprintf(stderr, "FAIL: %s (got %.6g, bound %.6g)\n", message, value, bound);
        ++g_failures;
    }
}

Vec3 dirFromElevationAzimuth(f64 elevation_rad, f64 azimuth_rad) {
    return Vec3{static_cast<f32>(std::cos(elevation_rad) * std::sin(azimuth_rad)),
                static_cast<f32>(std::sin(elevation_rad)),
                static_cast<f32>(std::cos(elevation_rad) * std::cos(azimuth_rad))};
}

// Composite Simpson in double.
template <typename F>
f64 simpson(F f, f64 a, f64 b, int intervals) {
    if (intervals % 2 != 0) {
        ++intervals;
    }
    const f64 h = (b - a) / intervals;
    f64 sum = f(a) + f(b);
    for (int i = 1; i < intervals; ++i) {
        sum += f(a + h * i) * ((i % 2 != 0) ? 4.0 : 2.0);
    }
    return sum * h / 3.0;
}

// ---------------------------------------------------------------------------------------------
// Independent double-precision spherical-shell atmosphere reference.
// ---------------------------------------------------------------------------------------------

struct RefAtmosphere {
    f64 R;
    f64 Rt;
    f64 betaR[3];
    f64 betaMsca;
    f64 betaMext;
    f64 HR;
    f64 HM;
    f64 g;

    explicit RefAtmosphere(const AtmosphereParams& p)
        : R(p.earth_radius), Rt(p.atmo_radius), betaR{p.rayleigh_coeff.x, p.rayleigh_coeff.y, p.rayleigh_coeff.z},
          betaMsca(p.mie_coeff), betaMext(static_cast<f64>(p.mie_coeff) / 0.9), HR(p.rayleigh_scale_h),
          HM(p.mie_scale_h), g(p.mie_scatter_dir) {}

    f64 height(f64 r0, f64 mu, f64 t) const { return std::sqrt(r0 * r0 + 2.0 * r0 * mu * t + t * t) - R; }

    f64 distTop(f64 r0, f64 mu) const {
        const f64 disc = r0 * r0 * (mu * mu - 1.0) + Rt * Rt;
        return -r0 * mu + std::sqrt(std::max(0.0, disc));
    }

    bool hitsGround(f64 r0, f64 mu) const { return mu < 0.0 && r0 * r0 * (mu * mu - 1.0) + R * R >= 0.0; }

    f64 distGround(f64 r0, f64 mu) const { return -r0 * mu - std::sqrt(std::max(0.0, r0 * r0 * (mu * mu - 1.0) + R * R)); }

    // Density-weighted path length (Rayleigh, Mie) to the top of the atmosphere, Simpson.
    void densityPath(f64 r0, f64 mu, f64 length, int intervals, f64& outR, f64& outM) const {
        outR = simpson([&](f64 t) { return std::exp(-height(r0, mu, t) / HR); }, 0.0, length, intervals);
        outM = simpson([&](f64 t) { return std::exp(-height(r0, mu, t) / HM); }, 0.0, length, intervals);
    }

    void opticalDepthToTop(f64 r0, f64 mu, f64 tau[3]) const {
        f64 pr = 0.0;
        f64 pm = 0.0;
        densityPath(r0, mu, distTop(r0, mu), 4000, pr, pm);
        for (int c = 0; c < 3; ++c) {
            tau[c] = betaR[c] * pr + betaMext * pm;
        }
    }

    // Brute-force single scattering: midpoint along the view ray, Simpson towards the sun.
    void inscatter(f64 altitude, const Vec3& view, const Vec3& sun, int viewSteps, int lightIntervals,
                   f64 out[3]) const {
        const f64 vx = view.x, vy = view.y, vz = view.z;
        const f64 vl = std::sqrt(vx * vx + vy * vy + vz * vz);
        const f64 sx = sun.x, sy = sun.y, sz = sun.z;
        const f64 sl = std::sqrt(sx * sx + sy * sy + sz * sz);
        const f64 mu = vy / vl;
        const f64 nu = (vx * sx + vy * sy + vz * sz) / (vl * sl);
        const f64 r0 = R + altitude;
        const f64 tEnd = hitsGround(r0, mu) ? distGround(r0, mu) : distTop(r0, mu);
        const f64 phaseR = 3.0 / (16.0 * kPiD) * (1.0 + nu * nu);
        const f64 g2 = g * g;
        const f64 phaseM = 3.0 / (8.0 * kPiD) * (1.0 - g2) * (1.0 + nu * nu) /
                           ((2.0 + g2) * std::pow(1.0 + g2 - 2.0 * g * nu, 1.5));
        out[0] = out[1] = out[2] = 0.0;
        const f64 dt = tEnd / viewSteps;
        f64 viewR = 0.0;
        f64 viewM = 0.0;
        f64 prevT = 0.0;
        for (int i = 0; i < viewSteps; ++i) {
            const f64 t = dt * (i + 0.5);
            // View optical depth to t by Simpson from the previous sample point.
            const f64 addR = simpson([&](f64 s) { return std::exp(-height(r0, mu, s) / HR); }, prevT, t, 8);
            const f64 addM = simpson([&](f64 s) { return std::exp(-height(r0, mu, s) / HM); }, prevT, t, 8);
            viewR += addR;
            viewM += addM;
            prevT = t;

            const f64 h = height(r0, mu, t);
            const f64 r = R + h;
            const f64 muS = (r0 * sy / sl + t * nu) / r;
            if (hitsGround(r, muS)) {
                continue;
            }
            f64 sunR = 0.0;
            f64 sunM = 0.0;
            densityPath(r, muS, distTop(r, muS), lightIntervals, sunR, sunM);
            const f64 rhoR = std::exp(-h / HR);
            const f64 rhoM = std::exp(-h / HM);
            for (int c = 0; c < 3; ++c) {
                const f64 tau = betaR[c] * (viewR + sunR) + betaMext * (viewM + sunM);
                out[c] += std::exp(-tau) * (betaR[c] * rhoR * phaseR + betaMsca * rhoM * phaseM) * dt;
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Row 1 — Rayleigh/Mie scattering, transmittance, sky colours.
// ---------------------------------------------------------------------------------------------

void testPhaseFunctionsIntegrateToOne() {
    const auto integrate = [](auto phase) {
        return 2.0 * kPiD * simpson([&](f64 mu) { return static_cast<f64>(phase(static_cast<f32>(mu))); },
                                    -1.0, 1.0, 200000);
    };
    const f64 rayleigh = integrate([](f32 mu) { return fuse::renderer::rayleigh_phase(mu); });
    std::printf("  rayleigh phase integral over sphere = %.7f\n", rayleigh);
    expectLe(std::fabs(rayleigh - 1.0), 1e-5, "rayleigh phase integrates to 1");

    for (const f32 g : {0.f, 0.3f, 0.758f, 0.9f}) {
        const f64 mie = integrate([g](f32 mu) { return fuse::renderer::mie_phase(mu, g); });
        std::printf("  mie phase (g=%.3f) integral over sphere = %.7f\n", g, mie);
        expectLe(std::fabs(mie - 1.0), 1e-4, "mie phase integrates to 1");
    }
    expectTrue(fuse::renderer::mie_phase(1.f, 0.758f) > 100.f * fuse::renderer::mie_phase(-1.f, 0.758f),
               "mie phase strongly forward for g=0.758");
}

void testTransmittanceMatchesAnalyticOpticalDepth() {
    const AtmosphereParams params{};
    const RefAtmosphere ref(params);
    const f64 R = params.earth_radius;
    const f64 top = params.atmo_radius - params.earth_radius;
    const f64 betaMext = fuse::renderer::atmosphere_mie_extinction(params);
    const f64 betaR[3] = {params.rayleigh_coeff.x, params.rayleigh_coeff.y, params.rayleigh_coeff.z};

    const auto tauFromT = [](const Vec3& t, int c) {
        const f32 v = (c == 0) ? t.x : (c == 1) ? t.y : t.z;
        return -std::log(static_cast<f64>(v));
    };

    // (a) Zenith from the ground: exact closed form for a radial ray, H (1 - exp(-top / H)).
    {
        const Vec3 t = fuse::renderer::compute_transmittance(params.earth_radius, 1.f, params);
        f64 worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            const f64 analytic = betaR[c] * params.rayleigh_scale_h * (1.0 - std::exp(-top / params.rayleigh_scale_h)) +
                                 betaMext * params.mie_scale_h * (1.0 - std::exp(-top / params.mie_scale_h));
            worst = std::max(worst, std::fabs(tauFromT(t, c) / analytic - 1.0));
        }
        std::printf("  zenith optical depth vs closed form: max rel err %.3e\n", worst);
        expectLe(worst, 1e-3, "zenith optical depth matches closed-form exponential integral");
    }

    // (b) Slant rays vs the flat-slab closed form tau = beta H / mu (curvature makes the sphere slightly
    //     thinner; the first-order correction is ~ (H / R) (1 - mu^2) / mu^2 * 1.5).
    for (const f64 zenithDeg : {30.0, 60.0, 75.0}) {
        const f64 mu = std::cos(zenithDeg * kDeg);
        const Vec3 t = fuse::renderer::compute_transmittance(params.earth_radius, static_cast<f32>(mu), params);
        f64 worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            const f64 flat = (betaR[c] * params.rayleigh_scale_h + betaMext * params.mie_scale_h) / mu;
            worst = std::max(worst, std::fabs(tauFromT(t, c) / flat - 1.0));
        }
        const f64 curvature = 1.5 * (params.rayleigh_scale_h / R) * (1.0 - mu * mu) / (mu * mu) + 2e-3;
        std::printf("  zenith %.0f deg optical depth vs flat slab: max rel err %.3e (curvature bound %.3e)\n",
                    zenithDeg, worst, curvature);
        expectLe(worst, curvature, "slant optical depth matches flat exponential slab within curvature term");
    }

    // (c) Horizontal ray from the ground: Chapman grazing limit tau = beta sqrt(pi R H / 2) (1 + H / (8 R)).
    {
        const Vec3 t = fuse::renderer::compute_transmittance(params.earth_radius, 0.f, params);
        f64 worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            const f64 chapR = std::sqrt(kPiD * R * params.rayleigh_scale_h / 2.0) * (1.0 + params.rayleigh_scale_h / (8.0 * R));
            const f64 chapM = std::sqrt(kPiD * R * params.mie_scale_h / 2.0) * (1.0 + params.mie_scale_h / (8.0 * R));
            const f64 analytic = betaR[c] * chapR + betaMext * chapM;
            worst = std::max(worst, std::fabs(tauFromT(t, c) / analytic - 1.0));
        }
        std::printf("  horizontal optical depth vs Chapman grazing limit: max rel err %.3e\n", worst);
        expectLe(worst, 5e-3, "horizontal optical depth matches spherical Chapman limit");
    }

    // (d) Arbitrary altitudes / angles vs double-precision spherical quadrature.
    f64 worst = 0.0;
    int occluded = 0;
    for (const f64 altitude : {0.0, 500.0, 2000.0, 10000.0, 40000.0}) {
        for (int i = 0; i <= 200; ++i) {
            const f64 mu = -0.3 + 1.3 * i / 200.0;
            const f64 r0 = R + altitude;
            const Vec3 t = fuse::renderer::compute_transmittance(static_cast<f32>(r0), static_cast<f32>(mu), params);
            if (ref.hitsGround(r0, mu)) {
                expectTrue(t.x == 0.f && t.y == 0.f && t.z == 0.f, "planet-occluded ray has zero transmittance");
                ++occluded;
                continue;
            }
            f64 tau[3];
            ref.opticalDepthToTop(r0, mu, tau);
            for (int c = 0; c < 3; ++c) {
                const f64 err = std::fabs(tauFromT(t, c) - tau[c]) / std::max(tau[c], 1e-3);
                worst = std::max(worst, err);
            }
        }
    }
    std::printf("  transmittance vs double spherical quadrature (%d occluded rays): max rel tau err %.3e\n",
                occluded, worst);
    expectLe(worst, 2e-3, "transmittance optical depth matches spherical reference");
    expectTrue(occluded > 0, "some below-horizon rays exercised");

    // (e) LUT entries are the reference transmittance at their bin centres.
    fuse::renderer::TransmittanceLutDesc desc{};
    desc.altitude_bins = 8;
    desc.cos_zenith_bins = 16;
    fuse::renderer::TransmittanceLut lut;
    expectTrue(lut.build(desc), "transmittance LUT builds");
    const f32 r = fuse::renderer::transmittance_lut_altitude_for_bin(3, desc);
    const f32 c = fuse::renderer::transmittance_lut_cos_zenith_for_bin(12, desc);
    const Vec3 expected = fuse::renderer::compute_transmittance(r, c, params);
    const Vec3 got = lut.entries()[fuse::renderer::transmittance_lut_flat_index(3, 12, 16)];
    expectTrue(got.x == expected.x && got.y == expected.y && got.z == expected.z, "LUT bin equals reference");
}

void testSkyColoursNoonAndSunset() {
    const AtmosphereParams params{};
    const RefAtmosphere ref(params);
    const Vec3 origin{0.f, 1.f, 0.f};

    // Midday (sun 65 deg up): zenith and the sky away from the sun are blue-dominant.
    // (With the sun exactly overhead the zenith *is* the Mie aureole around the sun and is near-white.)
    const Vec3 noonSun = dirFromElevationAzimuth(65.0 * kDeg, 0.0);
    const Vec3 zenithNoon = fuse::renderer::compute_sky_inscatter(origin, {0.f, 1.f, 0.f}, noonSun, params);
    const Vec3 awayNoon = fuse::renderer::compute_sky_inscatter(origin, dirFromElevationAzimuth(40.0 * kDeg, kPiD), noonSun, params);
    const Vec3 horizonNoon = fuse::renderer::compute_sky_inscatter(origin, dirFromElevationAzimuth(3.0 * kDeg, 0.5 * kPiD), noonSun, params);
    std::printf("  midday zenith rgb  = (%.4e, %.4e, %.4e)  b/r = %.2f\n", zenithNoon.x, zenithNoon.y, zenithNoon.z,
                zenithNoon.z / zenithNoon.x);
    std::printf("  midday away rgb    = (%.4e, %.4e, %.4e)  b/r = %.2f\n", awayNoon.x, awayNoon.y, awayNoon.z,
                awayNoon.z / awayNoon.x);
    std::printf("  midday horizon rgb = (%.4e, %.4e, %.4e)  b/r = %.2f\n", horizonNoon.x, horizonNoon.y, horizonNoon.z,
                horizonNoon.z / horizonNoon.x);
    expectTrue(zenithNoon.z > zenithNoon.y && zenithNoon.y > zenithNoon.x, "midday zenith b > g > r");
    expectTrue(zenithNoon.z > 1.5f * zenithNoon.x, "midday zenith blue-dominant (b > 1.5 r)");
    expectTrue(awayNoon.z > awayNoon.y && awayNoon.y > awayNoon.x, "midday sky away from sun b > g > r");
    expectTrue(awayNoon.z > 2.5f * awayNoon.x, "midday sky away from sun deep blue (b > 2.5 r)");
    expectTrue(horizonNoon.z > horizonNoon.x, "midday horizon still blue-leaning");
    expectTrue(horizonNoon.x / horizonNoon.z > zenithNoon.x / zenithNoon.z, "midday horizon whiter than zenith");

    // Low sun (2 deg elevation): transmitted sun and the sky around it red > green > blue.
    const f64 sunEl = 2.0 * kDeg;
    const Vec3 sunsetSun = dirFromElevationAzimuth(sunEl, 0.0);
    const Vec3 sunT = fuse::renderer::atmosphere_transmittance_to_top(params.earth_radius + 1.f, sunsetSun.y, 64u, params);
    std::printf("  sunset sun transmittance rgb = (%.4f, %.4f, %.4f)\n", sunT.x, sunT.y, sunT.z);
    expectTrue(sunT.x > sunT.y && sunT.y > sunT.z, "sunset sun disk colour r > g > b");
    expectTrue(sunT.x > 4.f * sunT.z, "sunset sun strongly reddened");

    const Vec3 nearSunDirs[] = {dirFromElevationAzimuth(sunEl + 3.0 * kDeg, 0.0),
                                dirFromElevationAzimuth(sunEl, 4.0 * kDeg),
                                dirFromElevationAzimuth(0.5 * kDeg, -8.0 * kDeg)};
    for (const Vec3& d : nearSunDirs) {
        const Vec3 c = fuse::renderer::compute_sky_colour(origin, d, sunsetSun, params);
        std::printf("  sunset near-sun sky rgb = (%.4e, %.4e, %.4e)\n", c.x, c.y, c.z);
        expectTrue(c.x > c.y && c.y > c.z, "sunset sky near the sun r > g > b");
    }

    // Whole-disk colour through compute_sky_colour (disk radiance integrated over its solid angle).
    const Vec3 sunCentre = fuse::renderer::compute_sky_colour(origin, sunsetSun, sunsetSun, params);
    expectTrue(sunCentre.x > sunCentre.y && sunCentre.y > sunCentre.z, "sunset sun disk pixel r > g > b");

    // Sunset zenith stays blue (physically correct: short path overhead).
    const Vec3 zenithSunset = fuse::renderer::compute_sky_inscatter(origin, {0.f, 1.f, 0.f}, sunsetSun, params);
    expectTrue(zenithSunset.z > zenithSunset.x, "sunset zenith remains blue");
    expectTrue(zenithSunset.z < 0.25f * zenithNoon.z, "sunset zenith much darker than midday zenith");
    const f32 ratioSunset = sunT.x / sunT.z;
    const Vec3 middayT = fuse::renderer::atmosphere_transmittance_to_top(params.earth_radius + 1.f, noonSun.y, 64u, params);
    std::printf("  sun r/b transmittance ratio: midday %.2f, sunset %.2f\n", middayT.x / middayT.z, ratioSunset);
    expectTrue(ratioSunset > 20.f * (middayT.x / middayT.z), "sun reddens strongly from midday to sunset");

    // Night: sun well below the horizon -> no single scattering at the zenith.
    const Vec3 night = fuse::renderer::compute_sky_inscatter(origin, {0.f, 1.f, 0.f}, dirFromElevationAzimuth(-20.0 * kDeg, 0.0), params);
    expectTrue(night.x == 0.f && night.y == 0.f && night.z == 0.f, "sun 20 deg below horizon: zenith single scatter is zero");

    // Accuracy vs brute-force double single-scattering reference (spec sample counts: 16 view, 8 light).
    struct Case { const char* name; f64 el; f64 az; Vec3 sun; };
    const Case cases[] = {
        {"overhead sun zenith", 90.0, 0.0, Vec3{0.f, 1.f, 0.f}},
        {"midday zenith", 90.0, 0.0, noonSun},
        {"midday el30 az90", 30.0, 90.0, noonSun},
        {"midday el5", 5.0, 0.0, noonSun},
        {"sun30 el45 az180", 45.0, 180.0, dirFromElevationAzimuth(30.0 * kDeg, 0.0)},
        {"sunset zenith", 90.0, 0.0, sunsetSun},
        {"sunset el10 toward", 10.0, 0.0, sunsetSun},
        {"sunset el10 away", 10.0, 180.0, sunsetSun},
        {"sunset el3 az90", 3.0, 90.0, sunsetSun},
    };
    f64 worstLum = 0.0;
    f64 worstChroma = 0.0;
    for (const Case& k : cases) {
        const Vec3 d = dirFromElevationAzimuth(k.el * kDeg, k.az * kDeg);
        const Vec3 got = fuse::renderer::compute_sky_inscatter(origin, d, k.sun, params);
        f64 expected[3];
        ref.inscatter(1.0, d, k.sun, 1024, 400, expected);
        const f64 gotSum = static_cast<f64>(got.x) + got.y + got.z;
        const f64 refSum = expected[0] + expected[1] + expected[2];
        const f64 lumErr = std::fabs(gotSum / refSum - 1.0);
        f64 chromaErr = 0.0;
        const f64 gotC[3] = {got.x / gotSum, got.y / gotSum, got.z / gotSum};
        for (int c = 0; c < 3; ++c) {
            chromaErr = std::max(chromaErr, std::fabs(gotC[c] - expected[c] / refSum));
        }
        std::printf("  %-20s ref rgb (%.4e %.4e %.4e) rel radiance err %.3f chroma err %.4f\n", k.name,
                    expected[0], expected[1], expected[2], lumErr, chromaErr);
        worstLum = std::max(worstLum, lumErr);
        worstChroma = std::max(worstChroma, chromaErr);
    }
    std::printf("  single scattering vs brute-force reference: max rel radiance err %.3f, max chroma err %.4f\n",
                worstLum, worstChroma);
    expectLe(worstLum, 0.08, "single-scattering radiance within 8% of brute-force reference");
    expectLe(worstChroma, 0.01, "single-scattering chromaticity within 0.01 of brute-force reference");
}

void testSkyLutMatchesDirectEvaluation() {
    // The LUT's sun-elevation axis must mean elevation (regression: the table sun used y = cos(el)).
    fuse::renderer::SkyLutDesc desc{};
    desc.sun_elevation_bins = 19;  // 10 deg bins
    desc.view_elevation_bins = 19;
    fuse::renderer::SkyLut lut;
    expectTrue(lut.build(desc, Vec3{0.f, 0.3f, 1.f}), "sky LUT builds");
    f64 worst = 0.0;
    for (const f64 sunEl : {0.0, 10.0, 60.0}) {
        for (const f64 viewEl : {10.0, 40.0, 90.0}) {
            const Vec3 direct = fuse::renderer::compute_sky_colour({}, dirFromElevationAzimuth(viewEl * kDeg, 0.0),
                                                                   dirFromElevationAzimuth(sunEl * kDeg, 0.0), desc.atmosphere);
            // Query slightly off-centre: nearest-bin lookup must still return this bin.
            const Vec3 sampled = lut.sample(static_cast<f32>((sunEl + 3.0) * kDeg), static_cast<f32>((viewEl - 3.0) * kDeg));
            worst = std::max(worst, static_cast<f64>(std::fabs(sampled.z - direct.z) / direct.z));
        }
    }
    std::printf("  sky LUT vs direct evaluation at bin centres: max rel err %.2e\n", worst);
    expectLe(worst, 1e-3, "sky LUT entries equal direct sky evaluation for the labelled sun/view elevation");
    const Vec3 sunsetDir = lut.sample(static_cast<f32>(0.0), static_cast<f32>(80.0 * kDeg));
    const Vec3 noonDir = lut.sample(static_cast<f32>(60.0 * kDeg), static_cast<f32>(80.0 * kDeg));
    expectTrue(noonDir.z > 3.f * sunsetDir.z, "LUT: high sun gives a brighter zenith than a horizon sun");
}

// ---------------------------------------------------------------------------------------------
// Row 2 — 10-bit gradient banding.
// ---------------------------------------------------------------------------------------------

// Test-side display transform (exposure, Reinhard per channel, sRGB OETF) — stands in for the
// post-process tonemap so the sky gradient is judged in display code values.
f64 displayEncode(f64 radiance, f64 exposure) {
    const f64 x = radiance * exposure;
    const f64 mapped = x / (1.0 + x);
    return mapped <= 0.0031308 ? 12.92 * mapped : 1.055 * std::pow(mapped, 1.0 / 2.4) - 0.055;
}

struct Sweep {
    const char* name;
    f64 sunElevationDeg;
    f64 viewAzimuthDeg;  // relative to the sun azimuth
};

constexpr u32 kRows = 1080u;
constexpr u32 kCols = 1920u;
constexpr f64 kVerticalFovDeg = 60.0;
constexpr f64 kPitchDeg = 30.0;

Vec3 rowDirection(u32 row, f64 azimuthRad) {
    const f64 ndcY = 1.0 - 2.0 * (row + 0.5) / kRows;
    const f64 elevation = kPitchDeg * kDeg + std::atan(ndcY * std::tan(0.5 * kVerticalFovDeg * kDeg));
    return dirFromElevationAzimuth(elevation, azimuthRad);
}

void testSkyGradientNoBanding10Bit() {
    const AtmosphereParams params{};
    const Vec3 origin{0.f, 1.f, 0.f};
    const Sweep sweeps[] = {
        {"noon", 90.0, 0.0},
        {"afternoon aside", 30.0, 90.0},
        {"afternoon away", 30.0, 180.0},
        {"sunset toward", 2.0, 0.0},
        {"sunset aside", 2.0, 90.0},
        {"sunset away", 2.0, 180.0},
    };
    const f64 exposureScales[] = {0.5, 1.0, 2.0};

    u32 worstSlowStep = 0;
    f64 worstCurvature = 0.0;
    f64 worstSteepElevation = 0.0;
    f64 worstFloatStep = 0.0;
    f64 worstDitherMean = 0.0;
    f64 worstPlainMean = 0.0;
    u32 slowRows = 0;
    u32 totalRows = 0;
    u32 monotonicViolations = 0;
    u32 aureoleRows = 0;
    u32 unexplainedSteep = 0;
    u32 steepPlateaus = 0;
    constexpr f64 kHorizonBandDeg = 10.0;
    constexpr f64 kAureoleDeg = 25.0;
    for (const Sweep& sweep : sweeps) {
        const Vec3 sun = dirFromElevationAzimuth(sweep.sunElevationDeg * kDeg, 0.0);
        std::vector<Vec3> column(kRows);
        std::vector<f64> elevation(kRows);
        std::vector<f64> sunSeparation(kRows);
        // The sun disk is a hard physical edge (row 3), not a gradient — exclude rows within 1 deg.
        std::vector<bool> skip(kRows, false);
        f64 skyPeak = 0.0;
        for (u32 row = 0; row < kRows; ++row) {
            const Vec3 dir = rowDirection(row, sweep.viewAzimuthDeg * kDeg);
            elevation[row] = std::asin(static_cast<f64>(dir.y)) / kDeg;
            column[row] = fuse::renderer::compute_sky_colour(origin, dir, sun, params);
            sunSeparation[row] = fuse::renderer::sun_disk_angular_separation_rad(dir, sun) / kDeg;
            skip[row] = sunSeparation[row] < 1.0;
            if (!skip[row]) {
                const Vec3& c = column[row];
                skyPeak = std::max(skyPeak, static_cast<f64>(std::max(c.x, std::max(c.y, c.z))));
            }
        }

        for (const f64 scale : exposureScales) {
            const f64 exposure = scale / skyPeak;
            std::vector<std::array<f64, 3>> v(kRows);
            std::vector<std::array<u32, 3>> codes(kRows);
            for (u32 row = 0; row < kRows; ++row) {
                const Vec3& c = column[row];
                const f64 rgb[3] = {c.x, c.y, c.z};
                for (int ch = 0; ch < 3; ++ch) {
                    const f64 display = displayEncode(rgb[ch], exposure);
                    v[row][ch] = display * 1023.0;
                    codes[row][ch] = fuse::renderer::sky_quantize_unorm(static_cast<f32>(display), 10u);
                }
            }

            u32 sweepSlowStep = 0;
            f64 sweepCurvature = 0.0;
            f64 sweepSteepElevation = 0.0;
            f64 sweepFloat = 0.0;
            for (u32 row = 0; row + 1 < kRows; ++row) {
                if (skip[row] || skip[row + 1]) {
                    continue;
                }
                ++totalRows;
                bool slow = true;
                for (int ch = 0; ch < 3; ++ch) {
                    const f64 dv = v[row + 1][ch] - v[row][ch];
                    sweepFloat = std::max(sweepFloat, std::fabs(dv));
                    slow = slow && std::fabs(dv) <= 1.0;
                    // Quantised output follows the continuous signal's direction (no reversals / glitches).
                    const long long dq = static_cast<long long>(codes[row + 1][ch]) - static_cast<long long>(codes[row][ch]);
                    if ((dv > 0.0 && dq < 0) || (dv < 0.0 && dq > 0)) {
                        ++monotonicViolations;
                    }
                    if (row > 0 && !skip[row - 1]) {
                        sweepCurvature = std::max(sweepCurvature, std::fabs(v[row + 1][ch] - 2.0 * v[row][ch] + v[row - 1][ch]));
                    }
                }
                if (slow) {
                    ++slowRows;
                    for (int ch = 0; ch < 3; ++ch) {
                        const u32 a = codes[row][ch];
                        const u32 b = codes[row + 1][ch];
                        sweepSlowStep = std::max(sweepSlowStep, a > b ? a - b : b - a);
                    }
                } else {
                    // A > 1 LSB/row gradient changes code on every row, so it cannot form a band; it must
                    // come from the horizon brightening or the Mie aureole around the sun.
                    const f64 el = std::min(elevation[row], elevation[row + 1]);
                    const f64 sep = std::min(sunSeparation[row], sunSeparation[row + 1]);
                    if (el < kHorizonBandDeg) {
                        sweepSteepElevation = std::max(sweepSteepElevation, el);
                    } else if (sep < kAureoleDeg) {
                        ++aureoleRows;
                    } else {
                        ++unexplainedSteep;
                    }
                    for (int ch = 0; ch < 3; ++ch) {
                        const u32 a = codes[row][ch];
                        const u32 b = codes[row + 1][ch];
                        const f64 dv = std::fabs(v[row + 1][ch] - v[row][ch]);
                        if (dv > 1.0 && a == b) {
                            ++steepPlateaus;
                        }
                    }
                }

                // Dithered row (1920 px of the same value): the row mean must reproduce the continuous
                // value; undithered output is off by up to 0.5 LSB (the contour).
                if (row % 8u == 0u) {
                    for (int ch = 0; ch < 3; ++ch) {
                        const f64 target = v[row][ch];
                        if (target < 1.0 || target > 1022.0) {
                            continue;  // clipped at the code range ends
                        }
                        // 1920 px x 4 frames (temporal accumulation as the display / TAA integrates it).
                        f64 sumDither = 0.0;
                        for (u32 frame = 0; frame < 4u; ++frame) {
                            for (u32 x = 0; x < kCols; ++x) {
                                sumDither += fuse::renderer::sky_quantize_unorm(
                                    static_cast<f32>(target / 1023.0), 10u,
                                    fuse::renderer::sky_dither_tpdf(x, row, frame * 3u + static_cast<u32>(ch)));
                            }
                        }
                        worstDitherMean = std::max(worstDitherMean, std::fabs(sumDither / (4.0 * kCols) - target));
                        worstPlainMean = std::max(worstPlainMean, std::fabs(static_cast<f64>(codes[row][ch]) - target));
                    }
                }
            }
            std::printf("  sweep %-16s exposure x%.1f: max |d2| %.3f LSB/row^2, slow-gradient max step %u code(s), "
                        "steepest %.2f LSB/row, horizon-band steep rows up to %.2f deg\n",
                        sweep.name, scale, sweepCurvature, sweepSlowStep, sweepFloat, sweepSteepElevation);
            worstSlowStep = std::max(worstSlowStep, sweepSlowStep);
            worstCurvature = std::max(worstCurvature, sweepCurvature);
            worstSteepElevation = std::max(worstSteepElevation, sweepSteepElevation);
            worstFloatStep = std::max(worstFloatStep, sweepFloat);
        }
    }
    std::printf("  10-bit 1080-row sweeps: %u/%u row pairs have a sub-LSB gradient; there the max step is %u code(s);"
                " max |d2| %.3f LSB; steeper pairs (max %.2f LSB/row): horizon band up to %.2f deg, %u in the sun"
                " aureole (< %.0f deg), %u unexplained, %u steep plateaus; %u quantiser direction violations\n",
                slowRows, totalRows, worstSlowStep, worstCurvature, worstFloatStep, worstSteepElevation, aureoleRows,
                kAureoleDeg, unexplainedSteep, steepPlateaus, monotonicViolations);
    std::printf("  dithered row-mean error %.3f LSB vs undithered %.3f LSB\n", worstDitherMean, worstPlainMean);
    expectLe(worstSlowStep, 1.0, "10-bit sky gradient: at most 1 code between adjacent rows wherever the gradient is sub-LSB");
    expectLe(worstCurvature, 0.5, "sky gradient smooth: no sampling kinks or jumps (|second difference| <= 0.5 LSB)");
    expectTrue(unexplainedSteep == 0u, "gradients steeper than 1 LSB/row only in the horizon band or sun aureole");
    expectTrue(steepPlateaus == 0u, "steep gradients never hold a code across a row (no plateau + jump)");
    expectTrue(monotonicViolations == 0u, "quantised gradient never reverses against the continuous gradient");
    expectLe(worstDitherMean, 0.04, "TPDF dither reproduces sub-LSB gradient on average");
    expectTrue(worstPlainMean > 0.4, "undithered reference shows the sub-LSB contour the dither removes");

    // Dither statistics: zero-mean, bounded, signal-independent error.
    f64 sum = 0.0;
    f64 sumSq = 0.0;
    f32 lo = 1.f;
    f32 hi = -1.f;
    const u32 n = 512u * 512u;
    for (u32 y = 0; y < 512u; ++y) {
        for (u32 x = 0; x < 512u; ++x) {
            const f32 d = fuse::renderer::sky_dither_tpdf(x, y, 7u);
            sum += d;
            sumSq += static_cast<f64>(d) * d;
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
    }
    const f64 mean = sum / n;
    const f64 variance = sumSq / n - mean * mean;
    std::printf("  TPDF dither: mean %.4f, variance %.4f (ideal 1/6 = %.4f), range [%.3f, %.3f]\n", mean,
                variance, 1.0 / 6.0, lo, hi);
    expectLe(std::fabs(mean), 0.005, "dither zero mean");
    expectLe(std::fabs(variance - 1.0 / 6.0), 0.005, "dither has triangular-PDF variance");
    expectTrue(lo > -1.f && hi < 1.f, "dither bounded to (-1, 1) LSB");
    // Temporal decorrelation: same pixel, consecutive frames.
    f64 corr = 0.0;
    for (u32 i = 0; i < 4096u; ++i) {
        corr += static_cast<f64>(fuse::renderer::sky_dither_tpdf(i, 3u, 0u)) * fuse::renderer::sky_dither_tpdf(i, 3u, 1u);
    }
    corr /= 4096.0 * (1.0 / 6.0);
    std::printf("  dither frame-to-frame correlation %.4f\n", corr);
    expectLe(std::fabs(corr), 0.05, "dither decorrelated across frames");
}

// ---------------------------------------------------------------------------------------------
// Row 3 — sun disk angular size.
// ---------------------------------------------------------------------------------------------

Vec3 rotateAwayFrom(const Vec3& sun, f64 angleRad) {
    // Rotate the sun direction by `angleRad` towards an orthogonal axis (double precision).
    const f64 sx = sun.x, sy = sun.y, sz = sun.z;
    const f64 l = std::sqrt(sx * sx + sy * sy + sz * sz);
    const f64 ux = sx / l, uy = sy / l, uz = sz / l;
    // Orthogonal axis: normalise(up x sun) or x-axis.
    f64 ox = -uz, oy = 0.0, oz = ux;
    f64 ol = std::sqrt(ox * ox + oz * oz);
    if (ol < 1e-6) {
        ox = 1.0; oy = 0.0; oz = 0.0; ol = 1.0;
    }
    ox /= ol; oy /= ol; oz /= ol;
    const f64 c = std::cos(angleRad), s = std::sin(angleRad);
    return Vec3{static_cast<f32>(ux * c + ox * s), static_cast<f32>(uy * c + oy * s), static_cast<f32>(uz * c + oz * s)};
}

void testSunDiskAngularSize() {
    const f64 radius = fuse::renderer::sun_angular_radius_rad();
    std::printf("  sun angular radius %.9f rad = %.6f deg (diameter %.6f deg)\n", radius, radius / kDeg, 2.0 * radius / kDeg);
    expectLe(std::fabs(2.0 * radius / kDeg - 0.5), 1e-6, "sun apparent diameter is 0.5 deg");

    // Edge located on real direction vectors at several sun elevations (f32 path, as the shader runs).
    f64 worstEdge = 0.0;
    for (const f64 el : {2.0, 30.0, 89.0}) {
        const Vec3 sun = dirFromElevationAzimuth(el * kDeg, 0.3);
        f64 lastInside = -1.0;
        f64 firstOutside = -1.0;
        for (int i = 0; i <= 2000; ++i) {
            const f64 theta = (0.24 + 0.02 * i / 2000.0) * kDeg;  // 0.00001 deg steps
            const Vec3 view = rotateAwayFrom(sun, theta);
            const f32 sep = fuse::renderer::sun_disk_angular_separation_rad(view, sun);
            const f32 factor = fuse::renderer::sun_disk_radiance_factor(sep);
            if (factor > 0.f) {
                lastInside = theta;
            } else if (firstOutside < 0.0) {
                firstOutside = theta;
            }
        }
        const f64 edge = 0.5 * (lastInside + firstOutside);
        worstEdge = std::max(worstEdge, std::fabs(edge - radius));
        std::printf("  sun el %.0f deg: measured disk edge %.6f deg (diameter %.6f deg)\n", el, edge / kDeg, 2.0 * edge / kDeg);
    }
    expectLe(worstEdge / kDeg, 2e-4, "measured disk diameter within 0.0004 deg of 0.5 deg");

    // The old acos(dot) separation in f32 is quantised near 0 — measure its error for comparison.
    {
        const Vec3 sun = dirFromElevationAzimuth(30.0 * kDeg, 0.3);
        f64 worstAcos = 0.0;
        f64 worstAtan = 0.0;
        for (int i = 1; i <= 400; ++i) {
            const f64 theta = 0.3 * kDeg * i / 400.0;
            const Vec3 view = rotateAwayFrom(sun, theta);
            const f32 acosSep = std::acos(std::min(1.f, view.normalized().dot(sun.normalized())));
            worstAcos = std::max(worstAcos, std::fabs(static_cast<f64>(acosSep) - theta));
            worstAtan = std::max(worstAtan, std::fabs(static_cast<f64>(fuse::renderer::sun_disk_angular_separation_rad(view, sun)) - theta));
        }
        std::printf("  separation error below 0.3 deg: atan2 %.2e deg vs f32 acos(dot) %.2e deg\n", worstAtan / kDeg, worstAcos / kDeg);
        expectLe(worstAtan / kDeg, 1e-4, "sun separation accurate at sub-degree angles");
    }

    // Limb darkening profile and half-maximum diameter.
    expectLe(std::fabs(fuse::renderer::sun_disk_radiance_factor(0.f) - 1.0), 1e-6, "disk centre relative radiance 1");
    const f64 limb = fuse::renderer::sun_disk_radiance_factor(static_cast<f32>(radius * 0.99999));
    expectLe(std::fabs(limb - (1.0 - fuse::renderer::kSunLimbDarkeningU)), 0.01, "limb radiance 1 - u");
    f64 halfMax = 0.0;
    f32 prev = 2.f;
    bool monotonic = true;
    for (int i = 0; i <= 100000; ++i) {
        const f64 theta = radius * i / 100000.0;
        const f32 f = fuse::renderer::sun_disk_radiance_factor(static_cast<f32>(theta));
        monotonic = monotonic && f <= prev;
        prev = f;
        if (f >= 0.5f) {
            halfMax = theta;
        }
    }
    std::printf("  limb-darkened FWHM diameter %.5f deg\n", 2.0 * halfMax / kDeg);
    expectTrue(monotonic, "limb darkening monotonic");
    expectTrue(2.0 * halfMax / kDeg > 0.49 && 2.0 * halfMax / kDeg <= 0.5, "half-maximum diameter within [0.49, 0.5] deg");

    // Energy: the normalised disk radiance integrates to 1 over its solid angle.
    const f64 energy = simpson(
        [&](f64 theta) {
            return 2.0 * kPiD * std::sin(theta) *
                   fuse::renderer::sun_disk_radiance_per_irradiance(static_cast<f32>(theta));
        },
        0.0, radius * (1.0 - 1e-9), 200000);
    std::printf("  disk radiance solid-angle integral %.6f\n", energy);
    expectLe(std::fabs(energy - 1.0), 2e-3, "sun disk delivers unit irradiance");

    // Rendered disk through compute_sky_colour at noon: disk-only radiance integrates to the zenith
    // transmittance (the sun irradiance that reaches the ground).
    const AtmosphereParams params{};
    const Vec3 sun{0.f, 1.f, 0.f};
    const Vec3 origin{0.f, 1.f, 0.f};
    const Vec3 transmittance = fuse::renderer::atmosphere_transmittance_to_top(params.earth_radius + 1.f, 1.f, 64u, params);
    const f64 diskIrradiance = simpson(
        [&](f64 theta) {
            const Vec3 view = rotateAwayFrom(sun, theta);
            const Vec3 all = fuse::renderer::compute_sky_colour(origin, view, sun, params);
            const Vec3 sky = fuse::renderer::compute_sky_inscatter(origin, view, sun, params);
            return 2.0 * kPiD * std::sin(theta) * static_cast<f64>(all.y - sky.y);
        },
        0.0, radius * (1.0 - 1e-6), 4000);
    std::printf("  rendered noon disk irradiance (green) %.5f vs transmittance %.5f\n", diskIrradiance, transmittance.y);
    expectLe(std::fabs(diskIrradiance / transmittance.y - 1.0), 5e-3, "rendered disk carries transmitted irradiance");
    const Vec3 outside = rotateAwayFrom(sun, radius * 1.001);
    const Vec3 a = fuse::renderer::compute_sky_colour(origin, outside, sun, params);
    const Vec3 b = fuse::renderer::compute_sky_inscatter(origin, outside, sun, params);
    expectTrue(a.x == b.x && a.y == b.y && a.z == b.z, "no disk radiance just outside 0.25 deg");
}

// ---------------------------------------------------------------------------------------------
// Row 4 — volumetric fog height falloff vs analytic exponential reference.
// ---------------------------------------------------------------------------------------------

struct Lcg {
    u32 state;
    f64 next() {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) * (1.0 / 16777216.0);
    }
};

// Independent double reference: Simpson over the clamped exponential, split at the base crossing.
f64 referenceFogOpticalDepth(const fuse::renderer::VolumetricFogParams& p, const Vec3& o, const Vec3& d, f64 length) {
    const f64 dl = std::sqrt(static_cast<f64>(d.x) * d.x + static_cast<f64>(d.y) * d.y + static_cast<f64>(d.z) * d.z);
    const f64 dy = d.y / dl;
    const auto density = [&](f64 t) {
        const f64 h = static_cast<f64>(o.y) + dy * t - p.base_height;
        return static_cast<f64>(p.density) * std::exp(-static_cast<f64>(p.height_falloff) * std::max(0.0, h));
    };
    f64 split = -1.0;
    if (dy != 0.0) {
        split = (p.base_height - static_cast<f64>(o.y)) / dy;
    }
    if (split > 0.0 && split < length) {
        return simpson(density, 0.0, split, 20000) + simpson(density, split, length, 20000);
    }
    return simpson(density, 0.0, length, 40000);
}

void testVolumetricFogFalloffMatchesAnalytic() {
    fuse::renderer::VolumetricFogParams params{};  // plan defaults: density 0.02, falloff 0.2, 32 steps

    // Density profile itself: rho(h) / rho(base) = exp(-k h), constant below base.
    for (const f32 h : {0.f, 0.5f, 5.f, 20.f, 50.f}) {
        const f64 ratio = fuse::renderer::sample_volumetric_fog_density(params, {3.f, h, -7.f}) / params.density;
        expectLe(std::fabs(ratio - std::exp(-0.2 * h)), 1e-6, "density ratio equals exp(-k h)");
    }
    expectLe(std::fabs(fuse::renderer::sample_volumetric_fog_density(params, {0.f, -10.f, 0.f}) - params.density), 1e-9,
             "density constant below base height");

    Lcg rng{12345u};
    f64 worstAnalytic = 0.0;
    f64 worstMarchRel = 0.0;
    f64 worstBoundRatio = 0.0;
    f64 worstHorizontal = 0.0;
    f64 worstTransmittance = 0.0;
    f64 worstDefaultMidpoint = 0.0;
    int rays = 0;
    for (const f32 falloff : {0.05f, 0.2f, 1.0f}) {
        for (const f32 base : {0.f, 12.f}) {
            params.height_falloff = falloff;
            params.base_height = base;
            for (int i = 0; i < 700; ++i) {
                Vec3 origin{static_cast<f32>(rng.next() * 200.0 - 100.0), static_cast<f32>(rng.next() * 80.0 - 20.0),
                            static_cast<f32>(rng.next() * 200.0 - 100.0)};
                const f64 az = rng.next() * 2.0 * kPiD;
                f64 dy;
                switch (i % 7) {
                    case 0: dy = 0.0; break;          // horizontal
                    case 1: dy = 1e-6; break;         // near-horizontal
                    case 2: dy = -1e-4; break;
                    case 3: dy = 1e-2 * (rng.next() - 0.5); break;
                    case 4: dy = (rng.next() < 0.5) ? 1.0 : -1.0; break;  // vertical
                    default: dy = rng.next() * 2.0 - 1.0; break;         // arbitrary
                }
                const f64 horiz = std::sqrt(std::max(0.0, 1.0 - dy * dy));
                const Vec3 dir{static_cast<f32>(horiz * std::cos(az)), static_cast<f32>(dy), static_cast<f32>(horiz * std::sin(az))};
                const f64 length = 1.0 + rng.next() * 300.0;
                const f32 distance = static_cast<f32>(length);

                const f64 reference = referenceFogOpticalDepth(params, origin, dir, distance);
                const f64 analytic = fuse::renderer::analytic_volumetric_fog_optical_depth(params, origin, dir, distance);
                const f64 marched = fuse::renderer::march_volumetric_fog_optical_depth(params, origin, dir, distance);
                worstAnalytic = std::max(worstAnalytic, std::fabs(analytic / reference - 1.0));

                // Midpoint-rule bound: per segment the exact/midpoint ratio for exp(-a t) is sinh(x)/x with
                // x = a dt / 2, so |rel err| <= 1 - x / sinh(x); the base-height kink adds rho k |dy| dt^2 / 8.
                const f64 dyn = static_cast<f64>(dir.y) / dir.length();
                const f64 dt = distance / static_cast<f64>(params.march_steps);
                const f64 x = 0.5 * falloff * std::fabs(dyn) * dt;
                const f64 relBound = (x < 1e-8) ? x * x / 6.0 : 1.0 - x / std::sinh(x);
                const f64 kink = params.density * falloff * std::fabs(dyn) * dt * dt / 8.0;
                const f64 bound = relBound * reference + kink + 2e-6 * reference + 1e-9;
                const f64 err = std::fabs(marched - reference);
                worstBoundRatio = std::max(worstBoundRatio, err / bound);
                worstMarchRel = std::max(worstMarchRel, err / std::max(reference, 1e-12));
                if (i % 7 == 0) {
                    worstHorizontal = std::max(worstHorizontal, err / reference);
                }

                const f64 tMarch = fuse::renderer::march_volumetric_fog_transmittance(params, origin, dir, distance);
                worstTransmittance = std::max(worstTransmittance, std::fabs(tMarch - std::exp(-reference)));
                if (falloff == 0.2f && length <= 100.0) {
                    worstDefaultMidpoint = std::max(worstDefaultMidpoint, err / std::max(reference, 1e-12));
                }
                ++rays;
            }
        }
    }
    std::printf("  fog: %d rays; analytic closed form vs double quadrature max rel err %.2e\n", rays, worstAnalytic);
    std::printf("  fog: 32-step march vs reference max rel err %.3e, max err/bound %.3f, horizontal rel err %.2e\n",
                worstMarchRel, worstBoundRatio, worstHorizontal);
    std::printf("  fog: segment-exact march transmittance vs exp(-reference) max abs err %.2e; "
                "midpoint density quadrature at plan defaults (k=0.2, rays <= 100 m) max rel err %.3e\n",
                worstTransmittance, worstDefaultMidpoint);
    expectLe(worstTransmittance, 2e-6, "fog transmittance along the march matches exp(-analytic) on every ray");
    expectLe(worstAnalytic, 5e-6, "fog closed form matches quadrature (incl. horizontal / near-horizontal)");
    expectLe(worstBoundRatio, 1.0, "fog ray march within midpoint error bound on every ray");
    expectLe(worstHorizontal, 2e-6, "horizontal fog rays exact");

    // Second-order convergence on a steep ray entirely above base height.
    params = fuse::renderer::VolumetricFogParams{};
    const Vec3 origin{0.f, 1.f, 0.f};
    const Vec3 dir = Vec3{0.3f, 0.8f, 0.1f}.normalized();
    const f64 reference = referenceFogOpticalDepth(params, origin, dir, 60.0);
    f64 prevErr = 0.0;
    for (const u32 steps : {8u, 16u, 32u, 64u}) {
        params.march_steps = steps;
        const f64 err = std::fabs(fuse::renderer::march_volumetric_fog_optical_depth(params, origin, dir, 60.f) - reference) / reference;
        if (prevErr > 0.0) {
            std::printf("  fog convergence %u steps: rel err %.3e (ratio %.2f)\n", steps, err, prevErr / err);
            expectTrue(prevErr / err > 3.5 && prevErr / err < 4.5, "fog march converges at second order");
        }
        prevErr = err;
    }
}

} // namespace

int main() {
    fuse::core::initialize();

    std::printf("[row] Sky renders correct Rayleigh scattering\n");
    testPhaseFunctionsIntegrateToOne();
    testTransmittanceMatchesAnalyticOpticalDepth();
    testSkyColoursNoonAndSunset();
    testSkyLutMatchesDirectEvaluation();
    std::printf("[row] No banding in sky gradient at 10-bit\n");
    testSkyGradientNoBanding10Bit();
    std::printf("[row] Sun disk 0.5 deg apparent diameter\n");
    testSunDiskAngularSize();
    std::printf("[row] Volumetric fog falloff vs analytic exponential\n");
    testVolumetricFogFalloffMatchesAnalytic();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b5_atmosphere_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b5_atmosphere_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
