// WP-9.2 3D Gaussian splatting: Slang twin of gs_common.glsl (see there). Compile with -fp-mode precise so
// no multiply-add is contracted.
#pragma once

static const uint kGsTile = 16u;
static const uint kGsGroup = 256u;
static const uint kGsFlagDepthTest = 1u << 0;
static const uint kGsFlagReversedZ = 1u << 1;
static const uint kGsSplatFloats = 60u;
static const uint kGsPadValue = 0xFFFFFFFFu;

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

struct GsPush {
    uint64_t frame;
    uint padKeyHigh;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<GsPush> pc;

// The bindless sampled images (binding 1 of the FUSE bindless set).
[[vk::binding(1, 0)]] Texture2D<float4> fuse_gs_textures[];

static GsFrame F;
void gsBegin() { F = ((GsFrame*)pc.frame)[0]; }

float* gsFloats(uint64_t address) { return (float*)address; }
uint4* gsProjectedPtr(uint64_t address) { return (uint4*)address; }
uint* gsUints(uint64_t address) { return (uint*)address; }
uint64_t* gsKeys(uint64_t address) { return (uint64_t*)address; }
float4* gsTexels(uint64_t address) { return (float4*)address; }

float gsClamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
uint4 gsProjected(uint i, uint w) { return gsProjectedPtr(F.projected)[i * 4u + w]; }
uint gsTilesTouched(uint4 rect) { return (rect.z - rect.x) * (rect.w - rect.y); }
