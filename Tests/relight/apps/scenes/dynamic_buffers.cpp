// dynamic_buffers: per-frame buffer streaming as games do it.
//   - a DYNAMIC | WRITEONLY vertex buffer (POOL_DEFAULT) refilled every frame: the first write
//     with D3DLOCK_DISCARD, the following ones appended with D3DLOCK_NOOVERWRITE, each drawn right
//     after its write (a new buffer version per write in the sidecar);
//   - a dynamic index buffer rewritten with DISCARD;
//   - DrawPrimitiveUP (strip) and DrawIndexedPrimitiveUP (u16, MinIndex > 0);
//   - D3D9 only: DrawIndexedPrimitive with a negative BaseVertexIndex (indices stored +8, base -4).
//     D3D8 has no negative base vertex; its twin draws the same quad with base 0 and plain indices.
#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"dynamic_buffers", 4, false, 0,
                              "Dynamic VB/IB with DISCARD/NOOVERWRITE, DrawPrimitiveUP, DrawIndexedPrimitiveUP, "
                              "negative BaseVertexIndex (D3D9)"};

static int g_dynVb, g_dynIb, g_staticVb, g_negIb;
static const uint32_t kQuadColors[4] = {0xffe04040, 0xff40e040, 0xff4040e0, 0xffe0e040};

static void quadVerts(VtxPC* v, float x, float y, float s, uint32_t c)
{
    v[0] = {x, y, 0.0f, c};
    v[1] = {x, y + s, 0.0f, c};
    v[2] = {x + s, y + s, 0.0f, c};
    v[3] = {x + s, y, 0.0f, c};
}

void rl::sceneInit(Gfx& g)
{
    g_dynVb = g.createVertexBuffer(16 * sizeof(VtxPC), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_DEFAULT);
    g_dynIb = g.createIndexBuffer(6 * sizeof(uint16_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                  D3DPOOL_DEFAULT);
    // Static VB for the base-vertex draw: 4 padding vertices then the quad at [4..7].
    VtxPC s[8];
    for (int i = 0; i < 4; ++i)
        s[i] = {0.0f, 0.0f, 0.0f, 0xff000000};
    quadVerts(s + 4, 0.9f, -1.7f, 0.9f, 0xffe080e0);
    g_staticVb = g.createVertexBuffer(sizeof s, D3DUSAGE_WRITEONLY, FVF_PC, D3DPOOL_MANAGED, s);
    if (!g.isD3D8()) {
        // Indices stored with +8: with BaseVertexIndex = -4 they address vertices 4..7.
        const uint16_t idx[6] = {8, 9, 10, 8, 10, 11};
        g_negIb = g.createIndexBuffer(sizeof idx, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx);
    } else {
        const uint16_t idx[6] = {4, 5, 6, 4, 6, 7};
        g_negIb = g.createIndexBuffer(sizeof idx, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, idx);
    }
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    Camera cam = setCamera(g, {0.0f, 0.0f, -4.5f}, {0.0f, 0.0f, 0.0f});
    Mat4 world = identity();
    g.setTransform(D3DTS_WORLD, world);
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
    g.setFVF(FVF_PC);

    // Dynamic VB: 4 quads per frame, positions move with the frame.
    g.setStreamSource(0, g_dynVb, 0, sizeof(VtxPC));
    const uint16_t qidx[6] = {0, 1, 2, 0, 2, 3};
    g.writeBuffer(g_dynIb, 0, qidx, sizeof qidx, D3DLOCK_DISCARD);
    g.setIndices(g_dynIb);
    float dx = 0.05f * float(frame);
    for (int q = 0; q < 4; ++q) {
        VtxPC v[4];
        quadVerts(v, -2.3f + 1.2f * float(q) + dx, 0.6f, 0.9f, kQuadColors[q]);
        g.writeBuffer(g_dynVb, uint32_t(q * 4 * sizeof(VtxPC)), v, sizeof v, q == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
        if (q % 2 == 0)
            g.drawPrimitive(D3DPT_TRIANGLEFAN, uint32_t(q * 4), 2);
        else
            g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, q * 4, 0, 4, 0, 2);
    }

    // DrawPrimitiveUP: a strip.
    VtxPC strip[6];
    for (int i = 0; i < 6; ++i)
        strip[i] = {-2.3f + 0.3f * float(i), (i & 1) ? -0.2f : -0.7f, 0.0f, 0xff40e0e0};
    g.drawPrimitiveUP(D3DPT_TRIANGLESTRIP, 4, strip, sizeof(VtxPC));

    // DrawIndexedPrimitiveUP: vertices [2..5] used (MinIndex 2, NumVertices 4).
    VtxPC up[6];
    up[0] = up[1] = VtxPC{0.0f, 0.0f, 0.0f, 0xff000000};
    quadVerts(up + 2, 0.2f, -0.8f, 0.8f, 0xfff08020);
    const uint16_t upIdx[6] = {2, 3, 4, 2, 4, 5};
    g.drawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 2, 4, 2, upIdx, D3DFMT_INDEX16, up, sizeof(VtxPC));

    // Base-vertex draw (negative on D3D9). The UP calls unbound stream 0 and the indices.
    g.setStreamSource(0, g_staticVb, 0, sizeof(VtxPC));
    g.setIndices(g_negIb);
    if (!g.isD3D8())
        g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, -4, 8, 4, 0, 2); // MinIndex is relative to the base
    else
        g.drawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 4, 0, 2);

    if (frame == kScene.frames - 1) {
        for (int q = 0; q < 4; ++q) {
            uint32_t c = kQuadColors[q];
            probeAt(g, world, cam, {-2.3f + 1.2f * float(q) + dx + 0.45f, 1.05f, 0.0f}, uint8_t(c >> 16), uint8_t(c >> 8),
                    uint8_t(c), 1, "dynamic VB quad");
        }
        probeAt(g, world, cam, {0.6f, -0.4f, 0.0f}, 0xf0, 0x80, 0x20, 1, "DrawIndexedPrimitiveUP quad");
        probeAt(g, world, cam, {1.35f, -1.25f, 0.0f}, 0xe0, 0x80, 0xe0, 1, "base-vertex quad");
    }
}
