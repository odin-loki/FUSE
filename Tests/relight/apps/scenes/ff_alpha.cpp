// ff_alpha: alpha test (GREATER / LESSEQUAL) on an alpha-ramp texture, and alpha
// blending: SRCALPHA/INVSRCALPHA, ONE/ONE, DESTCOLOR/ZERO, ONE/INVSRCALPHA, SRCALPHA/ONE, and the blend ops
// ADD / SUBTRACT / REVSUBTRACT / MIN / MAX; D3D9 also sets separate alpha blending
// (SEPARATEALPHABLENDENABLE + SRCBLENDALPHA / DESTBLENDALPHA / BLENDOPALPHA).
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_alpha", 3, false, 0,
                              "Alpha test ops on an alpha ramp, blend factors and blend ops, separate alpha (D3D9)"};

static int g_ramp, g_vb;
static const uint32_t kBgLeft = 0xff4060a0, kBgRight = 0xffc0a040;

static void cell(int i, float& x, float& y)
{
    x = -2.3f + float(i % 4) * 1.2f;
    y = 0.9f - float(i / 4) * 1.2f;
}

void rl::sceneInit(Gfx& g)
{
    // 32x8 ramp: colour white, alpha = 8 * x + 4 (4 .. 252).
    g_ramp = g.createTexture(32, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    std::vector<uint32_t> t(32 * 8);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 32; ++x)
            t[y * 32 + x] = argb(8 * x + 4, 0xff, 0xff, 0xff);
    g.uploadTexture(g_ramp, 0, t.data());

    // Vertices: [0..3] background left, [4..7] background right, then 10 cells x 4 vertices.
    std::vector<VtxPCT> v;
    auto quad = [&](float x0, float y0, float x1, float y1, float z, uint32_t c) {
        v.push_back({x0, y0, z, c, 0.0f, 1.0f});
        v.push_back({x0, y1, z, c, 0.0f, 0.0f});
        v.push_back({x1, y1, z, c, 1.0f, 0.0f});
        v.push_back({x1, y0, z, c, 1.0f, 1.0f});
    };
    quad(-3.0f, -2.0f, 0.0f, 2.0f, 1.0f, kBgLeft);
    quad(0.0f, -2.0f, 3.0f, 2.0f, 1.0f, kBgRight);
    for (int i = 0; i < 10; ++i) {
        float x, y;
        cell(i, x, y);
        // Blend cells use a uniform colour with alpha 0x80; the alpha-test cells are white.
        uint32_t c = i < 2 ? 0xffffffff : argb(0x80, 0xff, 0x40 + 0x10 * uint32_t(i - 2), 0x20);
        quad(x, y, x + 1.0f, y + 1.0f, 0.5f, c);
    }
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPCT)), D3DUSAGE_WRITEONLY, FVF_PCT, D3DPOOL_MANAGED,
                                v.data());
}

static void blend(Gfx& g, uint32_t src, uint32_t dst, uint32_t op)
{
    g.setRenderState(D3DRS_SRCBLEND, src);
    g.setRenderState(D3DRS_DESTBLEND, dst);
    g.setRenderState(D3DRS_BLENDOP, op);
}

static uint8_t clamp8(float f) { return uint8_t(f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : floorf(f + 0.5f)); }

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 0.0f, -3.6f}, {0.0f, 0.0f, 0.0f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g.setFVF(FVF_PCT);
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPCT));
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g.setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g.setTexture(0, NONE);
    g.tag("background");
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 4, 2);
    g.tag("world");

    // Alpha test on the ramp texture (colour and alpha from the texture).
    g.setTexture(0, g_ramp);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g.setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g.setRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    g.setRenderState(D3DRS_ALPHAREF, 0x80);
    g.setRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 8, 2);
    g.setRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 12, 2);
    g.setRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    // Blending with the uniform-colour cells (alpha 0x80).
    g.setTexture(0, NONE);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g.setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g.setRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g.setRenderState(D3DRS_ZWRITEENABLE, FALSE);
    struct B {
        uint32_t src, dst, op;
    };
    const B modes[8] = {
        {D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD},
        {D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD},
        {D3DBLEND_DESTCOLOR, D3DBLEND_ZERO, D3DBLENDOP_ADD},
        {D3DBLEND_ONE, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD},
        {D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_SUBTRACT},
        {D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_REVSUBTRACT},
        {D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_MIN},
        {D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_MAX},
    };
    for (int i = 0; i < 8; ++i) {
        blend(g, modes[i].src, modes[i].dst, modes[i].op);
        if (i == 0 && !g.isD3D8()) {
            // Separate alpha: the X8R8G8B8 back buffer drops alpha, so this only shows in the capture.
            g.setRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            g.setRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
            g.setRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
            g.setRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_MAX);
        }
        g.drawPrimitive(D3DPT_TRIANGLEFAN, 16 + 4 * i, 2);
        if (i == 0 && !g.isD3D8())
            g.setRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    }
    g.setRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g.setRenderState(D3DRS_ZWRITEENABLE, TRUE);
    blend(g, D3DBLEND_ONE, D3DBLEND_ZERO, D3DBLENDOP_ADD);

    if (frame == kScene.frames - 1) {
        float x, y;
        // Alpha test cells: left quarter of the ramp (alpha <= 0x80) vs right quarter (> 0x80).
        cell(0, x, y);
        probeAt(g, world, cam, {x + 0.12f, y + 0.5f, 0.5f}, 0x40, 0x60, 0xa0, 0, "GREATER rejects low alpha");
        probeAt(g, world, cam, {x + 0.88f, y + 0.5f, 0.5f}, 0xff, 0xff, 0xff, 0, "GREATER keeps high alpha");
        cell(1, x, y);
        probeAt(g, world, cam, {x + 0.12f, y + 0.5f, 0.5f}, 0xff, 0xff, 0xff, 0, "LESSEQUAL keeps low alpha");
        // SRCALPHA / INVSRCALPHA over the right background: c = s * a + d * (1 - a), a = 128/255.
        cell(2, x, y);
        const float a = 128.0f / 255.0f;
        uint32_t c2 = kBgRight, s2 = argb(0x80, 0xff, 0x40, 0x20);
        auto ch = [](uint32_t c, int sh) { return float((c >> sh) & 0xff); };
        probeAt(g, world, cam, {x + 0.5f, y + 0.5f, 0.5f}, clamp8(ch(s2, 16) * a + ch(c2, 16) * (1 - a)),
                clamp8(ch(s2, 8) * a + ch(c2, 8) * (1 - a)), clamp8(ch(s2, 0) * a + ch(c2, 0) * (1 - a)), 2,
                "SRCALPHA/INVSRCALPHA");
    }
}
