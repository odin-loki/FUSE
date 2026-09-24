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

#endif // FUSE_TM_COMMON_GLSL
