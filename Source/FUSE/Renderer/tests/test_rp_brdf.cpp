// WP-2.2 CPU gates: BRDF look-up tables and multi-scatter energy compensation (stub-safe).
//
//   dfg        the "brdf_lut.dfg" / "brdf_lut.sphere" bakes: CpuReference == CpuParallel bit for bit at
//              0 / 2 / 4 workers; kernel stats; a GPU request without a device falls back to CpuParallel;
//              DFG texels == an independent f64 estimate (256 x 256 stratified VNDF samples, and a brute
//              hemisphere quadrature at rough texels; the closed form 1 - ln 2 at roughness 1, N.V = 1);
//              the LTC table words are the fitted file's
//   sphere     the horizon-clipped cap table == an independent brute-force sphere quadrature; the exact
//              limits (cap fully above: z, fully below: 0)
//   furnace    white furnace: albedo 1 at every roughness (0.045..1), N.V (0.05..1) and metallic (0, 0.5,
//              1) reflects 1 +- 1% (f64 multiple-importance quadrature of brdf::multi_scatter_cos with
//              the baked LUT); the single-scatter WP-2.1 lobe's loss is reported for comparison
//   reference  brdf::multi_scatter_cos == an independent f64 evaluation of the compensated BRDF (1e-5);
//              comp(1) == 1 / E_ss; Brdf::evaluateMultiScatter == multi_scatter_cos / N.L

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#include <fuse/renderer/material/brdf.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace fuse::renderer;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec3;
namespace kernel = fuse::kernel;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

constexpr f64 kPi = 3.141592653589793;

// --- f64 GGX (the shader's formulas, independent of the f32 code) ----------------------------------
struct D3 {
    f64 x = 0, y = 0, z = 0;
};
D3 add(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 scale(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 unit(D3 a) { return scale(a, 1.0 / std::sqrt(dot(a, a))); }

f64 alphaOf(f64 roughness) {
    const f64 r = std::clamp(roughness, 0.045, 1.0);
    return r * r;
}

f64 ggxD(f64 noh, f64 a2) {
    const f64 dd = (noh * a2 - noh) * noh + 1.0;
    return a2 / (kPi * dd * dd);
}

f64 visibility(f64 nov, f64 nol, f64 a2) {
    const f64 gv = nol * std::sqrt(nov * nov * (1.0 - a2) + a2);
    const f64 gl = nov * std::sqrt(nol * nol * (1.0 - a2) + a2);
    return 0.5 / std::max(gv + gl, 1e-5);
}

f64 g1(f64 nov, f64 a2) { return 2.0 * nov / (nov + std::sqrt(a2 + (1.0 - a2) * nov * nov)); }

/// VNDF sample (f64 twin of ltc::sample_vndf).
D3 vndf(D3 v, f64 alpha, f64 u1, f64 u2) {
    const D3 vh = unit({alpha * v.x, alpha * v.y, v.z});
    const f64 lensq = vh.x * vh.x + vh.y * vh.y;
    const D3 t1 = lensq > 0.0 ? scale(D3{-vh.y, vh.x, 0.0}, 1.0 / std::sqrt(lensq)) : D3{1.0, 0.0, 0.0};
    const D3 t2{vh.y * t1.z - vh.z * t1.y, vh.z * t1.x - vh.x * t1.z, vh.x * t1.y - vh.y * t1.x};
    const f64 r = std::sqrt(u1);
    const f64 phi = 2.0 * kPi * u2;
    const f64 p1 = r * std::cos(phi);
    f64 p2 = r * std::sin(phi);
    const f64 s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * std::sqrt(std::max(1.0 - p1 * p1, 0.0)) + s * p2;
    const D3 nh = add(add(scale(t1, p1), scale(t2, p2)), scale(vh, std::sqrt(std::max(1.0 - p1 * p1 - p2 * p2, 0.0))));
    return unit({alpha * nh.x, alpha * nh.y, std::max(nh.z, 0.0)});
}

/// Independent f64 (A, B) at (N.V, roughness): 256 x 256 stratified VNDF samples.
void referenceDfg(f64 nov, f64 roughness, f64& a, f64& b) {
    const f64 alpha = alphaOf(roughness);
    const f64 a2 = alpha * alpha;
    nov = std::max(nov, 1e-4);
    const D3 v{std::sqrt(std::max(1.0 - nov * nov, 0.0)), 0.0, nov};
    const f64 gv1 = g1(nov, a2);
    constexpr u32 kN = 256;
    a = b = 0.0;
    for (u32 j = 0; j < kN; ++j) {
        for (u32 i = 0; i < kN; ++i) {
            const D3 h = vndf(v, alpha, (i + 0.5) / kN, (j + 0.5) / kN);
            const f64 voh = dot(v, h);
            const D3 l = add(scale(h, 2.0 * voh), scale(v, -1.0));
            if (l.z <= 0.0 || voh <= 0.0) {
                continue;
            }
            const f64 w = 4.0 * nov * visibility(nov, l.z, a2) * l.z / gv1;
            const f64 fc = std::pow(std::clamp(1.0 - voh, 0.0, 1.0), 5.0);
            a += w * (1.0 - fc);
            b += w * fc;
        }
    }
    a /= kN * kN;
    b /= kN * kN;
}

/// Brute-force hemisphere quadrature of (A, B) (rough lobes only: no importance sampling at all).
void bruteDfg(f64 nov, f64 roughness, f64& a, f64& b) {
    const f64 a2 = alphaOf(roughness) * alphaOf(roughness);
    const D3 v{std::sqrt(1.0 - nov * nov), 0.0, nov};
    constexpr u32 kT = 1024;
    constexpr u32 kP = 512;
    a = b = 0.0;
    for (u32 i = 0; i < kT; ++i) {
        const f64 ct = (i + 0.5) / kT; // uniform in cos(theta)
        const f64 st = std::sqrt(1.0 - ct * ct);
        for (u32 j = 0; j < kP; ++j) {
            const f64 phi = 2.0 * kPi * (j + 0.5) / kP;
            const D3 l{st * std::cos(phi), st * std::sin(phi), ct};
            const D3 h = unit(add(v, l));
            const f64 f = ggxD(h.z, a2) * visibility(nov, ct, a2) * ct;
            const f64 fc = std::pow(std::clamp(1.0 - dot(v, h), 0.0, 1.0), 5.0);
            a += f * (1.0 - fc);
            b += f * fc;
        }
    }
    const f64 dw = (1.0 / kT) * (2.0 * kPi / kP);
    a *= dw;
    b *= dw;
}

// --- dfg ----------------------------------------------------------------------------------------------
void testDfg() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    ltc::BrdfLut reference;
    expect(ltc::bakeBrdfLut(reference, kernel::Backend::CpuReference) && reference.valid(), "CpuReference bake");
    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        ltc::BrdfLut parallel;
        expect(ltc::bakeBrdfLut(parallel, kernel::Backend::CpuParallel), "CpuParallel bake");
        const kernel::ParityReport r = kernel::compare_bitwise(std::span<const f32>(reference.data(), reference.size()),
                                                               std::span<const f32>(parallel.data(), parallel.size()));
        char label[96];
        std::snprintf(label, sizeof(label), "bake CpuReference == CpuParallel bit for bit (%u workers)", workers);
        expect(r.ok && r.compared == ltc::kLutWords, label);
    }
    // Stats: one launch per bake, one item per texel, 8 x 8 workgroups.
    kernel::reset_kernel_stats();
    ltc::BrdfLut again;
    ltc::bakeBrdfLut(again, kernel::Backend::CpuReference);
    kernel::KernelStats ds{};
    kernel::KernelStats ss{};
    expect(kernel::find_kernel_stats(ltc::kDfgBakeName, ds) && ds.launches == 1u && ds.items == 64u * 64u &&
               ds.workgroups == 64u && ds.last_backend == kernel::Backend::CpuReference,
           "brdf_lut.dfg stats (launches, items, workgroups, backend)");
    expect(kernel::find_kernel_stats(ltc::kSphereBakeName, ss) && ss.launches == 1u && ss.items == 64u * 64u,
           "brdf_lut.sphere stats");
    std::printf("dfg: bake on CpuReference: brdf_lut.dfg %.1f ms, brdf_lut.sphere %.1f ms\n", static_cast<f64>(ds.total_ns) * 1e-6,
                static_cast<f64>(ss.total_ns) * 1e-6);
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto}) {
            ltc::BrdfLut fallback;
            expect(ltc::bakeBrdfLut(fallback, gpu), "GPU-requested bake succeeds");
            const kernel::LaunchRecord last = kernel::last_launch();
            expect(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel && last.name != nullptr &&
                       std::string_view(last.name) == ltc::kSphereBakeName,
                   "GPU request without a device falls back to CpuParallel (recorded)");
            expect(std::memcmp(fallback.data(), reference.data(), reference.bytes()) == 0, "fallback bake == reference");
        }
    }
    scheduler.shutdown();

    // The LTC section holds the fitted table.
    expect(std::memcmp(reference.data() + ltc::kLtcOffset, ltc::ltcTable(), 64u * 64u * 4u * sizeof(f32)) == 0,
           "LTC section == ltc_lut_data.inc");
    bool ltcSane = true;
    for (u32 i = 0; i < 64u * 64u; ++i) {
        const f32* m = ltc::ltcTable() + i * 4u;
        // M^-1 of a transformed cosine: positive diagonal, non-singular.
        ltcSane = ltcSane && std::isfinite(m[0]) && std::isfinite(m[1]) && std::isfinite(m[2]) && std::isfinite(m[3]) &&
                  m[0] > 0.f && m[3] > 0.f && m[0] * m[3] - m[1] * m[2] > 0.f;
    }
    expect(ltcSane, "LTC table: finite, positive-determinant M^-1 on every texel");

    // DFG texels against the independent f64 estimates (texel centres: no interpolation).
    f64 worst = 0.0;
    f64 worstBrute = 0.0;
    u32 checked = 0;
    for (u32 j : {0u, 3u, 8u, 16u, 31u, 47u, 63u}) {
        for (u32 i : {0u, 5u, 20u, 40u, 55u, 62u, 63u}) {
            const f64 t = i / 63.0;
            const f64 nov = std::max(1.0 - t * t, 1e-4);
            const f64 rough = j / 63.0;
            f64 ra = 0.0;
            f64 rb = 0.0;
            referenceDfg(nov, rough, ra, rb);
            const f32* texel = reference.data() + ltc::kDfgOffset + (j * 64u + i) * 2u;
            worst = std::max({worst, std::fabs(texel[0] - ra), std::fabs(texel[1] - rb)});
            if (rough >= 0.5 && nov >= 0.2) {
                f64 ba = 0.0;
                f64 bb = 0.0;
                bruteDfg(nov, rough, ba, bb);
                worstBrute = std::max({worstBrute, std::fabs(texel[0] - ba), std::fabs(texel[1] - bb)});
            }
            ++checked;
        }
    }
    std::printf("dfg: %u texels vs f64 VNDF reference: max abs %.2e; rough texels vs brute hemisphere quadrature: "
                "max abs %.2e\n",
                checked, worst, worstBrute);
    expect(worst < 2e-3, "DFG texels == f64 VNDF estimate within 2e-3");
    expect(worstBrute < 3e-3, "DFG texels == brute-force hemisphere quadrature within 3e-3");
    // Sanity: E_ss = A + B in (0, 1], ~1 at low roughness and normal incidence, and decreasing with roughness.
    const f32* lut = reference.data();
    f32 a0 = 0.f, b0 = 0.f, a1 = 0.f, b1 = 0.f;
    ltc::sample_dfg(lut, 1.f, 0.045f, a0, b0);
    ltc::sample_dfg(lut, 1.f, 1.f, a1, b1);
    // Closed form at roughness 1 (alpha = 1: D = 1/pi, V = 0.5 / (N.L + N.V)) and N.V = 1:
    // E_ss = integral of mu / (1 + mu) over [0, 1] = 1 - ln 2.
    const f64 exact = 1.0 - std::log(2.0);
    std::printf("dfg: E_ss(N.V = 1) = %.4f at roughness 0.045, %.5f at roughness 1 (closed form 1 - ln 2 = %.5f)\n", a0 + b0,
                a1 + b1, exact);
    expect(a0 + b0 > 0.99f && a0 + b0 <= 1.001f, "E_ss ~ 1 for a smooth lobe at normal incidence");
    expect(std::fabs((a1 + b1) - exact) < 1e-3, "E_ss(N.V = 1, roughness 1) == 1 - ln 2");
}

// --- sphere ------------------------------------------------------------------------------------------
/// Brute force: (1/pi) integral over the upper hemisphere of cos inside the cap / ff.
f64 bruteSphere(f64 z, f64 ff) {
    const f64 sigma = std::asin(std::sqrt(ff));
    const f64 cosSigma = std::cos(sigma);
    const D3 axis{std::sqrt(std::max(1.0 - z * z, 0.0)), 0.0, z};
    constexpr u32 kT = 2048;
    constexpr u32 kP = 1024;
    f64 sum = 0.0;
    for (u32 i = 0; i < kT; ++i) {
        const f64 ct = (i + 0.5) / kT;
        const f64 st = std::sqrt(1.0 - ct * ct);
        for (u32 j = 0; j < kP; ++j) {
            const f64 phi = 2.0 * kPi * (j + 0.5) / kP;
            const D3 w{st * std::cos(phi), st * std::sin(phi), ct};
            if (dot(w, axis) >= cosSigma) {
                sum += ct;
            }
        }
    }
    return sum * (1.0 / kT) * (2.0 * kPi / kP) / kPi / ff;
}

void testSphere() {
    const ltc::BrdfLut& lut = ltc::sharedBrdfLut();
    expect(lut.valid(), "shared LUT");
    const f32* t = lut.data() + ltc::kSphereOffset;
    f64 worstLimit = 0.0;
    for (u32 j = 1; j < 64u; ++j) {
        const f64 ff = j / 63.0;
        const f64 sigma = std::asin(std::sqrt(ff));
        for (u32 i = 0; i < 64u; ++i) {
            const f64 z = -1.0 + 2.0 * i / 63.0;
            const f64 elevation = std::acos(z); // from the normal
            if (elevation + sigma <= kPi / 2.0 - 1e-6) {
                worstLimit = std::max(worstLimit, std::fabs(t[j * 64u + i] - z)); // fully above: FF = ff z
            } else if (elevation - sigma >= kPi / 2.0 + 1e-6) {
                worstLimit = std::max(worstLimit, std::fabs(static_cast<f64>(t[j * 64u + i]))); // below: 0
            }
        }
    }
    f64 worst = 0.0;
    u32 checked = 0;
    for (u32 j : {2u, 10u, 30u, 50u, 63u}) {
        for (u32 i : {16u, 28u, 31u, 32u, 36u, 45u}) {
            const f64 ref = bruteSphere(-1.0 + 2.0 * i / 63.0, j / 63.0);
            worst = std::max(worst, std::fabs(t[j * 64u + i] - ref));
            ++checked;
        }
    }
    std::printf("sphere: exact limits max abs %.2e; %u straddling texels vs brute-force quadrature max abs %.2e\n",
                worstLimit, checked, worst);
    expect(worstLimit < 1e-5, "sphere table == z above the horizon, 0 below");
    expect(worst < 2e-3, "sphere table == brute-force sphere quadrature within 2e-3");
}

// --- furnace -----------------------------------------------------------------------------------------
struct Energy {
    f64 compensated = 0.0;
    f64 single = 0.0; ///< WP-2.1 / B5.3 single-scatter lobe (Brdf::shade)
};

/// Directional albedo (irradiance-normalised reflected radiance under a uniform unit sky) by
/// multiple importance sampling: one VNDF and one cosine sample per stratum, balance heuristic.
Energy furnace(const f32* lut, f32 roughness, f32 nov, f32 metallic, f32 albedo) {
    const Vec3 n{0.f, 0.f, 1.f};
    const Vec3 v{std::sqrt(std::max(1.f - nov * nov, 0.f)), 0.f, nov};
    f32 a = 0.f;
    f32 b = 0.f;
    ltc::sample_dfg(lut, std::max(nov, brdf::kMinNoV), roughness, a, b);
    const brdf::MultiScatterTerms terms = brdf::multi_scatter_terms(Vec3{albedo, albedo, albedo}, metallic, a, b);
    const f64 alpha = alphaOf(roughness);
    const f64 a2 = alpha * alpha;
    const D3 vd{v.x, v.y, v.z};
    const f64 gv1 = g1(std::max<f64>(nov, 1e-4), a2);
    auto pdfVndf = [&](const D3& l) {
        const D3 h = unit(add(vd, l));
        return gv1 * ggxD(h.z, a2) / (4.0 * std::max<f64>(nov, 1e-4));
    };
    constexpr u32 kN = 256;
    Energy e{};
    for (u32 j = 0; j < kN; ++j) {
        for (u32 i = 0; i < kN; ++i) {
            const f64 u1 = (i + 0.5) / kN;
            const f64 u2 = (j + 0.5) / kN;
            D3 ls[2];
            const D3 h = vndf(vd, alpha, u1, u2);
            ls[0] = add(scale(h, 2.0 * dot(vd, h)), scale(vd, -1.0));
            const f64 r = std::sqrt(u1);
            ls[1] = {r * std::cos(2.0 * kPi * u2), r * std::sin(2.0 * kPi * u2), std::sqrt(std::max(1.0 - u1, 0.0))};
            for (const D3& l : ls) {
                if (!(l.z > 0.0)) {
                    continue;
                }
                const f64 pdf = pdfVndf(l) + l.z / kPi;
                const Vec3 lf{static_cast<f32>(l.x), static_cast<f32>(l.y), static_cast<f32>(l.z)};
                e.compensated += brdf::multi_scatter_cos(terms, Vec3{albedo, albedo, albedo}, roughness, n, v, lf).x / pdf;
                e.single += Brdf::shade(Vec3{albedo, albedo, albedo}, roughness, metallic, n, v, lf).x / pdf;
            }
        }
    }
    e.compensated /= kN * kN;
    e.single /= kN * kN;
    return e;
}

void testFurnace() {
    const f32* lut = ltc::sharedBrdfLut().data();
    f64 worst = 0.0;
    f64 minSingle = 10.0;
    f64 maxSingle = 0.0;
    u32 configs = 0;
    std::printf("furnace: albedo 1, E = reflected / incident (compensated | single-scatter WP-2.1 lobe)\n");
    for (f32 metallic : {1.f, 0.f, 0.5f}) {
        for (f32 roughness : {0.045f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.f}) {
            std::printf("  m %.1f r %.3f:", metallic, roughness);
            for (f32 nov : {0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.f}) {
                const Energy e = furnace(lut, roughness, nov, metallic, 1.f);
                worst = std::max(worst, std::fabs(e.compensated - 1.0));
                minSingle = std::min(minSingle, e.single);
                maxSingle = std::max(maxSingle, e.single);
                std::printf(" %.4f|%.3f", e.compensated, e.single);
                ++configs;
            }
            std::printf("\n");
        }
    }
    std::printf("furnace: %u configurations, compensated max |E - 1| = %.2e; single-scatter E in [%.3f, %.3f]\n", configs,
                worst, minSingle, maxSingle);
    expect(worst <= 0.01, "white furnace: albedo 1 returns 1 +- 1% at every roughness, N.V and metallic");
    expect(minSingle < 0.9, "the single-scatter lobe loses energy (what the compensation restores)");
}

// --- reference ---------------------------------------------------------------------------------------
void testReference() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    f64 worst = 0.0;
    f64 worstComp = 0.0;
    f64 worstWrapper = 0.0;
    for (u32 k = 0; k < 4000u; ++k) {
        const f32 a = 0.05f + 0.9f * u(rng);
        const f32 b = 0.2f * u(rng) * (1.f - a);
        worstComp = std::max(worstComp, std::fabs(static_cast<f64>(brdf::energy_compensation(1.f, a, b)) - 1.0 / (a + b)) *
                                            static_cast<f64>(a + b));
        const Vec3 albedo{u(rng), u(rng), u(rng)};
        const f32 metallic = k % 3u == 0u ? 0.f : (k % 3u == 1u ? 1.f : u(rng));
        const f32 roughness = 0.02f + u(rng);
        const f64 th = 1.5 * u(rng), ph = 6.28 * u(rng), tl = 1.5 * u(rng), pl = 6.28 * u(rng);
        const Vec3 n{0.f, 0.f, 1.f};
        const Vec3 v{static_cast<f32>(std::sin(th) * std::cos(ph)), static_cast<f32>(std::sin(th) * std::sin(ph)),
                     static_cast<f32>(std::cos(th))};
        const Vec3 l{static_cast<f32>(std::sin(tl) * std::cos(pl)), static_cast<f32>(std::sin(tl) * std::sin(pl)),
                     static_cast<f32>(std::cos(tl))};
        const brdf::MultiScatterTerms t = brdf::multi_scatter_terms(albedo, metallic, a, b);
        const Vec3 got = brdf::multi_scatter_cos(t, albedo, roughness, n, v, l);
        // Independent f64: linear blend of the dielectric and the metal, each (single scatter) x comp(F0),
        // + Lambert x (1 - E_spec(0.04)).
        const f64 a2 = alphaOf(roughness) * alphaOf(roughness);
        const D3 vd{v.x, v.y, v.z}, ld{l.x, l.y, l.z};
        const D3 h = unit(add(vd, ld));
        const f64 nov = std::max(vd.z, 1e-4), nol = ld.z;
        const f64 dv = ggxD(std::max(h.z, 0.0), a2) * visibility(nov, nol, a2);
        const f64 fc = std::pow(std::clamp(1.0 - std::max(dot(vd, h), 0.0), 0.0, 1.0), 5.0);
        auto comp = [&](f64 f0) {
            const f64 ems = 1.0 - (a + b);
            const f64 favg = f0 + (1.0 - f0) / 21.0;
            return 1.0 + favg * ems / (1.0 - favg * ems);
        };
        const f64 m = metallic;
        const f64 eD = (0.04 * a + b) * comp(0.04);
        const f32 al[3] = {albedo.x, albedo.y, albedo.z};
        const f32 gv[3] = {got.x, got.y, got.z};
        for (u32 c = 0; c < 3u; ++c) {
            const f64 fd = (0.04 + 0.96 * fc) * comp(0.04);
            const f64 fm = (al[c] + (1.0 - al[c]) * fc) * comp(al[c]);
            const f64 ref = (dv * ((1.0 - m) * fd + m * fm) + (1.0 - m) * (1.0 - eD) * al[c] / kPi) * nol;
            const f64 mag = std::max(std::fabs(ref), 1e-3);
            worst = std::max(worst, std::fabs(gv[c] - ref) / mag);
        }
        const Vec3 wrapped = Brdf::evaluateMultiScatter(albedo, roughness, metallic, n, v, l, a, b) * l.z;
        worstWrapper = std::max(worstWrapper, static_cast<f64>(std::fabs(wrapped.y - got.y)) / std::max(std::fabs(got.y), 1e-3f));
    }
    std::printf("reference: multi_scatter_cos vs f64 max rel %.2e; comp(1) vs 1/E_ss %.2e; Brdf wrapper %.2e\n", worst,
                worstComp, worstWrapper);
    // f32 vs f64: a few ulp per operation (D's denominator uses |N x H|^2, so no cancellation near the
    // peak); 1e-5 relative.
    expect(worst < 1e-5, "multi_scatter_cos == independent f64 evaluation (1e-5 relative)");
    expect(worstComp < 1e-5, "comp(F0 = 1) == 1 / E_ss");
    expect(worstWrapper < 1e-5, "Brdf::evaluateMultiScatter == multi_scatter_cos / N.L");
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "dfg") {
        testDfg();
    }
    if (all || suite == "sphere") {
        testSphere();
    }
    if (all || suite == "furnace") {
        testFurnace();
    }
    if (all || suite == "reference") {
        testReference();
    }
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL %s: %d failure(s)\n", suite.c_str(), g_failures);
        return EXIT_FAILURE;
    }
    std::printf("PASS %s\n", suite.c_str());
    return EXIT_SUCCESS;
}
