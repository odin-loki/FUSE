// WP-8.2 Hillaire atmosphere: CPU gates (no device; also run in the stub tree). Lavapipe gates:
// test_rp_atmosphere_gpu.cpp.
//
//   layout     AtParams / AtPush sizes; the GLSL and Slang mirrors of AtParams (names, order, offsets); LUT buffer
//              sections 256-aligned and disjoint
//   luts       the CPU reference of every LUT: transmittance texels vs the brute-force optical depth; the
//              parametrisations round-trip; bilinear / trilinear helpers exact at texel centres and linear in
//              between; sky-view texels == the integrator on the same ray; multi-scattering texels == Psi of
//              their integrals; aerial slices == the integrator to the slice distance (single + MS);
//              transmittance through the froxels monotone
//   reference  sky radiance (sky-view LUT, bilinear, single scattering) vs the brute-force double single-scatter
//              reference of the B5 gates (atmosphere_brute_force.hpp): every channel within +-6% over 6 sun
//              elevations x 7 view elevations x 5 azimuths
//   energy     multi-scattering: 0 <= f_ms < 1 on every texel; Psi == the explicit sum of scattering orders;
//              ground irradiance (sky + direct) <= the top-of-atmosphere flux and MS adds 0 < dE; MS share of
//              the noon zenith radiance within a physical band
//   sweep      time of day -10 .. 90 degrees: sky finite and >= 0, continuous (bounded step-to-step change),
//              zenith radiance monotone in sun elevation
//   legacy     the additive bilinear samplers of the B5 TransmittanceLut / SkyLut
//   api        settings validation, static-LUT change detection, stub-backend behaviour
#include <fuse/renderer/atmosphere/atmosphere_brute_force.hpp>
#include <fuse/renderer/atmosphere/atmosphere_gpu.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/renderer/atmosphere/sky_lut.hpp>
#include <fuse/renderer/atmosphere/sky_scatter.hpp>
#include <fuse/renderer/atmosphere/transmittance_lut.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::atmosphere;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec3;

constexpr f64 kPiD = 3.14159265358979323846;
constexpr f64 kDeg = kPiD / 180.0;

int g_failures = 0;

void expect(bool condition, const char* message) {
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

Vec3 dirElAz(f64 el, f64 az) {
    return Vec3{static_cast<f32>(std::cos(el) * std::sin(az)), static_cast<f32>(std::sin(el)),
                static_cast<f32>(std::cos(el) * std::cos(az))};
}

AtParams params(const AtmosphereLutSettings& s, const AtmosphereLutView& v) {
    AtParams p{};
    const bool ok = resolve_at_params(s, v, p);
    expect(ok, "resolve_at_params");
    return p;
}

AtmosphereLutView sunView(f64 sunElDeg, f64 sunAzDeg = 0.0) {
    AtmosphereLutView v{};
    v.sunDirection = dirElAz(sunElDeg * kDeg, sunAzDeg * kDeg);
    return v;
}

// --- layout --------------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define AT_FIELD(n) Field{#n, offsetof(AtParams, n)}
const Field kFields[] = {
    AT_FIELD(transmittance), AT_FIELD(multiscatter), AT_FIELD(skyView), AT_FIELD(aerialScatter),
    AT_FIELD(aerialTransmittance), AT_FIELD(reserved0), AT_FIELD(transWidth), AT_FIELD(transHeight), AT_FIELD(msWidth),
    AT_FIELD(msHeight), AT_FIELD(skyWidth), AT_FIELD(skyHeight), AT_FIELD(apWidth), AT_FIELD(apHeight), AT_FIELD(apDepth),
    AT_FIELD(flags), AT_FIELD(transSteps), AT_FIELD(msSteps), AT_FIELD(msDirSqrt), AT_FIELD(skySteps),
    AT_FIELD(apStepsPerSlice), AT_FIELD(reserved1), AT_FIELD(bottomRadius), AT_FIELD(topRadius),
    AT_FIELD(rayleighHeight), AT_FIELD(mieHeight), AT_FIELD(rayleighScattering), AT_FIELD(mieScattering),
    AT_FIELD(mieExtinction), AT_FIELD(mieG), AT_FIELD(msFactor), AT_FIELD(ozoneAbsorption), AT_FIELD(ozoneCenter),
    AT_FIELD(ozoneHalfWidth), AT_FIELD(sunAngularRadius), AT_FIELD(sunDiskNorm), AT_FIELD(groundAlbedo),
    AT_FIELD(sunIlluminance), AT_FIELD(sunDir), AT_FIELD(up), AT_FIELD(cameraPos), AT_FIELD(camForward),
    AT_FIELD(camRight), AT_FIELD(camUp),
};
#undef AT_FIELD

/// Parses the block opened by `open` (e.g. "struct AtParams {") of a shader source up to its closing brace:
/// (name, std430 / scalar offset) per field.
bool parseShaderStruct(const std::string& path, const std::string& open, std::vector<size_t>& offsets,
                       size_t& size, std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const size_t begin = text.find(open);
    const size_t end = text.find("}", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + open.size(), end - begin - open.size()));
    std::string line;
    size_t offset = 0;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        size_t count = 1;
        const size_t bracket = decl.find('[');
        if (bracket != std::string::npos) {
            count = static_cast<size_t>(std::stoul(decl.substr(bracket + 1)));
            decl = decl.substr(0, bracket);
        }
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = (offset + 7u) / 8u * 8u;
    return true;
}

void testLayout() {
    expect(sizeof(AtParams) == 320u, "AtParams is 320 bytes");
    expect(sizeof(AtPush) == 32u, "AtPush is 32 bytes (<= 128 guaranteed push-constant bytes)");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_ATMOSPHERE_SHADER_DIR) + "/at_sample." + lang;
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, "struct AtParams {", offsets, size, names);
        expect(parsed, "at_sample struct AtParams parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == sizeof(AtParams);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(),
                             offsets[i], kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: at_sample.%s AtParams %zu fields, %zu bytes\n", lang, offsets.size(), size);
        expect(same, "shader AtParams == C++ AtParams (names, order, offsets, size)");
        // Push block of the kernels.
        std::vector<size_t> pOffsets;
        std::vector<std::string> pNames;
        size_t pSize = 0;
        const std::string common = std::string(FUSE_RP_ATMOSPHERE_SHADER_DIR) + "/at_common." + lang;
        const bool pushParsed = parseShaderStruct(
            common, std::string(lang) == "slang" ? "struct AtPush {" : "uniform AtPushBlock {", pOffsets, pSize, pNames);
        const std::vector<std::string> pExpected = {"params", "src", "dst", "mode", "reserved"};
        const std::vector<size_t> pExpectedOffsets = {offsetof(AtPush, params), offsetof(AtPush, src),
                                                      offsetof(AtPush, dst), offsetof(AtPush, mode),
                                                      offsetof(AtPush, reserved)};
        expect(pushParsed && pNames == pExpected && pOffsets == pExpectedOffsets && pSize == sizeof(AtPush),
               "shader push block == AtPush");
    }
    for (const AtmosphereLutSizes& z : {AtmosphereLutSizes{}, AtmosphereLutSizes{7, 5, 3, 9, 11, 13, 5, 3, 17}}) {
        AtmosphereLutSettings s{};
        s.sizes = z;
        const AtParams p = params(s, AtmosphereLutView{});
        const AtmosphereBufferLayout l = AtmosphereBufferLayout::compute(p);
        const u64 at[5] = {l.transmittance, l.multiscatter, l.skyView, l.aerialScatter, l.aerialTransmittance};
        const u64 texels[5] = {static_cast<u64>(z.transWidth) * z.transHeight, static_cast<u64>(z.msWidth) * z.msHeight,
                               static_cast<u64>(z.skyWidth) * z.skyHeight,
                               static_cast<u64>(z.apWidth) * z.apHeight * z.apDepth,
                               static_cast<u64>(z.apWidth) * z.apHeight * z.apDepth};
        bool ok = true;
        for (u32 i = 0; i < 5u; ++i) {
            ok = ok && at[i] % 256u == 0u && l.bytes[i] == texels[i] * 16u && at[i] + l.bytes[i] <= l.totalBytes;
            for (u32 j = i + 1u; j < 5u; ++j) {
                ok = ok && (at[i] + l.bytes[i] <= at[j] || at[j] + l.bytes[j] <= at[i]);
            }
        }
        std::printf("layout: LUT buffer %.1f KiB\n", static_cast<f64>(l.totalBytes) / 1024.0);
        expect(ok, "LUT sections are 256-aligned, disjoint, sized W x H (x D) x 16 bytes");
    }
}

// --- luts ------------------------------------------------------------------------------------------------
void testLuts() {
    AtmosphereLutSettings s{};
    const AtParams p = params(s, sunView(20.0, 30.0));
    const BruteForceSingleScatter bf(s.atmosphere);

    // Transmittance texels vs the brute-force optical depth (Simpson, 20k intervals).
    std::vector<AtTexel> trans;
    build_transmittance_lut(p, trans);
    f64 worstOd = 0.0;
    for (u32 y = 0; y < p.transHeight; y += 3u) {
        for (u32 x = 0; x < p.transWidth; x += 5u) {
            f32 r = 0.f, mu = 0.f;
            at_transmittance_r_mu(p, static_cast<f32>(x) / static_cast<f32>(p.transWidth - 1u),
                                  static_cast<f32>(y) / static_cast<f32>(p.transHeight - 1u), r, mu);
            f64 ref[3];
            bf.transmittance(r, mu, 20000, ref);
            const AtTexel& t = trans[static_cast<usize>(y) * p.transWidth + x];
            const f64 got[3] = {t.r, t.g, t.b};
            for (int c = 0; c < 3; ++c) {
                const f64 odRef = -std::log(ref[c]);
                if (odRef > 1e-3 && ref[c] > 1e-30) {
                    worstOd = std::max(worstOd, std::fabs(-std::log(got[c]) / odRef - 1.0));
                }
            }
        }
    }
    std::printf("luts: transmittance texels vs brute-force optical depth: max rel err %.2e\n", worstOd);
    expectLe(worstOd, 5e-3, "transmittance LUT optical depth within 0.5% of the brute-force integral");

    // Parametrisation round trips.
    f64 worstRt = 0.0;
    for (f32 xR : {0.f, 0.1f, 0.5f, 0.9f, 1.f}) {
        for (f32 xMu : {0.f, 0.25f, 0.5f, 0.75f, 0.999f}) {
            f32 r = 0.f, mu = 0.f, u = 0.f, v = 0.f;
            at_transmittance_r_mu(p, xMu, xR, r, mu);
            at_transmittance_uv(p, r, mu, u, v);
            worstRt = std::max(worstRt, static_cast<f64>(std::fabs(u - xMu) + std::fabs(v - xR)));
        }
    }
    for (f32 v0 : {0.02f, 0.2f, 0.45f, 0.55f, 0.8f, 0.98f}) {
        for (f32 u0 : {0.f, 0.3f, 0.7f, 1.f}) {
            f32 mu = 0.f, lvc = 0.f, u = 0.f, v = 0.f;
            at_sky_view_dir(p, p.up[3], u0, v0, mu, lvc);
            at_sky_view_uv(p, p.up[3], mu, lvc, u, v);
            worstRt = std::max(worstRt, static_cast<f64>(std::fabs(u - u0) + std::fabs(v - v0)));
        }
    }
    std::printf("luts: parametrisation round trips: max |d uv| %.2e\n", worstRt);
    expectLe(worstRt, 2e-3, "LUT parametrisations invert");

    // Bilinear helper: exact at texel centres, linear in between.
    std::vector<AtTexel> table(5u * 4u);
    for (u32 i = 0; i < table.size(); ++i) {
        table[i] = AtTexel{static_cast<f32>(i % 5u) * 2.f + static_cast<f32>(i / 5u) * 7.f, static_cast<f32>(i), 1.f, 0.f};
    }
    bool exact = true;
    for (u32 y = 0; y < 4u; ++y) {
        for (u32 x = 0; x < 5u; ++x) {
            const AtTexel t = at_bilinear(table.data(), 5, 4, static_cast<f32>(x), static_cast<f32>(y));
            exact = exact && t.r == table[y * 5u + x].r && t.g == table[y * 5u + x].g;
        }
    }
    const AtTexel mid = at_bilinear(table.data(), 5, 4, 1.25f, 2.5f);
    exact = exact && std::fabs(mid.r - (1.25f * 2.f + 2.5f * 7.f)) < 1e-5f;
    const AtTexel clamped = at_bilinear(table.data(), 5, 4, -3.f, 9.f);
    exact = exact && clamped.r == table[15].r;
    expect(exact, "at_bilinear: exact at texel centres, linear in between, clamped outside");

    AtCpuLuts luts;
    luts.build(p);
    const AtLutView lv = luts.view();

    // Sky-view texels == the integrator on the texel's ray; LUT sampling at a texel direction returns it.
    f64 worstSky = 0.0;
    for (u32 y = 1; y < p.skyHeight; y += 13u) {
        for (u32 x = 0; x < p.skyWidth; x += 17u) {
            f32 mu = 0.f, lvc = 0.f;
            at_sky_view_dir(p, p.up[3], static_cast<f32>(x) / static_cast<f32>(p.skyWidth - 1u),
                            static_cast<f32>(y) / static_cast<f32>(p.skyHeight - 1u), mu, lvc);
            const f32 sinV = std::sqrt(std::max(0.f, 1.f - mu * mu));
            const f32 sunX = std::sqrt(std::max(0.f, 1.f - p.sunDir[3] * p.sunDir[3]));
            const Vec3 direct = at_integrate_sky(p, lv, p.up[3], mu, sinV * lvc * sunX + mu * p.sunDir[3], p.sunDir[3], p.skySteps);
            const AtTexel& t = luts.skyView[static_cast<usize>(y) * p.skyWidth + x];
            worstSky = std::max(worstSky, static_cast<f64>(std::fabs(t.b - direct.z) / std::max(1e-12f, direct.z)));
        }
    }
    std::printf("luts: sky-view texels vs the integrator on their ray: max rel err %.2e\n", worstSky);
    expectLe(worstSky, 1e-5, "sky-view texel == integrator");

    // Multi-scattering texels == Psi of their integrals.
    f64 worstMs = 0.0;
    for (u32 y = 0; y < p.msHeight; y += 7u) {
        for (u32 x = 0; x < p.msWidth; x += 5u) {
            const AtTexel t = multiscatter_texel(p, lv, x, y);
            const f32 muS = static_cast<f32>(x) / static_cast<f32>(p.msWidth - 1u) * 2.f - 1.f;
            const f32 top = p.topRadius - p.bottomRadius;
            const f32 h = std::clamp(static_cast<f32>(y) / static_cast<f32>(p.msHeight - 1u) * top, 1.f, top - 1.f);
            Vec3 l2{}, f{};
            multiscatter_integrals(p, lv, p.bottomRadius + h, muS, l2, f);
            const f64 psi = l2.z / (1.0 - f.z);
            worstMs = std::max(worstMs, std::fabs(t.b - psi) / std::max(1e-12, psi));
        }
    }
    std::printf("luts: multi-scattering texels vs Psi = L2 / (1 - f_ms): max rel err %.2e\n", worstMs);
    expectLe(worstMs, 1e-5, "multi-scattering texel == L2 / (1 - f_ms)");

    // Aerial perspective: slice z == the integrator to maxDist ((z + 1) / D)^2 (uniform steps, same count).
    f64 worstAp = 0.0;
    bool monotone = true;
    const usize stride = static_cast<usize>(p.apWidth) * p.apHeight;
    for (u32 y : {0u, 13u, 31u}) {
        for (u32 x : {0u, 16u, 31u}) {
            f32 prevT = 1.f;
            f32 prevS = 0.f;
            for (u32 z = 0; z < p.apDepth; ++z) {
                const AtTexel& t = luts.aerialTransmittance[z * stride + y * p.apWidth + x];
                const AtTexel& sc = luts.aerialScatter[z * stride + y * p.apWidth + x];
                monotone = monotone && t.b <= prevT && sc.b >= prevS && t.b > 0.f;
                prevT = t.b;
                prevS = sc.b;
            }
        }
    }
    // Direct check of one column against a fine uniform march of the same integrand (independent step count).
    {
        std::vector<AtTexel> col(static_cast<usize>(p.apDepth) * stride);
        std::vector<AtTexel> colT(col.size());
        aerial_column(p, lv, 16u, 16u, col.data(), colT.data());
        AtParams fine = p;
        fine.apStepsPerSlice = 64u;
        std::vector<AtTexel> ref(col.size());
        std::vector<AtTexel> refT(col.size());
        aerial_column(fine, lv, 16u, 16u, ref.data(), refT.data());
        for (u32 z = 0; z < p.apDepth; ++z) {
            worstAp = std::max(worstAp, static_cast<f64>(std::fabs(col[z * stride].b - ref[z * stride].b) /
                                                         std::max(1e-12f, ref[z * stride].b)));
            worstAp = std::max(worstAp, static_cast<f64>(std::fabs(colT[z * stride].b - refT[z * stride].b)));
        }
    }
    std::printf("luts: aerial perspective (2 steps / slice) vs 64 steps / slice: max rel err %.2e; monotone %s\n",
                worstAp, monotone ? "yes" : "no");
    expect(monotone, "aerial perspective: transmittance falls and in-scatter grows with distance");
    expectLe(worstAp, 2e-2, "aerial perspective converged in the step count (2%)");

    // at_aerial: exact at a froxel centre, continuous to (0, 1) at the camera.
    Vec3 s0{}, t0{};
    const f32 d0 = p.cameraPos[3] * (1.f / static_cast<f32>(p.apDepth)) * (1.f / static_cast<f32>(p.apDepth));
    at_aerial(p, lv, (16.f + 0.5f) / 32.f, (16.f + 0.5f) / 32.f, d0 * 4.f, s0, t0);
    const AtTexel& slice1 = luts.aerialScatter[1u * stride + 16u * p.apWidth + 16u];
    Vec3 sNear{}, tNear{};
    at_aerial(p, lv, 0.5f, 0.5f, 0.f, sNear, tNear);
    std::printf("luts: at_aerial at slice 1 centre %.4e vs texel %.4e; at the camera scatter %.1e transmittance %.6f\n",
                s0.z, slice1.b, sNear.z, tNear.z);
    expect(std::fabs(s0.z - slice1.b) <= 1e-5f * slice1.b && sNear.z == 0.f && tNear.z == 1.f,
           "at_aerial exact at froxel centres and (0, 1) at the camera");
}

// --- reference -------------------------------------------------------------------------------------------
void testReference() {
    AtmosphereLutSettings s{};
    s.multiScattering = false; // the brute-force reference is single scattering
    const BruteForceSingleScatter bf(s.atmosphere);
    f64 worst = 0.0;
    f64 worstLum = 0.0;
    u32 cases = 0;
    for (const f64 sunEl : {90.0, 65.0, 30.0, 10.0, 5.0, 2.0}) {
        const AtmosphereLutView v = sunView(sunEl, 0.0);
        const AtParams p = params(s, v);
        AtCpuLuts luts;
        build_transmittance_lut(p, luts.transmittance);
        build_sky_view_lut(p, luts.view(), luts.skyView);
        f64 worstHere = 0.0;
        for (const f64 el : {90.0, 60.0, 30.0, 15.0, 10.0, 5.0, 3.0}) {
            for (const f64 az : {0.0, 45.0, 90.0, 135.0, 180.0}) {
                const Vec3 d = dirElAz(el * kDeg, az * kDeg);
                const Vec3 got = at_sky_radiance(p, luts.view(), d, false);
                f64 ref[3];
                bf.inscatter(1.0, d.x, d.y, d.z, v.sunDirection.x, v.sunDirection.y, v.sunDirection.z, 1024, 400, ref);
                const f64 g[3] = {got.x, got.y, got.z};
                for (int c = 0; c < 3; ++c) {
                    worstHere = std::max(worstHere, std::fabs(g[c] / ref[c] - 1.0));
                }
                worstLum = std::max(worstLum, std::fabs((g[0] + g[1] + g[2]) / (ref[0] + ref[1] + ref[2]) - 1.0));
                ++cases;
            }
        }
        std::printf("reference: sun %4.1f deg: max per-channel rel err vs brute force %.4f\n", sunEl, worstHere);
        worst = std::max(worst, worstHere);
    }
    std::printf("reference: %u directions: sky-view LUT radiance vs brute-force single scatter: max per-channel %.4f,"
                " max radiance (rgb sum) %.4f\n",
                cases, worst, worstLum);
    expectLe(worst, 0.06, "sky radiance within +-6% of the brute-force single-scatter reference (every channel)");
}

// --- energy ----------------------------------------------------------------------------------------------
/// Downward sky irradiance at the camera (per unit TOA irradiance normal to the sun), sky-view LUT, midpoint
/// quadrature over the upper hemisphere.
f64 skyIrradiance(const AtParams& p, const AtLutView& lv) {
    constexpr u32 kTheta = 48;
    constexpr u32 kPhi = 96;
    f64 e = 0.0;
    for (u32 i = 0; i < kTheta; ++i) {
        const f64 theta = (static_cast<f64>(i) + 0.5) / kTheta * (kPiD * 0.5);
        for (u32 j = 0; j < kPhi; ++j) {
            const f64 phi = (static_cast<f64>(j) + 0.5) / kPhi * 2.0 * kPiD;
            const Vec3 d = dirElAz(kPiD * 0.5 - theta, phi);
            const Vec3 L = at_sky_view(p, lv, d);
            const f64 lum = (static_cast<f64>(L.x) + L.y + L.z) / 3.0;
            e += lum * std::cos(theta) * std::sin(theta) * (kPiD * 0.5 / kTheta) * (2.0 * kPiD / kPhi);
        }
    }
    return e;
}

void testEnergy() {
    AtmosphereLutSettings s{};
    // 1 + 2: f_ms and the series of scattering orders on every texel.
    {
        const AtParams p = params(s, sunView(30.0));
        AtCpuLuts luts;
        build_transmittance_lut(p, luts.transmittance);
        build_multiscatter_lut(p, luts.view(), luts.multiscatter);
        f32 fMin = 1.f, fMax = 0.f;
        bool finite = true;
        for (const AtTexel& t : luts.multiscatter) {
            fMin = std::min(fMin, t.a);
            fMax = std::max(fMax, t.a);
            finite = finite && std::isfinite(t.r) && std::isfinite(t.g) && std::isfinite(t.b) && t.r >= 0.f &&
                     t.g >= 0.f && t.b >= 0.f;
        }
        std::printf("energy: f_ms over the multi-scattering LUT in [%.4f, %.4f]\n", fMin, fMax);
        expect(finite, "Psi_ms finite and >= 0");
        expect(fMin >= 0.f && fMax < 1.f, "0 <= f_ms < 1 on every texel (each scattering order loses energy)");
        // Psi == sum over orders k >= 2 of L2 f^(k - 2) (explicit series) at a few texels.
        f64 worst = 0.0;
        for (const f32 muS : {-0.2f, 0.1f, 0.5f, 1.f}) {
            for (const f32 h : {1.f, 2000.f, 20000.f}) {
                Vec3 l2{}, f{};
                multiscatter_integrals(p, luts.view(), p.bottomRadius + h, muS, l2, f);
                f64 series = 0.0;
                f64 order = l2.z;
                for (u32 k = 0; k < 400u; ++k) {
                    series += order;
                    order *= f.z;
                }
                worst = std::max(worst, std::fabs(series - l2.z / (1.0 - f.z)) / std::max(1e-20, series));
            }
        }
        std::printf("energy: Psi_ms vs the explicit sum of scattering orders: max rel err %.2e\n", worst);
        expectLe(worst, 1e-6, "Psi_ms == L2 x sum f_ms^k");
    }
    // 3 + 4: ground irradiance budget, MS on vs off.
    for (const f64 sunEl : {90.0, 45.0, 20.0, 5.0}) {
        f64 e[2] = {0.0, 0.0};
        f64 direct = 0.0;
        f64 zenith[2] = {0.0, 0.0};
        for (u32 ms = 0; ms < 2u; ++ms) {
            AtmosphereLutSettings sm = s;
            sm.multiScattering = ms == 1u;
            const AtParams p = params(sm, sunView(sunEl));
            AtCpuLuts luts;
            build_transmittance_lut(p, luts.transmittance);
            build_multiscatter_lut(p, luts.view(), luts.multiscatter);
            build_sky_view_lut(p, luts.view(), luts.skyView);
            e[ms] = skyIrradiance(p, luts.view());
            const Vec3 z = at_sky_view(p, luts.view(), Vec3{0.f, 1.f, 0.f});
            zenith[ms] = (static_cast<f64>(z.x) + z.y + z.z) / 3.0;
            const Vec3 t = at_sun_transmittance(p, luts.view(), p.up[3], p.sunDir[3]);
            direct = (static_cast<f64>(t.x) + t.y + t.z) / 3.0 * p.sunDir[3];
        }
        const f64 toa = std::sin(sunEl * kDeg);
        const f64 total = e[1] + direct;
        std::printf("energy: sun %4.1f deg: TOA %.4f, direct %.4f, sky single %.4f, sky with MS %.4f (+%.1f%%), "
                    "ground total / TOA %.3f; zenith MS share %.1f%%\n",
                    sunEl, toa, direct, e[0], e[1], 100.0 * (e[1] / e[0] - 1.0), total / toa,
                    100.0 * (zenith[1] - zenith[0]) / zenith[1]);
        expect(e[1] > e[0], "multi-scattering adds sky irradiance");
        expectLe(total / toa, 1.0, "ground irradiance (sky + direct) <= top-of-atmosphere flux");
        expect(total / toa > 0.5, "ground irradiance above half the TOA flux (clear sky)");
        const f64 share = (zenith[1] - zenith[0]) / zenith[1];
        expect(share > 0.01 && share < 0.6, "multi-scattering share of the zenith radiance within [1%, 60%]");
    }
}

// --- sweep ---------------------------------------------------------------------------------------------
/// Time-of-day stability of a radiance series sampled at equal sun-elevation steps: the largest step in
/// log(L + eps) (how fast the sky may change: twilight legitimately dims ~50% per 0.5 degree) and the largest
/// second difference (a pop / discontinuity shows as a spike there). eps = 1e-3 x `peak` (the brightest sky of
/// the sweep: deep twilight below that is dark next to the day and at the level of the shadow-edge sampling).
struct Stability {
    f64 maxStep = 0.0;
    f64 maxCurvature = 0.0;
};

Stability stability(const std::vector<f64>& series, f64 peak) {
    Stability st{};
    const f64 eps = 1e-3 * peak;
    for (usize i = 1; i < series.size(); ++i) {
        const f64 d1 = std::log(series[i] + eps) - std::log(series[i - 1] + eps);
        st.maxStep = std::max(st.maxStep, std::fabs(d1));
        if (i + 1u < series.size()) {
            const f64 d2 = std::log(series[i + 1] + eps) - 2.0 * std::log(series[i] + eps) + std::log(series[i - 1] + eps);
            st.maxCurvature = std::max(st.maxCurvature, std::fabs(d2));
        }
    }
    return st;
}

void testSweep() {
    AtmosphereLutSettings s{};
    AtParams p0 = params(s, sunView(30.0));
    AtCpuLuts luts;
    build_transmittance_lut(p0, luts.transmittance);
    build_multiscatter_lut(p0, luts.view(), luts.multiscatter);
    const AtLutView lv = luts.view();
    const Vec3 dirs[] = {dirElAz(90.0 * kDeg, 0.0), dirElAz(30.0 * kDeg, 0.0), dirElAz(30.0 * kDeg, kPiD),
                         dirElAz(5.0 * kDeg, 0.5 * kPiD), dirElAz(2.0 * kDeg, 0.0), dirElAz(-5.0 * kDeg, 0.0)};
    constexpr u32 kDirs = sizeof(dirs) / sizeof(dirs[0]);
    std::vector<f64> series[kDirs];
    bool finite = true;
    bool monotone = true;
    for (f64 el = -10.0; el <= 90.0001; el += 0.5) {
        const AtParams p = params(s, sunView(el, 20.0));
        for (u32 i = 0; i < kDirs; ++i) {
            const Vec3 d = dirs[i];
            const Vec3 sun{p.sunDir[0], p.sunDir[1], p.sunDir[2]};
            const Vec3 L = at_integrate_sky(p, lv, p.up[3], d.y, d.dot(sun), p.sunDir[3], p.skySteps);
            const f64 lum = (static_cast<f64>(L.x) + L.y + L.z) / 3.0;
            finite = finite && std::isfinite(lum) && L.x >= 0.f && L.y >= 0.f && L.z >= 0.f;
            if (i == 0u && !series[0].empty() && lum + 1e-12 < series[0].back()) {
                monotone = false;
            }
            series[i].push_back(lum);
        }
    }
    Stability worst{};
    f64 peak = 0.0;
    for (const std::vector<f64>& v : series) {
        for (const f64 x : v) {
            peak = std::max(peak, x);
        }
    }
    for (const std::vector<f64>& v : series) {
        const Stability st = stability(v, peak);
        worst.maxStep = std::max(worst.maxStep, st.maxStep);
        worst.maxCurvature = std::max(worst.maxCurvature, st.maxCurvature);
    }
    std::printf("sweep: %zu sun elevations (-10 .. 90 deg, 0.5 deg steps) x %u directions: finite %s, zenith monotone %s,"
                " max |d log L| %.3f, max |d2 log L| %.3f per step\n",
                series[0].size(), kDirs, finite ? "yes" : "no", monotone ? "yes" : "no", worst.maxStep, worst.maxCurvature);
    expect(finite, "sweep: sky finite and >= 0");
    expect(monotone, "sweep: zenith radiance non-decreasing with sun elevation");
    expectLe(worst.maxStep, 1.0, "sweep: sky changes by less than a factor e per 0.5 degree");
    expectLe(worst.maxCurvature, 0.3, "sweep: no pops (second difference of log radiance)");
}

// --- legacy ----------------------------------------------------------------------------------------------
void testLegacy() {
    using fuse::renderer::SkyLut;
    using fuse::renderer::SkyLutDesc;
    using fuse::renderer::TransmittanceLut;
    using fuse::renderer::TransmittanceLutDesc;
    TransmittanceLutDesc td{};
    td.altitude_bins = 17;
    td.cos_zenith_bins = 33;
    TransmittanceLut tl;
    expect(tl.build(td), "legacy transmittance LUT builds");
    f64 worstCentre = 0.0;
    f64 worstMid = 0.0;
    f64 worstNearest = 0.0;
    f64 sumMid = 0.0;
    f64 sumNearest = 0.0;
    for (u32 a = 0; a + 1u < td.altitude_bins; a += 3u) {
        for (u32 c = 20; c + 1u < td.cos_zenith_bins; c += 2u) { // mu >= 0.25: away from the horizon kink
            const f32 alt = fuse::renderer::transmittance_lut_altitude_for_bin(a, td);
            const f32 mu = fuse::renderer::transmittance_lut_cos_zenith_for_bin(c, td);
            const Vec3 centre = tl.sampleBilinear(alt, mu);
            const Vec3 exact = tl.entries()[fuse::renderer::transmittance_lut_flat_index(a, c, td.cos_zenith_bins)];
            worstCentre = std::max(worstCentre, static_cast<f64>(std::fabs(centre.z - exact.z)));
            const f32 alt2 = 0.5f * (alt + fuse::renderer::transmittance_lut_altitude_for_bin(a + 1u, td));
            const f32 mu2 = 0.5f * (mu + fuse::renderer::transmittance_lut_cos_zenith_for_bin(c + 1u, td));
            const Vec3 truth = fuse::renderer::compute_transmittance(alt2, mu2, td.atmosphere);
            const f64 eMid = std::fabs(tl.sampleBilinear(alt2, mu2).z - truth.z);
            const f64 eNearest = std::fabs(tl.sample(alt2, mu2).z - truth.z);
            worstMid = std::max(worstMid, eMid);
            worstNearest = std::max(worstNearest, eNearest);
            sumMid += eMid;
            sumNearest += eNearest;
        }
    }
    std::printf("legacy: TransmittanceLut bilinear: centre err %.1e, mid-cell err %.4f (nearest %.4f)\n", worstCentre,
                worstMid, worstNearest);
    expect(worstCentre < 1e-6, "legacy transmittance bilinear exact at bin centres");
    expect(worstMid < worstNearest && sumMid < 0.25 * sumNearest, "legacy transmittance bilinear beats nearest between bins");

    SkyLutDesc sd{};
    sd.sun_elevation_bins = 19;
    sd.view_elevation_bins = 19;
    SkyLut sl;
    expect(sl.build(sd, Vec3{0.f, 0.3f, 1.f}), "legacy sky LUT builds");
    const f32 bin = static_cast<f32>(kPiD / 18.0);
    const Vec3 c0 = sl.sampleBilinear(-0.5f * static_cast<f32>(kPiD) + 12.f * bin, -0.5f * static_cast<f32>(kPiD) + 14.f * bin);
    const Vec3 e0 = sl.entries()[12u * 19u + 14u];
    const Vec3 a = sl.entries()[12u * 19u + 14u];
    const Vec3 b = sl.entries()[12u * 19u + 15u];
    const Vec3 m = sl.sampleBilinear(-0.5f * static_cast<f32>(kPiD) + 12.f * bin, -0.5f * static_cast<f32>(kPiD) + 14.5f * bin);
    expect(std::fabs(c0.z - e0.z) <= 1e-6f * e0.z && std::fabs(m.z - 0.5f * (a.z + b.z)) <= 1e-4f * a.z,
           "legacy sky LUT bilinear: exact at bins, midpoint average between them");
}

// --- api -------------------------------------------------------------------------------------------------
void testApi() {
    AtmosphereLutSettings s{};
    AtParams p{};
    expect(resolve_at_params(s, AtmosphereLutView{}, p), "defaults resolve");
    expect(p.transWidth == 256u && p.transHeight == 64u && p.msWidth == 32u && p.skyWidth == 192u &&
               p.skyHeight == 108u && p.apDepth == 32u,
           "Hillaire 2020 default LUT sizes");
    expect(std::fabs(p.up[3] - (p.bottomRadius + 1.f)) < 1.f && p.up[1] == 1.f, "camera radius / local up");
    expect(p.flags == static_cast<u32>(kAtFlagMultiScatter), "multi-scattering on by default");
    AtmosphereLutSettings bad = s;
    bad.sizes.skyWidth = 1;
    expect(!resolve_at_params(bad, AtmosphereLutView{}, p), "a 1-texel LUT edge is rejected");
    bad = s;
    bad.sampling.msSteps = 0;
    expect(!resolve_at_params(bad, AtmosphereLutView{}, p), "zero samples rejected");
    bad = s;
    bad.atmosphere.atmo_radius = bad.atmosphere.earth_radius;
    expect(!resolve_at_params(bad, AtmosphereLutView{}, p), "empty shell rejected");
    AtmosphereLutView badView{};
    badView.sunDirection = Vec3{0.f, 0.f, 0.f};
    expect(!resolve_at_params(s, badView, p), "zero sun direction rejected");
    // Camera far above the atmosphere clamps into the shell; the LUT addresses survive a resolve.
    AtmosphereLutView high{};
    high.cameraPosition = Vec3{0.f, 5.0e6f, 0.f};
    p.transmittance = 0x1000u;
    expect(resolve_at_params(s, high, p) && p.transmittance == 0x1000u && p.up[3] <= p.topRadius - 1.f + 1.f,
           "camera clamped into the shell, LUT addresses kept");
    // Static change detection: the sun / camera do not rebuild, the medium does.
    AtParams a{}, b{};
    resolve_at_params(s, sunView(10.0), a);
    resolve_at_params(s, sunView(70.0, 45.0), b);
    expect(at_static_equal(a, b), "sun / camera changes keep the static LUTs");
    AtmosphereLutSettings other = s;
    other.groundAlbedo = Vec3{0.5f, 0.5f, 0.5f};
    resolve_at_params(other, sunView(10.0), b);
    expect(!at_static_equal(a, b), "a medium / ground change rebuilds them");
    // Stub / no device.
    expect(!queryAtmosphereCapabilities(nullptr).atmosphere, "no device: not capable");
    AtmosphereGpu gpu;
    expect(!gpu.init(AtmosphereGpuDesc{}) && !gpu.valid(), "init without a device fails cleanly");
    expect(!gpu.beginFrame(1, s, AtmosphereLutView{}), "beginFrame before init fails");
    // Legacy sky model agreement: the new integrator (MS off) and compute_sky_inscatter describe one atmosphere.
    AtmosphereLutSettings ss = s;
    ss.multiScattering = false;
    const AtParams q = params(ss, sunView(45.0));
    AtCpuLuts luts;
    build_transmittance_lut(q, luts.transmittance);
    const Vec3 d = dirElAz(40.0 * kDeg, 1.0);
    const Vec3 sun{q.sunDir[0], q.sunDir[1], q.sunDir[2]};
    const Vec3 mine = at_integrate_sky(q, luts.view(), q.up[3], d.y, d.dot(sun), q.sunDir[3], 64u);
    const Vec3 legacy = fuse::renderer::compute_sky_inscatter(Vec3{0.f, 1.f, 0.f}, d, sun, ss.atmosphere);
    std::printf("api: integrator vs the B5 compute_sky_inscatter at el 40: %.4e vs %.4e (blue)\n", mine.z, legacy.z);
    expect(std::fabs(mine.z / legacy.z - 1.f) < 0.08f, "same atmosphere as the B5 CPU sky (within its 8% gate)");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "luts") {
        testLuts();
    }
    if (all || suite == "reference") {
        testReference();
    }
    if (all || suite == "energy") {
        testEnergy();
    }
    if (all || suite == "sweep") {
        testSweep();
    }
    if (all || suite == "legacy") {
        testLegacy();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
