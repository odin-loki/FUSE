#pragma once

// Asset plan W0.7: the layered-material evaluation, CPU reference. shaders/material_layers/ml_common.{glsl,slang}
// are line-by-line twins (same operations, same order, f32); keep the three in sync.
//
//   surface = base set (UV0 or triplanar, optionally stochastic) x macro variation
//           + up to 3 layers (height-blended material layers or wetness), masks from vertex colour / slope / height
//           + detail albedo / normal (detailScale x frequency, faded with the view distance)
//
// Stochastic tiling: the triangle-grid ("hex tile") variant of Heitz & Neyret 2018, "High-Performance By-Example
// Noise using a Histogram-Preserving Blending Operator" (HPG 2018), with the variance-preserving linear blend of
// Burley 2019, "On Histogram-Preserving Blending for Randomized Texture Tiling" (JCGT 8(4)): three random
// translations of the texture on a skewed triangle grid, blended mean + sum w_i (s_i - mean) / sqrt(sum w_i^2), which
// keeps the mean and the variance of the texture (not the full histogram: there is no Gaussianisation LUT).
// Translations only (no rotation), so tangent-space normals stay valid.
//
// Triplanar: projections P.zy / P.xz / P.xy weighted by |N|^sharpness, normals by the whiteout triplanar blend
// (B. Golus, "Normal Mapping for a Triplanar Shader", 2017). Every term is a continuous function of (P, N), so a
// surface with a continuous normal (a bevelled / rounded cube edge) shades continuously.
//
// Texture filtering: MlView::filter (null = the texel pool's wrap-around bilinear, what materials.eval / .balls run).
// The WP-1.5 resolve's layered bin samples mip-mapped bindless images with textureGrad instead; every tap gets the
// screen-space derivatives of its projection coordinate (MlGrad: UV0's, or the world position's for triplanar, times
// the scale; the stochastic offsets are constant, so all three taps share the footprint), and its CPU reference
// installs ml_mips.hpp's trilinear filter here. The pool filter ignores the derivatives (results unchanged).
//
// Height blend of a layer: t = saturate((hLayer - hBase + 2 (2 m - 1)) * contrast / 2 + 1/2) for the mask m in [0, 1]
// and contrast >= 1: t = 0 at m = 0, 1 at m = 1, and in between the higher of the two height maps wins.

#include <fuse/renderer/material_layers/ml_types.hpp>

#include <cmath>

namespace fuse::renderer::material_layers {

struct MlF2 {
    f32 x = 0.f, y = 0.f;
};
struct MlF3 {
    f32 x = 0.f, y = 0.f, z = 0.f;
};
struct MlF4 {
    f32 x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

/// Texture filter of an evaluation (CPU only): `texture` is the MlTexture index, `uv` the (unwrapped) coordinate and
/// `dx` / `dy` its screen-space derivatives (textureGrad's). Returns decoded RGBA.
using MlFilterFn = MlF4 (*)(const void* user, u32 texture, MlF2 uv, MlF2 dx, MlF2 dy);

/// The library the kernels read (CPU side of MlParams' addresses).
struct MlView {
    const MlMaterial* materials = nullptr;
    u32 materialCount = 0;
    const MlTexture* textures = nullptr;
    u32 textureCount = 0;
    const u32* texels = nullptr;
    const f32* lut = nullptr; ///< kMlLutEntries
    /// Null: the texel pool's wrap-around bilinear filter (ml_bilinear, what materials.eval / materials.balls run).
    /// The WP-1.5 layered resolve bin samples mip-mapped bindless images with textureGrad instead; its CPU reference
    /// installs the trilinear mip-chain filter of ml_mips.hpp here (the shaders: ML_BINDLESS_TEXTURES).
    MlFilterFn filter = nullptr;
    const void* filterUser = nullptr;
};

/// Screen-space derivatives of a surface point (per pixel): UV0 and world position. They only steer texture
/// filtering (the projection coordinate's derivatives go to the filter); the pool filter ignores them.
struct MlGrad {
    MlF2 duvdx;
    MlF2 duvdy;
    MlF3 dPdx;
    MlF3 dPdy;
};

/// Result of sampling one texture set in the material's projection.
struct MlSetSample {
    MlF3 albedo;
    f32 height = 0.5f;
    f32 roughness = 1.f;
    f32 ao = 1.f;
    MlF3 normal; ///< world, normalised
};

/// Surface frame of an evaluation.
struct MlFrame {
    MlF3 P;
    MlF3 N; ///< geometric, normalised
    MlF3 T; ///< orthonormalised tangent
    MlF3 B;
    MlF2 uv;
    MlF2 duvdx; ///< derivatives (MlGrad)
    MlF2 duvdy;
    MlF3 dPdx;
    MlF3 dPdy;
    bool triplanar = false;
    bool stochastic = false;
    f32 sharpness = 4.f;
    f32 lattice = 2.f;
};

// --- scalar helpers ------------------------------------------------------------------------------------------------
inline f32 ml_saturate(f32 x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline f32 ml_lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
inline MlF3 ml_add(MlF3 a, MlF3 b) { return MlF3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline MlF3 ml_sub(MlF3 a, MlF3 b) { return MlF3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline MlF3 ml_mul(MlF3 a, f32 s) { return MlF3{a.x * s, a.y * s, a.z * s}; }
inline MlF3 ml_mul3(MlF3 a, MlF3 b) { return MlF3{a.x * b.x, a.y * b.y, a.z * b.z}; }
inline MlF3 ml_lerp3(MlF3 a, MlF3 b, f32 t) {
    return MlF3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
inline f32 ml_dot(MlF3 a, MlF3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline MlF3 ml_cross(MlF3 a, MlF3 b) {
    return MlF3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
/// Normalised `v`, or `fallback` for a zero vector.
inline MlF3 ml_normalize_or(MlF3 v, MlF3 fallback) {
    const f32 l = ml_dot(v, v);
    if (l > 0.f) {
        const f32 inv = 1.f / std::sqrt(l);
        return MlF3{v.x * inv, v.y * inv, v.z * inv};
    }
    return fallback;
}

/// lowbias32 (C. Wellons), integer only: bit-identical on the CPU and the GPU.
inline u32 ml_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
/// [0, 1) from the top 24 bits (exact in f32).
inline f32 ml_unit(u32 h) { return static_cast<f32>(h >> 8) * (1.f / 16777216.f); }

/// 3D value noise in [0, 1] (smoothstep-interpolated lattice hashes).
inline f32 ml_value_noise(MlF3 p, u32 seed) {
    const f32 fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const f32 tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const f32 ux = tx * tx * (3.f - 2.f * tx);
    const f32 uy = ty * ty * (3.f - 2.f * ty);
    const f32 uz = tz * tz * (3.f - 2.f * tz);
    const u32 ix = static_cast<u32>(static_cast<i32>(fx));
    const u32 iy = static_cast<u32>(static_cast<i32>(fy));
    const u32 iz = static_cast<u32>(static_cast<i32>(fz));
    f32 c[8];
    for (u32 i = 0; i < 8u; ++i) {
        const u32 h = ml_hash(((ix + (i & 1u)) * 0x8da6b343u) ^ ((iy + ((i >> 1) & 1u)) * 0xd8163841u) ^
                              ((iz + (i >> 2)) * 0xcb1ab31fu) ^ seed);
        c[i] = ml_unit(h);
    }
    const f32 x00 = ml_lerp(c[0], c[1], ux);
    const f32 x10 = ml_lerp(c[2], c[3], ux);
    const f32 x01 = ml_lerp(c[4], c[5], ux);
    const f32 x11 = ml_lerp(c[6], c[7], ux);
    return ml_lerp(ml_lerp(x00, x10, uy), ml_lerp(x01, x11, uy), uz);
}

// --- textures ------------------------------------------------------------------------------------------------------
inline MlF4 ml_texel(const MlView& v, const MlTexture& t, u32 x, u32 y) {
    const u32 w = v.texels[t.offset + y * t.width + x];
    const u32 rgbBase = (t.flags & kMlTexSrgb) != 0u ? 0u : 256u;
    return MlF4{v.lut[rgbBase + (w & 255u)], v.lut[rgbBase + ((w >> 8) & 255u)], v.lut[rgbBase + ((w >> 16) & 255u)],
                v.lut[256u + (w >> 24)]};
}

/// Wrap-around bilinear filter, texel centres at (i + 0.5) / size.
inline MlF4 ml_bilinear(const MlView& v, const MlTexture& t, MlF2 uv) {
    const f32 W = static_cast<f32>(t.width);
    const f32 H = static_cast<f32>(t.height);
    const f32 x = (uv.x - std::floor(uv.x)) * W - 0.5f;
    const f32 y = (uv.y - std::floor(uv.y)) * H - 0.5f;
    const f32 x0 = std::floor(x);
    const f32 y0 = std::floor(y);
    const f32 fx = x - x0;
    const f32 fy = y - y0;
    const u32 ix0 = x0 < 0.f ? t.width - 1u : static_cast<u32>(x0);
    const u32 iy0 = y0 < 0.f ? t.height - 1u : static_cast<u32>(y0);
    const u32 ix1 = ix0 + 1u >= t.width ? 0u : ix0 + 1u;
    const u32 iy1 = iy0 + 1u >= t.height ? 0u : iy0 + 1u;
    const MlF4 a = ml_texel(v, t, ix0, iy0);
    const MlF4 b = ml_texel(v, t, ix1, iy0);
    const MlF4 c = ml_texel(v, t, ix0, iy1);
    const MlF4 d = ml_texel(v, t, ix1, iy1);
    const f32 rx = ml_lerp(ml_lerp(a.x, b.x, fx), ml_lerp(c.x, d.x, fx), fy);
    const f32 ry = ml_lerp(ml_lerp(a.y, b.y, fx), ml_lerp(c.y, d.y, fx), fy);
    const f32 rz = ml_lerp(ml_lerp(a.z, b.z, fx), ml_lerp(c.z, d.z, fx), fy);
    const f32 rw = ml_lerp(ml_lerp(a.w, b.w, fx), ml_lerp(c.w, d.w, fx), fy);
    return MlF4{rx, ry, rz, rw};
}

/// Stochastic-tiling lattice of `uv`: the three triangle-grid vertices around it, their weights and random offsets.
struct MlStochastic {
    f32 w[3] = {};
    MlF2 offset[3];
};

inline MlStochastic ml_stochastic_lattice(MlF2 uv, f32 lattice, u32 seed) {
    const f32 sx = uv.x * lattice;
    const f32 sy = uv.y * lattice;
    const f32 kx = sx - 0.57735027f * sy;
    const f32 ky = 1.15470054f * sy;
    const f32 bx = std::floor(kx);
    const f32 by = std::floor(ky);
    const f32 fx = kx - bx;
    const f32 fy = ky - by;
    const f32 z = 1.f - fx - fy;
    const i32 ibx = static_cast<i32>(bx);
    const i32 iby = static_cast<i32>(by);
    i32 vx[3];
    i32 vy[3];
    MlStochastic s{};
    if (z > 0.f) {
        s.w[0] = z;
        s.w[1] = fy;
        s.w[2] = fx;
        vx[0] = ibx;
        vy[0] = iby;
        vx[1] = ibx;
        vy[1] = iby + 1;
        vx[2] = ibx + 1;
        vy[2] = iby;
    } else {
        s.w[0] = -z;
        s.w[1] = 1.f - fy;
        s.w[2] = 1.f - fx;
        vx[0] = ibx + 1;
        vy[0] = iby + 1;
        vx[1] = ibx + 1;
        vy[1] = iby;
        vx[2] = ibx;
        vy[2] = iby + 1;
    }
    for (u32 i = 0; i < 3u; ++i) {
        const u32 h = ml_hash((static_cast<u32>(vx[i]) * 0x27d4eb2fu) ^ (static_cast<u32>(vy[i]) * 0x165667b1u) ^ seed);
        s.offset[i] = MlF2{ml_unit(h), ml_unit(ml_hash(h ^ 0x9e3779b9u))};
    }
    return s;
}

/// One filtered sample of texture `tex`: the view's filter hook, else the pool's bilinear filter.
inline MlF4 ml_filter(const MlView& v, u32 tex, const MlTexture& t, MlF2 uv, MlF2 dx, MlF2 dy) {
    if (v.filter != nullptr) {
        return v.filter(v.filterUser, tex, uv, dx, dy);
    }
    return ml_bilinear(v, t, uv);
}

/// One texture of a set: plain wrap-around bilinear, or the three stochastic taps blended with the variance-
/// preserving operator. `fallback` for kMlNoTexture. `dx` / `dy`: derivatives of `uv` (the stochastic offsets are
/// constant per lattice vertex, so every tap has the same footprint).
inline MlF4 ml_sample_tex(const MlView& v, u32 tex, MlF2 uv, MlF2 dx, MlF2 dy, bool stochastic, const MlStochastic& s,
                          MlF4 fallback) {
    if (tex == kMlNoTexture || tex >= v.textureCount) {
        return fallback;
    }
    const MlTexture& t = v.textures[tex];
    if (!stochastic) {
        return ml_filter(v, tex, t, uv, dx, dy);
    }
    const MlF4 a = ml_filter(v, tex, t, MlF2{uv.x + s.offset[0].x, uv.y + s.offset[0].y}, dx, dy);
    const MlF4 b = ml_filter(v, tex, t, MlF2{uv.x + s.offset[1].x, uv.y + s.offset[1].y}, dx, dy);
    const MlF4 c = ml_filter(v, tex, t, MlF2{uv.x + s.offset[2].x, uv.y + s.offset[2].y}, dx, dy);
    const f32 inv = 1.f / std::sqrt(s.w[0] * s.w[0] + s.w[1] * s.w[1] + s.w[2] * s.w[2]);
    const f32 rx = t.mean[0] + (s.w[0] * (a.x - t.mean[0]) + s.w[1] * (b.x - t.mean[0]) + s.w[2] * (c.x - t.mean[0])) * inv;
    const f32 ry = t.mean[1] + (s.w[0] * (a.y - t.mean[1]) + s.w[1] * (b.y - t.mean[1]) + s.w[2] * (c.y - t.mean[1])) * inv;
    const f32 rz = t.mean[2] + (s.w[0] * (a.z - t.mean[2]) + s.w[1] * (b.z - t.mean[2]) + s.w[2] * (c.z - t.mean[2])) * inv;
    const f32 rw = t.mean[3] + (s.w[0] * (a.w - t.mean[3]) + s.w[1] * (b.w - t.mean[3]) + s.w[2] * (c.w - t.mean[3])) * inv;
    return MlF4{ml_saturate(rx), ml_saturate(ry), ml_saturate(rz), ml_saturate(rw)};
}

/// Tangent-space normal of a normal-set texel (unorm xy, z reconstructed), xy scaled by `strength`.
inline MlF3 ml_tangent_normal(MlF4 n, f32 strength) {
    const f32 x = n.x * 2.f - 1.f;
    const f32 y = n.y * 2.f - 1.f;
    const f32 z = std::sqrt(std::fmax(1.f - x * x - y * y, 0.f));
    return MlF3{x * strength, y * strength, z};
}

/// One projection tap of a texture set: the albedo texel (rgb, height) and the normal texel.
struct MlTap {
    MlF4 a;
    MlF4 n;
};

inline MlTap ml_tap(const MlView& v, u32 albedoTex, u32 normalTex, MlF2 uv, MlF2 dx, MlF2 dy, const MlFrame& f,
                    u32 seed) {
    MlStochastic s{};
    if (f.stochastic) {
        s = ml_stochastic_lattice(uv, f.lattice, seed);
    }
    MlTap t{};
    t.a = ml_sample_tex(v, albedoTex, uv, dx, dy, f.stochastic, s, MlF4{1.f, 1.f, 1.f, 0.5f});
    t.n = ml_sample_tex(v, normalTex, uv, dx, dy, f.stochastic, s, MlF4{0.5f, 0.5f, 1.f, 1.f});
    return t;
}

/// Samples a texture set in the frame's projection at `scale` x (UV0 or P).
inline MlSetSample ml_sample_set(const MlView& v, u32 albedoTex, u32 normalTex, f32 scale, f32 strength, u32 seed,
                                 const MlFrame& f) {
    MlSetSample r{};
    if (!f.triplanar) {
        const MlTap t = ml_tap(v, albedoTex, normalTex, MlF2{f.uv.x * scale, f.uv.y * scale},
                               MlF2{f.duvdx.x * scale, f.duvdx.y * scale}, MlF2{f.duvdy.x * scale, f.duvdy.y * scale}, f,
                               seed);
        r.albedo = MlF3{t.a.x, t.a.y, t.a.z};
        r.height = t.a.w;
        r.roughness = t.n.z;
        r.ao = t.n.w;
        const MlF3 tn = ml_tangent_normal(t.n, strength);
        const MlF3 w = ml_add(ml_add(ml_mul(f.T, tn.x), ml_mul(f.B, tn.y)), ml_mul(f.N, tn.z));
        r.normal = ml_normalize_or(w, f.N);
        return r;
    }
    const f32 ax = std::pow(std::fabs(f.N.x), f.sharpness);
    const f32 ay = std::pow(std::fabs(f.N.y), f.sharpness);
    const f32 az = std::pow(std::fabs(f.N.z), f.sharpness);
    const f32 inv = 1.f / (ax + ay + az);
    const f32 wx = ax * inv, wy = ay * inv, wz = az * inv;
    const MlTap tx = ml_tap(v, albedoTex, normalTex, MlF2{f.P.z * scale, f.P.y * scale},
                            MlF2{f.dPdx.z * scale, f.dPdx.y * scale}, MlF2{f.dPdy.z * scale, f.dPdy.y * scale}, f, seed);
    const MlTap ty = ml_tap(v, albedoTex, normalTex, MlF2{f.P.x * scale, f.P.z * scale},
                            MlF2{f.dPdx.x * scale, f.dPdx.z * scale}, MlF2{f.dPdy.x * scale, f.dPdy.z * scale}, f,
                            seed ^ 0x51ed27u);
    const MlTap tz = ml_tap(v, albedoTex, normalTex, MlF2{f.P.x * scale, f.P.y * scale},
                            MlF2{f.dPdx.x * scale, f.dPdx.y * scale}, MlF2{f.dPdy.x * scale, f.dPdy.y * scale}, f,
                            seed ^ 0xa3b195u);
    r.albedo = MlF3{tx.a.x * wx + ty.a.x * wy + tz.a.x * wz, tx.a.y * wx + ty.a.y * wy + tz.a.y * wz,
                    tx.a.z * wx + ty.a.z * wy + tz.a.z * wz};
    r.height = tx.a.w * wx + ty.a.w * wy + tz.a.w * wz;
    r.roughness = tx.n.z * wx + ty.n.z * wy + tz.n.z * wz;
    r.ao = tx.n.w * wx + ty.n.w * wy + tz.n.w * wz;
    // Whiteout triplanar: each projection's tangent normal swizzled into world space around the geometric normal.
    const MlF3 nx = ml_tangent_normal(tx.n, strength);
    const MlF3 ny = ml_tangent_normal(ty.n, strength);
    const MlF3 nz = ml_tangent_normal(tz.n, strength);
    const MlF3 px{nx.x + f.N.z, nx.y + f.N.y, std::fabs(nx.z) * f.N.x}; // world = px.zyx
    const MlF3 py{ny.x + f.N.x, ny.y + f.N.z, std::fabs(ny.z) * f.N.y}; // world = py.xzy
    const MlF3 pz{nz.x + f.N.x, nz.y + f.N.y, std::fabs(nz.z) * f.N.z}; // world = pz.xyz
    const MlF3 w{px.z * wx + py.x * wy + pz.x * wz, px.y * wx + py.z * wy + pz.y * wz,
                 px.x * wx + py.y * wy + pz.z * wz};
    r.normal = ml_normalize_or(w, f.N);
    return r;
}

inline f32 ml_layer_mask(const MlLayer& l, const MlSurface& s, const MlFrame& f) {
    f32 m = 1.f;
    switch (l.mask) {
    case kMlMaskVertexR: m = s.color[0]; break;
    case kMlMaskVertexG: m = s.color[1]; break;
    case kMlMaskVertexB: m = s.color[2]; break;
    case kMlMaskVertexA: m = s.color[3]; break;
    case kMlMaskSlopeUp: m = (f.N.y - l.maskBias) * l.maskScale; break;
    case kMlMaskWorldHeight: m = (f.P.y - l.maskBias) * l.maskScale; break;
    default: m = 1.f; break;
    }
    return ml_saturate(ml_saturate(m) * l.coverage);
}

inline f32 ml_height_blend(f32 hBase, f32 hLayer, f32 m, f32 contrast) {
    return ml_saturate((hLayer - hBase + 2.f * (2.f * m - 1.f)) * contrast * 0.5f + 0.5f);
}

/// Evaluates the layered material of `s` (an out-of-range material id gives the default surface). `g`: the point's
/// screen-space derivatives (texture footprints; ignored by the pool filter).
inline MlResult ml_evaluate(const MlView& v, const MlSurface& s, const MlGrad& g) {
    MlFrame f{};
    f.duvdx = g.duvdx;
    f.duvdy = g.duvdy;
    f.dPdx = g.dPdx;
    f.dPdy = g.dPdy;
    f.P = MlF3{s.position[0], s.position[1], s.position[2]};
    f.N = ml_normalize_or(MlF3{s.normal[0], s.normal[1], s.normal[2]}, MlF3{0.f, 1.f, 0.f});
    const MlF3 t0{s.tangent[0], s.tangent[1], s.tangent[2]};
    const MlF3 tp = ml_sub(t0, ml_mul(f.N, ml_dot(f.N, t0)));
    const MlF3 helper = std::fabs(f.N.y) < 0.999f ? MlF3{0.f, 1.f, 0.f} : MlF3{1.f, 0.f, 0.f};
    f.T = ml_normalize_or(tp, ml_normalize_or(ml_cross(helper, f.N), MlF3{1.f, 0.f, 0.f}));
    f.B = ml_mul(ml_cross(f.N, f.T), s.tangentSign < 0.f ? -1.f : 1.f);
    f.uv = MlF2{s.uv[0], s.uv[1]};
    MlResult r{};
    if (s.material >= v.materialCount) {
        r.albedo[0] = r.albedo[1] = r.albedo[2] = 1.f;
        r.roughness = 0.5f;
        r.normal[0] = f.N.x;
        r.normal[1] = f.N.y;
        r.normal[2] = f.N.z;
        r.ao = 1.f;
        r.height = 0.5f;
        return r;
    }
    const MlMaterial& m = v.materials[s.material];
    f.triplanar = (m.flags & kMlFlagTriplanar) != 0u;
    f.stochastic = (m.flags & kMlFlagStochastic) != 0u;
    f.sharpness = m.triplanarSharpness;
    f.lattice = m.stochasticLattice;

    const MlSetSample base = ml_sample_set(v, m.albedoTex, m.normalTex, m.uvScale, m.normalStrength, 0x1234567u, f);
    MlF3 albedo = ml_mul3(MlF3{m.albedo[0], m.albedo[1], m.albedo[2]}, base.albedo);
    f32 height = base.height;
    f32 roughness = m.roughness * base.roughness;
    f32 metallic = m.metallic;
    f32 ao = base.ao;
    MlF3 N = base.normal;
    if ((m.flags & kMlFlagMacro) != 0u) {
        const f32 n = ml_value_noise(ml_mul(f.P, m.macroScale), 0x2545f491u);
        albedo = ml_mul(albedo, 1.f + m.macroStrength * (n * 2.f - 1.f));
    }
    const u32 layers = m.layerCount < kMlMaxLayers ? m.layerCount : kMlMaxLayers;
    for (u32 i = 0; i < layers; ++i) {
        const MlLayer& l = m.layers[i];
        const f32 mask = ml_layer_mask(l, s, f);
        if (l.mode == kMlLayerWet) {
            const f32 t = ml_saturate(mask * 2.f - height);
            const MlF3 dark{1.f + (l.albedo[0] - 1.f) * t * (1.f - metallic), 1.f + (l.albedo[1] - 1.f) * t * (1.f - metallic),
                            1.f + (l.albedo[2] - 1.f) * t * (1.f - metallic)};
            albedo = ml_mul3(albedo, dark);
            roughness = ml_lerp(roughness, l.roughness, t);
            N = ml_normalize_or(ml_lerp3(N, f.N, t), f.N);
            continue;
        }
        const MlSetSample ls = ml_sample_set(v, l.albedoTex, l.normalTex, m.uvScale * l.uvScale, l.normalStrength,
                                             0x68e31da4u + i * 0x1b56c4e9u, f);
        const f32 t = ml_height_blend(height, ls.height, mask, l.contrast);
        albedo = ml_lerp3(albedo, ml_mul3(MlF3{l.albedo[0], l.albedo[1], l.albedo[2]}, ls.albedo), t);
        roughness = ml_lerp(roughness, l.roughness * ls.roughness, t);
        metallic = ml_lerp(metallic, l.metallic, t);
        ao = ml_lerp(ao, ls.ao, t);
        N = ml_normalize_or(ml_lerp3(N, ls.normal, t), f.N);
        height = ml_lerp(height, ls.height, t);
    }
    if ((m.flags & kMlFlagDetail) != 0u) {
        const f32 range = m.detailFadeEnd - m.detailFadeStart;
        const f32 fade = 1.f - ml_saturate(range > 0.f ? (s.viewDistance - m.detailFadeStart) / range : 0.f);
        const f32 k = m.detailStrength * fade;
        if (k > 0.f) {
            const MlSetSample d = ml_sample_set(v, m.detailAlbedoTex, m.detailNormalTex, m.uvScale * m.detailScale, 1.f,
                                                0x3c6ef372u, f);
            albedo = MlF3{albedo.x * std::fmax(1.f + k * (d.albedo.x * 2.f - 1.f), 0.f),
                          albedo.y * std::fmax(1.f + k * (d.albedo.y * 2.f - 1.f), 0.f),
                          albedo.z * std::fmax(1.f + k * (d.albedo.z * 2.f - 1.f), 0.f)};
            N = ml_normalize_or(ml_add(N, ml_mul(ml_sub(d.normal, f.N), k)), f.N);
        }
    }
    r.albedo[0] = std::fmax(albedo.x, 0.f);
    r.albedo[1] = std::fmax(albedo.y, 0.f);
    r.albedo[2] = std::fmax(albedo.z, 0.f);
    r.roughness = ml_saturate(roughness);
    r.normal[0] = N.x;
    r.normal[1] = N.y;
    r.normal[2] = N.z;
    r.metallic = ml_saturate(metallic);
    r.ao = ml_saturate(ao);
    r.height = height;
    return r;
}

/// ml_evaluate without derivatives (the texel-pool path: materials.eval / materials.balls).
inline MlResult ml_evaluate(const MlView& v, const MlSurface& s) { return ml_evaluate(v, s, MlGrad{}); }

// --- golden material-ball scene --------------------------------------------------------------------------------------
/// Nearest ball hit along the ray (o, d); false for none. `t` the distance, `ball` its index.
inline bool ml_trace_balls(const MlBall* balls, u32 count, MlF3 o, MlF3 d, f32& tHit, u32& ball) {
    tHit = 3.0e38f;
    bool hit = false;
    for (u32 i = 0; i < count; ++i) {
        const MlF3 c{balls[i].center[0], balls[i].center[1], balls[i].center[2]};
        const MlF3 oc = ml_sub(o, c);
        const f32 b = ml_dot(oc, d);
        const f32 cc = ml_dot(oc, oc) - balls[i].radius * balls[i].radius;
        const f32 disc = b * b - cc;
        if (disc < 0.f) {
            continue;
        }
        const f32 t = -b - std::sqrt(disc);
        if (t > 0.f && t < tHit) {
            tHit = t;
            ball = i;
            hit = true;
        }
    }
    return hit;
}

/// Surface record of a ball hit: spherical UV (u around +Y, twice as many repeats around as pole to pole), tangent
/// along +phi, vertex colour R = world value noise (moss mask), B = lower hemisphere (wetness).
inline MlSurface ml_ball_surface(const MlBall& b, MlF3 P, f32 dist) {
    MlSurface s{};
    const MlF3 c{b.center[0], b.center[1], b.center[2]};
    const MlF3 p = ml_mul(ml_sub(P, c), 1.f / b.radius);
    s.position[0] = P.x;
    s.position[1] = P.y;
    s.position[2] = P.z;
    s.viewDistance = dist;
    s.normal[0] = p.x;
    s.normal[1] = p.y;
    s.normal[2] = p.z;
    s.tangent[0] = p.z;
    s.tangent[1] = 0.f;
    s.tangent[2] = -p.x;
    s.tangentSign = 1.f;
    s.material = b.material;
    const f32 phi = std::atan2(p.x, p.z);
    const f32 theta = std::acos(p.y < -1.f ? -1.f : (p.y > 1.f ? 1.f : p.y));
    s.uv[0] = (phi * 0.15915494f + 0.5f) * 2.f;
    s.uv[1] = theta * 0.31830989f;
    s.color[0] = ml_saturate(ml_value_noise(ml_mul(P, 3.f), 0x7f4a7c15u) * 1.6f - 0.3f);
    s.color[1] = 1.f;
    s.color[2] = ml_saturate(0.35f - p.y * 0.8f);
    s.color[3] = 1.f;
    return s;
}

/// Sun (GGX + Lambert) and hemisphere ambient, exposure, x / (1 + x) tone map. Linear output.
inline MlF3 ml_shade(const MlParams& P, const MlResult& r, MlF3 V) {
    const MlF3 N{r.normal[0], r.normal[1], r.normal[2]};
    const MlF3 L{P.sunDir[0], P.sunDir[1], P.sunDir[2]};
    const MlF3 albedo{r.albedo[0], r.albedo[1], r.albedo[2]};
    const f32 NdotL = std::fmax(ml_dot(N, L), 0.f);
    const f32 NdotV = std::fmax(ml_dot(N, V), 1.0e-4f);
    const MlF3 H = ml_normalize_or(ml_add(L, V), N);
    const f32 NdotH = std::fmax(ml_dot(N, H), 0.f);
    const f32 VdotH = std::fmax(ml_dot(V, H), 0.f);
    const f32 a = std::fmax(r.roughness * r.roughness, 0.002f);
    const f32 a2 = a * a;
    const f32 dd = NdotH * NdotH * (a2 - 1.f) + 1.f;
    const f32 D = a2 / (3.14159265f * dd * dd);
    const f32 k = a * 0.5f;
    const f32 G = (NdotL / (NdotL * (1.f - k) + k)) * (NdotV / (NdotV * (1.f - k) + k));
    const MlF3 F0 = ml_lerp3(MlF3{0.04f, 0.04f, 0.04f}, albedo, r.metallic);
    const f32 fw = std::pow(1.f - VdotH, 5.f);
    const MlF3 F = ml_add(F0, ml_mul(ml_sub(MlF3{1.f, 1.f, 1.f}, F0), fw));
    const f32 specScale = NdotL > 0.f ? D * G / (4.f * NdotL * NdotV) : 0.f;
    const MlF3 diffuse = ml_mul(albedo, (1.f - r.metallic) * 0.31830989f);
    const MlF3 direct = ml_mul(ml_add(diffuse, ml_mul(F, specScale)), P.sunIntensity * NdotL);
    const f32 hemi = N.y * 0.5f + 0.5f;
    const MlF3 sky = ml_lerp3(MlF3{P.groundColor[0], P.groundColor[1], P.groundColor[2]},
                              MlF3{P.skyColor[0], P.skyColor[1], P.skyColor[2]}, hemi);
    const MlF3 amb = ml_mul(ml_mul3(ml_add(ml_mul(albedo, 1.f - r.metallic), ml_mul(F0, 0.5f)), sky), P.ambient * r.ao);
    const MlF3 c = ml_mul(ml_add(direct, amb), P.exposure);
    return MlF3{c.x / (1.f + c.x), c.y / (1.f + c.y), c.z / (1.f + c.z)};
}

/// One pixel of the ball scene (f32x4, a = 1 on a ball, the background otherwise).
inline MlF4 ml_ball_pixel(const MlParams& P, const MlView& v, const MlBall* balls, u32 x, u32 y) {
    const f32 sx = ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(P.width) * 2.f - 1.f) * P.tanHalfY * P.aspect;
    const f32 sy = (1.f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(P.height) * 2.f) * P.tanHalfY;
    const MlF3 fwd{P.camForward[0], P.camForward[1], P.camForward[2]};
    const MlF3 rgt{P.camRight[0], P.camRight[1], P.camRight[2]};
    const MlF3 up{P.camUp[0], P.camUp[1], P.camUp[2]};
    const MlF3 d = ml_normalize_or(ml_add(ml_add(fwd, ml_mul(rgt, sx)), ml_mul(up, sy)), fwd);
    const MlF3 o{P.camPos[0], P.camPos[1], P.camPos[2]};
    f32 t = 0.f;
    u32 ball = 0;
    const u32 count = P.ballCount < kMlMaxBalls ? P.ballCount : kMlMaxBalls;
    if (!ml_trace_balls(balls, count, o, d, t, ball)) {
        return MlF4{P.background[0], P.background[1], P.background[2], P.background[3]};
    }
    const MlF3 hitP = ml_add(o, ml_mul(d, t));
    const MlSurface s = ml_ball_surface(balls[ball], hitP, t);
    const MlResult r = ml_evaluate(v, s);
    const MlF3 c = ml_shade(P, r, ml_mul(d, -1.f));
    return MlF4{c.x, c.y, c.z, 1.f};
}

} // namespace fuse::renderer::material_layers
