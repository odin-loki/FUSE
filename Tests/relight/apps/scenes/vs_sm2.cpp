// vs_sm2 (D3D9 only): shader model 2 with a two-stream vertex declaration (stream 0: position,
// normal, texcoord; stream 1: colour). vs_2_0 does N.L lighting from VS constants (c4 light
// direction, c5 ambient, def c7) and writes oD0 / oT0.xy; ps_2_0 samples s0 and modulates. Draws:
// vs_2_0 + ps_2_0, vs_2_0 + fixed-function pixel stages, fixed-function VS (declaration) + ps_2_0.
#include <stdio.h>
#include <stdlib.h>

#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"vs_sm2", 3, false, 0,
                              "vs_2_0 + ps_2_0, two vertex streams via declaration, VS constants + def, "
                              "vs_2_0 + FF pixel, FF VS + ps_2_0"};

struct VtxPN2 {
    float x, y, z, nx, ny, nz, u, v;
};
static int g_decl, g_vs, g_ps, g_vb0, g_vb1, g_ib, g_tex;
static const uint32_t kTex = 0xffc0c0c0;

void rl::sceneInit(Gfx& g)
{
    if (g.caps().vsVersion < 0xFFFE0200u || g.caps().psVersion < 0xFFFF0200u) {
        fprintf(stderr, "vs_sm2: needs vs_2_0 / ps_2_0\n");
        exit(1);
    }
    g_decl = g.createVertexDeclaration({{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0}});
    ShaderAsm vs(false, 2, 0);
    vs.dclUsage("dcl_position v0", D3DDECLUSAGE_POSITION, 0, dst(R_INPUT, 0));
    vs.dclUsage("dcl_normal v1", D3DDECLUSAGE_NORMAL, 0, dst(R_INPUT, 1));
    vs.dclUsage("dcl_texcoord v2", D3DDECLUSAGE_TEXCOORD, 0, dst(R_INPUT, 2));
    vs.dclUsage("dcl_color v3", D3DDECLUSAGE_COLOR, 0, dst(R_INPUT, 3));
    vs.def("def c7, 0, 0, 0, 0", 7, 0.0f, 0.0f, 0.0f, 0.0f);
    vs.op("m4x4 oPos, v0, c0", OP_M4x4, {dst(R_RASTOUT, 0), src(R_INPUT, 0), src(R_CONST, 0)});
    vs.op("dp3 r0.x, v1, c4", OP_DP3, {dst(R_TEMP, 0, MASK_X), src(R_INPUT, 1), src(R_CONST, 4)});
    vs.op("max r0.x, r0.x, c7.x", OP_MAX, {dst(R_TEMP, 0, MASK_X), src(R_TEMP, 0, SWZ_XXXX), src(R_CONST, 7, SWZ_XXXX)});
    vs.op("mad oD0, v3, r0.x, c5", OP_MAD,
          {dst(R_ATTROUT, 0), src(R_INPUT, 3), src(R_TEMP, 0, SWZ_XXXX), src(R_CONST, 5)});
    vs.op("mov oT0.xy, v2", OP_MOV, {dst(R_TEXCRDOUT, 0, MASK_XY), src(R_INPUT, 2)});
    g_vs = g.createVertexShader(vs.finish(), vs.listing(), g_decl);

    ShaderAsm ps(true, 2, 0);
    ps.dclPlain("dcl t0.xy", dst(R_TEXTURE, 0, MASK_XY));
    ps.dclPlain("dcl v0", dst(R_INPUT, 0));
    ps.dclSampler2D("dcl_2d s0", 0);
    ps.op("texld r0, t0, s0", OP_TEX, {dst(R_TEMP, 0), src(R_TEXTURE, 0), src(R_SAMPLER, 0)});
    ps.op("mul r0, r0, v0", OP_MUL, {dst(R_TEMP, 0), src(R_TEMP, 0), src(R_INPUT, 0)});
    ps.op("mov oC0, r0", OP_MOV, {dst(R_COLOROUT, 0), src(R_TEMP, 0)});
    g_ps = g.createPixelShader(ps.finish(), ps.listing());

    std::vector<VtxPN2> v;
    std::vector<uint32_t> c;
    const uint32_t colors[3] = {0xff8080ff, 0xffffffff, 0xff80ff80};
    for (int i = 0; i < 3; ++i) {
        float x = -2.1f + 1.5f * float(i);
        v.push_back({x, -0.6f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f});
        v.push_back({x, 0.6f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f});
        v.push_back({x + 1.2f, 0.6f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f});
        v.push_back({x + 1.2f, -0.6f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f});
        for (int k = 0; k < 4; ++k)
            c.push_back(colors[i]);
    }
    g_vb0 = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPN2)), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, v.data());
    g_vb1 = g.createVertexBuffer(uint32_t(c.size() * 4), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, c.data());
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    g_ib = g.createIndexBuffer(sizeof idx, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx);
    g_tex = g.createTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_tex, 0, makeCheckerARGB(8, 8, kTex, kTex, 8).data());
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 0.0f, -3.5f}, {0.0f, 0.0f, 0.0f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g.setTexture(0, g_tex);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g.setVertexDeclaration(g_decl);
    g.setStreamSource(0, g_vb0, 0, sizeof(VtxPN2));
    g.setStreamSource(1, g_vb1, 0, 4);
    g.setIndices(g_ib);

    Mat4 wvpT = transpose(mul(mul(world, cam.view), cam.proj));
    g.setVSConstF(0, wvpT.m, 4);
    const float lightDir[4] = {0.0f, 0.0f, -1.0f, 0.0f}; // towards the light, object space
    const float ambient[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g.setVSConstF(4, lightDir, 1);
    g.setVSConstF(5, ambient, 1);
    g.setVertexShader(g_vs);
    g.setPixelShader(g_ps);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    g.setPixelShader(NONE); // fixed-function pixel stages (default MODULATE texture x diffuse)
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 4, 0, 4, 0, 2);
    g.setVertexShader(NONE); // fixed-function VS through the declaration (unlit: colour from stream 1)
    g.setPixelShader(g_ps);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 8, 0, 4, 0, 2);
    g.setPixelShader(NONE);
    g.setStreamSource(1, NONE, 0, 0);

    if (frame == kScene.frames - 1) {
        // texture 0xc0 x vertex colour, N.L = 1, ambient 0.
        probeAt(g, world, cam, {-1.5f, 0.0f, 0.0f}, 0x60, 0x60, 0xc0, 2, "vs_2_0 + ps_2_0");
        probeAt(g, world, cam, {0.0f, 0.0f, 0.0f}, 0xc0, 0xc0, 0xc0, 2, "vs_2_0 + FF pixel");
        probeAt(g, world, cam, {1.5f, 0.0f, 0.0f}, 0x60, 0xc0, 0x60, 2, "FF VS + ps_2_0");
    }
}
