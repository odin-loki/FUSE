// WP-6.2 ray-traced shadows and reflections: shared GLSL declarations and math (twin: rtfx_common.slang).
// Records mirror include/fuse/renderer/rt_effects/rt_effects_types.hpp; the functions mirror
// rt_effects_kernel.hpp (same integer sequence bit for bit, same float expressions in the same order).
// #include after bindless.glsl and gpu_scene.glsl.
#ifndef FUSE_RTFX_COMMON_GLSL
#define FUSE_RTFX_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_ray_query : require

#define FUSE_RTFX_TILE 8
#define FUSE_RTFX_MAX_SHADOW_LIGHTS 4u
#define FUSE_RTFX_MAX_HIT_LIGHTS 2u
#define FUSE_RTFX_NO_HIT (-1.0)
#define FUSE_RTFX_LIGHT_DIRECTIONAL 1u
#define FUSE_RTFX_LIGHT_SUN 2u
#define FUSE_RTFX_LIGHT_POINT 3u
#define FUSE_RTFX_LIGHT_RECT 4u
#define FUSE_RTFX_LIGHT_DISK 5u
#define FUSE_RTFX_FLAG_HIT_SHADOWS 1u
#define FUSE_RTFX_FLAG_ATMOSPHERE_SKY 2u
#define FUSE_RTFX_RAY_TRACED 1u
#define FUSE_RTFX_RAY_HIT 2u
#define FUSE_RTFX_STREAM_REFLECTION 16u
#define FUSE_RTFX_INVALID 0xFFFFFFFFu
#define FUSE_RTFX_MASK_ALL 0x7Fu // rt::kRtMaskAll: bit 7 (kRtMaskDead) is never traced
#define FUSE_RTFX_DEFAULT_ALBEDO 0.8
#define FUSE_RTFX_PI 3.14159265358979323846

struct FuseRtfxShadowLight { // RtfxShadowLight (64 bytes)
    float position[3];
    uint kind;
    float axisX[3];
    uint slot;
    float axisY[3];
    float cosCone;
    float direction[3];
    uint samples;
};

struct FuseRtfxHitLight { // RtfxHitLight (32 bytes)
    float direction[3];
    uint shadowed;
    float irradiance[3];
    uint slot;
};

struct FuseRtfxShadowView { // RtfxShadowView (48 bytes)
    uint64_t visibility;
    uint width;
    uint height;
    uint count;
    uint pad0;
    uint slots[4];
    uint pad1[2];
};

struct FuseRtfxFrame { // RtfxFrameConstants (576 bytes)
    FuseRtfxShadowView view;
    uint64_t tlas;
    uint64_t hitDistance;
    uint64_t reflection;
    uint64_t atmosphere;
    float invViewProj[16];
    float cameraPosition[3];
    uint frameIndex;
    float invWidth;
    float invHeight;
    uint scene;
    uint flags;
    uint gbufferNormal;
    uint gbufferRoughMetal;
    uint gbufferDepth;
    uint reflectionSamples;
    float normalBias;
    float viewBias;
    float farDistance;
    float mirrorRoughness;
    float ambient[3];
    uint hitLightCount;
    float sky[3];
    uint shadowCullMask;
    uint reflectionCullMask;
    uint seed;
    uint pad0;
    uint pad1;
    FuseRtfxShadowLight shadowLights[4];
    FuseRtfxHitLight hitLights[2];
};

struct FuseRtfxRayRecord { // RtfxRayRecord (48 bytes)
    float origin[3];
    float tMin;
    float direction[3];
    float tMax;
    float t;
    uint instance;
    uint primitive;
    uint flags;
};

struct FuseRtfxMaterial { // Material::GPUMaterial (128 bytes, shaders/common/material.glsl)
    vec4 baseColor;         // rgb = base colour, w = metallic
    vec4 roughnessEmissive; // x = roughness, yzw = emissive colour
    uint textures[6];
    float emissiveIntensity;
    float normalStrength;
    uint shadingModel;
    uint flags;
    uint proceduralSeed;
    uint padding;
    vec4 extensions[3];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseRtfxFrameRef { FuseRtfxFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FuseRtfxFloats { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer FuseRtfxVec4s { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer FuseRtfxRecords { FuseRtfxRayRecord v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseRtfxMaterials { FuseRtfxMaterial v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FuseRtfxWords { uint v[]; };

layout(push_constant) uniform FuseRtfxPush { // RtfxPush
    uint64_t frame;
    uint64_t dump;
} pc;

FuseRtfxFrame fuse_rtfx_frame() { return FuseRtfxFrameRef(pc.frame).f; }
vec3 fuse_rtfx_vec3(float v[3]) { return vec3(v[0], v[1], v[2]); }

vec4 fuse_rtfx_fetch(uint handle, ivec2 p) { return texelFetch(fuse_textures_2d[fuse_handle_index(handle)], p, 0); }

// --- sequence (rtfxHash / rtfxPixelSeed / rtfxOwen / rtfxSample2) ---------------------------------------
uint fuse_rtfx_hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint fuse_rtfx_pixel_seed(uint px, uint py, uint stream, uint seed) {
    return fuse_rtfx_hash(px + fuse_rtfx_hash(py + fuse_rtfx_hash(stream + fuse_rtfx_hash(seed))));
}

uint fuse_rtfx_laine_karras(uint x, uint seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

uint fuse_rtfx_owen(uint x, uint seed) { return bitfieldReverse(fuse_rtfx_laine_karras(bitfieldReverse(x), seed)); }

uint fuse_rtfx_hash_combine(uint seed, uint v) { return seed ^ (v + (seed << 6u) + (seed >> 2u)); }

void fuse_rtfx_sobol2(uint index, out uint x, out uint y) {
    x = bitfieldReverse(index);
    uint v = 0x80000000u;
    uint r = 0u;
    for (uint bit = 0u; bit < 32u; ++bit) {
        if (((index >> bit) & 1u) != 0u) {
            r ^= v;
        }
        v ^= v >> 1u;
    }
    y = r;
}

vec2 fuse_rtfx_sample2(uint n, uint seed) {
    const uint index = fuse_rtfx_owen(n, seed);
    uint x;
    uint y;
    fuse_rtfx_sobol2(index, x, y);
    const uint ux = fuse_rtfx_owen(x, fuse_rtfx_hash_combine(seed, 0u));
    const uint uy = fuse_rtfx_owen(y, fuse_rtfx_hash_combine(seed, 1u));
    return vec2(float(ux >> 8u) * (1.0 / 16777216.0), float(uy >> 8u) * (1.0 / 16777216.0));
}

// --- surface --------------------------------------------------------------------------------------------
vec3 fuse_rtfx_normalize(vec3 a, vec3 fallback) {
    const float l = sqrt(dot(a, a));
    if (!(l > 1e-20)) {
        return fallback;
    }
    const float inv = 1.0 / l;
    return vec3(a.x * inv, a.y * inv, a.z * inv);
}

vec3 fuse_rtfx_reconstruct(FuseRtfxFrame F, uvec2 p, float depth) {
    const float nx = (float(p.x) + 0.5) * F.invWidth * 2.0 - 1.0;
    const float ny = (float(p.y) + 0.5) * F.invHeight * 2.0 - 1.0;
    const float x = F.invViewProj[0] * nx + F.invViewProj[4] * ny + F.invViewProj[8] * depth + F.invViewProj[12];
    const float y = F.invViewProj[1] * nx + F.invViewProj[5] * ny + F.invViewProj[9] * depth + F.invViewProj[13];
    const float z = F.invViewProj[2] * nx + F.invViewProj[6] * ny + F.invViewProj[10] * depth + F.invViewProj[14];
    const float w = F.invViewProj[3] * nx + F.invViewProj[7] * ny + F.invViewProj[11] * depth + F.invViewProj[15];
    const float inv = 1.0 / w;
    return vec3(x * inv, y * inv, z * inv);
}

vec3 fuse_rtfx_oct_decode(float ox, float oy) {
    vec3 n = vec3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return fuse_rtfx_normalize(n, vec3(0.0, 0.0, 1.0));
}

vec3 fuse_rtfx_ray_origin(vec3 p, vec3 n, vec3 camera, float normalBias, float viewBias) {
    const vec3 d = p - camera;
    const float bias = normalBias + viewBias * sqrt(dot(d, d));
    return p + n * bias;
}

void fuse_rtfx_basis(vec3 n, out vec3 t, out vec3 b) {
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// --- light sampling ---------------------------------------------------------------------------------------
vec2 fuse_rtfx_concentric(float u, float v) {
    const float a = 2.0 * u - 1.0;
    const float b = 2.0 * v - 1.0;
    if (a == 0.0 && b == 0.0) {
        return vec2(0.0);
    }
    const float quarter = FUSE_RTFX_PI * 0.25;
    float r;
    float phi;
    if (abs(a) > abs(b)) {
        r = a;
        phi = quarter * (b / a);
    } else {
        r = b;
        phi = FUSE_RTFX_PI * 0.5 - quarter * (a / b);
    }
    return vec2(r * cos(phi), r * sin(phi));
}

bool fuse_rtfx_shadow_ray(FuseRtfxShadowLight light, vec3 origin, float u, float v, float farDistance, out vec3 dir,
                          out float tMax) {
    const vec3 up = vec3(0.0, 1.0, 0.0);
    dir = up;
    tMax = 0.0;
    if (light.kind == FUSE_RTFX_LIGHT_DIRECTIONAL) {
        dir = fuse_rtfx_normalize(fuse_rtfx_vec3(light.direction), up);
        tMax = farDistance;
        return true;
    }
    if (light.kind == FUSE_RTFX_LIGHT_SUN) {
        const vec3 axis = fuse_rtfx_normalize(fuse_rtfx_vec3(light.direction), up);
        vec3 t;
        vec3 b;
        fuse_rtfx_basis(axis, t, b);
        const float cosTheta = 1.0 - u * (1.0 - light.cosCone);
        const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        const float phi = 2.0 * FUSE_RTFX_PI * v;
        const vec3 d = t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + axis * cosTheta;
        dir = fuse_rtfx_normalize(d, axis);
        tMax = farDistance;
        return true;
    }
    if (light.kind == FUSE_RTFX_LIGHT_POINT || light.kind == FUSE_RTFX_LIGHT_RECT || light.kind == FUSE_RTFX_LIGHT_DISK) {
        vec3 target = fuse_rtfx_vec3(light.position);
        if (light.kind == FUSE_RTFX_LIGHT_RECT) {
            target = target + (fuse_rtfx_vec3(light.axisX) * (2.0 * u - 1.0) + fuse_rtfx_vec3(light.axisY) * (2.0 * v - 1.0));
        } else if (light.kind == FUSE_RTFX_LIGHT_DISK) {
            const vec2 d = fuse_rtfx_concentric(u, v);
            target = target + (fuse_rtfx_vec3(light.axisX) * d.x + fuse_rtfx_vec3(light.axisY) * d.y);
        }
        const vec3 d = target - origin;
        const float dist = sqrt(dot(d, d));
        if (!(dist > 1e-6)) {
            return true; // on the light: nothing occludes it (tMax 0)
        }
        dir = d * (1.0 / dist);
        tMax = dist * 0.9999;
        return true;
    }
    return false;
}

// --- reflections --------------------------------------------------------------------------------------------
vec3 fuse_rtfx_sample_vndf(vec3 wi, float alpha, float u, float v) {
    const vec3 z = vec3(0.0, 0.0, 1.0);
    const vec3 wiStd = fuse_rtfx_normalize(vec3(wi.x * alpha, wi.y * alpha, wi.z), z);
    const float phi = 2.0 * FUSE_RTFX_PI * u;
    const float cz = (1.0 - v) * (1.0 + wiStd.z) - wiStd.z;
    const float sinTheta = sqrt(min(max(1.0 - cz * cz, 0.0), 1.0));
    const vec3 h = vec3(sinTheta * cos(phi) + wiStd.x, sinTheta * sin(phi) + wiStd.y, cz + wiStd.z);
    return fuse_rtfx_normalize(vec3(h.x * alpha, h.y * alpha, h.z), z);
}

bool fuse_rtfx_reflection_ray(vec3 n, vec3 view, float roughness, float mirrorRoughness, float u, float v, out vec3 dir) {
    vec3 t;
    vec3 b;
    fuse_rtfx_basis(n, t, b);
    const vec3 wi = fuse_rtfx_normalize(vec3(dot(view, t), dot(view, b), max(dot(view, n), 1e-4)), n);
    vec3 m = vec3(0.0, 0.0, 1.0);
    const float r = min(max(roughness, 0.0), 1.0);
    if (r >= mirrorRoughness) {
        m = fuse_rtfx_sample_vndf(wi, r * r, u, v);
    }
    const float d = 2.0 * dot(wi, m);
    const vec3 l = vec3(m.x * d - wi.x, m.y * d - wi.y, m.z * d - wi.z);
    dir = n;
    if (!(l.z > 0.0)) {
        return false;
    }
    dir = fuse_rtfx_normalize(t * l.x + b * l.y + n * l.z, n);
    return true;
}

// --- ray queries -----------------------------------------------------------------------------------------
// Closest committed triangle hit; t < 0 = miss.
float fuse_rtfx_trace(uint64_t tlas, uint cullMask, vec3 origin, vec3 dir, float tMax, out uint instance, out uint primitive) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(tlas), gl_RayFlagsOpaqueEXT, cullMask & FUSE_RTFX_MASK_ALL, origin, 0.0, dir,
                          tMax);
    while (rayQueryProceedEXT(q)) {
    }
    instance = FUSE_RTFX_INVALID;
    primitive = FUSE_RTFX_INVALID;
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
        instance = uint(rayQueryGetIntersectionInstanceIdEXT(q, true));
        primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
        return rayQueryGetIntersectionTEXT(q, true);
    }
    return FUSE_RTFX_NO_HIT;
}

// Any hit (shadow rays of reflection hits).
bool fuse_rtfx_occluded(uint64_t tlas, uint cullMask, vec3 origin, vec3 dir, float tMax) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(tlas), gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          cullMask & FUSE_RTFX_MASK_ALL, origin, 0.0, dir, tMax);
    while (rayQueryProceedEXT(q)) {
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}

void fuse_rtfx_record(uint64_t dump, uint index, vec3 origin, vec3 dir, float tMax, float t, uint instance, uint primitive,
                      uint flags) {
    if (dump == 0ul) {
        return;
    }
    FuseRtfxRayRecord r;
    r.origin[0] = origin.x;
    r.origin[1] = origin.y;
    r.origin[2] = origin.z;
    r.tMin = 0.0;
    r.direction[0] = dir.x;
    r.direction[1] = dir.y;
    r.direction[2] = dir.z;
    r.tMax = tMax;
    r.t = t;
    r.instance = instance;
    r.primitive = primitive;
    r.flags = flags;
    FuseRtfxRecords(dump).v[index] = r;
}

#endif // FUSE_RTFX_COMMON_GLSL
