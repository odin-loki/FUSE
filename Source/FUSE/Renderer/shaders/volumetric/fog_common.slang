// WP-8.1 froxel fog: Slang twin of fog_common.glsl (see there). Compile with -fp-mode precise (Slang does not
// honour `precise` per variable), so no multiply-add is contracted. Functions take the frame constants as a
// pointer (FogFrame*, the BDA of FogFrameConstants).
#pragma once

static const uint kFogFlagHistory = 1u << 0;
static const uint kFogFlagReproject = 1u << 1;
static const uint kFogFlagLights = 1u << 2;
static const uint kFogFlagShadows = 1u << 3;
static const uint kFogFlagReversedZ = 1u << 4;
static const uint kFogVolumeBox = 1u;
static const uint kFogMaxVolumes = 8u;
static const float kFogInv4Pi = 0.0795774715459477;
static const float kFogSeriesTau = 0.1;

// FogVolume, 64 bytes.
struct FogVolume {
    float center[3];
    uint shape;
    float halfExtent[3];
    float density;
    float albedo[3];
    float edge;
    float reserved[4];
};

// FogFrameConstants, 1360 bytes.
struct FogFrame {
    uint64_t lighting;
    uint64_t shadows;
    uint64_t current;
    uint64_t historyPrev;
    uint64_t historyCur;
    uint64_t integrated;
    uint64_t dump;
    uint64_t reserved0;
    uint gridX;
    uint gridY;
    uint gridZ;
    uint froxelCount;
    uint width;
    uint height;
    uint inputDepth;
    uint inputLit;
    uint output_;
    uint flags;
    uint frameIndex;
    uint volumeCount;
    float position[3];
    float nearPlane;
    float right[3];
    float farPlane;
    float up[3];
    float tanX;
    float back[3];
    float tanY;
    float prevPosition[3];
    float reserved1;
    float prevRight[3];
    float prevTanX;
    float prevUp[3];
    float prevTanY;
    float prevBack[3];
    float reserved2;
    float jitter[3];
    float temporalAlpha;
    float density;
    float heightFalloff;
    float baseHeight;
    float anisotropy;
    float albedo[3];
    float invWidth;
    float ambient[3];
    float depthNear;
    float depthFar;
    float invGridX;
    float invGridY;
    float invHeight;
    float sliceDepth[132];
    FogVolume volumes[8];
};

struct FogPush {
    uint64_t frame;
    uint64_t reserved;
};

float3 fogVec3(float a[3]) { return float3(a[0], a[1], a[2]); }
float fogDot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float fogLerp(float a, float b, float t) { return a + (b - a) * t; }
float4 fogLerp4(float4 a, float4 b, float t) {
    return float4(fogLerp(a.x, b.x, t), fogLerp(a.y, b.y, t), fogLerp(a.z, b.z, t), fogLerp(a.w, b.w, t));
}
float fogClamp(float v, float lo, float hi) { return min(max(v, lo), hi); }

float3 fogSafeNormalize(float3 v, float3 fallback) {
    const float len2 = fogDot(v, v);
    if (!(len2 > 0.0) || isinf(len2)) {
        return fallback;
    }
    const float inv = 1.0 / sqrt(len2);
    return float3(v.x * inv, v.y * inv, v.z * inv);
}

uint fogIndex(FogFrame* R, uint x, uint y, uint z) { return (y * R.gridX + x) * R.gridZ + z; }

float3 fogWorld(FogFrame* R, float sx, float sy, float depth) {
    const float nx = sx * 2.0 - 1.0;
    const float ny = 1.0 - sy * 2.0;
    const float vx = nx * R.tanX * depth;
    const float vy = ny * R.tanY * depth;
    const float vz = -depth;
    return float3(R.position[0] + R.right[0] * vx + R.up[0] * vy + R.back[0] * vz,
                  R.position[1] + R.right[1] * vx + R.up[1] * vy + R.back[1] * vz,
                  R.position[2] + R.right[2] * vx + R.up[2] * vy + R.back[2] * vz);
}

uint fogFindSlice(FogFrame* R, float d) {
    uint lo = 0u;
    uint hi = R.gridZ;
    while (hi - lo > 1u) {
        const uint mid = (lo + hi) / 2u;
        if (d >= R.sliceDepth[mid]) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

float3 fogFroxelPoint(FogFrame* R, uint x, uint y, uint z, float jx, float jy, float jz) {
    const float sx = (float(x) + jx) * R.invGridX;
    const float sy = (float(y) + jy) * R.invGridY;
    const float z0 = R.sliceDepth[z];
    const float z1 = R.sliceDepth[z + 1u];
    const float depth = z0 + (z1 - z0) * jz;
    return fogWorld(R, sx, sy, depth);
}

// --- medium -----------------------------------------------------------------------------------------
float fogVolumeWeight(FogVolume v, float3 p) {
    const float dx = p.x - v.center[0];
    const float dy = p.y - v.center[1];
    const float dz = p.z - v.center[2];
    float d = 0.0;
    if (v.shape == kFogVolumeBox) {
        d = max(max(abs(dx) / v.halfExtent[0], abs(dy) / v.halfExtent[1]), abs(dz) / v.halfExtent[2]);
    } else {
        d = sqrt(dx * dx + dy * dy + dz * dz) / v.halfExtent[0];
    }
    if (!(d < 1.0)) {
        return 0.0;
    }
    return min((1.0 - d) / max(v.edge, 1e-4), 1.0);
}

float fogMedium(FogFrame* R, float3 p, out float3 scattering) {
    const float heightDelta = p.y - R.baseHeight;
    const float fog = R.density * exp(-R.heightFalloff * max(0.0, heightDelta));
    float extinction = fog;
    float3 s = float3(R.albedo[0] * fog, R.albedo[1] * fog, R.albedo[2] * fog);
    const uint count = min(R.volumeCount, kFogMaxVolumes);
    for (uint i = 0u; i < count; ++i) {
        const FogVolume v = R.volumes[i];
        const float w = fogVolumeWeight(v, p);
        if (w > 0.0) {
            const float e = v.density * w;
            extinction = extinction + e;
            s = float3(s.x + v.albedo[0] * e, s.y + v.albedo[1] * e, s.z + v.albedo[2] * e);
        }
    }
    scattering = s;
    return extinction;
}

// --- temporal / apply sampling ------------------------------------------------------------------------
float4 fogSampleGrid(FogFrame* R, float4* buf, float gx, float gy, float gz) {
    const float cx = fogClamp(gx, 0.0, float(R.gridX - 1u));
    const float cy = fogClamp(gy, 0.0, float(R.gridY - 1u));
    const float cz = fogClamp(gz, 0.0, float(R.gridZ - 1u));
    const uint x0 = uint(cx);
    const uint y0 = uint(cy);
    const uint z0 = uint(cz);
    const uint x1 = min(x0 + 1u, R.gridX - 1u);
    const uint y1 = min(y0 + 1u, R.gridY - 1u);
    const uint z1 = min(z0 + 1u, R.gridZ - 1u);
    const float wx = cx - float(x0);
    const float wy = cy - float(y0);
    const float wz = cz - float(z0);
    const float4 c00 = fogLerp4(buf[fogIndex(R, x0, y0, z0)], buf[fogIndex(R, x0, y0, z1)], wz);
    const float4 c10 = fogLerp4(buf[fogIndex(R, x1, y0, z0)], buf[fogIndex(R, x1, y0, z1)], wz);
    const float4 c01 = fogLerp4(buf[fogIndex(R, x0, y1, z0)], buf[fogIndex(R, x0, y1, z1)], wz);
    const float4 c11 = fogLerp4(buf[fogIndex(R, x1, y1, z0)], buf[fogIndex(R, x1, y1, z1)], wz);
    return fogLerp4(fogLerp4(c00, c10, wx), fogLerp4(c01, c11, wx), wy);
}

float4 fogColumnAt(FogFrame* R, float4* vol, uint x, uint y, uint k, float f) {
    const float4 hi = vol[fogIndex(R, x, y, k)];
    const float4 lo = k == 0u ? float4(0.0, 0.0, 0.0, 1.0) : vol[fogIndex(R, x, y, k - 1u)];
    return fogLerp4(lo, hi, f);
}

// The apply helper (fuse_fog_sample of fog_common.glsl).
float4 fogSample(FogFrame* R, float4* vol, float sx, float sy, float depth) {
    const float d = fogClamp(depth, 0.0, R.farPlane);
    const uint k = fogFindSlice(R, d);
    const float lo = k == 0u ? 0.0 : R.sliceDepth[k];
    const float f = fogClamp((d - lo) / (R.sliceDepth[k + 1u] - lo), 0.0, 1.0);
    const float cx = fogClamp(sx * float(R.gridX) - 0.5, 0.0, float(R.gridX - 1u));
    const float cy = fogClamp(sy * float(R.gridY) - 0.5, 0.0, float(R.gridY - 1u));
    const uint x0 = uint(cx);
    const uint y0 = uint(cy);
    const uint x1 = min(x0 + 1u, R.gridX - 1u);
    const uint y1 = min(y0 + 1u, R.gridY - 1u);
    const float wx = cx - float(x0);
    const float wy = cy - float(y0);
    const float4 c00 = fogColumnAt(R, vol, x0, y0, k, f);
    const float4 c10 = fogColumnAt(R, vol, x1, y0, k, f);
    const float4 c01 = fogColumnAt(R, vol, x0, y1, k, f);
    const float4 c11 = fogColumnAt(R, vol, x1, y1, k, f);
    return fogLerp4(fogLerp4(c00, c10, wx), fogLerp4(c01, c11, wx), wy);
}
