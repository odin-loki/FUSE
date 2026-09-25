// ff_triangle: fixed-function transform, untextured; the three geometry-hash paths (plan §4.1.1):
// non-indexed (DrawPrimitive list, strip, fan), indexed u16 and indexed u32 (DrawIndexedPrimitive
// with non-zero BaseVertexIndex / MinIndex / StartIndex).
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"ff_triangle", 3, false, 0,
                              "FF transform; DrawPrimitive list/strip/fan; indexed u16 and u32 with base vertex, "
                              "min index and start index"};

static int g_vb, g_ib16, g_ib32;
static Camera g_cam;

// Four flat-coloured shapes. Each has a single vertex colour so the probes are exact.
static const uint32_t kRed = 0xffe01010, kGreen = 0xff10c020, kBlue = 0xff2040f0, kYellow = 0xfff0e020,
                      kCyan = 0xff10d0d0, kMagenta = 0xffd020c0;

void rl::sceneInit(Gfx& g)
{
    std::vector<VtxPC> v;
    // [0..2] list triangle (red), left top.
    v.push_back({-2.4f, 0.4f, 0.0f, kRed});
    v.push_back({-1.4f, 1.6f, 0.0f, kRed});
    v.push_back({-0.6f, 0.4f, 0.0f, kRed});
    // [3..6] strip quad (green), right top.
    v.push_back({0.6f, 0.4f, 0.0f, kGreen});
    v.push_back({0.6f, 1.6f, 0.0f, kGreen});
    v.push_back({2.2f, 0.4f, 0.0f, kGreen});
    v.push_back({2.2f, 1.6f, 0.0f, kGreen});
    // [7..12] fan pentagon (yellow), centre top: centre + 5 rim vertices, 4 triangles.
    v.push_back({0.0f, 1.0f, 0.5f, kYellow});
    for (int i = 0; i < 5; ++i) {
        float a = float(i) * 2.0f * kPi / 4.0f; // 4 wedges of 90 degrees; the 5th vertex closes it
        v.push_back({0.45f * sinf(a), 1.0f + 0.45f * cosf(a), 0.5f, kYellow});
    }
    // [13..15] padding; the u16 draw has BaseVertexIndex 14, so its local indices 2..5 are [16..19].
    v.push_back({0, 0, 0, 0xff000000});
    v.push_back({0, 0, 0, 0xff000000});
    v.push_back({0, 0, 0, 0xff000000});
    // [16..19] indexed quad (blue, u16).
    v.push_back({-2.2f, -1.6f, 0.0f, kBlue});
    v.push_back({-2.2f, -0.4f, 0.0f, kBlue});
    v.push_back({-0.6f, -0.4f, 0.0f, kBlue});
    v.push_back({-0.6f, -1.6f, 0.0f, kBlue});
    // [20..23] indexed quad (cyan, u32).
    v.push_back({0.6f, -1.6f, 0.0f, kCyan});
    v.push_back({0.6f, -0.4f, 0.0f, kCyan});
    v.push_back({2.2f, -0.4f, 0.0f, kCyan});
    v.push_back({2.2f, -1.6f, 0.0f, kCyan});
    // [24..26] a second u32 triangle (magenta) addressed through indices far from zero.
    v.push_back({-0.3f, -0.6f, 0.2f, kMagenta});
    v.push_back({0.0f, -0.1f, 0.2f, kMagenta});
    v.push_back({0.3f, -0.6f, 0.2f, kMagenta});

    uint32_t size = uint32_t(v.size() * sizeof(VtxPC));
    g_vb = g.createVertexBuffer(size, D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED, v.data());

    // u16: 2 unused leading indices (StartIndex = 2); quad uses local vertices 2..5 (MinIndex = 2).
    const uint16_t i16[] = {0, 0, 2, 3, 4, 2, 4, 5};
    g_ib16 = g.createIndexBuffer(sizeof i16, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, i16);
    // u32: quad 20..23, then the triangle 24..26.
    const uint32_t i32[] = {20, 21, 22, 20, 22, 23, 24, 25, 26};
    g_ib32 = g.createIndexBuffer(sizeof i32, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, i32);
}

void rl::sceneFrame(Gfx& g, int frame)
{
    (void)frame;
    baseState(g);
    g_cam = setCamera(g, {0.0f, 0.0f, -4.5f}, {0.0f, 0.0f, 0.0f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff203040, 1.0f, 0);
    g.setFVF(FVF_PC);
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPC));
    g.drawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    g.drawPrimitive(D3DPT_TRIANGLESTRIP, 3, 2);
    g.drawPrimitive(D3DPT_TRIANGLEFAN, 7, 4);
    g.setIndices(g_ib16);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 14, 2, 4, 2, 2);
    g.setIndices(g_ib32);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 20, 4, 0, 2);
    g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 24, 3, 6, 1);

    if (frame == kScene.frames - 1) {
        g.probe(2, 2, 0x20, 0x30, 0x40, 0, "clear colour");
        probeAt(g, world, g_cam, {-1.47f, 0.8f, 0.0f}, 0xe0, 0x10, 0x10, 1, "list triangle");
        probeAt(g, world, g_cam, {1.4f, 1.0f, 0.0f}, 0x10, 0xc0, 0x20, 1, "strip quad");
        probeAt(g, world, g_cam, {0.0f, 1.0f, 0.5f}, 0xf0, 0xe0, 0x20, 1, "fan");
        probeAt(g, world, g_cam, {-1.4f, -1.0f, 0.0f}, 0x20, 0x40, 0xf0, 1, "indexed u16 quad");
        probeAt(g, world, g_cam, {1.4f, -1.2f, 0.0f}, 0x10, 0xd0, 0xd0, 1, "indexed u32 quad");
        probeAt(g, world, g_cam, {0.0f, -0.45f, 0.2f}, 0xd0, 0x20, 0xc0, 1, "indexed u32 triangle");
    }
}
