// Asset plan W0.7 layered materials: the records and the evaluation of every kernel. GLSL twin of ml_common.slang;
// CPU twin: include/fuse/renderer/material_layers/ml_kernel.hpp (same operations, same order, f32). The C++ mirror of
// the records is ml_types.hpp (checked by fuse_rp_material_layers_layout).
#ifndef FUSE_ML_COMMON_GLSL
#define FUSE_ML_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define ML_NO_TEXTURE 0xFFFFFFFFu
#define ML_MAX_LAYERS 3u
#define ML_MAX_BALLS 32u
#define ML_FLAG_TRIPLANAR (1u << 0)
#define ML_FLAG_STOCHASTIC (1u << 1)
#define ML_FLAG_DETAIL (1u << 2)
#define ML_FLAG_MACRO (1u << 3)
#define ML_MASK_VERTEX_R 1u
#define ML_MASK_VERTEX_G 2u
#define ML_MASK_VERTEX_B 3u
#define ML_MASK_VERTEX_A 4u
#define ML_MASK_SLOPE_UP 5u
#define ML_MASK_WORLD_HEIGHT 6u
#define ML_LAYER_WET 1u
#define ML_TEX_SRGB 1u

// MlTexture, 32 bytes.
struct MlTexture {
    uint offset;
    uint width;
    uint height;
    uint flags;
    float mean[4];
};

// MlLayer, 64 bytes.
struct MlLayer {
    float albedo[3];
    float roughness;
    float metallic;
    float uvScale;
    float contrast;
    float coverage;
    uint albedoTex;
    uint normalTex;
    uint mask;
    uint mode;
    float maskBias;
    float maskScale;
    float normalStrength;
    uint reserved;
};

// MlMaterial, 288 bytes.
struct MlMaterial {
    float albedo[3];
    float roughness;
    float metallic;
    float uvScale;
    float normalStrength;
    uint flags;
    uint albedoTex;
    uint normalTex;
    uint layerCount;
    uint shadingModel;
    uint detailAlbedoTex;
    uint detailNormalTex;
    float detailScale;
    float detailStrength;
    float detailFadeStart;
    float detailFadeEnd;
    float triplanarSharpness;
    float stochasticLattice;
    float macroScale;
    float macroStrength;
    uint procedural;
    uint category;
    MlLayer layers[3];
};

// MlSurface, 80 bytes.
struct MlSurface {
    float position[3];
    float viewDistance;
    float normal[3];
    float tangentSign;
    float tangent[3];
    uint material;
    float uv[2];
    float reserved[2];
    float color[4];
};

// MlResult, 48 bytes.
struct MlResult {
    float albedo[3];
    float roughness;
    float normal[3];
    float metallic;
    float ao;
    float height;
    float reserved[2];
};

// MlBall, 32 bytes.
struct MlBall {
    float center[3];
    float radius;
    uint material;
    uint reserved[3];
};

// MlResolveTable, 32 bytes (the WP-1.5 resolve's layered-material table; MlTexture::offset = bindless image handle).
struct MlResolveTable {
    uint64_t materials;
    uint64_t textures;
    uint materialCount;
    uint textureCount;
    uint reserved[2];
};

// MlParams, 224 bytes.
struct MlParams {
    uint64_t materials;
    uint64_t textures;
    uint64_t texels;
    uint64_t lut;
    uint64_t surfaces;
    uint64_t results;
    uint64_t image;
    uint64_t balls;
    uint count;
    uint width;
    uint height;
    uint flags;
    uint ballCount;
    uint materialCount;
    uint textureCount;
    uint reserved0;
    float camPos[3];
    float tanHalfY;
    float camForward[3];
    float aspect;
    float camRight[3];
    float exposure;
    float camUp[3];
    float reserved1;
    float sunDir[3];
    float sunIntensity;
    float skyColor[3];
    float ambient;
    float groundColor[3];
    float reserved2;
    float background[4];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MlParamsRef { MlParams p; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MlMaterialsRef { MlMaterial v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MlTexturesRef { MlTexture v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer MlUintsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer MlFloatsRef { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MlSurfacesRef { MlSurface v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer MlResultsRef { MlResult v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MlBallsRef { MlBall v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer MlImageRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MlResolveTableRef { MlResolveTable t; };

// Includers with their own push constants (the WP-1.5 material resolve) define ML_NO_PUSH_CONSTANTS.
#ifndef ML_NO_PUSH_CONSTANTS
layout(push_constant) uniform MlPushBlock {
    uint64_t params;
    uint64_t src;
    uint64_t dst;
    uint count;
    uint reserved;
} pc;
#endif

MlParams ml_load(uint64_t address) { return MlParamsRef(address).p; }

// The library the evaluation reads.
struct MlView {
    uint64_t materials;
    uint materialCount;
    uint64_t textures;
    uint textureCount;
    uint64_t texels;
    uint64_t lut;
    uint sampler_; // ML_BINDLESS_TEXTURES: bindless sampler handle of every texture
};

MlView ml_view(MlParams P) {
    MlView v;
    v.materials = P.materials;
    v.materialCount = P.materialCount;
    v.textures = P.textures;
    v.textureCount = P.textureCount;
    v.texels = P.texels;
    v.lut = P.lut;
    v.sampler_ = 0u;
    return v;
}

// The view of a resolve table (textures are bindless images; no texel pool / LUT).
MlView ml_resolve_view(uint64_t table, uint smp) {
    const MlResolveTable t = MlResolveTableRef(table).t;
    MlView v;
    v.materials = t.materials;
    v.materialCount = t.materialCount;
    v.textures = t.textures;
    v.textureCount = t.textureCount;
    v.texels = 0ul;
    v.lut = 0ul;
    v.sampler_ = smp;
    return v;
}

// Screen-space derivatives of a surface point (ml_kernel.hpp MlGrad).
struct MlGrad {
    vec2 duvdx;
    vec2 duvdy;
    vec3 dPdx;
    vec3 dPdy;
};

struct MlSetSample {
    vec3 albedo;
    float height;
    float roughness;
    float ao;
    vec3 normal;
};

struct MlFrame {
    vec3 P;
    vec3 N;
    vec3 T;
    vec3 B;
    vec2 uv;
    vec2 duvdx;
    vec2 duvdy;
    vec3 dPdx;
    vec3 dPdy;
    bool triplanar;
    bool stochastic;
    float sharpness;
    float lattice;
};

// --- scalar helpers -------------------------------------------------------------------------------------------
float ml_saturate(float x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
float ml_lerp(float a, float b, float t) { return a + (b - a) * t; }
vec3 ml_lerp3(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
float ml_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 ml_cross(vec3 a, vec3 b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
vec3 ml_normalize_or(vec3 v, vec3 fallback) {
    const float l = ml_dot(v, v);
    if (l > 0.0) {
        const float inv = 1.0 / sqrt(l);
        return vec3(v.x * inv, v.y * inv, v.z * inv);
    }
    return fallback;
}

uint ml_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float ml_unit(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

float ml_value_noise(vec3 p, uint seed) {
    const float fx = floor(p.x);
    const float fy = floor(p.y);
    const float fz = floor(p.z);
    const float tx = p.x - fx;
    const float ty = p.y - fy;
    const float tz = p.z - fz;
    const float ux = tx * tx * (3.0 - 2.0 * tx);
    const float uy = ty * ty * (3.0 - 2.0 * ty);
    const float uz = tz * tz * (3.0 - 2.0 * tz);
    const uint ix = uint(int(fx));
    const uint iy = uint(int(fy));
    const uint iz = uint(int(fz));
    float c[8];
    for (uint i = 0u; i < 8u; ++i) {
        const uint h = ml_hash(((ix + (i & 1u)) * 0x8da6b343u) ^ ((iy + ((i >> 1) & 1u)) * 0xd8163841u) ^
                               ((iz + (i >> 2)) * 0xcb1ab31fu) ^ seed);
        c[i] = ml_unit(h);
    }
    const float x00 = ml_lerp(c[0], c[1], ux);
    const float x10 = ml_lerp(c[2], c[3], ux);
    const float x01 = ml_lerp(c[4], c[5], ux);
    const float x11 = ml_lerp(c[6], c[7], ux);
    return ml_lerp(ml_lerp(x00, x10, uy), ml_lerp(x01, x11, uy), uz);
}

// --- textures ---------------------------------------------------------------------------------------------------
vec4 ml_texel(MlView v, MlTexture t, uint x, uint y) {
    const uint w = MlUintsRef(v.texels).v[t.offset + y * t.width + x];
    const uint rgbBase = (t.flags & ML_TEX_SRGB) != 0u ? 0u : 256u;
    MlFloatsRef lut = MlFloatsRef(v.lut);
    return vec4(lut.v[rgbBase + (w & 255u)], lut.v[rgbBase + ((w >> 8) & 255u)], lut.v[rgbBase + ((w >> 16) & 255u)],
                lut.v[256u + (w >> 24)]);
}

vec4 ml_bilinear(MlView v, MlTexture t, vec2 uv) {
    const float W = float(t.width);
    const float H = float(t.height);
    const float x = (uv.x - floor(uv.x)) * W - 0.5;
    const float y = (uv.y - floor(uv.y)) * H - 0.5;
    const float x0 = floor(x);
    const float y0 = floor(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const uint ix0 = x0 < 0.0 ? t.width - 1u : uint(x0);
    const uint iy0 = y0 < 0.0 ? t.height - 1u : uint(y0);
    const uint ix1 = ix0 + 1u >= t.width ? 0u : ix0 + 1u;
    const uint iy1 = iy0 + 1u >= t.height ? 0u : iy0 + 1u;
    const vec4 a = ml_texel(v, t, ix0, iy0);
    const vec4 b = ml_texel(v, t, ix1, iy0);
    const vec4 c = ml_texel(v, t, ix0, iy1);
    const vec4 d = ml_texel(v, t, ix1, iy1);
    const float rx = ml_lerp(ml_lerp(a.x, b.x, fx), ml_lerp(c.x, d.x, fx), fy);
    const float ry = ml_lerp(ml_lerp(a.y, b.y, fx), ml_lerp(c.y, d.y, fx), fy);
    const float rz = ml_lerp(ml_lerp(a.z, b.z, fx), ml_lerp(c.z, d.z, fx), fy);
    const float rw = ml_lerp(ml_lerp(a.w, b.w, fx), ml_lerp(c.w, d.w, fx), fy);
    return vec4(rx, ry, rz, rw);
}

struct MlStochastic {
    vec3 w;
    vec2 offset[3];
};

MlStochastic ml_stochastic_lattice(vec2 uv, float lattice, uint seed) {
    const float sx = uv.x * lattice;
    const float sy = uv.y * lattice;
    const float kx = sx - 0.57735027 * sy;
    const float ky = 1.15470054 * sy;
    const float bx = floor(kx);
    const float by = floor(ky);
    const float fx = kx - bx;
    const float fy = ky - by;
    const float z = 1.0 - fx - fy;
    const int ibx = int(bx);
    const int iby = int(by);
    int vx[3];
    int vy[3];
    MlStochastic s;
    if (z > 0.0) {
        s.w = vec3(z, fy, fx);
        vx[0] = ibx;
        vy[0] = iby;
        vx[1] = ibx;
        vy[1] = iby + 1;
        vx[2] = ibx + 1;
        vy[2] = iby;
    } else {
        s.w = vec3(-z, 1.0 - fy, 1.0 - fx);
        vx[0] = ibx + 1;
        vy[0] = iby + 1;
        vx[1] = ibx + 1;
        vy[1] = iby;
        vx[2] = ibx;
        vy[2] = iby + 1;
    }
    for (uint i = 0u; i < 3u; ++i) {
        const uint h = ml_hash((uint(vx[i]) * 0x27d4eb2fu) ^ (uint(vy[i]) * 0x165667b1u) ^ seed);
        s.offset[i] = vec2(ml_unit(h), ml_unit(ml_hash(h ^ 0x9e3779b9u)));
    }
    return s;
}

#ifdef ML_BINDLESS_TEXTURES
// The includer defines vec4 ml_bindless_sample(uint image, uint smp, vec2 uv, vec2 dx, vec2 dy) (textureGrad on the
// bindless sampled image) before including this file; MlTexture::offset is the image's handle (MlResolveTable).
vec4 ml_filter(MlView v, MlTexture t, vec2 uv, vec2 dx, vec2 dy) { return ml_bindless_sample(t.offset, v.sampler_, uv, dx, dy); }
#else
vec4 ml_filter(MlView v, MlTexture t, vec2 uv, vec2 dx, vec2 dy) { return ml_bilinear(v, t, uv); }
#endif

vec4 ml_sample_tex(MlView v, uint tex, vec2 uv, vec2 dx, vec2 dy, bool stochastic, MlStochastic s, vec4 fallback) {
    if (tex == ML_NO_TEXTURE || tex >= v.textureCount) {
        return fallback;
    }
    const MlTexture t = MlTexturesRef(v.textures).v[tex];
    if (!stochastic) {
        return ml_filter(v, t, uv, dx, dy);
    }
    const vec4 a = ml_filter(v, t, vec2(uv.x + s.offset[0].x, uv.y + s.offset[0].y), dx, dy);
    const vec4 b = ml_filter(v, t, vec2(uv.x + s.offset[1].x, uv.y + s.offset[1].y), dx, dy);
    const vec4 c = ml_filter(v, t, vec2(uv.x + s.offset[2].x, uv.y + s.offset[2].y), dx, dy);
    const float inv = 1.0 / sqrt(s.w.x * s.w.x + s.w.y * s.w.y + s.w.z * s.w.z);
    const float rx = t.mean[0] + (s.w.x * (a.x - t.mean[0]) + s.w.y * (b.x - t.mean[0]) + s.w.z * (c.x - t.mean[0])) * inv;
    const float ry = t.mean[1] + (s.w.x * (a.y - t.mean[1]) + s.w.y * (b.y - t.mean[1]) + s.w.z * (c.y - t.mean[1])) * inv;
    const float rz = t.mean[2] + (s.w.x * (a.z - t.mean[2]) + s.w.y * (b.z - t.mean[2]) + s.w.z * (c.z - t.mean[2])) * inv;
    const float rw = t.mean[3] + (s.w.x * (a.w - t.mean[3]) + s.w.y * (b.w - t.mean[3]) + s.w.z * (c.w - t.mean[3])) * inv;
    return vec4(ml_saturate(rx), ml_saturate(ry), ml_saturate(rz), ml_saturate(rw));
}

vec3 ml_tangent_normal(vec4 n, float strength) {
    const float x = n.x * 2.0 - 1.0;
    const float y = n.y * 2.0 - 1.0;
    const float z = sqrt(max(1.0 - x * x - y * y, 0.0));
    return vec3(x * strength, y * strength, z);
}

struct MlTap {
    vec4 a;
    vec4 n;
};

MlTap ml_tap(MlView v, uint albedoTex, uint normalTex, vec2 uv, vec2 dx, vec2 dy, MlFrame f, uint seed) {
    MlStochastic s;
    s.w = vec3(0.0);
    s.offset[0] = vec2(0.0);
    s.offset[1] = vec2(0.0);
    s.offset[2] = vec2(0.0);
    if (f.stochastic) {
        s = ml_stochastic_lattice(uv, f.lattice, seed);
    }
    MlTap t;
    t.a = ml_sample_tex(v, albedoTex, uv, dx, dy, f.stochastic, s, vec4(1.0, 1.0, 1.0, 0.5));
    t.n = ml_sample_tex(v, normalTex, uv, dx, dy, f.stochastic, s, vec4(0.5, 0.5, 1.0, 1.0));
    return t;
}

MlSetSample ml_sample_set(MlView v, uint albedoTex, uint normalTex, float scale, float strength, uint seed, MlFrame f) {
    MlSetSample r;
    if (!f.triplanar) {
        const MlTap t = ml_tap(v, albedoTex, normalTex, vec2(f.uv.x * scale, f.uv.y * scale),
                               vec2(f.duvdx.x * scale, f.duvdx.y * scale), vec2(f.duvdy.x * scale, f.duvdy.y * scale), f,
                               seed);
        r.albedo = t.a.xyz;
        r.height = t.a.w;
        r.roughness = t.n.z;
        r.ao = t.n.w;
        const vec3 tn = ml_tangent_normal(t.n, strength);
        const vec3 w = f.T * tn.x + f.B * tn.y + f.N * tn.z;
        r.normal = ml_normalize_or(w, f.N);
        return r;
    }
    const float ax = pow(abs(f.N.x), f.sharpness);
    const float ay = pow(abs(f.N.y), f.sharpness);
    const float az = pow(abs(f.N.z), f.sharpness);
    const float inv = 1.0 / (ax + ay + az);
    const float wx = ax * inv;
    const float wy = ay * inv;
    const float wz = az * inv;
    const MlTap tx = ml_tap(v, albedoTex, normalTex, vec2(f.P.z * scale, f.P.y * scale),
                            vec2(f.dPdx.z * scale, f.dPdx.y * scale), vec2(f.dPdy.z * scale, f.dPdy.y * scale), f, seed);
    const MlTap ty = ml_tap(v, albedoTex, normalTex, vec2(f.P.x * scale, f.P.z * scale),
                            vec2(f.dPdx.x * scale, f.dPdx.z * scale), vec2(f.dPdy.x * scale, f.dPdy.z * scale), f,
                            seed ^ 0x51ed27u);
    const MlTap tz = ml_tap(v, albedoTex, normalTex, vec2(f.P.x * scale, f.P.y * scale),
                            vec2(f.dPdx.x * scale, f.dPdx.y * scale), vec2(f.dPdy.x * scale, f.dPdy.y * scale), f,
                            seed ^ 0xa3b195u);
    r.albedo = vec3(tx.a.x * wx + ty.a.x * wy + tz.a.x * wz, tx.a.y * wx + ty.a.y * wy + tz.a.y * wz,
                    tx.a.z * wx + ty.a.z * wy + tz.a.z * wz);
    r.height = tx.a.w * wx + ty.a.w * wy + tz.a.w * wz;
    r.roughness = tx.n.z * wx + ty.n.z * wy + tz.n.z * wz;
    r.ao = tx.n.w * wx + ty.n.w * wy + tz.n.w * wz;
    const vec3 nx = ml_tangent_normal(tx.n, strength);
    const vec3 ny = ml_tangent_normal(ty.n, strength);
    const vec3 nz = ml_tangent_normal(tz.n, strength);
    const vec3 px = vec3(nx.x + f.N.z, nx.y + f.N.y, abs(nx.z) * f.N.x);
    const vec3 py = vec3(ny.x + f.N.x, ny.y + f.N.z, abs(ny.z) * f.N.y);
    const vec3 pz = vec3(nz.x + f.N.x, nz.y + f.N.y, abs(nz.z) * f.N.z);
    const vec3 w = vec3(px.z * wx + py.x * wy + pz.x * wz, px.y * wx + py.z * wy + pz.y * wz,
                        px.x * wx + py.y * wy + pz.z * wz);
    r.normal = ml_normalize_or(w, f.N);
    return r;
}

float ml_layer_mask(MlLayer l, MlSurface s, MlFrame f) {
    float m = 1.0;
    if (l.mask == ML_MASK_VERTEX_R) {
        m = s.color[0];
    } else if (l.mask == ML_MASK_VERTEX_G) {
        m = s.color[1];
    } else if (l.mask == ML_MASK_VERTEX_B) {
        m = s.color[2];
    } else if (l.mask == ML_MASK_VERTEX_A) {
        m = s.color[3];
    } else if (l.mask == ML_MASK_SLOPE_UP) {
        m = (f.N.y - l.maskBias) * l.maskScale;
    } else if (l.mask == ML_MASK_WORLD_HEIGHT) {
        m = (f.P.y - l.maskBias) * l.maskScale;
    }
    return ml_saturate(ml_saturate(m) * l.coverage);
}

float ml_height_blend(float hBase, float hLayer, float m, float contrast) {
    return ml_saturate((hLayer - hBase + 2.0 * (2.0 * m - 1.0)) * contrast * 0.5 + 0.5);
}

MlResult ml_evaluate_grad(MlView v, MlSurface s, MlGrad g) {
    MlFrame f;
    f.duvdx = g.duvdx;
    f.duvdy = g.duvdy;
    f.dPdx = g.dPdx;
    f.dPdy = g.dPdy;
    f.P = vec3(s.position[0], s.position[1], s.position[2]);
    f.N = ml_normalize_or(vec3(s.normal[0], s.normal[1], s.normal[2]), vec3(0.0, 1.0, 0.0));
    const vec3 t0 = vec3(s.tangent[0], s.tangent[1], s.tangent[2]);
    const vec3 tp = t0 - f.N * ml_dot(f.N, t0);
    const vec3 helper = abs(f.N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    f.T = ml_normalize_or(tp, ml_normalize_or(ml_cross(helper, f.N), vec3(1.0, 0.0, 0.0)));
    f.B = ml_cross(f.N, f.T) * (s.tangentSign < 0.0 ? -1.0 : 1.0);
    f.uv = vec2(s.uv[0], s.uv[1]);
    f.triplanar = false;
    f.stochastic = false;
    f.sharpness = 4.0;
    f.lattice = 2.0;
    MlResult r;
    r.reserved[0] = 0.0;
    r.reserved[1] = 0.0;
    if (s.material >= v.materialCount) {
        r.albedo[0] = 1.0;
        r.albedo[1] = 1.0;
        r.albedo[2] = 1.0;
        r.roughness = 0.5;
        r.normal[0] = f.N.x;
        r.normal[1] = f.N.y;
        r.normal[2] = f.N.z;
        r.metallic = 0.0;
        r.ao = 1.0;
        r.height = 0.5;
        return r;
    }
    const MlMaterial m = MlMaterialsRef(v.materials).v[s.material];
    f.triplanar = (m.flags & ML_FLAG_TRIPLANAR) != 0u;
    f.stochastic = (m.flags & ML_FLAG_STOCHASTIC) != 0u;
    f.sharpness = m.triplanarSharpness;
    f.lattice = m.stochasticLattice;

    const MlSetSample base = ml_sample_set(v, m.albedoTex, m.normalTex, m.uvScale, m.normalStrength, 0x1234567u, f);
    vec3 albedo = vec3(m.albedo[0], m.albedo[1], m.albedo[2]) * base.albedo;
    float height = base.height;
    float roughness = m.roughness * base.roughness;
    float metallic = m.metallic;
    float ao = base.ao;
    vec3 N = base.normal;
    if ((m.flags & ML_FLAG_MACRO) != 0u) {
        const float n = ml_value_noise(f.P * m.macroScale, 0x2545f491u);
        albedo = albedo * (1.0 + m.macroStrength * (n * 2.0 - 1.0));
    }
    const uint layers = m.layerCount < ML_MAX_LAYERS ? m.layerCount : ML_MAX_LAYERS;
    for (uint i = 0u; i < layers; ++i) {
        const MlLayer l = m.layers[i];
        const float mask = ml_layer_mask(l, s, f);
        if (l.mode == ML_LAYER_WET) {
            const float t = ml_saturate(mask * 2.0 - height);
            const vec3 dark = vec3(1.0 + (l.albedo[0] - 1.0) * t * (1.0 - metallic),
                                   1.0 + (l.albedo[1] - 1.0) * t * (1.0 - metallic),
                                   1.0 + (l.albedo[2] - 1.0) * t * (1.0 - metallic));
            albedo = albedo * dark;
            roughness = ml_lerp(roughness, l.roughness, t);
            N = ml_normalize_or(ml_lerp3(N, f.N, t), f.N);
            continue;
        }
        const MlSetSample ls = ml_sample_set(v, l.albedoTex, l.normalTex, m.uvScale * l.uvScale, l.normalStrength,
                                             0x68e31da4u + i * 0x1b56c4e9u, f);
        const float t = ml_height_blend(height, ls.height, mask, l.contrast);
        albedo = ml_lerp3(albedo, vec3(l.albedo[0], l.albedo[1], l.albedo[2]) * ls.albedo, t);
        roughness = ml_lerp(roughness, l.roughness * ls.roughness, t);
        metallic = ml_lerp(metallic, l.metallic, t);
        ao = ml_lerp(ao, ls.ao, t);
        N = ml_normalize_or(ml_lerp3(N, ls.normal, t), f.N);
        height = ml_lerp(height, ls.height, t);
    }
    if ((m.flags & ML_FLAG_DETAIL) != 0u) {
        const float range = m.detailFadeEnd - m.detailFadeStart;
        const float fade = 1.0 - ml_saturate(range > 0.0 ? (s.viewDistance - m.detailFadeStart) / range : 0.0);
        const float k = m.detailStrength * fade;
        if (k > 0.0) {
            const MlSetSample d = ml_sample_set(v, m.detailAlbedoTex, m.detailNormalTex, m.uvScale * m.detailScale, 1.0,
                                                0x3c6ef372u, f);
            albedo = vec3(albedo.x * max(1.0 + k * (d.albedo.x * 2.0 - 1.0), 0.0),
                          albedo.y * max(1.0 + k * (d.albedo.y * 2.0 - 1.0), 0.0),
                          albedo.z * max(1.0 + k * (d.albedo.z * 2.0 - 1.0), 0.0));
            N = ml_normalize_or(N + (d.normal - f.N) * k, f.N);
        }
    }
    r.albedo[0] = max(albedo.x, 0.0);
    r.albedo[1] = max(albedo.y, 0.0);
    r.albedo[2] = max(albedo.z, 0.0);
    r.roughness = ml_saturate(roughness);
    r.normal[0] = N.x;
    r.normal[1] = N.y;
    r.normal[2] = N.z;
    r.metallic = ml_saturate(metallic);
    r.ao = ml_saturate(ao);
    r.height = height;
    return r;
}

// ml_evaluate_grad without derivatives (the texel-pool path).
MlResult ml_evaluate(MlView v, MlSurface s) {
    MlGrad g;
    g.duvdx = vec2(0.0);
    g.duvdy = vec2(0.0);
    g.dPdx = vec3(0.0);
    g.dPdy = vec3(0.0);
    return ml_evaluate_grad(v, s, g);
}

// --- golden material-ball scene ----------------------------------------------------------------------------------
bool ml_trace_balls(uint64_t balls, uint count, vec3 o, vec3 d, out float tHit, out uint ball) {
    tHit = 3.0e38;
    ball = 0u;
    bool hit = false;
    MlBallsRef B = MlBallsRef(balls);
    for (uint i = 0u; i < count; ++i) {
        const MlBall b = B.v[i];
        const vec3 c = vec3(b.center[0], b.center[1], b.center[2]);
        const vec3 oc = o - c;
        const float bb = ml_dot(oc, d);
        const float cc = ml_dot(oc, oc) - b.radius * b.radius;
        const float disc = bb * bb - cc;
        if (disc < 0.0) {
            continue;
        }
        const float t = -bb - sqrt(disc);
        if (t > 0.0 && t < tHit) {
            tHit = t;
            ball = i;
            hit = true;
        }
    }
    return hit;
}

MlSurface ml_ball_surface(MlBall b, vec3 P, float dist) {
    MlSurface s;
    const vec3 c = vec3(b.center[0], b.center[1], b.center[2]);
    const vec3 p = (P - c) * (1.0 / b.radius);
    s.position[0] = P.x;
    s.position[1] = P.y;
    s.position[2] = P.z;
    s.viewDistance = dist;
    s.normal[0] = p.x;
    s.normal[1] = p.y;
    s.normal[2] = p.z;
    s.tangent[0] = p.z;
    s.tangent[1] = 0.0;
    s.tangent[2] = -p.x;
    s.tangentSign = 1.0;
    s.material = b.material;
    const float phi = atan(p.x, p.z);
    const float theta = acos(p.y < -1.0 ? -1.0 : (p.y > 1.0 ? 1.0 : p.y));
    s.uv[0] = (phi * 0.15915494 + 0.5) * 2.0;
    s.uv[1] = theta * 0.31830989;
    s.reserved[0] = 0.0;
    s.reserved[1] = 0.0;
    s.color[0] = ml_saturate(ml_value_noise(P * 3.0, 0x7f4a7c15u) * 1.6 - 0.3);
    s.color[1] = 1.0;
    s.color[2] = ml_saturate(0.35 - p.y * 0.8);
    s.color[3] = 1.0;
    return s;
}

vec3 ml_shade(MlParams P, MlResult r, vec3 V) {
    const vec3 N = vec3(r.normal[0], r.normal[1], r.normal[2]);
    const vec3 L = vec3(P.sunDir[0], P.sunDir[1], P.sunDir[2]);
    const vec3 albedo = vec3(r.albedo[0], r.albedo[1], r.albedo[2]);
    const float NdotL = max(ml_dot(N, L), 0.0);
    const float NdotV = max(ml_dot(N, V), 1.0e-4);
    const vec3 H = ml_normalize_or(L + V, N);
    const float NdotH = max(ml_dot(N, H), 0.0);
    const float VdotH = max(ml_dot(V, H), 0.0);
    const float a = max(r.roughness * r.roughness, 0.002);
    const float a2 = a * a;
    const float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    const float D = a2 / (3.14159265 * dd * dd);
    const float k = a * 0.5;
    const float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
    const vec3 F0 = ml_lerp3(vec3(0.04), albedo, r.metallic);
    const float fw = pow(1.0 - VdotH, 5.0);
    const vec3 F = F0 + (vec3(1.0) - F0) * fw;
    const float specScale = NdotL > 0.0 ? D * G / (4.0 * NdotL * NdotV) : 0.0;
    const vec3 diffuse = albedo * ((1.0 - r.metallic) * 0.31830989);
    const vec3 direct = (diffuse + F * specScale) * (P.sunIntensity * NdotL);
    const float hemi = N.y * 0.5 + 0.5;
    const vec3 sky = ml_lerp3(vec3(P.groundColor[0], P.groundColor[1], P.groundColor[2]),
                              vec3(P.skyColor[0], P.skyColor[1], P.skyColor[2]), hemi);
    const vec3 amb = ((albedo * (1.0 - r.metallic) + F0 * 0.5) * sky) * (P.ambient * r.ao);
    const vec3 c = (direct + amb) * P.exposure;
    return vec3(c.x / (1.0 + c.x), c.y / (1.0 + c.y), c.z / (1.0 + c.z));
}

vec4 ml_ball_pixel(MlParams P, MlView v, uint x, uint y) {
    const float sx = ((float(x) + 0.5) / float(P.width) * 2.0 - 1.0) * P.tanHalfY * P.aspect;
    const float sy = (1.0 - (float(y) + 0.5) / float(P.height) * 2.0) * P.tanHalfY;
    const vec3 fwd = vec3(P.camForward[0], P.camForward[1], P.camForward[2]);
    const vec3 rgt = vec3(P.camRight[0], P.camRight[1], P.camRight[2]);
    const vec3 up = vec3(P.camUp[0], P.camUp[1], P.camUp[2]);
    const vec3 d = ml_normalize_or(fwd + rgt * sx + up * sy, fwd);
    const vec3 o = vec3(P.camPos[0], P.camPos[1], P.camPos[2]);
    float t;
    uint ball;
    const uint count = P.ballCount < ML_MAX_BALLS ? P.ballCount : ML_MAX_BALLS;
    if (!ml_trace_balls(P.balls, count, o, d, t, ball)) {
        return vec4(P.background[0], P.background[1], P.background[2], P.background[3]);
    }
    const vec3 hitP = o + d * t;
    const MlSurface s = ml_ball_surface(MlBallsRef(P.balls).v[ball], hitP, t);
    const MlResult r = ml_evaluate(v, s);
    const vec3 c = ml_shade(P, r, -d);
    return vec4(c, 1.0);
}

#endif // FUSE_ML_COMMON_GLSL
