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

#endif
