// raster_vs_rhw (D3D9 only): the geometry the raster remaster (RL-4.2) takes from other sources than the
// fixed-function vertex stream, next to one fixed-function draw that gives the frame its camera:
//   ff_cube      fixed-function cube (unlit, face colours) - the Main camera draw;
//   vs_quad_a    vs_2_0 quad (m4x4 oPos, v0, c0; mov oD0, v1) whose c0..c3 = WORLD x VIEW x PROJECTION of the
//                D3DTS_* matrices set for it: the RL-1.6 capture's object-space positions re-project exactly;
//   vs_quad_b    the same shader, drawn with BaseVertexIndex 4, whose constants carry a world translation the
//                D3DTS_WORLD matrix does not (identity) and a projection with another depth range than
//                D3DTS_PROJECTION: Remix's back-transform (inverse projection without the perspective divide)
//                cannot reproduce its clip positions;
//   rhw_backdrop pre-transformed (D3DFVF_XYZRHW) band across the top at z = 0.995, rhw 1 (drawn first; the
//                geometry in front occludes it);
//   rhw_panel    pre-transformed quad at z = 0.2, rhw 0.5, drawn last over the others.
// No UI: with rtx.preTransformedVerticesIsUI off, the XYZRHW draws are scene draws Remix rasterizes (PositionT).
#include <stdio.h>
#include <stdlib.h>

#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"raster_vs_rhw", 3, false, 0,
                              "vs_2_0 quads (D3D transforms consistent / inconsistent with the shader constants, "
                              "BaseVertexIndex), XYZRHW backdrop + panel, fixed-function cube"};

static int g_decl, g_vs, g_vbCube, g_ibCube, g_vbQuad, g_ibQuad;
static uint32_t g_cubeIdx;
static const uint32_t kQuadA = 0xff40c040, kQuadB = 0xffc04040, kBackdrop = 0xff203860, kPanel = 0xffe0a020;

void rl::sceneInit(Gfx& g)
{
    if (g.caps().vsVersion < 0xFFFE0200u) {
        fprintf(stderr, "raster_vs_rhw: needs vs_2_0\n");
        exit(1);
    }
    g_decl = g.createVertexDeclaration({{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}});
    ShaderAsm vs(false, 2, 0);
    vs.dclUsage("dcl_position v0", D3DDECLUSAGE_POSITION, 0, dst(R_INPUT, 0));
    vs.dclUsage("dcl_color v1", D3DDECLUSAGE_COLOR, 0, dst(R_INPUT, 1));
    vs.op("m4x4 oPos, v0, c0", OP_M4x4, {dst(R_RASTOUT, 0), src(R_INPUT, 0), src(R_CONST, 0)});
    vs.op("mov oD0, v1", OP_MOV, {dst(R_ATTROUT, 0), src(R_INPUT, 1)});
    g_vs = g.createVertexShader(vs.finish(), vs.listing(), g_decl);

    std::vector<VtxPNT> cube;
    std::vector<uint16_t> idx;
    makeCube(cube, idx, 0.5f);
    std::vector<VtxPC> v;
    for (const VtxPNT& p : cube) {
        uint32_t c = argb(0xff, uint32_t(128 + 100 * p.nx), uint32_t(160 + 60 * p.ny), uint32_t(128 - 100 * p.nz));
        v.push_back({p.x, p.y, p.z, c});
    }
    g_cubeIdx = uint32_t(idx.size());
    g_vbCube = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPC)), D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED, v.data());
    g_ibCube = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());

    // Two unit quads (4 vertices each, uniform colour): A at vertices 0..3, B at 4..7 (BaseVertexIndex 4).
    VtxPC q[8];
    const float c[4][2] = {{-0.5f, -0.5f}, {-0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, -0.5f}};
    for (int k = 0; k < 4; ++k) {
        q[k] = {c[k][0], c[k][1], 0.0f, kQuadA};
        q[4 + k] = {c[k][0], c[k][1], 0.0f, kQuadB};
    }
    g_vbQuad = g.createVertexBuffer(sizeof q, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, q);
    const uint16_t qi[6] = {0, 1, 2, 0, 2, 3};
    g_ibQuad = g.createIndexBuffer(sizeof qi, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, qi);
}

static void rhwQuad(Gfx& g, float x0, float y0, float x1, float y1, float z, float rhw, uint32_t color)
{
    const VtxTL t[4] = {{x0, y0, z, rhw, color, 0.0f, 0.0f},
                        {x1, y0, z, rhw, color, 1.0f, 0.0f},
                        {x1, y1, z, rhw, color, 1.0f, 1.0f},
                        {x0, y1, z, rhw, color, 0.0f, 1.0f}};
    g.setFVF(FVF_TL);
    g.drawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, t, sizeof(VtxTL));
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 0.3f, -4.0f}, {0.0f, 0.0f, 0.0f});
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g.setTexture(0, NONE);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); // the vertex colour, no texture
    g.setTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g.setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    g.setTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    g.tag("rhw_backdrop");
    rhwQuad(g, 0.0f, 0.0f, 128.0f, 40.0f, 0.995f, 1.0f, kBackdrop);

    g.tag("ff_cube");
    Mat4 cubeWorld = mul(rotationY(0.6f + 0.1f * float(frame)), translation(-1.5f, 0.0f, 0.0f));
    g.setTransform(D3DTS_WORLD, cubeWorld);
    g.setFVF(FVF_PC);
    g.setStreamSource(0, g_vbCube, 0, sizeof(VtxPC));
    g.setIndices(g_ibCube);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, g_cubeIdx / 3);

    g.setVertexDeclaration(g_decl);
    g.setVertexShader(g_vs);
    g.setStreamSource(0, g_vbQuad, 0, sizeof(VtxPC));
    g.setIndices(g_ibQuad);
    g.tag("vs_quad_a");
    Mat4 worldA = mul(scaling(1.2f, 1.2f, 1.2f), translation(0.0f, 0.1f, 0.0f));
    g.setTransform(D3DTS_WORLD, worldA);
    Mat4 wvpA = transpose(mul(mul(worldA, cam.view), cam.proj));
    g.setVSConstF(0, wvpA.m, 4);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    g.tag("vs_quad_b");
    Mat4 worldB = translation(1.5f, -0.2f, 0.0f); // in the constants only
    Camera camB = cam;
    camB.proj = perspectiveFovLH(kPi / 4.0f, cam.aspect, 1.0f, 20.0f); // in the constants only (other depth range)
    g.setTransform(D3DTS_WORLD, identity());
    Mat4 wvpB = transpose(mul(mul(worldB, camB.view), camB.proj));
    g.setVSConstF(0, wvpB.m, 4);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 4, 0, 4, 0, 2);
    g.setVertexShader(NONE);

    g.tag("rhw_panel");
    rhwQuad(g, 92.0f, 62.0f, 120.0f, 88.0f, 0.2f, 0.5f, kPanel);

    if (frame == kScene.frames - 1) {
        g.probe(4, 4, 0x20, 0x38, 0x60, 0, "XYZRHW backdrop");
        g.probe(106, 75, 0xe0, 0xa0, 0x20, 0, "XYZRHW panel");
        probeAt(g, worldA, cam, {0.0f, 0.0f, 0.0f}, 0x40, 0xc0, 0x40, 0, "vs_2_0 quad A (vertex colour)");
        probeAt(g, worldB, camB, {0.0f, 0.0f, 0.0f}, 0xc0, 0x40, 0x40, 0, "vs_2_0 quad B (constants-only world)");
        g.probe(1, 90, 0, 0, 0, 0, "clear colour");
    }
}
