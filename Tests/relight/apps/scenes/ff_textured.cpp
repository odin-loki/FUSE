// ff_textured: fixed-function texturing. A mip-mapped A8R8G8B8 checker (texture hash = mip 0), a
// R5G6B5 gradient on a second UV set, texture-stage combiners (MODULATE, SELECTARG1, ADD,
// MODULATE2X, BLENDTEXTUREALPHA, TFACTOR), TEXCOORDINDEX, a texture transform, and sampler
// states (point / linear / mip filtering, wrap / clamp / mirror addressing).
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_textured", 3, false, 0,
                              "FF texturing: mipped A8R8G8B8 + R5G6B5 textures, stage combiners, two UV sets, "
                              "TEXCOORDINDEX, texture transform, sampler filter/address states"};

struct Vtx2 { // D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2
    float x, y, z;
    uint32_t color;
    float u0, v0, u1, v1;
};
static const uint32_t FVF_2 = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;

static int g_vb, g_ib, g_checker, g_grad;
static const uint32_t kC0 = 0xffd03020, kC1 = 0xff2050e0; // checker colours (opaque)
static const int kQuads = 6;

// 2 x 3 grid of quads, each 1.2 x 1.2, uv0 0..1, uv1 0..1.
static void quadOrigin(int i, float& x, float& y)
{
    x = -2.1f + float(i % 3) * 1.5f;
    y = 0.15f - float(i / 3) * 1.5f;
}

void rl::sceneInit(Gfx& g)
{
    // Checker 32x32, 8-texel cells; mips are 2x2 box filters of the level above (integer, exact).
    const uint32_t levels = 6;
    g_checker = g.createTexture(32, 32, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    std::vector<uint32_t> lvl = makeCheckerARGB(32, 32, kC0, kC1, 8);
    uint32_t w = 32;
    for (uint32_t l = 0; l < levels; ++l) {
        g.uploadTexture(g_checker, l, lvl.data());
        uint32_t nw = w > 1 ? w / 2 : 1;
        std::vector<uint32_t> next(size_t(nw) * nw);
        for (uint32_t y = 0; y < nw; ++y)
            for (uint32_t x = 0; x < nw; ++x) {
                uint32_t out = 0;
                for (int c = 0; c < 32; c += 8) {
                    uint32_t s = 0;
                    for (int k = 0; k < 4; ++k) {
                        uint32_t sx = x * 2 + (k & 1), sy = y * 2 + (k >> 1);
                        s += (lvl[size_t(sy) * w + sx] >> c) & 0xff;
                    }
                    out |= ((s + 2) / 4) << c;
                }
                next[size_t(y) * nw + x] = out;
            }
        lvl.swap(next);
        w = nw;
    }
    // R5G6B5 16x16 gradient: red along x, green along y.
    g_grad = g.createTexture(16, 16, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED);
    std::vector<uint16_t> grad(16 * 16);
    for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x)
            grad[y * 16 + x] = uint16_t(((x * 2) << 11) | ((y * 4) << 5) | 16);
    g.uploadTexture(g_grad, 0, grad.data());

    std::vector<Vtx2> v;
    for (int i = 0; i < kQuads; ++i) {
        float x, y;
        quadOrigin(i, x, y);
        const float s = 1.2f;
        uint32_t c = (i == 2) ? 0x80ffffff : 0xffffffff; // quad 2: half vertex alpha for BLENDDIFFUSEALPHA
        v.push_back({x, y, 0.0f, c, 0.0f, 1.0f, 0.0f, 1.0f});
        v.push_back({x, y + s, 0.0f, c, 0.0f, 0.0f, 0.0f, 0.0f});
        v.push_back({x + s, y + s, 0.0f, c, 1.0f, 0.0f, 1.0f, 0.0f});
        v.push_back({x + s, y, 0.0f, c, 1.0f, 1.0f, 1.0f, 1.0f});
    }
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(Vtx2)), D3DUSAGE_WRITEONLY, FVF_2, D3DPOOL_MANAGED, v.data());
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    g_ib = g.createIndexBuffer(sizeof idx, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx);
}

static void stage(Gfx& g, uint32_t s, uint32_t cop, uint32_t a1, uint32_t a2, uint32_t aop, uint32_t aa1, uint32_t aa2)
{
    g.setTextureStageState(s, D3DTSS_COLOROP, cop);
    g.setTextureStageState(s, D3DTSS_COLORARG1, a1);
    g.setTextureStageState(s, D3DTSS_COLORARG2, a2);
    g.setTextureStageState(s, D3DTSS_ALPHAOP, aop);
    g.setTextureStageState(s, D3DTSS_ALPHAARG1, aa1);
    g.setTextureStageState(s, D3DTSS_ALPHAARG2, aa2);
}

static void drawQuad(Gfx& g, int i) { g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, i * 4, 0, 4, 0, 2); }

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 0.0f, -4.2f}, {0.0f, 0.0f, 0.0f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff101010, 1.0f, 0);
    g.setFVF(FVF_2);
    g.setStreamSource(0, g_vb, 0, sizeof(Vtx2));
    g.setIndices(g_ib);
    g.setTexture(0, g_checker);
    g.setTexture(1, NONE);
    for (uint32_t s = 0; s < 2; ++s) {
        g.setSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        g.setSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        g.setSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        g.setSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        g.setSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        g.setTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
    }
    stage(g, 1, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT);

    // 0: MODULATE texture x diffuse (white), point sampled.
    stage(g, 0, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE);
    drawQuad(g, 0);

    // 1: SELECTARG1 texture, trilinear, texture transform (uv * 2 -> 4x4 cells), mirror addressing.
    stage(g, 0, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g.setSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    g.setSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR);
    g.setSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_MIRROR);
    Mat4 tt = identity();
    tt.m[0] = 2.0f;
    tt.m[5] = 2.0f;
    g.setTransform(D3DTS_TEXTURE0, tt);
    g.setTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    drawQuad(g, 1);
    g.setTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g.setSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    g.setSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    // 2: BLENDDIFFUSEALPHA between texture and TFACTOR (vertex alpha 0x80).
    g.setRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00);
    stage(g, 0, D3DTOP_BLENDDIFFUSEALPHA, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_TEXTURE);
    drawQuad(g, 2);

    // 3: two stages: checker (uv0) MODULATE gradient (uv1 via TEXCOORDINDEX 1), clamp on stage 1.
    stage(g, 0, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE);
    g.setTexture(1, g_grad);
    g.setSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g.setSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    stage(g, 1, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_CURRENT);
    drawQuad(g, 3);

    // 4: stage 1 ADD (checker + gradient).
    stage(g, 1, D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_CURRENT);
    drawQuad(g, 4);

    // 5: stage 0 MODULATE2X with a grey TFACTOR, stage 1 disabled; gradient on stage 0 with uv1.
    g.setTexture(1, NONE);
    stage(g, 1, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_CURRENT);
    g.setTexture(0, g_grad);
    g.setTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1);
    g.setRenderState(D3DRS_TEXTUREFACTOR, 0xff808080);
    stage(g, 0, D3DTOP_MODULATE2X, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE);
    drawQuad(g, 5);
    g.setTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);

    if (frame == kScene.frames - 1) {
        g.probe(1, 1, 0x10, 0x10, 0x10, 0, "clear colour");
        // Quad 0, uv (0.125, 0.125) and (0.375, 0.125): cells (0,0) = C0 and (1,0) = C1.
        float x, y;
        quadOrigin(0, x, y);
        probeAt(g, world, cam, {x + 0.15f, y + 1.05f, 0.0f}, 0xd0, 0x30, 0x20, 2, "MODULATE checker cell 0");
        probeAt(g, world, cam, {x + 0.45f, y + 1.05f, 0.0f}, 0x20, 0x50, 0xe0, 2, "MODULATE checker cell 1");
    }
}
