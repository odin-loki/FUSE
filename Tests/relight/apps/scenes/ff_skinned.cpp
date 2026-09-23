// ff_skinned: fixed-function vertex blending. An arm (tube) of 4 bones drawn with indexed vertex
// blending (D3DFVF_XYZB3 | LASTBETA_UBYTE4: 2 weights + UBYTE4 matrix indices, D3DVBF_2WEIGHTS,
// INDEXEDVERTEXBLENDENABLE, D3DTS_WORLDMATRIX(0..3)), and a flag with non-indexed blending
// (D3DFVF_XYZB1, D3DVBF_1WEIGHTS, WORLD / WORLD1). Lit by a directional light (normals blended).
// The device uses software vertex processing (indexed blending needs SWVP on many D3D9 GPUs).
// Wine 9 reports MaxVertexBlendMatrixIndex = 0 in both modes; the draws are issued anyway.
#include <stdio.h>
#include <stdlib.h>

#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_skinned", 4, false, DEV_SOFTWARE_VP,
                              "FF indexed vertex blending (XYZB3 + LASTBETA_UBYTE4, 2 weights, WORLDMATRIX 0..3) "
                              "and non-indexed 1-weight blending (WORLD/WORLD1), lit"};

struct VtxSkin { // D3DFVF_XYZB3 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_NORMAL | D3DFVF_DIFFUSE
    float x, y, z, w0, w1;
    uint32_t indices;
    float nx, ny, nz;
    uint32_t color;
};
static const uint32_t FVF_SKIN = D3DFVF_XYZB3 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_NORMAL | D3DFVF_DIFFUSE;
struct VtxB1 { // D3DFVF_XYZB1 | D3DFVF_NORMAL | D3DFVF_DIFFUSE
    float x, y, z, w0;
    float nx, ny, nz;
    uint32_t color;
};
static const uint32_t FVF_B1 = D3DFVF_XYZB1 | D3DFVF_NORMAL | D3DFVF_DIFFUSE;

static int g_vbArm, g_ibArm, g_vbFlag;
static uint32_t g_armVerts, g_armIdx;
static const int kBones = 4;

void rl::sceneInit(Gfx& g)
{
    const Caps& c = g.caps();
    // Wine 9 wined3d reports MaxVertexBlendMatrixIndex = 0 even with SWVP but implements indexed
    // blending in its GLSL fixed-function pipeline; the draw is issued regardless (stderr note only).
    if (c.maxVertexBlendMatrices < 3)
        fprintf(stderr, "ff_skinned: note: MaxVertexBlendMatrices=%u < 3\n", c.maxVertexBlendMatrices);
    if (c.maxVertexBlendMatrixIndex < kBones - 1)
        fprintf(stderr, "ff_skinned: note: MaxVertexBlendMatrixIndex=%u < %d (drawing anyway)\n",
                c.maxVertexBlendMatrixIndex, kBones - 1);
    const int rings = 13, segs = 8;
    std::vector<VtxSkin> v;
    for (int r = 0; r < rings; ++r) {
        float x = -1.5f + 3.0f * float(r) / float(rings - 1);
        float fx = x + 1.5f; // 0..3
        int b = int(fx);
        if (b > kBones - 2)
            b = kBones - 2;
        float t = fx - float(b);
        for (int s = 0; s < segs; ++s) {
            float a = 2.0f * kPi * float(s) / float(segs);
            float ny = cosf(a), nz = sinf(a);
            uint32_t idx = uint32_t(b) | (uint32_t(b + 1) << 8) | (uint32_t(b + 1) << 16);
            uint32_t col = argb(0xff, uint32_t(80 + 50 * b), 0xa0, uint32_t(220 - 50 * b));
            v.push_back({x, 0.25f * ny, 0.25f * nz, 1.0f - t, t, idx, 0.0f, ny, nz, col});
        }
    }
    std::vector<uint16_t> idx;
    for (int r = 0; r + 1 < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            uint16_t a = uint16_t(r * segs + s), b = uint16_t(r * segs + (s + 1) % segs);
            uint16_t c0 = uint16_t(a + segs), d = uint16_t(b + segs);
            uint16_t q[6] = {a, c0, d, a, d, b};
            idx.insert(idx.end(), q, q + 6);
        }
    g_armVerts = uint32_t(v.size());
    g_armIdx = uint32_t(idx.size());
    g_vbArm = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxSkin)), D3DUSAGE_WRITEONLY, FVF_SKIN, D3DPOOL_MANAGED,
                                   v.data());
    g_ibArm = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());

    // Flag: 6 x 2 strip, weight 1 at the pole (WORLD), falling to 0 at the tip (WORLD1).
    std::vector<VtxB1> f;
    for (int i = 0; i < 6; ++i) {
        float x = float(i) * 0.3f, w = 1.0f - float(i) / 5.0f;
        f.push_back({x, 0.0f, 0.0f, w, 0.0f, 0.0f, -1.0f, 0xffe0c040});
        f.push_back({x, 0.6f, 0.0f, w, 0.0f, 0.0f, -1.0f, 0xffe0c040});
    }
    g_vbFlag = g.createVertexBuffer(uint32_t(f.size() * sizeof(VtxB1)), D3DUSAGE_WRITEONLY, FVF_B1, D3DPOOL_MANAGED, f.data());
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    setCamera(g, {0.0f, 1.0f, -4.0f}, {0.0f, 0.2f, 0.0f});
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff182028, 1.0f, 0);
    g.setRenderState(D3DRS_LIGHTING, TRUE);
    g.setRenderState(D3DRS_AMBIENT, 0xff303030);
    g.setRenderState(D3DRS_NORMALIZENORMALS, TRUE);
    g.setRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    g.setRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
    Light l{};
    l.Type = D3DLIGHT_DIRECTIONAL;
    l.Diffuse = color(1.0f, 1.0f, 1.0f);
    l.Direction = normalize(Vec3{0.3f, -0.5f, 1.0f});
    g.setLight(0, l);
    g.lightEnable(0, true);
    Material m{};
    m.Diffuse = color(1, 1, 1);
    m.Ambient = color(1, 1, 1);
    g.setMaterial(m);

    // Bone chain: bone i rotates about its joint (x = -1.5 + i) by `a`, after its parent.
    float a = 0.25f + 0.1f * float(frame);
    Mat4 place = translation(0.0f, 0.4f, 0.0f);
    Mat4 bone = identity();
    for (int i = 0; i < kBones; ++i) {
        if (i > 0) {
            float jx = -1.5f + float(i);
            bone = mul(mul(mul(translation(-jx, 0.0f, 0.0f), rotationZ(a)), translation(jx, 0.0f, 0.0f)), bone);
        }
        g.setTransform(256 + uint32_t(i), mul(bone, place)); // D3DTS_WORLDMATRIX(i)
    }
    g.setRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
    g.setRenderState(D3DRS_VERTEXBLEND, D3DVBF_2WEIGHTS);
    g.setFVF(FVF_SKIN);
    g.setStreamSource(0, g_vbArm, 0, sizeof(VtxSkin));
    g.setIndices(g_ibArm);
    g.tag("skinned");
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, g_armVerts, 0, g_armIdx / 3);

    // Non-indexed: WORLD (pole) and WORLD1 (tip) blended by one weight.
    g.setRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    g.setRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
    g.setTransform(256, translation(-1.2f, -1.2f, 0.0f));
    g.setTransform(257, mul(rotationY(0.3f + 0.15f * float(frame)), translation(-1.2f, -1.0f, 0.0f)));
    g.setFVF(FVF_B1);
    g.setStreamSource(0, g_vbFlag, 0, sizeof(VtxB1));
    g.drawPrimitive(D3DPT_TRIANGLESTRIP, 0, 10);
    g.setRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    g.tag("world");

    if (frame == kScene.frames - 1)
        g.probe(1, 1, 0x18, 0x20, 0x28, 0, "clear colour");
}
