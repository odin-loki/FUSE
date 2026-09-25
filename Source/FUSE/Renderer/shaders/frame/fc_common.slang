// Frame composer: Slang twin of fc_common.glsl (see there). Compile with -fp-mode precise and
// -I <Renderer>/shaders (the WP-8.2 at_sample.slang helpers).
#pragma once
#include "atmosphere/at_sample.slang"

static const uint kFcModeSky = 0u;
static const uint kFcModeGather = 1u;
static const uint kFcModeResolve = 2u;
static const uint kFcModeShadowPack = 3u;
static const uint kFcModeRestir = 4u;
static const uint kFcModeReflect = 5u;

static const uint kFcFlagSky = 1u << 0;
static const uint kFcFlagAerial = 1u << 1;
static const uint kFcFlagSunDisk = 1u << 2;
static const uint kFcFlagClouds = 1u << 3;
static const uint kFcFlagSplats = 1u << 4;

// FrameConstants, 224 bytes.
struct FcFrame {
    uint64_t atmosphere;
    uint64_t background;
    uint64_t distance;
    uint64_t clouds;
    uint64_t splats;
    uint64_t denoised;
    uint64_t visibility;
    uint64_t restirDi;
    uint64_t restirAlbedo;
    uint64_t reflection;
    uint width;
    uint height;
    uint inColor;
    uint inDepth;
    uint outColor;
    uint outResolve;
    uint flags;
    uint gbufferNormal;
    uint gbufferAlbedo;
    uint gbufferRoughMetal;
    uint reserved1[2];
    float invViewProj[16];
    float cameraPos[4];
    float skyDistance;
    float reserved2[3];
};

struct FcPush {
    uint64_t frame;
    uint mode;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<FcPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_fc_textures[];

static FcFrame F;

void fcBegin() { F = ((FcFrame*)pc.frame)[0]; }

float4* fcTexels(uint64_t address) { return (float4*)address; }
float* fcFloats(uint64_t address) { return (float*)address; }

float4 fcFetch(uint handle, int2 p) { return fuse_fc_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

float3 fcUnproject(uint2 p, float depth) {
    const float nx = (float(p.x) + 0.5) / float(F.width) * 2.0 - 1.0;
    const float ny = (float(p.y) + 0.5) / float(F.height) * 2.0 - 1.0;
    const float px = F.invViewProj[0] * nx + F.invViewProj[4] * ny + F.invViewProj[8] * depth + F.invViewProj[12];
    const float py = F.invViewProj[1] * nx + F.invViewProj[5] * ny + F.invViewProj[9] * depth + F.invViewProj[13];
    const float pz = F.invViewProj[2] * nx + F.invViewProj[6] * ny + F.invViewProj[10] * depth + F.invViewProj[14];
    const float pw = F.invViewProj[3] * nx + F.invViewProj[7] * ny + F.invViewProj[11] * depth + F.invViewProj[15];
    return float3(px / pw, py / pw, pz / pw);
}

float3 fcOctDecode(float ox, float oy) {
    float x = ox;
    float y = oy;
    const float z = 1.0 - abs(ox) - abs(oy);
    if (z < 0.0) {
        x = (1.0 - abs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
        y = (1.0 - abs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
    }
    const float l = sqrt(x * x + y * y + z * z);
    return float3(x / l, y / l, z / l);
}

float fcViewDistance(uint2 p, float depth) {
    if (!(depth < 1.0)) {
        return F.skyDistance;
    }
    const float3 q = fcUnproject(p, depth);
    const float dx = q.x - F.cameraPos[0];
    const float dy = q.y - F.cameraPos[1];
    const float dz = q.z - F.cameraPos[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}
