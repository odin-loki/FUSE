// FUSE Relight test-app kit (RL-0.4): a tiny D3D shader bytecode builder.
// There is no fxc/D3DX in the MinGW toolchain (and none may be vendored), so the shader apps
// emit the documented D3D9 token stream directly (D3D9 "Shader Code Format": version token,
// instruction tokens, destination/source parameter tokens, END). The builder also keeps the
// assembly listing, which the apps store in the sidecar next to the bytecode.
#pragma once

#include <stdint.h>
#include <string.h>
#include <initializer_list>
#include <string>
#include <vector>

namespace rl {

// D3DSHADER_PARAM_REGISTER_TYPE
enum RegType : uint32_t {
    R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5,
    R_TEXCRDOUT = 6, R_OUTPUT = 6, R_CONSTINT = 7, R_COLOROUT = 8, R_DEPTHOUT = 9, R_SAMPLER = 10,
    R_CONSTBOOL = 14, R_LOOP = 15, R_MISCTYPE = 17, R_PREDICATE = 19
};

// D3DSHADER_INSTRUCTION_OPCODE_TYPE (subset)
enum Op : uint32_t {
    OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7,
    OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13, OP_LRP = 18,
    OP_FRC = 19, OP_M4x4 = 20, OP_M4x3 = 21, OP_M3x3 = 23, OP_LOOP = 27, OP_ENDLOOP = 29,
    OP_DCL = 31, OP_IF = 40, OP_ELSE = 42, OP_ENDIF = 43, OP_DEFB = 47, OP_DEFI = 48,
    OP_TEXCOORD = 64, OP_TEX = 66 /* tex (ps 1.x) and texld (ps 1.4+) */, OP_DEF = 81,
    OP_PHASE = 0xFFFD, OP_END = 0xFFFF
};

enum : uint32_t { SWZ_XYZW = 0xE4, SWZ_XXXX = 0x00, SWZ_YYYY = 0x55, SWZ_ZZZZ = 0xAA, SWZ_WWWW = 0xFF };
enum : uint32_t { MASK_X = 1, MASK_Y = 2, MASK_Z = 4, MASK_W = 8, MASK_ALL = 15, MASK_XY = 3, MASK_XYZ = 7 };

inline uint32_t regBits(uint32_t type, uint32_t num)
{
    return 0x80000000u | (num & 0x7FF) | ((type & 7u) << 28) | (((type >> 3) & 3u) << 11);
}
inline uint32_t dst(uint32_t type, uint32_t num, uint32_t mask = MASK_ALL, uint32_t resultMod = 0)
{
    return regBits(type, num) | ((mask & 15u) << 16) | ((resultMod & 15u) << 20);
}
inline uint32_t src(uint32_t type, uint32_t num, uint32_t swizzle = SWZ_XYZW, uint32_t srcMod = 0)
{
    return regBits(type, num) | ((swizzle & 0xFFu) << 16) | ((srcMod & 15u) << 24);
}
static const uint32_t SRCMOD_NEG = 1;
static const uint32_t RESMOD_SAT = 1;

class ShaderAsm {
public:
    ShaderAsm(bool pixel, int major, int minor) : m_pixel(pixel), m_major(major), m_minor(minor)
    {
        m_tokens.push_back((pixel ? 0xFFFF0000u : 0xFFFE0000u) | (uint32_t(major) << 8) | uint32_t(minor));
        m_listing = std::string(pixel ? "ps_" : "vs_") + std::to_string(major) + "_" + std::to_string(minor) + "\n";
    }
    // One instruction. `text` is its assembly line for the listing. SM2+ encodes the parameter
    // count in bits 24..27 of the instruction token; SM1 leaves it zero.
    ShaderAsm& op(const char* text, uint32_t opcode, std::initializer_list<uint32_t> params)
    {
        uint32_t tok = opcode;
        if (m_major >= 2)
            tok |= uint32_t(params.size()) << 24;
        m_tokens.push_back(tok);
        m_tokens.insert(m_tokens.end(), params.begin(), params.end());
        m_listing += text;
        m_listing += "\n";
        return *this;
    }
    // dcl_<usage><index> for vertex inputs / vs_3_0 outputs / ps_3_0 inputs.
    ShaderAsm& dclUsage(const char* text, uint32_t usage, uint32_t usageIndex, uint32_t dstParam)
    {
        return op(text, OP_DCL, {0x80000000u | (usage & 31u) | ((usageIndex & 15u) << 16), dstParam});
    }
    // ps_2_x dcl t#/v# (no usage).
    ShaderAsm& dclPlain(const char* text, uint32_t dstParam) { return op(text, OP_DCL, {0x80000000u, dstParam}); }
    // dcl_2d s# (texture type 2 = D3DSTT_2D).
    ShaderAsm& dclSampler2D(const char* text, uint32_t sampler)
    {
        return op(text, OP_DCL, {0x80000000u | (2u << 27), dst(R_SAMPLER, sampler)});
    }
    ShaderAsm& def(const char* text, uint32_t reg, float x, float y, float z, float w)
    {
        uint32_t f[4];
        float v[4] = {x, y, z, w};
        for (int i = 0; i < 4; ++i)
            memcpy(&f[i], &v[i], 4);
        return op(text, OP_DEF, {dst(R_CONST, reg), f[0], f[1], f[2], f[3]});
    }
    ShaderAsm& defi(const char* text, uint32_t reg, int x, int y, int z, int w)
    {
        return op(text, OP_DEFI, {dst(R_CONSTINT, reg), uint32_t(x), uint32_t(y), uint32_t(z), uint32_t(w)});
    }
    const std::vector<uint32_t>& finish()
    {
        if (!m_done) {
            m_tokens.push_back(0x0000FFFFu);
            m_done = true;
        }
        return m_tokens;
    }
    const std::string& listing() const { return m_listing; }
    bool pixel() const { return m_pixel; }

private:
    bool m_pixel;
    int m_major, m_minor;
    bool m_done = false;
    std::vector<uint32_t> m_tokens;
    std::string m_listing;
};

} // namespace rl
