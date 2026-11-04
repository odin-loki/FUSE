// WP-9.2 3D Gaussian splatting: records, push constants and buffer views every gsplat kernel shares. GLSL
// twin of gs_common.slang; the C++ mirror is include/fuse/renderer/gsplat/gsplat_types.hpp (GsFrame ==
// GsFrameConstants, checked by fuse_rp_gsplat_layout). Arithmetic results live in `precise` variables so no
// multiply-add is contracted: the kernels keep the CPU reference's (gsplat_reference.cpp) operation order.
#ifndef FUSE_GS_COMMON_GLSL
#define FUSE_GS_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_samplerless_texture_functions : require
#include "bindless.glsl"

#define GS_TILE 16u
#define GS_GROUP 256u
#define GS_FLAG_DEPTH_TEST (1u << 0)
#define GS_FLAG_REVERSED_Z (1u << 1)
#define GS_SPLAT_FLOATS 60u
#define GS_PAD_VALUE 0xFFFFFFFFu

// GsFrameConstants, 224 bytes.
struct GsFrame {
    uint64_t splats;
    uint64_t projected;
    uint64_t offsets;
    uint64_t keys;
    uint64_t values;
    uint64_t ranges;
    uint64_t counters;
    uint64_t output_;
    uint splatCount;
    uint capacity;
    uint width;
    uint height;
    uint tilesX;
    uint tilesY;
    uint tileCount;
    uint depthHandle;
    uint flags;
    uint shDegree;
    uint reserved0;
    uint reserved1;
    float view[12];
    float camPos[4];
    float fx;
    float fy;
    float cx;
    float cy;
    float nearZ;
    float tanFovX;
    float tanFovY;
    float lowPass;
    float depthA;
    float depthB;
    float alphaMin;
    float transmittanceMin;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GsFrameRef { GsFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer GsFloatsRef { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer GsProjectedRef { uvec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer GsUintsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer GsKeysRef { uint64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer GsTexelsRef { vec4 v[]; };

layout(push_constant) uniform GsPush {
    uint64_t frame;
    uint padKeyHigh;
    uint reserved;
} pc;

// Every F.field is a load through the frame's BDA (uniform, cached).
#define F (GsFrameRef(pc.frame).f)

float gs_clamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }

// GsProjected word w of splat i (4 uvec4 per splat).
uvec4 gs_projected(uint i, uint w) { return GsProjectedRef(F.projected).v[i * 4u + w]; }

uint gs_tiles_touched(uvec4 rect) { return (rect.z - rect.x) * (rect.w - rect.y); }

// Correctly rounded division and square root. The CPU reference (gsplat_reference.cpp) divides and takes square
// roots in IEEE f32; Vulkan only guarantees correct rounding for add / sub / mul (OpFDiv is 2.5 ulp, sqrt inherits
// inversesqrt), and hardware uses that slack (the RTX 3090 differs from the oracle by up to ~250 ulp on the conic,
// where det = a c - b^2 amplifies the Jacobian's division error, and the 3-sigma radius ceil(3 sqrt(lambda)) and the
// tile rectangle floor((mx -+ r) / 16) are discrete decisions on these quotients). gs_div / gs_sqrt round the
// hardware result to the IEEE one with an exact Dekker residual (needs only correctly rounded, uncontracted mul /
// add), so "gsplat.preprocess" equals gs_preprocess_splat bit for bit on any conformant device. Lavapipe already
// rounds correctly: there the walk never moves.
float gs_resid(float a, float q, float b) { // a - q * b, accurate enough to compare neighbours
    precise float p = q * b;
    precise float cq = 4097.0 * q;
    precise float qh = cq - (cq - q);
    precise float ql = q - qh;
    precise float cb = 4097.0 * b;
    precise float bh = cb - (cb - b);
    precise float bl = b - bh;
    precise float e = ((qh * bh - p) + qh * bl + ql * bh) + ql * bl;
    precise float r = (a - p) - e;
    return r;
}
float gs_next_toward(float q, bool up) { // q != 0; up: toward +inf
    const uint bits = floatBitsToUint(q);
    return uintBitsToFloat((q > 0.0) == up ? bits + 1u : bits - 1u);
}
float gs_round_rn(float a, float q, float b, bool root) { // walk q to the nearest representable a / b (root: sqrt(a))
    float r = gs_resid(a, q, root ? q : b);
    for (int i = 0; i < 4 && r != 0.0; ++i) {
        const float qn = gs_next_toward(q, root ? r > 0.0 : (r > 0.0) == (b > 0.0));
        const float rn = gs_resid(a, qn, root ? qn : b);
        if (abs(rn) < abs(r) || (abs(rn) == abs(r) && (floatBitsToUint(q) & 1u) != 0u)) {
            q = qn;
            r = rn;
        } else {
            break;
        }
    }
    return q;
}
float gs_div(float a, float b) {
    precise float q = a / b;
    if (q == 0.0 || isinf(q) || isnan(q)) return q;
    return gs_round_rn(a, q, b, false);
}
float gs_sqrt(float x) {
    precise float s = sqrt(x);
    if (!(x > 0.0) || isinf(x)) return s;
    return gs_round_rn(x, s, s, true);
}

#endif
