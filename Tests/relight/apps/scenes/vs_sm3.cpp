// vs_sm3 (D3D9 only): shader model 3. vs_3_0 with declared outputs (dcl_position o0,
// dcl_texcoord o1, dcl_color o2), integer and boolean constants set by the app
// (SetVertexShaderConstantI / B: the Remix vertex-shader hash covers F, I and B constants), a
// loop over i1 and an if on b0, plus defi; ps_3_0 with dcl_texcoord / dcl_color inputs.
// colour = vertex colour + i1.x * c8 + (b0 ? c9 : 0), sampled texture is white -> probes.
#include <stdio.h>
#include <stdlib.h>

#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"vs_sm3", 3, false, 0,
                              "vs_3_0 + ps_3_0: declared outputs, loop on app-set int constant, if on app-set bool "
                              "constant, defi"};

static int g_decl, g_vs, g_ps, g_vb, g_tex;

void rl::sceneInit(Gfx& g)
{
    if (g.caps().vsVersion < 0xFFFE0300u || g.caps().psVersion < 0xFFFF0300u) {
        fprintf(stderr, "vs_sm3: needs vs_3_0 / ps_3_0\n");
        exit(1);
    }
    g_decl = g.createVertexDeclaration({{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}});
    ShaderAsm vs(false, 3, 0);
    vs.dclUsage("dcl_position v0", D3DDECLUSAGE_POSITION, 0, dst(R_INPUT, 0));
    vs.dclUsage("dcl_color v1", D3DDECLUSAGE_COLOR, 0, dst(R_INPUT, 1));
    vs.dclUsage("dcl_texcoord v2", D3DDECLUSAGE_TEXCOORD, 0, dst(R_INPUT, 2));
    vs.dclUsage("dcl_position o0", D3DDECLUSAGE_POSITION, 0, dst(R_OUTPUT, 0));
    vs.dclUsage("dcl_texcoord o1.xy", D3DDECLUSAGE_TEXCOORD, 0, dst(R_OUTPUT, 1, MASK_XY));
    vs.dclUsage("dcl_color o2", D3DDECLUSAGE_COLOR, 0, dst(R_OUTPUT, 2));
    vs.def("def c10, 0, 0, 0, 0", 10, 0.0f, 0.0f, 0.0f, 0.0f);
    vs.defi("defi i0, 1, 0, 1, 0", 0, 1, 0, 1, 0);
    vs.op("m4x4 o0, v0, c0", OP_M4x4, {dst(R_OUTPUT, 0), src(R_INPUT, 0), src(R_CONST, 0)});
    vs.op("mov r1, c10", OP_MOV, {dst(R_TEMP, 1), src(R_CONST, 10)});
    vs.op("loop aL, i1", OP_LOOP, {src(R_LOOP, 0), src(R_CONSTINT, 1)});
    vs.op("add r1, r1, c8", OP_ADD, {dst(R_TEMP, 1), src(R_TEMP, 1), src(R_CONST, 8)});
    vs.op("endloop", OP_ENDLOOP, {});
    vs.op("loop aL, i0", OP_LOOP, {src(R_LOOP, 0), src(R_CONSTINT, 0)});
    vs.op("add r1, r1, c10", OP_ADD, {dst(R_TEMP, 1), src(R_TEMP, 1), src(R_CONST, 10)});
    vs.op("endloop", OP_ENDLOOP, {});
    vs.op("if b0", OP_IF, {src(R_CONSTBOOL, 0)});
    vs.op("add r1, r1, c9", OP_ADD, {dst(R_TEMP, 1), src(R_TEMP, 1), src(R_CONST, 9)});
    vs.op("endif", OP_ENDIF, {});
    vs.op("add o2, v1, r1", OP_ADD, {dst(R_OUTPUT, 2), src(R_INPUT, 1), src(R_TEMP, 1)});
    vs.op("mov o1.xy, v2", OP_MOV, {dst(R_OUTPUT, 1, MASK_XY), src(R_INPUT, 2)});
    g_vs = g.createVertexShader(vs.finish(), vs.listing(), g_decl);

    ShaderAsm ps(true, 3, 0);
    ps.dclUsage("dcl_texcoord v0.xy", D3DDECLUSAGE_TEXCOORD, 0, dst(R_INPUT, 0, MASK_XY));
    ps.dclUsage("dcl_color v1", D3DDECLUSAGE_COLOR, 0, dst(R_INPUT, 1));
    ps.dclSampler2D("dcl_2d s0", 0);
    ps.op("texld r0, v0, s0", OP_TEX, {dst(R_TEMP, 0), src(R_INPUT, 0), src(R_SAMPLER, 0)});
    ps.op("mul oC0, r0, v1", OP_MUL, {dst(R_COLOROUT, 0), src(R_TEMP, 0), src(R_INPUT, 1)});
    g_ps = g.createPixelShader(ps.finish(), ps.listing());

    std::vector<VtxPCT> v;
    for (int i = 0; i < 2; ++i) {
        float x = -1.5f + 1.7f * float(i);
        v.push_back({x, -0.6f, 0.0f, 0xff204080, 0.0f, 1.0f});
        v.push_back({x, 0.6f, 0.0f, 0xff204080, 0.0f, 0.0f});
        v.push_back({x + 1.3f, 0.6f, 0.0f, 0xff204080, 1.0f, 0.0f});
        v.push_back({x + 1.3f, -0.6f, 0.0f, 0xff204080, 1.0f, 1.0f});
    }
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPCT)), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, v.data());
    g_tex = g.createTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_tex, 0, makeCheckerARGB(8, 8, 0xffffffff, 0xffffffff, 8).data());
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
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPCT));
    Mat4 wvpT = transpose(mul(mul(world, cam.view), cam.proj));
    g.setVSConstF(0, wvpT.m, 4);
    const float c8[4] = {0.25f, 0.0f, 0.0f, 0.0f}, c9[4] = {0.0f, 0.25f, 0.0f, 0.0f};
    g.setVSConstF(8, c8, 1);
    g.setVSConstF(9, c9, 1);
    g.setVertexShader(g_vs);
    g.setPixelShader(g_ps);
    // Quad 0: i1 = 2 iterations, b0 = false. Quad 1: i1 = 1 iteration, b0 = true.
    const int i1a[4] = {2, 0, 1, 0}, i1b[4] = {1, 0, 1, 0};
    const int b0a = FALSE, b0b = TRUE;
    g.setVSConstI(1, i1a, 1);
    g.setVSConstB(0, &b0a, 1);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    g.setVSConstI(1, i1b, 1);
    g.setVSConstB(0, &b0b, 1);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 4, 2);
    g.setVertexShader(NONE);
    g.setPixelShader(NONE);

    if (frame == kScene.frames - 1) {
        // Vertex colour (0x20, 0x40, 0x80) = (0.125, 0.251, 0.502).
        probeAt(g, world, cam, {-0.85f, 0.0f, 0.0f}, 0x9f, 0x40, 0x80, 2, "2 loop iterations, b0 false");
        probeAt(g, world, cam, {0.85f, 0.0f, 0.0f}, 0x60, 0x80, 0x80, 2, "1 loop iteration, b0 true");
    }
}
