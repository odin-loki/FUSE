// FUSE GPU clustered lighting (WP-2.1): records of include/fuse/renderer/lighting/gpu/clustered_gpu_types.hpp
// and the math of the B5 oracle (include/fuse/renderer/lighting/clustered_kernel.hpp) and of the shade
// reference (include/fuse/renderer/lighting/gpu/clustered_gpu_kernel.hpp), same expressions in the same
// order. `precise` everywhere the oracle's lists depend on the result (no contracted multiply-adds):
// the cluster AABBs and the light spheres are bit-identical to the CPU's. Slang twin: lc_common.slang.
// #include after bindless.glsl and gpu_scene.glsl.
#ifndef FUSE_LC_COMMON_GLSL
#define FUSE_LC_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_LC_LINEAR_GROUP 64u
#define FUSE_LC_SCAN_GROUP 256u
#define FUSE_LC_TILE 8u
#define FUSE_LC_SLICE_PAD 2u
#define FUSE_LC_FLAG_REVERSED_Z 1u
#define FUSE_LC_PI 3.14159265358979
#define FUSE_LC_MIN_ROUGHNESS 0.045
#define FUSE_LC_SPOT_MIN_WIDTH 1e-4

// LightingFrameConstants, 768 bytes.
struct FuseLcFrame {
    uint64_t aabbs;
    uint64_t bounds;
    uint64_t sliceCounts;
    uint64_t sliceLights;
    uint64_t clusterCounts;
    uint64_t clusterDropped;
    uint64_t clusterSlots;
    uint64_t listHeader;
    uint64_t grid;
    uint64_t directional;
    uint64_t lightList;
    uint64_t brdfLut; // WP-2.2 BRDF / LTC LUT (f32), 0 = WP-2.1 lobe
    float cameraPosition[3];
    float nearPlane;
    float right[3];
    float farPlane;
    float up[3];
    float tanX;
    float back[3];
    float tanY;
    float ambient[3];
    uint flags;
    uint width;
    uint height;
    float invWidth;
    float invHeight;
    uint tilesX;
    uint tilesY;
    uint slicesZ;
    uint capacity;
    uint clusterCount;
    uint lightCount;
    uint lightCapacity;
    uint scene;
    uint output_;
    uint gbufferNormalAo;
    uint gbufferAlbedo;
    uint gbufferRoughMetal;
    uint gbufferDepth;
    uint gbufferEmissive;
    uint shadowsLo; // WP-3.2: BDA of VsmShadowConstants (0 = unshadowed)
    uint shadowsHi;
    float sliceDepth[68];
    float ndcX[36];
    float ndcY[20];
    uint rtShadowsLo; // WP-6.2: BDA of the RtfxShadowView (0 = no ray-traced shadows)
    uint rtShadowsHi;
    uint ddgiLo; // WP-6.1: BDA of the DdgiVolumeView (0 = no DDGI indirect diffuse)
    uint ddgiHi;
};

// GpuClusterAabb / GpuLightBounds, 32 bytes each.
struct FuseLcAabb {
    float minP[3];
    float pad0;
    float maxP[3];
    float pad1;
};
struct FuseLcBounds {
    float center[3];
    float radius;
    uint sliceLo;
    uint sliceHi;
    uint type;
    uint pad;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseLcFrameRef { FuseLcFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FuseLcWordsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer FuseLcAabbsRef { FuseLcAabb v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer FuseLcBoundsRef { FuseLcBounds v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer FuseLcDumpRef { vec4 v[]; };

// LightListHeader word offsets.
#define FUSE_LC_HEADER_TOTAL 0u
#define FUSE_LC_HEADER_AT_CAPACITY 1u
#define FUSE_LC_HEADER_DROPPED 2u
#define FUSE_LC_HEADER_NON_EMPTY 3u
#define FUSE_LC_HEADER_DIRECTIONAL 4u
#define FUSE_LC_HEADER_CLUSTERS 5u
#define FUSE_LC_HEADER_CAPACITY 6u
#define FUSE_LC_HEADER_LIGHTS 7u

vec3 fuse_lc_vec3(float v[3]) { return vec3(v[0], v[1], v[2]); }

// Left-to-right like fuse::math::Vec3::dot.
float fuse_lc_dot(vec3 a, vec3 b) {
    precise float r = a.x * b.x + a.y * b.y + a.z * b.z;
    return r;
}

float fuse_lc_length(vec3 v) { return sqrt(fuse_lc_dot(v, v)); }

// --- oracle camera / slice math (clustered_kernel.hpp) -------------------------------------------
// Positive linear view depth from a device depth value; 0 for the cleared / sky value.
float fuse_lc_view_depth(float d, float n, float f, bool reversedZ) {
    if (reversedZ) {
        return d > 0.0 ? n / d : 0.0;
    }
    if (d >= 1.0 || f <= n) {
        return 0.0;
    }
    precise float r = (n * f) / (f - d * (f - n));
    return r;
}

// Exponential depth slice (clamped to the grid). log differs from libm by a few ulp; callers that
// need the oracle's exact candidates widen the window (FUSE_LC_SLICE_PAD).
uint fuse_lc_slice_from_depth(float d, uint slices, float n, float f) {
    if (slices == 0u || !(n > 0.0) || !(f > n)) {
        return 0u;
    }
    precise float c = clamp(d, n, f);
    precise float ld = max(log(c / n) / log(f / n), 0.0);
    return min(uint(ld * float(slices)), slices - 1u);
}

bool fuse_lc_map_cluster(float sx, float sy, float viewDepth, FuseLcFrame F, out uint cluster) {
    cluster = 0u;
    if (F.tilesX == 0u || F.tilesY == 0u || F.slicesZ == 0u) {
        return false;
    }
    if (viewDepth < F.nearPlane || viewDepth > F.farPlane) {
        return false;
    }
    const uint tx = min(uint(clamp(sx, 0.0, 1.0) * float(F.tilesX)), F.tilesX - 1u);
    const uint ty = min(uint(clamp(sy, 0.0, 1.0) * float(F.tilesY)), F.tilesY - 1u);
    const uint sz = fuse_lc_slice_from_depth(viewDepth, F.slicesZ, F.nearPlane, F.farPlane);
    cluster = (ty * F.tilesX + tx) * F.slicesZ + sz;
    return true;
}

vec3 fuse_lc_view_position(float sx, float sy, float viewDepth, FuseLcFrame F) {
    precise float nx = sx * 2.0 - 1.0;
    precise float ny = 1.0 - sy * 2.0;
    precise vec3 r = vec3(nx * F.tanX * viewDepth, ny * F.tanY * viewDepth, -viewDepth);
    return r;
}

vec3 fuse_lc_view_to_world(vec3 v, FuseLcFrame F) {
    precise vec3 r = fuse_lc_vec3(F.cameraPosition) + fuse_lc_vec3(F.right) * v.x + fuse_lc_vec3(F.up) * v.y +
                     fuse_lc_vec3(F.back) * v.z;
    return r;
}

vec3 fuse_lc_world_to_view(vec3 w, FuseLcFrame F) {
    precise vec3 rel = w - fuse_lc_vec3(F.cameraPosition);
    return vec3(fuse_lc_dot(rel, fuse_lc_vec3(F.right)), fuse_lc_dot(rel, fuse_lc_vec3(F.up)),
                fuse_lc_dot(rel, fuse_lc_vec3(F.back)));
}

// Closed sphere-vs-AABB overlap (touching counts); false for negative or non-finite radius.
bool fuse_lc_sphere_aabb(vec3 c, float radius, FuseLcAabb b) {
    if (!(radius >= 0.0) || isinf(radius)) {
        return false;
    }
    precise float dx = c.x - clamp(c.x, b.minP[0], b.maxP[0]);
    precise float dy = c.y - clamp(c.y, b.minP[1], b.maxP[1]);
    precise float dz = c.z - clamp(c.z, b.minP[2], b.maxP[2]);
    precise float d2 = dx * dx + dy * dy + dz * dz;
    precise float r2 = radius * radius;
    return d2 <= r2;
}

// --- shading (clustered_gpu_kernel.hpp) -----------------------------------------------------------
vec3 fuse_lc_safe_normalize(vec3 v, vec3 fallback) {
    precise float len2 = fuse_lc_dot(v, v);
    if (!(len2 > 0.0) || isinf(len2)) {
        return fallback;
    }
    precise float inv = 1.0 / sqrt(len2);
    precise vec3 r = vec3(v.x * inv, v.y * inv, v.z * inv);
    return r;
}

vec3 fuse_lc_oct_decode(float ox, float oy) {
    precise vec3 n = vec3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        precise float x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        precise float y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return fuse_lc_safe_normalize(n, vec3(0.0, 0.0, 1.0));
}

float fuse_lc_falloff(float d, float radius) {
    if (!(radius > 0.0) || !(d < radius)) {
        return 0.0;
    }
    precise float ratio = d / radius;
    precise float ratio2 = ratio * ratio;
    precise float w = clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    precise float r = (w * w) / max(d * d, 1e-4);
    return r;
}

float fuse_lc_spot_cone(float cosAngle, float cosInner, float cosOuter) {
    precise float t = clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, FUSE_LC_SPOT_MIN_WIDTH), 0.0, 1.0);
    precise float r = t * t;
    return r;
}

struct FuseLcSurface {
    vec3 position;
    vec3 normal;
    vec3 albedo;
    float roughness;
    float metallic;
    float ao;
    vec3 emissive;
};

float fuse_lc_brdf_channel(float albedo, float metallic, float f5, float dv, float kd, float nDotL) {
    precise float f0 = 0.04 + (albedo - 0.04) * metallic;
    precise float fr = f0 + (1.0 - f0) * f5;
    precise float spec = dv * fr;
    precise float diff = (1.0 - fr) * kd * albedo;
    precise float r = (diff + spec) * nDotL;
    return r;
}

// (diffuse + specular) * N.L, 0 when N.L <= 0.
vec3 fuse_lc_brdf_cos(FuseLcSurface s, vec3 v, vec3 l) {
    const float nDotL = fuse_lc_dot(s.normal, l);
    if (!(nDotL > 0.0)) {
        return vec3(0.0);
    }
    precise vec3 hv = v + l;
    const vec3 h = fuse_lc_safe_normalize(hv, s.normal);
    const float nDotV = max(fuse_lc_dot(s.normal, v), 1e-4);
    const float nDotH = max(fuse_lc_dot(s.normal, h), 0.0);
    const float vDotH = max(fuse_lc_dot(v, h), 0.0);
    precise float r = clamp(s.roughness, FUSE_LC_MIN_ROUGHNESS, 1.0);
    precise float a = r * r;
    precise float a2 = a * a;
    precise float dd = (nDotH * a2 - nDotH) * nDotH + 1.0;
    precise float d = a2 / (float(FUSE_LC_PI) * dd * dd);
    precise float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    precise float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    precise float vis = 0.5 / max(gv + gl, 1e-5);
    const float metallic = clamp(s.metallic, 0.0, 1.0);
    precise float f = clamp(1.0 - vDotH, 0.0, 1.0);
    precise float f2 = f * f;
    precise float f5 = f2 * f2 * f;
    precise float dv = d * vis;
    precise float kd = (1.0 - metallic) / float(FUSE_LC_PI);
    return vec3(fuse_lc_brdf_channel(s.albedo.x, metallic, f5, dv, kd, nDotL),
                fuse_lc_brdf_channel(s.albedo.y, metallic, f5, dv, kd, nDotL),
                fuse_lc_brdf_channel(s.albedo.z, metallic, f5, dv, kd, nDotL));
}

vec3 fuse_lc_light(FuseGpuLight light, FuseLcSurface s, vec3 v) {
    vec3 l = vec3(0.0);
    float attenuation = 1.0;
    if (light.type == FUSE_LIGHT_DIRECTIONAL) {
        l = fuse_lc_safe_normalize(-fuse_lc_vec3(light.direction), vec3(0.0, 0.0, 1.0));
    } else if (light.type == FUSE_LIGHT_POINT || light.type == FUSE_LIGHT_SPOT) {
        precise vec3 toLight = fuse_lc_vec3(light.position) - s.position;
        const float distance = fuse_lc_length(toLight);
        attenuation = fuse_lc_falloff(distance, light.range);
        if (attenuation == 0.0) {
            return vec3(0.0);
        }
        precise float inv = 1.0 / max(distance, 1e-6);
        precise vec3 lv = toLight * inv;
        l = lv;
        if (light.type == FUSE_LIGHT_SPOT) {
            const vec3 axis = fuse_lc_safe_normalize(fuse_lc_vec3(light.direction), vec3(0.0, 0.0, -1.0));
            precise float att = attenuation * fuse_lc_spot_cone(-fuse_lc_dot(l, axis), light.cosInner, light.cosOuter);
            attenuation = att;
            if (attenuation == 0.0) {
                return vec3(0.0);
            }
        }
    } else {
        return vec3(0.0);
    }
    const vec3 lobe = fuse_lc_brdf_cos(s, v, l);
    precise float scale = light.intensity * attenuation;
    precise vec3 r = vec3(lobe.x * light.color[0] * scale, lobe.y * light.color[1] * scale, lobe.z * light.color[2] * scale);
    return r;
}

#endif // FUSE_LC_COMMON_GLSL
