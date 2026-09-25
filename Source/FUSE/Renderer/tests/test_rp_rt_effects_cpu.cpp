// WP-6.2 ray-traced shadows and reflections: CPU gates (stub-safe). Lavapipe gates: test_rp_rt_effects.cpp.
//
//   layout     record sizes / offsets; the GLSL and Slang twins declare every record's fields in the C++ order
//              (rtfx_common, the light loop's RtfxShadowView copy in lc_ltc, the lighting constants' rtShadows)
//   sequence   Owen-scrambled Sobol: bit reversal, exact 24-bit floats, and the first 2^m points of every pixel
//              sequence (index shuffled, continued across frames) form a (0, m, 2)-net for m <= 10
//   sampling   rectangle / disk / sun-cone / GGX-VNDF samplers: support, uniformity moments, VNDF histogram ==
//              the numerically integrated D_v density; position reconstruction == f64; ray-origin offset
//   reference  the offline path tracer on an analytic scene: rectangle-light penumbra == the exact visible
//              fraction (parallel occluder: the blocked region is a scaled copy of the occluder), hard shadows,
//              occluder distance, mirror / rough reflections of the sky == sky x P(above horizon) (numeric),
//              mirror hit shading == the closed form
//   api        packShadowLight / packHitLight for every kind (LTC encodings decoded), buildFrameConstants
//              (inverse, masks never include kRtMaskDead), the light loop's CPU RT hook (lighting_gpu::shadowed),
//              RtEffects::init refuses without a T2 device
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/renderer/lighting/gpu/clustered_gpu_types.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/renderer/rt_effects/rt_effects.hpp>
#include <fuse/renderer/rt_effects/rt_effects_kernel.hpp>
#include <fuse/renderer/rt_effects/rt_effects_reference.hpp>
#include <fuse/renderer/rt_effects/rt_effects_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::rt_effects;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using V3 = RtfxVec3<f64>;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The text of `struct <name> {...}` in `src` (empty when absent).
std::string structBody(const std::string& src, const std::string& name) {
    const usize at = src.find("struct " + name + " {");
    if (at == std::string::npos) {
        return {};
    }
    const usize end = src.find("};", at);
    return src.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

/// Every field name appears in order in `body`.
bool fieldsInOrder(const std::string& body, const std::vector<std::string>& fields) {
    usize cursor = 0;
    for (const std::string& f : fields) {
        bool found = false;
        while (true) {
            const usize at = body.find(f, cursor);
            if (at == std::string::npos) {
                break;
            }
            const char before = at > 0 ? body[at - 1] : ' ';
            const char after = at + f.size() < body.size() ? body[at + f.size()] : ' ';
            if ((before == ' ' || before == '\t') && (after == ';' || after == '[')) {
                cursor = at + f.size();
                found = true;
                break;
            }
            cursor = at + 1;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// --- layout -----------------------------------------------------------------------------------------------
void testLayout() {
    expect(sizeof(RtfxShadowLight) == 64u && sizeof(RtfxHitLight) == 32u && sizeof(RtfxShadowView) == 48u &&
               sizeof(RtfxFrameConstants) == 576u && sizeof(RtfxPush) == 16u && sizeof(RtfxReflectionTexel) == 16u &&
               sizeof(RtfxRayRecord) == 48u,
           "record sizes");
    expect(offsetof(RtfxFrameConstants, view) == 0u, "the shadow view heads the frame constants (the lighting reads it there)");
    expect(sizeof(lighting_gpu::LightingFrameConstants) == 768u && offsetof(lighting_gpu::LightingFrameConstants, rtShadowsLo) == 752u,
           "lighting constants carry rtShadowsLo / Hi at 752");
    const std::vector<std::string> light = {"position", "kind", "axisX", "slot", "axisY", "cosCone", "direction", "samples"};
    const std::vector<std::string> hit = {"direction", "shadowed", "irradiance", "slot"};
    const std::vector<std::string> view = {"visibility", "width", "height", "count", "pad0", "slots", "pad1"};
    const std::vector<std::string> frame = {"view",           "tlas",          "hitDistance",       "reflection",   "atmosphere",
                                            "invViewProj",    "cameraPosition", "frameIndex",       "invWidth",     "invHeight",
                                            "scene",          "flags",         "gbufferNormal",     "gbufferRoughMetal",
                                            "gbufferDepth",   "reflectionSamples", "normalBias",    "viewBias",     "farDistance",
                                            "mirrorRoughness", "ambient",      "hitLightCount",     "sky",          "shadowCullMask",
                                            "reflectionCullMask", "seed",      "pad0",              "pad1",         "shadowLights",
                                            "hitLights"};
    const std::vector<std::string> record = {"origin", "tMin", "direction", "tMax", "t", "instance", "primitive", "flags"};
    const std::string dir = FUSE_RT_EFFECTS_SHADER_DIR;
    const std::string glsl = readFile(dir + "/rtfx_common.glsl");
    const std::string slang = readFile(dir + "/rtfx_common.slang");
    expect(!glsl.empty() && !slang.empty(), "shader sources readable");
    const struct {
        const char* g;
        const char* s;
        const std::vector<std::string>* fields;
    } checks[] = {{"FuseRtfxShadowLight", "RtfxShadowLight", &light}, {"FuseRtfxHitLight", "RtfxHitLight", &hit},
                  {"FuseRtfxShadowView", "RtfxShadowView", &view},    {"FuseRtfxFrame", "RtfxFrame", &frame},
                  {"FuseRtfxRayRecord", "RtfxRayRecord", &record}};
    for (const auto& c : checks) {
        const bool ok = fieldsInOrder(structBody(glsl, c.g), *c.fields) && fieldsInOrder(structBody(slang, c.s), *c.fields);
        if (!ok) {
            std::fprintf(stderr, "  struct %s / %s\n", c.g, c.s);
        }
        expect(ok, "GLSL / Slang records declare the C++ fields in order");
    }
    const std::string lc = FUSE_LIGHTING_SHADER_DIR;
    const std::string ltcGlsl = readFile(lc + "/lc_ltc.glsl");
    const std::string ltcSlang = readFile(lc + "/lc_ltc.slang");
    expect(fieldsInOrder(structBody(ltcGlsl, "FuseLcRtView"), view) && fieldsInOrder(structBody(ltcSlang, "LcRtView"), view),
           "the light loop's RtfxShadowView copies match");
    const std::string lcGlsl = readFile(lc + "/lc_common.glsl");
    const std::string lcSlang = readFile(lc + "/lc_common.slang");
    const std::vector<std::string> tail = {"ndcY", "rtShadowsLo", "rtShadowsHi", "ddgiLo", "ddgiHi"};
    expect(fieldsInOrder(structBody(lcGlsl, "FuseLcFrame"), tail) && fieldsInOrder(structBody(lcSlang, "LcFrame"), tail),
           "the lighting frame constants end with rtShadowsLo / Hi, ddgiLo / Hi in both shader languages");
    expect(sizeof(lighting_gpu::LightingFrameConstants) == 768u && offsetof(lighting_gpu::LightingFrameConstants, rtShadowsLo) == 752u &&
               offsetof(lighting_gpu::LightingFrameConstants, ddgiLo) == 760u && offsetof(lighting_gpu::LightingFrameConstants, ddgiHi) == 764u,
           "LightingFrameConstants: 768 bytes, rtShadowsLo / Hi at 752, ddgiLo / Hi at 760");
    for (const u32 w : {1u, 64u, 257u, 1920u}) {
        const RtEffectsOutputLayout l = RtEffectsOutputLayout::compute(w, 3u);
        const u64 px = static_cast<u64>(w) * 3u;
        expect(l.visibility == 0u && l.hitDistance >= px * 16u && l.reflection >= l.hitDistance + px * 16u &&
                   l.bytes >= l.reflection + px * 16u && l.hitDistance % 256u == 0u && l.reflection % 256u == 0u,
               "output sections disjoint and 256-aligned");
    }
    expect(kRtfxFlagHitShadows == 1u && kRtfxFlagAtmosphereSky == 2u &&
               glsl.find("#define FUSE_RTFX_FLAG_ATMOSPHERE_SKY 2u") != std::string::npos &&
               slang.find("kFuseRtfxFlagAtmosphereSky = 2u") != std::string::npos,
           "kRtfxFlagAtmosphereSky == FUSE_RTFX_FLAG_ATMOSPHERE_SKY == kFuseRtfxFlagAtmosphereSky");
    std::printf("layout: records pinned, GLSL / Slang / light-loop twins in C++ field order\n");
}

// --- sequence -----------------------------------------------------------------------------------------------
u32 naiveReverse(u32 x) {
    u32 r = 0;
    for (u32 i = 0; i < 32u; ++i) {
        r |= ((x >> i) & 1u) << (31u - i);
    }
    return r;
}

void testSequence() {
    std::mt19937 rng(62);
    u32 reverseBad = 0;
    for (u32 i = 0; i < 20000u; ++i) {
        const u32 x = rng();
        reverseBad += rtfxReverseBits(x) != naiveReverse(x) ? 1u : 0u;
    }
    expect(reverseBad == 0u, "bit reversal");
    expect(rtfxUnit(0xFFFFFFFFu) < 1.f && rtfxUnit(0u) == 0.f && rtfxUnit(0x80000000u) == 0.5f, "24-bit unit floats in [0, 1)");
    // (0, m, 2)-net: every elementary box of area 2^-m holds exactly one of the first 2^m points.
    u32 nets = 0, netBad = 0;
    for (u32 s = 0; s < 24u; ++s) {
        const u32 seed = rtfxPixelSeed(s * 7u, s * 13u + 1u, s % 5u, 0x5EEDu + s);
        for (u32 m = 1; m <= 10u; ++m) {
            const u32 n = 1u << m;
            std::vector<u32> xs(n), ys(n);
            for (u32 i = 0; i < n; ++i) {
                rtfxSample2(i, seed, xs[i], ys[i]);
            }
            for (u32 a = 0; a <= m; ++a) {
                const u32 b = m - a;
                std::vector<u32> count(n, 0u);
                for (u32 i = 0; i < n; ++i) {
                    const u32 bx = a == 0u ? 0u : xs[i] >> (32u - a);
                    const u32 by = b == 0u ? 0u : ys[i] >> (32u - b);
                    ++count[(bx << b) | by];
                }
                bool ok = true;
                for (const u32 c : count) {
                    ok = ok && c == 1u;
                }
                netBad += ok ? 0u : 1u;
                ++nets;
            }
        }
    }
    std::printf("sequence: %u (seed, m, box shape) nets checked, %u not (0, m, 2)-nets\n", nets, netBad);
    expect(netBad == 0u, "the first 2^m points of every pixel sequence form a (0, m, 2)-net");
    // Frames continue the sequence: 64 frames x 4 samples == the first 256 points (a net), and the
    // per-pixel means of many decorrelated pixels are unbiased.
    f64 mean = 0.0;
    u32 count = 0;
    for (u32 p = 0; p < 256u; ++p) {
        const u32 seed = rtfxPixelSeed(p % 16u, p / 16u, 0u, 1u);
        u32 x = 0, y = 0;
        rtfxSample2(0u, seed, x, y);
        mean += rtfxUnit(x) + rtfxUnit(y);
        count += 2u;
    }
    mean /= count;
    std::printf("  first samples of 256 pixels: mean %.4f (0.5 expected)\n", mean);
    expect(std::fabs(mean - 0.5) < 0.05, "pixel seeds decorrelate the first samples");
}

// --- sampling --------------------------------------------------------------------------------------------------
void testSampling() {
    const u32 n = 4096;
    const u32 seed = rtfxPixelSeed(3, 5, 0, 9);
    // Rectangle: support + uniform moments (E[s] = 0, E[s^2] = 1/3 in the axis coordinates).
    RtfxShadowLight rect{};
    rect.kind = kRtfxLightRect;
    rect.position[0] = 1.f;
    rect.position[1] = 5.f;
    rect.axisX[0] = 1.5f;
    rect.axisY[2] = 0.5f;
    const V3 origin{0.0, 0.0, 0.0};
    f64 m1x = 0, m2x = 0, m1y = 0, m2y = 0;
    u32 outside = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 ux = 0, uy = 0;
        rtfxSample2(i, seed, ux, uy);
        V3 d{};
        f64 tMax = 0;
        rtfxShadowRay<f64>(rect, origin, rtfxUnit(ux), rtfxUnit(uy), 100.0, d, tMax);
        const V3 p = rtfxScale(d, tMax / 0.9999);
        const f64 sx = (p.x - 1.0) / 1.5, sy = p.z / 0.5;
        outside += (std::fabs(sx) > 1.0 + 1e-9 || std::fabs(sy) > 1.0 + 1e-9 || std::fabs(p.y - 5.0) > 1e-9) ? 1u : 0u;
        m1x += sx;
        m2x += sx * sx;
        m1y += sy;
        m2y += sy * sy;
    }
    m1x /= n;
    m2x /= n;
    m1y /= n;
    m2y /= n;
    std::printf("sampling: rectangle E[s] (%.4f, %.4f), E[s^2] (%.4f, %.4f) (0, 1/3), %u outside\n", m1x, m1y, m2x, m2y, outside);
    expect(outside == 0u && std::fabs(m1x) < 0.01 && std::fabs(m1y) < 0.01 && std::fabs(m2x - 1.0 / 3.0) < 0.01 &&
               std::fabs(m2y - 1.0 / 3.0) < 0.01,
           "rectangle samples uniform by area on the light");
    // Disk: fraction inside radius r == r^2, E[r^2] = 1/2.
    u32 inside = 0, beyond = 0;
    f64 r2 = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 ux = 0, uy = 0;
        rtfxSample2(i, seed, ux, uy);
        f64 dx = 0, dy = 0;
        rtfxConcentric<f64>(rtfxUnit(ux), rtfxUnit(uy), dx, dy);
        const f64 rr = dx * dx + dy * dy;
        inside += rr < 0.25 ? 1u : 0u;
        beyond += rr > 1.0 + 1e-12 ? 1u : 0u;
        r2 += rr;
    }
    std::printf("  disk: P(r < 0.5) %.4f (0.25), E[r^2] %.4f (0.5), %u outside\n", inside / static_cast<f64>(n), r2 / n, beyond);
    expect(beyond == 0u && std::fabs(inside / static_cast<f64>(n) - 0.25) < 0.01 && std::fabs(r2 / n - 0.5) < 0.01,
           "concentric disk samples uniform by area");
    // Sun cone: inside the cone, uniform in solid angle (E[cos] = (1 + cosCone) / 2).
    RtfxShadowLight sun{};
    sun.kind = kRtfxLightSun;
    const f32 axis[3] = {0.3f, 0.9f, -0.2f};
    const f32 al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    for (u32 c = 0; c < 3u; ++c) {
        sun.direction[c] = axis[c] / al;
    }
    sun.cosCone = std::cos(0.1f);
    f64 meanCos = 0;
    u32 coneOut = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 ux = 0, uy = 0;
        rtfxSample2(i, seed, ux, uy);
        V3 d{};
        f64 tMax = 0;
        rtfxShadowRay<f64>(sun, origin, rtfxUnit(ux), rtfxUnit(uy), 100.0, d, tMax);
        const f64 c = rtfxDot(d, rtfxLoad<f64>(sun.direction));
        coneOut += c < static_cast<f64>(sun.cosCone) - 1e-12 ? 1u : 0u;
        meanCos += c;
    }
    meanCos /= n;
    const f64 expectCos = (1.0 + static_cast<f64>(sun.cosCone)) * 0.5;
    std::printf("  sun cone: E[cos] %.8f (%.8f), %u outside the cone\n", meanCos, expectCos, coneOut);
    expect(coneOut == 0u && std::fabs(meanCos - expectCos) < 1e-5, "sun samples uniform in the cone");
    // GGX VNDF: histogram of cos(theta_m) == the numerically integrated density.
    f64 worst = 0.0;
    for (const f64 alpha : {0.1, 0.3, 0.6, 1.0}) {
        for (const f64 theta : {0.0, 0.7, 1.3}) {
            const V3 wi{std::sin(theta), 0.0, std::cos(theta)};
            constexpr u32 kBins = 16;
            f64 hist[kBins] = {};
            const u32 samples = 1u << 16;
            for (u32 i = 0; i < samples; ++i) {
                u32 ux = 0, uy = 0;
                rtfxSample2(i, seed ^ 0x1234u, ux, uy);
                const V3 m = rtfxSampleVndf<f64>(wi, alpha, rtfxUnit(ux), rtfxUnit(uy));
                const u32 b = std::min<u32>(static_cast<u32>(std::fmax(m.z, 0.0) * kBins), kBins - 1u);
                hist[b] += 1.0 / samples;
            }
            // Numeric: integrate D_v over (cos theta, phi) cells.
            f64 num[kBins] = {};
            const u32 nz = 400, nphi = 256;
            f64 total = 0.0;
            for (u32 iz = 0; iz < nz * kBins; ++iz) {
                const f64 cz = (iz + 0.5) / (nz * kBins);
                const f64 sz = std::sqrt(std::fmax(0.0, 1.0 - cz * cz));
                for (u32 ip = 0; ip < nphi; ++ip) {
                    const f64 phi = (ip + 0.5) / nphi * 2.0 * kRtfxPi;
                    const V3 m{sz * std::cos(phi), sz * std::sin(phi), cz};
                    const f64 w = rtfxVndfPdf<f64>(wi, m, alpha) * (1.0 / (nz * kBins)) * (2.0 * kRtfxPi / nphi);
                    num[iz / nz] += w;
                    total += w;
                }
            }
            for (u32 b = 0; b < kBins; ++b) {
                worst = std::max(worst, std::fabs(hist[b] - num[b] / total));
            }
            expect(std::fabs(total - 1.0) < 2e-3, "D_v integrates to 1");
        }
    }
    std::printf("  GGX VNDF: max |histogram - integrated D_v| per cos-theta bin %.2e (12 lobes)\n", worst);
    expect(worst < 4e-3, "VNDF samples follow D_v");
    // Mirror: a perfect reflection below mirrorRoughness.
    {
        const V3 nrm = rtfxNormalize(V3{0.1, 1.0, -0.2}, V3{0, 1, 0});
        const V3 view = rtfxNormalize(V3{0.5, 0.6, 0.3}, V3{0, 1, 0});
        V3 d{};
        const bool ok = rtfxReflectionRay<f64>(nrm, view, 0.01, 0.03, 0.3, 0.7, d);
        const V3 r = rtfxSub(rtfxScale(nrm, 2.0 * rtfxDot(view, nrm)), view);
        expect(ok && rtfxLength(rtfxSub(d, r)) < 1e-12, "mirror reflection below mirrorRoughness");
    }
    // Reconstruction: f32 kernel vs f64 through a Vulkan-convention projection; the origin offset.
    {
        const f32 f = 1.f / std::tan(0.5f);
        f32 proj[16] = {};
        proj[0] = f / 1.333f;
        proj[5] = -f;
        proj[10] = 80.f / (0.2f - 80.f);
        proj[11] = -1.f;
        proj[14] = 0.2f * 80.f / (0.2f - 80.f);
        RtEffectsFrameDesc d{};
        std::memcpy(d.viewProj, proj, sizeof(proj));
        RtfxFrameConstants c{};
        expect(buildFrameConstants(d, 128, 96, c), "buildFrameConstants");
        std::mt19937 rng(7);
        std::uniform_real_distribution<f32> u(0.f, 1.f);
        f64 worstRel = 0.0;
        for (u32 i = 0; i < 2000u; ++i) {
            const u32 px = static_cast<u32>(u(rng) * 127.f), py = static_cast<u32>(u(rng) * 95.f);
            const f64 zView = -(0.5 + 60.0 * u(rng));
            const f64 clipZ = static_cast<f64>(proj[10]) * zView + proj[14];
            const f32 depth = static_cast<f32>(clipZ / -zView);
            const RtfxVec3<f32> p32 = rtfxReconstruct<f32>(c.invViewProj, px, py, c.invWidth, c.invHeight, depth);
            const V3 p64 = rtfxReconstruct<f64>(c.invViewProj, px, py, c.invWidth, c.invHeight, depth);
            const f64 err = rtfxLength(rtfxSub(V3{p32.x, p32.y, p32.z}, p64)) / rtfxLength(p64);
            worstRel = std::max(worstRel, err);
            expect(std::fabs(p64.z - zView) < 1e-4 * -zView + 1e-3, "reconstructed view depth");
        }
        std::printf("  reconstruction: max |f32 - f64| / |P| %.2e over 2000 pixels\n", worstRel);
        expect(worstRel < 4e-5, "f32 reconstruction == f64 (relative 4e-5: far z/w cancellation in f32)");
        const V3 o = rtfxRayOrigin<f64>(V3{0, 0, -10}, V3{0, 1, 0}, V3{0, 0, 0}, 0.01, 1e-3);
        expect(std::fabs(o.y - 0.02) < 1e-12 && o.x == 0.0 && o.z == -10.0, "ray origin = P + N (normalBias + viewBias |P - camera|)");
    }
}

// --- reference ------------------------------------------------------------------------------------------------
struct CpuScene {
    gpu_scene::GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    rt::RtReferenceScene bvh;
    std::vector<Material::GPUMaterial> materials;
};

gpu_scene::GpuTransform boxXf(f64 x0, f64 y0, f64 z0, f64 x1, f64 y1, f64 z1) {
    gpu_scene::GpuTransform t{};
    t.rows[0][0] = static_cast<f32>((x1 - x0) * 0.5);
    t.rows[1][1] = static_cast<f32>((y1 - y0) * 0.5);
    t.rows[2][2] = static_cast<f32>((z1 - z0) * 0.5);
    t.rows[0][3] = static_cast<f32>((x1 + x0) * 0.5);
    t.rows[1][3] = static_cast<f32>((y1 + y0) * 0.5);
    t.rows[2][3] = static_cast<f32>((z1 + z0) * 0.5);
    return t;
}

bool buildCpuScene(CpuScene& s, bool occluder) {
    gpu_scene::GpuSceneDesc d{};
    if (!s.gpu.init(d)) {
        return false;
    }
    s.gpu.beginFrame(1);
    s.meshes.resize(2);
    if (!mr_test::build(mr_test::plane(4, 40.f, 1.f), s.meshes[0]) || !mr_test::build(mr_test::box(), s.meshes[1])) {
        return false;
    }
    for (u32 i = 0; i < 2u; ++i) {
        if (s.gpu.addMeshletMesh(s.meshes[i]) != i || !s.bvh.setMeshFromScene(s.gpu, i, s.meshes[i].positions.data())) {
            return false;
        }
    }
    Material::GPUMaterial ground{};
    ground.baseColor = {0.5f, 0.5f, 0.5f, 0.f};
    Material::GPUMaterial red{};
    red.baseColor = {0.8f, 0.2f, 0.1f, 0.25f};
    red.roughnessEmissive = {0.5f, 0.1f, 0.2f, 0.3f};
    red.emissiveIntensity = 2.f;
    s.materials = {ground, red, red};
    for (u32 i = 0; i < s.materials.size(); ++i) {
        s.gpu.setMaterial(i, s.materials[i]);
    }
    gpu_scene::InstanceDesc g{};
    g.mesh = 0;
    g.material = 0;
    s.gpu.addInstance(g);
    if (occluder) {
        // Thin square occluder [-1, 1] x [-1, 1] at y = 2 (thickness 2e-3).
        gpu_scene::InstanceDesc b{};
        b.mesh = 1;
        b.material = 1;
        b.transform = boxXf(-1.0, 1.999, -1.0, 1.0, 2.001, 1.0);
        s.gpu.addInstance(b);
    } else {
        // A box wall facing -z far behind the mirror target (for the hit-shading check).
        gpu_scene::InstanceDesc b{};
        b.mesh = 1;
        b.material = 1;
        b.transform = boxXf(-5.0, 0.0, -21.0, 5.0, 6.0, -20.0);
        s.gpu.addInstance(b);
    }
    s.gpu.commit();
    s.bvh.setInstances(s.gpu);
    return true;
}

/// Overlap length of [a0, a1] and [b0, b1].
f64 overlap(f64 a0, f64 a1, f64 b0, f64 b1) { return std::max(0.0, std::min(a1, b1) - std::max(a0, b0)); }

void testReference() {
    CpuScene s;
    if (!buildCpuScene(s, true)) {
        expect(false, "CPU scene");
        return;
    }
    const RtEffectsReference ref(s.bvh, s.gpu, s.materials.data(), static_cast<u32>(s.materials.size()));
    RtfxFrameConstants c{};
    c.farDistance = 1000.f;
    c.shadowCullMask = rt::kRtMaskShadow;
    c.reflectionCullMask = rt::kRtMaskVisible;
    c.mirrorRoughness = 0.03f;
    c.normalBias = 0.01f; // hit points' shadow-ray origins
    // Rectangle light at y = 6 facing down, half extents 1.5 x 1.0.
    RtfxShadowLight rect{};
    rect.kind = kRtfxLightRect;
    rect.position[1] = 6.f;
    rect.axisX[0] = 1.5f;
    rect.axisY[2] = 1.0f;
    rect.direction[1] = -1.f;
    rect.samples = 1u;
    f64 worstZ = 0.0, worstAbs = 0.0;
    u32 penumbra = 0;
    for (u32 i = 0; i < 24u; ++i) {
        RtfxSurface surf{};
        surf.valid = true;
        surf.origin = V3{-2.6 + 0.23 * i, 1e-4, 0.35 - 0.02 * i};
        surf.normal = V3{0, 1, 0};
        const RtfxEstimate e = ref.shadow(c, rect, surf, 48u, 1000u + i);
        // Exact: blocked light points = occluder square scaled by k = (6 - py) / (2 - py) about the receiver.
        const f64 py = surf.origin.y;
        const f64 k = (6.0 - py) / (2.0 - py);
        const f64 bx0 = surf.origin.x + (-1.0 - surf.origin.x) * k, bx1 = surf.origin.x + (1.0 - surf.origin.x) * k;
        const f64 bz0 = surf.origin.z + (-1.0 - surf.origin.z) * k, bz1 = surf.origin.z + (1.0 - surf.origin.z) * k;
        const f64 exact = 1.0 - overlap(-1.5, 1.5, bx0, bx1) * overlap(-1.0, 1.0, bz0, bz1) / (3.0 * 2.0);
        // Thickness: the 2e-3 slab widens the blocked region by ~1e-3 k-scaled: a few 1e-3 of visibility.
        const f64 sigma = std::sqrt(exact * (1.0 - exact) / e.samples) + 4e-3;
        worstZ = std::max(worstZ, std::fabs(e.mean[0] - exact) / sigma);
        worstAbs = std::max(worstAbs, std::fabs(e.mean[0] - exact));
        penumbra += exact > 0.02 && exact < 0.98 ? 1u : 0u;
        if (e.hits > 0u) {
            // Occluders are hit at the slab: t = (2 - py) / cos to the sampled point, within the light's spread.
            expect(e.hitDistance > 1.9 && e.hitDistance < 3.2, "mean occluder distance lies on the occluder");
        }
    }
    std::printf("reference: rectangle penumbra (%u partial of 24 receivers): max |ref - exact| %.4f, max z %.2f\n", penumbra, worstAbs,
                worstZ);
    expect(penumbra >= 8u && worstZ < 4.0, "rectangle-light visibility == the exact visible fraction");
    // Hard shadows: directional light straight down.
    RtfxShadowLight dirLight{};
    dirLight.kind = kRtfxLightDirectional;
    dirLight.direction[1] = 1.f;
    u32 hardBad = 0;
    for (u32 i = 0; i < 40u; ++i) {
        RtfxSurface surf{};
        surf.valid = true;
        surf.origin = V3{-2.0 + 0.1 * i + 0.013, 1e-4, 0.5};
        const RtfxEstimate e = ref.shadow(c, dirLight, surf, 8u, 7u);
        const bool blocked = std::fabs(surf.origin.x) < 1.0;
        hardBad += (e.samples != 1u || e.mean[0] != (blocked ? 0.0 : 1.0) ||
                    (blocked && std::fabs(e.hitDistance - (1.999 - 1e-4)) > 1e-5))
                       ? 1u
                       : 0u;
    }
    expect(hardBad == 0u, "directional hard shadows and occluder distance exact");
    // Point light: one ray, tMax short of the light.
    {
        RtfxShadowLight point{};
        point.kind = kRtfxLightPoint;
        point.position[1] = 1.f; // below the occluder: nothing blocks a receiver under it
        RtfxSurface surf{};
        surf.valid = true;
        surf.origin = V3{0.2, 1e-4, 0.1};
        expect(ref.shadow(c, point, surf, 8u, 1u).mean[0] == 1.0, "point light below the occluder: lit");
        point.position[1] = 5.f;
        expect(ref.shadow(c, point, surf, 8u, 1u).mean[0] == 0.0, "point light above the occluder: shadowed");
    }
    // Reflections of an empty sky: radiance = sky x P(reflected direction above the horizon).
    CpuScene open;
    if (!buildCpuScene(open, false)) {
        expect(false, "CPU scene (open)");
        return;
    }
    const RtEffectsReference ref2(open.bvh, open.gpu, open.materials.data(), static_cast<u32>(open.materials.size()));
    c.sky[0] = 1.f;
    c.sky[1] = 2.f;
    c.sky[2] = 3.f;
    f64 worstSky = 0.0;
    for (const f64 roughness : {0.0, 0.2, 0.5, 0.9}) {
        RtfxSurface surf{};
        surf.valid = true;
        surf.position = V3{0, 50, 10}; // above the wall: directions above the horizon see only sky
        surf.origin = V3{0, 50.001, 10};
        surf.normal = V3{0, 1, 0};
        surf.view = rtfxNormalize(V3{0.0, 0.5, -1.0}, V3{0, 1, 0}); // reflections leave toward +z, away from the wall
        surf.roughness = roughness;
        const RtfxEstimate e = ref2.reflection(c, surf, 64u, 3u);
        // P(above horizon) by integrating D_v(m) [reflect(wi, m).z > 0] numerically.
        f64 p = 1.0;
        if (roughness >= 0.03) {
            const f64 alpha = roughness * roughness;
            V3 t{}, b{};
            rtfxBasis(surf.normal, t, b);
            const V3 wi = rtfxNormalize(V3{rtfxDot(surf.view, t), rtfxDot(surf.view, b), rtfxDot(surf.view, surf.normal)}, V3{0, 0, 1});
            f64 above = 0.0, total = 0.0;
            const u32 nz = 2000, nphi = 256;
            for (u32 iz = 0; iz < nz; ++iz) {
                const f64 cz = (iz + 0.5) / nz;
                const f64 sz = std::sqrt(1.0 - cz * cz);
                for (u32 ip = 0; ip < nphi; ++ip) {
                    const f64 phi = (ip + 0.5) / nphi * 2.0 * kRtfxPi;
                    const V3 m{sz * std::cos(phi), sz * std::sin(phi), cz};
                    const f64 w = rtfxVndfPdf<f64>(wi, m, alpha);
                    total += w;
                    above += (2.0 * rtfxDot(wi, m) * m.z - wi.z) > 0.0 ? w : 0.0;
                }
            }
            p = above / total;
        }
        for (u32 ch = 0; ch < 3u; ++ch) {
            const f64 expected = c.sky[ch] * p;
            const f64 sigma = std::sqrt(e.variance[ch] / e.samples) + 1e-3 * c.sky[ch];
            worstSky = std::max(worstSky, std::fabs(e.mean[ch] - expected) / sigma);
        }
        expect(e.hits == 0u && e.hitDistance == static_cast<f64>(kRtfxNoHit), "sky reflections hit nothing");
    }
    std::printf("  rough sky reflections: max z vs sky x P(above horizon) %.2f\n", worstSky);
    expect(worstSky < 4.0, "reflection estimator == the integrated lobe");
    // kRtfxFlagAtmosphereSky: misses take the sky-radiance hook (the kernels' at_sky_radiance); without the flag
    // (or without a hook) the constant sky.
    {
        RtEffectsReference ref3(open.bvh, open.gpu, open.materials.data(), static_cast<u32>(open.materials.size()));
        ref3.setSkyRadiance([](const void*, const V3& d) { return V3{d.x + 2.0, d.y + 3.0, d.z + 4.0}; }, nullptr);
        RtfxSurface surf{};
        surf.valid = true;
        surf.position = V3{0, 50, 10};
        surf.origin = V3{0, 50.001, 10};
        surf.normal = V3{0, 1, 0};
        surf.view = rtfxNormalize(V3{0.0, 0.5, -1.0}, V3{0, 1, 0});
        surf.roughness = 0.0; // mirror: one ray along reflect(-view, n)
        const V3 r = rtfxNormalize(V3{-surf.view.x, surf.view.y, -surf.view.z}, V3{0, 1, 0});
        const u32 flags = c.flags;
        c.flags = flags | kRtfxFlagAtmosphereSky;
        const RtfxEstimate a = ref3.reflection(c, surf, 1u, 1u);
        c.flags = flags;
        const RtfxEstimate b = ref3.reflection(c, surf, 1u, 1u);
        const f64 errA = std::max({std::fabs(a.mean[0] - (r.x + 2.0)), std::fabs(a.mean[1] - (r.y + 3.0)), std::fabs(a.mean[2] - (r.z + 4.0))});
        const f64 errB = std::max({std::fabs(b.mean[0] - c.sky[0]), std::fabs(b.mean[1] - c.sky[1]), std::fabs(b.mean[2] - c.sky[2])});
        std::printf("  atmosphere-sky misses: hook error %.2e, flag off == constant sky error %.2e\n", errA, errB);
        expect(a.hits == 0u && errA < 1e-9, "kRtfxFlagAtmosphereSky: a miss returns the sky-radiance hook along the ray");
        expect(errB == 0.0, "without kRtfxFlagAtmosphereSky a miss returns the constant sky");
    }
    // Mirror hit shading: a mirror at the origin looking at the wall (z = -20 face, normal +z).
    {
        c.ambient[0] = c.ambient[1] = c.ambient[2] = 0.1f;
        c.hitLightCount = 1;
        c.hitLights[0].direction[1] = 0.f;
        c.hitLights[0].direction[2] = 1.f; // toward +z: lights the wall's front face head-on
        c.hitLights[0].irradiance[0] = c.hitLights[0].irradiance[1] = c.hitLights[0].irradiance[2] = 3.f;
        c.hitLights[0].shadowed = 1u;
        c.flags = kRtfxFlagHitShadows;
        RtfxSurface surf{};
        surf.valid = true;
        surf.position = V3{0, 1, 0};
        surf.origin = V3{0, 1, -0.001};
        surf.normal = V3{0, 0, -1}; // a vertical mirror facing -z
        surf.view = V3{0, 0, -1};   // viewed head-on: reflects straight to -z
        surf.roughness = 0.0;
        const RtfxEstimate e = ref2.reflection(c, surf, 8u, 1u);
        const Material::GPUMaterial& m = open.materials[1];
        const f64 irr = kRtfxPi * 0.1 + 3.0;
        const f64 diffuse = (1.0 - 0.25) / kRtfxPi;
        const f64 expected[3] = {0.1 * 2.0 + 0.8 * diffuse * irr, 0.2 * 2.0 + 0.2 * diffuse * irr, 0.3 * 2.0 + 0.1 * diffuse * irr};
        f64 err = 0.0;
        for (u32 ch = 0; ch < 3u; ++ch) {
            err = std::max(err, std::fabs(e.mean[ch] - expected[ch]));
        }
        (void)m;
        std::printf("  mirror: ref (%.5f %.5f %.5f) closed form (%.5f %.5f %.5f)\n", e.mean[0], e.mean[1], e.mean[2], expected[0], expected[1],
                    expected[2]);
        std::printf("  mirror hit shading: |ref - closed form| %.2e, hit distance %.6f (19.999 expected)\n", err, e.hitDistance);
        expect(e.samples == 1u && e.hits == 1u && err < 1e-5 && std::fabs(e.hitDistance - 19.999) < 1e-4,
               "mirror hit shading == emission + Lambert closed form");
    }
    s.gpu.destroy();
    open.gpu.destroy();
}

// --- api ------------------------------------------------------------------------------------------------------
void testApi() {
    // Rectangle / disk: the LTC encodings decode back to the centre, half axes and normal.
    ltc::AreaLightDesc a{};
    a.center = {1.f, 4.f, -3.f};
    a.normal = {0.f, -1.f, 0.f};
    a.tangent = {1.f, 0.f, 0.3f};
    a.halfWidth = 1.2f;
    a.halfHeight = 0.6f;
    RtfxShadowLight r{};
    expect(packShadowLight(3, ltc::makeRectLight(a), 8, r), "pack rectangle");
    const f64 lx = std::sqrt(static_cast<f64>(r.axisX[0]) * r.axisX[0] + static_cast<f64>(r.axisX[1]) * r.axisX[1] +
                             static_cast<f64>(r.axisX[2]) * r.axisX[2]);
    const f64 ly = std::sqrt(static_cast<f64>(r.axisY[0]) * r.axisY[0] + static_cast<f64>(r.axisY[1]) * r.axisY[1] +
                             static_cast<f64>(r.axisY[2]) * r.axisY[2]);
    const f64 cross[3] = {static_cast<f64>(r.axisX[1]) * r.axisY[2] - static_cast<f64>(r.axisX[2]) * r.axisY[1],
                          static_cast<f64>(r.axisX[2]) * r.axisY[0] - static_cast<f64>(r.axisX[0]) * r.axisY[2],
                          static_cast<f64>(r.axisX[0]) * r.axisY[1] - static_cast<f64>(r.axisX[1]) * r.axisY[0]};
    expect(r.kind == kRtfxLightRect && r.slot == 3u && r.samples == 8u && r.position[1] == 4.f && std::fabs(lx - 1.2) < 1e-4 &&
               std::fabs(ly - 0.6) < 1e-4 && cross[1] < 0.0 && std::fabs(r.direction[1] + 1.f) < 1e-6,
           "rectangle: centre, half axes (ex x ey = lit-side normal)");
    RtfxShadowLight d{};
    expect(packShadowLight(4, ltc::makeDiskLight(a), 100, d) && d.kind == kRtfxLightDisk && d.samples == kRtfxMaxSamples,
           "pack disk (samples clamped)");
    gpu_scene::GpuLight sun{};
    sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    sun.direction[0] = 0.f;
    sun.direction[1] = -2.f;
    sun.direction[2] = 0.f;
    RtfxShadowLight hard{};
    expect(packShadowLight(0, sun, 4, hard) && hard.kind == kRtfxLightDirectional && hard.samples == 1u && hard.direction[1] == 1.f,
           "punctual sun: hard, toward the light");
    ltc::setSunAngularRadius(sun, 0.05f);
    RtfxShadowLight soft{};
    expect(packShadowLight(0, sun, 4, soft) && soft.kind == kRtfxLightSun && soft.samples == 4u && soft.cosCone < 1.f,
           "sun disk: soft cone");
    gpu_scene::GpuLight spot{};
    spot.type = static_cast<u32>(gpu_scene::GpuLightType::Spot);
    spot.position[0] = 2.f;
    RtfxShadowLight sp{};
    expect(packShadowLight(2, spot, 4, sp) && sp.kind == kRtfxLightPoint && sp.samples == 1u && sp.position[0] == 2.f, "spot: hard");
    gpu_scene::GpuLight none{};
    RtfxShadowLight nl{};
    expect(!packShadowLight(9, none, 1, nl), "free slot rejected");
    RtfxHitLight hl{};
    sun.color[0] = 0.5f;
    sun.intensity = 4.f;
    expect(packHitLight(0, sun, true, hl) && hl.irradiance[0] == 2.f && hl.direction[1] == 1.f && hl.shadowed == 1u, "hit light");
    expect(!packHitLight(2, spot, true, hl), "hit lights are directional only");
    // Frame constants: inverse, masks, the view.
    RtEffectsFrameDesc desc{};
    const f32 vp[16] = {1.2f, 0.1f, 0.f, 0.f, 0.f, -1.6f, 0.2f, 0.f, 0.3f, 0.f, -1.001f, -1.f, 0.5f, -0.2f, 3.f, 4.f};
    std::memcpy(desc.viewProj, vp, sizeof(vp));
    desc.shadowCullMask = 0xFFu;
    desc.reflectionCullMask = 0xFFu;
    desc.shadowLights[0] = {7u, ltc::makeRectLight(a), 4u};
    desc.shadowLights[1] = {8u, none, 1u}; // dropped
    desc.shadowLights[2] = {9u, spot, 1u};
    desc.shadowLightCount = 3;
    RtfxFrameConstants c{};
    expect(buildFrameConstants(desc, 64, 32, c), "buildFrameConstants");
    f64 worst = 0.0;
    for (u32 col = 0; col < 4u; ++col) {
        for (u32 row = 0; row < 4u; ++row) {
            f64 sum = 0.0;
            for (u32 k = 0; k < 4u; ++k) {
                sum += static_cast<f64>(c.invViewProj[k * 4 + row]) * vp[col * 4 + k];
            }
            worst = std::max(worst, std::fabs(sum - (row == col ? 1.0 : 0.0)));
        }
    }
    expect(worst < 1e-5, "invViewProj x viewProj == I");
    expect(c.shadowCullMask == rt::kRtMaskAll && c.reflectionCullMask == rt::kRtMaskAll && (c.shadowCullMask & rt::kRtMaskDead) == 0u,
           "cull masks never include the dead-slot bit");
    expect(c.view.count == 2u && c.view.slots[0] == 7u && c.view.slots[1] == 9u && c.view.width == 64u && c.view.height == 32u,
           "view: the packed lights in order, free slots dropped");
    // The light loop's CPU RT hook: covered slots take the pixel's visibility, others the VSM hook.
    lighting_gpu::ShadeParams p{};
    struct Rt {
        static f32 fn(const void*, u32 slot, u32 px, u32) { return slot == 1u ? 0.25f * static_cast<f32>(px) : -1.f; }
    };
    struct Vsm {
        static f32 fn(const void*, u32, const lighting_gpu::SurfaceSample&) { return 0.5f; }
    };
    p.rt_shadow = &Rt::fn;
    p.shadow = &Vsm::fn;
    const lighting_gpu::SurfaceSample s{};
    const fuse::math::Vec3 col{2.f, 4.f, 8.f};
    const fuse::math::Vec3 a1 = lighting_gpu::shadowed(p, 1u, s, col, 2u, 0u);
    const fuse::math::Vec3 a2 = lighting_gpu::shadowed(p, 0u, s, col, 2u, 0u);
    const fuse::math::Vec3 a3 = lighting_gpu::shadowed(p, 1u, s, col); // no pixel (forward pass): VSM
    expect(a1.x == 1.f && a1.z == 4.f && a2.x == 1.f && a2.y == 2.f && a3.x == 1.f, "light loop: RT visibility first, VSM otherwise");
    // Init refuses without a T2 device.
    RtEffects fx;
    RtEffectsDesc ed{};
    expect(!fx.init(ed) && !fx.valid() && std::strcmp(fx.reason(), "ok") != 0, "init without a device fails with a reason");
    std::printf("api: pack / frame constants / RT hook / init refusal ok (%s)\n", fx.reason());
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "layout";
    if (suite == "layout") {
        testLayout();
    } else if (suite == "sequence") {
        testSequence();
    } else if (suite == "sampling") {
        testSampling();
    } else if (suite == "reference") {
        testReference();
    } else if (suite == "api") {
        testApi();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
