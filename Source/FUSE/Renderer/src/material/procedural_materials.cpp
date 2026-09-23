#include <fuse/renderer/material/procedural_materials.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

using fuse::math::Vec3;

f32 fade(f32 t) {
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

f32 smoothstep(f32 e0, f32 e1, f32 x) {
    const f32 t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Dot with one of the 12 cube-edge gradients selected by the hash.
f32 gradDot(u32 h, f32 x, f32 y, f32 z) {
    switch (h % 12u) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    default: return -y - z;
    }
}

Vec3 seedOffset(u32 seed) {
    // Decorrelates materials sharing a function id without introducing any periodicity. Kept within
    // +-64 so float precision at the highest noise frequency stays well below a texel of detail.
    const u32 h0 = ProceduralNoise::hash(static_cast<i32>(seed), 17, 31, 0x9E3779B9u);
    const u32 h1 = ProceduralNoise::hash(static_cast<i32>(seed), 53, 7, 0x85EBCA6Bu);
    const u32 h2 = ProceduralNoise::hash(static_cast<i32>(seed), 3, 97, 0xC2B2AE35u);
    const auto toRange = [](u32 h) { return static_cast<f32>(h & 0xFFFFu) * (1.f / 65535.f) * 128.f - 64.f; };
    return {toRange(h0), toRange(h1), toRange(h2)};
}

Vec3 clamp01(const Vec3& v) {
    return {std::clamp(v.x, 0.f, 1.f), std::clamp(v.y, 0.f, 1.f), std::clamp(v.z, 0.f, 1.f)};
}

Vec3 mix(const Vec3& a, const Vec3& b, f32 t) {
    return a + (b - a) * t;
}

} // namespace

u32 ProceduralNoise::hash(i32 x, i32 y, i32 z, u32 seed) {
    // 32-bit avalanche hash over the lattice coordinates (lowbias32 finaliser rounds).
    u32 h = seed ^ 0x27D4EB2Fu;
    const u32 coords[3] = {static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z)};
    for (const u32 c : coords) {
        h ^= c + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
    }
    return h;
}

f32 ProceduralNoise::perlin(const Vec3& p, u32 seed) {
    const f32 fx = std::floor(p.x);
    const f32 fy = std::floor(p.y);
    const f32 fz = std::floor(p.z);
    const i32 ix = static_cast<i32>(fx);
    const i32 iy = static_cast<i32>(fy);
    const i32 iz = static_cast<i32>(fz);
    const f32 x = p.x - fx;
    const f32 y = p.y - fy;
    const f32 z = p.z - fz;

    const f32 n000 = gradDot(hash(ix, iy, iz, seed), x, y, z);
    const f32 n100 = gradDot(hash(ix + 1, iy, iz, seed), x - 1.f, y, z);
    const f32 n010 = gradDot(hash(ix, iy + 1, iz, seed), x, y - 1.f, z);
    const f32 n110 = gradDot(hash(ix + 1, iy + 1, iz, seed), x - 1.f, y - 1.f, z);
    const f32 n001 = gradDot(hash(ix, iy, iz + 1, seed), x, y, z - 1.f);
    const f32 n101 = gradDot(hash(ix + 1, iy, iz + 1, seed), x - 1.f, y, z - 1.f);
    const f32 n011 = gradDot(hash(ix, iy + 1, iz + 1, seed), x, y - 1.f, z - 1.f);
    const f32 n111 = gradDot(hash(ix + 1, iy + 1, iz + 1, seed), x - 1.f, y - 1.f, z - 1.f);

    const f32 u = fade(x);
    const f32 v = fade(y);
    const f32 w = fade(z);
    const f32 nx00 = lerp(n000, n100, u);
    const f32 nx10 = lerp(n010, n110, u);
    const f32 nx01 = lerp(n001, n101, u);
    const f32 nx11 = lerp(n011, n111, u);
    return lerp(lerp(nx00, nx10, v), lerp(nx01, nx11, v), w);
}

f32 ProceduralNoise::fbm(const Vec3& p, u32 octaves, u32 seed) {
    f32 sum = 0.f;
    f32 amplitude = 0.5f;
    f32 norm = 0.f;
    Vec3 q = p;
    for (u32 i = 0; i < octaves; ++i) {
        sum += amplitude * perlin(q, seed + i * 0x632BE5ABu);
        norm += amplitude;
        amplitude *= 0.5f;
        q = q * 2.f;
    }
    return norm > 0.f ? sum / norm : 0.f;
}

MaterialSample ProceduralMaterials::wood(const Vec3& worldPos, u32 seed) {
    const Vec3 p = worldPos + seedOffset(seed);
    // Concentric growth rings around the Y axis, warped by low-frequency noise. sin() (not fract())
    // keeps the ring profile continuous — no hard ring edges.
    const f32 warp = ProceduralNoise::perlin(p * 0.3f, seed) * 1.5f;
    const f32 radius = std::sqrt(p.x * p.x + p.z * p.z) + warp;
    const f32 ring = std::sin(radius * 6.2831853f * 4.f) * 0.5f + 0.5f;
    const f32 grain = ProceduralNoise::fbm({p.x * 10.f, p.y * 1.5f, p.z * 10.f}, 4u, seed ^ 0xA511E9B3u);

    MaterialSample s{};
    s.albedo = clamp01(Vec3{0.6f + ring * 0.3f, 0.35f + ring * 0.15f, 0.1f + ring * 0.05f} * (1.f + grain * 0.15f));
    s.roughness = std::clamp(0.6f + grain * 0.1f, 0.05f, 1.f);
    s.metallic = 0.f;
    s.normalOffset = {grain * 0.05f, 0.f, 0.f};
    return s;
}

MaterialSample ProceduralMaterials::metal(const Vec3& worldPos, u32 seed) {
    const Vec3 p = worldPos + seedOffset(seed);
    // Brushed metal: streaks stretched along X, plus broad smudges.
    const f32 streak = ProceduralNoise::fbm({p.x * 2.f, p.y * 60.f, p.z * 60.f}, 3u, seed);
    const f32 smudge = ProceduralNoise::fbm(p * 1.5f, 3u, seed ^ 0x5BD1E995u);

    MaterialSample s{};
    const Vec3 base{0.91f, 0.92f, 0.92f};
    s.albedo = clamp01(base * (1.f + streak * 0.06f + smudge * 0.04f));
    s.roughness = std::clamp(0.25f + streak * 0.08f + smudge * 0.1f, 0.05f, 1.f);
    s.metallic = 1.f;
    s.normalOffset = {0.f, streak * 0.03f, 0.f};
    return s;
}

MaterialSample ProceduralMaterials::concrete(const Vec3& worldPos, u32 seed) {
    const Vec3 p = worldPos + seedOffset(seed);
    const f32 base = ProceduralNoise::fbm(p * 4.f, 5u, seed);
    const f32 stain = ProceduralNoise::fbm(p * 0.5f, 3u, seed ^ 0x1B873593u);
    // Pores: smooth threshold of high-frequency noise (smoothstep keeps it continuous).
    const f32 pores = smoothstep(0.25f, 0.45f, ProceduralNoise::perlin(p * 40.f, seed ^ 0xCC9E2D51u));

    MaterialSample s{};
    const f32 grey = 0.5f + base * 0.1f + stain * 0.08f;
    s.albedo = clamp01(mix(Vec3{grey, grey * 0.98f, grey * 0.95f}, Vec3{0.2f, 0.2f, 0.19f}, pores * 0.6f));
    s.roughness = std::clamp(0.88f + base * 0.06f + pores * 0.05f, 0.05f, 1.f);
    s.metallic = 0.f;
    s.normalOffset = {base * 0.1f, pores * -0.1f, 0.f};
    return s;
}

MaterialSample ProceduralMaterials::evaluate(ProceduralMaterialId id, const Vec3& worldPos, u32 seed) {
    switch (id) {
    case ProceduralMaterialId::Wood:
        return wood(worldPos, seed);
    case ProceduralMaterialId::Metal:
        return metal(worldPos, seed);
    case ProceduralMaterialId::Concrete:
        return concrete(worldPos, seed);
    default:
        return MaterialSample{};
    }
}

} // namespace fuse::renderer
