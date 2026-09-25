// B5 gate proofs — G-buffer encoding and PBR materials (B5.2 / B5.3, master plan §B5.12 checklist).
//
// CI has no GPU, so every row is proven on the CPU reference of the shader math
// (shaders/common/gbuffer.glsl, brdf.glsl, material.glsl) against independent references:
// analytic formulas, closed-form integrals, brute-force searches and Monte Carlo estimates.

#include <fuse/core/init.hpp>
#include <fuse/math/sdf.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/material/brdf.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/material/procedural_materials.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

namespace {

using fuse::f32;
using fuse::u16;
using fuse::u32;
using fuse::usize;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLess(double value, double bound, const char* message) {
    if (!(value < bound)) {
        std::fprintf(stderr, "FAIL: %s (got %.6g, bound %.6g)\n", message, value, bound);
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;

Vec3 mul(const Vec3& a, const Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

f32 maxAbs(const Vec3& v) {
    return std::max({std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)});
}

std::vector<Vec3> fibonacciSphere(u32 count) {
    std::vector<Vec3> dirs;
    dirs.reserve(count);
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (u32 i = 0; i < count; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / count;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = golden * i;
        dirs.push_back({static_cast<f32>(r * std::cos(phi)), static_cast<f32>(r * std::sin(phi)),
                        static_cast<f32>(z)});
    }
    return dirs;
}

// Directions that stress the octahedral map: axes/poles, the equator (fold line), the lower-hemisphere
// octant seams (x = 0 / y = 0 with z < 0), the diagonals and tiny offsets around each of them.
std::vector<Vec3> octahedralStressDirections() {
    std::vector<Vec3> dirs;
    const f32 axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& a : axes) {
        dirs.push_back({a[0], a[1], a[2]});
    }
    const f32 eps[] = {0.f, 1e-7f, -1e-7f, 1e-5f, -1e-5f, 1e-3f, -1e-3f, 0.01f, -0.01f};
    for (int i = 0; i < 720; ++i) {
        const f32 t = static_cast<f32>(i) * static_cast<f32>(kPi) / 360.f;
        for (const f32 e : eps) {
            dirs.push_back(Vec3{std::cos(t), std::sin(t), e}.normalized());          // equator
            dirs.push_back(Vec3{e, std::cos(t), std::sin(t)}.normalized());          // x = 0 seam
            dirs.push_back(Vec3{std::cos(t), e, std::sin(t)}.normalized());          // y = 0 seam
            dirs.push_back(Vec3{std::cos(t) + e, std::cos(t), std::sin(t)}.normalized()); // |x| = |y|
            dirs.push_back(Vec3{e, e * 0.5f, t > static_cast<f32>(kPi) ? -1.f : 1.f}.normalized()); // poles
        }
    }
    return dirs;
}

// ---------------------------------------------------------------------------------------------
// Row 1: G-buffer octahedral normal encoding round-trips with < 0.001 angular error.
// ---------------------------------------------------------------------------------------------

void testHalfConversionAgainstReference() {
    // Exhaustive: every finite half survives half -> float -> half unchanged.
    u32 roundTripErrors = 0;
    for (u32 bits = 0; bits < 0x10000u; ++bits) {
        const u16 h = static_cast<u16>(bits);
        if ((h & 0x7C00u) == 0x7C00u && (h & 0x3FFu) != 0u) {
            continue; // NaN payloads
        }
        if (GBufferQuantize::floatToHalf(GBufferQuantize::halfToFloat(h)) != h) {
            ++roundTripErrors;
        }
    }
    expectTrue(roundTripErrors == 0u, "every finite half round-trips through float");

    // Round-to-nearest-even at exact midpoints between adjacent halves (1 + 2^-11 lies between 1 and
    // 1 + 2^-10 -> even mantissa 1.0; 1 + 3*2^-11 -> 1 + 2^-9).
    expectTrue(GBufferQuantize::toHalf(1.f + std::ldexp(1.f, -11)) == 1.f, "half RNE ties to even (down)");
    expectTrue(GBufferQuantize::toHalf(1.f + 3.f * std::ldexp(1.f, -11)) == 1.f + std::ldexp(1.f, -9),
               "half RNE ties to even (up)");
    expectTrue(GBufferQuantize::toHalf(65504.f) == 65504.f, "largest finite half");
    expectTrue(std::isinf(GBufferQuantize::toHalf(65520.f)), "65520 rounds to inf");
    expectTrue(GBufferQuantize::toHalf(std::ldexp(1.f, -24)) == std::ldexp(1.f, -24), "smallest subnormal");

// clang-cl defines __FLT16_MAX__ but links no compiler-rt (__truncsfhf2); cl.exe has no _Float16.
#if defined(__FLT16_MAX__) && !defined(_MSC_VER)
    // Independent reference: the compiler's own IEEE binary16 conversion.
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> expo(-30.f, 17.f);
    std::uniform_real_distribution<float> unit(-1.f, 1.f);
    u32 mismatches = 0;
    for (int i = 0; i < 200000; ++i) {
        const float v = unit(rng) * std::exp2(expo(rng));
        const _Float16 ref = static_cast<_Float16>(v);
        u16 refBits = 0;
        std::memcpy(&refBits, &ref, sizeof(refBits));
        if (GBufferQuantize::floatToHalf(v) != refBits) {
            ++mismatches;
        }
    }
    expectTrue(mismatches == 0u, "floatToHalf matches compiler _Float16 conversion on 200k values");
    std::printf("  half conversion: exhaustive round trip ok, %u/200000 mismatches vs _Float16\n", mismatches);
#else
    std::printf("  half conversion: exhaustive round trip ok (_Float16 reference unavailable)\n");
#endif
}

void testOctahedralRoundTrip() {
    std::vector<Vec3> dirs = fibonacciSphere(1000000u);
    const std::vector<Vec3> stress = octahedralStressDirections();
    dirs.insert(dirs.end(), stress.begin(), stress.end());

    double maxF32Signed = 0.0;
    double maxF32Unsigned = 0.0;
    double maxRgba16f = 0.0;
    double maxNaiveUnsignedHalf = 0.0;
    double maxLowerHemisphere = 0.0;
    for (const Vec3& n : dirs) {
        maxF32Signed = std::max<double>(
            maxF32Signed,
            GBufferEncoding::angularErrorRadians(n, GBufferEncoding::decodeOctSigned(GBufferEncoding::encodeOctSigned(n))));
        const f32 e = GBufferEncoding::angularErrorRadians(
            n, GBufferEncoding::decodeNormal(GBufferEncoding::encodeNormal(n)));
        maxF32Unsigned = std::max<double>(maxF32Unsigned, e);
        if (n.z < 0.f) {
            maxLowerHemisphere = std::max<double>(maxLowerHemisphere, e);
        }

        // Full G-buffer path: write_gbuffer -> RGBA16F storage -> read.
        GBufferPackedData data{};
        data.normal = n;
        const GBufferMrt stored = GBufferPacking::quantizeToStorage(GBufferPacking::pack(data));
        const GBufferPackedData read = GBufferPacking::unpack(stored);
        maxRgba16f = std::max<double>(maxRgba16f, GBufferEncoding::angularErrorRadians(n, read.normal));

        // The unsigned [0,1] encoding stored as plain halves (what a naive RT0 write would keep).
        const Vec2 u = GBufferEncoding::encodeNormal(n);
        const Vec3 nu = GBufferEncoding::decodeNormal({GBufferQuantize::toHalf(u.x), GBufferQuantize::toHalf(u.y)});
        maxNaiveUnsignedHalf = std::max<double>(maxNaiveUnsignedHalf, GBufferEncoding::angularErrorRadians(n, nu));
    }

    std::printf("  oct normal: %zu directions; max error f32 signed %.3g rad, f32 unsigned %.3g rad, "
                "lower hemisphere %.3g rad\n",
                dirs.size(), maxF32Signed, maxF32Unsigned, maxLowerHemisphere);
    std::printf("  oct normal: RGBA16F G-buffer path max %.6f rad (gate < 0.001); naive unsigned-half "
                "storage would be %.6f rad\n",
                maxRgba16f, maxNaiveUnsignedHalf);
    expectLess(maxF32Signed, 1e-5, "f32 signed octahedral round trip");
    expectLess(maxF32Unsigned, 1e-5, "f32 unsigned octahedral round trip (incl. lower hemisphere)");
    expectLess(maxRgba16f, 0.001, "GATE: RGBA16F G-buffer normal round trip < 0.001 rad");

    // Diagnostic: how much a wider (5x5 ulp) brute-force snap would still gain over the 3x3 search the
    // shader uses. The gate above already bounds the error that is actually stored.
    double maxMissedGain = 0.0;
    for (usize i = 0; i < dirs.size(); i += 97) {
        const Vec3 n = dirs[i];
        const Vec2 pick = GBufferEncoding::encodeNormalRgba16f(n);
        const f32 pickErr = GBufferEncoding::angularErrorRadians(n, GBufferEncoding::decodeOctSigned(pick));
        u16 bx = GBufferQuantize::floatToHalf(pick.x);
        u16 by = GBufferQuantize::floatToHalf(pick.y);
        bx = GBufferQuantize::halfNextDown(GBufferQuantize::halfNextDown(bx));
        by = GBufferQuantize::halfNextDown(GBufferQuantize::halfNextDown(by));
        f32 bestErr = pickErr;
        u16 cx = bx;
        for (int a = 0; a < 5; ++a, cx = GBufferQuantize::halfNextUp(cx)) {
            u16 cy = by;
            for (int b = 0; b < 5; ++b, cy = GBufferQuantize::halfNextUp(cy)) {
                const Vec2 c{GBufferQuantize::halfToFloat(cx), GBufferQuantize::halfToFloat(cy)};
                if (std::fabs(c.x) > 1.f || std::fabs(c.y) > 1.f) {
                    continue;
                }
                bestErr = std::min(bestErr, GBufferEncoding::angularErrorRadians(n, GBufferEncoding::decodeOctSigned(c)));
            }
        }
        maxMissedGain = std::max<double>(maxMissedGain, pickErr - bestErr);
    }
    std::printf("  oct normal: a 5x5 brute-force snap would gain at most %.3g rad over the 3x3 search\n", maxMissedGain);

    // Encoded values are exactly representable in the RGBA16F attachment.
    const Vec2 enc = GBufferEncoding::encodeNormalRgba16f(Vec3{0.3f, -0.8f, -0.52f});
    expectTrue(GBufferQuantize::toHalf(enc.x) == enc.x && GBufferQuantize::toHalf(enc.y) == enc.y,
               "RT0 normal channels are half-exact");
    // Poles / axes decode exactly to themselves (sign(0) fold regression).
    const Vec3 down = GBufferEncoding::decodeOctSigned(GBufferEncoding::encodeOctSigned({0.f, 0.f, -1.f}));
    expectTrue(down.z < -0.999999f, "-Z pole does not collapse onto +Z");
}

// ---------------------------------------------------------------------------------------------
// Row 2: PBR metallic sweep — dielectric to metal with no colour discontinuity.
// ---------------------------------------------------------------------------------------------

// Independent Cook-Torrance reference written in angle form (Walter et al. 2007, Heitz 2014):
//   D  = 1 / (pi a^2 cos^4(th) (1 + tan^2(th)/a^2)^2),  Lambda = (-1 + sqrt(1 + a^2 tan^2)) / 2,
//   G2 = 1 / (1 + Lambda_v + Lambda_l),                f = F D G2 / (4 NoL NoV) + (1-F)(1-m) albedo / pi.
Vec3 referenceBrdf(const Vec3& albedo, double roughness, double metallic, const Vec3& N, const Vec3& V,
                   const Vec3& L) {
    const double NoL = N.dot(L);
    const double NoV = N.dot(V);
    if (NoL <= 0.0 || NoV <= 0.0) {
        return {};
    }
    const Vec3 H = (V + L).normalized();
    const double cosH = N.dot(H);
    const double a = roughness * roughness;
    const double tan2H = (1.0 - cosH * cosH) / (cosH * cosH);
    const double denom = 1.0 + tan2H / (a * a);
    const double D = 1.0 / (kPi * a * a * cosH * cosH * cosH * cosH * denom * denom);
    const auto lambda = [a](double c) {
        const double t2 = (1.0 - c * c) / (c * c);
        return (-1.0 + std::sqrt(1.0 + a * a * t2)) * 0.5;
    };
    const double G2 = 1.0 / (1.0 + lambda(NoV) + lambda(NoL));
    const double VoH = V.dot(H);
    const double w = std::pow(1.0 - VoH, 5.0);
    const double spec = D * G2 / (4.0 * NoL * NoV);
    const double c[3] = {albedo.x, albedo.y, albedo.z};
    double out[3];
    for (int i = 0; i < 3; ++i) {
        const double f0 = 0.04 * (1.0 - metallic) + c[i] * metallic;
        const double F = f0 + (1.0 - f0) * w;
        out[i] = F * spec + (1.0 - F) * (1.0 - metallic) * c[i] / kPi;
    }
    return {static_cast<f32>(out[0]), static_cast<f32>(out[1]), static_cast<f32>(out[2])};
}

Vec3 randomHemisphere(std::mt19937& rng, f32 minCos) {
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    const f32 z = minCos + (1.f - minCos) * u(rng);
    const f32 r = std::sqrt(std::max(0.f, 1.f - z * z));
    const f32 phi = 2.f * static_cast<f32>(kPi) * u(rng);
    return {r * std::cos(phi), r * std::sin(phi), z};
}

void testBrdfAgainstReference() {
    const Vec3 N{0.f, 0.f, 1.f};
    std::mt19937 rng(11u);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    double maxRel = 0.0;
    double maxReciprocity = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const Vec3 V = randomHemisphere(rng, 0.05f);
        const Vec3 L = randomHemisphere(rng, 0.05f);
        const Vec3 albedo{u(rng), u(rng), u(rng)};
        const f32 roughness = 0.05f + 0.95f * u(rng);
        const f32 metallic = u(rng);
        const Vec3 f = Brdf::evaluate(albedo, roughness, metallic, N, V, L);
        const Vec3 ref = referenceBrdf(albedo, roughness, metallic, N, V, L);
        const f32 scale = std::max(maxAbs(ref), 1e-3f);
        maxRel = std::max<double>(maxRel, maxAbs(f - ref) / scale);
        const Vec3 fSwap = Brdf::evaluate(albedo, roughness, metallic, N, L, V);
        maxReciprocity = std::max<double>(maxReciprocity, maxAbs(f - fSwap) / std::max(maxAbs(f), 1e-3f));
    }
    std::printf("  brdf: max relative error vs angle-form GGX/Heitz-Smith reference %.3g; reciprocity %.3g\n",
                maxRel, maxReciprocity);
    expectLess(maxRel, 2e-3, "BRDF matches independent Cook-Torrance reference");
    expectLess(maxReciprocity, 1e-4, "BRDF reciprocity f(V,L) = f(L,V)");

    // D normalisation: 2 pi \int D(cos) cos sin dtheta = 1 (projected microfacet area).
    double maxNormErr = 0.0;
    for (const f32 r : {0.1f, 0.25f, 0.5f, 0.75f, 1.f}) {
        const int steps = 400000;
        double sum = 0.0;
        for (int k = 0; k < steps; ++k) {
            const double th = (k + 0.5) * (kPi / 2.0) / steps;
            sum += Brdf::dGgx(static_cast<f32>(std::cos(th)), r) * std::cos(th) * std::sin(th);
        }
        maxNormErr = std::max(maxNormErr, std::fabs(2.0 * kPi * sum * (kPi / 2.0) / steps - 1.0));
    }
    std::printf("  brdf: GGX D normalisation max error %.3g\n", maxNormErr);
    expectLess(maxNormErr, 1e-3, "GGX D integrates to 1 over projected hemisphere");
}

// Directional albedo E(V) = \int f(V,L) NoL dL by stratified GGX half-vector importance sampling.
double directionalAlbedo(const Vec3& albedo, f32 roughness, f32 metallic, f32 NoV, int grid) {
    const Vec3 N{0.f, 0.f, 1.f};
    const Vec3 V{std::sqrt(1.f - NoV * NoV), 0.f, NoV};
    const double a = static_cast<double>(roughness) * roughness;
    double sum = 0.0;
    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            const double u1 = (i + 0.5) / grid;
            const double u2 = (j + 0.5) / grid;
            const double cosH = std::sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1));
            const double sinH = std::sqrt(std::max(0.0, 1.0 - cosH * cosH));
            const double phi = 2.0 * kPi * u2;
            const Vec3 H{static_cast<f32>(sinH * std::cos(phi)), static_cast<f32>(sinH * std::sin(phi)),
                         static_cast<f32>(cosH)};
            const f32 VoH = V.dot(H);
            const Vec3 L = H * (2.f * VoH) - V;
            if (VoH <= 0.f || L.z <= 0.f) {
                continue;
            }
            const double pdf = Brdf::dGgx(static_cast<f32>(cosH), roughness) * cosH / (4.0 * VoH);
            const Vec3 f = Brdf::evaluate(albedo, roughness, metallic, N, V, L);
            sum += f.x * L.z / pdf;
        }
    }
    return sum / (static_cast<double>(grid) * grid);
}

// Directional albedo by stratified cosine sampling (diffuse-dominated lobes).
double directionalAlbedoCosine(const Vec3& albedo, f32 roughness, f32 metallic, f32 NoV, int grid) {
    const Vec3 N{0.f, 0.f, 1.f};
    const Vec3 V{std::sqrt(1.f - NoV * NoV), 0.f, NoV};
    double sum = 0.0;
    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            const double r = std::sqrt((i + 0.5) / grid);
            const double phi = 2.0 * kPi * (j + 0.5) / grid;
            const Vec3 L{static_cast<f32>(r * std::cos(phi)), static_cast<f32>(r * std::sin(phi)),
                         static_cast<f32>(std::sqrt(std::max(0.0, 1.0 - r * r)))};
            sum += Brdf::evaluate(albedo, roughness, metallic, N, V, L).x * kPi; // f cos / (cos / pi)
        }
    }
    return sum / (static_cast<double>(grid) * grid);
}

void testWhiteFurnace() {
    // F0 = 1 metal: the specular lobe alone must never return more than it receives. Single-scatter
    // GGX loses energy with roughness (no multiple-scattering term), most at rough + grazing.
    const Vec3 white{1.f, 1.f, 1.f};
    double maxMetal = 0.0;
    double minMetal = 2.0;
    double minSmoothMetal = 2.0;
    for (const f32 r : {0.1f, 0.3f, 0.5f, 0.8f, 1.f}) {
        for (const f32 NoV : {1.f, 0.7f, 0.4f, 0.15f, 0.05f}) {
            const double e = directionalAlbedo(white, r, 1.f, NoV, 512);
            maxMetal = std::max(maxMetal, e);
            minMetal = std::min(minMetal, e);
            if (r <= 0.3f && NoV >= 0.15f) {
                minSmoothMetal = std::min(minSmoothMetal, e);
            }
        }
    }
    // White dielectric: B5.3's uncoupled Lambert + GGX (diffuse weighted by 1 - F(VoH), as in the UE4
    // model) conserves energy at normal-ish view but gains energy at grazing view, where Fresnel
    // specular grows but the diffuse weight barely drops. Measured and bounded, not hidden.
    double maxDielectricFrontal = 0.0;
    double maxDielectricGrazing = 0.0;
    for (const f32 r : {0.3f, 0.5f, 0.8f, 1.f}) {
        for (const f32 NoV : {1.f, 0.7f, 0.4f, 0.15f, 0.05f}) {
            const double e = directionalAlbedoCosine(white, r, 0.f, NoV, 512);
            if (NoV >= 0.7f) {
                maxDielectricFrontal = std::max(maxDielectricFrontal, e);
            } else {
                maxDielectricGrazing = std::max(maxDielectricGrazing, e);
            }
        }
    }
    std::printf("  white furnace: F0=1 metal E(V) in [%.4f, %.4f] (rough<=0.3, NoV>=0.15: >= %.4f); "
                "white dielectric max %.4f (NoV>=0.7), %.4f (grazing)\n",
                minMetal, maxMetal, minSmoothMetal, maxDielectricFrontal, maxDielectricGrazing);
    expectLess(maxMetal, 1.01, "white furnace: metal never reflects more energy than received");
    expectTrue(minSmoothMetal > 0.85, "white furnace: smooth metal loses < 15%");
    expectTrue(minMetal > 0.25, "white furnace: rough single-scatter loss bounded");
    // Closed form: at alpha = 1 GGX is uniform (D = 1/pi) and with V = N the height-correlated G2 is
    // 2 cos / (1 + cos), so E = \int_0^1 mu / (1 + mu) dmu = 1 - ln 2.
    const double eRough = directionalAlbedo(white, 1.f, 1.f, 1.f, 512);
    std::printf("  white furnace: alpha=1 normal incidence E %.5f vs closed form 1 - ln2 = %.5f\n", eRough,
                1.0 - std::log(2.0));
    expectLess(std::fabs(eRough - (1.0 - std::log(2.0))), 2e-3, "white furnace matches closed form 1 - ln 2");
    expectLess(maxDielectricFrontal, 1.01, "white furnace: white dielectric <= 1 at NoV >= 0.7");
    expectLess(maxDielectricGrazing, 1.35, "white furnace: grazing dielectric gain stays at the model's known level");
}

void testMetallicSweep() {
    const Vec3 N{0.f, 0.f, 1.f};
    const Vec3 albedos[] = {{1.f, 0.f, 0.f}, {1.f, 0.78f, 0.34f}, {0.95f, 0.95f, 0.95f}, {0.02f, 0.02f, 0.02f},
                            {0.2f, 0.5f, 0.9f}};
    const Vec3 views[] = {Vec3{0.f, 0.f, 1.f}, Vec3{0.6f, 0.f, 0.8f}, Vec3{0.97f, 0.f, 0.243f}.normalized()};
    const Vec3 lights[] = {Vec3{0.f, 0.f, 1.f}, Vec3{-0.6f, 0.f, 0.8f}, Vec3{0.3f, 0.5f, 0.81f}.normalized(),
                           Vec3{-0.9f, 0.1f, 0.2f}.normalized()};
    const int steps = 1000;
    const f32 h = 1.f / steps;
    double worstStepRatio = 0.0;  // max per-step change / smooth bound
    double worstSecondDiff = 0.0; // max |L(m+h) - 2L(m) + L(m-h)| / h^2
    double worstEndpoint = 0.0;
    double worstQuantizedRatio = 0.0;
    double worstChroma = 0.0;
    for (const Vec3& albedo : albedos) {
        for (const f32 roughness : {0.1f, 0.35f, 0.6f, 0.9f}) {
            for (const Vec3& V : views) {
                for (const Vec3& L : lights) {
                    std::vector<Vec3> c(steps + 1);
                    for (int k = 0; k <= steps; ++k) {
                        c[k] = Brdf::shade(albedo, roughness, static_cast<f32>(k) * h, N, V, L);
                    }
                    // Endpoints against the independent reference (pure dielectric / pure metal).
                    const f32 nol = N.dot(L);
                    worstEndpoint = std::max<double>(
                        worstEndpoint, maxAbs(c[0] - referenceBrdf(albedo, roughness, 0.0, N, V, L) * nol) /
                                           std::max(maxAbs(c[0]), 1e-3f));
                    worstEndpoint = std::max<double>(
                        worstEndpoint, maxAbs(c[steps] - referenceBrdf(albedo, roughness, 1.0, N, V, L) * nol) /
                                           std::max(maxAbs(c[steps]), 1e-3f));

                    // L(m) is a quadratic in m (F0 is linear in m, diffuse weight (1-F)(1-m)), so a
                    // continuous transition has constant L'' and |dL/dm| <= |L(1) - L(0)| + 1.5 |L''|.
                    // Curvature is measured with a wide stride so float rounding does not dominate.
                    const f32 endDelta = maxAbs(c[steps] - c[0]);
                    const int stride = 250;
                    const f32 hs = static_cast<f32>(stride) * h;
                    f32 maxCurv = 0.f;
                    for (int k = stride; k + stride <= steps; k += stride) {
                        maxCurv = std::max(maxCurv, maxAbs(c[k + stride] - c[k] * 2.f + c[k - stride]) / (hs * hs));
                    }
                    worstSecondDiff = std::max<double>(worstSecondDiff, maxCurv);
                    const f32 bound = (endDelta + 1.5f * maxCurv) * h * 1.001f + 1e-6f;
                    f32 maxStep = 0.f;
                    for (int k = 0; k < steps; ++k) {
                        maxStep = std::max(maxStep, maxAbs(c[k + 1] - c[k]));
                    }
                    worstStepRatio = std::max<double>(worstStepRatio, maxStep / bound);

                    // Chromaticity (colour, not brightness) must also move continuously. Near-black
                    // samples (off-lobe smooth metal) are skipped: their hue is ill-conditioned.
                    f32 peakSum = 0.f;
                    for (const Vec3& ck : c) {
                        peakSum = std::max(peakSum, ck.x + ck.y + ck.z);
                    }
                    const f32 minSum = std::max(1e-3f, 0.02f * peakSum);
                    for (int k = 0; k < steps; ++k) {
                        const f32 s0 = c[k].x + c[k].y + c[k].z;
                        const f32 s1 = c[k + 1].x + c[k + 1].y + c[k + 1].z;
                        if (s0 > minSum && s1 > minSum) {
                            worstChroma = std::max<double>(worstChroma, maxAbs(c[k] * (1.f / s0) - c[k + 1] * (1.f / s1)));
                        }
                    }

                    // Through the G-buffer: metallic lives in RT2 (RGBA8) -> 256 levels.
                    f32 maxQStep = 0.f;
                    Vec3 prev = Brdf::shade(albedo, roughness, 0.f, N, V, L);
                    for (int q = 1; q <= 255; ++q) {
                        const f32 m = GBufferQuantize::toUnorm8(static_cast<f32>(q) / 255.f);
                        const Vec3 cur = Brdf::shade(albedo, roughness, m, N, V, L);
                        maxQStep = std::max(maxQStep, maxAbs(cur - prev));
                        prev = cur;
                    }
                    worstQuantizedRatio = std::max<double>(worstQuantizedRatio,
                                                           maxQStep / ((endDelta + 1.5f * maxCurv) / 255.f * 1.001f + 1e-6f));
                }
            }
        }
    }
    std::printf("  metallic sweep (%d steps, 5 albedos x 4 roughness x 12 V/L): max step / smooth bound %.3f, "
                "max |L''| %.3f, endpoint rel err %.3g, max chroma step %.4g, RGBA8 step / bound %.3f\n",
                steps, worstStepRatio, worstSecondDiff, worstEndpoint, worstChroma, worstQuantizedRatio);
    expectLess(worstStepRatio, 1.0, "GATE: metallic sweep per-step colour change within smooth bound");
    // Analytic: L'' = 2 (albedo - 0.04)(1 - Fresnel weight) albedo NoL / pi <= 2 / pi.
    expectLess(worstSecondDiff, 2.0 / kPi + 1e-3, "metallic sweep curvature within analytic bound 2/pi");
    expectLess(worstEndpoint, 2e-3, "metallic endpoints match pure dielectric / pure metal reference");
    expectLess(worstChroma, 0.01, "metallic sweep chromaticity moves continuously");
    expectLess(worstQuantizedRatio, 1.0, "metallic through RGBA8 G-buffer stays within smooth bound");
}

// ---------------------------------------------------------------------------------------------
// Row 3: procedural wood / metal / concrete on SDF surfaces — no seams, no tiling.
// ---------------------------------------------------------------------------------------------

f32 sceneSdf(const Vec3& p) {
    const f32 sphere = fuse::math::SDF::sphere(p - Vec3{-1.2f, 0.3f, 0.f}, 1.5f);
    const f32 box = fuse::math::SDF::box(p - Vec3{1.4f, -0.2f, 0.4f}, {1.1f, 0.8f, 1.3f});
    return fuse::math::SDF::opSmoothUnion(sphere, box, 0.4f);
}

Vec3 sdfGradient(const std::function<f32(const Vec3&)>& sdf, const Vec3& p) {
    const f32 e = 1e-3f;
    return Vec3{sdf(p + Vec3{e, 0, 0}) - sdf(p - Vec3{e, 0, 0}), sdf(p + Vec3{0, e, 0}) - sdf(p - Vec3{0, e, 0}),
                sdf(p + Vec3{0, 0, e}) - sdf(p - Vec3{0, 0, e})}
        .normalized();
}

// Newton projection of a point onto the zero level set.
Vec3 projectToSurface(const std::function<f32(const Vec3&)>& sdf, Vec3 p) {
    for (int i = 0; i < 32; ++i) {
        const f32 d = sdf(p);
        if (std::fabs(d) < 1e-6f) {
            break;
        }
        p = p - sdfGradient(sdf, p) * d;
    }
    return p;
}

std::vector<f32> sampleChannels(const MaterialSample& s) {
    return {s.albedo.x, s.albedo.y, s.albedo.z, s.roughness, s.metallic};
}

f32 channelDiff(const MaterialSample& a, const MaterialSample& b) {
    const std::vector<f32> ca = sampleChannels(a);
    const std::vector<f32> cb = sampleChannels(b);
    f32 d = 0.f;
    for (usize i = 0; i < ca.size(); ++i) {
        d = std::max(d, std::fabs(ca[i] - cb[i]));
    }
    return d;
}

// Seam detector: J(eps) = max |f(p + eps t) - f(p)| over surface points p and tangents t. For a
// continuous (Lipschitz) field J shrinks ~linearly with eps; a seam keeps J(eps) ~ jump height.
struct SeamStats {
    f32 j1e3 = 0.f;
    f32 j1e4 = 0.f;
};

SeamStats seamStats(const std::function<MaterialSample(const Vec3&)>& material,
                    const std::vector<Vec3>& points,
                    const std::vector<Vec3>& tangents) {
    SeamStats s{};
    for (usize i = 0; i < points.size(); ++i) {
        const MaterialSample base = material(points[i]);
        s.j1e3 = std::max(s.j1e3, channelDiff(base, material(points[i] + tangents[i] * 1e-3f)));
        s.j1e4 = std::max(s.j1e4, channelDiff(base, material(points[i] + tangents[i] * 1e-4f)));
    }
    return s;
}

void testProceduralMaterialsOnSdf() {
    // Surface points: random points projected onto a smooth-union SDF scene, plus points pinned to
    // the box's edges/corners and to integer lattice planes (noise cell faces).
    std::mt19937 rng(23u);
    std::uniform_real_distribution<f32> u(-3.f, 3.f);
    const std::function<f32(const Vec3&)> sdf = sceneSdf;
    std::vector<Vec3> points;
    std::vector<Vec3> tangents;
    for (int i = 0; i < 4000; ++i) {
        const Vec3 p = projectToSurface(sdf, {u(rng), u(rng), u(rng)});
        if (std::fabs(sdf(p)) > 1e-4f) {
            continue;
        }
        const Vec3 n = sdfGradient(sdf, p);
        const Vec3 helper = std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        points.push_back(p);
        tangents.push_back(fuse::math::cross(n, helper).normalized());
    }
    for (int i = 0; i < 1000; ++i) {
        // Straddle lattice planes: x exactly on an integer / 40 (concrete pore frequency) and integers.
        Vec3 p = projectToSurface(sdf, {u(rng), u(rng), u(rng)});
        p.x = std::round(p.x * 40.f) / 40.f - 5e-5f;
        points.push_back(p);
        tangents.push_back({1.f, 0.f, 0.f});
        p.x = std::round(p.x) - 5e-5f;
        points.push_back(p);
        tangents.push_back({1.f, 0.f, 0.f});
    }
    const usize surfaceCount = points.size();

    // Sensitivity check of the detector: a UV-style fract() pattern (a tiling seam) must be flagged.
    const auto seamy = [](const Vec3& p) {
        MaterialSample s{};
        const f32 f = p.x - std::floor(p.x);
        s.albedo = {f, f, f};
        return s;
    };
    const SeamStats seamRef = seamStats(seamy, points, tangents);
    expectTrue(seamRef.j1e4 > 0.5f, "seam detector flags a fract() tiling seam");

    struct Case {
        const char* name;
        ProceduralMaterialId id;
    };
    const Case cases[] = {{"wood", ProceduralMaterialId::Wood},
                          {"metal", ProceduralMaterialId::Metal},
                          {"concrete", ProceduralMaterialId::Concrete}};
    for (const Case& c : cases) {
        for (const u32 seed : {0u, 1u, 12345u}) {
            const auto mat = [&](const Vec3& p) { return ProceduralMaterials::evaluate(c.id, p, seed); };

            // Ranges / validity and variation.
            bool inRange = true;
            double mean[3] = {0, 0, 0};
            double mean2 = 0.0;
            f32 metalMin = 2.f;
            f32 metalMax = -1.f;
            f32 roughMin = 2.f;
            for (usize i = 0; i < surfaceCount; ++i) {
                const MaterialSample s = mat(points[i]);
                for (const f32 v : sampleChannels(s)) {
                    inRange = inRange && std::isfinite(v) && v >= 0.f && v <= 1.f;
                }
                inRange = inRange && s.roughness >= 0.05f;
                mean[0] += s.albedo.x;
                mean[1] += s.albedo.y;
                mean[2] += s.albedo.z;
                mean2 += s.albedo.y * s.albedo.y;
                metalMin = std::min(metalMin, s.metallic);
                metalMax = std::max(metalMax, s.metallic);
                roughMin = std::min(roughMin, s.roughness);
            }
            for (double& m : mean) {
                m /= static_cast<double>(surfaceCount);
            }
            const double stddev = std::sqrt(std::max(0.0, mean2 / surfaceCount - mean[1] * mean[1]));
            expectTrue(inRange, "procedural outputs finite and in [0,1], roughness >= 0.05");
            expectTrue(stddev > 0.005, "procedural albedo varies over the surface");
            if (c.id == ProceduralMaterialId::Metal) {
                expectTrue(metalMin == 1.f && metalMax == 1.f, "metal is fully metallic");
            } else {
                expectTrue(metalMin == 0.f && metalMax == 0.f, "wood/concrete are dielectric");
            }
            if (c.id == ProceduralMaterialId::Wood) {
                expectTrue(mean[0] > mean[1] && mean[1] > mean[2], "wood albedo is warm (r > g > b)");
            }
            if (c.id == ProceduralMaterialId::Concrete) {
                expectTrue(roughMin > 0.75f && std::fabs(mean[0] - mean[2]) < 0.1, "concrete is rough and grey");
            }

            // Seams.
            const SeamStats st = seamStats(mat, points, tangents);
            if (seed == 0u) {
                std::printf("  %-8s on SDF: %zu surface pts, albedo mean (%.2f %.2f %.2f) sd %.3f, "
                            "J(1e-3) %.4f, J(1e-4) %.5f (ratio %.3f)\n",
                            c.name, surfaceCount, mean[0], mean[1], mean[2], stddev, st.j1e3, st.j1e4,
                            st.j1e3 > 0.f ? st.j1e4 / st.j1e3 : 0.f);
            }
            expectLess(st.j1e4, 0.02, "GATE: no seam — max change over 1e-4 surface step");
            expectLess(st.j1e4, 0.2f * st.j1e3 + 1e-4f, "GATE: change scales with step (continuous field)");

            // Tiling: shifting by candidate periods (incl. 256 / 289, the classic permutation-table
            // periods, at each noise frequency's lattice scale) must decorrelate like a random shift.
            double randomDiff = 0.0;
            for (usize i = 0; i + 1 < surfaceCount; ++i) {
                randomDiff += channelDiff(mat(points[i]), mat(points[(i * 7919u + 13u) % surfaceCount]));
            }
            randomDiff /= static_cast<double>(surfaceCount - 1);
            double worstRatio = 1e9;
            for (const f32 period : {1.f, 2.f, 10.f, 25.6f, 64.f, 256.f, 289.f, 512.f, 1024.f, 6.4f, 7.225f}) {
                for (int axis = 0; axis < 3; ++axis) {
                    const Vec3 shift = axis == 0 ? Vec3{period, 0, 0} : axis == 1 ? Vec3{0, period, 0} : Vec3{0, 0, period};
                    double d = 0.0;
                    for (usize i = 0; i < surfaceCount; i += 5) {
                        d += channelDiff(mat(points[i]), mat(points[i] + shift));
                    }
                    d /= static_cast<double>((surfaceCount + 4) / 5);
                    worstRatio = std::min(worstRatio, d / randomDiff);
                }
            }
            if (seed == 0u) {
                std::printf("  %-8s tiling: min shifted/random difference ratio %.3f (1 = uncorrelated, 0 = tiles)\n",
                            c.name, worstRatio);
            }
            expectTrue(worstRatio > 0.3, "GATE: no tiling — periodic shifts decorrelate");
        }
    }

    // Noise field itself: zero on lattice points, bounded, C1 across cell faces, no 256-period.
    double maxLattice = 0.0;
    double maxAbsNoise = 0.0;
    double maxGradJump = 0.0;
    std::uniform_real_distribution<f32> big(-200.f, 200.f);
    for (int i = 0; i < 20000; ++i) {
        const Vec3 lattice{std::floor(big(rng)), std::floor(big(rng)), std::floor(big(rng))};
        maxLattice = std::max<double>(maxLattice, std::fabs(ProceduralNoise::perlin(lattice, 5u)));
        const Vec3 p{big(rng), big(rng), big(rng)};
        maxAbsNoise = std::max<double>(maxAbsNoise, std::fabs(ProceduralNoise::perlin(p, 5u)));
        // One-sided x-derivatives on both sides of the face x = k.
        const Vec3 f{std::round(p.x), p.y, p.z};
        const f32 e = 1e-3f;
        const f32 left = (ProceduralNoise::perlin(f, 5u) - ProceduralNoise::perlin(f - Vec3{e, 0, 0}, 5u)) / e;
        const f32 right = (ProceduralNoise::perlin(f + Vec3{e, 0, 0}, 5u) - ProceduralNoise::perlin(f, 5u)) / e;
        maxGradJump = std::max<double>(maxGradJump, std::fabs(left - right));
    }
    std::printf("  noise: |n(lattice)| max %.2g, |n| max %.3f, gradient jump across cell faces %.4f\n", maxLattice,
                maxAbsNoise, maxGradJump);
    expectLess(maxLattice, 1e-6, "gradient noise vanishes on lattice points");
    expectLess(maxAbsNoise, 1.2, "gradient noise bounded");
    expectLess(maxGradJump, 0.05, "gradient noise is C1 across cell faces");
}

// ---------------------------------------------------------------------------------------------
// Row 4: bindless material SSBO lookup for 1000 materials in one frame.
// Row 5: emissive surfaces contribute correct radiance to GI probe rays.
// ---------------------------------------------------------------------------------------------

u32 hashU32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

f32 hashUnit(u32 x) {
    return static_cast<f32>(hashU32(x) & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
}

Material makeMaterial(u32 i, const std::vector<TextureHandle>& textures) {
    Material m{};
    m.baseColor = {hashUnit(i * 3u), hashUnit(i * 3u + 1u), hashUnit(i * 3u + 2u)};
    m.roughness = static_cast<f32>(i) / 999.f;
    m.metallic = hashUnit(i ^ 0xABCDu);
    m.emissiveColor = (i % 5u == 0u) ? Vec3{hashUnit(i + 7u), hashUnit(i + 8u), hashUnit(i + 9u)} : Vec3{};
    m.emissiveIntensity = (i % 5u == 0u) ? static_cast<f32>(i % 50u) + 0.5f : 0.f;
    m.shadingModel = static_cast<ShadingModel>(i % 6u);
    m.normalStrength = 0.5f + hashUnit(i + 99u);
    m.parameters.subsurface.scatterRadius = static_cast<f32>(i);
    if (i % 7u == 0u) {
        m.isProcedural = true;
        m.proceduralFnId = 1u + (i / 7u) % 3u;
        m.proceduralSeed = i;
    }
    if (i % 3u == 0u) {
        m.baseColorTex = textures[i % textures.size()];
    }
    if (i % 4u == 0u) {
        m.normalTex = textures[(i / 4u) % textures.size()];
    }
    return m;
}

void testMaterialSsbo1000AndEmissive() {
    VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap for material SSBO gate");
    if (bootstrap == nullptr) {
        return;
    }
    BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());
    ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager for material SSBO gate");

    // A few real sampled textures; SSBO rows must carry their bindless heap slots.
    std::vector<TextureHandle> textures;
    for (u32 i = 0; i < 16u; ++i) {
        TextureDesc desc{};
        desc.width = 4;
        desc.height = 4;
        desc.format = GpuFormat::R8G8B8A8Unorm;
        desc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) |
                                             static_cast<u32>(ImageUsage::TransferDst));
        desc.name = "material_gate_tex";
        textures.push_back(resources.createTexture(desc));
        expectTrue(textures.back().isValid(), "material gate texture created");
    }

    MaterialSystem system;
    system.init(resources);
    std::vector<Material> source;
    // Register 10, flush, then 990 more: the SSBO must grow and receive every row.
    for (u32 i = 0; i < 1000u; ++i) {
        source.push_back(makeMaterial(i, textures));
        expectTrue(system.registerMaterial(source.back()) == i, "material id is registration order");
        if (i == 9u) {
            system.flushGpuBuffer();
        }
    }
    system.flushGpuBuffer();
    // In-place update of one row after the table exists.
    source[500].baseColor = {0.123f, 0.456f, 0.789f};
    source[500].emissiveColor = {1.f, 0.5f, 0.25f};
    source[500].emissiveIntensity = 8.f;
    system.updateMaterial(500u, source[500]);
    system.flushGpuBuffer();

    const Buffer* ssbo = resources.getBuffer(system.materialSsbo());
    expectTrue(ssbo != nullptr && ssbo->bindlessIndex != UINT32_MAX, "material SSBO registered in bindless heap");
    const usize bytes = 1000u * MaterialLayout::kGpuMaterialStride;
    expectTrue(ssbo != nullptr && ssbo->desc.size >= bytes, "material SSBO sized for 1000 rows");
    std::vector<unsigned char> readback(bytes);
    expectTrue(resources.readBuffer(system.materialSsbo(), readback.data(), bytes), "material SSBO readback");

    // One frame: a 256x256 material-id buffer covering all 1000 ids in scrambled order; every pixel
    // fetches its row by id from the SSBO bytes and evaluates it at its world position.
    const u32 width = 256u;
    const u32 height = 256u;
    std::vector<bool> seen(1000u, false);
    u32 mismatches = 0;
    u32 textureIndexMismatches = 0;
    u32 textureIndicesDifferingFromHandleSlot = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 pixel = y * width + x;
            const u32 id = (pixel < 1000u) ? (pixel * 617u) % 1000u : hashU32(pixel) % 1000u;
            seen[id] = true;
            Material::GPUMaterial row{};
            if (!MaterialLayout::fetchRow(readback.data(), readback.size(), id, row)) {
                ++mismatches;
                continue;
            }
            const Material& m = source[id];
            const Vec3 worldPos{static_cast<f32>(x) * 0.05f, static_cast<f32>(y) * 0.05f, 1.f};
            const MaterialSample s = MaterialEval::sample(row, worldPos);
            // Expected sample built straight from the authoring material (not through pack()).
            MaterialSample expect{};
            if (m.isProcedural) {
                expect = ProceduralMaterials::evaluate(static_cast<ProceduralMaterialId>(m.proceduralFnId), worldPos,
                                                       m.proceduralSeed);
            } else {
                expect.albedo = m.baseColor;
                expect.roughness = m.roughness;
                expect.metallic = m.metallic;
            }
            const Vec3 expectEmissive = m.emissiveColor * m.emissiveIntensity;
            const bool ok = channelDiff(s, expect) == 0.f && maxAbs(s.emissive - expectEmissive) <= 1e-6f &&
                            row.shadingModel == static_cast<u32>(m.shadingModel) &&
                            row.subsurfaceBlock.w == m.parameters.subsurface.scatterRadius &&
                            row.normalStrength == m.normalStrength &&
                            ((row.flags & MaterialFlagBits::kProcedural) != 0u) == m.isProcedural;
            if (!ok) {
                ++mismatches;
            }
            if (pixel < 1000u) {
                const auto bindlessOf = [&](const TextureHandle& h) {
                    const Texture* t = resources.getTexture(h);
                    return t != nullptr ? t->bindlessIndex : UINT32_MAX;
                };
                if (row.baseColorTexIdx != bindlessOf(m.baseColorTex) || row.normalTexIdx != bindlessOf(m.normalTex) ||
                    ((row.flags & MaterialFlagBits::kHasNormalMap) != 0u) != m.normalTex.isValid()) {
                    ++textureIndexMismatches;
                }
                if (m.baseColorTex.isValid() && row.baseColorTexIdx != m.baseColorTex.index()) {
                    ++textureIndicesDifferingFromHandleSlot;
                }
            }
        }
    }
    const u32 covered = static_cast<u32>(std::count(seen.begin(), seen.end(), true));
    std::printf("  material SSBO: 1000 rows x %zu B, %u/1000 ids referenced in a %ux%u frame, %u row mismatches, "
                "%u bindless texture mismatches (%u rows where heap slot != handle slot)\n",
                MaterialLayout::kGpuMaterialStride, covered, width, height, mismatches, textureIndexMismatches,
                textureIndicesDifferingFromHandleSlot);
    expectTrue(covered == 1000u, "all 1000 materials referenced in one frame");
    expectTrue(mismatches == 0u, "GATE: every pixel's bindless material lookup returns its own material");
    expectTrue(textureIndexMismatches == 0u, "texture slots in SSBO rows are bindless heap indices");
    Material::GPUMaterial oob{};
    expectTrue(!MaterialLayout::fetchRow(readback.data(), readback.size(), 1000u, oob), "out-of-range id rejected");

    // Procedural id field never spills into the texture-presence flags.
    Material wide{};
    wide.isProcedural = true;
    wide.proceduralFnId = 0xFFFFFu;
    expectTrue((wide.pack().flags & (MaterialFlagBits::kHasNormalMap | MaterialFlagBits::kHasAoMap |
                                     MaterialFlagBits::kHasMetallicMap)) == 0u,
               "large procedural id does not set texture flag bits");

    // ---- Row 5: emissive radiance into GI probes -------------------------------------------------
    Material::GPUMaterial emissiveRow{};
    expectTrue(MaterialLayout::fetchRow(readback.data(), readback.size(), 500u, emissiveRow), "emissive row fetch");
    const Vec3 le = MaterialEval::emissiveRadiance(emissiveRow);
    expectTrue(maxAbs(le - Vec3{8.f, 4.f, 2.f}) < 1e-6f, "emissive radiance = colour x intensity");
    Material::GPUMaterial dark{};
    std::memcpy(&dark, readback.data() + 1u * MaterialLayout::kGpuMaterialStride, sizeof(dark));
    expectTrue(maxAbs(MaterialEval::emissiveRadiance(dark)) == 0.f, "non-emissive material emits nothing");

    // Probe at the origin looking up +Z; an emissive SDF sphere of radius R at distance d. Probe rays
    // are sphere-traced against the SDF, the hit's material row is evaluated, and the cosine-weighted
    // estimator gives irradiance. Reference: E = pi Le sin^2(alpha) cos(beta) (sphere fully above the
    // horizon, alpha = asin(R/d), beta = angle between normal and sphere centre).
    const auto probeIrradiance = [&](const Vec3& centre, f32 radius, const Material::GPUMaterial& row,
                                     const Vec3& bounceIrradiance) {
        const int grid = 384;
        Vec3 sum{};
        for (int i = 0; i < grid; ++i) {
            for (int j = 0; j < grid; ++j) {
                const f32 r = std::sqrt((i + 0.5f) / grid);
                const f32 phi = 2.f * static_cast<f32>(kPi) * (j + 0.5f) / grid;
                const Vec3 dir{r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.f, 1.f - r * r))};
                f32 t = 0.f;
                for (int step = 0; step < 256; ++step) {
                    const f32 d = fuse::math::SDF::sphere(dir * t - centre, radius);
                    if (d < 1e-5f) {
                        const MaterialSample hit = MaterialEval::sample(row, dir * t);
                        sum = sum + MaterialEval::probeRayRadiance(hit, bounceIrradiance);
                        break;
                    }
                    t += d;
                    if (t > 100.f) {
                        break;
                    }
                }
            }
        }
        return sum * (static_cast<f32>(kPi) / static_cast<f32>(grid * grid)); // E = pi * mean(L) for cos pdf
    };

    double worstRel = 0.0;
    struct Config {
        Vec3 centre;
        f32 radius;
    };
    const Config configs[] = {{{0.f, 0.f, 4.f}, 1.f}, {{0.f, 0.f, 2.f}, 1.5f}, {{1.5f, 0.f, 3.f}, 0.8f},
                              {{-1.f, 2.f, 5.f}, 1.2f}};
    for (const Config& cfg : configs) {
        const f32 dist = cfg.centre.length();
        const f32 sin2 = (cfg.radius / dist) * (cfg.radius / dist);
        const f32 cosBeta = cfg.centre.z / dist;
        const Vec3 expected = le * (static_cast<f32>(kPi) * sin2 * cosBeta);
        const Vec3 e = probeIrradiance(cfg.centre, cfg.radius, emissiveRow, {});
        worstRel = std::max<double>(worstRel, maxAbs(e - expected) / maxAbs(expected));
    }
    std::printf("  emissive -> probe: SDF-traced cosine-weighted probe irradiance vs analytic pi Le sin^2(a) cos(b): "
                "max rel err %.4f\n",
                worstRel);
    expectLess(worstRel, 0.01, "GATE: emissive sphere irradiance at probe matches analytic within 1%");

    // Emission adds to (does not replace) the diffuse bounce: L = Le + rho (1 - m) E / pi.
    Material::GPUMaterial bounceRow = emissiveRow;
    bounceRow.flags = 0u;
    const Vec3 irr{2.f, 1.f, 0.5f};
    const MaterialSample bs = MaterialEval::sample(bounceRow, {});
    const Vec3 radiance = MaterialEval::probeRayRadiance(bs, irr);
    const Vec3 expectBounce = le + mul(bs.albedo, irr) * ((1.f - bs.metallic) / static_cast<f32>(kPi));
    expectTrue(maxAbs(radiance - expectBounce) < 1e-5f, "probe ray radiance = emission + Lambertian bounce");

    // The same emission reaches deferred GI through G-buffer RT5 (RGBA16F): precision and mask.
    double worstRt5 = 0.0;
    bool noInf = true;
    for (u32 i = 0; i < 1000u; i += 5u) {
        Material::GPUMaterial row{};
        (void)MaterialLayout::fetchRow(readback.data(), readback.size(), i, row);
        GBufferPackedData data{};
        data.normal = {0.f, 0.f, 1.f};
        data.emissive = MaterialEval::emissiveRadiance(row);
        const GBufferMrt stored = GBufferPacking::quantizeToStorage(GBufferPacking::pack(data));
        const GBufferPackedData read = GBufferPacking::unpack(stored);
        if (maxAbs(data.emissive) > 0.f) {
            worstRt5 = std::max<double>(worstRt5, maxAbs(read.emissive - data.emissive) / maxAbs(data.emissive));
            expectTrue(stored.rt2.z == 1.f, "emissive mask set in RT2");
        }
    }
    GBufferPackedData hot{};
    hot.emissive = {1e6f, 70000.f, 1.f};
    const GBufferMrt hotStored = GBufferPacking::quantizeToStorage(GBufferPacking::pack(hot));
    noInf = std::isfinite(hotStored.rt5.x) && std::isfinite(hotStored.rt5.y);
    std::printf("  emissive -> RT5 (RGBA16F): max relative error %.2g\n", worstRt5);
    expectLess(worstRt5, 1e-3, "emissive radiance survives RGBA16F RT5 within half precision");
    expectTrue(noInf, "emissive beyond half range clamps instead of storing inf");

    system.destroy();
    for (const TextureHandle& t : textures) {
        resources.destroyTexture(t);
    }
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    std::printf("[B5 gate] G-buffer octahedral normals\n");
    testHalfConversionAgainstReference();
    testOctahedralRoundTrip();
    std::printf("[B5 gate] PBR dielectric -> metal\n");
    testBrdfAgainstReference();
    testWhiteFurnace();
    testMetallicSweep();
    std::printf("[B5 gate] procedural materials on SDF surfaces\n");
    testProceduralMaterialsOnSdf();
    std::printf("[B5 gate] material SSBO (1000 materials) + emissive -> GI probes\n");
    testMaterialSsbo1000AndEmissive();
    std::printf("[B5 gate] hardware-only (not measured here): G-buffer pass (1000 objects) < 2 ms; "
                "RenderDoc capture of the bindless material fetch\n");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b5_gbuffer_materials_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b5_gbuffer_materials_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
