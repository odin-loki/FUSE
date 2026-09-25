// WP-8.1 froxel fog: records and the per-froxel math every fog kernel shares, plus the apply helper other
// passes can use to sample the integrated volume (fuse_fog_sample). GLSL twin of fog_common.slang. The C++
// mirror of the records is include/fuse/renderer/volumetric/gpu/froxel_fog_types.hpp; the math is a
// line-for-line port of froxel_fog_kernel.hpp (same f32 expressions in the same order, every arithmetic result
// `precise` so nothing is contracted).
//
// Include after bindless.glsl. Every function reads the frame constants through a FuseFogFrameRef (the BDA
// of FogFrameConstants: FroxelFog::frameConstantsAddress()).
#ifndef FUSE_FOG_COMMON_GLSL
#define FUSE_FOG_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_FOG_FLAG_HISTORY (1u << 0)
#define FUSE_FOG_FLAG_REPROJECT (1u << 1)
#define FUSE_FOG_FLAG_LIGHTS (1u << 2)
#define FUSE_FOG_FLAG_SHADOWS (1u << 3)
#define FUSE_FOG_FLAG_REVERSED_Z (1u << 4)
#define FUSE_FOG_VOLUME_BOX 1u
#define FUSE_FOG_MAX_VOLUMES 8u
#define FUSE_FOG_INV_4PI 0.0795774715459477
#define FUSE_FOG_SERIES_TAU 0.1

// FogVolume, 64 bytes.
struct FuseFogVolume {
    float center[3];
    uint shape;
    float halfExtent[3];
    float density;
    float albedo[3];
    float edge;
    float reserved[4];
};

// FogFrameConstants, 1360 bytes.
struct FuseFogFrame {
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
    FuseFogVolume volumes[8];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseFogFrameRef { FuseFogFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer FuseFogTexelsRef { vec4 v[]; };

vec3 fuse_fog_vec3(float a[3]) { return vec3(a[0], a[1], a[2]); }

float fuse_fog_dot(vec3 a, vec3 b) {
    precise float r = a.x * b.x + a.y * b.y + a.z * b.z;
    return r;
}

float fuse_fog_lerp(float a, float b, float t) {
    precise float r = a + (b - a) * t;
    return r;
}

vec4 fuse_fog_lerp4(vec4 a, vec4 b, float t) {
    return vec4(fuse_fog_lerp(a.x, b.x, t), fuse_fog_lerp(a.y, b.y, t), fuse_fog_lerp(a.z, b.z, t), fuse_fog_lerp(a.w, b.w, t));
}

float fuse_fog_clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }

vec3 fuse_fog_safe_normalize(vec3 v, vec3 fallback) {
    precise float len2 = fuse_fog_dot(v, v);
    if (!(len2 > 0.0) || isinf(len2)) {
        return fallback;
    }
    precise float inv = 1.0 / sqrt(len2);
    precise vec3 r = vec3(v.x * inv, v.y * inv, v.z * inv);
    return r;
}

uint fuse_fog_index(FuseFogFrameRef R, uint x, uint y, uint z) { return (y * R.f.gridX + x) * R.f.gridZ + z; }

// View-space point of the fog camera -> world.
vec3 fuse_fog_world(FuseFogFrameRef R, float sx, float sy, float depth) {
    precise float nx = sx * 2.0 - 1.0;
    precise float ny = 1.0 - sy * 2.0;
    precise float vx = nx * R.f.tanX * depth;
    precise float vy = ny * R.f.tanY * depth;
    precise float vz = -depth;
    precise float wx = R.f.position[0] + R.f.right[0] * vx + R.f.up[0] * vy + R.f.back[0] * vz;
    precise float wy = R.f.position[1] + R.f.right[1] * vx + R.f.up[1] * vy + R.f.back[1] * vz;
    precise float wz = R.f.position[2] + R.f.right[2] * vx + R.f.up[2] * vy + R.f.back[2] * vz;
    return vec3(wx, wy, wz);
}

// Slice s with sliceDepth[s] <= d < sliceDepth[s + 1] (0 below, gridZ - 1 above).
uint fuse_fog_find_slice(FuseFogFrameRef R, float d) {
    uint lo = 0u;
    uint hi = R.f.gridZ;
    while (hi - lo > 1u) {
        const uint mid = (lo + hi) / 2u;
        if (d >= R.f.sliceDepth[mid]) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

vec3 fuse_fog_froxel_point(FuseFogFrameRef R, uint x, uint y, uint z, float jx, float jy, float jz) {
    precise float sx = (float(x) + jx) * R.f.invGridX;
    precise float sy = (float(y) + jy) * R.f.invGridY;
    const float z0 = R.f.sliceDepth[z];
    const float z1 = R.f.sliceDepth[z + 1u];
    precise float depth = z0 + (z1 - z0) * jz;
    return fuse_fog_world(R, sx, sy, depth);
}

// --- medium -----------------------------------------------------------------------------------------
float fuse_fog_volume_weight(FuseFogVolume v, vec3 p) {
    precise float dx = p.x - v.center[0];
    precise float dy = p.y - v.center[1];
    precise float dz = p.z - v.center[2];
    precise float d = 0.0;
    if (v.shape == FUSE_FOG_VOLUME_BOX) {
        precise float ax = abs(dx) / v.halfExtent[0];
        precise float ay = abs(dy) / v.halfExtent[1];
        precise float az = abs(dz) / v.halfExtent[2];
        d = max(max(ax, ay), az);
    } else {
        precise float len = sqrt(dx * dx + dy * dy + dz * dz);
        d = len / v.halfExtent[0];
    }
    if (!(d < 1.0)) {
        return 0.0;
    }
    precise float w = (1.0 - d) / max(v.edge, 1e-4);
    return min(w, 1.0);
}

float fuse_fog_medium(FuseFogFrameRef R, vec3 p, out vec3 scattering) {
    precise float heightDelta = p.y - R.f.baseHeight;
    precise float arg = -R.f.heightFalloff * max(0.0, heightDelta);
    precise float fog = R.f.density * exp(arg);
    precise float extinction = fog;
    precise vec3 s = vec3(R.f.albedo[0] * fog, R.f.albedo[1] * fog, R.f.albedo[2] * fog);
    const uint count = min(R.f.volumeCount, FUSE_FOG_MAX_VOLUMES);
    for (uint i = 0u; i < count; ++i) {
        const FuseFogVolume v = R.f.volumes[i];
        const float w = fuse_fog_volume_weight(v, p);
        if (w > 0.0) {
            precise float e = v.density * w;
            extinction = extinction + e;
            s = vec3(s.x + v.albedo[0] * e, s.y + v.albedo[1] * e, s.z + v.albedo[2] * e);
        }
    }
    scattering = s;
    return extinction;
}

// --- temporal / apply sampling ------------------------------------------------------------------------
vec4 fuse_fog_sample_grid(FuseFogFrameRef R, FuseFogTexelsRef buf, float gx, float gy, float gz) {
    const float cx = fuse_fog_clamp(gx, 0.0, float(R.f.gridX - 1u));
    const float cy = fuse_fog_clamp(gy, 0.0, float(R.f.gridY - 1u));
    const float cz = fuse_fog_clamp(gz, 0.0, float(R.f.gridZ - 1u));
    const uint x0 = uint(cx);
    const uint y0 = uint(cy);
    const uint z0 = uint(cz);
    const uint x1 = min(x0 + 1u, R.f.gridX - 1u);
    const uint y1 = min(y0 + 1u, R.f.gridY - 1u);
    const uint z1 = min(z0 + 1u, R.f.gridZ - 1u);
    precise float wx = cx - float(x0);
    precise float wy = cy - float(y0);
    precise float wz = cz - float(z0);
    const vec4 c00 = fuse_fog_lerp4(buf.v[fuse_fog_index(R, x0, y0, z0)], buf.v[fuse_fog_index(R, x0, y0, z1)], wz);
    const vec4 c10 = fuse_fog_lerp4(buf.v[fuse_fog_index(R, x1, y0, z0)], buf.v[fuse_fog_index(R, x1, y0, z1)], wz);
    const vec4 c01 = fuse_fog_lerp4(buf.v[fuse_fog_index(R, x0, y1, z0)], buf.v[fuse_fog_index(R, x0, y1, z1)], wz);
    const vec4 c11 = fuse_fog_lerp4(buf.v[fuse_fog_index(R, x1, y1, z0)], buf.v[fuse_fog_index(R, x1, y1, z1)], wz);
    return fuse_fog_lerp4(fuse_fog_lerp4(c00, c10, wx), fuse_fog_lerp4(c01, c11, wx), wy);
}

vec4 fuse_fog_column_at(FuseFogFrameRef R, FuseFogTexelsRef vol, uint x, uint y, uint k, float f) {
    const vec4 hi = vol.v[fuse_fog_index(R, x, y, k)];
    const vec4 lo = k == 0u ? vec4(0.0, 0.0, 0.0, 1.0) : vol.v[fuse_fog_index(R, x, y, k - 1u)];
    return fuse_fog_lerp4(lo, hi, f);
}

// The apply helper: (in-scattering, transmittance) between the camera and view depth `depth` along the ray
// through screen point (sx, sy) of the fog camera (0, 0 = top left). `vol` = the integrated volume
// (FroxelFog::integratedAddress()). A pass calling it declares a StorageRead of FogGraphRefs::work.
vec4 fuse_fog_sample(FuseFogFrameRef R, FuseFogTexelsRef vol, float sx, float sy, float depth) {
    const float d = fuse_fog_clamp(depth, 0.0, R.f.farPlane);
    const uint k = fuse_fog_find_slice(R, d);
    const float lo = k == 0u ? 0.0 : R.f.sliceDepth[k];
    precise float ft = (d - lo) / (R.f.sliceDepth[k + 1u] - lo);
    const float f = fuse_fog_clamp(ft, 0.0, 1.0);
    precise float gx = sx * float(R.f.gridX) - 0.5;
    precise float gy = sy * float(R.f.gridY) - 0.5;
    const float cx = fuse_fog_clamp(gx, 0.0, float(R.f.gridX - 1u));
    const float cy = fuse_fog_clamp(gy, 0.0, float(R.f.gridY - 1u));
    const uint x0 = uint(cx);
    const uint y0 = uint(cy);
    const uint x1 = min(x0 + 1u, R.f.gridX - 1u);
    const uint y1 = min(y0 + 1u, R.f.gridY - 1u);
    precise float wx = cx - float(x0);
    precise float wy = cy - float(y0);
    const vec4 c00 = fuse_fog_column_at(R, vol, x0, y0, k, f);
    const vec4 c10 = fuse_fog_column_at(R, vol, x1, y0, k, f);
    const vec4 c01 = fuse_fog_column_at(R, vol, x0, y1, k, f);
    const vec4 c11 = fuse_fog_column_at(R, vol, x1, y1, k, f);
    return fuse_fog_lerp4(fuse_fog_lerp4(c00, c10, wx), fuse_fog_lerp4(c01, c11, wx), wy);
}

#endif // FUSE_FOG_COMMON_GLSL
