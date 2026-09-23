// B5.12 DDGI gate rows — proven on the CPU reference probe trace/blend/sample path
// (the host implementation of the device kernels) against independent references:
// an independent ray tracer + Monte Carlo integrator, analytic form factors, and
// closed-form hysteresis response. GPU timing (< 2 ms per update cycle) is hardware-only.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/material/material.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::usize;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

constexpr f32 kPi = 3.14159265358979323846f;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLess(f64 value, f64 bound, const char* message) {
    if (!(value < bound)) {
        std::fprintf(stderr, "FAIL: %s (got %g, bound %g)\n", message, value, bound);
        ++g_failures;
    }
}

void expectGreater(f64 value, f64 bound, const char* message) {
    if (!(value > bound)) {
        std::fprintf(stderr, "FAIL: %s (got %g, bound %g)\n", message, value, bound);
        ++g_failures;
    }
}

Vec3 mul(const Vec3& a, const Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

f32 luminance(const Vec3& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

bool finiteNonNegative(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.x >= 0.f && v.y >= 0.f &&
           v.z >= 0.f;
}

// ---------------------------------------------------------------------------------------------
// Independent reference renderer (written separately from DdgiCpuScene / DdgiCpuVolume).
// ---------------------------------------------------------------------------------------------

struct RefBox {
    Vec3 lo;
    Vec3 hi;
    Vec3 albedo;
    Vec3 emissive;
};

struct RefScene {
    std::vector<RefBox> boxes;
    Vec3 sun_dir{0.f, 1.f, 0.f};
    Vec3 sun_irradiance{};
    Vec3 sky{};
};

struct RefHit {
    f32 t = std::numeric_limits<f32>::infinity();
    Vec3 normal{};
    bool back = false;
    int box = -1;
};

f32 comp(const Vec3& v, int i) {
    const f32 c[3] = {v.x, v.y, v.z};
    return c[i];
}

RefHit refTrace(const RefScene& scene, const Vec3& o, const Vec3& d, f32 t_max) {
    RefHit best{};
    best.t = t_max;
    for (int b = 0; b < static_cast<int>(scene.boxes.size()); ++b) {
        const RefBox& box = scene.boxes[b];
        // Test all six face planes; keep the nearest plane crossing inside the face rectangle.
        for (int ax = 0; ax < 3; ++ax) {
            const f32 dd = comp(d, ax);
            if (std::fabs(dd) < 1e-9f) {
                continue;
            }
            for (int side = 0; side < 2; ++side) {
                const f32 plane = side == 0 ? comp(box.lo, ax) : comp(box.hi, ax);
                const f32 t = (plane - comp(o, ax)) / dd;
                if (t <= 0.f || t >= best.t) {
                    continue;
                }
                const Vec3 p = o + d * t;
                bool inside = true;
                for (int k = 0; k < 3; ++k) {
                    if (k == ax) {
                        continue;
                    }
                    const f32 pk = comp(p, k);
                    if (pk < comp(box.lo, k) || pk > comp(box.hi, k)) {
                        inside = false;
                    }
                }
                if (!inside) {
                    continue;
                }
                Vec3 n{};
                const f32 outward = side == 0 ? -1.f : 1.f;
                if (ax == 0) {
                    n.x = outward;
                } else if (ax == 1) {
                    n.y = outward;
                } else {
                    n.z = outward;
                }
                best.t = t;
                best.normal = n;
                best.back = n.dot(d) > 0.f;
                best.box = b;
            }
        }
    }
    return best;
}

/// Radiance arriving at `o` from direction `d`: emission + sun-lit Lambertian (one bounce of
/// light at the hit), sky on escape.
Vec3 refRadiance(const RefScene& scene, const Vec3& o, const Vec3& d, f32 t_max = 1e30f) {
    const RefHit hit = refTrace(scene, o, d, t_max);
    if (hit.box < 0) {
        return scene.sky;
    }
    if (hit.back) {
        return {};
    }
    const RefBox& box = scene.boxes[hit.box];
    Vec3 l = box.emissive;
    const f32 c = hit.normal.dot(scene.sun_dir);
    if (c > 0.f) {
        const Vec3 p = o + d * hit.t + hit.normal * 1e-4f;
        if (refTrace(scene, p, scene.sun_dir, 1e30f).box < 0) {
            l = l + mul(box.albedo, scene.sun_irradiance) * (c / kPi);
        }
    }
    return l;
}

void orthonormalBasis(const Vec3& n, Vec3& t, Vec3& b) {
    const Vec3 helper = std::fabs(n.x) > 0.9f ? Vec3{0.f, 1.f, 0.f} : Vec3{1.f, 0.f, 0.f};
    t = fuse::math::cross(helper, n).normalized();
    b = fuse::math::cross(n, t);
}

/// Monte Carlo irradiance E = integral L cos over the hemisphere (cosine-weighted sampling).
Vec3 refIrradianceMC(const RefScene& scene, const Vec3& x, const Vec3& n, u32 samples, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> uni(0.f, 1.f);
    Vec3 t{};
    Vec3 b{};
    orthonormalBasis(n, t, b);
    Vec3 sum{};
    for (u32 i = 0; i < samples; ++i) {
        const f32 u1 = uni(rng);
        const f32 u2 = uni(rng);
        const f32 r = std::sqrt(u1);
        const f32 phi = 2.f * kPi * u2;
        const f32 lx = r * std::cos(phi);
        const f32 lz = r * std::sin(phi);
        const f32 ly = std::sqrt(std::max(0.f, 1.f - u1));
        const Vec3 d = (t * lx + n * ly + b * lz).normalized();
        sum = sum + refRadiance(scene, x + n * 1e-4f, d);
    }
    // pdf = cos / pi, so E = pi * mean(L).
    return sum * (kPi / static_cast<f32>(samples));
}

DdgiCpuScene toDdgiScene(const RefScene& ref) {
    DdgiCpuScene scene{};
    for (const RefBox& box : ref.boxes) {
        DdgiCpuSurface surface{};
        surface.albedo = box.albedo;
        surface.emissive = box.emissive;
        scene.addBox(box.lo, box.hi, surface);
    }
    scene.sun_direction = ref.sun_dir;
    scene.sun_irradiance = ref.sun_irradiance;
    scene.sky_radiance = ref.sky;
    return scene;
}

/// Continuous octahedral plane -> direction with the standard edge folding, used to derive
/// where a border texel "really" points (independent of copyOctahedralBorder).
Vec3 foldedDirection(f32 ox, f32 oy) {
    for (int i = 0; i < 4; ++i) {
        if (ox < -1.f) {
            ox = -2.f - ox;
            oy = -oy;
        } else if (ox > 1.f) {
            ox = 2.f - ox;
            oy = -oy;
        }
        if (oy < -1.f) {
            oy = -2.f - oy;
            ox = -ox;
        } else if (oy > 1.f) {
            oy = 2.f - oy;
            ox = -ox;
        }
    }
    return DdgiIrradianceEncoding::decodeDirection({ox * 0.5f + 0.5f, oy * 0.5f + 0.5f});
}

/// Check every border texel of a bordered tile equals the interior texel pointing the same way.
template <typename Getter>
bool borderMatchesFold(u32 res, Getter get, f32 tolerance) {
    bool ok = true;
    const u32 last = res + 1u;
    for (u32 y = 0; y <= last; ++y) {
        for (u32 x = 0; x <= last; ++x) {
            if (x != 0u && y != 0u && x != last && y != last) {
                continue;
            }
            const f32 ox = ((static_cast<f32>(x) - 1.f + 0.5f) / static_cast<f32>(res)) * 2.f - 1.f;
            const f32 oy = ((static_cast<f32>(y) - 1.f + 0.5f) / static_cast<f32>(res)) * 2.f - 1.f;
            const Vec3 dir = foldedDirection(ox, oy);
            u32 best_x = 0u;
            u32 best_y = 0u;
            f32 best_dot = -2.f;
            for (u32 iy = 0; iy < res; ++iy) {
                for (u32 ix = 0; ix < res; ++ix) {
                    const f32 d = dir.dot(ddgi_cpu::texelDirection(ix, iy, res));
                    if (d > best_dot) {
                        best_dot = d;
                        best_x = ix;
                        best_y = iy;
                    }
                }
            }
            const Vec3 a = get(x, y);
            const Vec3 b = get(best_x + 1u, best_y + 1u);
            if ((a - b).length() > tolerance || best_dot < 0.9999f) {
                ok = false;
            }
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------------------------

const Vec3 kWhite{0.8f, 0.8f, 0.8f};
const Vec3 kRed{0.8f, 0.1f, 0.1f};
const Vec3 kGreen{0.1f, 0.8f, 0.1f};

/// Large room around the default 16x8x16 grid (origin 1, spacing 2): floor, three walls,
/// two blockers, open top and front. Probe (11, 1, 11) sits inside the first blocker.
RefScene bigRoom(f32 sun_scale) {
    RefScene s{};
    s.boxes.push_back({{-1.f, -0.5f, -1.f}, {33.f, 0.f, 33.f}, kWhite, {}});
    s.boxes.push_back({{-0.5f, 0.f, -1.f}, {0.f, 16.f, 33.f}, kRed, {}});
    s.boxes.push_back({{32.f, 0.f, -1.f}, {32.5f, 16.f, 33.f}, kGreen, {}});
    s.boxes.push_back({{-1.f, 0.f, -0.5f}, {33.f, 16.f, 0.f}, kWhite, {}});
    s.boxes.push_back({{10.f, 0.f, 10.f}, {14.f, 8.f, 14.f}, kWhite, {}});
    s.boxes.push_back({{20.f, 0.f, 18.f}, {24.f, 5.f, 26.f}, {0.1f, 0.2f, 0.8f}, {}});
    s.sun_dir = Vec3{0.3f, 1.f, 0.5f}.normalized();
    s.sun_irradiance = Vec3{1.f, 0.95f, 0.9f} * sun_scale;
    s.sky = {0.05f, 0.06f, 0.08f};
    return s;
}

/// Cornell-like box: interior [0,4]^3, white floor + back wall, red left, green right,
/// open top and front, slanted sun + dim sky.
RefScene cornellBox() {
    RefScene s{};
    s.boxes.push_back({{-0.2f, -0.2f, -0.2f}, {4.2f, 0.f, 4.f}, kWhite, {}});
    s.boxes.push_back({{-0.2f, 0.f, -0.2f}, {4.2f, 4.f, 0.f}, kWhite, {}});
    s.boxes.push_back({{-0.2f, 0.f, 0.f}, {0.f, 4.f, 4.f}, kRed, {}});
    s.boxes.push_back({{4.f, 0.f, 0.f}, {4.2f, 4.f, 4.f}, kGreen, {}});
    s.sun_dir = Vec3{-0.5f, 1.f, 0.35f}.normalized();
    s.sun_irradiance = {3.f, 3.f, 3.f};
    s.sky = {0.1f, 0.1f, 0.1f};
    return s;
}

DDGIDesc cornellDesc() {
    DDGIDesc desc{};
    desc.grid_origin = {0.25f, 0.25f, 0.25f};
    desc.probe_spacing = {0.5f, 0.5f, 0.5f};
    desc.grid_dims = {8, 8, 8};
    desc.probes_per_frame = 512;
    return desc;
}

std::vector<u32> allProbes(u32 count) {
    std::vector<u32> v(count);
    for (u32 i = 0; i < count; ++i) {
        v[i] = i;
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------------------------

void testOctahedralRoundTrip() {
    std::mt19937 rng(7u);
    std::normal_distribution<f32> g(0.f, 1.f);
    f32 worst = 0.f;
    u32 lower = 0u;
    for (u32 i = 0; i < 20000u; ++i) {
        const Vec3 d = Vec3{g(rng), g(rng), g(rng)}.normalized();
        lower += d.z < 0.f ? 1u : 0u;
        const Vec3 back = DdgiIrradianceEncoding::decodeDirection(DdgiIrradianceEncoding::encodeDirection(d));
        worst = std::max(worst, DdgiIrradianceEncoding::angularErrorRadians(d, back));
    }
    std::printf("MEASURE: octahedral round-trip worst error %.2e rad over 20000 dirs (%u in -z)\n", worst, lower);
    expectLess(worst, 1e-3, "octahedral encode/decode round-trips in both hemispheres");
}

void testRayGeneration() {
    constexpr u32 kRays = 256u;
    Vec3 mean{};
    bool unit = true;
    f32 cos_sum_err = 0.f;
    for (u32 i = 0; i < kRays; ++i) {
        const Vec3 d = ddgi_cpu::sphericalFibonacci(i, kRays);
        unit = unit && std::fabs(d.length() - 1.f) < 1e-4f;
        mean = mean + d;
    }
    mean = mean * (1.f / kRays);
    // Quadrature check: mean clamped cosine against random normals is 1/4 for a uniform set.
    std::mt19937 rng(3u);
    std::normal_distribution<f32> g(0.f, 1.f);
    for (u32 k = 0; k < 64u; ++k) {
        const Vec3 n = Vec3{g(rng), g(rng), g(rng)}.normalized();
        f32 s = 0.f;
        for (u32 i = 0; i < kRays; ++i) {
            s += std::max(0.f, n.dot(ddgi_cpu::sphericalFibonacci(i, kRays)));
        }
        cos_sum_err = std::max(cos_sum_err, std::fabs(s / kRays - 0.25f) / 0.25f);
    }
    std::printf("MEASURE: spherical Fibonacci |mean| %.2e, worst clamped-cos quadrature error %.2f%%\n",
                mean.length(), cos_sum_err * 100.f);
    expectTrue(unit, "spherical Fibonacci directions are unit length");
    expectLess(mean.length(), 1e-2, "spherical Fibonacci set is balanced");
    expectLess(cos_sum_err, 0.02, "spherical Fibonacci integrates clamped cosine within 2%");

    const DdgiRayRotation r0 = ddgi_cpu::updateRotation(1234u, 0u);
    const DdgiRayRotation r0b = ddgi_cpu::updateRotation(1234u, 0u);
    const DdgiRayRotation r1 = ddgi_cpu::updateRotation(1234u, 1u);
    const Vec3 rows[3] = {r1.row0, r1.row1, r1.row2};
    f32 ortho_err = 0.f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            ortho_err = std::max(ortho_err, std::fabs(rows[i].dot(rows[j]) - (i == j ? 1.f : 0.f)));
        }
    }
    const f32 det = rows[0].dot(fuse::math::cross(rows[1], rows[2]));
    expectLess(ortho_err, 1e-5, "per-update ray rotation is orthonormal");
    expectLess(std::fabs(det - 1.f), 1e-5, "per-update ray rotation is proper (det +1)");
    expectTrue((r0.row0 - r0b.row0).length() < 1e-7f, "ray rotation deterministic per frame");
    expectTrue((r0.row0 - r1.row0).length() > 1e-3f, "ray rotation changes between updates");
}

void testBorderCopyTemplate() {
    constexpr u32 kRes = 8u;
    constexpr u32 kStride = kRes + 2u;
    std::vector<Vec3> tile(kStride * kStride);
    for (u32 y = 0; y < kRes; ++y) {
        for (u32 x = 0; x < kRes; ++x) {
            tile[(y + 1u) * kStride + x + 1u] = ddgi_cpu::texelDirection(x, y, kRes);
        }
    }
    ddgi_cpu::copyOctahedralBorder(tile.data(), kRes, kStride);
    const bool ok = borderMatchesFold(kRes, [&](u32 x, u32 y) { return tile[y * kStride + x]; }, 1e-6f);
    expectTrue(ok, "octahedral border texels equal the interior texel in the folded direction");
}

// ---------------------------------------------------------------------------------------------
// Gate: DDGI probes initialise and first update completes without error
// ---------------------------------------------------------------------------------------------

void testGateInitAndFirstUpdate() {
    const RefScene ref = bigRoom(1.f);
    const DdgiCpuScene scene = toDdgiScene(ref);
    DDGIDesc desc{};
    desc.grid_origin = {1.f, 1.f, 1.f};

    DdgiCpuVolume volume;
    expectTrue(volume.init(desc), "CPU probe volume initialises (2048 probes)");
    expectTrue(volume.probeCount() == 2048u, "probe count 16x8x16");
    expectTrue(volume.irradianceTileSize() == 10u && volume.distanceTileSize() == 18u,
               "bordered octahedral tiles 8+2 / 16+2");

    const DdgiCpuUpdateStats stats = volume.update(scene, 0u);
    expectTrue(stats.probes_updated == 64u, "first update blends 64 probes");
    expectTrue(stats.rays_traced == 64u * 256u, "first update traces 64 x 256 rays");
    expectTrue(volume.probeUpdateCount(0u) == 1u && volume.probeUpdateCount(63u) == 1u &&
                   volume.probeUpdateCount(64u) == 0u,
               "first update touches exactly the scheduled probes");

    bool finite = true;
    for (u32 p = 0; p < 64u; ++p) {
        for (u32 y = 0; y < volume.irradianceTileSize(); ++y) {
            for (u32 x = 0; x < volume.irradianceTileSize(); ++x) {
                finite = finite && finiteNonNegative(volume.irradianceTexel(p, x, y));
            }
        }
        for (u32 y = 0; y < volume.distanceTileSize(); ++y) {
            for (u32 x = 0; x < volume.distanceTileSize(); ++x) {
                const Vec2 m = volume.distanceTexel(p, x, y);
                finite = finite && std::isfinite(m.x) && m.x >= 0.f && m.x <= desc.max_ray_distance + 1e-3f &&
                         m.y + 1e-3f >= m.x * m.x;
            }
        }
    }
    expectTrue(finite, "first-update texels finite, non-negative, distance moments consistent");

    // Border texels of both atlases follow the octahedral fold.
    const u32 probe = 37u;
    expectTrue(borderMatchesFold(desc.irradiance_res,
                                 [&](u32 x, u32 y) { return volume.irradianceTexel(probe, x, y); },
                                 1e-6f),
               "irradiance tile border copy matches octahedral fold");
    expectTrue(borderMatchesFold(desc.depth_res,
                                 [&](u32 x, u32 y) {
                                     const Vec2 m = volume.distanceTexel(probe, x, y);
                                     return Vec3{m.x, m.y, 0.f};
                                 },
                                 1e-6f),
               "distance tile border copy matches octahedral fold");

    // First update uses no history (hysteresis 0): texels equal the fresh cosine-weighted
    // estimate. Recompute interior texels with the independent tracer.
    const DdgiRayRotation rot = ddgi_cpu::updateRotation(volume.config().rotation_seed, 0u);
    std::vector<Vec3> dirs(desc.rays_per_probe);
    std::vector<Vec3> radiance(desc.rays_per_probe);
    const Vec3 origin = ddgi_util::probeWorldPosition(desc, probe);
    for (u32 r = 0; r < desc.rays_per_probe; ++r) {
        dirs[r] = rot.apply(ddgi_cpu::sphericalFibonacci(r, desc.rays_per_probe)).normalized();
        radiance[r] = refRadiance(ref, origin, dirs[r], desc.max_ray_distance);
    }
    f32 worst_rel = 0.f;
    for (u32 y = 0; y < desc.irradiance_res; ++y) {
        for (u32 x = 0; x < desc.irradiance_res; ++x) {
            const Vec3 td = ddgi_cpu::texelDirection(x, y, desc.irradiance_res);
            Vec3 s{};
            f32 w = 0.f;
            for (u32 r = 0; r < desc.rays_per_probe; ++r) {
                const f32 c = std::max(0.f, td.dot(dirs[r]));
                s = s + radiance[r] * c;
                w += c;
            }
            const Vec3 expected = s * (1.f / w);
            const Vec3 got = volume.irradianceTexel(probe, x + 1u, y + 1u);
            worst_rel = std::max(worst_rel, (got - expected).length() / std::max(expected.length(), 1e-4f));
        }
    }
    std::printf("MEASURE: first-update texel vs independent re-trace worst relative diff %.2e\n", worst_rel);
    expectLess(worst_rel, 1e-3, "first-update irradiance texels match independent trace");

    // Probe (11, 1, 11) is inside a blocker: only backfaces -> black, short distances.
    const u32 buried = ProbeGridLayout::probeIndexFromCoord(desc, {5u, 0u, 5u});
    const u32 ids[1] = {buried};
    volume.updateProbes(scene, ids, 1u, 1u);
    expectTrue(volume.probeMeanTexel(buried).length() < 1e-6f, "probe inside geometry receives no light");
    expectLess(volume.probeMeanDistance(buried).x, 1.f, "probe inside geometry reports short backface distance");
}

// ---------------------------------------------------------------------------------------------
// Gates: 2048 probes update 64 per frame + irradiance responds to light changes in 64 frames
// ---------------------------------------------------------------------------------------------

struct ResponseStats {
    f64 mean = 0.0;
    f64 p10 = 0.0;
    f64 min = 0.0;
    u32 count = 0;
};

ResponseStats stepResponse(const DdgiCpuVolume& before,
                           const DdgiCpuVolume& after,
                           const DdgiCpuVolume& target,
                           f32 min_step) {
    std::vector<f64> fractions;
    for (u32 p = 0; p < before.probeCount(); ++p) {
        const f32 a = luminance(before.probeMeanTexel(p));
        const f32 b = luminance(target.probeMeanTexel(p));
        const f32 v = luminance(after.probeMeanTexel(p));
        if (std::fabs(b - a) < min_step) {
            continue;
        }
        fractions.push_back((v - a) / (b - a));
    }
    ResponseStats s{};
    if (fractions.empty()) {
        return s;
    }
    std::sort(fractions.begin(), fractions.end());
    f64 sum = 0.0;
    for (f64 f : fractions) {
        sum += f;
    }
    s.count = static_cast<u32>(fractions.size());
    s.mean = sum / fractions.size();
    s.min = fractions.front();
    s.p10 = fractions[fractions.size() / 10u];
    return s;
}

void runFrames(DdgiCpuVolume& volume, const DdgiCpuScene& scene, u32 first, u32 count) {
    for (u32 f = first; f < first + count; ++f) {
        volume.update(scene, f);
    }
}

void testGateRollingBudgetAndResponse() {
    DDGIDesc desc{};
    desc.grid_origin = {1.f, 1.f, 1.f};
    DdgiCpuConfig config{};
    config.multi_bounce = false; // linear in the light -> clean step reference
    const DdgiCpuScene dim = toDdgiScene(bigRoom(1.f));

    DdgiCpuVolume warm;
    warm.init(desc, config);

    // Rolling budget: 2048 probes / 64 per frame -> each probe exactly once per 32 frames.
    u32 updated = 0u;
    for (u32 f = 0; f < 32u; ++f) {
        updated += warm.update(dim, f).probes_updated;
    }
    bool once = true;
    for (u32 p = 0; p < warm.probeCount(); ++p) {
        once = once && warm.probeUpdateCount(p) == 1u;
    }
    expectTrue(updated == 2048u, "32 frames x 64 probes = 2048 probe updates");
    expectTrue(once, "after 32 frames every probe updated exactly once");
    runFrames(warm, dim, 32u, 32u);
    bool twice = true;
    for (u32 p = 0; p < warm.probeCount(); ++p) {
        twice = twice && warm.probeUpdateCount(p) == 2u;
    }
    expectTrue(twice, "after 64 frames every probe updated exactly twice");

    // Static light: how often does change detection fire on ray noise alone?
    {
        DdgiCpuVolume stable = warm;
        u32 fast_texels = 0u;
        for (u32 f = 64u; f < 96u; ++f) {
            fast_texels += stable.update(dim, f).fast_response_texels;
        }
        const f64 texel_updates = 32.0 * 64.0 * desc.irradiance_res * desc.irradiance_res;
        const f64 rate = fast_texels / texel_updates;
        std::printf("MEASURE: static light, change-detection false-trigger rate %.3f%% of texel updates\n",
                    100.0 * rate);
        expectLess(rate, 0.01, "change detection rarely fires under static light (< 1%)");
    }

    const f32 h = desc.hysteresis;
    std::printf("MEASURE: hysteresis %.2f pure EMA step response 1-h^n: n=2 %.1f%%, n=32 %.1f%%, "
                "n=64 %.1f%%; updates for 90%%: %d, 95%%: %d\n",
                h,
                100.0 * (1.0 - std::pow(h, 2.0)),
                100.0 * (1.0 - std::pow(h, 32.0)),
                100.0 * (1.0 - std::pow(h, 64.0)),
                static_cast<int>(std::ceil(std::log(0.1) / std::log(h))),
                static_cast<int>(std::ceil(std::log(0.05) / std::log(h))));

    struct Step {
        const char* name;
        f32 scale;
    };
    const Step steps[] = {{"sun x4", 4.f}, {"sun x0.8", 0.8f}, {"sun off", 0.f}};
    for (const Step& step : steps) {
        const DdgiCpuScene lit = toDdgiScene(bigRoom(step.scale));
        // Reference: converged irradiance under the new light, averaged over independent ray
        // rotations (4 full-grid updates at hysteresis 0.7, change detection off).
        DDGIDesc target_desc = desc;
        target_desc.hysteresis = 0.7f;
        DdgiCpuConfig target_config = config;
        target_config.change_threshold = 1e9f;
        target_config.probe_change_threshold = 1e9f;
        DdgiCpuVolume target;
        target.init(target_desc, target_config);
        const std::vector<u32> ids = allProbes(target.probeCount());
        for (u32 f = 0; f < 4u; ++f) {
            target.updateProbes(lit, ids.data(), static_cast<u32>(ids.size()), 100000u + f);
        }

        DdgiCpuVolume fast = warm;
        runFrames(fast, lit, 64u, 64u);
        const ResponseStats rf = stepResponse(warm, fast, target, 0.01f);
        std::vector<f64> rel_err;
        for (u32 p = 0; p < fast.probeCount(); ++p) {
            const f32 b = luminance(target.probeMeanTexel(p));
            const f32 a = luminance(warm.probeMeanTexel(p));
            if (std::max(a, b) > 1e-3f) {
                rel_err.push_back(std::fabs(luminance(fast.probeMeanTexel(p)) - b) / std::max(a, b));
            }
        }
        std::sort(rel_err.begin(), rel_err.end());

        // Same step with change detection off: pure hysteresis EMA (first step only).
        ResponseStats re{};
        const bool check_ema = &step == &steps[0];
        if (check_ema) {
            DdgiCpuVolume ema = warm;
            ema.config().change_threshold = 1e9f;
            ema.config().probe_change_threshold = 1e9f;
            runFrames(ema, lit, 64u, 64u);
            re = stepResponse(warm, ema, target, 0.01f);
        }

        std::printf("MEASURE: %s, 64 frames (2 updates/probe), %u probes: step response mean %.1f%% p10 %.1f%%; "
                    "error vs converged p50 %.1f%% p90 %.1f%%\n",
                    step.name,
                    rf.count,
                    100.0 * rf.mean,
                    100.0 * rf.p10,
                    100.0 * rel_err[rel_err.size() / 2u],
                    100.0 * rel_err[rel_err.size() * 9u / 10u]);
        if (check_ema) {
            std::printf("MEASURE: %s with change detection off (pure EMA): mean %.1f%% (analytic 1-h^2 = %.1f%%)\n",
                        step.name,
                        100.0 * re.mean,
                        100.0 * (1.0 - std::pow(h, 2.0)));
            expectLess(std::fabs(re.mean - (1.0 - std::pow(h, 2.0))), 0.02,
                       "pure EMA response matches 1 - h^2 (hysteresis math)");
        }
        expectTrue(rf.count > 512u, "light step changes at least a quarter of the probes");
        expectGreater(rf.mean, 0.95, "mean step response within 64 frames >= 95%");
        expectLess(rf.mean, 1.05, "step response does not overshoot");
        expectGreater(rf.p10, 0.85, "90% of probes reach >= 85% of the step within 64 frames");
        expectLess(rel_err[rel_err.size() * 9u / 10u], 0.08,
                   "90% of probes within 8% of the converged irradiance after 64 frames");
    }
}

// ---------------------------------------------------------------------------------------------
// Gate: moving the sun rotates the GI colour cast
// ---------------------------------------------------------------------------------------------

/// (R - G) / (R + G) of the indirect irradiance at a white floor point.
f32 floorCast(f32 azimuth_deg, const Vec3& point) {
    RefScene ref{};
    ref.boxes.push_back({{-4.f, -0.2f, -4.f}, {4.f, 0.f, 4.f}, kWhite, {}});
    ref.boxes.push_back({{3.f, 0.f, -4.f}, {3.2f, 4.f, 4.f}, kRed, {}});
    ref.boxes.push_back({{-3.2f, 0.f, -4.f}, {-3.f, 4.f, 4.f}, kGreen, {}});
    const f32 az = azimuth_deg * kPi / 180.f;
    const f32 elev = 40.f * kPi / 180.f;
    ref.sun_dir = Vec3{std::cos(az) * std::cos(elev), std::sin(elev), std::sin(az) * std::cos(elev)}.normalized();
    ref.sun_irradiance = {3.f, 3.f, 3.f};
    ref.sky = {0.05f, 0.05f, 0.05f};
    const DdgiCpuScene scene = toDdgiScene(ref);

    DDGIDesc desc{};
    desc.grid_origin = {-2.5f, 0.5f, -3.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {6, 4, 7};
    DdgiCpuVolume volume;
    volume.init(desc);
    const std::vector<u32> ids = allProbes(volume.probeCount());
    for (u32 f = 0; f < 3u; ++f) {
        volume.updateProbes(scene, ids.data(), static_cast<u32>(ids.size()), f);
    }
    const Vec3 e = volume.sampleIrradiance(point, {0.f, 1.f, 0.f});
    return (e.x - e.y) / std::max(e.x + e.y, 1e-6f);
}

void testGateSunRotatesColourCast() {
    const Vec3 centre{0.f, 0.f, 0.f};
    const f32 sun_west = floorCast(180.f, centre); // sun on -x: lights the red (+x) wall
    const f32 sun_east = floorCast(0.f, centre);   // sun on +x: lights the green (-x) wall
    std::printf("MEASURE: floor centre cast (R-G)/(R+G): sun west %+.3f, sun east %+.3f\n", sun_west, sun_east);
    expectGreater(sun_west, 0.15, "sun on the red wall -> red indirect cast on the white floor");
    expectLess(sun_east, -0.15, "sun swapped to the green wall -> green indirect cast");

    // Timelapse as data: sweep the sun azimuth; the cast must track -cos(azimuth).
    std::printf("MEASURE: sun azimuth sweep cast:");
    f64 sxy = 0.0;
    f64 sxx = 0.0;
    f64 syy = 0.0;
    f64 sx = 0.0;
    f64 sy = 0.0;
    bool signs = true;
    constexpr u32 kSteps = 8u;
    for (u32 i = 0; i < kSteps; ++i) {
        const f32 az = 360.f * static_cast<f32>(i) / kSteps;
        const f32 cast = floorCast(az, centre);
        const f64 x = -std::cos(az * kPi / 180.f);
        std::printf(" %03.0f:%+.2f", az, cast);
        if (std::fabs(x) > 0.5 && (cast > 0.f) != (x > 0.0)) {
            signs = false;
        }
        sx += x;
        sy += cast;
        sxx += x * x;
        syy += static_cast<f64>(cast) * cast;
        sxy += x * cast;
    }
    const f64 n = kSteps;
    const f64 corr = (n * sxy - sx * sy) / std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
    std::printf("\nMEASURE: cast vs -cos(azimuth) correlation %.3f\n", corr);
    expectTrue(signs, "colour cast flips sign as the sun crosses between the walls");
    expectGreater(corr, 0.9, "colour cast follows the sun azimuth (correlation > 0.9)");
}

// ---------------------------------------------------------------------------------------------
// Gate: GI on a white Lambertian surface matches reference Monte Carlo within 10%
// ---------------------------------------------------------------------------------------------

void testGateMonteCarloReference() {
    const RefScene ref = cornellBox();
    const DdgiCpuScene scene = toDdgiScene(ref);
    const DDGIDesc desc = cornellDesc();
    DdgiCpuConfig config{};
    config.multi_bounce = false; // one-bounce, same light transport as the MC reference
    DdgiCpuVolume volume;
    expectTrue(volume.init(desc, config), "Cornell probe volume initialises");
    const std::vector<u32> ids = allProbes(volume.probeCount());
    for (u32 f = 0; f < 16u; ++f) {
        volume.updateProbes(scene, ids.data(), static_cast<u32>(ids.size()), f);
    }

    struct Point {
        Vec3 p;
        Vec3 n;
    };
    const Point points[] = {
        {{1.0f, 0.f, 1.0f}, {0.f, 1.f, 0.f}},
        {{2.0f, 0.f, 2.0f}, {0.f, 1.f, 0.f}},
        {{3.1f, 0.f, 1.3f}, {0.f, 1.f, 0.f}},
        {{1.4f, 0.f, 3.0f}, {0.f, 1.f, 0.f}},
        {{2.7f, 0.f, 3.3f}, {0.f, 1.f, 0.f}},
        {{0.6f, 0.f, 2.2f}, {0.f, 1.f, 0.f}},
        {{2.0f, 2.0f, 0.f}, {0.f, 0.f, 1.f}},
        {{1.0f, 1.0f, 0.f}, {0.f, 0.f, 1.f}},
        {{3.2f, 2.8f, 0.f}, {0.f, 0.f, 1.f}},
    };
    f32 worst_lum = 0.f;
    f32 worst_channel = 0.f;
    f32 sum_lum = 0.f;
    u32 index = 0u;
    for (const Point& pt : points) {
        const Vec3 mc = refIrradianceMC(ref, pt.p, pt.n, 40000u, 101u + index);
        const Vec3 probe = volume.sampleIrradiance(pt.p, pt.n);
        const f32 lum_err = std::fabs(luminance(probe) - luminance(mc)) / luminance(mc);
        const f32 ch_err = std::max({std::fabs(probe.x - mc.x) / mc.x,
                                     std::fabs(probe.y - mc.y) / mc.y,
                                     std::fabs(probe.z - mc.z) / mc.z});
        std::printf("MEASURE: MC point (%.1f,%.1f,%.1f) n(%.0f,%.0f,%.0f): MC (%.4f %.4f %.4f) DDGI "
                    "(%.4f %.4f %.4f) lum err %.2f%% worst channel %.2f%%\n",
                    pt.p.x, pt.p.y, pt.p.z, pt.n.x, pt.n.y, pt.n.z, mc.x, mc.y, mc.z, probe.x, probe.y,
                    probe.z, 100.f * lum_err, 100.f * ch_err);
        worst_lum = std::max(worst_lum, lum_err);
        worst_channel = std::max(worst_channel, ch_err);
        sum_lum += lum_err;
        ++index;
    }
    std::printf("MEASURE: DDGI vs MC over %u points: mean lum err %.2f%%, worst lum err %.2f%%, worst channel "
                "%.2f%%\n",
                index, 100.f * sum_lum / index, 100.f * worst_lum, 100.f * worst_channel);
    expectLess(worst_lum, 0.10, "probe GI on white Lambertian surface within 10% of Monte Carlo (luminance)");
    expectLess(worst_channel, 0.10, "probe GI within 10% of Monte Carlo per RGB channel");
}

// ---------------------------------------------------------------------------------------------
// Gate: emissive surfaces contribute correct radiance to GI probes
// ---------------------------------------------------------------------------------------------

/// Irradiance from a uniformly emitting convex polygon (Lambert's contour formula).
f32 polygonIrradiance(const std::vector<Vec3>& corners, const Vec3& x, const Vec3& n, f32 radiance) {
    f32 sum = 0.f;
    for (usize i = 0; i < corners.size(); ++i) {
        const Vec3 a = (corners[i] - x).normalized();
        const Vec3 b = (corners[(i + 1u) % corners.size()] - x).normalized();
        const f32 theta = std::acos(std::clamp(a.dot(b), -1.f, 1.f));
        const Vec3 g = fuse::math::cross(a, b).normalized();
        sum += theta * g.dot(n);
    }
    return 0.5f * radiance * std::fabs(sum);
}

/// Differential area to a parallel rectangle with one corner above it (a x b at height c).
f64 cornerFormFactor(f64 a, f64 b, f64 c) {
    const f64 A = a / c;
    const f64 B = b / c;
    const f64 sa = std::sqrt(1.0 + A * A);
    const f64 sb = std::sqrt(1.0 + B * B);
    return (A / sa * std::atan(B / sa) + B / sb * std::atan(A / sb)) / (2.0 * 3.14159265358979323846);
}

void testGateEmissiveContribution() {
    fuse::renderer::Material panel_material{};
    panel_material.baseColor = {0.f, 0.f, 0.f};
    panel_material.emissiveColor = {1.f, 0.5f, 0.25f};
    panel_material.emissiveIntensity = 4.f;
    const DdgiCpuSurface panel = ddgiSurfaceFromMaterial(panel_material);
    expectTrue(std::fabs(panel.emissive.x - 4.f) < 1e-6f && std::fabs(panel.emissive.z - 1.f) < 1e-6f,
               "material emissive colour x intensity becomes surface radiance");

    // 2 x 2 panel, 1 m above a single probe at the origin, facing down.
    const f32 half = 1.f;
    const f32 height = 1.f;
    DdgiCpuScene scene{};
    scene.addBox({-half, height, -half}, {half, height + 1e-3f, half}, panel);

    DDGIDesc desc{};
    desc.grid_dims = {1, 1, 1};
    desc.probes_per_frame = 1;
    DdgiCpuVolume volume;
    volume.init(desc);
    for (u32 f = 0; f < 128u; ++f) {
        volume.update(scene, f);
    }

    // Analytic: four corner rectangles, E = pi * L * F.
    const f64 f_total = 4.0 * cornerFormFactor(half, half, height);
    const Vec3 up{0.f, 1.f, 0.f};
    const Vec3 e_up = volume.probeIrradiance(0u, up);
    const f64 analytic_up = kPi * f_total * panel.emissive.x;
    const f64 err_up = std::fabs(e_up.x - analytic_up) / analytic_up;
    const f64 chroma = std::fabs(e_up.y / e_up.x - 0.5) + std::fabs(e_up.z / e_up.x - 0.25);
    std::printf("MEASURE: emissive panel L=%.1f F=%.4f: analytic E %.4f, probe E %.4f (%.2f%%), chroma err %.1e\n",
                panel.emissive.x, f_total, analytic_up, e_up.x, 100.0 * err_up, chroma);
    expectLess(err_up, 0.03, "probe irradiance from emissive panel within 3% of form-factor result");
    expectLess(chroma, 1e-3, "emissive colour carried unchanged into probe irradiance");

    // Tilted normals and a displaced sample point vs Lambert's contour integral.
    const std::vector<Vec3> corners = {
        {-half, height, -half}, {half, height, -half}, {half, height, half}, {-half, height, half}};
    const Vec3 normals[] = {Vec3{0.5f, 0.866f, 0.f}, Vec3{0.f, 0.866f, -0.5f}, Vec3{-0.35f, 0.9f, 0.25f}};
    f64 worst = err_up;
    for (const Vec3& raw : normals) {
        const Vec3 n = raw.normalized();
        const f64 analytic = polygonIrradiance(corners, {0.f, 0.f, 0.f}, n, panel.emissive.x);
        const Vec3 e = volume.sampleIrradiance({0.f, 0.f, 0.f}, n);
        const f64 err = std::fabs(e.x - analytic) / analytic;
        worst = std::max(worst, err);
        std::printf("MEASURE: emissive tilted n(%.2f,%.2f,%.2f): analytic %.4f probe %.4f (%.2f%%)\n",
                    n.x, n.y, n.z, analytic, e.x, 100.0 * err);
    }
    // Tilted normals cross the octahedral equator crease, where bilinear interpolation of the
    // 8x8 tile is least accurate; the same scene at 16x16 shows the error is resolution-bound.
    expectLess(worst, 0.07, "tilted-normal probe irradiance within 7% of contour integral (8x8 texels)");
    DDGIDesc fine_desc = desc;
    fine_desc.irradiance_res = 16;
    DdgiCpuVolume fine;
    fine.init(fine_desc);
    for (u32 f = 0; f < 128u; ++f) {
        fine.update(scene, f);
    }
    f64 worst_fine = 0.0;
    for (const Vec3& raw : normals) {
        const Vec3 n = raw.normalized();
        const f64 analytic = polygonIrradiance(corners, {0.f, 0.f, 0.f}, n, panel.emissive.x);
        worst_fine = std::max(worst_fine, std::fabs(fine.probeIrradiance(0u, n).x - analytic) / analytic);
    }
    std::printf("MEASURE: emissive tilted normals worst error: 8x8 %.2f%%, 16x16 %.2f%%\n",
                100.0 * worst, 100.0 * worst_fine);
    expectLess(worst_fine, 0.035, "tilted-normal probe irradiance within 3.5% at 16x16 texels");
    expectLess(std::fabs(polygonIrradiance(corners, {0.f, 0.f, 0.f}, up, 1.f) / kPi - f_total), 1e-4,
               "contour integral and form-factor references agree");

    // Facing away from the panel: no emissive light.
    const Vec3 e_down = volume.probeIrradiance(0u, {0.f, -1.f, 0.f});
    expectLess(e_down.x, 1e-4, "probe irradiance facing away from the panel is ~0");
}

// ---------------------------------------------------------------------------------------------
// Timing (RUN_SERIAL): CPU cost of one 64-probe x 256-ray update
// ---------------------------------------------------------------------------------------------

void testTimingUpdateCycle() {
    DDGIDesc desc{};
    desc.grid_origin = {1.f, 1.f, 1.f};
    const DdgiCpuScene scene = toDdgiScene(bigRoom(1.f));
    DdgiCpuVolume volume;
    volume.init(desc);
    runFrames(volume, scene, 0u, 32u); // every probe has history (multi-bounce reads real data)

    constexpr u32 kSamples = 32u;
    std::vector<f64> ms;
    for (u32 i = 0; i < kSamples; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const DdgiCpuUpdateStats s = volume.update(scene, 32u + i);
        const auto t1 = std::chrono::steady_clock::now();
        expectTrue(s.probes_updated == 64u && s.rays_traced == 64u * 256u, "timed update is 64 x 256");
        ms.push_back(std::chrono::duration<f64, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    std::printf("MEASURE: CPU DDGI update 64 probes x 256 rays (multi-bounce, single thread): min %.2f ms, "
                "median %.2f ms, max %.2f ms. GPU < 2 ms target is hardware-only.\n",
                ms.front(), ms[ms.size() / 2u], ms.back());
    expectTrue(ms[ms.size() / 2u] > 0.0, "timing measured");
}

} // namespace

int main(int argc, char** argv) {
    const bool timing = argc > 1 && std::strcmp(argv[1], "--timing") == 0;
    if (timing) {
        testTimingUpdateCycle();
    } else {
        testOctahedralRoundTrip();
        testRayGeneration();
        testBorderCopyTemplate();
        testGateInitAndFirstUpdate();
        testGateRollingBudgetAndResponse();
        testGateSunRotatesColourCast();
        testGateMonteCarloReference();
        testGateEmissiveContribution();
    }

    if (g_failures == 0) {
        std::printf("fuse_b5_ddgi_gates%s: all checks passed\n", timing ? " (timing)" : "");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b5_ddgi_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
