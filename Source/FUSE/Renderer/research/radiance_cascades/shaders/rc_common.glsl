// WP-6.5 radiance cascades (research): push constants, buffer references and the helpers both kernels share.
// GLSL twin of rc_common.slang. The CPU reference is src/rc_reference.cpp (rc_trace, bilinear, childAverage):
// every arithmetic result is held in a `precise` variable, so no multiply-add is contracted and the kernels
// keep the reference's IEEE operations and order.
#ifndef FUSE_RC_COMMON_GLSL
#define FUSE_RC_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define RC_FLAG_BILINEAR_FIX (1u << 0)
#define RC_FLAG_HAS_UPPER (1u << 1)

layout(buffer_reference, std430, buffer_reference_align = 16) buffer RcTexelsRef { vec4 v[]; };

// RcPush (include/fuse/renderer/research/rc/rc_types.hpp), 112 bytes.
layout(push_constant) uniform RcPushBlock {
    uint64_t scene;
    uint64_t dirs;
    uint64_t upper;
    uint64_t dst;
    uint width;
    uint height;
    uint probesX;
    uint probesY;
    uint dirCount;
    uint upperProbesX;
    uint upperProbesY;
    uint flags;
    float spacing;
    float upperSpacing;
    float tStart;
    float tEnd;
    float step;
    float invStep;
    float skyR;
    float skyG;
    float skyB;
    uint count;
    uint branch;
    float invBranch;
} pc;

// Marches from a along the unit direction d over [0, len): the first opaque texel ends the ray (rgb = its
// emission, w = transmittance 0); outside the grid the ray continues unless it moves away from it.
vec4 rc_trace(vec2 a, vec2 d, float len) {
    precise float n = ceil(len * pc.invStep);
    const uint count = n > 0.0 ? uint(n) : 0u;
    const float fw = float(pc.width);
    const float fh = float(pc.height);
    for (uint k = 0u; k < count; ++k) {
        precise float t = float(k) * pc.step;
        precise float px = a.x + d.x * t;
        precise float py = a.y + d.y * t;
        const float fx = floor(px);
        const float fy = floor(py);
        if (fx < 0.0 || fy < 0.0 || fx >= fw || fy >= fh) {
            if ((fx < 0.0 && d.x <= 0.0) || (fx >= fw && d.x >= 0.0) || (fy < 0.0 && d.y <= 0.0) ||
                (fy >= fh && d.y >= 0.0)) {
                break;
            }
            continue;
        }
        const vec4 v = RcTexelsRef(pc.scene).v[uint(fy) * pc.width + uint(fx)];
        if (v.w > 0.5) {
            return vec4(v.xyz, 0.0);
        }
    }
    return vec4(0.0, 0.0, 0.0, 1.0);
}

uint rc_clamp_index(int v, uint count) {
    if (v < 0) {
        return 0u;
    }
    const uint u = uint(v);
    return u >= count ? count - 1u : u;
}

// Bilinear neighbours of p on a probe grid of `spacing`: xy = (x0, x1), zw = (y0, y1); weights in w
// ((x0, y0), (x1, y0), (x0, y1), (x1, y1)).
uvec4 rc_bilinear(vec2 p, float spacing, uint countX, uint countY, out vec4 w) {
    precise float u = p.x / spacing - 0.5;
    precise float v = p.y / spacing - 0.5;
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
    return uvec4(rc_clamp_index(ix, countX), rc_clamp_index(ix + 1, countX), rc_clamp_index(iy, countY),
                 rc_clamp_index(iy + 1, countY));
}

// Mean of the b children bk .. bk + b-1 of direction k on upper probe (x, y).
vec3 rc_child_average(uint x, uint y, uint k) {
    const uint upperDirs = pc.dirCount * pc.branch;
    const uint base = (y * pc.upperProbesX + x) * upperDirs + k * pc.branch;
    precise float r = 0.0;
    precise float g = 0.0;
    precise float b = 0.0;
    for (uint j = 0u; j < pc.branch; ++j) {
        const vec4 v = RcTexelsRef(pc.upper).v[base + j];
        r = r + v.x;
        g = g + v.y;
        b = b + v.z;
    }
    precise float mr = r * pc.invBranch;
    precise float mg = g * pc.invBranch;
    precise float mb = b * pc.invBranch;
    return vec3(mr, mg, mb);
}

#endif
