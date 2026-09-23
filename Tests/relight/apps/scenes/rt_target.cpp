// rt_target: render-to-texture. A 64x64 A8R8G8B8 render-target texture with its own 64x64 D24S8
// depth surface (non-primary render target) receives a rotating cube (tag "offscreen"); the back
// buffer then shows that texture on a quad (tag "world", the Remix "raytraced render target"
// candidate) next to a directly drawn cube. The render target is dumped as <app>.rt0.png.
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"rt_target", 3, false, 0,
                              "Render-to-texture: RT texture + own depth surface, RT sampled on a back-buffer quad, "
                              "RT dump"};

static int g_rt, g_ds, g_cubeVb, g_cubeIb, g_quadVb, g_tex;
static const uint32_t kRtClear = 0xff602040;

void rl::sceneInit(Gfx& g)
{
    g_rt = g.createTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT);
    g_ds = g.createDepthStencil(64, 64, D3DFMT_D24S8);
    std::vector<VtxPNT> cube;
    std::vector<uint16_t> idx;
    makeCube(cube, idx, 0.5f);
    g_cubeVb = g.createVertexBuffer(uint32_t(cube.size() * sizeof(VtxPNT)), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED,
                                    cube.data());
    g_cubeIb = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());
    g_tex = g.createTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_tex, 0, makeCheckerARGB(16, 16, 0xfff0a020, 0xff20a0f0, 4).data());
    VtxPCT quad[4] = {{-1.8f, -1.0f, 0.0f, 0xffffffff, 0.0f, 1.0f},
                      {-1.8f, 1.0f, 0.0f, 0xffffffff, 0.0f, 0.0f},
                      {0.2f, 1.0f, 0.0f, 0xffffffff, 1.0f, 0.0f},
                      {0.2f, -1.0f, 0.0f, 0xffffffff, 1.0f, 1.0f}};
    g_quadVb = g.createVertexBuffer(sizeof quad, D3DUSAGE_WRITEONLY, FVF_PCT, D3DPOOL_MANAGED, quad);
    g.dumpRenderTarget(g_rt, "rt0");
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g.setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g.setSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // Pass 1: offscreen.
    g.tag("offscreen");
    g.setRenderTarget(g_rt);
    g.setDepthStencil(g_ds);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kRtClear, 1.0f, 0);
    g.setTransform(D3DTS_VIEW, lookAtLH({0.0f, 1.2f, -2.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
    g.setTransform(D3DTS_PROJECTION, perspectiveFovLH(kPi / 3.0f, 1.0f, 0.5f, 20.0f));
    g.setTransform(D3DTS_WORLD, rotationY(0.4f + 0.3f * float(frame)));
    g.setFVF(FVF_PNT);
    g.setTexture(0, g_tex);
    g.setStreamSource(0, g_cubeVb, 0, sizeof(VtxPNT));
    g.setIndices(g_cubeIb);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);

    // Pass 2: back buffer.
    g.tag("world");
    // Depth first: D3D8 sets colour and depth together and rejects a depth surface smaller than the target.
    g.setDepthStencil(DS_DEFAULT);
    g.setRenderTarget(RT_BACKBUFFER);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202020, 1.0f, 0);
    Camera cam = setCamera(g, {0.0f, 0.0f, -3.0f}, {0.0f, 0.0f, 0.0f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.setFVF(FVF_PCT);
    g.setTexture(0, g_rt);
    g.setStreamSource(0, g_quadVb, 0, sizeof(VtxPCT));
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    g.setFVF(FVF_PNT);
    g.setTexture(0, g_tex);
    g.setStreamSource(0, g_cubeVb, 0, sizeof(VtxPNT));
    g.setTransform(D3DTS_WORLD, mul(rotationY(-0.5f), translation(1.2f, 0.0f, 0.0f)));
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    g.setTexture(0, NONE);

    if (frame == kScene.frames - 1) {
        // Quad uv (0.06, 0.06): the RT's clear colour (the cube does not reach its corner).
        probeAt(g, world, cam, {-1.68f, 0.88f, 0.0f}, 0x60, 0x20, 0x40, 1, "RT clear colour through the quad");
        g.probe(1, 1, 0x20, 0x20, 0x20, 0, "back-buffer clear colour");
    }
}
