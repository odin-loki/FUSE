// ff_lit: fixed-function lighting. Directional, point (range + attenuation) and spot (theta / phi /
// falloff) lights plus a set-but-disabled light; material colour sources (material, COLOR1,
// COLOR2), specular with LOCALVIEWER, NORMALIZENORMALS under a scaled world matrix, global
// AMBIENT, and an emissive-only object whose colour follows from the inputs alone (probe).
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_lit", 3, false, 0,
                              "FF lighting: directional/point/spot + disabled light, material colour sources, "
                              "specular, LOCALVIEWER, NORMALIZENORMALS, ambient, emissive"};

static int g_vb, g_ib;
static uint32_t g_indexCount, g_vertexCount;

// D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR
struct VtxL {
    float x, y, z, nx, ny, nz;
    uint32_t diffuse, specular;
};
static const uint32_t FVF_L = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR;

void rl::sceneInit(Gfx& g)
{
    // UV sphere, radius 0.5, 12 rings x 16 segments; vertex colours vary with latitude.
    const int rings = 12, segs = 16;
    std::vector<VtxL> v;
    for (int r = 0; r <= rings; ++r) {
        float th = kPi * float(r) / float(rings);
        for (int s = 0; s <= segs; ++s) {
            float ph = 2.0f * kPi * float(s) / float(segs);
            float nx = sinf(th) * cosf(ph), ny = cosf(th), nz = sinf(th) * sinf(ph);
            uint32_t d = argb(0xff, uint32_t(40 + r * 16), 0xc0, uint32_t(230 - r * 16));
            v.push_back({0.5f * nx, 0.5f * ny, 0.5f * nz, nx, ny, nz, d, 0xff404040});
        }
    }
    std::vector<uint16_t> idx;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            uint16_t a = uint16_t(r * (segs + 1) + s), b = uint16_t(a + segs + 1);
            uint16_t q[6] = {a, b, uint16_t(b + 1), a, uint16_t(b + 1), uint16_t(a + 1)};
            idx.insert(idx.end(), q, q + 6);
        }
    g_vertexCount = uint32_t(v.size());
    g_indexCount = uint32_t(idx.size());
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxL)), D3DUSAGE_WRITEONLY, FVF_L, D3DPOOL_MANAGED, v.data());
    g_ib = g.createIndexBuffer(uint32_t(idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx.data());
}

static Light makeLight(uint32_t type)
{
    Light l{};
    l.Type = type;
    return l;
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 0.6f, -4.0f}, {0.0f, 0.0f, 0.0f});
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g.setRenderState(D3DRS_LIGHTING, TRUE);
    g.setRenderState(D3DRS_AMBIENT, 0xff202020);
    g.setRenderState(D3DRS_SPECULARENABLE, TRUE);
    g.setRenderState(D3DRS_LOCALVIEWER, TRUE);
    g.setRenderState(D3DRS_NORMALIZENORMALS, TRUE);
    g.setRenderState(D3DRS_COLORVERTEX, TRUE);

    Light dir = makeLight(D3DLIGHT_DIRECTIONAL);
    dir.Diffuse = color(0.8f, 0.8f, 0.75f);
    dir.Specular = color(1.0f, 1.0f, 1.0f);
    dir.Ambient = color(0.05f, 0.05f, 0.05f);
    dir.Direction = normalize(Vec3{0.4f, -0.6f, 0.7f});
    g.setLight(0, dir);
    g.lightEnable(0, true);

    Light pt = makeLight(D3DLIGHT_POINT);
    pt.Diffuse = color(1.0f, 0.3f, 0.2f);
    pt.Specular = color(1.0f, 0.5f, 0.5f);
    pt.Position = Vec3{-1.6f, 1.0f, -1.0f};
    pt.Range = 6.0f;
    pt.Attenuation0 = 0.2f;
    pt.Attenuation1 = 0.3f;
    pt.Attenuation2 = 0.05f;
    g.setLight(1, pt);
    g.lightEnable(1, true);

    Light spot = makeLight(D3DLIGHT_SPOT);
    spot.Diffuse = color(0.2f, 0.4f, 1.0f);
    spot.Specular = color(0.5f, 0.5f, 1.0f);
    spot.Position = Vec3{1.5f, 2.0f, -1.5f};
    spot.Direction = normalize(Vec3{-0.3f, -1.0f, 0.6f});
    spot.Range = 10.0f;
    spot.Attenuation0 = 1.0f;
    spot.Theta = 0.35f;
    spot.Phi = 0.9f;
    spot.Falloff = 1.0f;
    g.setLight(2, spot);
    g.lightEnable(2, true);

    Light off = makeLight(D3DLIGHT_POINT); // set but disabled: must not contribute
    off.Diffuse = color(0.0f, 1.0f, 0.0f);
    off.Position = Vec3{0.0f, 0.0f, -2.0f};
    off.Range = 100.0f;
    off.Attenuation0 = 1.0f;
    g.setLight(3, off);
    g.lightEnable(3, false);

    g.setFVF(FVF_L);
    g.setStreamSource(0, g_vb, 0, sizeof(VtxL));
    g.setIndices(g_ib);

    Material m{};
    m.Diffuse = color(0.9f, 0.9f, 0.9f);
    m.Ambient = color(1.0f, 1.0f, 1.0f);
    m.Specular = color(0.6f, 0.6f, 0.6f);
    m.Power = 24.0f;
    g.setMaterial(m);

    struct Obj {
        float x, y, scale;
        uint32_t diffSrc, ambSrc, specSrc, emisSrc;
        const char* tag;
    };
    const Obj objs[4] = {
        {-1.25f, 0.55f, 1.0f, D3DMCS_MATERIAL, D3DMCS_MATERIAL, D3DMCS_MATERIAL, D3DMCS_MATERIAL, "world"},
        {0.0f, 0.55f, 1.4f, D3DMCS_COLOR1, D3DMCS_MATERIAL, D3DMCS_COLOR2, D3DMCS_MATERIAL, "world"},
        {1.25f, 0.55f, 1.0f, D3DMCS_MATERIAL, D3DMCS_COLOR1, D3DMCS_MATERIAL, D3DMCS_MATERIAL, "world"},
        {-0.6f, -0.75f, 0.8f, D3DMCS_MATERIAL, D3DMCS_MATERIAL, D3DMCS_MATERIAL, D3DMCS_COLOR1, "world"},
    };
    for (const Obj& o : objs) {
        g.setRenderState(D3DRS_DIFFUSEMATERIALSOURCE, o.diffSrc);
        g.setRenderState(D3DRS_AMBIENTMATERIALSOURCE, o.ambSrc);
        g.setRenderState(D3DRS_SPECULARMATERIALSOURCE, o.specSrc);
        g.setRenderState(D3DRS_EMISSIVEMATERIALSOURCE, o.emisSrc);
        Mat4 world = mul(mul(scaling(o.scale, o.scale, o.scale), rotationY(0.3f * float(frame))), translation(o.x, o.y, 0.0f));
        g.setTransform(D3DTS_WORLD, world);
        g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, g_vertexCount, 0, g_indexCount / 3);
    }

    // Emissive-only sphere: black diffuse / ambient / specular material, no ambient light
    // contribution, emissive (0.25, 0.5, 0.75) -> exactly (64, 128, 191) +- rounding.
    g.setRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    g.setRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    g.setRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    g.setRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    g.setRenderState(D3DRS_SPECULARENABLE, FALSE);
    Material em{};
    em.Emissive = color(0.25f, 0.5f, 0.75f);
    g.setMaterial(em);
    Mat4 world = translation(0.75f, -0.75f, 0.0f);
    g.setTransform(D3DTS_WORLD, world);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, g_vertexCount, 0, g_indexCount / 3);

    if (frame == kScene.frames - 1) {
        g.probe(1, 1, 0, 0, 0, 0, "clear colour");
        probeAt(g, world, cam, {0.0f, 0.0f, -0.5f}, 64, 128, 191, 2, "emissive-only sphere");
    }
}
