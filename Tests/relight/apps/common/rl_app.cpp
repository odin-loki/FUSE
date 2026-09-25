// FUSE Relight test-app kit (RL-0.4): rl::Gfx recording wrapper, sidecar writer and main().
// See rl_app.h for the contract and schema/rl_app_sidecar.schema.json for the output.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9types.h>

#include <array>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rl_app.h"
#include "rl_dump.h"

namespace rl {

// ---- enum names ------------------------------------------------------------------------------------
struct NameEntry {
    uint32_t value;
    const char* name;
};
#define RL_N(prefix, x) {prefix##x, #x}

static const NameEntry kRenderStates[] = {
    RL_N(D3DRS_, ZENABLE), RL_N(D3DRS_, FILLMODE), RL_N(D3DRS_, SHADEMODE), RL_N(D3DRS_, ZWRITEENABLE),
    RL_N(D3DRS_, ALPHATESTENABLE), RL_N(D3DRS_, LASTPIXEL), RL_N(D3DRS_, SRCBLEND), RL_N(D3DRS_, DESTBLEND),
    RL_N(D3DRS_, CULLMODE), RL_N(D3DRS_, ZFUNC), RL_N(D3DRS_, ALPHAREF), RL_N(D3DRS_, ALPHAFUNC),
    RL_N(D3DRS_, DITHERENABLE), RL_N(D3DRS_, ALPHABLENDENABLE), RL_N(D3DRS_, FOGENABLE), RL_N(D3DRS_, SPECULARENABLE),
    RL_N(D3DRS_, FOGCOLOR), RL_N(D3DRS_, FOGTABLEMODE), RL_N(D3DRS_, FOGSTART), RL_N(D3DRS_, FOGEND),
    RL_N(D3DRS_, FOGDENSITY), RL_N(D3DRS_, RANGEFOGENABLE), RL_N(D3DRS_, STENCILENABLE), RL_N(D3DRS_, STENCILFAIL),
    RL_N(D3DRS_, STENCILZFAIL), RL_N(D3DRS_, STENCILPASS), RL_N(D3DRS_, STENCILFUNC), RL_N(D3DRS_, STENCILREF),
    RL_N(D3DRS_, STENCILMASK), RL_N(D3DRS_, STENCILWRITEMASK), RL_N(D3DRS_, TEXTUREFACTOR), RL_N(D3DRS_, WRAP0),
    RL_N(D3DRS_, WRAP1), RL_N(D3DRS_, WRAP2), RL_N(D3DRS_, WRAP3), RL_N(D3DRS_, WRAP4), RL_N(D3DRS_, WRAP5),
    RL_N(D3DRS_, WRAP6), RL_N(D3DRS_, WRAP7), RL_N(D3DRS_, CLIPPING), RL_N(D3DRS_, LIGHTING), RL_N(D3DRS_, AMBIENT),
    RL_N(D3DRS_, FOGVERTEXMODE), RL_N(D3DRS_, COLORVERTEX), RL_N(D3DRS_, LOCALVIEWER), RL_N(D3DRS_, NORMALIZENORMALS),
    RL_N(D3DRS_, DIFFUSEMATERIALSOURCE), RL_N(D3DRS_, SPECULARMATERIALSOURCE), RL_N(D3DRS_, AMBIENTMATERIALSOURCE),
    RL_N(D3DRS_, EMISSIVEMATERIALSOURCE), RL_N(D3DRS_, VERTEXBLEND), RL_N(D3DRS_, CLIPPLANEENABLE),
    RL_N(D3DRS_, POINTSIZE), RL_N(D3DRS_, POINTSIZE_MIN), RL_N(D3DRS_, POINTSPRITEENABLE), RL_N(D3DRS_, POINTSCALEENABLE),
    RL_N(D3DRS_, POINTSCALE_A), RL_N(D3DRS_, POINTSCALE_B), RL_N(D3DRS_, POINTSCALE_C),
    RL_N(D3DRS_, MULTISAMPLEANTIALIAS), RL_N(D3DRS_, MULTISAMPLEMASK), RL_N(D3DRS_, PATCHEDGESTYLE),
    RL_N(D3DRS_, DEBUGMONITORTOKEN), RL_N(D3DRS_, POINTSIZE_MAX), RL_N(D3DRS_, INDEXEDVERTEXBLENDENABLE),
    RL_N(D3DRS_, COLORWRITEENABLE), RL_N(D3DRS_, TWEENFACTOR), RL_N(D3DRS_, BLENDOP), RL_N(D3DRS_, POSITIONDEGREE),
    RL_N(D3DRS_, NORMALDEGREE), RL_N(D3DRS_, SCISSORTESTENABLE), RL_N(D3DRS_, SLOPESCALEDEPTHBIAS),
    RL_N(D3DRS_, ANTIALIASEDLINEENABLE), RL_N(D3DRS_, MINTESSELLATIONLEVEL), RL_N(D3DRS_, MAXTESSELLATIONLEVEL),
    RL_N(D3DRS_, ADAPTIVETESS_X), RL_N(D3DRS_, ADAPTIVETESS_Y), RL_N(D3DRS_, ADAPTIVETESS_Z), RL_N(D3DRS_, ADAPTIVETESS_W),
    RL_N(D3DRS_, ENABLEADAPTIVETESSELLATION), RL_N(D3DRS_, TWOSIDEDSTENCILMODE), RL_N(D3DRS_, CCW_STENCILFAIL),
    RL_N(D3DRS_, CCW_STENCILZFAIL), RL_N(D3DRS_, CCW_STENCILPASS), RL_N(D3DRS_, CCW_STENCILFUNC),
    RL_N(D3DRS_, COLORWRITEENABLE1), RL_N(D3DRS_, COLORWRITEENABLE2), RL_N(D3DRS_, COLORWRITEENABLE3),
    RL_N(D3DRS_, BLENDFACTOR), RL_N(D3DRS_, SRGBWRITEENABLE), RL_N(D3DRS_, DEPTHBIAS), RL_N(D3DRS_, WRAP8),
    RL_N(D3DRS_, WRAP9), RL_N(D3DRS_, WRAP10), RL_N(D3DRS_, WRAP11), RL_N(D3DRS_, WRAP12), RL_N(D3DRS_, WRAP13),
    RL_N(D3DRS_, WRAP14), RL_N(D3DRS_, WRAP15), RL_N(D3DRS_, SEPARATEALPHABLENDENABLE), RL_N(D3DRS_, SRCBLENDALPHA),
    RL_N(D3DRS_, DESTBLENDALPHA), RL_N(D3DRS_, BLENDOPALPHA)};
// Render states whose DWORD is a float bit pattern.
static const uint32_t kFloatRenderStates[] = {D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MIN, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C, D3DRS_POINTSIZE_MAX,
    D3DRS_TWEENFACTOR, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_DEPTHBIAS, D3DRS_MINTESSELLATIONLEVEL,
    D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y, D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W};
static const NameEntry kStageStates[] = {
    RL_N(D3DTSS_, COLOROP), RL_N(D3DTSS_, COLORARG1), RL_N(D3DTSS_, COLORARG2), RL_N(D3DTSS_, ALPHAOP),
    RL_N(D3DTSS_, ALPHAARG1), RL_N(D3DTSS_, ALPHAARG2), RL_N(D3DTSS_, BUMPENVMAT00), RL_N(D3DTSS_, BUMPENVMAT01),
    RL_N(D3DTSS_, BUMPENVMAT10), RL_N(D3DTSS_, BUMPENVMAT11), RL_N(D3DTSS_, TEXCOORDINDEX),
    RL_N(D3DTSS_, BUMPENVLSCALE), RL_N(D3DTSS_, BUMPENVLOFFSET), RL_N(D3DTSS_, TEXTURETRANSFORMFLAGS),
    RL_N(D3DTSS_, COLORARG0), RL_N(D3DTSS_, ALPHAARG0), RL_N(D3DTSS_, RESULTARG), RL_N(D3DTSS_, CONSTANT)};
static const NameEntry kSamplerStates[] = {
    RL_N(D3DSAMP_, ADDRESSU), RL_N(D3DSAMP_, ADDRESSV), RL_N(D3DSAMP_, ADDRESSW), RL_N(D3DSAMP_, BORDERCOLOR),
    RL_N(D3DSAMP_, MAGFILTER), RL_N(D3DSAMP_, MINFILTER), RL_N(D3DSAMP_, MIPFILTER), RL_N(D3DSAMP_, MIPMAPLODBIAS),
    RL_N(D3DSAMP_, MAXMIPLEVEL), RL_N(D3DSAMP_, MAXANISOTROPY), RL_N(D3DSAMP_, SRGBTEXTURE),
    RL_N(D3DSAMP_, ELEMENTINDEX), RL_N(D3DSAMP_, DMAPOFFSET)};
static const NameEntry kFormats[] = {
    RL_N(D3DFMT_, UNKNOWN), RL_N(D3DFMT_, R8G8B8), RL_N(D3DFMT_, A8R8G8B8), RL_N(D3DFMT_, X8R8G8B8),
    RL_N(D3DFMT_, R5G6B5), RL_N(D3DFMT_, X1R5G5B5), RL_N(D3DFMT_, A1R5G5B5), RL_N(D3DFMT_, A4R4G4B4),
    RL_N(D3DFMT_, R3G3B2), RL_N(D3DFMT_, A8), RL_N(D3DFMT_, A8R3G3B2), RL_N(D3DFMT_, X4R4G4B4),
    RL_N(D3DFMT_, A2B10G10R10), RL_N(D3DFMT_, A8B8G8R8), RL_N(D3DFMT_, X8B8G8R8), RL_N(D3DFMT_, G16R16),
    RL_N(D3DFMT_, A2R10G10B10), RL_N(D3DFMT_, A16B16G16R16), RL_N(D3DFMT_, A8P8), RL_N(D3DFMT_, P8),
    RL_N(D3DFMT_, L8), RL_N(D3DFMT_, A8L8), RL_N(D3DFMT_, A4L4), RL_N(D3DFMT_, V8U8), RL_N(D3DFMT_, L6V5U5),
    RL_N(D3DFMT_, X8L8V8U8), RL_N(D3DFMT_, Q8W8V8U8), RL_N(D3DFMT_, V16U16), RL_N(D3DFMT_, A2W10V10U10),
    RL_N(D3DFMT_, DXT1), RL_N(D3DFMT_, DXT2), RL_N(D3DFMT_, DXT3), RL_N(D3DFMT_, DXT4), RL_N(D3DFMT_, DXT5),
    RL_N(D3DFMT_, D16_LOCKABLE), RL_N(D3DFMT_, D32), RL_N(D3DFMT_, D15S1), RL_N(D3DFMT_, D24S8), RL_N(D3DFMT_, D24X8),
    RL_N(D3DFMT_, D24X4S4), RL_N(D3DFMT_, D16), RL_N(D3DFMT_, L16), RL_N(D3DFMT_, INDEX16), RL_N(D3DFMT_, INDEX32),
    RL_N(D3DFMT_, Q16W16V16U16), RL_N(D3DFMT_, R16F), RL_N(D3DFMT_, G16R16F), RL_N(D3DFMT_, A16B16G16R16F),
    RL_N(D3DFMT_, R32F), RL_N(D3DFMT_, G32R32F), RL_N(D3DFMT_, A32B32G32R32F)};
static const NameEntry kPrimitives[] = {RL_N(D3DPT_, POINTLIST), RL_N(D3DPT_, LINELIST), RL_N(D3DPT_, LINESTRIP),
    RL_N(D3DPT_, TRIANGLELIST), RL_N(D3DPT_, TRIANGLESTRIP), RL_N(D3DPT_, TRIANGLEFAN)};
static const NameEntry kDeclTypes[] = {RL_N(D3DDECLTYPE_, FLOAT1), RL_N(D3DDECLTYPE_, FLOAT2),
    RL_N(D3DDECLTYPE_, FLOAT3), RL_N(D3DDECLTYPE_, FLOAT4), RL_N(D3DDECLTYPE_, D3DCOLOR), RL_N(D3DDECLTYPE_, UBYTE4),
    RL_N(D3DDECLTYPE_, SHORT2), RL_N(D3DDECLTYPE_, SHORT4), RL_N(D3DDECLTYPE_, UBYTE4N), RL_N(D3DDECLTYPE_, SHORT2N),
    RL_N(D3DDECLTYPE_, SHORT4N), RL_N(D3DDECLTYPE_, USHORT2N), RL_N(D3DDECLTYPE_, USHORT4N), RL_N(D3DDECLTYPE_, UDEC3),
    RL_N(D3DDECLTYPE_, DEC3N), RL_N(D3DDECLTYPE_, FLOAT16_2), RL_N(D3DDECLTYPE_, FLOAT16_4), RL_N(D3DDECLTYPE_, UNUSED)};
static const uint32_t kDeclTypeSize[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0};
static const NameEntry kDeclUsages[] = {RL_N(D3DDECLUSAGE_, POSITION), RL_N(D3DDECLUSAGE_, BLENDWEIGHT),
    RL_N(D3DDECLUSAGE_, BLENDINDICES), RL_N(D3DDECLUSAGE_, NORMAL), RL_N(D3DDECLUSAGE_, PSIZE),
    RL_N(D3DDECLUSAGE_, TEXCOORD), RL_N(D3DDECLUSAGE_, TANGENT), RL_N(D3DDECLUSAGE_, BINORMAL),
    RL_N(D3DDECLUSAGE_, TESSFACTOR), RL_N(D3DDECLUSAGE_, POSITIONT), RL_N(D3DDECLUSAGE_, COLOR), RL_N(D3DDECLUSAGE_, FOG),
    RL_N(D3DDECLUSAGE_, DEPTH), RL_N(D3DDECLUSAGE_, SAMPLE)};
static const NameEntry kLightTypes[] = {RL_N(D3DLIGHT_, POINT), RL_N(D3DLIGHT_, SPOT), RL_N(D3DLIGHT_, DIRECTIONAL)};
static const NameEntry kPools[] = {RL_N(D3DPOOL_, DEFAULT), RL_N(D3DPOOL_, MANAGED), RL_N(D3DPOOL_, SYSTEMMEM),
    RL_N(D3DPOOL_, SCRATCH)};
#undef RL_N

template <size_t N>
static std::string lookup(const NameEntry (&table)[N], uint32_t v, const char* fallbackPrefix)
{
    for (const NameEntry& e : table)
        if (e.value == v)
            return e.name;
    return std::string(fallbackPrefix) + std::to_string(v);
}
static bool isFloatRenderState(uint32_t s)
{
    for (uint32_t f : kFloatRenderStates)
        if (f == s)
            return true;
    return false;
}
static std::string transformName(uint32_t t)
{
    if (t == D3DTS_VIEW) return "VIEW";
    if (t == D3DTS_PROJECTION) return "PROJECTION";
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7) return "TEXTURE" + std::to_string(t - D3DTS_TEXTURE0);
    if (t == 256) return "WORLD";
    if (t > 256 && t < 512) return "WORLDMATRIX_" + std::to_string(t - 256);
    return "TRANSFORM_" + std::to_string(t);
}
static std::string hex32(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

// ---- helpers ------------------------------------------------------------------------------------
uint32_t fvfStride(uint32_t fvf)
{
    uint32_t s = 0;
    fvfElements(fvf, &s);
    return s;
}

std::vector<VertexElement> fvfElements(uint32_t fvf, uint32_t* strideOut)
{
    std::vector<VertexElement> e;
    uint16_t off = 0;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t idx) {
        e.push_back(VertexElement{0, off, type, D3DDECLMETHOD_DEFAULT, usage, idx});
        off = uint16_t(off + kDeclTypeSize[type]);
    };
    uint32_t pos = fvf & D3DFVF_POSITION_MASK;
    if (pos == D3DFVF_XYZRHW)
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
    else if (pos == D3DFVF_XYZW)
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
    else if (pos) {
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
        if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5) {
            int betas = int((pos - D3DFVF_XYZB1) / 2 + 1);
            bool lastIdx = (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) != 0;
            int weights = lastIdx ? betas - 1 : betas;
            if (weights > 0)
                add(uint8_t(D3DDECLTYPE_FLOAT1 + weights - 1), D3DDECLUSAGE_BLENDWEIGHT, 0);
            if (lastIdx)
                add((fvf & D3DFVF_LASTBETA_UBYTE4) ? D3DDECLTYPE_UBYTE4 : D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0);
        }
    }
    if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
    if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
    uint32_t texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (uint32_t i = 0; i < texCount; ++i) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3u;
        uint8_t type = sz == 0 ? D3DDECLTYPE_FLOAT2 : sz == 1 ? D3DDECLTYPE_FLOAT3 : sz == 2 ? D3DDECLTYPE_FLOAT4 : D3DDECLTYPE_FLOAT1;
        add(type, D3DDECLUSAGE_TEXCOORD, uint8_t(i));
    }
    if (strideOut)
        *strideOut = off;
    return e;
}

bool formatBlockInfo(uint32_t fmt, uint32_t& bpb, uint32_t& bw, uint32_t& bh)
{
    bw = bh = 1;
    switch (fmt) {
    case D3DFMT_DXT1: bpb = 8; bw = bh = 4; return true;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: bpb = 16; bw = bh = 4; return true;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_Q8W8V8U8:
    case D3DFMT_X8L8V8U8: case D3DFMT_V16U16: case D3DFMT_G16R16: case D3DFMT_A2B10G10R10: case D3DFMT_A2R10G10B10:
    case D3DFMT_R32F: case D3DFMT_G16R16F: case D3DFMT_A2W10V10U10: bpb = 4; return true;
    case D3DFMT_R8G8B8: bpb = 3; return true;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_A8R3G3B2:
    case D3DFMT_A8P8: bpb = 2; return true;
    case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_A4L4: case D3DFMT_P8: case D3DFMT_R3G3B2: bpb = 1; return true;
    case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: bpb = 8; return true;
    case D3DFMT_A32B32G32R32F: bpb = 16; return true;
    default: bpb = 0; return false;
    }
}

std::vector<uint32_t> makeCheckerARGB(uint32_t w, uint32_t h, uint32_t c0, uint32_t c1, uint32_t cell)
{
    std::vector<uint32_t> t(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            t[size_t(y) * w + x] = (((x / cell) + (y / cell)) & 1) ? c1 : c0;
    return t;
}

static uint32_t primVertexCount(uint32_t type, uint32_t primCount)
{
    switch (type) {
    case D3DPT_POINTLIST: return primCount;
    case D3DPT_LINELIST: return primCount * 2;
    case D3DPT_LINESTRIP: return primCount + 1;
    case D3DPT_TRIANGLELIST: return primCount * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return primCount + 2;
    default: return 0;
    }
}

// ---- recorder ------------------------------------------------------------------------------------
class Recorder {
public:
    struct Buffer {
        int id;
        bool index;
        uint32_t size, usage, pool, fvfOrFormat;
        std::vector<uint8_t> shadow;
        JV versions = JV::arr();
        int version = -1;
    };
    struct Texture {
        int id;
        const char* kind;
        uint32_t w, h, levels, usage, format, pool;
        std::vector<std::string> levelBlob;
        JV uploads = JV::arr();
    };
    struct StreamBind {
        int id = NONE;
        uint32_t offset = 0, stride = 0;
    };
    struct State {
        std::map<uint32_t, uint32_t> rs, tss[8], samp[16];
        std::map<uint32_t, Mat4> xf;
        std::map<uint32_t, Light> lights;
        std::map<uint32_t, bool> lightOn;
        bool hasMat = false, hasVp = false;
        Material mat{};
        Viewport vp{};
        int tex[16];
        int rt = RT_BACKBUFFER, ds = DS_DEFAULT;
        bool formatIsDecl = false;
        uint32_t fvf = 0;
        int decl = NONE, vs = NONE, ps = NONE;
        std::map<uint32_t, std::array<float, 4>> vsF, psF;
        std::map<uint32_t, std::array<int, 4>> vsI;
        std::map<uint32_t, int> vsB;
        StreamBind streams[16];
        int indices = NONE;
        std::string tag = "world";
        State()
        {
            for (int& t : tex)
                t = NONE;
        }
    };

    Api api = API_D3D9;
    State st;
    std::map<int, Buffer> buffers;
    std::map<int, Texture> textures;
    std::map<int, std::vector<VertexElement>> decls;
    JV declsJson = JV::arr(), shadersJson = JV::arr(), draws = JV::arr(), clears = JV::arr(), probes = JV::arr();
    JV blobs = JV::obj(), annotations = JV::obj(), stateBlocks = JV::arr(), recordedFrames = JV::arr();
    std::map<std::string, int> stateBlockIndex;
    std::map<int, int> shaderDecl;
    std::vector<std::pair<int, std::string>> rtDumps;
    int nextId = 0;
    int frame = -1;
    bool recording = false;
    int seq = 0;

    std::string blob(const void* data, size_t size)
    {
        char hex[65];
        rl_sha256_hex(data, size, hex);
        std::string key(hex);
        std::vector<char> b64(rl_base64_size(size));
        rl_base64_encode(data, size, b64.data());
        blobs.set(key, JV::str(b64.data()));
        return key;
    }

    static JV colorJ(const Color& c) { return JV::floats(&c.r, 4); }
    static JV vec3J(const Vec3& v) { return JV::floats(&v.x, 3); }

    static JV elementsJ(const std::vector<VertexElement>& e)
    {
        JV a = JV::arr();
        for (const VertexElement& v : e) {
            JV o = JV::obj();
            o.set("stream", JV::integer(v.Stream));
            o.set("offset", JV::integer(v.Offset));
            o.set("type", JV::str(lookup(kDeclTypes, v.Type, "DECLTYPE_")));
            o.set("size", JV::integer(v.Type < 18 ? kDeclTypeSize[v.Type] : 0));
            o.set("method", JV::integer(v.Method));
            o.set("usage", JV::str(lookup(kDeclUsages, v.Usage, "DECLUSAGE_")));
            o.set("usage_index", JV::integer(v.UsageIndex));
            a.push(o);
        }
        return a;
    }

    JV stateJ() const
    {
        JV s = JV::obj();
        JV rsj = JV::obj(), rsf = JV::obj();
        for (auto& kv : st.rs) {
            std::string n = lookup(kRenderStates, kv.first, "RS_");
            rsj.set(n, JV::uinteger(kv.second));
            if (isFloatRenderState(kv.first)) {
                float f;
                memcpy(&f, &kv.second, 4);
                rsf.set(n, JV::num(f));
            }
        }
        s.set("render_states", rsj);
        s.set("render_states_float", rsf);
        JV stages = JV::arr();
        for (int i = 0; i < 8; ++i) {
            if (st.tss[i].empty())
                continue;
            JV o = JV::obj(), v = JV::obj();
            o.set("stage", JV::integer(i));
            for (auto& kv : st.tss[i])
                v.set(lookup(kStageStates, kv.first, "TSS_"), JV::uinteger(kv.second));
            o.set("states", v);
            stages.push(o);
        }
        s.set("texture_stages", stages);
        JV samps = JV::arr();
        for (int i = 0; i < 16; ++i) {
            if (st.samp[i].empty())
                continue;
            JV o = JV::obj(), v = JV::obj();
            o.set("sampler", JV::integer(i));
            for (auto& kv : st.samp[i])
                v.set(lookup(kSamplerStates, kv.first, "SAMP_"), JV::uinteger(kv.second));
            o.set("states", v);
            samps.push(o);
        }
        s.set("samplers", samps);
        JV tex = JV::arr();
        for (int i = 0; i < 16; ++i)
            if (st.tex[i] != NONE) {
                JV o = JV::obj();
                o.set("stage", JV::integer(i));
                o.set("texture", JV::integer(st.tex[i]));
                tex.push(o);
            }
        s.set("textures", tex);
        JV lights = JV::arr();
        for (auto& kv : st.lights) {
            const Light& l = kv.second;
            JV o = JV::obj();
            o.set("index", JV::integer(kv.first));
            auto on = st.lightOn.find(kv.first);
            o.set("enabled", JV::boolean(on != st.lightOn.end() && on->second));
            o.set("type", JV::str(lookup(kLightTypes, l.Type, "LIGHT_")));
            o.set("diffuse", colorJ(l.Diffuse));
            o.set("specular", colorJ(l.Specular));
            o.set("ambient", colorJ(l.Ambient));
            o.set("position", vec3J(l.Position));
            o.set("direction", vec3J(l.Direction));
            o.set("range", JV::num(l.Range));
            o.set("falloff", JV::num(l.Falloff));
            o.set("attenuation", JV::floats(&l.Attenuation0, 3));
            o.set("theta", JV::num(l.Theta));
            o.set("phi", JV::num(l.Phi));
            lights.push(o);
        }
        s.set("lights", lights);
        if (st.hasMat) {
            JV m = JV::obj();
            m.set("diffuse", colorJ(st.mat.Diffuse));
            m.set("ambient", colorJ(st.mat.Ambient));
            m.set("specular", colorJ(st.mat.Specular));
            m.set("emissive", colorJ(st.mat.Emissive));
            m.set("power", JV::num(st.mat.Power));
            s.set("material", m);
        } else
            s.set("material", JV::null());
        JV vp = JV::obj();
        Viewport v = st.hasVp ? st.vp : Viewport{0, 0, uint32_t(kWidth), uint32_t(kHeight), 0.0f, 1.0f};
        vp.set("x", JV::integer(v.X));
        vp.set("y", JV::integer(v.Y));
        vp.set("width", JV::integer(v.Width));
        vp.set("height", JV::integer(v.Height));
        vp.set("min_z", JV::num(v.MinZ));
        vp.set("max_z", JV::num(v.MaxZ));
        s.set("viewport", vp);
        s.set("render_target", st.rt == RT_BACKBUFFER ? JV::str("backbuffer") : JV::integer(st.rt));
        s.set("depth_stencil", st.ds == DS_DEFAULT ? JV::str("default") : JV::integer(st.ds));
        s.set("vertex_shader", st.vs == NONE ? JV::null() : JV::integer(st.vs));
        s.set("pixel_shader", st.ps == NONE ? JV::null() : JV::integer(st.ps));
        auto constsF = [](const std::map<uint32_t, std::array<float, 4>>& m) {
            JV a = JV::arr();
            for (auto& kv : m) {
                JV o = JV::obj();
                o.set("register", JV::integer(kv.first));
                o.set("value", JV::floats(kv.second.data(), 4));
                a.push(o);
            }
            return a;
        };
        s.set("vs_const_f", constsF(st.vsF));
        JV ci = JV::arr();
        for (auto& kv : st.vsI) {
            JV o = JV::obj(), v4 = JV::arr();
            o.set("register", JV::integer(kv.first));
            for (int x : kv.second)
                v4.push(JV::integer(x));
            o.set("value", v4);
            ci.push(o);
        }
        s.set("vs_const_i", ci);
        JV cb = JV::arr();
        for (auto& kv : st.vsB) {
            JV o = JV::obj();
            o.set("register", JV::integer(kv.first));
            o.set("value", JV::boolean(kv.second != 0));
            cb.push(o);
        }
        s.set("vs_const_b", cb);
        s.set("ps_const_f", constsF(st.psF));
        return s;
    }

    int stateBlock()
    {
        JV s = stateJ();
        std::string key = s.dump(false);
        auto it = stateBlockIndex.find(key);
        if (it != stateBlockIndex.end())
            return it->second;
        int idx = int(stateBlocks.size());
        stateBlocks.push(s);
        stateBlockIndex[key] = idx;
        return idx;
    }

    JV vertexFormatJ() const
    {
        JV f = JV::obj();
        // D3D8 programmable draws take their layout from the shader's declaration.
        int decl = NONE;
        bool useDecl = st.formatIsDecl;
        if (api == API_D3D8 && st.vs != NONE) {
            auto it = shaderDecl.find(st.vs);
            decl = it == shaderDecl.end() ? NONE : it->second;
            useDecl = true;
        } else if (useDecl)
            decl = st.decl;
        if (useDecl) {
            f.set("fvf", JV::null());
            f.set("declaration", decl == NONE ? JV::null() : JV::integer(decl));
            auto it = decls.find(decl);
            f.set("elements", elementsJ(it == decls.end() ? std::vector<VertexElement>() : it->second));
        } else {
            f.set("fvf", JV::uinteger(st.fvf));
            f.set("declaration", JV::null());
            f.set("elements", elementsJ(fvfElements(st.fvf)));
        }
        return f;
    }

    JV transformsJ() const
    {
        JV t = JV::obj();
        for (auto& kv : st.xf)
            t.set(transformName(kv.first), JV::floats(kv.second.m, 16));
        return t;
    }

    JV& beginDraw(const char* call, uint32_t type, uint32_t primCount)
    {
        JV d = JV::obj();
        d.set("frame", JV::integer(frame));
        d.set("seq", JV::integer(seq++));
        d.set("call", JV::str(call));
        d.set("primitive", JV::str(lookup(kPrimitives, type, "PT_")));
        d.set("prim_count", JV::integer(primCount));
        d.set("tag", JV::str(st.tag));
        return draws.push(d);
    }

    void finishDraw(JV& d, bool usesStreams)
    {
        d.set("vertex_format", vertexFormatJ());
        JV streams = JV::arr();
        if (usesStreams)
            for (int i = 0; i < 16; ++i)
                if (st.streams[i].id != NONE) {
                    JV o = JV::obj();
                    o.set("stream", JV::integer(i));
                    o.set("buffer", JV::integer(st.streams[i].id));
                    o.set("version", JV::integer(buffers[st.streams[i].id].version));
                    o.set("offset", JV::integer(st.streams[i].offset));
                    o.set("stride", JV::integer(st.streams[i].stride));
                    streams.push(o);
                }
        d.set("streams", streams);
        d.set("transforms", transformsJ());
        d.set("state", JV::integer(stateBlock()));
    }
};

// ---- Gfx -----------------------------------------------------------------------------------------
Gfx::Gfx(Backend* be, Recorder* rec) : m_be(be), m_rec(rec), m_api(be->api())
{
    m_caps = be->caps();
    rec->api = m_api;
}

void Gfx::check(long hr, const char* what)
{
    if (hr < 0) {
        fprintf(stderr, "rl: %s failed (hr=0x%08lx)\n", what, (unsigned long)hr);
        fflush(stderr);
        exit(1);
    }
}

bool Gfx::formatSupported(uint32_t format, uint32_t usage, uint32_t resourceType)
{
    return m_be->formatSupported(format, usage, resourceType);
}

void Gfx::tag(const char* t) { m_rec->st.tag = t; }
void Gfx::annotate(const std::string& key, JV value) { m_rec->annotations.set(key, std::move(value)); }
void Gfx::probe(int x, int y, uint8_t r, uint8_t g, uint8_t b, int tol, const char* what)
{
    JV p = JV::obj();
    p.set("x", JV::integer(x));
    p.set("y", JV::integer(y));
    JV c = JV::arr();
    c.push(JV::integer(r));
    c.push(JV::integer(g));
    c.push(JV::integer(b));
    p.set("rgb", c);
    p.set("tolerance", JV::integer(tol));
    p.set("what", JV::str(what));
    m_rec->probes.push(p);
}
void Gfx::dumpRenderTarget(int texId, const char* name) { m_rec->rtDumps.emplace_back(texId, name); }

void Gfx::clear(uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    check(m_be->clear(flags, color, z, stencil), "Clear");
    if (!m_rec->recording)
        return;
    JV c = JV::obj();
    c.set("frame", JV::integer(m_rec->frame));
    c.set("seq", JV::integer(m_rec->seq++));
    c.set("flags", JV::uinteger(flags));
    c.set("color", JV::str(hex32(color)));
    c.set("z", JV::num(z));
    c.set("stencil", JV::uinteger(stencil));
    c.set("render_target", m_rec->st.rt == RT_BACKBUFFER ? JV::str("backbuffer") : JV::integer(m_rec->st.rt));
    m_rec->clears.push(c);
}

void Gfx::setRenderState(uint32_t s, uint32_t v)
{
    check(m_be->setRenderState(s, v), "SetRenderState");
    m_rec->st.rs[s] = v;
}
void Gfx::setTextureStageState(uint32_t stage, uint32_t t, uint32_t v)
{
    check(m_be->setTextureStageState(stage, t, v), "SetTextureStageState");
    m_rec->st.tss[stage & 7][t] = v;
}
void Gfx::setSamplerState(uint32_t sampler, uint32_t t, uint32_t v)
{
    check(m_be->setSamplerState(sampler, t, v), "SetSamplerState");
    m_rec->st.samp[sampler & 15][t] = v;
}
void Gfx::setTransform(uint32_t t, const Mat4& m)
{
    check(m_be->setTransform(t, m), "SetTransform");
    m_rec->st.xf[t] = m;
}
void Gfx::setLight(uint32_t i, const Light& l)
{
    check(m_be->setLight(i, l), "SetLight");
    m_rec->st.lights[i] = l;
}
void Gfx::lightEnable(uint32_t i, bool on)
{
    check(m_be->lightEnable(i, on), "LightEnable");
    m_rec->st.lightOn[i] = on;
}
void Gfx::setMaterial(const Material& m)
{
    check(m_be->setMaterial(m), "SetMaterial");
    m_rec->st.mat = m;
    m_rec->st.hasMat = true;
}
void Gfx::setViewport(const Viewport& v)
{
    check(m_be->setViewport(v), "SetViewport");
    m_rec->st.vp = v;
    m_rec->st.hasVp = true;
}

int Gfx::createTexture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    int id = m_rec->nextId++;
    check(m_be->createTexture(id, w, h, levels, usage, fmt, pool), "CreateTexture");
    Recorder::Texture t{id, "texture", w, h, levels, usage, fmt, pool, {}, JV::arr()};
    t.levelBlob.resize(levels);
    m_rec->textures[id] = t;
    return id;
}

void Gfx::uploadTexture(int id, uint32_t level, const void* data)
{
    Recorder::Texture& t = m_rec->textures.at(id);
    uint32_t bpb, bw, bh;
    if (!formatBlockInfo(t.format, bpb, bw, bh)) {
        fprintf(stderr, "rl: uploadTexture: unsupported format %u\n", t.format);
        exit(1);
    }
    uint32_t w = t.w >> level ? t.w >> level : 1, h = t.h >> level ? t.h >> level : 1;
    uint32_t blocksW = (w + bw - 1) / bw, blocksH = (h + bh - 1) / bh;
    uint32_t tight = blocksW * bpb, canon = (tight + 3u) & ~3u;
    check(m_be->writeTexture(id, level, (const uint8_t*)data, tight, blocksH), "LockRect(texture)");
    // Remix canonical packed layout (plan §4.1.3): rows = blocksHigh, rowBytes = align(elementSize * blocksWide, 4).
    std::vector<uint8_t> packed(size_t(canon) * blocksH, 0);
    for (uint32_t r = 0; r < blocksH; ++r)
        memcpy(packed.data() + size_t(r) * canon, (const uint8_t*)data + size_t(r) * tight, tight);
    std::string b = m_rec->blob(packed.data(), packed.size());
    t.levelBlob[level] = b;
    JV u = JV::obj();
    u.set("frame", JV::integer(m_rec->frame));
    u.set("level", JV::integer(level));
    u.set("method", JV::str("LockRect"));
    u.set("width", JV::integer(w));
    u.set("height", JV::integer(h));
    u.set("row_bytes", JV::integer(canon));
    u.set("rows", JV::integer(blocksH));
    u.set("blob", JV::str(b));
    t.uploads.push(u);
}

void Gfx::updateTexture(int src, int dst)
{
    check(m_be->updateTexture(src, dst), "UpdateTexture");
    Recorder::Texture& s = m_rec->textures.at(src);
    Recorder::Texture& d = m_rec->textures.at(dst);
    for (uint32_t l = 0; l < d.levels && l < s.levels; ++l) {
        d.levelBlob[l] = s.levelBlob[l];
        JV u = JV::obj();
        u.set("frame", JV::integer(m_rec->frame));
        u.set("level", JV::integer(l));
        u.set("method", JV::str("UpdateTexture"));
        u.set("source", JV::integer(src));
        u.set("source_level", JV::integer(l));
        u.set("blob", s.levelBlob[l].empty() ? JV::null() : JV::str(s.levelBlob[l]));
        d.uploads.push(u);
    }
}

void Gfx::updateSurface(int src, uint32_t srcLevel, int dst, uint32_t dstLevel)
{
    check(m_be->updateSurface(src, srcLevel, dst, dstLevel), m_api == API_D3D8 ? "CopyRects" : "UpdateSurface");
    Recorder::Texture& s = m_rec->textures.at(src);
    Recorder::Texture& d = m_rec->textures.at(dst);
    d.levelBlob[dstLevel] = s.levelBlob[srcLevel];
    JV u = JV::obj();
    u.set("frame", JV::integer(m_rec->frame));
    u.set("level", JV::integer(dstLevel));
    u.set("method", JV::str(m_api == API_D3D8 ? "CopyRects" : "UpdateSurface"));
    u.set("source", JV::integer(src));
    u.set("source_level", JV::integer(srcLevel));
    u.set("blob", s.levelBlob[srcLevel].empty() ? JV::null() : JV::str(s.levelBlob[srcLevel]));
    d.uploads.push(u);
}

int Gfx::createDepthStencil(uint32_t w, uint32_t h, uint32_t fmt)
{
    int id = m_rec->nextId++;
    check(m_be->createDepthStencil(id, w, h, fmt), "CreateDepthStencilSurface");
    Recorder::Texture t{id, "depth_stencil", w, h, 1, D3DUSAGE_DEPTHSTENCIL, fmt, D3DPOOL_DEFAULT, {}, JV::arr()};
    t.levelBlob.resize(1);
    m_rec->textures[id] = t;
    return id;
}
void Gfx::setRenderTarget(int texId)
{
    check(m_be->setRenderTarget(texId), "SetRenderTarget");
    m_rec->st.rt = texId;
    // D3D9 resets the viewport to the full new target on SetRenderTarget(0, ...).
    m_rec->st.hasVp = false;
    if (texId != RT_BACKBUFFER) {
        const Recorder::Texture& t = m_rec->textures.at(texId);
        m_rec->st.vp = Viewport{0, 0, t.w, t.h, 0.0f, 1.0f};
        m_rec->st.hasVp = true;
    }
}
void Gfx::setDepthStencil(int id)
{
    check(m_be->setDepthStencil(id), "SetDepthStencilSurface");
    m_rec->st.ds = id;
}

int Gfx::createVertexBuffer(uint32_t size, uint32_t usage, uint32_t fvf, uint32_t pool, const void* init)
{
    int id = m_rec->nextId++;
    check(m_be->createVertexBuffer(id, size, usage, fvf, pool), "CreateVertexBuffer");
    Recorder::Buffer b{id, false, size, usage, pool, fvf, std::vector<uint8_t>(size, 0), JV::arr(), -1};
    m_rec->buffers[id] = b;
    if (init)
        writeBuffer(id, 0, init, size, 0);
    return id;
}
int Gfx::createIndexBuffer(uint32_t size, uint32_t usage, uint32_t fmt, uint32_t pool, const void* init)
{
    int id = m_rec->nextId++;
    check(m_be->createIndexBuffer(id, size, usage, fmt, pool), "CreateIndexBuffer");
    Recorder::Buffer b{id, true, size, usage, pool, fmt, std::vector<uint8_t>(size, 0), JV::arr(), -1};
    m_rec->buffers[id] = b;
    if (init)
        writeBuffer(id, 0, init, size, 0);
    return id;
}
void Gfx::writeBuffer(int id, uint32_t offset, const void* data, uint32_t size, uint32_t lockFlags)
{
    check(m_be->writeBuffer(id, offset, data, size, lockFlags), "Lock(buffer)");
    Recorder::Buffer& b = m_rec->buffers.at(id);
    // After D3DLOCK_DISCARD the old contents are undefined; the shadow models them as zero.
    if (lockFlags & D3DLOCK_DISCARD)
        std::fill(b.shadow.begin(), b.shadow.end(), 0);
    memcpy(b.shadow.data() + offset, data, size);
    JV v = JV::obj();
    v.set("frame", JV::integer(m_rec->frame));
    v.set("offset", JV::integer(offset));
    v.set("size", JV::integer(size));
    v.set("lock_flags", JV::uinteger(lockFlags));
    v.set("blob", JV::str(m_rec->blob(b.shadow.data(), b.shadow.size())));
    b.versions.push(v);
    b.version++;
}
void Gfx::setStreamSource(uint32_t stream, int id, uint32_t offset, uint32_t stride)
{
    check(m_be->setStreamSource(stream, id, offset, stride), "SetStreamSource");
    m_rec->st.streams[stream & 15] = Recorder::StreamBind{id, offset, stride};
}
void Gfx::setIndices(int id)
{
    check(m_be->setIndices(id), "SetIndices");
    m_rec->st.indices = id;
}
void Gfx::setFVF(uint32_t fvf)
{
    check(m_be->setFVF(fvf), m_api == API_D3D8 ? "SetVertexShader(FVF)" : "SetFVF");
    m_rec->st.fvf = fvf;
    m_rec->st.formatIsDecl = false;
}
int Gfx::createVertexDeclaration(const std::vector<VertexElement>& elems)
{
    int id = m_rec->nextId++;
    check(m_be->createVertexDeclaration(id, elems), "CreateVertexDeclaration");
    m_rec->decls[id] = elems;
    JV d = JV::obj();
    d.set("id", JV::integer(id));
    d.set("elements", Recorder::elementsJ(elems));
    m_rec->declsJson.push(d);
    return id;
}
void Gfx::setVertexDeclaration(int id)
{
    check(m_be->setVertexDeclaration(id), "SetVertexDeclaration");
    m_rec->st.decl = id;
    m_rec->st.formatIsDecl = true;
}
static JV shaderJ(Recorder* r, int id, bool pixel, const std::vector<uint32_t>& code, const std::string& listing, int decl)
{
    JV s = JV::obj();
    s.set("id", JV::integer(id));
    s.set("stage", JV::str(pixel ? "pixel" : "vertex"));
    char ver[32];
    snprintf(ver, sizeof ver, "%s_%u_%u", pixel ? "ps" : "vs", (code[0] >> 8) & 0xff, code[0] & 0xff);
    s.set("version", JV::str(ver));
    s.set("declaration", decl == NONE ? JV::null() : JV::integer(decl));
    s.set("token_count", JV::integer((long long)code.size()));
    s.set("blob", JV::str(r->blob(code.data(), code.size() * 4)));
    s.set("listing", JV::str(listing));
    return s;
}
int Gfx::createVertexShader(const std::vector<uint32_t>& code, const std::string& listing, int declId)
{
    int id = m_rec->nextId++;
    check(m_be->createVertexShader(id, code, declId), "CreateVertexShader");
    m_rec->shaderDecl[id] = declId;
    m_rec->shadersJson.push(shaderJ(m_rec, id, false, code, listing, m_api == API_D3D8 ? declId : NONE));
    return id;
}
int Gfx::createPixelShader(const std::vector<uint32_t>& code, const std::string& listing)
{
    int id = m_rec->nextId++;
    check(m_be->createPixelShader(id, code), "CreatePixelShader");
    m_rec->shadersJson.push(shaderJ(m_rec, id, true, code, listing, NONE));
    return id;
}
void Gfx::setVertexShader(int id)
{
    check(m_be->setVertexShader(id), "SetVertexShader");
    m_rec->st.vs = id;
}
void Gfx::setPixelShader(int id)
{
    check(m_be->setPixelShader(id), "SetPixelShader");
    m_rec->st.ps = id;
}
void Gfx::setVSConstF(uint32_t start, const float* v, uint32_t count)
{
    check(m_be->setVSConstF(start, v, count), "SetVertexShaderConstantF");
    for (uint32_t i = 0; i < count; ++i)
        m_rec->st.vsF[start + i] = {v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]};
}
void Gfx::setVSConstI(uint32_t start, const int* v, uint32_t count)
{
    check(m_be->setVSConstI(start, v, count), "SetVertexShaderConstantI");
    for (uint32_t i = 0; i < count; ++i)
        m_rec->st.vsI[start + i] = {v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]};
}
void Gfx::setVSConstB(uint32_t start, const int* v, uint32_t count)
{
    check(m_be->setVSConstB(start, v, count), "SetVertexShaderConstantB");
    for (uint32_t i = 0; i < count; ++i)
        m_rec->st.vsB[start + i] = v[i];
}
void Gfx::setPSConstF(uint32_t start, const float* v, uint32_t count)
{
    check(m_be->setPSConstF(start, v, count), "SetPixelShaderConstantF");
    for (uint32_t i = 0; i < count; ++i)
        m_rec->st.psF[start + i] = {v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]};
}
void Gfx::setTexture(uint32_t stage, int id)
{
    check(m_be->setTexture(stage, id), "SetTexture");
    m_rec->st.tex[stage & 15] = id;
}

void Gfx::drawPrimitive(uint32_t type, uint32_t startVertex, uint32_t primCount)
{
    check(m_be->drawPrimitive(type, startVertex, primCount), "DrawPrimitive");
    if (!m_rec->recording)
        return;
    JV& d = m_rec->beginDraw("DrawPrimitive", type, primCount);
    d.set("start_vertex", JV::integer(startVertex));
    d.set("vertex_count", JV::integer(primVertexCount(type, primCount)));
    d.set("index_buffer", JV::null());
    m_rec->finishDraw(d, true);
}
void Gfx::drawIndexedPrimitive(uint32_t type, int baseVertex, uint32_t minIndex, uint32_t numVertices,
                               uint32_t startIndex, uint32_t primCount)
{
    check(m_be->drawIndexedPrimitive(type, baseVertex, minIndex, numVertices, startIndex, primCount), "DrawIndexedPrimitive");
    if (!m_rec->recording)
        return;
    JV& d = m_rec->beginDraw("DrawIndexedPrimitive", type, primCount);
    d.set("base_vertex", JV::integer(baseVertex));
    d.set("min_index", JV::integer(minIndex));
    d.set("num_vertices", JV::integer(numVertices));
    d.set("start_index", JV::integer(startIndex));
    d.set("index_count", JV::integer(primVertexCount(type, primCount)));
    JV ib = JV::obj();
    int id = m_rec->st.indices;
    ib.set("buffer", JV::integer(id));
    ib.set("version", JV::integer(id == NONE ? -1 : m_rec->buffers[id].version));
    ib.set("format", JV::str(id == NONE ? "UNKNOWN" : lookup(kFormats, m_rec->buffers[id].fvfOrFormat, "FMT_")));
    d.set("index_buffer", ib);
    m_rec->finishDraw(d, true);
}
void Gfx::drawPrimitiveUP(uint32_t type, uint32_t primCount, const void* v, uint32_t stride)
{
    check(m_be->drawPrimitiveUP(type, primCount, v, stride), "DrawPrimitiveUP");
    // D3D: the UP calls leave stream 0 unbound.
    m_rec->st.streams[0] = Recorder::StreamBind{};
    if (!m_rec->recording)
        return;
    uint32_t n = primVertexCount(type, primCount);
    JV& d = m_rec->beginDraw("DrawPrimitiveUP", type, primCount);
    d.set("vertex_count", JV::integer(n));
    d.set("index_buffer", JV::null());
    JV up = JV::obj();
    up.set("vertex_stride", JV::integer(stride));
    up.set("vertex_blob", JV::str(m_rec->blob(v, size_t(n) * stride)));
    up.set("index_blob", JV::null());
    up.set("index_format", JV::null());
    d.set("up", up);
    m_rec->finishDraw(d, false);
}
void Gfx::drawIndexedPrimitiveUP(uint32_t type, uint32_t minIndex, uint32_t numVertices, uint32_t primCount,
                                 const void* idx, uint32_t idxFmt, const void* v, uint32_t stride)
{
    check(m_be->drawIndexedPrimitiveUP(type, minIndex, numVertices, primCount, idx, idxFmt, v, stride),
          "DrawIndexedPrimitiveUP");
    m_rec->st.streams[0] = Recorder::StreamBind{};
    m_rec->st.indices = NONE;
    if (!m_rec->recording)
        return;
    uint32_t n = primVertexCount(type, primCount);
    JV& d = m_rec->beginDraw("DrawIndexedPrimitiveUP", type, primCount);
    d.set("min_index", JV::integer(minIndex));
    d.set("num_vertices", JV::integer(numVertices));
    d.set("index_count", JV::integer(n));
    d.set("index_buffer", JV::null());
    JV up = JV::obj();
    up.set("vertex_stride", JV::integer(stride));
    // Vertices [0, minIndex + numVertices) are what the index values can address.
    up.set("vertex_blob", JV::str(m_rec->blob(v, size_t(minIndex + numVertices) * stride)));
    up.set("index_blob", JV::str(m_rec->blob(idx, size_t(n) * (idxFmt == D3DFMT_INDEX32 ? 4 : 2))));
    up.set("index_format", JV::str(lookup(kFormats, idxFmt, "FMT_")));
    d.set("up", up);
    m_rec->finishDraw(d, false);
}

void Gfx::beginFrame(int frame, bool recorded)
{
    m_rec->frame = frame;
    m_rec->recording = recorded;
    m_rec->seq = 0;
    if (recorded)
        m_rec->recordedFrames.push(JV::integer(frame));
    check(m_be->beginScene(), "BeginScene");
}
void Gfx::endFrame() { check(m_be->endScene(), "EndScene"); }

} // namespace rl

// ---- main ----------------------------------------------------------------------------------------
using namespace rl;

static LRESULT CALLBACK rlWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void pumpMessages()
{
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int writeImage(const std::string& base, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
    if (rl_write_file((base + ".rgba").c_str(), rgba.data(), rgba.size()) != 0)
        return -1;
    return rl_write_png_rgba8((base + ".png").c_str(), rgba.data(), w, h);
}

int main(int argc, char** argv)
{
    std::string outDir = ".";
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            outDir = argv[++i];
        else if (!strcmp(argv[i], "--quiet"))
            quiet = true;
        else {
            fprintf(stderr, "usage: %s [--out DIR] [--quiet]\n", argv[0]);
            return 2;
        }
    }
    std::string app = std::string(RL_API == 8 ? "d3d8_" : "") + kScene.name;

    WNDCLASSA wc{};
    wc.lpfnWndProc = rlWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "FuseRelightTestApp";
    RegisterClassA(&wc);
    RECT rc = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("FuseRelightTestApp", app.c_str(), WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fprintf(stderr, "rl: CreateWindow failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    pumpMessages();

    Backend* be = createBackend();
    long hr = be->init(hwnd, kScene.devFlags);
    if (hr < 0) {
        fprintf(stderr, "rl: device creation failed (hr=0x%08lx)\n", (unsigned long)hr);
        return 1;
    }
    if (!quiet)
        printf("%s: adapter \"%s\"\n", app.c_str(), be->adapter().c_str());

    Recorder rec;
    Gfx g(be, &rec);
    sceneInit(g);
    std::vector<uint8_t> rgba;
    for (int f = 0; f < kScene.frames; ++f) {
        pumpMessages();
        bool last = f + 1 == kScene.frames;
        g.beginFrame(f, kScene.recordAllFrames || last);
        sceneFrame(g, f);
        g.endFrame();
        if (last) {
            hr = be->readBackBuffer(rgba);
            if (hr < 0) {
                fprintf(stderr, "rl: back-buffer readback failed (hr=0x%08lx)\n", (unsigned long)hr);
                return 1;
            }
        }
        hr = be->present();
        if (hr < 0) {
            fprintf(stderr, "rl: Present failed (hr=0x%08lx)\n", (unsigned long)hr);
            return 1;
        }
    }

    std::string base = outDir + "/" + app;
    JV dumps = JV::arr();
    if (writeImage(base, rgba, kWidth, kHeight) != 0) {
        fprintf(stderr, "rl: cannot write %s.{png,rgba}\n", base.c_str());
        return 1;
    }
    {
        JV d = JV::obj();
        d.set("name", JV::str("backbuffer"));
        d.set("source", JV::str(RL_API == 8 ? "LockRect(lockable back buffer)" : "GetRenderTargetData(back buffer)"));
        d.set("frame", JV::integer(kScene.frames - 1));
        d.set("width", JV::integer(kWidth));
        d.set("height", JV::integer(kHeight));
        d.set("png", JV::str(app + ".png"));
        d.set("raw", JV::str(app + ".rgba"));
        d.set("alpha", JV::str("forced_opaque"));
        dumps.push(d);
    }
    for (auto& rt : rec.rtDumps) {
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0;
        hr = be->readRenderTarget(rt.first, px, w, h);
        if (hr < 0) {
            fprintf(stderr, "rl: render-target readback of %d failed (hr=0x%08lx)\n", rt.first, (unsigned long)hr);
            return 1;
        }
        std::string b = base + "." + rt.second;
        if (writeImage(b, px, w, h) != 0)
            return 1;
        JV d = JV::obj();
        d.set("name", JV::str(rt.second));
        d.set("source", JV::str(RL_API == 8 ? "CopyRects(render target)" : "GetRenderTargetData(render target)"));
        d.set("texture", JV::integer(rt.first));
        d.set("frame", JV::integer(kScene.frames - 1));
        d.set("width", JV::integer(w));
        d.set("height", JV::integer(h));
        d.set("png", JV::str(app + "." + rt.second + ".png"));
        d.set("raw", JV::str(app + "." + rt.second + ".rgba"));
        d.set("alpha", JV::str("stored"));
        dumps.push(d);
    }

    JV root = JV::obj();
    root.set("schema", JV::str("fuse.relight.app_sidecar/1"));
    root.set("app", JV::str(app));
    root.set("scene", JV::str(kScene.name));
    root.set("api", JV::str(RL_API == 8 ? "d3d8" : "d3d9"));
    root.set("covers", JV::str(kScene.covers));
    root.set("width", JV::integer(kWidth));
    root.set("height", JV::integer(kHeight));
    root.set("frames", JV::integer(kScene.frames));
    root.set("recorded_frames", rec.recordedFrames);
    JV dev = JV::obj();
    dev.set("backbuffer_format", JV::str("X8R8G8B8"));
    dev.set("depth_stencil_format", JV::str("D24S8"));
    dev.set("vertex_processing", JV::str((kScene.devFlags & DEV_SOFTWARE_VP) ? "software" : "hardware"));
    root.set("device", dev);
    root.set("dumps", dumps);
    root.set("probes", rec.probes);
    root.set("annotations", rec.annotations);
    JV textures = JV::arr();
    for (auto& kv : rec.textures) {
        const Recorder::Texture& t = kv.second;
        JV o = JV::obj();
        o.set("id", JV::integer(t.id));
        o.set("kind", JV::str(t.kind));
        o.set("width", JV::integer(t.w));
        o.set("height", JV::integer(t.h));
        o.set("levels", JV::integer(t.levels));
        o.set("format", JV::str(lookup(kFormats, t.format, "FMT_")));
        o.set("format_value", JV::uinteger(t.format));
        o.set("usage", JV::uinteger(t.usage));
        o.set("pool", JV::str(lookup(kPools, t.pool, "POOL_")));
        o.set("uploads", t.uploads);
        textures.push(o);
    }
    root.set("textures", textures);
    JV buffers = JV::arr();
    for (auto& kv : rec.buffers) {
        const Recorder::Buffer& b = kv.second;
        JV o = JV::obj();
        o.set("id", JV::integer(b.id));
        o.set("kind", JV::str(b.index ? "index" : "vertex"));
        o.set("size", JV::integer(b.size));
        o.set("usage", JV::uinteger(b.usage));
        o.set("pool", JV::str(lookup(kPools, b.pool, "POOL_")));
        if (b.index)
            o.set("format", JV::str(lookup(kFormats, b.fvfOrFormat, "FMT_")));
        else
            o.set("fvf", JV::uinteger(b.fvfOrFormat));
        o.set("versions", b.versions);
        buffers.push(o);
    }
    root.set("buffers", buffers);
    root.set("declarations", rec.declsJson);
    root.set("shaders", rec.shadersJson);
    root.set("state_blocks", rec.stateBlocks);
    root.set("clears", rec.clears);
    root.set("draws", rec.draws);
    root.set("blobs", rec.blobs);
    std::string text = root.dump(true);
    if (rl_write_file((base + ".json").c_str(), text.data(), text.size()) != 0) {
        fprintf(stderr, "rl: cannot write %s.json\n", base.c_str());
        return 1;
    }
    if (!quiet)
        printf("%s: %d frames, %u draws, wrote %s.{json,png,rgba}\n", app.c_str(), kScene.frames,
               (unsigned)rec.draws.size(), base.c_str());
    delete be;
    DestroyWindow(hwnd);
    return 0;
}
