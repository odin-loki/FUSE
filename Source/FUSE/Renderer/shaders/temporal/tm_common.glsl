// FUSE temporal (WP-4.1): records and push constants shared by tm_motion.comp and tm_taau.comp.
// GLSL twin of tm_common.slang; the records mirror include/fuse/renderer/temporal/temporal_types.hpp
// (MotionFrameConstants 288 bytes, TaauFrameConstants 224 bytes, TemporalPush 16 bytes).
// Include after bindless.glsl.
#ifndef FUSE_TM_COMMON_GLSL
#define FUSE_TM_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUSE_TM_MOTION_SKY_VALID 1u

// MotionFrameConstants.
struct FuseTmMotionFrame {
    vec4 drawViewProj[4]; // columns
    vec4 viewProj[4];
    vec4 prevViewProj[4];
    vec4 skyReproj[3];    // H columns (x, y, w, pad)
    uint64_t motion;
    uint64_t depth;
    uint width;
    uint height;
    uint scene;
    uint vis;
    float jitterX;
    float jitterY;
    uint flags;
    uint pad;
};

// TaauFrameConstants.
struct FuseTmTaauFrame {
    uint64_t depth;
    uint64_t motion;
    uint64_t prevDepth;
    uint64_t prevMotion;
    uint64_t historyIn;
    uint64_t historyOut;
    uint renderW;
    uint renderH;
    uint displayW;
    uint displayH;
    float jitterX;
    float jitterY;
    float exposure;
    uint historyValid;
    uint hasCamera;
    uint hasPrev;
    float tanHalfX;
    float tanHalfY;
    vec4 curToPrevView[3]; // 3x4 row-major rows
    uint color;
    uint reactive;
    uint transparency;
    uint output_;
    float maxAccumulation;
    float accumulationMotionFalloff;
    float clampGamma;
    float depthRejection;
    float velocityRejectionPx;
    float clipFullMotionPx;
    float staticClipStrength;
    float spatialWeight;
    float sampleKernelScale;
    float reactiveStrength;
    float transparencyClip;
    uint historyFilter;
    uint dilateMotion;
    float dilateDepthThreshold;
    uint pad0;
    uint pad1;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseTmMotionFrameRef { FuseTmMotionFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FuseTmTaauFrameRef { FuseTmTaauFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FuseTmFloatsRef { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer FuseTmVec2Ref { vec2 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer FuseTmVec4Ref { vec4 v[]; };

layout(push_constant) uniform FuseTmPush {
    uint64_t frame; // BDA of this frame's constants
    uint64_t out_;  // taau.resolve: optional f32x4 dump, else 0
} pc;

// Correctly rounded division / square root. The CPU kernels (motion_kernel.hpp, taau_kernel.hpp) are IEEE f32
// with correctly rounded / and sqrt; Vulkan guarantees that only for add / sub / mul (OpFDiv 2.5 ulp, sqrt
// inherits inversesqrt) and real GPUs use the slack (RTX 3090: 1-2 ulp). In the motion pass the barycentric
// quotients scale the object-space point (up to ~20 ulp of the linear depth); in the TAAU resolve the quotients
// feed floor() texel choices, depth / velocity rejection tests and the clip box, so one ulp can switch a
// history sample. fuse_tm_div / fuse_tm_sqrt walk the hardware result to the IEEE one with an exact residual
// a - q * b (Dekker product: correctly rounded mul / add only, no contraction); a no-op on a correctly rounding
// device (Lavapipe). Same scheme as ssfx sx_div / sx_sqrt.
float fuse_tm_resid(float a, float q, float b) {
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
float fuse_tm_next_toward(float q, bool up) {
    const uint bits = floatBitsToUint(q);
    return uintBitsToFloat((q > 0.0) == up ? bits + 1u : bits - 1u);
}
float fuse_tm_round_rn(float a, float q, float b, bool root) {
    float r = fuse_tm_resid(a, q, root ? q : b);
    for (int i = 0; i < 4 && r != 0.0; ++i) {
        const float qn = fuse_tm_next_toward(q, root ? r > 0.0 : (r > 0.0) == (b > 0.0));
        const float rn = fuse_tm_resid(a, qn, root ? qn : b);
        if (abs(rn) < abs(r) || (abs(rn) == abs(r) && (floatBitsToUint(q) & 1u) != 0u)) {
            q = qn;
            r = rn;
        } else {
            break;
        }
    }
    return q;
}
float fuse_tm_div(float a, float b) {
    precise float q = a / b;
    if (q == 0.0 || isinf(q) || isnan(q)) {
        return q;
    }
    return fuse_tm_round_rn(a, q, b, false);
}
float fuse_tm_sqrt(float x) {
    precise float s = sqrt(x);
    if (!(x > 0.0) || isinf(x)) {
        return s;
    }
    return fuse_tm_round_rn(x, s, s, true);
}

#endif // FUSE_TM_COMMON_GLSL
