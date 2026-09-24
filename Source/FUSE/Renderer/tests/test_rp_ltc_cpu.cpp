// WP-2.2 CPU gates (stub-safe): LTC area lights and sun disks against Monte Carlo references, and the
// "light.shade" reference kernel with the compensated BRDF and area lights. Lavapipe gate:
// test_rp_ltc.cpp (the same per-light code on the GPU, Slang and GLSL).
//
//   integral the disk (boundary integral above the horizon, clipped 32-gon across it) and the
//            clipped-polygon LTC form factors == a numerical integral of the LTC density (1%, tails
//            floored at 1% of the light's projected solid angle); lights on the mirror direction (the
//            highlight): LTC == Monte Carlo within 3%
//   encode   rectangle tangent octahedral snorm16x2 round trip; GpuLight rows of makeRectLight /
//            makeDiskLight / setSunAngularRadius
//   rect     lighting_gpu::light_contribution (LTC) vs a Monte Carlo integral of the same compensated BRDF
//            over the rectangle (multiple importance sampling: uniform area + VNDF with ray hits, f64
//            accumulation, 2 x 65,536 samples): diffuse (exact: clipped polygon of the clamped cosine)
//            and specular (the LTC fit) separately, lights above and straddling the horizon
//   disk     the same for disks / ellipses (the diffuse, above and across the horizon, is exact up to
//            the 64-point boundary integral / area-preserving 32-gon)
//   sun      directional lights with an angular radius (0.27 and 3 degrees) vs Monte Carlo over the cone;
//            continuity with the punctual light as the radius goes to 0
//   furnace  a rectangle filling the hemisphere with radiance 1 (the white furnace through the LTC path)
//   shade    the shade kernel with a LUT and area / sun lights: CpuReference == CpuParallel bit for bit;
//            cluster-list shading == all-lights shading (area lights outside their range contribute
//            exactly 0); without a LUT the WP-2.1 lobe is unchanged and area lights contribute nothing
//
// Tolerances (justified by the measured Monte Carlo noise and the accuracy of the method):
//   diffuse (the clamped cosine's polygon / disk integral, exact up to f32)  max relative error 1% above
//   the horizon (MC noise <= 0.3%; measured <= 7e-4 above and across it)
//   specular: the LTC is an approximation of the GGX lobe (Heitz et al. 2016): a clamped cosine cannot
//   follow GGX's long tails, its horizon is not the BRDF's, and at roughness 1 / grazing view the lobe is
//   flatter than any cosine. The error is judged against max(MC, light x lobe albedo) (a light in a
//   lobe's far tail is judged against the light, not against ~0): mean 5% / p90 15% above the horizon,
//   12.5% / 40% across it; lights inside the lobe (>= 25% of its peak response): mean 10% / p90 30%.
//   A refit with 1.5x the samples and 4x the Nelder-Mead iterations moved none of these (the error is
//   the representation's, not the fit's convergence); the finer checks are the integral suite's.

#include <fuse/core/init.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_reference.hpp>
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>

#include "test_rp_ltc_mc.hpp"

#include <fuse/renderer/lighting/clustered_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::lighting_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;
namespace kernel = fuse::kernel;
using namespace ltc_test;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

/// Error budget of one area-light type (see the file comment for the metrics).
struct AreaBudget {
    f64 diffuseMax = 0.01;          ///< diffuse, light above the horizon: max relative error
    f64 straddleDiffuseMean = 0.01; ///< diffuse, light across the horizon: mean relative error
    f64 specMean[2] = {};           ///< specular error / max(MC, light x lobe albedo): mean (above, across)
    f64 specP90[2] = {};            ///<   90th percentile (above, across)
    f64 lobeMean = 0.0;             ///< lights in the lobe (>= 25% of its peak response), above: mean
    f64 lobeP90 = 0.0;              ///<   90th percentile
};

void runArea(u32 type, const char* name, const AreaBudget& budget) {
    std::mt19937 rng(type == ltc::kLightRect ? 101u : 202u);
    constexpr u32 kConfigs = 72;
    Stats diff[2], specIrr[2], lobe[2], total[2];
    u32 nonzero = 0;
    for (u32 k = 0; k < kConfigs; ++k) {
        for (u32 st = 0; st < 2u; ++st) {
            const Config c = makeConfig(rng, k, type, st == 1u);
            const f64 scale = lightScale(c);
            const f64 espec = surface_terms(c.s, c.v, lut()).ms.specular.x;
            const f64 refD = mcArea(c, Part::Diffuse);
            const f64 refS = mcArea(c, Part::Specular);
            const f64 gotD = ltcArea(c, Part::Diffuse);
            const f64 gotS = ltcArea(c, Part::Specular);
            const f64 got = ltcArea(c, Part::Total);
            if (!(scale > 0.0)) {
                continue;
            }
            ++nonzero;
            if (refD > 1e-9 * scale) {
                diff[st].rel.push_back(std::fabs(gotD - refD) / refD);
            }
            specIrr[st].rel.push_back(std::fabs(gotS - refS) / std::max(refS, scale * espec));
            if (refS >= 0.25 * lobePeak(c)) {
                lobe[st].rel.push_back(std::fabs(gotS - refS) / refS);
                total[st].rel.push_back(std::fabs(got - (refD + refS)) / (refD + refS));
            }
        }
    }
    std::printf("%s: %u configurations; diffuse relative error | specular error / max(MC, light x lobe albedo) | lights in the lobe: "
                "specular and total relative error\n",
                name, nonzero);
    const char* rows[2] = {"above     ", "straddling"};
    for (u32 st = 0; st < 2u; ++st) {
        std::printf("  %s diffuse mean %.2e max %.2e | specular mean %.2e p90 %.2e max %.2e | in lobe (%zu) spec mean %.2e "
                    "p90 %.2e total mean %.2e\n",
                    rows[st], diff[st].mean(), diff[st].max(), specIrr[st].mean(), specIrr[st].percentile(0.9), specIrr[st].max(),
                    lobe[st].rel.size(), lobe[st].mean(), lobe[st].percentile(0.9), total[st].mean());
    }
    char label[200];
    std::snprintf(label, sizeof(label), "%s diffuse == MC (above: max %.1f%%, across the horizon: mean %.1f%%)", name,
                  budget.diffuseMax * 100.0, budget.straddleDiffuseMean * 100.0);
    expect(nonzero == 2u * kConfigs && diff[0].max() <= budget.diffuseMax && diff[1].mean() <= budget.straddleDiffuseMean, label);
    for (u32 st = 0; st < 2u; ++st) {
        std::snprintf(label, sizeof(label), "%s specular LTC vs MC (%s): error / max(MC, light x lobe albedo) mean <= %.1f%%, p90 <= %.1f%%",
                      name, st == 0u ? "above the horizon" : "across it", budget.specMean[st] * 100.0, budget.specP90[st] * 100.0);
        expect(specIrr[st].mean() <= budget.specMean[st] && specIrr[st].percentile(0.9) <= budget.specP90[st], label);
    }
    std::snprintf(label, sizeof(label), "%s lights in the lobe: specular relative error mean <= %.1f%%, p90 <= %.1f%%", name,
                  budget.lobeMean * 100.0, budget.lobeP90 * 100.0);
    expect(lobe[0].rel.size() >= 8u && lobe[0].mean() <= budget.lobeMean && lobe[0].percentile(0.9) <= budget.lobeP90, label);
}

// --- integral ----------------------------------------------------------------------------------------
/// The LTC integrals against a direct numerical integration of the LTC density (ltc_density) over the
/// light: the disk's boundary integral / clipped 32-gon and the clipped polygon are integrals of the same
/// distribution; and lights centred on the mirror direction (the highlight) against the Monte Carlo
/// reference of the BRDF.
void testIntegral() {
    std::mt19937 rng(5);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    f64 worstDisk = 0.0;
    f64 worstRect = 0.0;
    u32 disks = 0;
    u32 rects = 0;
    for (u32 k = 0; k < 64u; ++k) {
        SurfaceSample s{};
        s.normal = {0.f, 0.f, 1.f};
        // Roughness >= 0.1: the numerical integral must resolve the density's peak (the sharpest lobes
        // are covered by the highlight check below).
        s.roughness = 0.1f + 0.9f * u(rng);
        const f32 nov = 0.1f + 0.9f * u(rng);
        const Vec3 v{std::sqrt(1.f - nov * nov), 0.f, nov};
        const SurfaceTerms t = surface_terms(s, v, lut());
        const f32 radius = k % 3u == 0u ? 0.0047f : (k % 3u == 1u ? 0.052f : 0.5f);
        const f64 el = 0.2 + 1.3 * u(rng), ph = 6.283 * u(rng);
        const D3 l{std::cos(el) * std::cos(ph), std::cos(el) * std::sin(ph), std::sin(el)};
        const Vec3 lf = f3(l);
        const f32 tanR = std::tan(radius);
        const Vec3 axis = std::fabs(lf.x) < 0.5f ? Vec3{1.f, 0.f, 0.f} : Vec3{0.f, 1.f, 0.f};
        const Vec3 uu = ltc::safe_unit(axis - lf * lf.dot(axis), Vec3{1.f, 0.f, 0.f});
        const Vec3 ex = uu * tanR;
        const Vec3 ey = ltc::cross3(uu, lf) * tanR;
        const f32 ffDisk = ltc::disk_form_factor(t.frame, false, lf, ex, ey);
        const f32 ffRect = ltc::rect_form_factor(t.frame, false, lf, ex, ey);
        // Numerical integrals over the disk / square at distance 1 (area sampling, dω = dA cos / d^2).
        constexpr u32 kN = 384;
        f64 sumDisk = 0.0;
        f64 sumRect = 0.0;
        for (u32 j = 0; j < kN; ++j) {
            for (u32 i = 0; i < kN; ++i) {
                const f64 a = (i + 0.5) / kN, b = (j + 0.5) / kN;
                const f64 rr = std::sqrt(a), phi = 6.283185307179586 * b;
                const D3 pd = add(l, add(mul(d3(ex), rr * std::cos(phi)), mul(d3(ey), rr * std::sin(phi))));
                const D3 pr = add(l, add(mul(d3(ex), 2.0 * a - 1.0), mul(d3(ey), 2.0 * b - 1.0)));
                for (u32 q = 0; q < 2u; ++q) {
                    const D3 pnt = q == 0u ? pd : pr;
                    const f64 d = len(pnt);
                    const f64 w = ltc::ltc_density(t.frame, f3(mul(pnt, 1.0 / d))) * dot(mul(pnt, 1.0 / d), l) / (d * d);
                    (q == 0u ? sumDisk : sumRect) += w;
                }
            }
        }
        const f64 area = static_cast<f64>(tanR) * tanR;
        const f64 refDisk = sumDisk * ltc_test::kPi * area / (kN * kN);
        const f64 refRect = sumRect * 4.0 * area / (kN * kN);
        // Relative error, floored at 1% of the light's projected solid angle (area x cos / d^2): in a lobe's
        // far tail (FF ~1e-9..1e-7) the f32 edge / cone terms cancel to a few 1e-10 absolute and the
        // numerical reference itself is noisy; a tail is judged against the light's size.
        const f64 floor = 1e-2 * area * std::max(l.z, 0.05);
        const f64 ed = std::fabs(ffDisk - refDisk) / std::max(refDisk, floor);
        const f64 er = std::fabs(ffRect - refRect) / std::max(refRect, floor);
        if (std::getenv("LTC_DBG") != nullptr && (ed > 5e-3 || er > 5e-3)) {
            std::printf("  k %u r %.3f nov %.3f radius %.4f disk %.6g ref %.6g | rect %.6g ref %.6g\n", k, s.roughness, nov,
                        radius, ffDisk, refDisk, ffRect, refRect);
        }
        worstDisk = std::max(worstDisk, ed);
        worstRect = std::max(worstRect, er);
        disks += refDisk > floor ? 1u : 0u;
        rects += refRect > floor ? 1u : 0u;
    }
    std::printf("integral: disk_form_factor vs the numerical LTC integral over %u disks (0.27 / 3 / 29 degrees): max "
                "rel %.2e; rect_form_factor over %u squares: max rel %.2e\n",
                disks, worstDisk, rects, worstRect);
    expect(disks > 32u && worstDisk < 1e-2, "disk / ellipse LTC form factor == numerical integral (1%)");
    expect(rects > 32u && worstRect < 1e-2, "clipped polygon LTC form factor == numerical integral (1%)");

    // Lights centred on the mirror direction (the highlight), 3 sizes, 4 roughnesses: LTC vs MC.
    f64 worstLobe = 0.0;
    for (f32 rough : {0.045f, 0.1f, 0.25f, 0.5f}) {
        for (f32 rad : {0.05f, 0.5f, 1.5f}) {
            for (u32 type : {ltc::kLightDisk, ltc::kLightRect}) {
                Config c{};
                c.s.normal = {0.f, 0.f, 1.f};
                c.s.roughness = rough;
                c.s.metallic = 1.f;
                c.s.albedo = {1.f, 1.f, 1.f};
                c.v = {0.8f, 0.f, 0.6f};
                const Vec3 l{-0.8f, 0.f, 0.6f};
                ltc::AreaLightDesc d{};
                d.center = l * 3.f;
                d.normal = l * -1.f;
                d.tangent = {0.f, 1.f, 0.f};
                d.halfWidth = d.halfHeight = rad;
                d.range = 100.f;
                c.light = type == ltc::kLightDisk ? ltc::makeDiskLight(d) : ltc::makeRectLight(d);
                const f64 got = ltcArea(c, Part::Specular);
                const f64 ref = mcArea(c, Part::Specular);
                worstLobe = std::max(worstLobe, std::fabs(got - ref) / ref);
            }
        }
    }
    std::printf("integral: highlight (light on the mirror direction, roughness 0.045..0.5, 3 sizes) LTC vs MC: max rel %.2e\n",
                worstLobe);
    expect(worstLobe < 0.03, "LTC highlight == Monte Carlo within 3%");
}

// --- encode -------------------------------------------------------------------------------------------
void testEncode() {
    std::mt19937 rng(3);
    f64 worst = 0.0;
    for (u32 i = 0; i < 20000u; ++i) {
        const Vec3 t = randomUnit(rng);
        const Vec3 d = ltc::decodeTangent(ltc::encodeTangent(t));
        worst = std::max(worst, static_cast<f64>((d - t).length()));
    }
    for (const Vec3 t : {Vec3{1.f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}, Vec3{0.f, -1.f, 0.f}, Vec3{-1.f, 0.f, 0.f}}) {
        worst = std::max(worst, static_cast<f64>((ltc::decodeTangent(ltc::encodeTangent(t)) - t).length()));
    }
    std::printf("encode: tangent octahedral snorm16x2 round trip max error %.2e\n", worst);
    expect(worst < 1e-4, "tangent round trip < 1e-4");
    ltc::AreaLightDesc d{};
    d.center = {1.f, 2.f, 3.f};
    d.normal = {0.f, -2.f, 0.f};
    d.tangent = {1.f, 0.5f, 0.f}; // not orthogonal: the row stores the orthogonalised width axis
    d.halfWidth = 0.7f;
    d.halfHeight = 0.2f;
    d.range = 9.f;
    const gpu_scene::GpuLight r = ltc::makeRectLight(d);
    const Vec3 t = ltc::decodeTangent(r.flags);
    expect(r.type == ltc::kLightRect && r.direction[1] == -1.f && r.cosInner == 0.7f && r.cosOuter == 0.2f && r.range == 9.f &&
               std::fabs(t.x - 1.f) < 1e-4f && std::fabs(t.y) < 1e-4f,
           "makeRectLight row");
    const gpu_scene::GpuLight k = ltc::makeDiskLight(d);
    expect(k.type == ltc::kLightDisk && k.cosInner == 0.7f && k.cosOuter == 0.2f, "makeDiskLight row (ellipse radii)");
    gpu_scene::GpuLight sun{};
    ltc::setSunAngularRadius(sun, 0.01f);
    expect(std::fabs(sun.cosOuter - std::cos(0.01f)) < 1e-7f && sun.cosOuter < ltc::kMinSunCos, "sun radius");
    ltc::setSunAngularRadius(sun, 0.f);
    expect(sun.cosOuter == 1.f, "radius 0 = punctual");
}

// --- sun ------------------------------------------------------------------------------------------------
void testSun() {
    std::mt19937 rng(9);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    Stats above[2], grazing[2];
    const f32 radii[2] = {0.0047f, 0.052f};
    for (u32 k = 0; k < 96u; ++k) {
        const u32 ri = k % 2u;
        const bool graze = (k / 2u) % 4u == 3u;
        SurfaceSample s{};
        s.normal = randomUnit(rng);
        const f32 roughnesses[6] = {0.045f, 0.1f, 0.25f, 0.5f, 0.75f, 1.f};
        s.roughness = roughnesses[(k / 2u) % 6u];
        s.metallic = (k / 12u) % 2u == 0u ? 0.f : 1.f;
        s.albedo = {0.9f, 0.6f, 0.3f};
        const Frame fr = frameOf(d3(s.normal), d3(randomUnit(rng)));
        const f64 cv = 0.15 + 0.85 * u(rng);
        const f64 phv = 2.0 * ltc_test::kPi * u(rng);
        const Vec3 v = f3(fr.toWorld({std::sqrt(1 - cv * cv) * std::cos(phv), std::sqrt(1 - cv * cv) * std::sin(phv), cv}));
        // Sun direction: near the mirror direction half the time (the highlight), else random; grazing:
        // the disk crosses the horizon.
        D3 l{};
        const f64 elev = graze ? static_cast<f64>(radii[ri]) * (2.0 * u(rng) - 1.0) : 0.2 + 1.3 * u(rng);
        if (!graze && k % 3u == 0u) {
            const D3 vn = d3(v);
            const D3 n = d3(s.normal);
            l = sub(mul(n, 2.0 * dot(n, vn)), vn);
            if (l.z < 0.0 && dot(l, n) < 0.1) {
                l = n;
            }
            l = unit(add(l, mul(d3(randomUnit(rng)), 0.05 * u(rng))));
        } else {
            const f64 ph = 2.0 * ltc_test::kPi * u(rng);
            l = fr.toWorld({std::cos(elev) * std::cos(ph), std::cos(elev) * std::sin(ph), std::sin(elev)});
        }
        gpu_scene::GpuLight sun{};
        sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
        sun.direction[0] = static_cast<f32>(-l.x);
        sun.direction[1] = static_cast<f32>(-l.y);
        sun.direction[2] = static_cast<f32>(-l.z);
        sun.intensity = 1.f;
        ltc::setSunAngularRadius(sun, radii[ri]);
        const SurfaceTerms t = surface_terms(s, v, lut());
        const f64 got = light_contribution(sun, s, v, t, lut()).x;
        const f64 ref = mcSun(s, v, f3(l), radii[ri], t);
        if (ref <= 1e-6 && got <= 1e-6) {
            continue;
        }
        // Error relative to the reference, floored at a tenth of the light's response at normal
        // incidence on this surface ((diffuse + specular albedo) / pi per unit irradiance): a sun at the
        // terminator or in a lobe's far tail is judged against how bright the light is, not against ~0.
        const f64 floor = 0.1 * (t.ms.diffuse.x + t.ms.specular.x) / ltc_test::kPi;
        const f64 err = std::fabs(got - ref) / std::max(ref, floor);
        (graze ? grazing : above)[ri].rel.push_back(err);
    }
    for (u32 ri = 0; ri < 2u; ++ri) {
        std::printf("sun %.2f deg: above mean %.2e p90 %.2e max %.2e | grazing mean %.2e max %.2e\n",
                    static_cast<f64>(radii[ri]) * 180.0 / ltc_test::kPi, above[ri].mean(), above[ri].percentile(0.9), above[ri].max(),
                    grazing[ri].mean(), grazing[ri].max());
        // 3 degrees: the LTC integral where the lobe varies across the disk, exact elsewhere; 0.27 degrees
        // (the sun): the sharpest lobes (roughness 0.045, alpha 0.002 < the radius) take the LTC, whose
        // fit concentrates GGX's long tails into the peak (+10-20% on those highlights).
        expect(above[ri].mean() <= (ri == 0u ? 0.05 : 0.02) && above[ri].percentile(0.9) <= (ri == 0u ? 0.2 : 0.03),
               "sun disk vs MC above the horizon");
        expect(grazing[ri].mean() <= 0.05 && grazing[ri].max() <= 0.2, "sun disk vs MC across the horizon");
    }
    // Continuity: a tiny radius shades like the punctual light for rough surfaces.
    SurfaceSample s{};
    s.normal = {0.f, 0.f, 1.f};
    s.albedo = {0.8f, 0.8f, 0.8f};
    f64 worst = 0.0;
    for (f32 r : {0.3f, 0.6f, 1.f}) {
        s.roughness = r;
        const Vec3 v = Vec3{0.4f, 0.f, 1.f}.normalized();
        const SurfaceTerms t = surface_terms(s, v, lut());
        const D3 l = unit({-0.3, 0.2, 1.0});
        gpu_scene::GpuLight sun = directionalOf(l);
        const f64 point = light_contribution(sun, s, v, t, lut()).x;
        ltc::setSunAngularRadius(sun, 0.002f);
        const f64 disk = light_contribution(sun, s, v, t, lut()).x;
        worst = std::max(worst, std::fabs(disk - point) / point);
    }
    std::printf("sun: radius 0.002 rad vs punctual (roughness >= 0.3): max relative %.2e\n", worst);
    expect(worst < 0.02, "small sun disk == punctual within 2%");
}

// --- furnace -----------------------------------------------------------------------------------------
void testFurnace() {
    f64 worst = 0.0;
    std::printf("furnace (LTC): a 2e4 x 2e4 rectangle at height 1, radiance 1, albedo 1:\n");
    for (f32 metallic : {0.f, 1.f}) {
        for (f32 r : {0.045f, 0.25f, 0.5f, 1.f}) {
            std::printf("  m %.0f r %.3f:", metallic, r);
            for (f32 nov : {0.2f, 0.5f, 1.f}) {
                SurfaceSample s{};
                s.normal = {0.f, 0.f, 1.f};
                s.albedo = {1.f, 1.f, 1.f};
                s.roughness = r;
                s.metallic = metallic;
                const Vec3 v{std::sqrt(1.f - nov * nov), 0.f, nov};
                ltc::AreaLightDesc d{};
                d.center = {0.f, 0.f, 1.f};
                d.normal = {0.f, 0.f, -1.f};
                d.halfWidth = d.halfHeight = 1e4f;
                d.intensity = 1.f;
                d.range = 1e9f;
                const SurfaceTerms t = surface_terms(s, v, lut());
                const f64 e = light_contribution(ltc::makeRectLight(d), s, v, t, lut()).x;
                worst = std::max(worst, std::fabs(e - 1.0));
                std::printf(" %.4f", e);
            }
            std::printf("\n");
        }
    }
    std::printf("furnace (LTC): max |E - 1| = %.2e\n", worst);
    // The BRDF itself passes the furnace within 1% (fuse_rp_brdf_furnace); through the LTC path the
    // diffuse is exact and the specular loses the part of the fitted distribution that falls below
    // the horizon (rough lobes at grazing view: up to ~9%), an LTC property (Heitz et al. 2016).
    expect(worst <= 0.1, "LTC furnace: an area light filling the hemisphere returns 1 +- 10%");
}

// --- shade --------------------------------------------------------------------------------------------
ClusterCameraDesc makeCamera(Vec3 eye, Vec3 at, u32 w, u32 h) {
    ClusterCameraDesc c{};
    c.position = eye;
    c.forward = at - eye;
    c.up = {0.f, 1.f, 0.f};
    c.nearPlane = 0.3f;
    c.farPlane = 120.f;
    c.fovYRadians = 1.1f;
    c.screenWidth = w;
    c.screenHeight = h;
    c.reversedZ = false;
    return c;
}

struct SyntheticGBuffer {
    std::vector<f32> depth;
    std::vector<Vec4> rt0, rt1, rt2, rt5;
};

SyntheticGBuffer makeGBuffer(const ClusterCameraDesc& cam, u32 w, u32 h) {
    SyntheticGBuffer g{};
    g.depth.assign(static_cast<usize>(w) * h, 1.f);
    g.rt0.assign(g.depth.size(), Vec4{});
    g.rt1 = g.rt2 = g.rt5 = g.rt0;
    const clustered_kernel::CameraView view = clustered_kernel::make_camera(cam);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 sx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
            const f32 sy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
            const Vec3 dir = clustered_kernel::view_to_world(
                                 view, clustered_kernel::view_position_from_screen(sx, sy, 1.f, view.tan_x, view.tan_y)) -
                             view.position;
            const usize i = static_cast<usize>(y) * w + x;
            if (dir.y >= -1e-3f) {
                continue;
            }
            const f32 t = (-1.f - view.position.y) / dir.y;
            if (t > cam.farPlane * 0.9f) {
                continue;
            }
            g.depth[i] = cam.farPlane * (t - cam.nearPlane) / (t * (cam.farPlane - cam.nearPlane));
            const Vec3 nrm = Vec3{0.1f * std::sin(0.3f * static_cast<f32>(x)), 1.f, 0.1f * std::cos(0.2f * static_cast<f32>(y))}.normalized();
            const f32 l1 = std::fabs(nrm.x) + std::fabs(nrm.y) + std::fabs(nrm.z);
            const Vec3 q = nrm * (1.f / l1);
            g.rt0[i] = {q.x, q.y, 0.f, 1.f}; // q.z >= 0 (normal points up, towards +y... encode as-is)
            if (q.z < 0.f) {
                g.rt0[i].x = (1.f - std::fabs(q.y)) * (q.x >= 0.f ? 1.f : -1.f);
                g.rt0[i].y = (1.f - std::fabs(q.x)) * (q.y >= 0.f ? 1.f : -1.f);
            }
            g.rt1[i] = {0.2f + 0.6f * sx, 0.5f, 0.8f - 0.5f * sy, 1.f};
            g.rt2[i] = {0.05f + 0.9f * sy, (x / 16u) % 3u == 0u ? 1.f : 0.f, 0.f, 0.f};
        }
    }
    return g;
}

void testShade() {
    constexpr u32 kW = 96;
    constexpr u32 kH = 64;
    const ClusterCameraDesc cam = makeCamera({0.f, 1.f, 3.f}, {0.5f, -0.8f, -20.f}, kW, kH);
    const SyntheticGBuffer g = makeGBuffer(cam, kW, kH);
    ClusterDesc desc{};
    std::mt19937 rng(21);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    std::vector<gpu_scene::GpuLight> lights;
    // Points, then area lights (clustered like points), then spots: oracle order == slot order.
    for (u32 i = 0; i < 200u; ++i) {
        gpu_scene::GpuLight l{};
        l.type = static_cast<u32>(gpu_scene::GpuLightType::Point);
        l.position[0] = u(rng) * 10.f;
        l.position[1] = -0.5f + 0.8f * (u(rng) * 0.5f + 0.5f);
        l.position[2] = -2.f - 25.f * (u(rng) * 0.5f + 0.5f);
        l.range = 1.f + 2.f * (u(rng) * 0.5f + 0.5f);
        l.intensity = 3.f;
        lights.push_back(l);
    }
    for (u32 i = 0; i < 120u; ++i) {
        ltc::AreaLightDesc d{};
        d.center = {u(rng) * 10.f, -0.2f + 1.5f * (u(rng) * 0.5f + 0.5f), -2.f - 25.f * (u(rng) * 0.5f + 0.5f)};
        d.normal = Vec3{0.3f * u(rng), -1.f, 0.3f * u(rng)}.normalized();
        d.tangent = randomUnit(rng);
        d.halfWidth = 0.2f + 0.8f * (u(rng) * 0.5f + 0.5f);
        d.halfHeight = 0.2f + 0.8f * (u(rng) * 0.5f + 0.5f);
        d.color = {1.f, 0.8f, 0.6f};
        d.intensity = 4.f;
        d.range = 2.f + 3.f * (u(rng) * 0.5f + 0.5f);
        lights.push_back(i % 2u == 0u ? ltc::makeRectLight(d) : ltc::makeDiskLight(d));
    }
    for (u32 i = 0; i < 100u; ++i) {
        gpu_scene::GpuLight l = lights[i];
        l.type = static_cast<u32>(gpu_scene::GpuLightType::Spot);
        l.direction[0] = 0.f;
        l.direction[1] = -1.f;
        l.direction[2] = 0.f;
        l.cosInner = 0.95f;
        l.cosOuter = 0.8f;
        lights.push_back(l);
    }
    gpu_scene::GpuLight sun = directionalOf(unit({0.4, 1.0, 0.3}));
    sun.intensity = 0.7f;
    ltc::setSunAngularRadius(sun, 0.0047f);
    lights.push_back(sun);
    lights.push_back(directionalOf(unit({-0.2, 0.5, -0.9}))); // punctual

    OracleLights o{};
    makeOracleLights(lights.data(), static_cast<u32>(lights.size()), o);
    expect(o.monotone && o.points.size() == 320u && o.spots.size() == 100u, "area lights join the oracle's point set");
    ClusterGridSoA oracle{};
    oracleLightGrid(desc, cam, o, oracle);
    ClusterGridSoA grid{};
    translateToSlots(oracle, o, grid);

    ShadeReferenceDesc sd{};
    sd.width = kW;
    sd.height = kH;
    sd.desc = desc;
    sd.camera = cam;
    sd.ambient = {0.02f, 0.03f, 0.04f};
    sd.gbuffer = GBufferTexels{g.depth.data(), g.rt0.data(), g.rt1.data(), g.rt2.data(), g.rt5.data()};
    sd.lights = lights.data();
    sd.lightCount = static_cast<u32>(lights.size());
    sd.grid = &grid;
    sd.directional = &o.directional;
    sd.brdfLut = lut();
    std::vector<Vec4> ref, par, brute, legacy;
    const u32 shaded = shadeReferenceFrame(sd, ref, kernel::Backend::CpuReference);
    shadeReferenceFrame(sd, par, kernel::Backend::CpuParallel);
    expect(shaded > kW * kH / 3u, "ground shaded");
    expect(std::memcmp(ref.data(), par.data(), ref.size() * sizeof(Vec4)) == 0, "CpuReference == CpuParallel bit for bit (LUT)");

    ClusterGridSoA all{};
    all.grid.assign(desc.clusterCount(), ClusterGridEntry{});
    for (u32 slot = 0; slot < lights.size(); ++slot) {
        if (lights[slot].type != static_cast<u32>(gpu_scene::GpuLightType::Directional)) {
            all.lightList.push_back(slot);
        }
    }
    for (ClusterGridEntry& e : all.grid) {
        e = {0u, static_cast<u32>(all.lightList.size())};
    }
    sd.grid = &all;
    shadeReferenceFrame(sd, brute, kernel::Backend::CpuParallel);
    u32 bitEqual = 0;
    for (usize i = 0; i < ref.size(); ++i) {
        bitEqual += std::memcmp(&ref[i], &brute[i], sizeof(Vec4)) == 0 ? 1u : 0u;
    }
    std::printf("shade: %u px shaded; cluster lists vs all lights (with 120 area lights): %u / %u px bit-identical\n", shaded,
                bitEqual, kW * kH);
    expect(bitEqual == kW * kH, "cluster-list shading == all-lights shading bit for bit (area windows exactly 0 outside)");

    // Area lights change the image; without the LUT they contribute nothing (the WP-2.1 lobe).
    sd.grid = &grid;
    sd.brdfLut = nullptr;
    shadeReferenceFrame(sd, legacy, kernel::Backend::CpuParallel);
    std::vector<gpu_scene::GpuLight> noArea = lights;
    for (gpu_scene::GpuLight& l : noArea) {
        if (l.type == ltc::kLightRect || l.type == ltc::kLightDisk) {
            l.intensity = 0.f;
        }
    }
    std::vector<Vec4> legacyNoArea;
    sd.lights = noArea.data();
    shadeReferenceFrame(sd, legacyNoArea, kernel::Backend::CpuParallel);
    expect(std::memcmp(legacy.data(), legacyNoArea.data(), legacy.size() * sizeof(Vec4)) == 0,
           "without a LUT area lights contribute nothing");
    f64 areaEnergy = 0.0;
    sd.brdfLut = lut();
    std::vector<Vec4> withLutNoArea;
    shadeReferenceFrame(sd, withLutNoArea, kernel::Backend::CpuParallel);
    for (usize i = 0; i < ref.size(); ++i) {
        areaEnergy += (ref[i].x - withLutNoArea[i].x) + (ref[i].y - withLutNoArea[i].y) + (ref[i].z - withLutNoArea[i].z);
    }
    std::printf("shade: area lights add %.3g radiance summed over the frame\n", areaEnergy);
    expect(areaEnergy > 1.0, "area lights light the ground");
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "integral") {
        testIntegral();
    }
    if (all || suite == "encode") {
        testEncode();
    }
    if (all || suite == "rect") {
        runArea(ltc::kLightRect, "rect", AreaBudget{0.01, 0.01, {0.05, 0.125}, {0.15, 0.4}, 0.1, 0.3});
    }
    if (all || suite == "disk") {
        runArea(ltc::kLightDisk, "disk", AreaBudget{0.01, 0.05, {0.05, 0.125}, {0.15, 0.4}, 0.1, 0.3});
    }
    if (all || suite == "sun") {
        testSun();
    }
    if (all || suite == "furnace") {
        testFurnace();
    }
    if (all || suite == "shade") {
        testShade();
    }
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL %s: %d failure(s)\n", suite.c_str(), g_failures);
        return EXIT_FAILURE;
    }
    std::printf("PASS %s\n", suite.c_str());
    return EXIT_SUCCESS;
}
