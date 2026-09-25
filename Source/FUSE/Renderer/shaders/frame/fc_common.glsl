// Frame composer: the FrameConstants record, push constants and helpers of fc_frame.comp. GLSL twin of
// fc_common.slang; C++ mirror + CPU reference: include/fuse/renderer/frame/frame_types.hpp (checked by
// fuse_rp_frame_layout). Compile with -I <Renderer>/shaders -I <Renderer>/shaders/common (at_sample, bindless).
#ifndef FUSE_FC_COMMON_GLSL
#define FUSE_FC_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_samplerless_texture_functions : require
#include "bindless.glsl"
#include "atmosphere/at_sample.glsl"

#define FC_MODE_SKY 0u
#define FC_MODE_GATHER 1u
#define FC_MODE_RESOLVE 2u
#define FC_MODE_SHADOW_PACK 3u

#define FC_FLAG_SKY (1u << 0)
#define FC_FLAG_AERIAL (1u << 1)
#define FC_FLAG_SUN_DISK (1u << 2)
#define FC_FLAG_CLOUDS (1u << 3)
#define FC_FLAG_SPLATS (1u << 4)

// FrameConstants, 192 bytes.
struct FcFrame {
    uint64_t atmosphere;
    uint64_t background;
    uint64_t distance;
    uint64_t clouds;
    uint64_t splats;
    uint64_t denoised;
    uint64_t visibility;
    uint64_t reserved0;
    uint width;
    uint height;
    uint inColor;
    uint inDepth;
    uint outSky;
    uint outResolve;
    uint flags;
    uint reserved1;
    float invViewProj[16];
    float cameraPos[4];
    float skyDistance;
    float reserved2[3];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FcFrameRef { FcFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer FcTexelsRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FcFloatsRef { float v[]; };

layout(push_constant) uniform FcPush {
    uint64_t frame;
    uint mode;
    uint reserved;
} pc;

#define F (FcFrameRef(pc.frame).f)

vec4 fc_fetch(uint handle, ivec2 p) { return texelFetch(fuse_textures_2d[fuse_handle_index(handle)], p, 0); }

// frame_unproject (frame_types.hpp): pixel centre at device depth through invViewProj.
vec3 fc_unproject(uvec2 p, float depth) {
    precise float nx = (float(p.x) + 0.5) / float(F.width) * 2.0 - 1.0;
    precise float ny = (float(p.y) + 0.5) / float(F.height) * 2.0 - 1.0;
    precise float px = F.invViewProj[0] * nx + F.invViewProj[4] * ny + F.invViewProj[8] * depth + F.invViewProj[12];
    precise float py = F.invViewProj[1] * nx + F.invViewProj[5] * ny + F.invViewProj[9] * depth + F.invViewProj[13];
    precise float pz = F.invViewProj[2] * nx + F.invViewProj[6] * ny + F.invViewProj[10] * depth + F.invViewProj[14];
    precise float pw = F.invViewProj[3] * nx + F.invViewProj[7] * ny + F.invViewProj[11] * depth + F.invViewProj[15];
    return vec3(px / pw, py / pw, pz / pw);
}

// frame_view_distance (frame_types.hpp).
float fc_view_distance(uvec2 p, float depth) {
    if (!(depth < 1.0)) {
        return F.skyDistance;
    }
    const vec3 q = fc_unproject(p, depth);
    precise float dx = q.x - F.cameraPos[0];
    precise float dy = q.y - F.cameraPos[1];
    precise float dz = q.z - F.cameraPos[2];
    precise float d2 = dx * dx + dy * dy + dz * dz;
    return sqrt(d2);
}

#endif
