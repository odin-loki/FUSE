// ff_multi_instance: one cube mesh (one VB + IB, one texture pair) drawn as 9 instances per frame
// for 60 frames, every frame recorded. Instances 0..3 are static, 4..6 move a little each frame
// (well within Remix's default uniqueObjectDistance of 300 units), 7 spins in place, and 8
// teleports 1200 units at frame 30 (beyond uniqueObjectDistance -> a new instance for the
// capture). World units are centimetre-like (cube = 100 units), as in most games.
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_multi_instance", 60, true, 0,
                              "Same mesh x9 instances over 60 recorded frames: static, moving, spinning, and a "
                              "teleport past uniqueObjectDistance at frame 30"};

static int g_vb, g_ib, g_texA, g_texB;
static const int kTeleportFrame = 30;

void rl::sceneInit(Gfx& g)
{
    std::vector<VtxPNT> v;
    std::vector<uint16_t> idx;
    makeCube(v, idx, 50.0f);
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPNT)), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED, v.data());
    g_ib = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());
    g_texA = g.createTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_texA, 0, makeCheckerARGB(16, 16, 0xffc04040, 0xfff0f0f0, 4).data());
    g_texB = g.createTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_texB, 0, makeCheckerARGB(16, 16, 0xff4060c0, 0xfff0f0f0, 8).data());

    JV inst = JV::obj();
    inst.set("static", JV::str("0-3"));
    inst.set("moving", JV::str("4-6 (<= 6 units per frame)"));
    inst.set("spinning", JV::str("7"));
    JV tp = JV::obj();
    tp.set("instance", JV::integer(8));
    tp.set("frame", JV::integer(kTeleportFrame));
    tp.set("distance", JV::num(1200.0));
    tp.set("assumed_unique_object_distance", JV::num(300.0));
    inst.set("teleport", tp);
    g.annotate("instances", inst);
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    setCamera(g, {0.0f, 500.0f, -1500.0f}, {0.0f, 0.0f, 0.0f}, kPi / 3.0f, 10.0f, 10000.0f);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff303038, 1.0f, 0);
    g.setFVF(FVF_PNT);
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPNT));
    g.setIndices(g_ib);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    float f = float(frame);
    for (int i = 0; i < 9; ++i) {
        float x = -600.0f + 300.0f * float(i % 5), z = i < 5 ? 0.0f : 400.0f;
        Mat4 world = translation(x, 0.0f, z);
        if (i >= 4 && i <= 6)
            world = translation(x + 4.0f * f, 3.0f * (float((frame * (i + 1)) % 7) - 3.0f), z - 2.0f * f);
        else if (i == 7)
            world = mul(rotationY(0.1f * f), translation(x, 0.0f, z));
        else if (i == 8)
            world = translation(frame < kTeleportFrame ? -600.0f : 600.0f, 150.0f, 800.0f);
        g.setTexture(0, i % 3 == 0 ? g_texB : g_texA);
        g.setTransform(D3DTS_WORLD, world);
        g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    }
    if (frame == kScene.frames - 1)
        g.probe(1, 1, 0x30, 0x30, 0x38, 0, "clear colour");
}
