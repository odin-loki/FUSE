// FUSE temporal (WP-4.1): records and push constants shared by tm_motion.slang and tm_taau.slang.
// Slang twin of tm_common.glsl (records: include/fuse/renderer/temporal/temporal_types.hpp). Compile with
// -fp-mode precise (Slang does not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kFuseTmMotionSkyValid = 1u;

struct TmMotionFrame {
    float4 drawViewProj[4]; // columns
    float4 viewProj[4];
    float4 prevViewProj[4];
    float4 skyReproj[3]; // H columns (x, y, w, pad)
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

struct TmTaauFrame {
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
    float4 curToPrevView[3]; // 3x4 row-major rows
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

struct TmPush {
    uint64_t frame; // BDA of this frame's constants
    uint64_t out_;  // taau.resolve: optional f32x4 dump, else 0
};
[[vk::push_constant]] ConstantBuffer<TmPush> pc;

// Correctly rounded division / square root (tm_common.glsl fuse_tm_div / fuse_tm_sqrt, see there).
float fuseTmResid(float a, float q, float b) {
    const float p = q * b;
    const float cq = 4097.0 * q;
    const float qh = cq - (cq - q);
    const float ql = q - qh;
    const float cb = 4097.0 * b;
    const float bh = cb - (cb - b);
    const float bl = b - bh;
    const float e = ((qh * bh - p) + qh * bl + ql * bh) + ql * bl;
    return (a - p) - e;
}
float fuseTmNextToward(float q, bool up) {
    const uint bits = asuint(q);
    return asfloat((q > 0.0) == up ? bits + 1u : bits - 1u);
}
float fuseTmRoundRn(float a, float q, float b, bool root) {
    float r = fuseTmResid(a, q, root ? q : b);
    for (int i = 0; i < 4 && r != 0.0; ++i) {
        const float qn = fuseTmNextToward(q, root ? r > 0.0 : (r > 0.0) == (b > 0.0));
        const float rn = fuseTmResid(a, qn, root ? qn : b);
        if (abs(rn) < abs(r) || (abs(rn) == abs(r) && (asuint(q) & 1u) != 0u)) {
            q = qn;
            r = rn;
        } else {
            break;
        }
    }
    return q;
}
float fuseTmDiv(float a, float b) {
    const float q = a / b;
    if (q == 0.0 || isinf(q) || isnan(q)) {
        return q;
    }
    return fuseTmRoundRn(a, q, b, false);
}
float fuseTmSqrt(float x) {
    const float s = sqrt(x);
    if (!(x > 0.0) || isinf(x)) {
        return s;
    }
    return fuseTmRoundRn(x, s, s, true);
}
