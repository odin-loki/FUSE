// WP-6.2 ray-traced shadows and reflections: shared Slang declarations and math (twin of rtfx_common.glsl;
// records mirror include/fuse/renderer/rt_effects/rt_effects_types.hpp, functions rt_effects_kernel.hpp).
// #include after gpu_scene.slang. Compile with -fp-mode precise.
#pragma once

static const uint kFuseRtfxTile = 8u;
static const uint kFuseRtfxMaxShadowLights = 4u;
static const uint kFuseRtfxMaxHitLights = 2u;
static const float kFuseRtfxNoHit = -1.0;
static const uint kFuseRtfxLightDirectional = 1u;
static const uint kFuseRtfxLightSun = 2u;
static const uint kFuseRtfxLightPoint = 3u;
static const uint kFuseRtfxLightRect = 4u;
static const uint kFuseRtfxLightDisk = 5u;
static const uint kFuseRtfxFlagHitShadows = 1u;
static const uint kFuseRtfxRayTraced = 1u;
static const uint kFuseRtfxRayHit = 2u;
static const uint kFuseRtfxStreamReflection = 16u;
static const uint kFuseRtfxInvalid = 0xFFFFFFFFu;
static const uint kFuseRtfxMaskAll = 0x7Fu; // rt::kRtMaskAll: bit 7 (kRtMaskDead) is never traced
static const float kFuseRtfxDefaultAlbedo = 0.8;
static const float kFuseRtfxPi = 3.14159265358979323846;

struct RtfxShadowLight { // RtfxShadowLight (64 bytes)
    float position[3];
    uint kind;
    float axisX[3];
    uint slot;
    float axisY[3];
    float cosCone;
    float direction[3];
    uint samples;
};

struct RtfxHitLight { // RtfxHitLight (32 bytes)
    float direction[3];
    uint shadowed;
    float irradiance[3];
    uint slot;
};

struct RtfxShadowView { // RtfxShadowView (48 bytes)
    uint64_t visibility;
    uint width;
    uint height;
    uint count;
    uint pad0;
    uint slots[4];
    uint pad1[2];
};

struct RtfxFrame { // RtfxFrameConstants (576 bytes)
    RtfxShadowView view;
    uint64_t tlas;
    uint64_t hitDistance;
    uint64_t reflection;
    uint64_t reserved0;
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
    RtfxShadowLight shadowLights[4];
    RtfxHitLight hitLights[2];
};

struct RtfxRayRecord { // RtfxRayRecord (48 bytes)
    float origin[3];
    float tMin;
    float direction[3];
    float tMax;
    float t;
    uint instance;
    uint primitive;
    uint flags;
};

struct RtfxMaterial { // Material::GPUMaterial (128 bytes)
    float4 baseColor;         // rgb = base colour, w = metallic
    float4 roughnessEmissive; // x = roughness, yzw = emissive colour
    uint textures[6];
    float emissiveIntensity;
    float normalStrength;
    uint shadingModel;
    uint flags;
    uint proceduralSeed;
    uint padding;
    float4 extensions[3];
};

struct RtfxPush { // RtfxPush
    uint64_t frame;
    uint64_t dump;
};
[[vk::push_constant]] ConstantBuffer<RtfxPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_rtfx_textures[];

RtfxFrame fuseRtfxFrame() { return ((RtfxFrame*)pc.frame)[0]; }
float3 fuseRtfxVec3(float v[3]) { return float3(v[0], v[1], v[2]); }
float4 fuseRtfxFetch(uint handle, int2 p) { return fuse_rtfx_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

// --- sequence ---------------------------------------------------------------------------------------------
uint fuseRtfxHash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint fuseRtfxPixelSeed(uint px, uint py, uint stream, uint seed) {
    return fuseRtfxHash(px + fuseRtfxHash(py + fuseRtfxHash(stream + fuseRtfxHash(seed))));
}

uint fuseRtfxLaineKarras(uint x, uint seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

uint fuseRtfxOwen(uint x, uint seed) { return reversebits(fuseRtfxLaineKarras(reversebits(x), seed)); }

uint fuseRtfxHashCombine(uint seed, uint v) { return seed ^ (v + (seed << 6u) + (seed >> 2u)); }

void fuseRtfxSobol2(uint index, out uint x, out uint y) {
    x = reversebits(index);
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

float2 fuseRtfxSample2(uint n, uint seed) {
    const uint index = fuseRtfxOwen(n, seed);
    uint x;
    uint y;
    fuseRtfxSobol2(index, x, y);
    const uint ux = fuseRtfxOwen(x, fuseRtfxHashCombine(seed, 0u));
    const uint uy = fuseRtfxOwen(y, fuseRtfxHashCombine(seed, 1u));
    return float2(float(ux >> 8u) * (1.0 / 16777216.0), float(uy >> 8u) * (1.0 / 16777216.0));
}

// --- surface ------------------------------------------------------------------------------------------------
float3 fuseRtfxNormalize(float3 a, float3 fallback) {
    const float l = sqrt(dot(a, a));
    if (!(l > 1e-20)) {
        return fallback;
    }
    const float inv = 1.0 / l;
    return float3(a.x * inv, a.y * inv, a.z * inv);
}

float3 fuseRtfxReconstruct(RtfxFrame F, uint2 p, float depth) {
    const float nx = (float(p.x) + 0.5) * F.invWidth * 2.0 - 1.0;
    const float ny = (float(p.y) + 0.5) * F.invHeight * 2.0 - 1.0;
    const float x = F.invViewProj[0] * nx + F.invViewProj[4] * ny + F.invViewProj[8] * depth + F.invViewProj[12];
    const float y = F.invViewProj[1] * nx + F.invViewProj[5] * ny + F.invViewProj[9] * depth + F.invViewProj[13];
    const float z = F.invViewProj[2] * nx + F.invViewProj[6] * ny + F.invViewProj[10] * depth + F.invViewProj[14];
    const float w = F.invViewProj[3] * nx + F.invViewProj[7] * ny + F.invViewProj[11] * depth + F.invViewProj[15];
    const float inv = 1.0 / w;
    return float3(x * inv, y * inv, z * inv);
}

float3 fuseRtfxOctDecode(float ox, float oy) {
    float3 n = float3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return fuseRtfxNormalize(n, float3(0.0, 0.0, 1.0));
}

float3 fuseRtfxRayOrigin(float3 p, float3 n, float3 camera, float normalBias, float viewBias) {
    const float3 d = p - camera;
    const float bias = normalBias + viewBias * sqrt(dot(d, d));
    return p + n * bias;
}

void fuseRtfxBasis(float3 n, out float3 t, out float3 b) {
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// --- light sampling -------------------------------------------------------------------------------------------
float2 fuseRtfxConcentric(float u, float v) {
    const float a = 2.0 * u - 1.0;
    const float b = 2.0 * v - 1.0;
    if (a == 0.0 && b == 0.0) {
        return float2(0.0, 0.0);
    }
    const float quarter = kFuseRtfxPi * 0.25;
    float r;
    float phi;
    if (abs(a) > abs(b)) {
        r = a;
        phi = quarter * (b / a);
    } else {
        r = b;
        phi = kFuseRtfxPi * 0.5 - quarter * (a / b);
    }
    return float2(r * cos(phi), r * sin(phi));
}

bool fuseRtfxShadowRay(RtfxShadowLight light, float3 origin, float u, float v, float farDistance, out float3 dir, out float tMax) {
    const float3 up = float3(0.0, 1.0, 0.0);
    dir = up;
    tMax = 0.0;
    if (light.kind == kFuseRtfxLightDirectional) {
        dir = fuseRtfxNormalize(fuseRtfxVec3(light.direction), up);
        tMax = farDistance;
        return true;
    }
    if (light.kind == kFuseRtfxLightSun) {
        const float3 axis = fuseRtfxNormalize(fuseRtfxVec3(light.direction), up);
        float3 t;
        float3 b;
        fuseRtfxBasis(axis, t, b);
        const float cosTheta = 1.0 - u * (1.0 - light.cosCone);
        const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        const float phi = 2.0 * kFuseRtfxPi * v;
        const float3 d = t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + axis * cosTheta;
        dir = fuseRtfxNormalize(d, axis);
        tMax = farDistance;
        return true;
    }
    if (light.kind == kFuseRtfxLightPoint || light.kind == kFuseRtfxLightRect || light.kind == kFuseRtfxLightDisk) {
        float3 target = fuseRtfxVec3(light.position);
        if (light.kind == kFuseRtfxLightRect) {
            target = target + (fuseRtfxVec3(light.axisX) * (2.0 * u - 1.0) + fuseRtfxVec3(light.axisY) * (2.0 * v - 1.0));
        } else if (light.kind == kFuseRtfxLightDisk) {
            const float2 d = fuseRtfxConcentric(u, v);
            target = target + (fuseRtfxVec3(light.axisX) * d.x + fuseRtfxVec3(light.axisY) * d.y);
        }
        const float3 d = target - origin;
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
float3 fuseRtfxSampleVndf(float3 wi, float alpha, float u, float v) {
    const float3 z = float3(0.0, 0.0, 1.0);
    const float3 wiStd = fuseRtfxNormalize(float3(wi.x * alpha, wi.y * alpha, wi.z), z);
    const float phi = 2.0 * kFuseRtfxPi * u;
    const float cz = (1.0 - v) * (1.0 + wiStd.z) - wiStd.z;
    const float sinTheta = sqrt(min(max(1.0 - cz * cz, 0.0), 1.0));
    const float3 h = float3(sinTheta * cos(phi) + wiStd.x, sinTheta * sin(phi) + wiStd.y, cz + wiStd.z);
    return fuseRtfxNormalize(float3(h.x * alpha, h.y * alpha, h.z), z);
}

bool fuseRtfxReflectionRay(float3 n, float3 view, float roughness, float mirrorRoughness, float u, float v, out float3 dir) {
    float3 t;
    float3 b;
    fuseRtfxBasis(n, t, b);
    const float3 wi = fuseRtfxNormalize(float3(dot(view, t), dot(view, b), max(dot(view, n), 1e-4)), n);
    float3 m = float3(0.0, 0.0, 1.0);
    const float r = min(max(roughness, 0.0), 1.0);
    if (r >= mirrorRoughness) {
        m = fuseRtfxSampleVndf(wi, r * r, u, v);
    }
    const float d = 2.0 * dot(wi, m);
    const float3 l = float3(m.x * d - wi.x, m.y * d - wi.y, m.z * d - wi.z);
    dir = n;
    if (!(l.z > 0.0)) {
        return false;
    }
    dir = fuseRtfxNormalize(t * l.x + b * l.y + n * l.z, n);
    return true;
}

// --- ray queries -----------------------------------------------------------------------------------------
// Closest committed triangle hit; t < 0 = miss.
float fuseRtfxTrace(uint64_t tlas, uint cullMask, float3 origin, float3 dir, float tMax, out uint instance, out uint primitive) {
    RayDesc desc;
    desc.Origin = origin;
    desc.TMin = 0.0;
    desc.Direction = dir;
    desc.TMax = tMax;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(as, RAY_FLAG_FORCE_OPAQUE, cullMask & kFuseRtfxMaskAll, desc);
    while (q.Proceed()) {
    }
    instance = kFuseRtfxInvalid;
    primitive = kFuseRtfxInvalid;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        instance = q.CommittedInstanceIndex();
        primitive = q.CommittedPrimitiveIndex();
        return q.CommittedRayT();
    }
    return kFuseRtfxNoHit;
}

// Any hit (shadow rays of reflection hits).
bool fuseRtfxOccluded(uint64_t tlas, uint cullMask, float3 origin, float3 dir, float tMax) {
    RayDesc desc;
    desc.Origin = origin;
    desc.TMin = 0.0;
    desc.Direction = dir;
    desc.TMax = tMax;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(as, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, cullMask & kFuseRtfxMaskAll, desc);
    while (q.Proceed()) {
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

void fuseRtfxRecord(uint64_t dump, uint index, float3 origin, float3 dir, float tMax, float t, uint instance, uint primitive,
                    uint flags) {
    if (dump == 0ull) {
        return;
    }
    RtfxRayRecord r;
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
    ((RtfxRayRecord*)dump)[index] = r;
}
