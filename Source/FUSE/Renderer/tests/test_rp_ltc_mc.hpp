// WP-2.2 test helpers shared by test_rp_ltc_cpu.cpp and test_rp_ltc.cpp: f64 Monte Carlo references of
// area lights and sun disks for the compensated BRDF, and the random configurations the gates use.
#pragma once

#include <fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace ltc_test {

using namespace fuse::renderer;
using namespace fuse::renderer::lighting_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::usize;
using fuse::math::Vec3;

inline const f32* lut() { return ltc::sharedBrdfLut().data(); }

inline constexpr f64 kPi = 3.141592653589793;

// --- f64 helpers -------------------------------------------------------------------------------------
struct D3 {
    f64 x = 0, y = 0, z = 0;
};
inline D3 d3(Vec3 v) { return {v.x, v.y, v.z}; }
inline Vec3 f3(D3 v) { return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)}; }
inline D3 add(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 sub(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 mul(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline f64 len(D3 a) { return std::sqrt(dot(a, a)); }
inline D3 unit(D3 a) { return mul(a, 1.0 / len(a)); }

inline f64 alphaOf(f64 roughness) {
    const f64 r = std::clamp(roughness, 0.045, 1.0);
    return r * r;
}
inline f64 ggxD(f64 noh, f64 a2) {
    const f64 dd = (noh * a2 - noh) * noh + 1.0;
    return a2 / (kPi * dd * dd);
}
inline f64 g1(f64 nov, f64 a2) { return 2.0 * nov / (nov + std::sqrt(a2 + (1.0 - a2) * nov * nov)); }

/// Orthonormal frame with n = z and t1 along the view's tangent projection.
struct Frame {
    D3 t, b, n;
    D3 toWorld(D3 l) const { return add(add(mul(t, l.x), mul(b, l.y)), mul(n, l.z)); }
    D3 toLocal(D3 w) const { return {dot(w, t), dot(w, b), dot(w, n)}; }
};
inline Frame frameOf(D3 n, D3 v) {
    D3 t = sub(v, mul(n, dot(n, v)));
    if (len(t) < 1e-6) {
        t = std::fabs(n.x) < 0.5 ? D3{1, 0, 0} : D3{0, 1, 0};
        t = sub(t, mul(n, dot(n, t)));
    }
    t = unit(t);
    return {t, cross(n, t), n};
}

inline D3 vndfLocal(D3 v, f64 alpha, f64 u1, f64 u2) {
    const D3 vh = unit({alpha * v.x, alpha * v.y, v.z});
    const f64 lensq = vh.x * vh.x + vh.y * vh.y;
    const D3 t1 = lensq > 0.0 ? mul(D3{-vh.y, vh.x, 0.0}, 1.0 / std::sqrt(lensq)) : D3{1.0, 0.0, 0.0};
    const D3 t2 = cross(vh, t1);
    const f64 r = std::sqrt(u1);
    const f64 phi = 2.0 * kPi * u2;
    const f64 p1 = r * std::cos(phi);
    f64 p2 = r * std::sin(phi);
    const f64 s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * std::sqrt(std::max(1.0 - p1 * p1, 0.0)) + s * p2;
    const D3 nh = add(add(mul(t1, p1), mul(t2, p2)), mul(vh, std::sqrt(std::max(1.0 - p1 * p1 - p2 * p2, 0.0))));
    return unit({alpha * nh.x, alpha * nh.y, std::max(nh.z, 0.0)});
}

// --- configurations ------------------------------------------------------------------------------------
enum class Part { Diffuse, Specular, Total };

struct Config {
    SurfaceSample s{};
    Vec3 v{};
    gpu_scene::GpuLight light{};
    bool straddles = false;
};

/// Terms of the surface with one lobe zeroed (Diffuse: no specular; Specular: no diffuse).
inline SurfaceTerms partTerms(const Config& c, Part part) {
    SurfaceTerms t = surface_terms(c.s, c.v, lut());
    if (part == Part::Diffuse) {
        t.ms.specular = {};
        t.ms.comp_dielectric = 0.f;
        t.ms.comp_metal = {};
    } else if (part == Part::Specular) {
        t.ms.diffuse = {};
    }
    return t;
}

/// Monte Carlo reference of one area light (rect or disk): MIS of uniform area sampling and VNDF
/// sampling with ray hits, 256 x 256 strata each.
inline f64 mcArea(const Config& c, Part part) {
    const SurfaceTerms t = partTerms(c, part);
    const gpu_scene::GpuLight& L = c.light;
    const bool disk = L.type == ltc::kLightDisk;
    const D3 center{L.position[0], L.position[1], L.position[2]};
    const D3 nl = unit({L.direction[0], L.direction[1], L.direction[2]});
    Vec3 exf{};
    Vec3 eyf{};
    ltc::area_axes(f3(nl), ltc::decode_tangent(L.flags), L.cosInner, L.cosOuter, exf, eyf);
    const D3 ex = d3(exf), ey = d3(eyf);
    const f64 rx = len(ex), ry = len(ey);
    const D3 ux = unit(ex), uy = unit(ey);
    const f64 area = disk ? kPi * rx * ry : 4.0 * rx * ry;
    const D3 p = d3(c.s.position);
    const f64 window = ltc::area_window(static_cast<f32>(len(sub(center, p))), L.range);
    const f64 radiance = L.intensity * window; // colour 1
    const D3 n = d3(c.s.normal);
    const D3 v = d3(c.v);
    const Frame fr = frameOf(n, v);
    const f64 alpha = alphaOf(c.s.roughness);
    const f64 a2 = alpha * alpha;
    const D3 vl = fr.toLocal(v);
    const f64 nov = std::max(vl.z, 1e-4);
    const f64 gv1 = g1(nov, a2);
    auto pdfVndf = [&](D3 lw) {
        const D3 h = unit(add(vl, fr.toLocal(lw)));
        return gv1 * ggxD(std::max(h.z, 0.0), a2) / (4.0 * nov);
    };
    auto f = [&](D3 lw) { return static_cast<f64>(brdf::multi_scatter_cos(t.ms, c.s.albedo, c.s.roughness, c.s.normal, c.v, f3(lw)).x); };
    constexpr u32 kN = 256;
    f64 sum = 0.0;
    for (u32 j = 0; j < kN; ++j) {
        for (u32 i = 0; i < kN; ++i) {
            const f64 u1 = (i + 0.5) / kN;
            const f64 u2 = (j + 0.5) / kN;
            // (1) uniform area sample
            D3 y{};
            if (disk) {
                const f64 r = std::sqrt(u1);
                y = add(center, add(mul(ex, r * std::cos(2.0 * kPi * u2)), mul(ey, r * std::sin(2.0 * kPi * u2))));
            } else {
                y = add(center, add(mul(ex, 2.0 * u1 - 1.0), mul(ey, 2.0 * u2 - 1.0)));
            }
            {
                const D3 d = sub(y, p);
                const f64 dist = len(d);
                const D3 lw = mul(d, 1.0 / dist);
                const f64 cosL = -dot(lw, nl);
                if (cosL > 0.0) {
                    const f64 pa = dist * dist / (area * cosL);
                    sum += f(lw) / (pa + pdfVndf(lw));
                }
            }
            // (2) VNDF sample, ray vs the light
            const D3 h = vndfLocal(vl, alpha, u1, u2);
            const D3 ll = sub(mul(h, 2.0 * dot(vl, h)), vl);
            if (ll.z <= 0.0) {
                continue;
            }
            const D3 lw = fr.toWorld(ll);
            const f64 denom = dot(lw, nl);
            if (!(denom < 0.0)) {
                continue;
            }
            const f64 tHit = dot(sub(center, p), nl) / denom;
            if (!(tHit > 0.0)) {
                continue;
            }
            const D3 q = sub(add(p, mul(lw, tHit)), center);
            const f64 qx = dot(q, ux) / rx;
            const f64 qy = dot(q, uy) / ry;
            const bool hit = disk ? (qx * qx + qy * qy <= 1.0) : (std::fabs(qx) <= 1.0 && std::fabs(qy) <= 1.0);
            if (!hit) {
                continue;
            }
            const f64 pa = tHit * tHit / (area * -denom);
            sum += f(lw) / (pa + pdfVndf(lw));
        }
    }
    return radiance * sum / (kN * kN);
}

inline f64 ltcArea(const Config& c, Part part) {
    const SurfaceTerms t = partTerms(c, part);
    return light_contribution(c.light, c.s, c.v, t, lut()).x;
}

/// Brightness scale of a light for the error metrics: its response on a white Lambertian surface
/// (radiance x window x form factor, the exact identity-LTC polygon / cone integral).
inline f64 lightScale(const Config& c) {
    SurfaceTerms t = surface_terms(c.s, c.v, lut());
    t.ms.specular = {};
    t.ms.comp_dielectric = 0.f;
    t.ms.comp_metal = {};
    t.ms.diffuse = {1.f, 1.f, 1.f};
    return light_contribution(c.light, c.s, c.v, t, lut()).x;
}

/// The specular response the light would have at the lobe's peak: radiance x window x the lobe's
/// albedo x the LTC form factor of the same light moved onto the lobe's average direction (so
/// "inside the lobe" can be told from "in its tail").
inline f64 lobePeak(const Config& c) {
    const SurfaceTerms t = surface_terms(c.s, c.v, lut());
    // Lobe direction: M e_z in the LTC frame (M^-1 has rows (m0, 0, m1), (0, 1, 0), (m2, 0, m3)).
    const f64 det = static_cast<f64>(t.frame.m[0]) * t.frame.m[3] - static_cast<f64>(t.frame.m[1]) * t.frame.m[2];
    const f64 mx = -t.frame.m[1] / det;
    const f64 mz = t.frame.m[0] / det;
    D3 dir = add(mul(d3(t.frame.t1), mx), mul(d3(t.frame.n), mz));
    dir = unit(dir);
    if (dir.z * 0.0 + dot(dir, d3(t.frame.n)) < 0.05) {
        dir = unit(add(dir, mul(d3(t.frame.n), 0.05 - dot(dir, d3(t.frame.n)))));
    }
    Config moved = c;
    const D3 p = d3(c.s.position);
    const D3 center{c.light.position[0], c.light.position[1], c.light.position[2]};
    const f64 dist = len(sub(center, p));
    const D3 nc = add(p, mul(dir, dist));
    moved.light.position[0] = static_cast<f32>(nc.x);
    moved.light.position[1] = static_cast<f32>(nc.y);
    moved.light.position[2] = static_cast<f32>(nc.z);
    // Face the surface point, keep the size.
    moved.light.direction[0] = static_cast<f32>(-dir.x);
    moved.light.direction[1] = static_cast<f32>(-dir.y);
    moved.light.direction[2] = static_cast<f32>(-dir.z);
    SurfaceTerms s = t;
    s.ms.diffuse = {};
    return light_contribution(moved.light, moved.s, moved.v, s, lut()).x;
}

inline Vec3 randomUnit(std::mt19937& rng) {
    std::normal_distribution<f32> g(0.f, 1.f);
    return Vec3{g(rng), g(rng), g(rng)}.normalized();
}

/// Punctual directional light towards `l` (unit), irradiance 1.
inline gpu_scene::GpuLight directionalOf(D3 l) {
    gpu_scene::GpuLight d{};
    d.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    d.direction[0] = static_cast<f32>(-l.x);
    d.direction[1] = static_cast<f32>(-l.y);
    d.direction[2] = static_cast<f32>(-l.z);
    d.intensity = 1.f;
    return d;
}

/// Random configuration: surface at the origin-ish, light above (or straddling) its horizon.
inline Config makeConfig(std::mt19937& rng, u32 k, u32 type, bool straddle) {
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    Config c{};
    c.s.position = {u(rng) - 0.5f, u(rng) - 0.5f, u(rng) - 0.5f};
    c.s.normal = randomUnit(rng);
    const f32 roughnesses[6] = {0.045f, 0.1f, 0.25f, 0.5f, 0.75f, 1.f};
    c.s.roughness = roughnesses[k % 6u];
    c.s.metallic = (k / 6u) % 3u == 0u ? 0.f : ((k / 6u) % 3u == 1u ? 1.f : 0.5f);
    c.s.albedo = {0.3f + 0.7f * u(rng), 0.3f + 0.7f * u(rng), 0.3f + 0.7f * u(rng)};
    // View: N.V in [0.1, 1].
    const Frame fr = frameOf(d3(c.s.normal), d3(randomUnit(rng)));
    const f64 cv = 0.1 + 0.9 * u(rng);
    const f64 phv = 2.0 * kPi * u(rng);
    c.v = f3(fr.toWorld({std::sqrt(1.0 - cv * cv) * std::cos(phv), std::sqrt(1.0 - cv * cv) * std::sin(phv), cv}));
    // Light direction: elevation above the horizon (straddling: near it), distance 1..4.
    const f64 elev = straddle ? 0.02 + 0.25 * u(rng) : 0.35 + 1.2 * u(rng);
    const f64 phl = 2.0 * kPi * u(rng);
    const D3 dir = fr.toWorld({std::cos(elev) * std::cos(phl), std::cos(elev) * std::sin(phl), std::sin(elev)});
    const f64 dist = 1.0 + 3.0 * u(rng);
    ltc::AreaLightDesc d{};
    d.center = f3(add(d3(c.s.position), mul(dir, dist)));
    // Facing the surface point, tilted by up to ~50 degrees.
    const D3 tilt = d3(randomUnit(rng));
    D3 nl = add(mul(dir, -1.0), mul(tilt, 0.8 * u(rng)));
    if (dot(nl, dir) > -0.2) {
        nl = mul(dir, -1.0);
    }
    d.normal = f3(unit(nl));
    d.tangent = randomUnit(rng);
    d.halfWidth = static_cast<f32>(dist * (0.1 + (straddle ? 0.6 : 0.4) * u(rng)));
    d.halfHeight = static_cast<f32>(dist * (0.1 + (straddle ? 0.6 : 0.4) * u(rng)));
    if (type == ltc::kLightDisk && k % 2u == 0u) {
        d.halfHeight = d.halfWidth; // circular
    }
    d.intensity = 2.f;
    d.range = static_cast<f32>(dist * 4.0);
    c.light = type == ltc::kLightRect ? ltc::makeRectLight(d) : ltc::makeDiskLight(d);
    return c;
}

struct Stats {
    std::vector<f64> rel;
    f64 mean() const {
        f64 s = 0.0;
        for (f64 r : rel) s += r;
        return rel.empty() ? 0.0 : s / static_cast<f64>(rel.size());
    }
    f64 percentile(f64 q) const {
        if (rel.empty()) return 0.0;
        std::vector<f64> v = rel;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1u, static_cast<usize>(q * static_cast<f64>(v.size())))];
    }
    f64 max() const { return rel.empty() ? 0.0 : *std::max_element(rel.begin(), rel.end()); }
};

/// Relative error, scaled by the total response of the configuration (a lobe that contributes a
/// negligible part of the pixel is judged against the pixel).
inline void record(Stats& s, f64 got, f64 ref, f64 total) { s.rel.push_back(std::fabs(got - ref) / std::max(std::fabs(total), 1e-12)); }

/// Monte Carlo over the sun's cone (uniform in solid angle, 256 x 256 strata): irradiance E at normal
/// incidence, radiance E / (pi sin^2).
inline f64 mcSun(const SurfaceSample& s, Vec3 v, Vec3 l, f32 radius, const SurfaceTerms& t) {
    const Frame fr = frameOf(d3(l), D3{0.3, 0.5, 0.8}); // any frame around l
    const f64 cosR = std::cos(static_cast<f64>(radius));
    const f64 omega = 2.0 * kPi * (1.0 - cosR);
    const f64 radiance = 1.0 / (kPi * std::sin(radius) * std::sin(radius));
    constexpr u32 kN = 256;
    f64 sum = 0.0;
    for (u32 j = 0; j < kN; ++j) {
        for (u32 i = 0; i < kN; ++i) {
            const f64 ct = 1.0 - (1.0 - cosR) * (i + 0.5) / kN;
            const f64 st = std::sqrt(std::max(1.0 - ct * ct, 0.0));
            const f64 ph = 2.0 * kPi * (j + 0.5) / kN;
            const D3 w = fr.toWorld({st * std::cos(ph), st * std::sin(ph), ct});
            sum += brdf::multi_scatter_cos(t.ms, s.albedo, s.roughness, s.normal, v, f3(w)).x;
        }
    }
    return radiance * omega * sum / (kN * kN);
}


} // namespace ltc_test
