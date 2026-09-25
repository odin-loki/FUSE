/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/d3d9/d3d9_rtx_geometry.cpp@0867d3c and src/dxso/dxso_compiler.cpp@0867d3c
// (emitLoadConstant's DxsoShaderMetaInfo bookkeeping, the source loads of processInstruction)

// FUSE Relight RL-1.6: the vertexshader hash component. See vs_hash.hpp.
//
// Modifications (FUSE): the compiler's bookkeeping is replayed on the token stream without
// compiling; the upstream hash itself is fuse::relight::hash::hashVertexShader (RL-0.5).
#include <fuse/relight/capture/vertex_capture/vs_hash.hpp>

#include <fuse/relight/hash/geometry_hash.hpp>

#include <algorithm>
#include <vector>

namespace fuse::relight::capture::vertex_capture {

namespace {

// D3DSHADER_INSTRUCTION_OPCODE_TYPE values.
enum Op : std::uint32_t {
    kNop = 0, kMov = 1, kAdd = 2, kSub = 3, kMad = 4, kMul = 5, kRcp = 6, kRsq = 7, kDp3 = 8, kDp4 = 9, kMin = 10,
    kMax = 11, kSlt = 12, kSge = 13, kExp = 14, kLog = 15, kLit = 16, kDst = 17, kLrp = 18, kFrc = 19, kM4x4 = 20,
    kM4x3 = 21, kM3x4 = 22, kM3x3 = 23, kM3x2 = 24, kCall = 25, kCallNz = 26, kLoop = 27, kRet = 28, kEndLoop = 29,
    kLabel = 30, kDcl = 31, kPow = 32, kCrs = 33, kSgn = 34, kAbs = 35, kNrm = 36, kSinCos = 37, kRep = 38,
    kEndRep = 39, kIf = 40, kIfc = 41, kElse = 42, kEndIf = 43, kBreak = 44, kBreakC = 45, kMova = 46, kDefB = 47,
    kDefI = 48, kTexKill = 65, kTex = 66, kExpP = 78, kLogP = 79, kCnd = 80, kDef = 81, kCmp = 88, kDp2Add = 90,
    kDsX = 91, kDsY = 92, kTexLdd = 93, kSetP = 94, kTexLdl = 95, kBreakP = 96, kPhase = 0xFFFD, kComment = 0xFFFE,
    kEnd = 0xFFFF,
};

// D3DSHADER_PARAM_REGISTER_TYPE values the bookkeeping cares about.
constexpr std::uint32_t kRegConst = 2, kRegConstInt = 7, kRegConst2 = 11, kRegConst3 = 12, kRegConst4 = 13,
                        kRegConstBool = 14;

constexpr std::uint32_t kParamBit = 0x80000000u;
constexpr std::uint32_t kRelativeBit = 1u << 13;  // D3DSHADER_ADDRMODE_RELATIVE
constexpr std::uint32_t kPredicatedBit = 1u << 28; // D3DSHADER_INSTRUCTION_PREDICATED

std::uint32_t registerType(std::uint32_t token) { return ((token >> 28) & 0x7u) | ((token >> 8) & 0x18u); }
std::uint32_t registerNumber(std::uint32_t token) { return token & 0x7FFu; }

bool hasDestination(std::uint32_t op) {
    switch (op) {
    case kNop: case kCall: case kCallNz: case kLoop: case kRet: case kEndLoop: case kLabel: case kRep: case kEndRep:
    case kIf: case kIfc: case kElse: case kEndIf: case kBreak: case kBreakC: case kBreakP: case kTexKill: case kPhase:
        return false;
    default:
        return true;
    }
}

/// Bit i: the compiler loads source i (the ops the dxso compiler implements; everything else loads
/// nothing: it is unhandled, a declaration or pure control flow).
std::uint32_t loadedSources(std::uint32_t op) {
    switch (op) {
    case kMov: case kMova: case kRcp: case kRsq: case kExp: case kLog: case kLit: case kFrc: case kSgn: case kAbs:
    case kNrm: case kSinCos: case kExpP: case kLogP: case kDsX: case kDsY: case kRep: case kIf: case kTex:
    case kTexLdl:
        return 0b1;
    case kAdd: case kSub: case kMul: case kDp3: case kDp4: case kMin: case kMax: case kSlt: case kSge: case kDst:
    case kPow: case kCrs: case kSetP: case kIfc: case kBreakC:
    case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2:
        return 0b11;
    case kMad: case kLrp: case kCmp: case kCnd: case kDp2Add:
        return 0b111;
    case kLoop: // loop aL, i#: the integer register
        return 0b10;
    case kTexLdd:
        return 0b1101;
    default:
        return 0;
    }
}

/// Rows of src1 a matrix op reads (emitMatrixAlu: componentCount).
std::uint32_t matrixRows(std::uint32_t op) {
    switch (op) {
    case kM4x4: case kM3x4: return 4;
    case kM4x3: case kM3x3: return 3;
    case kM3x2: return 2;
    default: return 1;
    }
}

class Bookkeeping {
public:
    // Every register a token can name (11-bit numbers; c# up to CONST4 + 2047), whatever the layout.
    explicit Bookkeeping(const ConstantLayout& layout)
        : m_layout(layout), m_definedF(8192, false), m_definedI(2048, false), m_definedB(2048, false) {}

    void define(std::uint32_t type, std::uint32_t num) {
        std::vector<bool>* set = nullptr;
        switch (type) {
        case kRegConst: case kRegConst2: case kRegConst3: case kRegConst4:
            num = floatIndex(type, num);
            set = &m_definedF;
            break;
        case kRegConstInt: set = &m_definedI; break;
        case kRegConstBool: set = &m_definedB; break;
        default: return;
        }
        if (num < set->size()) {
            (*set)[num] = true;
        }
    }

    // DxsoCompiler::emitLoadConstant.
    void load(std::uint32_t type, std::uint32_t num, bool relative) {
        switch (type) {
        case kRegConst: case kRegConst2: case kRegConst3: case kRegConst4: {
            num = floatIndex(type, num);
            if (relative) {
                m_ranges.maxConstIndexF = m_layout.floatCount;
            } else if (!(num < m_definedF.size() && m_definedF[num])) {
                m_ranges.maxConstIndexF = std::min(std::max(m_ranges.maxConstIndexF, num + 1), m_layout.floatCount);
            }
            break;
        }
        case kRegConstInt:
            if (!(num < m_definedI.size() && m_definedI[num])) {
                m_ranges.maxConstIndexI = std::min(std::max(m_ranges.maxConstIndexI, num + 1), m_layout.intCount);
            }
            break;
        case kRegConstBool:
            if (!(num < m_definedB.size() && m_definedB[num])) {
                m_ranges.maxConstIndexB = std::min(std::max(m_ranges.maxConstIndexB, num + 1), m_layout.boolCount);
            }
            break;
        default:
            break;
        }
    }

    ShaderConstantRanges finish() {
        m_ranges.valid = true;
        return m_ranges;
    }

private:
    static std::uint32_t floatIndex(std::uint32_t type, std::uint32_t num) {
        switch (type) {
        case kRegConst2: return num + 2048;
        case kRegConst3: return num + 4096;
        case kRegConst4: return num + 6144;
        default: return num;
        }
    }

    ConstantLayout m_layout;
    std::vector<bool> m_definedF, m_definedI, m_definedB;
    ShaderConstantRanges m_ranges;
};

} // namespace

ShaderConstantRanges analyzeVertexShader(std::span<const std::uint32_t> tokens, const ConstantLayout& layout) {
    if (tokens.empty() || (tokens[0] >> 16) != 0xFFFEu) {
        return {};
    }
    const std::uint32_t major = (tokens[0] >> 8) & 0xFFu;
    Bookkeeping book(layout);
    std::size_t i = 1;
    while (i < tokens.size()) {
        const std::uint32_t token = tokens[i];
        const std::uint32_t op = token & 0xFFFFu;
        if (op == kEnd) {
            return book.finish();
        }
        if (op == kComment) {
            i += 1 + ((token >> 16) & 0x7FFFu);
            continue;
        }
        // Parameter tokens of this instruction: [first, last).
        std::size_t first = i + 1, last = first;
        if (major >= 2) {
            last = first + ((token >> 24) & 0xFu);
        } else if (op == kDef) {
            last = first + 5;
        } else {
            while (last < tokens.size() && (tokens[last] & kParamBit) != 0) {
                ++last;
            }
        }
        if (last > tokens.size()) {
            return {};
        }
        i = last;

        if (op == kDef || op == kDefI || op == kDefB) {
            if (first < last) {
                book.define(registerType(tokens[first]), registerNumber(tokens[first]));
            }
            continue;
        }
        if (op == kDcl) {
            continue;
        }
        std::size_t p = first;
        if (hasDestination(op) && p < last) {
            const std::uint32_t dst = tokens[p++];
            if ((dst & kRelativeBit) != 0 && major >= 3) {
                ++p; // relative address token of an indexed output (o[aL])
            }
            if ((token & kPredicatedBit) != 0) {
                ++p; // predicate register
            }
        }
        const std::uint32_t loaded = loadedSources(op);
        for (std::uint32_t s = 0; p < last; ++s) {
            const std::uint32_t src = tokens[p++];
            const bool relative = (src & kRelativeBit) != 0;
            if (relative && major >= 2) {
                ++p; // the relative address register token
            }
            if (s < 32 && (loaded >> s & 1u) != 0) {
                const std::uint32_t rows = s == 1 ? matrixRows(op) : 1;
                for (std::uint32_t r = 0; r < rows; ++r) {
                    book.load(registerType(src), registerNumber(src) + r, relative);
                }
            }
        }
    }
    return {}; // no D3DSIO_END
}

hash::Hash64 hashVertexShader(std::span<const std::uint8_t> bytecode, const ShaderConstantRanges& ranges,
                              const void* floatConstants, const void* intConstants, const void* boolConstants) noexcept {
    return hash::hashVertexShader(bytecode, floatConstants, ranges.maxConstIndexF, intConstants, ranges.maxConstIndexI,
                                  boolConstants, ranges.maxConstIndexB);
}

std::optional<hash::Hash64> vertexShaderHash(const tap::DrawState& state) {
    const tap::ShaderRef& vs = state.vertexShader;
    if (vs.id == tap::kNoResource || !vs.tokens || vs.byteSize < 4) {
        return std::nullopt;
    }
    // D3D9DeviceEx::UseProgrammableVS(): a declaration with POSITIONT draws through fixed function.
    for (std::uint32_t e = 0; e < state.elementCount && e < tap::kMaxVertexElements; ++e) {
        if (state.elements[e].usage == 9) { // D3DDECLUSAGE_POSITIONT
            return std::nullopt;
        }
    }
    const ShaderConstantRanges ranges = analyzeVertexShader(std::span(vs.tokens, vs.byteSize / 4),
                                                            ConstantLayout::forVertexShaders(state.softwareVertexProcessing));
    if (!ranges.valid) {
        return std::nullopt;
    }
    // The hash reads maxConstIndexB * 4 / 32 bytes of the bool words.
    const std::uint32_t boolBytes = ranges.maxConstIndexB * 4 / 32;
    if ((ranges.maxConstIndexF && (!state.vsConstF || state.vsConstFCount < ranges.maxConstIndexF)) ||
        (ranges.maxConstIndexI && (!state.vsConstI || state.vsConstICount < ranges.maxConstIndexI)) ||
        (boolBytes && (!state.vsConstB || state.vsConstBCount < ranges.maxConstIndexB))) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(vs.tokens);
    return hashVertexShader(std::span(bytes, vs.byteSize), ranges, state.vsConstF, state.vsConstI, state.vsConstB);
}

std::function<std::optional<hash::Hash64>(const tap::DrawState&)> vertexShaderHashHook() {
    return [](const tap::DrawState& state) { return vertexShaderHash(state); };
}

} // namespace fuse::relight::capture::vertex_capture
