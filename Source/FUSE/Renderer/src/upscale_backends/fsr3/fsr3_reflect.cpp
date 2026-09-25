// WP-4.2: minimal SPIR-V reflection (see fsr3_reflect.hpp).
#include <fuse/renderer/upscale_backends/fsr3/fsr3_reflect.hpp>

#include <unordered_map>

namespace fuse::renderer::fsr3 {

namespace {
// SPIR-V opcodes / enums used here (SPIR-V 1.6 unified spec).
constexpr u32 kMagic = 0x07230203u;
constexpr u32 OpName = 5, OpMemberName = 6, OpExecutionMode = 16, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
              OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
              OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59,
              OpDecorate = 71, OpMemberDecorate = 72;
constexpr u32 DecorationBufferBlock = 3, DecorationArrayStride = 6, DecorationBinding = 33,
              DecorationDescriptorSet = 34, DecorationOffset = 35;
constexpr u32 StorageUniformConstant = 0, StorageUniform = 2, StoragePushConstant = 9, StorageStorageBuffer = 12;
constexpr u32 ExecutionModeLocalSize = 17;

std::string literalString(const u32* w, u32 count) {
    std::string s;
    for (u32 i = 0; i < count; ++i) {
        for (u32 b = 0; b < 4u; ++b) {
            const char c = static_cast<char>((w[i] >> (8u * b)) & 0xFFu);
            if (c == '\0') {
                return s;
            }
            s.push_back(c);
        }
    }
    return s;
}

struct Type {
    u32 op = 0;
    u32 a = 0, b = 0, c = 0; ///< operands (component type / count, image fields, pointer storage / pointee)
    u32 sampled = 0;         ///< OpTypeImage Sampled operand
    u32 format = 0;          ///< OpTypeImage format
    std::vector<u32> members;
};
} // namespace

bool reflect_spirv(const u32* words, usize wordCount, SpirvReflection& out) {
    out = SpirvReflection{};
    if (words == nullptr || wordCount < 5u || words[0] != kMagic) {
        return false;
    }
    std::unordered_map<u32, std::string> names;
    std::unordered_map<u64, std::string> memberNames;
    std::unordered_map<u64, u32> memberOffsets;
    std::unordered_map<u32, u32> bindingOf, setOf, arrayStride;
    std::unordered_map<u32, bool> isBufferBlock;
    std::unordered_map<u32, Type> types;
    std::unordered_map<u32, u32> constants;
    struct Var {
        u32 id = 0, pointerType = 0, storage = 0;
    };
    std::vector<Var> vars;

    usize pos = 5;
    while (pos < wordCount) {
        const u32 word = words[pos];
        const u32 op = word & 0xFFFFu;
        const u32 count = word >> 16u;
        if (count == 0u || pos + count > wordCount) {
            return false;
        }
        const u32* w = words + pos;
        switch (op) {
        case OpName:
            if (count >= 3u) {
                names[w[1]] = literalString(w + 2, count - 2u);
            }
            break;
        case OpMemberName:
            if (count >= 4u) {
                memberNames[(static_cast<u64>(w[1]) << 32u) | w[2]] = literalString(w + 3, count - 3u);
            }
            break;
        case OpExecutionMode:
            if (count >= 6u && w[2] == ExecutionModeLocalSize) {
                out.localSize[0] = w[3];
                out.localSize[1] = w[4];
                out.localSize[2] = w[5];
            }
            break;
        case OpDecorate:
            if (count >= 3u) {
                if (w[2] == DecorationBinding && count >= 4u) {
                    bindingOf[w[1]] = w[3];
                } else if (w[2] == DecorationDescriptorSet && count >= 4u) {
                    setOf[w[1]] = w[3];
                } else if (w[2] == DecorationBufferBlock) {
                    isBufferBlock[w[1]] = true;
                } else if (w[2] == DecorationArrayStride && count >= 4u) {
                    arrayStride[w[1]] = w[3];
                }
            }
            break;
        case OpMemberDecorate:
            if (count >= 5u && w[3] == DecorationOffset) {
                memberOffsets[(static_cast<u64>(w[1]) << 32u) | w[2]] = w[4];
            }
            break;
        case OpTypeInt:
        case OpTypeFloat:
            if (count >= 3u) {
                types[w[1]] = Type{op, w[2], 0, 0, 0, 0, {}};
            }
            break;
        case OpTypeVector:
        case OpTypeMatrix:
        case OpTypeArray:
            if (count >= 4u) {
                types[w[1]] = Type{op, w[2], w[3], 0, 0, 0, {}};
            }
            break;
        case OpTypeRuntimeArray:
            if (count >= 3u) {
                types[w[1]] = Type{op, w[2], 0, 0, 0, 0, {}};
            }
            break;
        case OpTypeImage:
            if (count >= 9u) {
                Type t{};
                t.op = op;
                t.sampled = w[7];
                t.format = w[8];
                types[w[1]] = t;
            }
            break;
        case OpTypeSampler:
            if (count >= 2u) {
                types[w[1]] = Type{op, 0, 0, 0, 0, 0, {}};
            }
            break;
        case OpTypeSampledImage:
            if (count >= 3u) {
                types[w[1]] = Type{op, w[2], 0, 0, 0, 0, {}};
            }
            break;
        case OpTypeStruct: {
            Type t{};
            t.op = op;
            for (u32 i = 2; i < count; ++i) {
                t.members.push_back(w[i]);
            }
            types[w[1]] = t;
            break;
        }
        case OpTypePointer:
            if (count >= 4u) {
                types[w[1]] = Type{op, w[2], w[3], 0, 0, 0, {}};
            }
            break;
        case OpConstant:
            if (count >= 4u) {
                constants[w[2]] = w[3];
            }
            break;
        case OpVariable:
            if (count >= 4u) {
                vars.push_back(Var{w[2], w[1], w[3]});
            }
            break;
        default:
            break;
        }
        pos += count;
    }

    // Byte size of a (scalar / vector / matrix / fixed array) type; 0 when unknown.
    auto sizeOf = [&](auto&& self, u32 id) -> u32 {
        const auto it = types.find(id);
        if (it == types.end()) {
            return 0u;
        }
        const Type& t = it->second;
        switch (t.op) {
        case OpTypeInt:
        case OpTypeFloat:
            return t.a / 8u;
        case OpTypeVector:
        case OpTypeMatrix:
            return self(self, t.a) * t.b;
        case OpTypeArray: {
            const auto c = constants.find(t.b);
            const auto s = arrayStride.find(id);
            if (c == constants.end()) {
                return 0u;
            }
            return (s != arrayStride.end() ? s->second : self(self, t.a)) * c->second;
        }
        default:
            return 0u;
        }
    };
    auto membersOf = [&](u32 structId) {
        std::vector<SpirvBlockMember> m;
        const auto it = types.find(structId);
        if (it == types.end() || it->second.op != OpTypeStruct) {
            return m;
        }
        for (u32 i = 0; i < static_cast<u32>(it->second.members.size()); ++i) {
            const u64 key = (static_cast<u64>(structId) << 32u) | i;
            SpirvBlockMember member{};
            const auto n = memberNames.find(key);
            member.name = n != memberNames.end() ? n->second : std::string{};
            const auto o = memberOffsets.find(key);
            member.offset = o != memberOffsets.end() ? o->second : 0u;
            member.size = sizeOf(sizeOf, it->second.members[i]);
            m.push_back(member);
        }
        return m;
    };

    for (const Var& v : vars) {
        const auto pt = types.find(v.pointerType);
        if (pt == types.end() || pt->second.op != OpTypePointer) {
            continue;
        }
        u32 pointee = pt->second.b;
        if (v.storage == StoragePushConstant) {
            out.pushConstants = membersOf(pointee);
            for (const SpirvBlockMember& m : out.pushConstants) {
                if (m.offset + m.size > out.pushConstantBytes) {
                    out.pushConstantBytes = m.offset + m.size;
                }
            }
            continue;
        }
        if (v.storage != StorageUniformConstant && v.storage != StorageUniform && v.storage != StorageStorageBuffer) {
            continue;
        }
        // Arrays of descriptors are not used by these passes; look through one array level anyway.
        auto t = types.find(pointee);
        if (t != types.end() && (t->second.op == OpTypeArray || t->second.op == OpTypeRuntimeArray)) {
            pointee = t->second.a;
            t = types.find(pointee);
        }
        if (t == types.end()) {
            continue;
        }
        SpirvBinding b{};
        const auto n = names.find(v.id);
        b.name = n != names.end() ? n->second : std::string{};
        const auto bs = bindingOf.find(v.id);
        const auto ss = setOf.find(v.id);
        b.binding = bs != bindingOf.end() ? bs->second : 0u;
        b.set = ss != setOf.end() ? ss->second : 0u;
        switch (t->second.op) {
        case OpTypeImage:
            b.kind = t->second.sampled == 2u ? SpirvDescriptorKind::StorageImage : SpirvDescriptorKind::SampledImage;
            b.imageFormat = t->second.format;
            break;
        case OpTypeSampler:
            b.kind = SpirvDescriptorKind::Sampler;
            break;
        case OpTypeSampledImage:
            b.kind = SpirvDescriptorKind::CombinedImageSampler;
            break;
        case OpTypeStruct: {
            const bool bufferBlock = isBufferBlock.count(pointee) != 0u || v.storage == StorageStorageBuffer;
            b.kind = bufferBlock ? SpirvDescriptorKind::StorageBuffer : SpirvDescriptorKind::UniformBuffer;
            const auto tn = names.find(pointee);
            b.typeName = tn != names.end() ? tn->second : std::string{};
            b.members = membersOf(pointee);
            break;
        }
        default:
            break;
        }
        out.bindings.push_back(b);
    }
    out.valid = true;
    return true;
}

} // namespace fuse::renderer::fsr3
