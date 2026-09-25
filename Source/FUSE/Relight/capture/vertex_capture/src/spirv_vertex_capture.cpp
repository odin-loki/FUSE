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
// Ported from dxvk-remix src/dxso/dxso_compiler.cpp@0867d3c (emitVertexCaptureInit,
// emitVertexCaptureOp, emitVertexCaptureWrite)

// FUSE Relight RL-1.6: vertex capture as a SPIR-V transform. See spirv_vertex_capture.hpp.
//
// Modifications (FUSE): a post-pass over finished SPIR-V instead of code emitted by the dxso
// compiler; the clip-space position is stored and back-transformed on the CPU (back_transform.hpp)
// instead of transformed in the shader; the base vertex comes from the region header instead of a
// constant buffer; a bounds check guards every store; the normal is stored untransformed.
#include <fuse/relight/capture/vertex_capture/spirv_vertex_capture.hpp>

#include <fuse/relight/capture/vertex_capture/capture_layout.hpp>

#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>

#include <array>
#include <cstring>
#include <map>
#include <set>
#include <optional>
#include <utility>

namespace fuse::relight::capture::vertex_capture {

std::string_view spirvCaptureStatusName(SpirvCaptureStatus status) {
    switch (status) {
    case SpirvCaptureStatus::Transformed: return "transformed";
    case SpirvCaptureStatus::Malformed: return "malformed";
    case SpirvCaptureStatus::NoVertexEntryPoint: return "no_vertex_entry_point";
    case SpirvCaptureStatus::NoPosition: return "no_position";
    case SpirvCaptureStatus::Unsupported: return "unsupported";
    case SpirvCaptureStatus::AlreadyTransformed: return "already_transformed";
    }
    return "?";
}

namespace {

using Words = std::vector<std::uint32_t>;

struct Inst {
    Words w; // w[0] = word count << 16 | opcode

    [[nodiscard]] std::uint32_t op() const { return w[0] & 0xFFFFu; }
    [[nodiscard]] std::uint32_t operand(std::size_t i) const { return i + 1 < w.size() ? w[i + 1] : 0; }
    [[nodiscard]] std::size_t operandCount() const { return w.size() - 1; }
};

Inst make(spv::Op op, std::initializer_list<std::uint32_t> operands) {
    Inst i;
    i.w.reserve(operands.size() + 1);
    i.w.push_back((std::uint32_t(operands.size() + 1) << 16) | std::uint32_t(op));
    i.w.insert(i.w.end(), operands.begin(), operands.end());
    return i;
}

Inst makeWords(spv::Op op, const Words& operands) {
    Inst i;
    i.w.push_back((std::uint32_t(operands.size() + 1) << 16) | std::uint32_t(op));
    i.w.insert(i.w.end(), operands.begin(), operands.end());
    return i;
}

void appendString(Words& out, std::string_view s) {
    const std::size_t words = s.size() / 4 + 1;
    const std::size_t start = out.size();
    out.resize(start + words, 0);
    std::memcpy(out.data() + start, s.data(), s.size());
}

/// Decodes a literal string starting at operand `first`; returns the string and the operand index
/// after it.
std::pair<std::string, std::size_t> readString(const Inst& inst, std::size_t first) {
    std::string s;
    std::size_t i = first;
    for (; i < inst.operandCount(); ++i) {
        const std::uint32_t word = inst.operand(i);
        bool end = false;
        for (int b = 0; b < 4; ++b) {
            const char c = char((word >> (8 * b)) & 0xFFu);
            if (c == 0) {
                end = true;
                break;
            }
            s.push_back(c);
        }
        if (end) {
            return {s, i + 1};
        }
    }
    return {s, i};
}

bool isDebugOrPreamble(std::uint32_t op) {
    switch (op) {
    case spv::OpCapability: case spv::OpExtension: case spv::OpExtInstImport: case spv::OpMemoryModel:
    case spv::OpEntryPoint: case spv::OpExecutionMode: case spv::OpExecutionModeId: case spv::OpString:
    case spv::OpSourceExtension: case spv::OpSource: case spv::OpSourceContinued: case spv::OpName:
    case spv::OpMemberName:
        return true;
    default:
        return false;
    }
}

bool isAnnotation(std::uint32_t op) {
    switch (op) {
    case spv::OpModuleProcessed: case spv::OpDecorate: case spv::OpMemberDecorate: case spv::OpDecorationGroup:
    case spv::OpGroupDecorate: case spv::OpGroupMemberDecorate: case spv::OpDecorateId: case spv::OpDecorateString:
    case spv::OpMemberDecorateString:
        return true;
    default:
        return false;
    }
}

struct PointerInfo {
    std::uint32_t storage = 0;
    std::uint32_t pointee = 0;
};

struct VectorInfo {
    std::uint32_t component = 0;
    std::uint32_t count = 0;
};

/// One component of a location's value: which variable and which of its components.
struct ComponentSource {
    std::uint32_t var = 0;      ///< 0 = not written by the shader
    std::uint32_t index = 0;    ///< component inside the variable
    std::uint32_t varWidth = 0; ///< 1 = scalar variable
    std::uint32_t varType = 0;  ///< the variable's value type
};
using LocationSources = std::array<ComponentSource, 4>;

class Transformer {
public:
    Transformer(std::span<const std::uint32_t> spirv, const SpirvCaptureOptions& options)
        : m_input(spirv), m_options(options) {}

    SpirvCaptureResult run();

private:
    SpirvCaptureResult fail(SpirvCaptureStatus status, std::string detail) {
        SpirvCaptureResult r;
        r.status = status;
        r.detail = std::move(detail);
        return r;
    }

    bool parse();
    void scan();

    std::uint32_t newId() { return m_bound++; }

    // ---- types and constants (reused when the module has them, else appended) -------------------
    std::uint32_t typeInt(bool isSigned);
    std::uint32_t typeFloat();
    std::uint32_t typeBool();
    std::uint32_t typeVector(std::uint32_t count);
    std::uint32_t typePointer(std::uint32_t storage, std::uint32_t pointee);
    std::uint32_t constUint(std::uint32_t value);
    std::uint32_t constFloat(float value);
    std::uint32_t glslImport();

    std::optional<LocationSources> outputLocation(std::uint32_t location) const;
    std::optional<LocationSources> inputNormal() const;

    /// Emits the loads for `sources` into `code` and returns a float vector of `count` components
    /// (missing components are `fill`).
    std::uint32_t compose(Words& code, const LocationSources& sources, std::uint32_t count, float fill);
    void emitCapture(Words& code);

    std::span<const std::uint32_t> m_input;
    SpirvCaptureOptions m_options;
    std::uint32_t m_version = 0, m_generator = 0, m_bound = 0, m_schema = 0;
    std::vector<Inst> m_insts;

    // module facts
    std::uint32_t m_entryIndex = 0; // index of the vertex OpEntryPoint
    std::uint32_t m_entryFunction = 0;
    std::map<std::uint32_t, std::string> m_names;
    std::map<std::uint32_t, std::uint32_t> m_builtin, m_location, m_component;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> m_memberBuiltin;
    std::map<std::uint32_t, PointerInfo> m_pointers;       // pointer type -> info
    std::map<std::uint32_t, VectorInfo> m_vectors;         // vector type -> info
    std::map<std::uint32_t, std::uint32_t> m_floatWidth;   // float type -> width
    std::map<std::uint32_t, std::pair<std::uint32_t, bool>> m_ints; // int type -> width, signed
    std::map<std::uint32_t, std::uint32_t> m_variables;    // global variable -> pointer type
    std::set<std::uint32_t> m_written; // global variables the module stores to (directly or through access chains)
    std::uint32_t m_boolType = 0;
    std::uint32_t m_glsl = 0;
    bool m_hasCaptureName = false;

    // additions
    std::vector<Inst> m_newPreamble;    // OpExtension / OpExtInstImport
    std::vector<Inst> m_newNames, m_newAnnotations, m_newGlobals;
    std::map<std::uint32_t, std::uint32_t> m_uintConsts;
    std::map<std::uint32_t, std::uint32_t> m_floatConsts; // bit pattern -> id

    // capture plumbing
    std::uint32_t m_ssbo = 0;
    std::uint32_t m_vertexIndexVar = 0, m_vertexIndexType = 0;
    std::uint32_t m_positionVar = 0, m_positionMember = kNone;
    std::optional<LocationSources> m_texcoord, m_color, m_normal;
    std::uint32_t m_fields = fields::kWritten;
    std::uint32_t m_returns = 0;

    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    static constexpr std::uint32_t kDataMember = 4;
};

bool Transformer::parse() {
    if (m_input.size() < 5 || m_input[0] != spv::MagicNumber) {
        return false;
    }
    m_version = m_input[1];
    m_generator = m_input[2];
    m_bound = m_input[3];
    m_schema = m_input[4];
    std::size_t i = 5;
    while (i < m_input.size()) {
        const std::uint32_t count = m_input[i] >> 16;
        if (count == 0 || i + count > m_input.size()) {
            return false;
        }
        Inst inst;
        inst.w.assign(m_input.begin() + std::ptrdiff_t(i), m_input.begin() + std::ptrdiff_t(i + count));
        m_insts.push_back(std::move(inst));
        i += count;
    }
    return true;
}

void Transformer::scan() {
    for (std::size_t k = 0; k < m_insts.size(); ++k) {
        const Inst& in = m_insts[k];
        switch (in.op()) {
        case spv::OpEntryPoint:
            if (in.operand(0) == spv::ExecutionModelVertex && m_entryFunction == 0) {
                m_entryIndex = std::uint32_t(k);
                m_entryFunction = in.operand(1);
            }
            break;
        case spv::OpExtInstImport:
            if (readString(in, 1).first == "GLSL.std.450") {
                m_glsl = in.operand(0);
            }
            break;
        case spv::OpName: {
            std::string name = readString(in, 1).first;
            if (name == kCaptureBufferName) {
                m_hasCaptureName = true;
            }
            m_names[in.operand(0)] = std::move(name);
            break;
        }
        case spv::OpDecorate:
            if (in.operand(1) == spv::DecorationBuiltIn) {
                m_builtin[in.operand(0)] = in.operand(2);
            } else if (in.operand(1) == spv::DecorationLocation) {
                m_location[in.operand(0)] = in.operand(2);
            } else if (in.operand(1) == spv::DecorationComponent) {
                m_component[in.operand(0)] = in.operand(2);
            }
            break;
        case spv::OpMemberDecorate:
            if (in.operand(2) == spv::DecorationBuiltIn) {
                m_memberBuiltin[{in.operand(0), in.operand(1)}] = in.operand(3);
            }
            break;
        case spv::OpTypeBool:
            m_boolType = in.operand(0);
            break;
        case spv::OpTypeInt:
            m_ints[in.operand(0)] = {in.operand(1), in.operand(2) != 0};
            break;
        case spv::OpTypeFloat:
            if (in.operandCount() == 2) { // no floating-point encoding operand (bfloat16 etc.)
                m_floatWidth[in.operand(0)] = in.operand(1);
            }
            break;
        case spv::OpTypeVector:
            m_vectors[in.operand(0)] = {in.operand(1), in.operand(2)};
            break;
        case spv::OpTypePointer:
            m_pointers[in.operand(0)] = {in.operand(1), in.operand(2)};
            break;
        case spv::OpVariable:
            if (in.operand(2) != spv::StorageClassFunction) {
                m_variables[in.operand(1)] = in.operand(0);
            }
            break;
        default:
            break;
        }
    }
    // Outputs the shader really writes. dxbc-spirv declares every fixed-function output of an SM1/2
    // shader (e.g. an "oT12_normal0" at NORMAL0's location) and zero-fills the ones (and the
    // components) the shader does not write, so an output that only ever receives zero constants
    // is treated as absent, as Remix's dxso compiler (which declares only the written registers)
    // would see it. (A shader that writes a literal zero to COLOR0 thus captures the default white.)
    std::map<std::uint32_t, std::uint32_t> root; // access chain result -> base variable
    std::set<std::uint32_t> zero;                // constants whose every bit is 0
    for (const Inst& in : m_insts) {
        const std::uint32_t op = in.op();
        if (op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain || op == spv::OpPtrAccessChain ||
            op == spv::OpInBoundsPtrAccessChain) {
            const auto base = root.find(in.operand(2));
            root[in.operand(1)] = base == root.end() ? in.operand(2) : base->second;
        } else if (op == spv::OpConstantNull || op == spv::OpConstantFalse) {
            zero.insert(in.operand(1));
        } else if (op == spv::OpConstant) {
            bool allZero = true;
            for (std::size_t k = 2; k < in.operandCount(); ++k) {
                allZero &= in.operand(k) == 0;
            }
            if (allZero) {
                zero.insert(in.operand(1));
            }
        } else if (op == spv::OpConstantComposite) {
            bool allZero = true;
            for (std::size_t k = 2; k < in.operandCount(); ++k) {
                allZero &= zero.count(in.operand(k)) != 0;
            }
            if (allZero) {
                zero.insert(in.operand(1));
            }
        }
    }
    for (const Inst& in : m_insts) {
        const std::uint32_t op = in.op();
        const bool store = op == spv::OpStore && zero.count(in.operand(1)) == 0;
        if (store || op == spv::OpCopyMemory || op == spv::OpCopyMemorySized) {
            const auto base = root.find(in.operand(0));
            m_written.insert(base == root.end() ? in.operand(0) : base->second);
        }
    }
}

std::uint32_t Transformer::typeInt(bool isSigned) {
    for (const auto& [id, info] : m_ints) {
        if (info.first == 32 && info.second == isSigned) {
            return id;
        }
    }
    const std::uint32_t id = newId();
    m_newGlobals.push_back(make(spv::OpTypeInt, {id, 32, isSigned ? 1u : 0u}));
    m_ints[id] = {32, isSigned};
    return id;
}

std::uint32_t Transformer::typeFloat() {
    for (const auto& [id, width] : m_floatWidth) {
        if (width == 32) {
            return id;
        }
    }
    const std::uint32_t id = newId();
    m_newGlobals.push_back(make(spv::OpTypeFloat, {id, 32}));
    m_floatWidth[id] = 32;
    return id;
}

std::uint32_t Transformer::typeBool() {
    if (m_boolType == 0) {
        m_boolType = newId();
        m_newGlobals.push_back(make(spv::OpTypeBool, {m_boolType}));
    }
    return m_boolType;
}

std::uint32_t Transformer::typeVector(std::uint32_t count) {
    const std::uint32_t f32 = typeFloat();
    for (const auto& [id, info] : m_vectors) {
        if (info.component == f32 && info.count == count) {
            return id;
        }
    }
    const std::uint32_t id = newId();
    m_newGlobals.push_back(make(spv::OpTypeVector, {id, f32, count}));
    m_vectors[id] = {f32, count};
    return id;
}

std::uint32_t Transformer::typePointer(std::uint32_t storage, std::uint32_t pointee) {
    for (const auto& [id, info] : m_pointers) {
        if (info.storage == storage && info.pointee == pointee) {
            return id;
        }
    }
    const std::uint32_t id = newId();
    m_newGlobals.push_back(make(spv::OpTypePointer, {id, storage, pointee}));
    m_pointers[id] = {storage, pointee};
    return id;
}

std::uint32_t Transformer::constUint(std::uint32_t value) {
    if (auto it = m_uintConsts.find(value); it != m_uintConsts.end()) {
        return it->second;
    }
    const std::uint32_t type = typeInt(false);
    const std::uint32_t id = newId();
    m_newGlobals.push_back(make(spv::OpConstant, {type, id, value}));
    m_uintConsts[value] = id;
    return id;
}

std::uint32_t Transformer::constFloat(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    if (auto it = m_floatConsts.find(bits); it != m_floatConsts.end()) {
        return it->second;
    }
    const std::uint32_t type = typeFloat();
    const std::uint32_t id = newId();
    m_newGlobals.push_back(make(spv::OpConstant, {type, id, bits}));
    m_floatConsts[bits] = id;
    return id;
}

std::uint32_t Transformer::glslImport() {
    if (m_glsl == 0) {
        m_glsl = newId();
        Words ops{m_glsl};
        appendString(ops, "GLSL.std.450");
        m_newPreamble.push_back(makeWords(spv::OpExtInstImport, ops));
    }
    return m_glsl;
}

std::optional<LocationSources> Transformer::outputLocation(std::uint32_t location) const {
    LocationSources sources{};
    bool any = false;
    for (const auto& [var, ptrType] : m_variables) {
        const auto ptr = m_pointers.find(ptrType);
        if (ptr == m_pointers.end() || ptr->second.storage != spv::StorageClassOutput || m_builtin.count(var)) {
            continue;
        }
        const auto loc = m_location.find(var);
        if (loc == m_location.end() || loc->second != location || m_written.count(var) == 0) {
            continue;
        }
        const std::uint32_t type = ptr->second.pointee;
        std::uint32_t width = 0;
        if (auto f = m_floatWidth.find(type); f != m_floatWidth.end() && f->second == 32) {
            width = 1;
        } else if (auto v = m_vectors.find(type); v != m_vectors.end()) {
            auto c = m_floatWidth.find(v->second.component);
            if (c != m_floatWidth.end() && c->second == 32) {
                width = v->second.count;
            }
        }
        if (width == 0) {
            continue; // not float32 (arrays, integers, halves): not captured
        }
        const auto comp = m_component.find(var);
        const std::uint32_t first = comp == m_component.end() ? 0 : comp->second;
        for (std::uint32_t c = 0; c < width && first + c < 4; ++c) {
            sources[first + c] = ComponentSource{var, c, width, type};
            any = true;
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return sources;
}

std::optional<LocationSources> Transformer::inputNormal() const {
    for (const auto& [var, ptrType] : m_variables) {
        const auto ptr = m_pointers.find(ptrType);
        if (ptr == m_pointers.end() || ptr->second.storage != spv::StorageClassInput || m_builtin.count(var)) {
            continue;
        }
        bool match = false;
        if (m_options.inputNormalLocation != SpirvCaptureOptions::kNoLocation) {
            const auto loc = m_location.find(var);
            match = loc != m_location.end() && loc->second == m_options.inputNormalLocation;
        } else if (const auto name = m_names.find(var); name != m_names.end()) {
            const std::string& n = name->second;
            static constexpr std::string_view kSuffix = "_normal0";
            match = n.size() > kSuffix.size() && n.compare(n.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
        }
        if (!match) {
            continue;
        }
        const std::uint32_t type = ptr->second.pointee;
        std::uint32_t width = 0;
        if (auto v = m_vectors.find(type); v != m_vectors.end()) {
            auto c = m_floatWidth.find(v->second.component);
            if (c != m_floatWidth.end() && c->second == 32) {
                width = v->second.count;
            }
        }
        if (width == 0) {
            continue;
        }
        LocationSources sources{};
        for (std::uint32_t c = 0; c < width && c < 4; ++c) {
            sources[c] = ComponentSource{var, c, width, type};
        }
        return sources;
    }
    return std::nullopt;
}

std::uint32_t Transformer::compose(Words& code, const LocationSources& sources, std::uint32_t count, float fill) {
    const std::uint32_t f32 = typeFloat();
    std::map<std::uint32_t, std::uint32_t> loaded; // var -> loaded value
    std::array<std::uint32_t, 4> parts{};
    auto emit = [&](const Inst& i) { code.insert(code.end(), i.w.begin(), i.w.end()); };
    for (std::uint32_t c = 0; c < count; ++c) {
        const ComponentSource& s = sources[c];
        if (s.var == 0) {
            parts[c] = constFloat(fill);
            continue;
        }
        auto it = loaded.find(s.var);
        if (it == loaded.end()) {
            const std::uint32_t value = newId();
            emit(make(spv::OpLoad, {s.varType, value, s.var}));
            it = loaded.emplace(s.var, value).first;
        }
        if (s.varWidth == 1) {
            parts[c] = it->second;
        } else {
            const std::uint32_t scalar = newId();
            emit(make(spv::OpCompositeExtract, {f32, scalar, it->second, s.index}));
            parts[c] = scalar;
        }
    }
    const std::uint32_t result = newId();
    Words ops{typeVector(count), result};
    ops.insert(ops.end(), parts.begin(), parts.begin() + count);
    emit(makeWords(spv::OpCompositeConstruct, ops));
    return result;
}

void Transformer::emitCapture(Words& code) {
    auto emit = [&](const Inst& i) { code.insert(code.end(), i.w.begin(), i.w.end()); };
    const std::uint32_t tInt = typeInt(true), tUint = typeInt(false), tBool = typeBool(), tFloat = typeFloat();
    const std::uint32_t tVec2 = typeVector(2), tVec3 = typeVector(3), tVec4 = typeVector(4);
    const std::uint32_t sb = spv::StorageClassStorageBuffer;

    // rel = gl_VertexIndex - header.baseVertex; captured when rel < header.vertexCount (unsigned).
    std::uint32_t vi = newId();
    emit(make(spv::OpLoad, {m_vertexIndexType, vi, m_vertexIndexVar}));
    if (m_vertexIndexType != tInt) {
        const std::uint32_t cast = newId();
        emit(make(spv::OpBitcast, {tInt, cast, vi}));
        vi = cast;
    }
    const std::uint32_t pBase = newId(), base = newId(), pCount = newId(), count = newId();
    emit(make(spv::OpAccessChain, {typePointer(sb, tInt), pBase, m_ssbo, constUint(0)}));
    emit(make(spv::OpLoad, {tInt, base, pBase}));
    emit(make(spv::OpAccessChain, {typePointer(sb, tUint), pCount, m_ssbo, constUint(1)}));
    emit(make(spv::OpLoad, {tUint, count, pCount}));
    const std::uint32_t rel = newId(), relU = newId(), inRange = newId();
    emit(make(spv::OpISub, {tInt, rel, vi, base}));
    emit(make(spv::OpBitcast, {tUint, relU, rel}));
    emit(make(spv::OpULessThan, {tBool, inRange, relU, count}));
    const std::uint32_t storeLabel = newId(), mergeLabel = newId();
    emit(make(spv::OpSelectionMerge, {mergeLabel, spv::SelectionControlMaskNone}));
    emit(make(spv::OpBranchConditional, {inRange, storeLabel, mergeLabel}));
    emit(make(spv::OpLabel, {storeLabel}));

    auto member = [&](std::uint32_t index, std::uint32_t valueType, std::uint32_t value) {
        const std::uint32_t ptr = newId();
        emit(make(spv::OpAccessChain,
                  {typePointer(sb, valueType), ptr, m_ssbo, constUint(kDataMember), relU, constUint(index)}));
        emit(make(spv::OpStore, {ptr, value}));
    };

    // clip-space position (Remix: oPos, back-transformed on the CPU here)
    std::uint32_t posPtr = m_positionVar;
    if (m_positionMember != kNone) {
        posPtr = newId();
        emit(make(spv::OpAccessChain,
                  {typePointer(spv::StorageClassOutput, tVec4), posPtr, m_positionVar, constUint(m_positionMember)}));
    }
    const std::uint32_t pos = newId();
    emit(make(spv::OpLoad, {tVec4, pos, posPtr}));
    member(0, tVec4, pos);

    // TEXCOORD0.xy, (0, 0) without one (Remix stores constvec2f32(0, 0))
    std::uint32_t tex = 0;
    if (m_texcoord) {
        tex = compose(code, *m_texcoord, 2, 0.0f);
    } else {
        tex = newId();
        emit(make(spv::OpCompositeConstruct, {tVec2, tex, constFloat(0.0f), constFloat(0.0f)}));
    }
    member(1, tVec2, tex);
    member(2, tUint, constUint(m_fields));

    // NORMAL0.xyz (output, else input), untransformed
    std::uint32_t normal = 0;
    if (m_normal) {
        normal = compose(code, *m_normal, 3, 0.0f);
    } else {
        normal = newId();
        emit(make(spv::OpCompositeConstruct, {tVec3, normal, constFloat(0.0f), constFloat(0.0f), constFloat(0.0f)}));
    }
    member(4, tVec3, normal);

    // COLOR0 as 0xAARRGGBB: FMin(FMax(c, 0), 1) * 255 + 0.5 -> uint (emitVertexCaptureOp)
    std::uint32_t color = constUint(0xFFFFFFFFu);
    if (m_color) {
        const std::uint32_t rgba = compose(code, *m_color, 4, 0.0f);
        const std::uint32_t glsl = glslImport();
        std::array<std::uint32_t, 4> bytes{};
        for (std::uint32_t c = 0; c < 4; ++c) {
            const std::uint32_t x = newId(), lo = newId(), hi = newId(), scaled = newId(), biased = newId(),
                                u = newId();
            emit(make(spv::OpCompositeExtract, {tFloat, x, rgba, c}));
            emit(make(spv::OpExtInst, {tFloat, lo, glsl, GLSLstd450FMax, x, constFloat(0.0f)}));
            emit(make(spv::OpExtInst, {tFloat, hi, glsl, GLSLstd450FMin, lo, constFloat(1.0f)}));
            emit(make(spv::OpFMul, {tFloat, scaled, hi, constFloat(255.0f)}));
            emit(make(spv::OpFAdd, {tFloat, biased, scaled, constFloat(0.5f)}));
            emit(make(spv::OpConvertFToU, {tUint, u, biased}));
            bytes[c] = u;
        }
        auto shl = [&](std::uint32_t v, std::uint32_t s) {
            const std::uint32_t r = newId();
            emit(make(spv::OpShiftLeftLogical, {tUint, r, v, constUint(s)}));
            return r;
        };
        auto bor = [&](std::uint32_t a, std::uint32_t b) {
            const std::uint32_t r = newId();
            emit(make(spv::OpBitwiseOr, {tUint, r, a, b}));
            return r;
        };
        color = bor(bor(shl(bytes[3], 24), shl(bytes[0], 16)), bor(shl(bytes[1], 8), bytes[2]));
    }
    member(5, tUint, color);

    emit(make(spv::OpBranch, {mergeLabel}));
    emit(make(spv::OpLabel, {mergeLabel}));
}

SpirvCaptureResult Transformer::run() {
    if (!parse()) {
        return fail(SpirvCaptureStatus::Malformed, "not a SPIR-V module or truncated instruction stream");
    }
    scan();
    if (m_hasCaptureName) {
        return fail(SpirvCaptureStatus::AlreadyTransformed, "the module already has the capture buffer");
    }
    if (m_entryFunction == 0) {
        return fail(SpirvCaptureStatus::NoVertexEntryPoint, "no OpEntryPoint Vertex");
    }

    // gl_Position: a BuiltIn Position variable, or a member of an output block.
    for (const auto& [var, ptrType] : m_variables) {
        const auto ptr = m_pointers.find(ptrType);
        if (ptr == m_pointers.end() || ptr->second.storage != spv::StorageClassOutput) {
            continue;
        }
        if (auto b = m_builtin.find(var); b != m_builtin.end() && b->second == spv::BuiltInPosition) {
            m_positionVar = var;
            if (auto v = m_vectors.find(ptr->second.pointee);
                v == m_vectors.end() || v->second.count != 4 || m_floatWidth.count(v->second.component) == 0 ||
                m_floatWidth.at(v->second.component) != 32) {
                return fail(SpirvCaptureStatus::Unsupported, "gl_Position is not a float32 vec4");
            }
            break;
        }
        for (const auto& [key, builtin] : m_memberBuiltin) {
            if (key.first == ptr->second.pointee && builtin == spv::BuiltInPosition) {
                m_positionVar = var;
                m_positionMember = key.second;
            }
        }
        if (m_positionVar) {
            break;
        }
    }
    if (m_positionVar == 0) {
        return fail(SpirvCaptureStatus::NoPosition, "no BuiltIn Position output");
    }
    m_texcoord = outputLocation(m_options.texcoord0Location);
    m_color = outputLocation(m_options.color0Location);
    m_normal = outputLocation(m_options.normal0Location);
    if (m_texcoord) {
        m_fields |= fields::kTexcoord;
    }
    if (m_color) {
        m_fields |= fields::kColor;
    }
    if (m_normal) {
        m_fields |= fields::kNormalOutput;
    } else if (m_options.captureInputNormal) {
        m_normal = inputNormal();
        if (m_normal) {
            m_fields |= fields::kNormalInput;
        }
    }

    // The entry point's function: every OpReturn is instrumented, OpReturnValue is rejected.
    std::size_t fnBegin = 0, fnEnd = 0;
    for (std::size_t k = 0; k < m_insts.size(); ++k) {
        if (m_insts[k].op() == spv::OpFunction && m_insts[k].operand(1) == m_entryFunction) {
            fnBegin = k;
            for (std::size_t e = k; e < m_insts.size(); ++e) {
                if (m_insts[e].op() == spv::OpFunctionEnd) {
                    fnEnd = e;
                    break;
                }
            }
            break;
        }
    }
    if (fnEnd == 0) {
        return fail(SpirvCaptureStatus::Malformed, "the entry point's function is missing");
    }
    for (std::size_t k = fnBegin; k < fnEnd; ++k) {
        if (m_insts[k].op() == spv::OpReturnValue) {
            return fail(SpirvCaptureStatus::Unsupported, "OpReturnValue in the entry point");
        }
        if (m_insts[k].op() == spv::OpReturn) {
            ++m_returns;
        }
    }

    // ---- declarations -------------------------------------------------------------------------------
    const std::uint32_t tInt = typeInt(true), tUint = typeInt(false);
    const std::uint32_t tVec2 = typeVector(2), tVec3 = typeVector(3), tVec4 = typeVector(4);
    const std::uint32_t vertexType = newId(), arrayType = newId(), blockType = newId();
    m_newGlobals.push_back(make(spv::OpTypeStruct, {vertexType, tVec4, tVec2, tUint, tUint, tVec3, tUint}));
    m_newGlobals.push_back(make(spv::OpTypeRuntimeArray, {arrayType, vertexType}));
    m_newGlobals.push_back(make(spv::OpTypeStruct, {blockType, tInt, tUint, tUint, tUint, arrayType}));
    const std::uint32_t blockPtr = typePointer(spv::StorageClassStorageBuffer, blockType);
    m_ssbo = newId();
    m_newGlobals.push_back(make(spv::OpVariable, {blockPtr, m_ssbo, spv::StorageClassStorageBuffer}));

    const std::uint32_t vertexOffsets[] = {0, 16, 24, 28, 32, 44};
    for (std::uint32_t m = 0; m < 6; ++m) {
        m_newAnnotations.push_back(make(spv::OpMemberDecorate, {vertexType, m, spv::DecorationOffset, vertexOffsets[m]}));
    }
    m_newAnnotations.push_back(make(spv::OpDecorate, {arrayType, spv::DecorationArrayStride, kCapturedVertexSize}));
    const std::uint32_t blockOffsets[] = {0, 4, 8, 12, kRegionHeaderSize};
    for (std::uint32_t m = 0; m < 5; ++m) {
        m_newAnnotations.push_back(make(spv::OpMemberDecorate, {blockType, m, spv::DecorationOffset, blockOffsets[m]}));
    }
    m_newAnnotations.push_back(make(spv::OpDecorate, {blockType, spv::DecorationBlock}));
    m_newAnnotations.push_back(make(spv::OpDecorate, {m_ssbo, spv::DecorationDescriptorSet, m_options.descriptorSet}));
    m_newAnnotations.push_back(make(spv::OpDecorate, {m_ssbo, spv::DecorationBinding, m_options.binding}));
    auto name = [&](std::uint32_t id, std::string_view text) {
        Words ops{id};
        appendString(ops, text);
        m_newNames.push_back(makeWords(spv::OpName, ops));
    };
    name(vertexType, "FuseCapturedVertex");
    name(blockType, "FuseVertexCaptureRegion");
    name(m_ssbo, kCaptureBufferName);

    // gl_VertexIndex (existing, else a new signed one).
    for (const auto& [var, ptrType] : m_variables) {
        const auto b = m_builtin.find(var);
        const auto ptr = m_pointers.find(ptrType);
        if (b != m_builtin.end() && b->second == spv::BuiltInVertexIndex && ptr != m_pointers.end()) {
            const auto intInfo = m_ints.find(ptr->second.pointee);
            if (intInfo == m_ints.end() || intInfo->second.first != 32) {
                return fail(SpirvCaptureStatus::Unsupported, "gl_VertexIndex is not a 32-bit integer");
            }
            m_vertexIndexVar = var;
            m_vertexIndexType = ptr->second.pointee;
            break;
        }
    }
    if (m_vertexIndexVar == 0) {
        m_vertexIndexVar = newId();
        m_vertexIndexType = tInt;
        m_newGlobals.push_back(
            make(spv::OpVariable, {typePointer(spv::StorageClassInput, tInt), m_vertexIndexVar, spv::StorageClassInput}));
        m_newAnnotations.push_back(make(spv::OpDecorate, {m_vertexIndexVar, spv::DecorationBuiltIn, spv::BuiltInVertexIndex}));
        name(m_vertexIndexVar, "fuse_vertex_index");
    }

    // ---- the instrumented function (built before the global lists are final: it adds constants) ---
    std::vector<Inst> function;
    for (std::size_t k = fnBegin; k <= fnEnd; ++k) {
        if (m_insts[k].op() != spv::OpReturn) {
            function.push_back(m_insts[k]);
            continue;
        }
        Inst code;
        emitCapture(code.w);
        code.w.push_back((1u << 16) | spv::OpReturn);
        function.push_back(std::move(code)); // several instructions in one entry, written verbatim
    }

    // ---- entry point interface ----------------------------------------------------------------------
    Inst& entry = m_insts[m_entryIndex];
    const std::size_t interfaceStart = readString(entry, 2).second;
    auto listed = [&](std::uint32_t id) {
        for (std::size_t k = interfaceStart; k < entry.operandCount(); ++k) {
            if (entry.operand(k) == id) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::uint32_t> add;
    if (!listed(m_vertexIndexVar)) {
        add.push_back(m_vertexIndexVar);
    }
    if (m_version >= 0x00010400u) { // SPIR-V 1.4+: every global the entry point uses
        add.push_back(m_ssbo);
    }
    for (std::uint32_t id : add) {
        entry.w.push_back(id);
    }
    entry.w[0] = (std::uint32_t(entry.w.size()) << 16) | spv::OpEntryPoint;

    if (m_version < 0x00010300u) {
        Words ops;
        appendString(ops, "SPV_KHR_storage_buffer_storage_class");
        m_newPreamble.insert(m_newPreamble.begin(), makeWords(spv::OpExtension, ops));
    }

    // ---- assemble ------------------------------------------------------------------------------------
    std::size_t preambleAt = 0, namesAt = 0, annotationsAt = 0, globalsAt = fnBegin;
    for (std::size_t k = 0; k < m_insts.size(); ++k) {
        const std::uint32_t op = m_insts[k].op();
        if (op == spv::OpCapability || op == spv::OpExtension) {
            preambleAt = k + 1;
        }
        if (isDebugOrPreamble(op)) {
            namesAt = k + 1;
        }
        if (isDebugOrPreamble(op) || isAnnotation(op)) {
            annotationsAt = k + 1;
        } else {
            break;
        }
    }
    for (std::size_t k = 0; k < m_insts.size(); ++k) {
        if (m_insts[k].op() == spv::OpFunction) {
            globalsAt = std::min(globalsAt, k);
            break;
        }
    }

    SpirvCaptureResult result;
    Words& out = result.words;
    out.reserve(m_input.size() + 512);
    out.push_back(spv::MagicNumber);
    out.push_back(m_version);
    out.push_back(m_generator);
    out.push_back(0); // bound, patched below
    out.push_back(m_schema);
    auto put = [&](const Inst& i) { out.insert(out.end(), i.w.begin(), i.w.end()); };
    for (std::size_t k = 0; k < m_insts.size(); ++k) {
        if (k == preambleAt) {
            for (const Inst& i : m_newPreamble) {
                put(i);
            }
        }
        if (k == namesAt) {
            for (const Inst& i : m_newNames) {
                put(i);
            }
        }
        if (k == annotationsAt) {
            for (const Inst& i : m_newAnnotations) {
                put(i);
            }
        }
        if (k == globalsAt) {
            for (const Inst& i : m_newGlobals) {
                put(i);
            }
        }
        if (k == fnBegin) {
            for (const Inst& i : function) {
                put(i);
            }
            k = fnEnd;
            continue;
        }
        put(m_insts[k]);
    }
    out[3] = m_bound;
    result.status = SpirvCaptureStatus::Transformed;
    result.fields = m_fields;
    result.returns = m_returns;
    return result;
}

} // namespace

SpirvCaptureResult addVertexCapture(std::span<const std::uint32_t> spirv, const SpirvCaptureOptions& options) {
    Transformer t(spirv, options);
    return t.run();
}

} // namespace fuse::relight::capture::vertex_capture
