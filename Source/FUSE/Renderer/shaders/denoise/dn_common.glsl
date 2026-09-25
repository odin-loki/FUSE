// WP-6.4 SVGF / A-SVGF: records, push constants and the per-pixel math every denoiser kernel shares.
// GLSL twin of dn_common.slang. C++ mirror of the records: include/fuse/renderer/denoise/denoise_types.hpp;
// CPU reference of the math: include/fuse/renderer/denoise/svgf_kernel.hpp (same operations in the same
// order; every value is `precise`, so no multiply-add is contracted).
#ifndef FUSE_DN_COMMON_GLSL
#define FUSE_DN_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_samplerless_texture_functions : require
#include "bindless.glsl"

#define DN_FLAG_HISTORY (1u << 0)
#define DN_FLAG_GRADIENTS (1u << 1)
#define DN_FLAG_SCALAR (1u << 2)
#define DN_FLAG_NORMAL_OCT (1u << 3)
#define DN_FLAG_NORMAL_IMAGE (1u << 4)
#define DN_FLAG_SPATIAL_VARIANCE (1u << 5)

#define DN_PASS_WRITE_IMAGE (1u << 0)

#define DN_GRADIENT_PREPARE 0u
#define DN_GRADIENT_ATROUS 1u

#define DN_STRATUM 3u

// DenoiseFrameConstants, 240 bytes.
struct DnFrame {
    uint64_t signal;
    uint64_t motion;
    uint64_t depth;
    uint64_t normal;
    uint64_t gradientIn;
    uint64_t guideCur;
    uint64_t guidePrev;
    uint64_t gradZ;
    uint64_t histPrev;
    uint64_t histCur;
    uint64_t momPrev;
    uint64_t momCur;
    uint64_t accum;
    uint64_t lambda;
    uint64_t output_;
    uint64_t reserved0;
    uint width;
    uint height;
    uint strataW;
    uint strataH;
    uint normalImage;
    uint outputImage;
    uint flags;
    uint signalStride;
    float alphaColor;
    float alphaMoments;
    float maxHistory;
    float reprojDepth;
    float reprojNormal;
    float minReprojWeight;
    float varianceHistory;
    float varianceBoost;
    float sigmaDepth;
    float sigmaLuminance;
    float sigmaVarianceLum;
    float depthEpsilon;
    uint sigmaNormal;
    uint atrousIterations;
    uint historyTap;
    uint gradientIterations;
    float gradientScale;
    float gradientEpsilon;
    uint reserved1;
    uint reserved2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer DnFrameRef { DnFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer DnTexelsRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer DnPairsRef { vec2 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer DnFloatsRef { float v[]; };

layout(push_constant) uniform DnPush {
    uint64_t frame;
    uint64_t src;
    uint64_t dst;
    uint64_t aux;
    uint mode;
    uint step;
    uint flags;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    uint reserved3;
    uint reserved4;
} pc;

DnFrame dn_frame() { return DnFrameRef(pc.frame).f; }

// --- helpers (svgf_kernel.hpp) ------------------------------------------------------------------------
float dn_atrous_tap(int d) {
    const int a = d < 0 ? -d : d;
    return a == 0 ? 0.375 : (a == 1 ? 0.25 : 0.0625);
}
float dn_gauss_tap(int d) { return d == 0 ? 0.5 : 0.25; }

float dn_luminance(vec4 c, uint flags) {
    if ((flags & DN_FLAG_SCALAR) != 0u) {
        return c.x;
    }
    precise float l = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
    return l;
}

float dn_pow_int(float b, uint e) {
    precise float r = 1.0;
    precise float bb = b;
    uint k = e;
    while (k != 0u) {
        if ((k & 1u) != 0u) {
            r = r * bb;
        }
        bb = bb * bb;
        k = k >> 1u;
    }
    return r;
}

float dn_dot3(vec4 a, vec4 b) {
    precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
    return d;
}

float dn_normal_weight(vec4 p, vec4 q, uint sigmaNormal) {
    const float d = dn_dot3(p, q);
    return dn_pow_int(d > 0.0 ? d : 0.0, sigmaNormal);
}

float dn_depth_term(DnFrame F, float zp, float zq, vec2 g, float ox, float oy) {
    precise float proj = g.x * ox + g.y * oy;
    precise float t = abs(zp - zq) / (F.sigmaDepth * abs(proj) + F.depthEpsilon);
    return t;
}

vec4 dn_oct_decode(float ox, float oy) {
    precise float nx = ox;
    precise float ny = oy;
    precise float nz = 1.0 - abs(ox) - abs(oy);
    if (nz < 0.0) {
        nx = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        ny = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
    }
    precise float len = sqrt(nx * nx + ny * ny + nz * nz);
    precise vec4 n = vec4(nx / len, ny / len, nz / len, 0.0);
    return n;
}

vec4 dn_read_signal(DnFrame F, uint i) {
    if (F.signalStride == 1u) {
        return vec4(DnFloatsRef(F.signal).v[i], 0.0, 0.0, 0.0);
    }
    const uint b = i * F.signalStride;
    DnFloatsRef s = DnFloatsRef(F.signal);
    return vec4(s.v[b], s.v[b + 1u], s.v[b + 2u], 0.0);
}

float dn_gradient_lambda(DnFrame F, vec4 g) {
    if (!(g.y > F.gradientEpsilon)) {
        return 0.0;
    }
    precise float l = F.gradientScale * abs(g.x) / g.y;
    return l < 1.0 ? l : 1.0;
}

uint dn_stratum_pixel(DnFrame F, uint sx, uint sy) {
    const uint px = sx * DN_STRATUM + 1u < F.width ? sx * DN_STRATUM + 1u : F.width - 1u;
    const uint py = sy * DN_STRATUM + 1u < F.height ? sy * DN_STRATUM + 1u : F.height - 1u;
    return py * F.width + px;
}

#endif
