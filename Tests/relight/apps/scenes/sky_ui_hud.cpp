// sky_ui_hud: the draw categories Remix classifies by heuristics (plan §1.6 / RL-1.2).
//   sky    : a textured sky box centred on a moving camera, drawn first with Z off and the
//            viewport depth range MinZ = MaxZ = 1 (Remix's sky min-Z heuristic) - tag "sky";
//   world  : a textured floor and three lit cubes - tag "world";
//   hud    : an orthographic HUD pass (VIEW = identity, PROJECTION = ortho, Z off, alpha blend) - tag "hud";
//   hud_positiont : pre-transformed D3DFVF_XYZRHW quads via DrawPrimitiveUP (crosshair, bars) -
//            tag "hud_positiont". The first HUD draw is the UI injection point.
// The camera orbits over the 5 frames; annotations.camera is the last frame's camera.
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"sky_ui_hud", 5, false, 0,
                              "Sky box (viewport MinZ=MaxZ=1, Z off), orbiting camera, world, ortho HUD pass and "
                              "POSITIONT HUD via DrawPrimitiveUP"};

static int g_skyVb, g_skyIb, g_skyTex, g_floorVb, g_floorIb, g_cubeVb, g_cubeIb, g_floorTex, g_hudVb, g_iconTex;
static uint32_t g_floorIdx;

void rl::sceneInit(Gfx& g)
{
    // Sky box: cube of half-size 50 seen from inside; vertical gradient texture 1x32 -> 4x32.
    std::vector<VtxPNT> sky;
    std::vector<uint16_t> skyIdx;
    makeCube(sky, skyIdx, 50.0f);
    std::vector<VtxPCT> skyV;
    for (const VtxPNT& p : sky)
        skyV.push_back({p.x, p.y, p.z, 0xffffffff, 0.5f, 0.5f - p.y / 100.0f});
    g_skyVb = g.createVertexBuffer(uint32_t(skyV.size() * sizeof(VtxPCT)), D3DUSAGE_WRITEONLY, FVF_PCT, D3DPOOL_MANAGED,
                                   skyV.data());
    g_skyIb = g.createIndexBuffer(uint32_t(skyIdx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                  skyIdx.data());
    g_skyTex = g.createTexture(4, 32, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED);
    std::vector<uint32_t> grad(4 * 32);
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            grad[y * 4 + x] = argb(0xff, 40 + y * 4, 90 + y * 4, 200 - y * 2);
    g.uploadTexture(g_skyTex, 0, grad.data());

    std::vector<VtxPNT> fl;
    std::vector<uint16_t> flIdx;
    makeGrid(fl, flIdx, 8, 8, 12.0f);
    g_floorIdx = uint32_t(flIdx.size());
    g_floorVb = g.createVertexBuffer(uint32_t(fl.size() * sizeof(VtxPNT)), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED,
                                     fl.data());
    g_floorIb = g.createIndexBuffer(uint32_t(flIdx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                    flIdx.data());
    g_floorTex = g.createTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_floorTex, 0, makeCheckerARGB(16, 16, 0xff506030, 0xff708040, 8).data());

    std::vector<VtxPNT> cube;
    std::vector<uint16_t> cubeIdx;
    makeCube(cube, cubeIdx, 0.5f);
    g_cubeVb = g.createVertexBuffer(uint32_t(cube.size() * sizeof(VtxPNT)), D3DUSAGE_WRITEONLY, FVF_PNT, D3DPOOL_MANAGED,
                                    cube.data());
    g_cubeIb = g.createIndexBuffer(uint32_t(cubeIdx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                   cubeIdx.data());

    // HUD (ortho, pixel units, y down via the projection): health bar back + fill, icon.
    std::vector<VtxPCT> hud;
    auto rect = [&](float x0, float y0, float x1, float y1, uint32_t c) {
        hud.push_back({x0, y1, 0.5f, c, 0.0f, 1.0f});
        hud.push_back({x0, y0, 0.5f, c, 0.0f, 0.0f});
        hud.push_back({x1, y0, 0.5f, c, 1.0f, 0.0f});
        hud.push_back({x1, y1, 0.5f, c, 1.0f, 1.0f});
    };
    rect(4.0f, 84.0f, 60.0f, 92.0f, 0xc0000000);  // bar background (translucent)
    rect(5.0f, 85.0f, 41.0f, 91.0f, 0xffe03030);  // bar fill (opaque)
    rect(108.0f, 76.0f, 124.0f, 92.0f, 0xffffffff); // icon (textured)
    g_hudVb = g.createVertexBuffer(uint32_t(hud.size() * sizeof(VtxPCT)), D3DUSAGE_WRITEONLY, FVF_PCT, D3DPOOL_MANAGED,
                                   hud.data());
    g_iconTex = g.createTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_iconTex, 0, makeCheckerARGB(8, 8, 0xff20c0f0, 0xfff0f020, 4).data());
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    float ang = 0.2f + 0.15f * float(frame);
    Vec3 eye{6.0f * sinf(ang), 2.5f, -6.0f * cosf(ang)};
    Camera cam = setCamera(g, eye, {0.0f, 0.5f, 0.0f}, kPi / 3.0f, 0.1f, 200.0f);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffff00ff, 1.0f, 0);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g.setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g.setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    g.setTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g.setSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g.setSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    // Sky.
    g.tag("sky");
    g.setViewport(Viewport{0, 0, uint32_t(kWidth), uint32_t(kHeight), 1.0f, 1.0f});
    g.setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g.setRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g.setTransform(D3DTS_WORLD, translation(eye.x, eye.y, eye.z));
    g.setFVF(FVF_PCT);
    g.setTexture(0, g_skyTex);
    g.setStreamSource(0, g_skyVb, 0, sizeof(VtxPCT));
    g.setIndices(g_skyIb);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    g.setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g.setRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g.setViewport(Viewport{0, 0, uint32_t(kWidth), uint32_t(kHeight), 0.0f, 1.0f});

    // World: lit floor and cubes.
    g.tag("world");
    g.setSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    g.setSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    g.setRenderState(D3DRS_LIGHTING, TRUE);
    g.setRenderState(D3DRS_AMBIENT, 0xff404040);
    Light sun{};
    sun.Type = D3DLIGHT_DIRECTIONAL;
    sun.Diffuse = color(1.0f, 0.95f, 0.8f);
    sun.Direction = normalize(Vec3{-0.4f, -1.0f, 0.3f});
    g.setLight(0, sun);
    g.lightEnable(0, true);
    Material m{};
    m.Diffuse = color(1, 1, 1);
    m.Ambient = color(1, 1, 1);
    g.setMaterial(m);
    g.setFVF(FVF_PNT);
    g.setTexture(0, g_floorTex);
    g.setTransform(D3DTS_WORLD, identity());
    g.setStreamSource(0, g_floorVb, 0, sizeof(VtxPNT));
    g.setIndices(g_floorIb);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 81, 0, g_floorIdx / 3);
    g.setTexture(0, g_iconTex);
    g.setStreamSource(0, g_cubeVb, 0, sizeof(VtxPNT));
    g.setIndices(g_cubeIb);
    for (int i = 0; i < 3; ++i) {
        g.setTransform(D3DTS_WORLD, mul(rotationY(0.5f * float(i)), translation(-2.0f + 2.0f * float(i), 0.5f, 1.0f)));
        g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);
    }
    g.setRenderState(D3DRS_LIGHTING, FALSE);

    // Ortho HUD: VIEW = identity, PROJECTION = pixel-space ortho (y down), Z off, alpha blend.
    g.tag("hud");
    g.setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g.setRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g.setRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g.setRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g.setTransform(D3DTS_WORLD, identity());
    g.setTransform(D3DTS_VIEW, identity());
    g.setTransform(D3DTS_PROJECTION, orthoOffCenterLH(0.0f, float(kWidth), float(kHeight), 0.0f, 0.0f, 1.0f));
    g.setFVF(FVF_PCT);
    g.setStreamSource(0, g_hudVb, 0, sizeof(VtxPCT));
    g.setTexture(0, NONE);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 4, 2);
    g.setTexture(0, g_iconTex);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 8, 2);

    // POSITIONT HUD (XYZRHW), user pointer: crosshair (two bars) and a text-like block row.
    g.tag("hud_positiont");
    g.setTexture(0, NONE);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g.setFVF(FVF_TL);
    const uint32_t cw = 0xff00ff80;
    VtxTL cross[12] = {
        {62.0f, 46.5f, 0.0f, 1.0f, cw, 0, 0}, {66.0f, 46.5f, 0.0f, 1.0f, cw, 0, 0}, {66.0f, 49.5f, 0.0f, 1.0f, cw, 0, 0},
        {62.0f, 46.5f, 0.0f, 1.0f, cw, 0, 0}, {66.0f, 49.5f, 0.0f, 1.0f, cw, 0, 0}, {62.0f, 49.5f, 0.0f, 1.0f, cw, 0, 0},
        {62.5f, 42.0f, 0.0f, 1.0f, cw, 0, 0}, {65.5f, 42.0f, 0.0f, 1.0f, cw, 0, 0}, {65.5f, 54.0f, 0.0f, 1.0f, cw, 0, 0},
        {62.5f, 42.0f, 0.0f, 1.0f, cw, 0, 0}, {65.5f, 54.0f, 0.0f, 1.0f, cw, 0, 0}, {62.5f, 54.0f, 0.0f, 1.0f, cw, 0, 0},
    };
    g.drawPrimitiveUP(D3DPT_TRIANGLELIST, 4, cross, sizeof(VtxTL));
    std::vector<VtxTL> text;
    std::vector<uint16_t> textIdx;
    for (int i = 0; i < 6; ++i) {
        float x0 = 4.0f + 7.0f * float(i), y0 = 4.0f, x1 = x0 + 5.0f, y1 = y0 + ((i % 3) + 5);
        uint16_t b = uint16_t(text.size());
        text.push_back({x0, y0, 0.0f, 1.0f, 0xfff0f0f0, 0, 0});
        text.push_back({x1, y0, 0.0f, 1.0f, 0xfff0f0f0, 0, 0});
        text.push_back({x1, y1, 0.0f, 1.0f, 0xfff0f0f0, 0, 0});
        text.push_back({x0, y1, 0.0f, 1.0f, 0xfff0f0f0, 0, 0});
        uint16_t q[6] = {b, uint16_t(b + 1), uint16_t(b + 2), b, uint16_t(b + 2), uint16_t(b + 3)};
        textIdx.insert(textIdx.end(), q, q + 6);
    }
    g.drawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, uint32_t(text.size()), uint32_t(textIdx.size() / 3), textIdx.data(),
                             D3DFMT_INDEX16, text.data(), sizeof(VtxTL));
    g.setRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g.setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g.tag("world");

    if (frame == kScene.frames - 1) {
        (void)cam;
        g.probe(20, 88, 0xe0, 0x30, 0x30, 1, "ortho HUD bar fill");
        g.probe(64, 48, 0x00, 0xff, 0x80, 0, "POSITIONT crosshair");
        g.probe(6, 6, 0xf0, 0xf0, 0xf0, 0, "POSITIONT text block");
    }
}
