// texture_formats: one 8x8 texture per common D3DFORMAT (colour, luminance, alpha, packed 16-bit,
// 10-bit, 16-bit UNORM, DXT1..5, float), uploaded with LockRect and drawn as a pixel-aligned tile
// (ortho projection, point sampling). Each texture has four solid quadrants (red, green, blue,
// white; alpha 255 / 192 / 128 / 64), so the texture hash inputs are known. Bump formats (V8U8,
// L6V5U5, X8L8V8U8, Q8W8V8U8, V16U16, A2W10V10U10, Q16W16V16U16) are created and uploaded (they
// are hashed like any texture) but not drawn: fixed-function colour from signed formats is
// implementation-defined. Also UpdateTexture (SYSTEMMEM -> DEFAULT, 4 levels) and UpdateSurface
// (D3D8: CopyRects). Formats the device rejects (CheckDeviceFormat) are skipped and listed in
// annotations.skipped_formats, so that list depends on the runtime.
#include <string.h>

#include "rl_scene.h"

using namespace rl;

const SceneInfo rl::kScene = {"texture_formats", 2, false, 0,
                              "Every common D3DFORMAT incl. DXT1-5, packed, float and bump formats; UpdateTexture "
                              "and UpdateSurface/CopyRects"};

struct Fmt {
    uint32_t fmt;
    const char* name;
    bool draw;
};
static const Fmt kFormats[] = {
    {D3DFMT_A8R8G8B8, "A8R8G8B8", true}, {D3DFMT_X8R8G8B8, "X8R8G8B8", true}, {D3DFMT_A8B8G8R8, "A8B8G8R8", true},
    {D3DFMT_X8B8G8R8, "X8B8G8R8", true}, {D3DFMT_R5G6B5, "R5G6B5", true}, {D3DFMT_X1R5G5B5, "X1R5G5B5", true},
    {D3DFMT_A1R5G5B5, "A1R5G5B5", true}, {D3DFMT_A4R4G4B4, "A4R4G4B4", true}, {D3DFMT_X4R4G4B4, "X4R4G4B4", true},
    {D3DFMT_R3G3B2, "R3G3B2", true}, {D3DFMT_A8R3G3B2, "A8R3G3B2", true}, {D3DFMT_L8, "L8", true},
    {D3DFMT_A8L8, "A8L8", true}, {D3DFMT_A4L4, "A4L4", true}, {D3DFMT_A8, "A8", true}, {D3DFMT_L16, "L16", true},
    {D3DFMT_DXT1, "DXT1", true}, {D3DFMT_DXT2, "DXT2", true}, {D3DFMT_DXT3, "DXT3", true}, {D3DFMT_DXT4, "DXT4", true},
    {D3DFMT_DXT5, "DXT5", true}, {D3DFMT_G16R16, "G16R16", true}, {D3DFMT_A2B10G10R10, "A2B10G10R10", true},
    {D3DFMT_A2R10G10B10, "A2R10G10B10", true}, {D3DFMT_A16B16G16R16, "A16B16G16R16", true},
    {D3DFMT_R16F, "R16F", true}, {D3DFMT_G16R16F, "G16R16F", true}, {D3DFMT_A16B16G16R16F, "A16B16G16R16F", true},
    {D3DFMT_R32F, "R32F", true}, {D3DFMT_G32R32F, "G32R32F", true}, {D3DFMT_A32B32G32R32F, "A32B32G32R32F", true},
    {D3DFMT_V8U8, "V8U8", false}, {D3DFMT_L6V5U5, "L6V5U5", false}, {D3DFMT_X8L8V8U8, "X8L8V8U8", false},
    {D3DFMT_Q8W8V8U8, "Q8W8V8U8", false}, {D3DFMT_V16U16, "V16U16", false}, {D3DFMT_A2W10V10U10, "A2W10V10U10", false},
    {D3DFMT_Q16W16V16U16, "Q16W16V16U16", false},
};

struct Rgba {
    uint32_t r, g, b, a;
};
// Quadrants: 0 top-left red, 1 top-right green, 2 bottom-left blue, 3 bottom-right white.
static const Rgba kQuad[4] = {{255, 0, 0, 255}, {0, 255, 0, 192}, {0, 0, 255, 128}, {255, 255, 255, 64}};

static uint16_t toHalf(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (f == 0.0f)
        return uint16_t(sign);
    if (e <= 0)
        return uint16_t(sign); // not reached for the values used here (0 or >= 1/255)
    uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
    uint32_t rest = m & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u)))
        ++h;
    return uint16_t(h);
}

template <class T>
static void put(std::vector<uint8_t>& o, T v)
{
    uint8_t b[sizeof(T)];
    memcpy(b, &v, sizeof(T));
    o.insert(o.end(), b, b + sizeof(T));
}

static uint16_t rgb565(const Rgba& c) { return uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3)); }

// One texel of `fmt` (non-block formats).
static void encodeTexel(uint32_t fmt, const Rgba& c, std::vector<uint8_t>& o)
{
    auto u10 = [](uint32_t v) { return (v * 1023u + 127u) / 255u; };
    auto u16 = [](uint32_t v) { return v * 257u; };
    auto sn8 = [](uint32_t v) { return uint8_t(int8_t(int(v) / 2 - 64)); };
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: put<uint32_t>(o, (c.a << 24) | (c.r << 16) | (c.g << 8) | c.b); break;
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: put<uint32_t>(o, (c.a << 24) | (c.b << 16) | (c.g << 8) | c.r); break;
    case D3DFMT_R5G6B5: put<uint16_t>(o, rgb565(c)); break;
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        put<uint16_t>(o, uint16_t(((c.a >= 128) << 15) | ((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3)));
        break;
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        put<uint16_t>(o, uint16_t(((c.a >> 4) << 12) | ((c.r >> 4) << 8) | ((c.g >> 4) << 4) | (c.b >> 4)));
        break;
    case D3DFMT_R3G3B2: put<uint8_t>(o, uint8_t(((c.r >> 5) << 5) | ((c.g >> 5) << 2) | (c.b >> 6))); break;
    case D3DFMT_A8R3G3B2: put<uint16_t>(o, uint16_t((c.a << 8) | ((c.r >> 5) << 5) | ((c.g >> 5) << 2) | (c.b >> 6))); break;
    case D3DFMT_L8: put<uint8_t>(o, uint8_t((c.r + c.g + c.b) / 3)); break;
    case D3DFMT_A8L8: put<uint16_t>(o, uint16_t((c.a << 8) | ((c.r + c.g + c.b) / 3))); break;
    case D3DFMT_A4L4: put<uint8_t>(o, uint8_t(((c.a >> 4) << 4) | (((c.r + c.g + c.b) / 3) >> 4))); break;
    case D3DFMT_A8: put<uint8_t>(o, uint8_t(c.a)); break;
    case D3DFMT_L16: put<uint16_t>(o, uint16_t(u16((c.r + c.g + c.b) / 3))); break;
    case D3DFMT_G16R16: put<uint32_t>(o, (u16(c.g) << 16) | u16(c.r)); break;
    case D3DFMT_A2B10G10R10: put<uint32_t>(o, ((c.a >> 6) << 30) | (u10(c.b) << 20) | (u10(c.g) << 10) | u10(c.r)); break;
    case D3DFMT_A2R10G10B10: put<uint32_t>(o, ((c.a >> 6) << 30) | (u10(c.r) << 20) | (u10(c.g) << 10) | u10(c.b)); break;
    case D3DFMT_A16B16G16R16:
        put<uint16_t>(o, uint16_t(u16(c.r))); put<uint16_t>(o, uint16_t(u16(c.g)));
        put<uint16_t>(o, uint16_t(u16(c.b))); put<uint16_t>(o, uint16_t(u16(c.a)));
        break;
    case D3DFMT_R16F: put<uint16_t>(o, toHalf(float(c.r) / 255.0f)); break;
    case D3DFMT_G16R16F: put<uint16_t>(o, toHalf(float(c.r) / 255.0f)); put<uint16_t>(o, toHalf(float(c.g) / 255.0f)); break;
    case D3DFMT_A16B16G16R16F:
        put<uint16_t>(o, toHalf(float(c.r) / 255.0f)); put<uint16_t>(o, toHalf(float(c.g) / 255.0f));
        put<uint16_t>(o, toHalf(float(c.b) / 255.0f)); put<uint16_t>(o, toHalf(float(c.a) / 255.0f));
        break;
    case D3DFMT_R32F: put<float>(o, float(c.r) / 255.0f); break;
    case D3DFMT_G32R32F: put<float>(o, float(c.r) / 255.0f); put<float>(o, float(c.g) / 255.0f); break;
    case D3DFMT_A32B32G32R32F:
        put<float>(o, float(c.r) / 255.0f); put<float>(o, float(c.g) / 255.0f);
        put<float>(o, float(c.b) / 255.0f); put<float>(o, float(c.a) / 255.0f);
        break;
    case D3DFMT_V8U8: put<uint8_t>(o, sn8(c.r)); put<uint8_t>(o, sn8(c.g)); break;
    case D3DFMT_L6V5U5: put<uint16_t>(o, uint16_t(((c.b >> 2) << 10) | ((c.g >> 3) << 5) | (c.r >> 3))); break;
    case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8:
        put<uint8_t>(o, sn8(c.r)); put<uint8_t>(o, sn8(c.g)); put<uint8_t>(o, sn8(c.b)); put<uint8_t>(o, sn8(c.a));
        break;
    case D3DFMT_V16U16: put<uint16_t>(o, uint16_t(c.r * 64)); put<uint16_t>(o, uint16_t(c.g * 64)); break;
    case D3DFMT_A2W10V10U10: put<uint32_t>(o, ((c.a >> 6) << 30) | ((c.b << 1) << 20) | ((c.g << 1) << 10) | (c.r << 1)); break;
    case D3DFMT_Q16W16V16U16:
        put<uint16_t>(o, uint16_t(c.r * 64)); put<uint16_t>(o, uint16_t(c.g * 64));
        put<uint16_t>(o, uint16_t(c.b * 64)); put<uint16_t>(o, uint16_t(c.a * 64));
        break;
    }
}

// One solid 4x4 block (DXT1 colour block: colour0 = c, colour1 = 0, all indices 0).
static void dxtColorBlock(const Rgba& c, std::vector<uint8_t>& o)
{
    put<uint16_t>(o, rgb565(c));
    put<uint16_t>(o, 0);
    put<uint32_t>(o, 0);
}

// Level data (tightly packed) for an 8x8 quadrant texture of `fmt`.
static std::vector<uint8_t> makeLevel(uint32_t fmt, uint32_t size)
{
    std::vector<uint8_t> o;
    uint32_t half = size / 2 ? size / 2 : 1;
    if (fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5) {
        uint32_t blocks = (size + 3) / 4;
        for (uint32_t by = 0; by < blocks; ++by)
            for (uint32_t bx = 0; bx < blocks; ++bx) {
                const Rgba& c = kQuad[(by * 4 >= half ? 2 : 0) + (bx * 4 >= half ? 1 : 0)];
                if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3) {
                    uint64_t alpha = 0;
                    for (int i = 0; i < 16; ++i)
                        alpha |= uint64_t(c.a >> 4) << (4 * i);
                    put<uint64_t>(o, alpha);
                } else if (fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5) {
                    put<uint8_t>(o, uint8_t(c.a));
                    put<uint8_t>(o, 0);
                    for (int i = 0; i < 6; ++i)
                        put<uint8_t>(o, 0);
                }
                dxtColorBlock(c, o);
            }
        return o;
    }
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
            encodeTexel(fmt, kQuad[(y >= half ? 2 : 0) + (x >= half ? 1 : 0)], o);
    return o;
}

struct Tile {
    int tex;
    const char* name;
};
static std::vector<Tile> g_tiles;
static int g_vb;

void rl::sceneInit(Gfx& g)
{
    JV skipped = JV::arr(), created = JV::arr();
    for (const Fmt& f : kFormats) {
        if (!g.formatSupported(f.fmt, 0, D3DRTYPE_TEXTURE)) {
            skipped.push(JV::str(f.name));
            continue;
        }
        int t = g.createTexture(8, 8, 1, 0, f.fmt, D3DPOOL_MANAGED);
        g.uploadTexture(t, 0, makeLevel(f.fmt, 8).data());
        created.push(JV::str(f.name));
        if (f.draw)
            g_tiles.push_back({t, f.name});
    }
    // UpdateTexture: SYSTEMMEM A8R8G8B8 with 4 levels -> DEFAULT.
    int src = g.createTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM);
    for (uint32_t l = 0; l < 4; ++l)
        g.uploadTexture(src, l, makeLevel(D3DFMT_A8R8G8B8, 8 >> l).data());
    int dst = g.createTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT);
    g.updateTexture(src, dst);
    g_tiles.push_back({dst, "UpdateTexture"});
    // UpdateSurface / CopyRects: SYSTEMMEM R5G6B5 level 0 -> DEFAULT level 0.
    int src2 = g.createTexture(8, 8, 1, 0, D3DFMT_R5G6B5, D3DPOOL_SYSTEMMEM);
    g.uploadTexture(src2, 0, makeLevel(D3DFMT_R5G6B5, 8).data());
    int dst2 = g.createTexture(8, 8, 1, 0, D3DFMT_R5G6B5, D3DPOOL_DEFAULT);
    g.updateSurface(src2, 0, dst2, 0);
    g_tiles.push_back({dst2, g.isD3D8() ? "CopyRects" : "UpdateSurface"});
    g.annotate("skipped_formats", skipped);
    g.annotate("created_formats", created);

    // Tiles: 14x14 pixels on a 16-pixel grid, 8 per row (pixel-space ortho).
    std::vector<VtxPCT> v;
    JV tiles = JV::arr();
    for (size_t i = 0; i < g_tiles.size(); ++i) {
        float x = float((i % 8) * 16 + 1), y = float((i / 8) * 16 + 1);
        v.push_back({x, y + 14.0f, 0.5f, 0xffffffff, 0.0f, 1.0f});
        v.push_back({x, y, 0.5f, 0xffffffff, 0.0f, 0.0f});
        v.push_back({x + 14.0f, y, 0.5f, 0xffffffff, 1.0f, 0.0f});
        v.push_back({x + 14.0f, y + 14.0f, 0.5f, 0xffffffff, 1.0f, 1.0f});
        JV t = JV::obj();
        t.set("tile", JV::integer((long long)i));
        t.set("texture", JV::integer(g_tiles[i].tex));
        t.set("name", JV::str(g_tiles[i].name));
        tiles.push(t);
    }
    g.annotate("tiles", tiles);
    g_vb = g.createVertexBuffer(uint32_t(v.size() * sizeof(VtxPCT)), D3DUSAGE_WRITEONLY, FVF_PCT, D3DPOOL_MANAGED, v.data());
}

void rl::sceneFrame(Gfx& g, int frame)
{
    baseState(g);
    g.setRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g.setTransform(D3DTS_WORLD, identity());
    g.setTransform(D3DTS_VIEW, identity());
    g.setTransform(D3DTS_PROJECTION, orthoOffCenterLH(0.0f, float(kWidth), float(kHeight), 0.0f, 0.0f, 1.0f));
    g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff404040, 1.0f, 0);
    g.setFVF(FVF_PCT);
    g.setStreamSource(0, g_vb, 0, sizeof(VtxPCT));
    g.setTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g.setTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g.setTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g.setSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g.setSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g.setSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g.setSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    for (size_t i = 0; i < g_tiles.size(); ++i) {
        g.setTexture(0, g_tiles[i].tex);
        g.drawPrimitive(D3DPT_TRIANGLEFAN, uint32_t(i * 4), 2);
    }
    g.setTexture(0, NONE);

    if (frame == kScene.frames - 1) {
        // Exact probes for formats whose quadrant colours (0 / 255 channels) survive encoding.
        static const char* exact[] = {"A8R8G8B8", "X8R8G8B8", "R5G6B5", "X1R5G5B5", "A4R4G4B4", "DXT1", "DXT3", "DXT5",
                                      "UpdateTexture", "UpdateSurface", "CopyRects"};
        for (size_t i = 0; i < g_tiles.size(); ++i)
            for (const char* e : exact)
                if (!strcmp(g_tiles[i].name, e)) {
                    int x = int((i % 8) * 16 + 1), y = int((i / 8) * 16 + 1);
                    for (int q = 0; q < 4; ++q)
                        g.probe(x + 3 + (q & 1) * 7, y + 3 + (q >> 1) * 7, uint8_t(kQuad[q].r), uint8_t(kQuad[q].g),
                                uint8_t(kQuad[q].b), 0, g_tiles[i].name);
                }
    }
}
