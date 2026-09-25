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
