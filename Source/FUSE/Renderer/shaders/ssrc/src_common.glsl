// Screen-space radiance cascades: records, push constants and the helpers the kernels share. GLSL twin of
// src_common.slang. The C++ mirror of the records is include/fuse/renderer/ssrc/ssrc_types.hpp; the math is a
// line-for-line port of src/ssrc/ssrc_reference.cpp (every arithmetic result held in a `precise` variable, so no
// multiply-add is contracted and the kernels keep the reference's IEEE operations and order).
#ifndef FUSE_SRC_COMMON_GLSL
#define FUSE_SRC_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_samplerless_texture_functions : require
#include "bindless.glsl"

#define SRC_FLAG_BILINEAR_FIX (1u << 0)
#define SRC_FLAG_REVERSED_Z (1u << 1)
#define SRC_FLAG_DDGI (1u << 2)
#define SRC_FLAG_COMPOSE (1u << 3)

#define SRC_MAX_CASCADES 8

// SsrcFrameConstants, 608 bytes.
struct SrcFrame {
    uint64_t geo;
    uint64_t lit;
    uint64_t diffuse;
    uint64_t albedo;
    uint64_t indirect;
    uint64_t records0;
    uint64_t records1;
    uint64_t dirs;
    uint64_t ddgiVolume;
    uint64_t dump;
    uint64_t reserved0;
    uint64_t reserved1;
    uint width;
    uint height;
    uint inputDepth;
    uint inputNormal;
    uint inputAlbedo;
    uint inputRoughMetal;
    uint inputLit;
    uint output_;
    uint flags;
    uint cascades;
    uint aoCascades;
    uint reserved2;
    float fx;
    float fy;
    float cx;
    float cy;
    float nearZ;
    float nearPlane;
    float farPlane;
    float reserved3;
    float viewRot[12];
    float viewToWorld[12];
    float cameraWorld[4];
    float ambient[4];
    float sky[4];
    float stride;
    float thickness;
    float thicknessSlope;
    float maxDistance;
    float originBias;
    float intensity;
    float ddgiScale;
    float planeTolerance;
    uint probesX[8];
    uint probesY[8];
    uint dirRes[8];
    uint dirOffset[8];
    float spacing[8];
    float tStart[8];
    float tEnd[8];
    float reserved5[8];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SrcFrameRef { SrcFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer SrcTexelsRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SrcRecordsRef { uvec2 v[]; };

// SsrcPush, 40 bytes.
layout(push_constant) uniform SrcPush {
    uint64_t frame;
    uint64_t upper;
    uint64_t dst;
    uint cascade;
    uint count;
    uint groupsX;
    uint reserved;
} pc;

// The frame constants: every F.field is a load through the frame's BDA (uniform, cached).
#define F (SrcFrameRef(pc.frame).f)

#ifdef SRC_MATH
// --- f16 records (ssrc::f32ToF16 / f16ToF32) ------------------------------------------------------------
uint src_f32_to_f16(float v) {
    float m = v;
    if (m > 65504.0) {
        m = 65504.0;
    }
    if (m < -65504.0) {
        m = -65504.0;
    }
    uint u = floatBitsToUint(m);
    const uint sign = u & 0x80000000u;
    u ^= sign;
    uint o = 0u;
    if (u >= 0x47800000u) {
        o = u > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (u < 0x38800000u) {
        precise float t = uintBitsToFloat(u) + 0.5;
        o = floatBitsToUint(t) - 0x3F000000u;
    } else {
        const uint odd = (u >> 13) & 1u;
        u += 0xC8000FFFu;
        u += odd;
        o = u >> 13;
    }
    return o | (sign >> 16);
}

float src_f16_to_f32(uint h) {
    const uint sign = (h & 0x8000u) << 16;
    const uint e = (h >> 10) & 0x1Fu;
    const uint m = h & 0x3FFu;
    if (e == 0u) {
        precise float f = float(m) * 5.9604644775390625e-8;
        return sign != 0u ? -f : f;
    }
    if (e == 31u) {
        return uintBitsToFloat(sign | 0x7F800000u | (m << 13));
    }
    return uintBitsToFloat(sign | ((e + 112u) << 23) | (m << 13));
}

vec4 src_decode(uvec2 r) {
    return vec4(src_f16_to_f32(r.x & 0xFFFFu), src_f16_to_f32(r.x >> 16), src_f16_to_f32(r.y & 0xFFFFu),
                src_f16_to_f32(r.y >> 16));
}

// --- vector helpers -----------------------------------------------------------------------------------
float src_dot(vec3 a, vec3 b) {
    precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
    return d;
}
vec3 src_scale(vec3 v, float s) {
    precise vec3 r = vec3(v.x * s, v.y * s, v.z * s);
    return r;
}
vec3 src_add(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.x + b.x, a.y + b.y, a.z + b.z);
    return r;
}
vec3 src_sub(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.x - b.x, a.y - b.y, a.z - b.z);
    return r;
}
vec3 src_normalize(vec3 v) {
    precise float len = sqrt(src_dot(v, v));
    if (len < 1e-8) {
        return vec3(0.0);
    }
    precise float inv = 1.0 / len;
    return src_scale(v, inv);
}
// ((p + 0.5) - c) / f: the view ray (z = 1) through a pixel centre.
float src_ray_coord(uint p, float c, float f) {
    precise float r = ((float(p) + 0.5) - c) / f;
    return r;
}

uint src_clamp_index(int v, uint count) {
    if (v < 0) {
        return 0u;
    }
    const uint u = uint(v);
    return u >= count ? count - 1u : u;
}

// Bilinear neighbours of p on a probe grid of `spacing`: xy = (x0, x1), zw = (y0, y1); weights in w
// ((x0, y0), (x1, y0), (x0, y1), (x1, y1)).
uvec4 src_bilinear(float px, float py, float spacing, uint countX, uint countY, out vec4 w) {
    precise float u = px / spacing - 0.5;
    precise float v = py / spacing - 0.5;
    const float fu = floor(u);
    const float fv = floor(v);
    precise float ax = u - fu;
    precise float ay = v - fv;
    const int ix = int(fu);
    const int iy = int(fv);
    precise float bx = 1.0 - ax;
    precise float by = 1.0 - ay;
    precise float w0 = bx * by;
    precise float w1 = ax * by;
    precise float w2 = bx * ay;
    precise float w3 = ax * ay;
    w = vec4(w0, w1, w2, w3);
    return uvec4(src_clamp_index(ix, countX), src_clamp_index(ix + 1, countX), src_clamp_index(iy, countY),
                 src_clamp_index(iy + 1, countY));
}

// --- probes (ssrc_reference.cpp probeAt) ---------------------------------------------------------------
struct SrcProbe {
    bool valid;
    uint pixel;
    vec3 o;
    vec3 n; // camera-facing unit normal of the probe pixel
};

float src_grid(uint p, float s) {
    precise float g = (float(p) + 0.5) * s;
    return g;
}

SrcProbe src_probe_at(uint ci, uint probeX, uint probeY) {
    SrcProbe p;
    p.valid = false;
    p.o = vec3(0.0);
    p.n = vec3(0.0);
    const float s = F.spacing[ci];
    const uint ux = min(uint(src_grid(probeX, s)), F.width - 1u);
    const uint uy = min(uint(src_grid(probeY, s)), F.height - 1u);
    p.pixel = uy * F.width + ux;
    const vec4 g = SrcTexelsRef(F.geo).v[p.pixel];
    const float z = g.x;
    if (!(z > 0.0)) {
        return p;
    }
    const float a = src_ray_coord(ux, F.cx, F.fx);
    const float b = src_ray_coord(uy, F.cy, F.fy);
    precise float pxz = a * z;
    precise float pyz = b * z;
    vec3 n = src_normalize(g.yzw);
    if (src_dot(n, vec3(a, b, 1.0)) > 0.0) {
        n = src_scale(n, -1.0);
    }
    precise float bias = F.originBias * z;
    p.o = src_add(vec3(pxz, pyz, z), src_scale(n, bias));
    p.n = n;
    p.valid = true;
    return p;
}
// Gather weight of cascade 0 direction k (ssrc_reference.cpp binWeight): the cosine-weighted solid angle of the bin
// summed over its 4 cascade-1 children (the bin centre alone with a single cascade).
float src_bin_weight(uint k, vec3 n) {
    if (F.cascades < 2u) {
        const vec4 dt = SrcTexelsRef(F.dirs).v[k];
        precise float w0 = max(0.0, src_dot(n, dt.xyz)) * dt.w;
        return w0;
    }
    const uint res = F.dirRes[0];
    const uint res2 = F.dirRes[1];
    const uint ku = k % res;
    const uint kv = k / res;
    precise float w = 0.0;
    for (uint j = 0u; j < 4u; ++j) {
        const uint ck = (2u * kv + (j >> 1u)) * res2 + 2u * ku + (j & 1u);
        const vec4 dt = SrcTexelsRef(F.dirs).v[F.dirOffset[1] + ck];
        const float cosine = max(0.0, src_dot(n, dt.xyz));
        w = w + cosine * dt.w;
    }
    return w;
}
#endif // SRC_MATH

#endif
