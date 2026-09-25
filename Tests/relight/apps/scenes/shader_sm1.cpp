// shader_sm1: shader model 1 through both APIs. A vs_1_1 (D3D9 bytecode carries dcl tokens; D3D8
// bytecode does not and gets its D3DVSD declaration from the vertex element list, element i ->
// register v<i>) transforms by the WVP in c0..c3 and tints the vertex colour by c4. Pixel
// shaders: ps_1_1 (tex t0; mul) and ps_1_4 (texld; mul; add a def constant), and a
// fixed-function-VS + ps_1_1 draw. The texture is one solid colour, so the output follows from
// the inputs (probes).
#include <stdio.h>
#include <stdlib.h>

#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"shader_sm1", 3, false, 0,
                              "vs_1_1 (+ VS constants), ps_1_1, ps_1_4 with def, FF VS + ps_1_1; D3D8 declaration "
                              "tokens vs D3D9 dcl"};

static int g_decl, g_vs, g_ps11, g_ps14, g_vb, g_tex;
static const uint32_t kTex = 0xff80c040; // solid texture colour

void rl::sceneInit(Gfx& g)
{
    if (g.caps().vsVersion < 0xFFFE0101u || g.caps().psVersion < 0xFFFF0104u) {
        fprintf(stderr, "shader_sm1: needs vs_1_1 / ps_1_4 (caps vs %08x ps %08x)\n", g.caps().vsVersion,
                g.caps().psVersion);
        exit(1);
    }
    // Vertex layout = FVF_PCT: v0 position, v1 colour, v2 texcoord.
    g_decl = g.createVertexDeclaration({{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}});
    ShaderAsm vs(false, 1, 1);
    if (!g.isD3D8()) {
        vs.dclUsage("dcl_position v0", D3DDECLUSAGE_POSITION, 0, dst(R_INPUT, 0));
        vs.dclUsage("dcl_color v1", D3DDECLUSAGE_COLOR, 0, dst(R_INPUT, 1));
        vs.dclUsage("dcl_texcoord v2", D3DDECLUSAGE_TEXCOORD, 0, dst(R_INPUT, 2));
    }
    vs.op("m4x4 oPos, v0, c0", OP_M4x4, {dst(R_RASTOUT, 0), src(R_INPUT, 0), src(R_CONST, 0)});
    vs.op("mul oD0, v1, c4", OP_MUL, {dst(R_ATTROUT, 0), src(R_INPUT, 1), src(R_CONST, 4)});
    vs.op("mov oT0, v2", OP_MOV, {dst(R_TEXCRDOUT, 0), src(R_INPUT, 2)});
    g_vs = g.createVertexShader(vs.finish(), vs.listing(), g_decl);

    ShaderAsm p11(true, 1, 1);
    p11.op("tex t0", OP_TEX, {dst(R_TEXTURE, 0)});
    p11.op("mul r0, t0, v0", OP_MUL, {dst(R_TEMP, 0), src(R_TEXTURE, 0), src(R_INPUT, 0)});
    g_ps11 = g.createPixelShader(p11.finish(), p11.listing());

    ShaderAsm p14(true, 1, 4);
    p14.def("def c0, 0.25, 0, 0, 0", 0, 0.25f, 0.0f, 0.0f, 0.0f);
    p14.op("texld r0, t0", OP_TEX, {dst(R_TEMP, 0), src(R_TEXTURE, 0)});
    p14.op("mul r0, r0, v0", OP_MUL, {dst(R_TEMP, 0), src(R_TEMP, 0), src(R_INPUT, 0)});
    p14.op("add r0, r0, c0", OP_ADD, {dst(R_TEMP, 0), src(R_TEMP, 0), src(R_CONST, 0)});
    g_ps14 = g.createPixelShader(p14.finish(), p14.listing());

    g_tex = g.createTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    g.uploadTexture(g_tex, 0, makeCheckerARGB(8, 8, kTex, kTex, 8).data());

    std::vector<VtxPCT> v;
    for (int i = 0; i < 3; ++i) {
        float x = -2.1f + 1.5f * float(i);
        v.push_back({x, -0.6f, 0.0f, 0xffffffff, 0.0f, 1.0f});
        v.push_back({x, 0.6f, 0.0f, 0xffffffff, 0.0f, 0.0f});
        v.push_back({x + 1.2f, 0.6f, 0.0f, 0xffffffff, 1.0f, 0.0f});
        v.push_back({x + 1.2f, -0.6f, 0.0f, 0xffffffff, 1.0f, 1.0f});
    }
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPCT)), D3DUSAGE_WRITEONLY, FVF_PCT, D3DPOOL_MANAGED, v.data());
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
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPCT));

    Mat4 wvpT = transpose(mul(mul(world, cam.view), cam.proj));
    g.setVSConstF(0, wvpT.m, 4);
    const float tint[4] = {1.0f, 0.5f, 1.0f, 1.0f};
    g.setVSConstF(4, tint, 1);
    g.setVertexDeclaration(g_decl);
    g.setVertexShader(g_vs);
    g.setPixelShader(g_ps11);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    g.setPixelShader(g_ps14);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 4, 2);
    // Fixed-function VS + ps_1_1.
    g.setVertexShader(NONE);
    g.setFVF(FVF_PCT);
    g.setPixelShader(g_ps11);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 8, 2);
    g.setPixelShader(NONE);

    if (frame == kScene.frames - 1) {
        // tex (0x80, 0xc0, 0x40) * tint (1, 0.5, 1) [* white vertex colour] (+ 0.25 red for ps_1_4).
        probeAt(g, world, cam, {-1.5f, 0.0f, 0.0f}, 0x80, 0x60, 0x40, 2, "vs_1_1 + ps_1_1");
        probeAt(g, world, cam, {0.0f, 0.0f, 0.0f}, 0xc0, 0x60, 0x40, 2, "vs_1_1 + ps_1_4 + def");
        probeAt(g, world, cam, {1.5f, 0.0f, 0.0f}, 0x80, 0xc0, 0x40, 2, "FF VS + ps_1_1");
    }
}
