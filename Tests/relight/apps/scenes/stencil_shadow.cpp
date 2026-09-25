// stencil_shadow: Doom-3-style stencil shadow volume (Remix ignores these draws by heuristic).
// A floor and a floating cube; the cube's shadow volume (its bottom face extruded along the light
// direction through the floor) is drawn with colour writes off (COLORWRITEENABLE = 0), Z writes
// off and z-pass stencil increment / decrement in two culled passes (tag "shadow_volume"); then a
// full-screen POSITIONT quad darkens stencil != 0 pixels by half (tag "shadow_darken").
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"stencil_shadow", 3, false, 0,
                              "Stencil shadow volume (COLORWRITEENABLE 0, two-pass incr/decr) and a POSITIONT "
                              "stencil-tested darkening quad"};

static int g_floorVb, g_cubeVb, g_volVb, g_ib;
static const uint32_t kFloor = 0xffc0c0b0, kCube = 0xff3070d0;
static const Vec3 kLight{0.6f, -1.0f, 0.3f}; // direction the light travels (not normalized)

// Cube: centre (0, 1.2, 0), half-size 0.4; floor y = 0. Volume = bottom face (y = 0.8) extruded
// along the light by t = 3 (to y = -2.2), through the floor.
static const float kCy = 1.2f, kH = 0.4f, kT = 3.0f;

void rl::sceneInit(Gfx& g)
{
    std::vector<VtxPC> floorV = {{-4, 0, -4, kFloor}, {-4, 0, 4, kFloor}, {4, 0, 4, kFloor}, {4, 0, -4, kFloor}};
    g_floorVb = g.createVertexBuffer(4 * sizeof(VtxPC), D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED, floorV.data());

    std::vector<VtxPNT> cube;
    std::vector<uint16_t> idx;
    makeCube(cube, idx, kH);
    std::vector<VtxPC> cv;
    for (const VtxPNT& p : cube)
        cv.push_back({p.x, p.y + kCy, p.z, kCube});
    g_cubeVb = g.createVertexBuffer(uint32_t(cv.size() * sizeof(VtxPC)), D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED,
                                    cv.data());
    g_ib = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());

    // Closed prism: top = bottom face of the cube, bottom = top translated by kLight * kT.
    const float xs[4] = {-kH, -kH, kH, kH}, zs[4] = {-kH, kH, kH, -kH};
    Vec3 top[4], bot[4];
    for (int i = 0; i < 4; ++i) {
        top[i] = Vec3{xs[i], kCy - kH, zs[i]};
        bot[i] = Vec3{xs[i] + kLight.x * kT, kCy - kH + kLight.y * kT, zs[i] + kLight.z * kT};
    }
    std::vector<VtxPC> vol;
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c) {
        vol.push_back({a.x, a.y, a.z, 0xffffffff});
        vol.push_back({b.x, b.y, b.z, 0xffffffff});
        vol.push_back({c.x, c.y, c.z, 0xffffffff});
    };
    // Caps (top winding clockwise seen from above = front-facing from above in D3D's CW-front convention).
    tri(top[0], top[1], top[2]);
    tri(top[0], top[2], top[3]);
    tri(bot[0], bot[2], bot[1]);
    tri(bot[0], bot[3], bot[2]);
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        tri(top[i], bot[i], bot[j]);
        tri(top[i], bot[j], top[j]);
    }
    g_volVb = g.createVertexBuffer(uint32_t(vol.size() * sizeof(VtxPC)), D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED,
                                   vol.data());
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {-1.0f, 4.0f, -5.0f}, {0.6f, 0.2f, 0.4f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff101820, 1.0f, 0);
    g.setFVF(FVF_PC);
    g.setStreamSource(0, g_floorVb, 0, sizeof(VtxPC));
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    g.setStreamSource(0, g_cubeVb, 0, sizeof(VtxPC));
    g.setIndices(g_ib);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);

    // Shadow volume: z-pass, front faces increment, back faces decrement.
    g.tag("shadow_volume");
    g.setRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g.setRenderState(D3DRS_COLORWRITEENABLE, 0);
    g.setRenderState(D3DRS_STENCILENABLE, TRUE);
    g.setRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    g.setRenderState(D3DRS_STENCILREF, 0);
    g.setRenderState(D3DRS_STENCILMASK, 0xffffffff);
    g.setRenderState(D3DRS_STENCILWRITEMASK, 0xffffffff);
    g.setRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    g.setRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    g.setStreamSource(0, g_volVb, 0, sizeof(VtxPC));
    g.setRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g.setRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCR);
    g.drawPrimitive(D3DPT_TRIANGLELIST, 0, 12);
    g.setRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    g.setRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_DECR);
    g.drawPrimitive(D3DPT_TRIANGLELIST, 0, 12);
    g.setRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g.setRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    g.setRenderState(D3DRS_ZWRITEENABLE, TRUE);

    // Darken where stencil != 0 with a full-screen POSITIONT quad (alpha 0x80 black).
    g.tag("shadow_darken");
    g.setRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
    g.setRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    g.setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g.setRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g.setRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g.setRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g.setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g.setFVF(FVF_TL);
    const float W = float(kWidth), H = float(kHeight);
    VtxTL quad[4] = {{0, 0, 0, 1, 0x80000000, 0, 0}, {W, 0, 0, 1, 0x80000000, 0, 0}, {W, H, 0, 1, 0x80000000, 0, 0},
                     {0, H, 0, 1, 0x80000000, 0, 0}};
    g.drawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, quad, sizeof(VtxTL));
    g.setRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g.setRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g.setRenderState(D3DRS_STENCILENABLE, FALSE);
    g.tag("world");

    if (frame == kScene.frames - 1) {
        // Where the light ray through the cube's bottom centre meets the floor: in shadow.
        float t = (kCy - kH) / -kLight.y;
        Vec3 hit{kLight.x * t, 0.0f, kLight.z * t};
        const float a = 128.0f / 255.0f;
        auto half = [&](uint32_t sh) { return uint8_t(floorf(float((kFloor >> sh) & 0xff) * (1.0f - a) + 0.5f)); };
        probeAt(g, world, cam, hit, half(16), half(8), half(0), 2, "floor in shadow");
        probeAt(g, world, cam, {-2.0f, 0.0f, 1.5f}, 0xc0, 0xc0, 0xb0, 0, "floor lit");
        probeAt(g, world, cam, {0.0f, kCy + kH, 0.0f}, 0x30, 0x70, 0xd0, 0, "cube top");
    }
}
