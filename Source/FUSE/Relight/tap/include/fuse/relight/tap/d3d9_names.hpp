// FUSE Relight RL-1.1: D3D9 enum names used by the recording tap's JSON (same spelling as the
// RL-0.4 app sidecars: the D3D9 name without its prefix). Values are the D3D9 SDK values; the
// d3d9 bridge (tap/dxvk/fuse_tap_dxvk.cpp, which sees the real d3d9types.h) static_asserts every
// entry against the SDK headers.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fuse::relight::tap::names {

struct NameEntry {
    std::uint32_t value;
    const char* name;
};

inline constexpr NameEntry kRenderStates[] = {
    {7, "ZENABLE"}, {8, "FILLMODE"}, {9, "SHADEMODE"}, {14, "ZWRITEENABLE"}, {15, "ALPHATESTENABLE"},
    {16, "LASTPIXEL"}, {19, "SRCBLEND"}, {20, "DESTBLEND"}, {22, "CULLMODE"}, {23, "ZFUNC"}, {24, "ALPHAREF"},
    {25, "ALPHAFUNC"}, {26, "DITHERENABLE"}, {27, "ALPHABLENDENABLE"}, {28, "FOGENABLE"}, {29, "SPECULARENABLE"},
    {34, "FOGCOLOR"}, {35, "FOGTABLEMODE"}, {36, "FOGSTART"}, {37, "FOGEND"}, {38, "FOGDENSITY"},
    {48, "RANGEFOGENABLE"}, {52, "STENCILENABLE"}, {53, "STENCILFAIL"}, {54, "STENCILZFAIL"}, {55, "STENCILPASS"},
    {56, "STENCILFUNC"}, {57, "STENCILREF"}, {58, "STENCILMASK"}, {59, "STENCILWRITEMASK"}, {60, "TEXTUREFACTOR"},
    {128, "WRAP0"}, {129, "WRAP1"}, {130, "WRAP2"}, {131, "WRAP3"}, {132, "WRAP4"}, {133, "WRAP5"}, {134, "WRAP6"},
    {135, "WRAP7"}, {136, "CLIPPING"}, {137, "LIGHTING"}, {139, "AMBIENT"}, {140, "FOGVERTEXMODE"},
    {141, "COLORVERTEX"}, {142, "LOCALVIEWER"}, {143, "NORMALIZENORMALS"}, {145, "DIFFUSEMATERIALSOURCE"},
    {146, "SPECULARMATERIALSOURCE"}, {147, "AMBIENTMATERIALSOURCE"}, {148, "EMISSIVEMATERIALSOURCE"},
    {151, "VERTEXBLEND"}, {152, "CLIPPLANEENABLE"}, {154, "POINTSIZE"}, {155, "POINTSIZE_MIN"},
    {156, "POINTSPRITEENABLE"}, {157, "POINTSCALEENABLE"}, {158, "POINTSCALE_A"}, {159, "POINTSCALE_B"},
    {160, "POINTSCALE_C"}, {161, "MULTISAMPLEANTIALIAS"}, {162, "MULTISAMPLEMASK"}, {163, "PATCHEDGESTYLE"},
    {165, "DEBUGMONITORTOKEN"}, {166, "POINTSIZE_MAX"}, {167, "INDEXEDVERTEXBLENDENABLE"}, {168, "COLORWRITEENABLE"},
    {170, "TWEENFACTOR"}, {171, "BLENDOP"}, {172, "POSITIONDEGREE"}, {173, "NORMALDEGREE"},
    {174, "SCISSORTESTENABLE"}, {175, "SLOPESCALEDEPTHBIAS"}, {176, "ANTIALIASEDLINEENABLE"},
    {178, "MINTESSELLATIONLEVEL"}, {179, "MAXTESSELLATIONLEVEL"}, {180, "ADAPTIVETESS_X"}, {181, "ADAPTIVETESS_Y"},
    {182, "ADAPTIVETESS_Z"}, {183, "ADAPTIVETESS_W"}, {184, "ENABLEADAPTIVETESSELLATION"},
    {185, "TWOSIDEDSTENCILMODE"}, {186, "CCW_STENCILFAIL"}, {187, "CCW_STENCILZFAIL"}, {188, "CCW_STENCILPASS"},
    {189, "CCW_STENCILFUNC"}, {190, "COLORWRITEENABLE1"}, {191, "COLORWRITEENABLE2"}, {192, "COLORWRITEENABLE3"},
    {193, "BLENDFACTOR"}, {194, "SRGBWRITEENABLE"}, {195, "DEPTHBIAS"}, {198, "WRAP8"}, {199, "WRAP9"},
    {200, "WRAP10"}, {201, "WRAP11"}, {202, "WRAP12"}, {203, "WRAP13"}, {204, "WRAP14"}, {205, "WRAP15"},
    {206, "SEPARATEALPHABLENDENABLE"}, {207, "SRCBLENDALPHA"}, {208, "DESTBLENDALPHA"}, {209, "BLENDOPALPHA"}};

inline constexpr NameEntry kTextureStageStates[] = {
    {1, "COLOROP"}, {2, "COLORARG1"}, {3, "COLORARG2"}, {4, "ALPHAOP"}, {5, "ALPHAARG1"}, {6, "ALPHAARG2"},
    {7, "BUMPENVMAT00"}, {8, "BUMPENVMAT01"}, {9, "BUMPENVMAT10"}, {10, "BUMPENVMAT11"}, {11, "TEXCOORDINDEX"},
    {22, "BUMPENVLSCALE"}, {23, "BUMPENVLOFFSET"}, {24, "TEXTURETRANSFORMFLAGS"}, {26, "COLORARG0"},
    {27, "ALPHAARG0"}, {28, "RESULTARG"}, {32, "CONSTANT"}};

inline constexpr NameEntry kSamplerStates[] = {
    {1, "ADDRESSU"}, {2, "ADDRESSV"}, {3, "ADDRESSW"}, {4, "BORDERCOLOR"}, {5, "MAGFILTER"}, {6, "MINFILTER"},
    {7, "MIPFILTER"}, {8, "MIPMAPLODBIAS"}, {9, "MAXMIPLEVEL"}, {10, "MAXANISOTROPY"}, {11, "SRGBTEXTURE"},
    {12, "ELEMENTINDEX"}, {13, "DMAPOFFSET"}};

inline constexpr NameEntry kPrimitiveTypes[] = {{1, "POINTLIST"},     {2, "LINELIST"},      {3, "LINESTRIP"},
                                                {4, "TRIANGLELIST"},  {5, "TRIANGLESTRIP"}, {6, "TRIANGLEFAN"}};

inline constexpr NameEntry kDeclTypes[] = {
    {0, "FLOAT1"},   {1, "FLOAT2"},    {2, "FLOAT3"},    {3, "FLOAT4"},    {4, "D3DCOLOR"},  {5, "UBYTE4"},
    {6, "SHORT2"},   {7, "SHORT4"},    {8, "UBYTE4N"},   {9, "SHORT2N"},   {10, "SHORT4N"},  {11, "USHORT2N"},
    {12, "USHORT4N"}, {13, "UDEC3"},   {14, "DEC3N"},    {15, "FLOAT16_2"}, {16, "FLOAT16_4"}, {17, "UNUSED"}};
/// Byte size per D3DDECLTYPE (Remix elementSize), indexed by type; 0 for UNUSED.
inline constexpr std::uint32_t kDeclTypeSize[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0};

inline constexpr NameEntry kDeclUsages[] = {
    {0, "POSITION"}, {1, "BLENDWEIGHT"}, {2, "BLENDINDICES"}, {3, "NORMAL"}, {4, "PSIZE"},     {5, "TEXCOORD"},
    {6, "TANGENT"},  {7, "BINORMAL"},    {8, "TESSFACTOR"},   {9, "POSITIONT"}, {10, "COLOR"}, {11, "FOG"},
    {12, "DEPTH"},   {13, "SAMPLE"}};

inline constexpr NameEntry kLightTypes[] = {{1, "POINT"}, {2, "SPOT"}, {3, "DIRECTIONAL"}};

inline constexpr NameEntry kIndexFormats[] = {{101, "INDEX16"}, {102, "INDEX32"}};

inline constexpr NameEntry kPools[] = {{0, "DEFAULT"}, {1, "MANAGED"}, {2, "SYSTEMMEM"}, {3, "SCRATCH"}};

/// Name of `value` in `table`, or nullptr.
template <std::size_t N>
constexpr const char* find(const NameEntry (&table)[N], std::uint32_t value) {
    for (const NameEntry& e : table) {
        if (e.value == value) {
            return e.name;
        }
    }
    return nullptr;
}

} // namespace fuse::relight::tap::names
