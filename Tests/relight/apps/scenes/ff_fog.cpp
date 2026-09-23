// ff_fog: fixed-function fog in four viewports (quadrants): vertex fog LINEAR, vertex fog LINEAR
// with RANGEFOGENABLE, table (pixel) fog EXP (pillars) / EXP2 (wall), table fog LINEAR. The same receding row of
// pillars and a far wall are drawn in each; the far wall lies beyond FOGEND in the linear
// quadrants, so it is exactly the fog colour there (probes).
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_fog", 3, false, 0,
                              "Vertex fog (LINEAR, RANGEFOG) and table fog (EXP, EXP2, LINEAR) in four viewports"};

static int g_vb, g_ib;
static uint32_t g_cubeIdx;
static const uint32_t kFog = 0xff8090a0;

void rl::sceneInit(Gfx& g)
{
    std::vector<VtxPNT> cube;
    std::vector<uint16_t> idx;
    makeCube(cube, idx, 0.5f);
    // Unlit: convert to position + colour (face colours from the normal).
    std::vector<VtxPC> v;
    for (const VtxPNT& p : cube) {
        uint32_t c = argb(0xff, uint32_t(128 + 100 * p.nx), uint32_t(160 + 60 * p.ny), uint32_t(128 - 100 * p.nz));
        v.push_back({p.x, p.y, p.z, c});
    }
    g_cubeIdx = uint32_t(idx.size());
    // Far wall quad [24..27].
    uint32_t base = uint32_t(v.size());
    v.push_back({-20.0f, -5.0f, 0.0f, 0xffff2020});
    v.push_back({-20.0f, 5.0f, 0.0f, 0xffff2020});
    v.push_back({20.0f, 5.0f, 0.0f, 0xffff2020});
    v.push_back({20.0f, -5.0f, 0.0f, 0xffff2020});
    const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
    for (uint16_t k : q)
        idx.push_back(uint16_t(base + k));
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPC)), D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED, v.data());
    g_ib = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 1.0f, -3.0f}, {0.0f, 0.3f, 6.0f});
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g.setFVF(FVF_PC);
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPC));
    g.setIndices(g_ib);
    g.setRenderState(D3DRS_FOGENABLE, TRUE);
    g.setRenderState(D3DRS_FOGCOLOR, kFog);

    struct Mode {
        uint32_t vertexMode, tableMode, range;
        float start, end, density;
    };
    const Mode modes[4] = {
        {D3DFOG_LINEAR, D3DFOG_NONE, FALSE, 2.0f, 22.0f, 0.0f},
        {D3DFOG_LINEAR, D3DFOG_NONE, TRUE, 2.0f, 22.0f, 0.0f},
        {D3DFOG_NONE, D3DFOG_EXP, FALSE, 0.0f, 0.0f, 0.06f},
        {D3DFOG_NONE, D3DFOG_LINEAR, FALSE, 3.0f, 24.0f, 0.0f},
    };
    // The perspective matrix is for the full back buffer; each quadrant viewport shows the whole view.
    for (int q = 0; q < 4; ++q) {
        Viewport vp{uint32_t(q % 2) * 64, uint32_t(q / 2) * 48, 64, 48, 0.0f, 1.0f};
        g.setViewport(vp);
        const Mode& m = modes[q];
        g.setRenderState(D3DRS_FOGVERTEXMODE, m.vertexMode);
        g.setRenderState(D3DRS_FOGTABLEMODE, m.tableMode);
        g.setRenderState(D3DRS_RANGEFOGENABLE, m.range);
        g.setRenderState(D3DRS_FOGSTART, f2dw(m.start));
        g.setRenderState(D3DRS_FOGEND, f2dw(m.end));
        g.setRenderState(D3DRS_FOGDENSITY, f2dw(m.density));
        for (int i = 0; i < 6; ++i) {
            Mat4 world = mul(rotationY(0.4f * float(i) + 0.1f * float(frame)), translation(i % 2 ? 1.2f : -1.2f, 0.0f, 1.0f + 2.0f * float(i)));
            g.setTransform(D3DTS_WORLD, world);
            g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, g_cubeIdx / 3);
        }
        Mat4 wall = translation(0.0f, 0.0f, 40.0f);
        g.setTransform(D3DTS_WORLD, wall);
        if (m.tableMode == D3DFOG_EXP)
            g.setRenderState(D3DRS_FOGTABLEMODE, D3DFOG_EXP2); // the EXP quadrant draws its wall with EXP2
        g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 24, 4, g_cubeIdx, 2);
    }
    g.setRenderState(D3DRS_FOGENABLE, FALSE);
    g.setViewport(Viewport{0, 0, uint32_t(kWidth), uint32_t(kHeight), 0.0f, 1.0f});

    if (frame == kScene.frames - 1) {
        // Wall centre, top of each linear quadrant: z = 40 is beyond FOGEND -> pure fog colour.
        int x, y;
        if (projectToPixel(mul(mul(translation(0.0f, 0.0f, 40.0f), cam.view), cam.proj), {0.0f, 3.0f, 0.0f}, 64, 48, x, y)) {
            g.probe(x, y, 0x80, 0x90, 0xa0, 2, "vertex LINEAR fog beyond FOGEND");
            g.probe(64 + x, y, 0x80, 0x90, 0xa0, 2, "vertex LINEAR range fog beyond FOGEND");
            g.probe(64 + x, 48 + y, 0x80, 0x90, 0xa0, 2, "table LINEAR fog beyond FOGEND");
        }
    }
}
