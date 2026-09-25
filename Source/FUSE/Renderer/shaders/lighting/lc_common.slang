// FUSE GPU clustered lighting (WP-2.1): Slang twin of lc_common.glsl (see there). #include after
// gpu_scene.slang. Compile with -fp-mode precise (Slang does not honour `precise` per variable), so no
// multiply-add is contracted and the cluster AABBs / light spheres are the oracle's bit for bit.
#pragma once

static const uint kFuseLcSlicePad = 2u;
static const uint kFuseLcFlagReversedZ = 1u;
static const uint kFuseLcFlagSkipAreaLights = 2u; // light.shade (pixel given): no rectangle / disk lights
static const float kFuseLcPi = 3.14159265358979;
static const float kFuseLcMinRoughness = 0.045;
static const float kFuseLcSpotMinWidth = 1e-4;
static const uint kFuseLightDirectional = 1u;
static const uint kFuseLightPoint = 2u;
static const uint kFuseLightSpot = 3u;

static const uint kFuseLcHeaderTotal = 0u;
static const uint kFuseLcHeaderAtCapacity = 1u;
static const uint kFuseLcHeaderDropped = 2u;
static const uint kFuseLcHeaderNonEmpty = 3u;
static const uint kFuseLcHeaderDirectional = 4u;
static const uint kFuseLcHeaderClusters = 5u;
static const uint kFuseLcHeaderCapacity = 6u;
static const uint kFuseLcHeaderLights = 7u;

// LightingFrameConstants, 768 bytes.
struct LcFrame {
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

struct LcAabb {
    float minP[3];
    float pad0;
    float maxP[3];
    float pad1;
};

struct LcBounds {
    float center[3];
    float radius;
    uint sliceLo;
    uint sliceHi;
    uint type;
    uint pad;
};

float3 fuseLcVec3(float v[3]) { return float3(v[0], v[1], v[2]); }

float fuseLcDot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

float fuseLcLength(float3 v) { return sqrt(fuseLcDot(v, v)); }

float fuseLcViewDepth(float d, float n, float f, bool reversedZ) {
    if (reversedZ) {
        return d > 0.0 ? n / d : 0.0;
    }
    if (d >= 1.0 || f <= n) {
        return 0.0;
    }
    return (n * f) / (f - d * (f - n));
}

uint fuseLcSliceFromDepth(float d, uint slices, float n, float f) {
    if (slices == 0u || !(n > 0.0) || !(f > n)) {
        return 0u;
    }
    const float c = clamp(d, n, f);
    const float ld = max(log(c / n) / log(f / n), 0.0);
    return min(uint(ld * float(slices)), slices - 1u);
}

bool fuseLcMapCluster(float sx, float sy, float viewDepth, LcFrame F, out uint cluster) {
    cluster = 0u;
    if (F.tilesX == 0u || F.tilesY == 0u || F.slicesZ == 0u) {
        return false;
    }
    if (viewDepth < F.nearPlane || viewDepth > F.farPlane) {
        return false;
    }
    const uint tx = min(uint(clamp(sx, 0.0, 1.0) * float(F.tilesX)), F.tilesX - 1u);
    const uint ty = min(uint(clamp(sy, 0.0, 1.0) * float(F.tilesY)), F.tilesY - 1u);
    const uint sz = fuseLcSliceFromDepth(viewDepth, F.slicesZ, F.nearPlane, F.farPlane);
    cluster = (ty * F.tilesX + tx) * F.slicesZ + sz;
    return true;
}

float3 fuseLcViewPosition(float sx, float sy, float viewDepth, LcFrame F) {
    const float nx = sx * 2.0 - 1.0;
    const float ny = 1.0 - sy * 2.0;
    return float3(nx * F.tanX * viewDepth, ny * F.tanY * viewDepth, -viewDepth);
}

float3 fuseLcViewToWorld(float3 v, LcFrame F) {
    return fuseLcVec3(F.cameraPosition) + fuseLcVec3(F.right) * v.x + fuseLcVec3(F.up) * v.y + fuseLcVec3(F.back) * v.z;
}

float3 fuseLcWorldToView(float3 w, LcFrame F) {
    const float3 rel = w - fuseLcVec3(F.cameraPosition);
    return float3(fuseLcDot(rel, fuseLcVec3(F.right)), fuseLcDot(rel, fuseLcVec3(F.up)), fuseLcDot(rel, fuseLcVec3(F.back)));
}

bool fuseLcSphereAabb(float3 c, float radius, LcAabb b) {
    if (!(radius >= 0.0) || isinf(radius)) {
        return false;
    }
    const float dx = c.x - clamp(c.x, b.minP[0], b.maxP[0]);
    const float dy = c.y - clamp(c.y, b.minP[1], b.maxP[1]);
    const float dz = c.z - clamp(c.z, b.minP[2], b.maxP[2]);
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

float3 fuseLcSafeNormalize(float3 v, float3 fallback) {
    const float len2 = fuseLcDot(v, v);
    if (!(len2 > 0.0) || isinf(len2)) {
        return fallback;
    }
    const float inv = 1.0 / sqrt(len2);
    return float3(v.x * inv, v.y * inv, v.z * inv);
}

float3 fuseLcOctDecode(float ox, float oy) {
    float3 n = float3(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return fuseLcSafeNormalize(n, float3(0.0, 0.0, 1.0));
}

float fuseLcFalloff(float d, float radius) {
    if (!(radius > 0.0) || !(d < radius)) {
        return 0.0;
    }
    const float ratio = d / radius;
    const float ratio2 = ratio * ratio;
    const float w = clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    return (w * w) / max(d * d, 1e-4);
}

float fuseLcSpotCone(float cosAngle, float cosInner, float cosOuter) {
    const float t = clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, kFuseLcSpotMinWidth), 0.0, 1.0);
    return t * t;
}

struct LcSurface {
    float3 position;
    float3 normal;
    float3 albedo;
    float roughness;
    float metallic;
    float ao;
    float3 emissive;
};

float fuseLcBrdfChannel(float albedo, float metallic, float f5, float dv, float kd, float nDotL) {
    const float f0 = 0.04 + (albedo - 0.04) * metallic;
    const float fr = f0 + (1.0 - f0) * f5;
    const float spec = dv * fr;
    const float diff = (1.0 - fr) * kd * albedo;
    return (diff + spec) * nDotL;
}

float3 fuseLcBrdfCos(LcSurface s, float3 v, float3 l) {
    const float nDotL = fuseLcDot(s.normal, l);
    if (!(nDotL > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 h = fuseLcSafeNormalize(v + l, s.normal);
    const float nDotV = max(fuseLcDot(s.normal, v), 1e-4);
    const float nDotH = max(fuseLcDot(s.normal, h), 0.0);
    const float vDotH = max(fuseLcDot(v, h), 0.0);
    const float r = clamp(s.roughness, kFuseLcMinRoughness, 1.0);
    const float a = r * r;
    const float a2 = a * a;
    const float dd = (nDotH * a2 - nDotH) * nDotH + 1.0;
    const float d = a2 / (kFuseLcPi * dd * dd);
    const float gv = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    const float gl = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    const float vis = 0.5 / max(gv + gl, 1e-5);
    const float metallic = clamp(s.metallic, 0.0, 1.0);
    const float f = clamp(1.0 - vDotH, 0.0, 1.0);
    const float f2 = f * f;
    const float f5 = f2 * f2 * f;
    const float dv = d * vis;
    const float kd = (1.0 - metallic) / kFuseLcPi;
    return float3(fuseLcBrdfChannel(s.albedo.x, metallic, f5, dv, kd, nDotL),
                  fuseLcBrdfChannel(s.albedo.y, metallic, f5, dv, kd, nDotL),
                  fuseLcBrdfChannel(s.albedo.z, metallic, f5, dv, kd, nDotL));
}

float3 fuseLcLight(GpuLight light, LcSurface s, float3 v) {
    float3 l = float3(0.0, 0.0, 0.0);
    float attenuation = 1.0;
    if (light.type == kFuseLightDirectional) {
        l = fuseLcSafeNormalize(-fuseLcVec3(light.direction), float3(0.0, 0.0, 1.0));
    } else if (light.type == kFuseLightPoint || light.type == kFuseLightSpot) {
        const float3 toLight = fuseLcVec3(light.position) - s.position;
        const float distance = fuseLcLength(toLight);
        attenuation = fuseLcFalloff(distance, light.range);
        if (attenuation == 0.0) {
            return float3(0.0, 0.0, 0.0);
        }
        l = toLight * (1.0 / max(distance, 1e-6));
        if (light.type == kFuseLightSpot) {
            const float3 axis = fuseLcSafeNormalize(fuseLcVec3(light.direction), float3(0.0, 0.0, -1.0));
            attenuation = attenuation * fuseLcSpotCone(-fuseLcDot(l, axis), light.cosInner, light.cosOuter);
            if (attenuation == 0.0) {
                return float3(0.0, 0.0, 0.0);
            }
        }
    } else {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 lobe = fuseLcBrdfCos(s, v, l);
    const float scale = light.intensity * attenuation;
    return float3(lobe.x * light.color[0] * scale, lobe.y * light.color[1] * scale, lobe.z * light.color[2] * scale);
}
