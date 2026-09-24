// WP-8.3 volumetric clouds: CPU reference (see include/fuse/renderer/clouds/cloud_reference.hpp). Every function
// is the twin of the same-named helper in shaders/clouds/cl_common.{glsl,slang}: same operations, same order, f32.
#include <fuse/renderer/clouds/cloud_reference.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::clouds {

using math::Vec3;

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

f32 saturate(f32 v) { return std::max(0.f, std::min(1.f, v)); }
f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 fract(f32 v) { return v - std::floor(v); }
f32 dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 mul3(const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 v3(const f32 (&a)[4]) { return Vec3{a[0], a[1], a[2]}; }
void set4(f32 (&dst)[4], const Vec3& v, f32 w) {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
    dst[3] = w;
}
bool pow2(u32 v) { return v != 0u && (v & (v - 1u)) == 0u; }
bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

/// 1 - exp(-x) without cancellation for small x.
f32 oneMinusExp(f32 x) {
    if (x < 0.02f) {
        return x * (1.f - x * (0.5f - x * (1.f / 6.f - x * (1.f / 24.f))));
    }
    return 1.f - std::exp(-x);
}

f32 remap(f32 v, f32 lo, f32 hi, f32 nlo, f32 nhi) { return nlo + (v - lo) / std::max(1e-5f, hi - lo) * (nhi - nlo); }

ClTexel lerpT(const ClTexel& a, const ClTexel& b, f32 t) {
    return ClTexel{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

/// Perlin's improved-noise gradient dot product.
f32 grad(u32 h, f32 x, f32 y, f32 z) {
    const u32 h4 = h & 15u;
    const f32 u = h4 < 8u ? x : y;
    const f32 v = h4 < 4u ? y : ((h4 == 12u || h4 == 14u) ? x : z);
    return ((h4 & 1u) == 0u ? u : -u) + ((h4 & 2u) == 0u ? v : -v);
}

f32 fade(f32 t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

/// Wrapped texel index and weight along one axis (texel centres at (i + 0.5) / n).
void wrapAxis(f32 c, u32 n, u32& i0, u32& i1, f32& t) {
    const f32 f = c * static_cast<f32>(n) - 0.5f;
    const f32 fl = std::floor(f);
    t = f - fl;
    const f32 nf = static_cast<f32>(n);
    const f32 w = fl - nf * std::floor(fl / nf);
    i0 = std::min(static_cast<u32>(w), n - 1u);
    i1 = i0 + 1u == n ? 0u : i0 + 1u;
}

/// Clamped bilinear fetch of a w x h table with `stride` texels per pixel, component `k`.
ClTexel bilinearClamp(const ClTexel* table, u32 w, u32 h, u32 stride, u32 k, f32 fx, f32 fy) {
    const f32 cx = std::max(0.f, std::min(fx, static_cast<f32>(w - 1u)));
    const f32 cy = std::max(0.f, std::min(fy, static_cast<f32>(h - 1u)));
    const u32 x0 = std::min(static_cast<u32>(cx), w - 2u);
    const u32 y0 = std::min(static_cast<u32>(cy), h - 2u);
    const f32 tx = cx - static_cast<f32>(x0);
    const f32 ty = cy - static_cast<f32>(y0);
    const ClTexel a = table[(y0 * w + x0) * stride + k];
    const ClTexel b = table[(y0 * w + x0 + 1u) * stride + k];
    const ClTexel c = table[((y0 + 1u) * w + x0) * stride + k];
    const ClTexel d = table[((y0 + 1u) * w + x0 + 1u) * stride + k];
    return lerpT(lerpT(a, b, tx), lerpT(c, d, tx), ty);
}

/// Catmull-Rom (4 x 4 taps, clamped to the texture) of component k, clamped to the 2 x 2 bilinear footprint.
ClTexel catmullRom(const ClTexel* table, u32 w, u32 h, u32 stride, u32 k, f32 fx, f32 fy) {
    const f32 cx = std::max(0.f, std::min(fx, static_cast<f32>(w - 1u)));
    const f32 cy = std::max(0.f, std::min(fy, static_cast<f32>(h - 1u)));
    const f32 flx = std::floor(cx);
    const f32 fly = std::floor(cy);
    const f32 tx = cx - flx;
    const f32 ty = cy - fly;
    const int x0 = static_cast<int>(flx);
    const int y0 = static_cast<int>(fly);
    const f32 wx[4] = {tx * (-0.5f + tx * (1.f - 0.5f * tx)), 1.f + tx * tx * (-2.5f + 1.5f * tx),
                       tx * (0.5f + tx * (2.f - 1.5f * tx)), tx * tx * (-0.5f + 0.5f * tx)};
    const f32 wy[4] = {ty * (-0.5f + ty * (1.f - 0.5f * ty)), 1.f + ty * ty * (-2.5f + 1.5f * ty),
                       ty * (0.5f + ty * (2.f - 1.5f * ty)), ty * ty * (-0.5f + 0.5f * ty)};
    ClTexel sum{0.f, 0.f, 0.f, 0.f};
    ClTexel lo{1e30f, 1e30f, 1e30f, 1e30f};
    ClTexel hi{-1e30f, -1e30f, -1e30f, -1e30f};
    for (int j = 0; j < 4; ++j) {
        const u32 yy = static_cast<u32>(std::min(std::max(y0 - 1 + j, 0), static_cast<int>(h) - 1));
        ClTexel row{0.f, 0.f, 0.f, 0.f};
        for (int i = 0; i < 4; ++i) {
            const u32 xx = static_cast<u32>(std::min(std::max(x0 - 1 + i, 0), static_cast<int>(w) - 1));
            const ClTexel& t = table[(yy * w + xx) * stride + k];
            row = ClTexel{row.r + t.r * wx[i], row.g + t.g * wx[i], row.b + t.b * wx[i], row.a + t.a * wx[i]};
            if ((i == 1 || i == 2) && (j == 1 || j == 2)) {
                lo = ClTexel{std::min(lo.r, t.r), std::min(lo.g, t.g), std::min(lo.b, t.b), std::min(lo.a, t.a)};
                hi = ClTexel{std::max(hi.r, t.r), std::max(hi.g, t.g), std::max(hi.b, t.b), std::max(hi.a, t.a)};
            }
        }
        sum = ClTexel{sum.r + row.r * wy[j], sum.g + row.g * wy[j], sum.b + row.b * wy[j], sum.a + row.a * wy[j]};
    }
    return ClTexel{std::max(lo.r, std::min(sum.r, hi.r)), std::max(lo.g, std::min(sum.g, hi.g)),
                   std::max(lo.b, std::min(sum.b, hi.b)), std::max(lo.a, std::min(sum.a, hi.a))};
}

} // namespace

// --- settings --------------------------------------------------------------------------------------------------
bool cloud_settings_valid(const CloudSettings& s) {
    const CloudNoiseSettings& n = s.noise;
    const CloudResolution& r = s.resolution;
    const CloudMedium& m = s.medium;
    if (!pow2(n.shapeSize) || !pow2(n.detailSize) || !pow2(n.weatherSize) || !pow2(n.shapeFrequency) ||
        !pow2(n.detailFrequency) || !pow2(n.weatherFrequency) || n.shapeSize < 4u || n.detailSize < 4u ||
        n.weatherSize < 4u || n.shapeFrequency > n.shapeSize || n.detailFrequency > n.detailSize ||
        n.weatherFrequency > n.weatherSize) {
        return false;
    }
    if (r.block != 1u && r.block != 2u && r.block != 4u) {
        return false;
    }
    if (r.width < 2u * r.block || r.height < 2u * r.block || r.width % r.block != 0u || r.height % r.block != 0u ||
        r.outWidth < 2u || r.outHeight < 2u) {
        return false;
    }
    if (!(m.cloudBottom >= 0.f) || !(m.cloudTop > m.cloudBottom) || !(m.maxDistance > 0.f) || !(m.planetRadius > 0.f) ||
        !(m.shapeScale > 0.f) || !(m.detailScale > 0.f) || !(m.weatherScale > 0.f) || !(m.densityScale >= 0.f) ||
        !(m.albedo >= 0.f && m.albedo <= 1.f)) {
        return false;
    }
    if (s.sampling.primarySteps == 0u || s.sampling.lightSteps == 0u || !(s.sampling.lightDistance > 0.f) ||
        s.lighting.octaves == 0u || s.lighting.octaves > 8u) {
        return false;
    }
    if (!(s.temporal.maxHistoryCount >= 1.f) || !(s.temporal.motionHistoryCount >= 1.f) ||
        !(s.temporal.depthRejection >= 1.f)) {
        return false;
    }
    return true;
}

void cloud_block_offset(u32 block, u32 index, u32& x, u32& y) {
    static constexpr u8 kBayer4[16][2] = {{0, 0}, {2, 2}, {2, 0}, {0, 2}, {1, 1}, {3, 3}, {3, 1}, {1, 3},
                                          {1, 0}, {3, 2}, {3, 0}, {1, 2}, {0, 1}, {2, 3}, {2, 1}, {0, 3}};
    static constexpr u8 kBayer2[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    if (block == 4u) {
        x = kBayer4[index % 16u][0];
        y = kBayer4[index % 16u][1];
    } else if (block == 2u) {
        x = kBayer2[index % 4u][0];
        y = kBayer2[index % 4u][1];
    } else {
        x = 0;
        y = 0;
    }
}

namespace {
bool basis(const CloudCamera& c, Vec3& f, Vec3& r, Vec3& u) {
    if (!finite3(c.position) || c.forward.length() < 1e-6f || c.right.length() < 1e-6f || c.up.length() < 1e-6f ||
        !(c.tanHalfFovX > 0.f) || !(c.tanHalfFovY > 0.f)) {
        return false;
    }
    f = c.forward.normalized();
    r = c.right.normalized();
    u = c.up.normalized();
    return true;
}
} // namespace

bool resolve_cloud_params(const CloudSettings& s, const CloudFrame& frame, const CloudHistoryState& history,
                          CloudParams& out) {
    if (!cloud_settings_valid(s)) {
        return false;
    }
    Vec3 f, r, u, pf, pr, pu;
    if (!basis(frame.camera, f, r, u)) {
        return false;
    }
    const bool hasPrev = basis(history.previousCamera, pf, pr, pu);
    CloudParams p{};
    p.atmosphere = frame.atmosphere != nullptr ? frame.atmosphereAddress : 0u;
    p.background = frame.backgroundAddress;
    p.depth = frame.depthAddress;
    p.shapeSize = s.noise.shapeSize;
    p.detailSize = s.noise.detailSize;
    p.weatherSize = s.noise.weatherSize;
    p.shapeFrequency = s.noise.shapeFrequency;
    p.detailFrequency = s.noise.detailFrequency;
    p.weatherFrequency = s.noise.weatherFrequency;
    p.seed = s.noise.seed;
    u32 flags = 0;
    if (s.medium.homogeneous) {
        flags |= kClFlagHomogeneous;
    }
    if (s.temporal.jitter) {
        flags |= kClFlagJitter;
    }
    if (s.temporal.enabled && history.historyValid && hasPrev) {
        flags |= kClFlagHistory;
    }
    if (!s.temporal.reproject) {
        flags |= kClFlagNoReproject;
    }
    if (frame.atmosphere != nullptr) {
        flags |= kClFlagAtmosphere;
        if (frame.aerialPerspective) {
            flags |= kClFlagAerial;
        }
    }
    if (frame.backgroundAddress != 0u) {
        flags |= kClFlagBackground;
    }
    if (frame.depthAddress != 0u) {
        flags |= kClFlagSceneDepth;
    }
    if (frame.sunDisk) {
        flags |= kClFlagSunDisk;
    }
    p.flags = flags;
    const CloudResolution& res = s.resolution;
    p.width = res.width;
    p.height = res.height;
    p.block = res.block;
    p.freshWidth = res.width / res.block;
    p.freshHeight = res.height / res.block;
    p.frameIndex = history.frameIndex;
    cloud_block_offset(res.block, history.frameIndex, p.offsetX, p.offsetY);
    p.cycle = history.frameIndex / (res.block * res.block);
    p.outWidth = res.outWidth;
    p.outHeight = res.outHeight;
    p.primarySteps = s.sampling.primarySteps;
    p.lightSteps = s.sampling.lightSteps;
    p.octaves = s.lighting.octaves;
    const CloudMedium& m = s.medium;
    p.planetRadius = frame.atmosphere != nullptr ? frame.atmosphere->bottomRadius : m.planetRadius;
    p.cloudBottom = m.cloudBottom;
    p.cloudTop = m.cloudTop;
    p.maxDistance = m.maxDistance;
    p.shapeScale = m.shapeScale;
    p.detailScale = m.detailScale;
    p.weatherScale = m.weatherScale;
    p.coverageScale = m.coverageScale;
    p.coverageBias = m.coverageBias;
    p.densityScale = m.densityScale;
    p.detailStrength = m.detailStrength;
    p.albedo = m.albedo;
    const CloudLighting& l = s.lighting;
    p.phaseForward = l.phaseForward;
    p.phaseBackward = l.phaseBackward;
    p.phaseBlend = l.phaseBlend;
    p.msAttenuation = l.msAttenuation;
    p.msExtinction = l.msExtinction;
    p.msEccentricity = l.msEccentricity;
    p.powderStrength = l.powderStrength;
    p.lightDistance = s.sampling.lightDistance;
    p.ambientScale = l.ambientScale;
    p.ambientBottom = l.ambientBottom;
    p.transmittanceCutoff = s.sampling.transmittanceCutoff;
    p.staticThreshold = s.temporal.staticThreshold;
    p.maxHistoryCount = s.temporal.maxHistoryCount;
    p.motionHistoryCount = std::min(s.temporal.motionHistoryCount, s.temporal.maxHistoryCount);
    p.depthRejection = s.temporal.depthRejection;
    Vec3 sun{};
    Vec3 illum = frame.sunIlluminance;
    if (frame.atmosphere != nullptr) {
        sun = Vec3{frame.atmosphere->sunDir[0], frame.atmosphere->sunDir[1], frame.atmosphere->sunDir[2]};
        illum = Vec3{frame.atmosphere->sunIlluminance[0], frame.atmosphere->sunIlluminance[1],
                     frame.atmosphere->sunIlluminance[2]};
    } else {
        if (frame.sunDirection.length() < 1e-6f) {
            return false;
        }
        sun = frame.sunDirection.normalized();
    }
    set4(p.sunDir, sun, 0.f);
    set4(p.sunIlluminance, illum, 0.f);
    set4(p.ambient, frame.ambient, 0.f);
    set4(p.windOffset, frame.windOffset, 0.f);
    const Vec3 delta = hasPrev ? frame.windOffset - history.previousWind : Vec3{};
    set4(p.windDelta, delta, 0.f);
    const CloudCamera& c = frame.camera;
    set4(p.cameraPos, c.position, 0.f);
    set4(p.camForward, f, c.tanHalfFovX);
    set4(p.camRight, r, c.tanHalfFovY);
    set4(p.camUp, u, 0.f);
    const CloudCamera& pc = hasPrev ? history.previousCamera : frame.camera;
    if (!hasPrev) {
        pf = f;
        pr = r;
        pu = u;
    }
    set4(p.prevCameraPos, pc.position, 0.f);
    set4(p.prevForward, pf, pc.tanHalfFovX);
    set4(p.prevRight, pr, pc.tanHalfFovY);
    set4(p.prevUp, pu, 0.f);
    out = p;
    return true;
}

bool cloud_noise_equal(const CloudParams& a, const CloudParams& b) {
    return a.shapeSize == b.shapeSize && a.detailSize == b.detailSize && a.weatherSize == b.weatherSize &&
           a.shapeFrequency == b.shapeFrequency && a.detailFrequency == b.detailFrequency &&
           a.weatherFrequency == b.weatherFrequency && a.seed == b.seed;
}

// --- noise -------------------------------------------------------------------------------------------------------
u32 cl_hash(u32 v) {
    const u32 s = v * 747796405u + 2891336453u;
    const u32 w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

u32 cl_hash4(u32 x, u32 y, u32 z, u32 seed) { return cl_hash(x + cl_hash(y + cl_hash(z + cl_hash(seed)))); }

f32 cl_unit(u32 h) { return static_cast<f32>(h >> 8u) * (1.f / 16777216.f); }

f32 cl_worley(const Vec3& q, u32 period, u32 seed) {
    const f32 flx = std::floor(q.x);
    const f32 fly = std::floor(q.y);
    const f32 flz = std::floor(q.z);
    const f32 fx = q.x - flx;
    const f32 fy = q.y - fly;
    const f32 fz = q.z - flz;
    const u32 ix = static_cast<u32>(flx);
    const u32 iy = static_cast<u32>(fly);
    const u32 iz = static_cast<u32>(flz);
    f32 best = 8.f;
    for (u32 kz = 0; kz < 3u; ++kz) {
        for (u32 ky = 0; ky < 3u; ++ky) {
            for (u32 kx = 0; kx < 3u; ++kx) {
                const u32 cx = (ix + period - 1u + kx) % period;
                const u32 cy = (iy + period - 1u + ky) % period;
                const u32 cz = (iz + period - 1u + kz) % period;
                const u32 h = cl_hash4(cx, cy, cz, seed);
                const f32 px = (static_cast<f32>(kx) - 1.f) + cl_unit(h) - fx;
                const f32 py = (static_cast<f32>(ky) - 1.f) + cl_unit(cl_hash(h)) - fy;
                const f32 pz = (static_cast<f32>(kz) - 1.f) + cl_unit(cl_hash(h ^ 0x9e3779b9u)) - fz;
                const f32 d = px * px + py * py + pz * pz;
                best = std::min(best, d);
            }
        }
    }
    return 1.f - std::min(1.f, std::sqrt(best));
}

f32 cl_perlin(const Vec3& q, u32 period, u32 seed) {
    const f32 flx = std::floor(q.x);
    const f32 fly = std::floor(q.y);
    const f32 flz = std::floor(q.z);
    const f32 fx = q.x - flx;
    const f32 fy = q.y - fly;
    const f32 fz = q.z - flz;
    const u32 x0 = static_cast<u32>(flx) % period;
    const u32 y0 = static_cast<u32>(fly) % period;
    const u32 z0 = static_cast<u32>(flz) % period;
    const u32 x1 = (x0 + 1u) % period;
    const u32 y1 = (y0 + 1u) % period;
    const u32 z1 = (z0 + 1u) % period;
    const f32 n000 = grad(cl_hash4(x0, y0, z0, seed), fx, fy, fz);
    const f32 n100 = grad(cl_hash4(x1, y0, z0, seed), fx - 1.f, fy, fz);
    const f32 n010 = grad(cl_hash4(x0, y1, z0, seed), fx, fy - 1.f, fz);
    const f32 n110 = grad(cl_hash4(x1, y1, z0, seed), fx - 1.f, fy - 1.f, fz);
    const f32 n001 = grad(cl_hash4(x0, y0, z1, seed), fx, fy, fz - 1.f);
    const f32 n101 = grad(cl_hash4(x1, y0, z1, seed), fx - 1.f, fy, fz - 1.f);
    const f32 n011 = grad(cl_hash4(x0, y1, z1, seed), fx, fy - 1.f, fz - 1.f);
    const f32 n111 = grad(cl_hash4(x1, y1, z1, seed), fx - 1.f, fy - 1.f, fz - 1.f);
    const f32 u = fade(fx);
    const f32 v = fade(fy);
    const f32 w = fade(fz);
    const f32 a = lerp(lerp(n000, n100, u), lerp(n010, n110, u), v);
    const f32 b = lerp(lerp(n001, n101, u), lerp(n011, n111, u), v);
    return lerp(a, b, w);
}

f32 cl_perlin_fbm(const Vec3& q, u32 period, u32 seed) {
    const f32 a = cl_perlin(q, period, seed);
    const f32 b = cl_perlin(q * 2.f, period * 2u, seed + 1u);
    const f32 c = cl_perlin(q * 4.f, period * 4u, seed + 2u);
    return (a + b * 0.5f + c * 0.25f) * (1.f / 1.75f);
}

f32 cl_worley_fbm(const Vec3& q, u32 period, u32 seed) {
    const f32 a = cl_worley(q, period, seed);
    const f32 b = cl_worley(q * 2.f, period * 2u, seed + 1u);
    const f32 c = cl_worley(q * 4.f, period * 4u, seed + 2u);
    return a * 0.625f + b * 0.25f + c * 0.125f;
}

namespace {
Vec3 tileCoord(u32 x, u32 y, u32 z, u32 n) {
    const f32 inv = 1.f / static_cast<f32>(n);
    return Vec3{(static_cast<f32>(x) + 0.5f) * inv, (static_cast<f32>(y) + 0.5f) * inv, (static_cast<f32>(z) + 0.5f) * inv};
}
} // namespace

ClTexel shape_noise_texel(const CloudParams& p, u32 x, u32 y, u32 z) {
    const Vec3 t = tileCoord(x, y, z, p.shapeSize);
    const u32 F = p.shapeFrequency;
    const f32 ff = static_cast<f32>(F);
    const f32 perlin = saturate(cl_perlin_fbm(t * ff, F, p.seed) * 0.5f + 0.5f);
    const f32 worley = cl_worley_fbm(t * ff, F, p.seed + 10u);
    ClTexel o{};
    o.r = remap(perlin, 0.f, 1.f, worley, 1.f);
    o.g = cl_worley_fbm(t * (ff * 2.f), F * 2u, p.seed + 20u);
    o.b = cl_worley_fbm(t * (ff * 4.f), F * 4u, p.seed + 30u);
    o.a = cl_worley_fbm(t * (ff * 8.f), F * 8u, p.seed + 40u);
    return o;
}

ClTexel detail_noise_texel(const CloudParams& p, u32 x, u32 y, u32 z) {
    const Vec3 t = tileCoord(x, y, z, p.detailSize);
    const u32 F = p.detailFrequency;
    const f32 ff = static_cast<f32>(F);
    ClTexel o{};
    o.r = cl_worley_fbm(t * ff, F, p.seed + 50u);
    o.g = cl_worley_fbm(t * (ff * 2.f), F * 2u, p.seed + 60u);
    o.b = cl_worley_fbm(t * (ff * 4.f), F * 4u, p.seed + 70u);
    o.a = 0.f;
    return o;
}

ClTexel weather_texel(const CloudParams& p, u32 x, u32 y) {
    const f32 inv = 1.f / static_cast<f32>(p.weatherSize);
    const u32 F = p.weatherFrequency;
    const f32 ff = static_cast<f32>(F);
    const f32 u = (static_cast<f32>(x) + 0.5f) * inv * ff;
    const f32 v = (static_cast<f32>(y) + 0.5f) * inv * ff;
    const Vec3 q{u, v, 0.5f};
    const Vec3 q2{u * 2.f, v * 2.f, 0.5f};
    ClTexel o{};
    o.r = saturate(0.5f + 1.2f * cl_perlin_fbm(q, F, p.seed + 80u));
    o.g = 0.5f + 0.5f * cl_worley(q2, F * 2u, p.seed + 90u);
    o.b = saturate(0.5f + 1.5f * cl_perlin_fbm(q, F, p.seed + 100u));
    o.a = 0.f;
    return o;
}

void build_shape_noise(const CloudParams& p, std::vector<ClTexel>& out) {
    const u32 n = p.shapeSize;
    out.resize(static_cast<usize>(n) * n * n);
    for (u32 z = 0; z < n; ++z) {
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                out[(static_cast<usize>(z) * n + y) * n + x] = shape_noise_texel(p, x, y, z);
            }
        }
    }
}

void build_detail_noise(const CloudParams& p, std::vector<ClTexel>& out) {
    const u32 n = p.detailSize;
    out.resize(static_cast<usize>(n) * n * n);
    for (u32 z = 0; z < n; ++z) {
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                out[(static_cast<usize>(z) * n + y) * n + x] = detail_noise_texel(p, x, y, z);
            }
        }
    }
}

void build_weather(const CloudParams& p, std::vector<ClTexel>& out) {
    const u32 n = p.weatherSize;
    out.resize(static_cast<usize>(n) * n);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            out[static_cast<usize>(y) * n + x] = weather_texel(p, x, y);
        }
    }
}

// --- sampling ----------------------------------------------------------------------------------------------------
ClTexel cl_sample3(const ClTexel* volume, u32 n, f32 cx, f32 cy, f32 cz) {
    u32 x0, x1, y0, y1, z0, z1;
    f32 tx, ty, tz;
    wrapAxis(cx, n, x0, x1, tx);
    wrapAxis(cy, n, y0, y1, ty);
    wrapAxis(cz, n, z0, z1, tz);
    const usize s = n;
    const ClTexel& a = volume[(z0 * s + y0) * s + x0];
    const ClTexel& b = volume[(z0 * s + y0) * s + x1];
    const ClTexel& c = volume[(z0 * s + y1) * s + x0];
    const ClTexel& d = volume[(z0 * s + y1) * s + x1];
    const ClTexel& e = volume[(z1 * s + y0) * s + x0];
    const ClTexel& f = volume[(z1 * s + y0) * s + x1];
    const ClTexel& g = volume[(z1 * s + y1) * s + x0];
    const ClTexel& h = volume[(z1 * s + y1) * s + x1];
    const ClTexel lo = lerpT(lerpT(a, b, tx), lerpT(c, d, tx), ty);
    const ClTexel hi = lerpT(lerpT(e, f, tx), lerpT(g, h, tx), ty);
    return lerpT(lo, hi, tz);
}

ClTexel cl_sample2(const ClTexel* table, u32 n, f32 cx, f32 cy) {
    u32 x0, x1, y0, y1;
    f32 tx, ty;
    wrapAxis(cx, n, x0, x1, tx);
    wrapAxis(cy, n, y0, y1, ty);
    const usize s = n;
    return lerpT(lerpT(table[y0 * s + x0], table[y0 * s + x1], tx), lerpT(table[y1 * s + x0], table[y1 * s + x1], tx), ty);
}

// --- density -------------------------------------------------------------------------------------------------------
f32 cl_height_gradient(f32 h, f32 type) {
    // Trapezoids (rise start, rise end, fall start, fall end) of stratus, stratocumulus, cumulus.
    const f32 t2 = saturate(type * 2.f);
    const f32 t3 = saturate(type * 2.f - 1.f);
    const f32 a = lerp(lerp(0.00f, 0.00f, t2), 0.00f, t3);
    const f32 b = lerp(lerp(0.10f, 0.15f, t2), 0.10f, t3);
    const f32 c = lerp(lerp(0.15f, 0.40f, t2), 0.70f, t3);
    const f32 d = lerp(lerp(0.30f, 0.65f, t2), 1.00f, t3);
    return saturate((h - a) / std::max(1e-4f, b - a)) * saturate((d - h) / std::max(1e-4f, d - c));
}

f32 cl_density(const CloudParams& p, const ClNoiseView& noise, const Vec3& pos, f32 h) {
    if (h < 0.f || h > 1.f) {
        return 0.f;
    }
    if ((p.flags & kClFlagHomogeneous) != 0u) {
        return p.densityScale;
    }
    const f32 wx = pos.x - p.windOffset[0];
    const f32 wy = pos.y - p.windOffset[1];
    const f32 wz = pos.z - p.windOffset[2];
    const ClTexel W = cl_sample2(noise.weather, p.weatherSize, wx * p.weatherScale, wz * p.weatherScale);
    const f32 coverage = saturate(W.r * p.coverageScale + p.coverageBias);
    if (coverage <= 0.f) {
        return 0.f;
    }
    const ClTexel S = cl_sample3(noise.shape, p.shapeSize, wx * p.shapeScale, wy * p.shapeScale, wz * p.shapeScale);
    const f32 low = S.g * 0.625f + S.b * 0.25f + S.a * 0.125f;
    f32 base = remap(S.r, low - 1.f, 1.f, 0.f, 1.f);
    base = saturate(base * cl_height_gradient(h, W.b));
    base = saturate(remap(base, 1.f - coverage, 1.f, 0.f, 1.f)) * coverage;
    if (base <= 0.f) {
        return 0.f;
    }
    const ClTexel D = cl_sample3(noise.detail, p.detailSize, wx * p.detailScale, wy * p.detailScale, wz * p.detailScale);
    const f32 hf = D.r * 0.625f + D.g * 0.25f + D.b * 0.125f;
    const f32 mod = lerp(hf, 1.f - hf, saturate(h * 10.f));
    const f32 dens = saturate(remap(base, mod * p.detailStrength, 1.f, 0.f, 1.f));
    return dens * W.g * p.densityScale;
}

f32 cl_altitude(const CloudParams& p, const Vec3& pos, f32& r, Vec3& up) {
    const f32 R = p.planetRadius;
    const f32 yc = pos.y + R;
    const f32 rr = std::sqrt(pos.x * pos.x + yc * yc + pos.z * pos.z);
    const f32 alt = (pos.x * pos.x + pos.z * pos.z + pos.y * (pos.y + 2.f * R)) / (rr + R);
    const f32 inv = 1.f / rr;
    up = Vec3{pos.x * inv, yc * inv, pos.z * inv};
    r = R + alt;
    return alt;
}

f32 cl_density_at(const CloudParams& p, const ClNoiseView& noise, const Vec3& pos) {
    f32 r = 0.f;
    Vec3 up{};
    const f32 alt = cl_altitude(p, pos, r, up);
    const f32 h = (alt - p.cloudBottom) / (p.cloudTop - p.cloudBottom);
    return cl_density(p, noise, pos, h);
}

// --- geometry ------------------------------------------------------------------------------------------------------
bool cl_sphere(const CloudParams& p, f32 alt, f32 r, f32 mu, f32 H, f32& t0, f32& t1) {
    const f32 c = (alt - H) * (r + p.planetRadius + H);
    const f32 b = r * mu;
    const f32 disc = b * b - c;
    if (disc < 0.f) {
        return false;
    }
    const f32 s = std::sqrt(disc);
    const f32 q = b >= 0.f ? -(b + s) : s - b;
    if (q == 0.f) {
        return false;
    }
    const f32 ta = q;
    const f32 tb = c / q;
    t0 = std::min(ta, tb);
    t1 = std::max(ta, tb);
    return true;
}

f32 cl_height(const CloudParams& p, f32 alt, f32 r, f32 mu, f32 t) {
    const f32 R = p.planetRadius;
    const f32 k = t * (2.f * r * mu + t);
    const f32 numerator = alt * (r + R) + k;
    const f32 rt = std::sqrt(std::max(0.f, r * r + k));
    return numerator / (rt + R);
}

bool cl_segment(const CloudParams& p, f32 alt, f32 r, f32 mu, f32& tStart, f32& tEnd) {
    f32 a0 = 0.f, a1 = 0.f, b0 = 0.f, b1 = 0.f;
    const bool top = cl_sphere(p, alt, r, mu, p.cloudTop, a0, a1);
    const bool bottom = cl_sphere(p, alt, r, mu, p.cloudBottom, b0, b1);
    if (!top) {
        return false;
    }
    f32 start = 0.f;
    f32 end = 0.f;
    if (alt < p.cloudBottom) {
        f32 g0 = 0.f, g1 = 0.f;
        if (cl_sphere(p, alt, r, mu, 0.f, g0, g1) && g0 > 0.f) {
            return false; // the ground blocks the ray
        }
        if (!bottom) {
            return false;
        }
        start = b1;
        end = a1;
    } else if (alt <= p.cloudTop) {
        start = 0.f;
        end = (bottom && b0 > 0.f) ? b0 : a1;
    } else {
        if (a1 <= 0.f) {
            return false;
        }
        start = std::max(a0, 0.f);
        end = (bottom && b0 > 0.f) ? b0 : a1;
    }
    end = std::min(end, p.maxDistance);
    tStart = start;
    tEnd = end;
    return end > start;
}

// --- lighting and integration ----------------------------------------------------------------------------------------
f32 cl_hg(f32 cosTheta, f32 g) {
    const f32 g2 = g * g;
    const f32 base = std::max(1e-6f, 1.f + g2 - 2.f * g * cosTheta);
    return (1.f - g2) / (4.f * kPi * base * std::sqrt(base));
}

f32 cl_phase(const CloudParams& p, f32 cosTheta, f32 eccentricity) {
    return lerp(cl_hg(cosTheta, p.phaseForward * eccentricity), cl_hg(cosTheta, p.phaseBackward * eccentricity),
                p.phaseBlend);
}

f32 cl_sun_transfer(const CloudParams& p, f32 cosTheta, f32 tau) {
    const f32 powder = oneMinusExp(2.f * tau);
    const f32 pm = lerp(1.f, std::min(1.f, 2.f * powder), p.powderStrength * saturate(0.5f - 0.5f * cosTheta));
    f32 sum = 0.f;
    f32 a = 1.f;
    f32 b = 1.f;
    f32 c = 1.f;
    for (u32 n = 0; n < p.octaves; ++n) {
        sum += a * cl_phase(p, cosTheta, c) * std::exp(-b * tau);
        a *= p.msAttenuation;
        b *= p.msExtinction;
        c *= p.msEccentricity;
    }
    return sum * pm;
}

f32 cl_light_depth(const CloudParams& p, const ClNoiseView& noise, const Vec3& pos, f32 alt, const Vec3& up) {
    const Vec3 L = v3(p.sunDir);
    const f32 r = p.planetRadius + alt;
    const f32 mu = dot3(L, up);
    f32 t0 = 0.f, t1 = 0.f;
    if (!cl_sphere(p, alt, r, mu, p.cloudTop, t0, t1) || t1 <= 0.f) {
        return 0.f;
    }
    const f32 len = std::min(t1, p.lightDistance);
    const f32 ds = len / static_cast<f32>(p.lightSteps);
    const f32 span = p.cloudTop - p.cloudBottom;
    f32 tau = 0.f;
    for (u32 j = 0; j < p.lightSteps; ++j) {
        const f32 s = (static_cast<f32>(j) + 0.5f) * ds;
        const f32 h = (cl_height(p, alt, r, mu, s) - p.cloudBottom) / span;
        const Vec3 q = pos + L * s;
        tau += cl_density(p, noise, q, h) * ds;
    }
    return tau;
}

Vec3 cl_ambient_top(const CloudParams& p, const ClAtmosphereView& atm) {
    if ((p.flags & kClFlagAtmosphere) == 0u || atm.params == nullptr || atm.luts == nullptr) {
        return v3(p.ambient);
    }
    const f32 c = 0.8660254f;
    const f32 s = 0.5f;
    Vec3 sum = atmosphere::at_sky_radiance(*atm.params, *atm.luts, Vec3{0.f, 1.f, 0.f}, false);
    sum = sum + atmosphere::at_sky_radiance(*atm.params, *atm.luts, Vec3{c, s, 0.f}, false);
    sum = sum + atmosphere::at_sky_radiance(*atm.params, *atm.luts, Vec3{-c, s, 0.f}, false);
    sum = sum + atmosphere::at_sky_radiance(*atm.params, *atm.luts, Vec3{0.f, s, c}, false);
    sum = sum + atmosphere::at_sky_radiance(*atm.params, *atm.luts, Vec3{0.f, s, -c}, false);
    return sum * (0.2f * p.ambientScale);
}

ClRay cl_integrate(const CloudParams& p, const ClNoiseView& noise, const ClAtmosphereView& atm, const Vec3& origin,
                   const Vec3& dir, f32 jitter) {
    ClRay out{};
    out.depth = p.maxDistance;
    f32 r = 0.f;
    Vec3 up{};
    const f32 alt = cl_altitude(p, origin, r, up);
    const f32 mu = dot3(dir, up);
    f32 start = 0.f, end = 0.f;
    if (!cl_segment(p, alt, r, mu, start, end)) {
        return out;
    }
    const bool withAtm = (p.flags & kClFlagAtmosphere) != 0u && atm.params != nullptr && atm.luts != nullptr;
    const f32 dt = (end - start) / static_cast<f32>(p.primarySteps);
    const Vec3 sun = v3(p.sunDir);
    const f32 cosTheta = dot3(dir, sun);
    const Vec3 ambientTop = cl_ambient_top(p, atm);
    const Vec3 sunConst = v3(p.sunIlluminance);
    const f32 span = p.cloudTop - p.cloudBottom;
    Vec3 L{0.f, 0.f, 0.f};
    f32 T = 1.f;
    f32 dsum = 0.f;
    f32 wsum = 0.f;
    for (u32 i = 0; i < p.primarySteps; ++i) {
        const f32 t = start + (static_cast<f32>(i) + jitter) * dt;
        const f32 altT = cl_height(p, alt, r, mu, t);
        const f32 h = (altT - p.cloudBottom) / span;
        const Vec3 pos = origin + dir * t;
        const f32 sigma = cl_density(p, noise, pos, h);
        if (sigma <= 0.f) {
            continue;
        }
        f32 rs = 0.f;
        Vec3 ups{};
        const f32 altS = cl_altitude(p, pos, rs, ups);
        const f32 tau = cl_light_depth(p, noise, pos, altS, ups);
        const Vec3 E = withAtm ? atmosphere::at_sun_illuminance_at(*atm.params, *atm.luts, pos) : sunConst;
        const Vec3 amb = ambientTop * lerp(p.ambientBottom, 1.f, saturate(h));
        const Vec3 X = E * cl_sun_transfer(p, cosTheta, tau) + amb;
        const f32 absorbed = oneMinusExp(sigma * dt);
        L = L + X * (T * p.albedo * absorbed);
        const f32 Tn = T * (1.f - absorbed);
        dsum += (T - Tn) * t;
        wsum += T - Tn;
        T = Tn;
        if (T < p.transmittanceCutoff) {
            break;
        }
    }
    out.scatter = L;
    out.transmittance = T;
    out.hasCloud = wsum > 1e-4f ? 1.f : 0.f;
    out.depth = wsum > 1e-4f ? dsum / wsum : 0.5f * (start + end);
    return out;
}

Vec3 cl_view_dir(const CloudParams& p, f32 u, f32 v) {
    const f32 sx = (2.f * u - 1.f) * p.camForward[3];
    const f32 sy = (2.f * v - 1.f) * p.camRight[3];
    const Vec3 d = v3(p.camForward) + v3(p.camRight) * sx - v3(p.camUp) * sy;
    return d * (1.f / std::sqrt(dot3(d, d)));
}

f32 cl_jitter(const CloudParams& p, u32 x, u32 y) {
    if ((p.flags & kClFlagJitter) == 0u) {
        return 0.5f;
    }
    return fract(cl_unit(cl_hash4(x, y, 0u, p.seed ^ 0x5bd1e995u)) + static_cast<f32>(p.cycle) * 0.618034f);
}

// --- texel kernels ---------------------------------------------------------------------------------------------------
void march_texel(const CloudParams& p, const ClNoiseView& noise, const ClAtmosphereView& atm, u32 fx, u32 fy,
                 ClTexel out[2]) {
    const u32 x = fx * p.block + p.offsetX;
    const u32 y = fy * p.block + p.offsetY;
    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(p.width);
    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(p.height);
    const Vec3 dir = cl_view_dir(p, u, v);
    const ClRay ray = cl_integrate(p, noise, atm, v3(p.cameraPos), dir, cl_jitter(p, x, y));
    out[0] = ClTexel{ray.scatter.x, ray.scatter.y, ray.scatter.z, ray.transmittance};
    out[1] = ClTexel{ray.depth, ray.hasCloud, 0.f, 0.f};
}

void reconstruct_texel(const CloudParams& p, const ClTexel* fresh, const ClTexel* historyIn, u32 x, u32 y,
                       ClTexel out[2]) {
    const u32 B = p.block;
    const u32 bx = x / B;
    const u32 by = y / B;
    const ClTexel F0 = fresh[(by * p.freshWidth + bx) * 2u];
    const ClTexel F1 = fresh[(by * p.freshWidth + bx) * 2u + 1u];
    const bool isFresh = x - bx * B == p.offsetX && y - by * B == p.offsetY;
    const ClTexel fallback1{F1.r, isFresh ? 1.f : 0.f, F1.g, 0.f};
    if ((p.flags & kClFlagHistory) == 0u) {
        out[0] = F0;
        out[1] = fallback1;
        return;
    }
    const f32 W = static_cast<f32>(p.width);
    const f32 H = static_cast<f32>(p.height);
    const f32 u = (static_cast<f32>(x) + 0.5f) / W;
    const f32 v = (static_cast<f32>(y) + 0.5f) / H;
    f32 pu = u;
    f32 pv = v;
    bool inside = true;
    bool offscreen = false;
    f32 motion = 0.f;
    if ((p.flags & kClFlagNoReproject) == 0u) {
        const Vec3 dir = cl_view_dir(p, u, v);
        const Vec3 world = v3(p.cameraPos) + dir * F1.r - v3(p.windDelta);
        const Vec3 rel = world - v3(p.prevCameraPos);
        const f32 z = dot3(rel, v3(p.prevForward));
        if (z <= 1e-3f) {
            inside = false;
        } else {
            const f32 px = dot3(rel, v3(p.prevRight)) / (z * p.prevForward[3]);
            const f32 py = dot3(rel, v3(p.prevUp)) / (z * p.prevRight[3]);
            pu = 0.5f + 0.5f * px;
            pv = 0.5f - 0.5f * py;
            const f32 mx = (pu - u) * W;
            const f32 my = (pv - v) * H;
            motion = std::sqrt(mx * mx + my * my);
            // Off screen: the nearest edge history, clamped to the fresh neighbourhood below.
            offscreen = pu < 0.f || pu > 1.f || pv < 0.f || pv > 1.f;
            pu = std::max(0.f, std::min(pu, 1.f));
            pv = std::max(0.f, std::min(pv, 1.f));
        }
    }
    if (!inside) {
        out[0] = F0;
        out[1] = fallback1;
        return;
    }
    const bool moving = motion > p.staticThreshold;
    ClTexel H0{};
    ClTexel H1{};
    if (!moving) {
        H0 = historyIn[(y * p.width + x) * 2u];
        H1 = historyIn[(y * p.width + x) * 2u + 1u];
    } else {
        H0 = catmullRom(historyIn, p.width, p.height, 2u, 0u, pu * W - 0.5f, pv * H - 0.5f);
        H1 = bilinearClamp(historyIn, p.width, p.height, 2u, 1u, pu * W - 0.5f, pv * H - 0.5f);
    }
    // History rejection: a history from another depth (disocclusion, a cloud edge moving over the sky) is clamped
    // to the fresh 3 x 3 block neighbourhood; a consistent one is kept sharp.
    const f32 expected = F1.r;
    if (moving && (offscreen || std::max(expected, H1.r) > p.depthRejection * std::min(expected, H1.r))) {
        ClTexel mn = F0;
        ClTexel mx = F0;
        for (u32 k = 0; k < 9u; ++k) {
            const u32 nx = std::min(static_cast<u32>(std::max(static_cast<int>(bx) + static_cast<int>(k % 3u) - 1, 0)),
                                    p.freshWidth - 1u);
            const u32 ny = std::min(static_cast<u32>(std::max(static_cast<int>(by) + static_cast<int>(k / 3u) - 1, 0)),
                                    p.freshHeight - 1u);
            const ClTexel n = fresh[(ny * p.freshWidth + nx) * 2u];
            mn = ClTexel{std::min(mn.r, n.r), std::min(mn.g, n.g), std::min(mn.b, n.b), std::min(mn.a, n.a)};
            mx = ClTexel{std::max(mx.r, n.r), std::max(mx.g, n.g), std::max(mx.b, n.b), std::max(mx.a, n.a)};
        }
        H0 = ClTexel{std::max(mn.r, std::min(H0.r, mx.r)), std::max(mn.g, std::min(H0.g, mx.g)),
                     std::max(mn.b, std::min(H0.b, mx.b)), std::max(mn.a, std::min(H0.a, mx.a))};
    }
    f32 count = H1.g;
    if (moving) {
        count = std::min(count, p.motionHistoryCount);
    }
    if (isFresh) {
        const f32 n = std::min(count + 1.f, moving ? p.motionHistoryCount : p.maxHistoryCount);
        const f32 w = 1.f / n;
        out[0] = w >= 1.f ? F0
                          : ClTexel{H0.r + (F0.r - H0.r) * w, H0.g + (F0.g - H0.g) * w, H0.b + (F0.b - H0.b) * w,
                                    H0.a + (F0.a - H0.a) * w};
        out[1] = ClTexel{F1.r, n, F1.g, 0.f};
    } else {
        out[0] = H0;
        out[1] = ClTexel{H1.r, count, H1.b, 0.f};
    }
}

ClTexel composite_texel(const CloudParams& p, const ClTexel* history, const ClAtmosphereView& atm,
                        const ClTexel* background, const f32* depth, u32 x, u32 y) {
    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(p.outWidth);
    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(p.outHeight);
    const f32 fx = u * static_cast<f32>(p.width) - 0.5f;
    const f32 fy = v * static_cast<f32>(p.height) - 0.5f;
    const ClTexel C0 = bilinearClamp(history, p.width, p.height, 2u, 0u, fx, fy);
    const ClTexel C1 = bilinearClamp(history, p.width, p.height, 2u, 1u, fx, fy);
    const bool withAtm = (p.flags & kClFlagAtmosphere) != 0u && atm.params != nullptr && atm.luts != nullptr;
    const Vec3 dir = cl_view_dir(p, u, v);
    Vec3 bg{0.f, 0.f, 0.f};
    if ((p.flags & kClFlagBackground) != 0u && background != nullptr) {
        const ClTexel b = background[y * p.outWidth + x];
        bg = Vec3{b.r, b.g, b.b};
    } else if (withAtm) {
        bg = atmosphere::at_sky_radiance(*atm.params, *atm.luts, dir, (p.flags & kClFlagSunDisk) != 0u);
    }
    if ((p.flags & kClFlagSceneDepth) != 0u && depth != nullptr && depth[y * p.outWidth + x] < C1.r) {
        return ClTexel{bg.x, bg.y, bg.z, 1.f};
    }
    const f32 T = C0.a;
    const Vec3 L{C0.r, C0.g, C0.b};
    Vec3 col{};
    if (withAtm && (p.flags & kClFlagAerial) != 0u) {
        Vec3 s{};
        Vec3 t{};
        atmosphere::at_aerial(*atm.params, *atm.luts, u, v, C1.r, s, t);
        const Vec3 E{atm.params->sunIlluminance[0], atm.params->sunIlluminance[1], atm.params->sunIlluminance[2]};
        col = bg * T + mul3(L, t) + mul3(s, E) * (1.f - T);
    } else {
        col = bg * T + L;
    }
    return ClTexel{col.x, col.y, col.z, T};
}

} // namespace fuse::renderer::clouds
