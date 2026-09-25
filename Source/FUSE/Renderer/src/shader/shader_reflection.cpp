#include <fuse/renderer/shader/shader_reflection.hpp>

#include <algorithm>
#include <unordered_map>

namespace fuse::renderer {

namespace {

// SPIR-V opcodes / enums (SPIR-V 1.6 unified spec).
constexpr u32 kOpName = 5;
constexpr u32 kOpEntryPoint = 15;
constexpr u32 kOpExecutionMode = 16;
constexpr u32 kOpTypeInt = 21;
constexpr u32 kOpTypeFloat = 22;
constexpr u32 kOpTypeVector = 23;
constexpr u32 kOpTypeMatrix = 24;
constexpr u32 kOpTypeImage = 25;
constexpr u32 kOpTypeSampler = 26;
constexpr u32 kOpTypeSampledImage = 27;
constexpr u32 kOpTypeArray = 28;
constexpr u32 kOpTypeRuntimeArray = 29;
constexpr u32 kOpTypeStruct = 30;
constexpr u32 kOpTypePointer = 32;
constexpr u32 kOpConstant = 43;
constexpr u32 kOpVariable = 59;
constexpr u32 kOpDecorate = 71;
constexpr u32 kOpMemberDecorate = 72;
constexpr u32 kOpTypeAccelerationStructureKHR = 5341;

constexpr u32 kExecutionModeLocalSize = 17;

constexpr u32 kDecorationBlock = 2;
constexpr u32 kDecorationBufferBlock = 3;
constexpr u32 kDecorationArrayStride = 6;
constexpr u32 kDecorationMatrixStride = 7;
constexpr u32 kDecorationBinding = 33;
constexpr u32 kDecorationDescriptorSet = 34;
constexpr u32 kDecorationOffset = 35;

constexpr u32 kStorageUniformConstant = 0;
constexpr u32 kStorageUniform = 2;
constexpr u32 kStoragePushConstant = 9;
constexpr u32 kStorageStorageBuffer = 12;

constexpr u32 kDimBuffer = 5;

// VkDescriptorType values.
constexpr u32 kDescSampler = 0;
constexpr u32 kDescCombinedImageSampler = 1;
constexpr u32 kDescSampledImage = 2;
constexpr u32 kDescStorageImage = 3;
constexpr u32 kDescUniformTexelBuffer = 4;
constexpr u32 kDescStorageTexelBuffer = 5;
constexpr u32 kDescUniformBuffer = 6;
constexpr u32 kDescStorageBuffer = 7;
constexpr u32 kDescAccelerationStructure = 1000150000u;
constexpr u32 kDescInvalid = 0xFFFFFFFFu;

u32 stageFlagsForExecutionModel(u32 model) {
    switch (model) {
    case 0:
        return 0x1u; // Vertex
    case 1:
        return 0x2u; // TessellationControl
    case 2:
        return 0x4u; // TessellationEvaluation
    case 3:
        return 0x8u; // Geometry
    case 4:
        return 0x10u; // Fragment
    case 5:
        return 0x20u; // GLCompute
    case 5267:        // TaskNV
    case 5364:        // TaskEXT
        return 0x40u;
    case 5268: // MeshNV
    case 5365: // MeshEXT
        return 0x80u;
    case 5313:
        return 0x100u; // RayGeneration
    case 5314:
        return 0x1000u; // Intersection
    case 5315:
        return 0x200u; // AnyHit
    case 5316:
        return 0x400u; // ClosestHit
    case 5317:
        return 0x800u; // Miss
    case 5318:
        return 0x2000u; // Callable
    default:
        return 0u;
    }
}

struct TypeInfo {
    u32 opcode = 0;
    std::vector<u32> operands; // words after the result id
};

struct Decorations {
    bool block = false;
    bool bufferBlock = false;
    bool hasSet = false;
    bool hasBinding = false;
    u32 set = 0;
    u32 binding = 0;
    u32 arrayStride = 0;
};

struct Module {
    std::unordered_map<u32, TypeInfo> types;
    std::unordered_map<u32, u64> constants;
    std::unordered_map<u32, Decorations> decorations;
    std::unordered_map<u64, u32> memberOffsets;       // (struct << 32 | member) -> Offset
    std::unordered_map<u64, u32> memberMatrixStrides; // (struct << 32 | member) -> MatrixStride
    std::unordered_map<u32, std::string> names;

    static u64 memberKey(u32 structId, u32 member) { return (static_cast<u64>(structId) << 32u) | member; }

    const TypeInfo* type(u32 id) const {
        const auto it = types.find(id);
        return it != types.end() ? &it->second : nullptr;
    }
    Decorations decorationsOf(u32 id) const {
        const auto it = decorations.find(id);
        return it != decorations.end() ? it->second : Decorations{};
    }

    /// Byte size of a type as laid out by its explicit Offset/ArrayStride/MatrixStride decorations.
    u32 sizeOf(u32 typeId, u32 depth = 0) const {
        const TypeInfo* t = type(typeId);
        if (t == nullptr || depth > 32u) {
            return 0;
        }
        switch (t->opcode) {
        case kOpTypeInt:
        case kOpTypeFloat:
            return t->operands.empty() ? 0u : t->operands[0] / 8u;
        case kOpTypeVector:
            return t->operands.size() < 2 ? 0u : sizeOf(t->operands[0], depth + 1) * t->operands[1];
        case kOpTypeMatrix:
            return t->operands.size() < 2 ? 0u : sizeOf(t->operands[0], depth + 1) * t->operands[1];
        case kOpTypeArray: {
            if (t->operands.size() < 2) {
                return 0;
            }
            const auto length = constants.find(t->operands[1]);
            const u32 count = length != constants.end() ? static_cast<u32>(length->second) : 0u;
            const u32 stride = decorationsOf(typeId).arrayStride;
            return count * (stride != 0u ? stride : sizeOf(t->operands[0], depth + 1));
        }
        case kOpTypeStruct: {
            u32 size = 0;
            for (u32 m = 0; m < t->operands.size(); ++m) {
                const auto offset = memberOffsets.find(memberKey(typeId, m));
                const u32 memberOffset = offset != memberOffsets.end() ? offset->second : size;
                u32 memberSize = sizeOf(t->operands[m], depth + 1);
                const TypeInfo* memberType = type(t->operands[m]);
                const auto matrixStride = memberMatrixStrides.find(memberKey(typeId, m));
                if (memberType != nullptr && memberType->opcode == kOpTypeMatrix &&
                    matrixStride != memberMatrixStrides.end() && memberType->operands.size() >= 2) {
                    memberSize = matrixStride->second * memberType->operands[1];
                }
                size = std::max(size, memberOffset + memberSize);
            }
            return size;
        }
        default:
            return 0;
        }
    }
};

std::string readString(const u32* words, usize count, usize& consumedWords) {
    std::string text;
    consumedWords = 0;
    for (usize i = 0; i < count; ++i) {
        const u32 word = words[i];
        ++consumedWords;
        for (u32 b = 0; b < 4u; ++b) {
            const char c = static_cast<char>((word >> (b * 8u)) & 0xFFu);
            if (c == '\0') {
                return text;
            }
            text.push_back(c);
        }
    }
    return text;
}

/// Unwraps arrays from `typeId`; returns the element type and sets `count` (0 = runtime array).
u32 unwrapArrays(const Module& module, u32 typeId, u32& count) {
    count = 1;
    for (u32 guard = 0; guard < 8u; ++guard) {
        const TypeInfo* t = module.type(typeId);
        if (t == nullptr || t->operands.empty()) {
            return typeId;
        }
        if (t->opcode == kOpTypeArray && t->operands.size() >= 2) {
            const auto length = module.constants.find(t->operands[1]);
            count *= length != module.constants.end() ? static_cast<u32>(length->second) : 1u;
            typeId = t->operands[0];
        } else if (t->opcode == kOpTypeRuntimeArray) {
            count = 0;
            typeId = t->operands[0];
        } else {
            return typeId;
        }
    }
    return typeId;
}

u32 descriptorTypeFor(const Module& module, u32 storageClass, u32 elementType) {
    const TypeInfo* t = module.type(elementType);
    if (t == nullptr) {
        return kDescInvalid;
    }
    if (storageClass == kStorageStorageBuffer) {
        return kDescStorageBuffer;
    }
    if (storageClass == kStorageUniform) {
        return module.decorationsOf(elementType).bufferBlock ? kDescStorageBuffer : kDescUniformBuffer;
    }
    if (storageClass != kStorageUniformConstant) {
        return kDescInvalid;
    }
    switch (t->opcode) {
    case kOpTypeSampler:
        return kDescSampler;
    case kOpTypeSampledImage:
        return kDescCombinedImageSampler;
    case kOpTypeAccelerationStructureKHR:
        return kDescAccelerationStructure;
    case kOpTypeImage: {
        // operands: sampled type, dim, depth, arrayed, ms, sampled, format
        if (t->operands.size() < 6) {
            return kDescInvalid;
        }
        const bool storage = t->operands[5] == 2u;
        if (t->operands[1] == kDimBuffer) {
            return storage ? kDescStorageTexelBuffer : kDescUniformTexelBuffer;
        }
        return storage ? kDescStorageImage : kDescSampledImage;
    }
    default:
        return kDescInvalid;
    }
}

bool bindingLess(const ShaderBindingReflection& a, const ShaderBindingReflection& b) {
    return a.set != b.set ? a.set < b.set : a.binding < b.binding;
}

} // namespace

u32 ShaderReflection::setCount() const {
    u32 count = 0;
    for (const ShaderBindingReflection& b : bindings) {
        count = std::max(count, b.set + 1u);
    }
    return count;
}

const ShaderBindingReflection* ShaderReflection::find(u32 set, u32 binding) const {
    for (const ShaderBindingReflection& b : bindings) {
        if (b.set == set && b.binding == binding) {
            return &b;
        }
    }
    return nullptr;
}

ShaderReflection reflectSpirv(const u32* words, usize wordCount) {
    ShaderReflection out;
    if (words == nullptr || wordCount < 5u || words[0] != 0x07230203u) {
        out.message = "not a SPIR-V module";
        return out;
    }

    Module module;
    struct Variable {
        u32 id;
        u32 pointerType;
        u32 storageClass;
    };
    std::vector<Variable> variables;
    u32 entryId = 0;
    bool haveEntry = false;

    usize i = 5;
    while (i < wordCount) {
        const u32 first = words[i];
        const u32 count = first >> 16u;
        const u32 opcode = first & 0xFFFFu;
        if (count == 0u || i + count > wordCount) {
            out.message = "truncated SPIR-V instruction stream";
            return out;
        }
        const u32* op = words + i + 1; // operands
        const usize n = count - 1u;
        switch (opcode) {
        case kOpName:
            if (n >= 2) {
                usize consumed = 0;
                module.names[op[0]] = readString(op + 1, n - 1, consumed);
            }
            break;
        case kOpEntryPoint:
            if (!haveEntry && n >= 3) {
                haveEntry = true;
                out.stageFlags = stageFlagsForExecutionModel(op[0]);
                entryId = op[1];
                usize consumed = 0;
                out.entryPoint = readString(op + 2, n - 2, consumed);
            }
            break;
        case kOpExecutionMode:
            if (n >= 5 && op[1] == kExecutionModeLocalSize && (!haveEntry || op[0] == entryId)) {
                out.localSize[0] = op[2];
                out.localSize[1] = op[3];
                out.localSize[2] = op[4];
            }
            break;
        case kOpTypeInt:
        case kOpTypeFloat:
        case kOpTypeVector:
        case kOpTypeMatrix:
        case kOpTypeImage:
        case kOpTypeSampler:
        case kOpTypeSampledImage:
        case kOpTypeArray:
        case kOpTypeRuntimeArray:
        case kOpTypeStruct:
        case kOpTypePointer:
        case kOpTypeAccelerationStructureKHR:
            if (n >= 1) {
                TypeInfo info;
                info.opcode = opcode;
                info.operands.assign(op + 1, op + n);
                module.types[op[0]] = std::move(info);
            }
            break;
        case kOpConstant:
            // result type, result id, value (low word first for 64-bit)
            if (n >= 3) {
                u64 value = op[2];
                if (n >= 4) {
                    value |= static_cast<u64>(op[3]) << 32u;
                }
                module.constants[op[1]] = value;
            }
            break;
        case kOpVariable:
            if (n >= 3) {
                variables.push_back(Variable{op[1], op[0], op[2]});
            }
            break;
        case kOpDecorate:
            if (n >= 2) {
                Decorations& d = module.decorations[op[0]];
                switch (op[1]) {
                case kDecorationBlock:
                    d.block = true;
                    break;
                case kDecorationBufferBlock:
                    d.bufferBlock = true;
                    break;
                case kDecorationArrayStride:
                    d.arrayStride = n >= 3 ? op[2] : 0u;
                    break;
                case kDecorationBinding:
                    d.hasBinding = n >= 3;
                    d.binding = n >= 3 ? op[2] : 0u;
                    break;
                case kDecorationDescriptorSet:
                    d.hasSet = n >= 3;
                    d.set = n >= 3 ? op[2] : 0u;
                    break;
                default:
                    break;
                }
            }
            break;
        case kOpMemberDecorate:
            if (n >= 4 && op[2] == kDecorationOffset) {
                module.memberOffsets[Module::memberKey(op[0], op[1])] = op[3];
            } else if (n >= 4 && op[2] == kDecorationMatrixStride) {
                module.memberMatrixStrides[Module::memberKey(op[0], op[1])] = op[3];
            }
            break;
        default:
            break;
        }
        i += count;
    }

    if (!haveEntry) {
        out.message = "SPIR-V module has no entry point";
        return out;
    }

    for (const Variable& var : variables) {
        const TypeInfo* pointer = module.type(var.pointerType);
        if (pointer == nullptr || pointer->opcode != kOpTypePointer || pointer->operands.size() < 2) {
            continue;
        }
        const u32 pointee = pointer->operands[1];
        if (var.storageClass == kStoragePushConstant) {
            out.pushConstantBytes = std::max(out.pushConstantBytes, module.sizeOf(pointee));
            continue;
        }
        const Decorations d = module.decorationsOf(var.id);
        if (!d.hasBinding) {
            continue;
        }
        u32 arrayCount = 1;
        const u32 element = unwrapArrays(module, pointee, arrayCount);
        const u32 type = descriptorTypeFor(module, var.storageClass, element);
        if (type == kDescInvalid) {
            continue;
        }
        ShaderBindingReflection binding;
        binding.set = d.hasSet ? d.set : 0u;
        binding.binding = d.binding;
        binding.descriptorType = type;
        binding.count = arrayCount;
        binding.stageFlags = out.stageFlags;
        const auto name = module.names.find(var.id);
        if (name != module.names.end() && !name->second.empty()) {
            binding.name = name->second;
        } else {
            const auto typeName = module.names.find(element);
            binding.name = typeName != module.names.end() ? typeName->second : std::string();
        }
        out.bindings.push_back(std::move(binding));
    }
    std::sort(out.bindings.begin(), out.bindings.end(), bindingLess);
    out.valid = true;
    out.message = "reflected " + std::to_string(out.bindings.size()) + " binding(s)";
    return out;
}

bool mergeShaderReflection(ShaderReflection& into, const ShaderReflection& other, std::string* error) {
    if (!other.valid) {
        if (error != nullptr) {
            *error = "cannot merge an invalid reflection";
        }
        return false;
    }
    if (!into.valid) {
        into = other;
        return true;
    }
    for (const ShaderBindingReflection& b : other.bindings) {
        auto it = std::find_if(into.bindings.begin(), into.bindings.end(), [&b](const ShaderBindingReflection& e) {
            return e.set == b.set && e.binding == b.binding;
        });
        if (it == into.bindings.end()) {
            into.bindings.push_back(b);
            continue;
        }
        if (it->descriptorType != b.descriptorType || it->count != b.count) {
            if (error != nullptr) {
                *error = "set " + std::to_string(b.set) + " binding " + std::to_string(b.binding) +
                         " differs between stages";
            }
            return false;
        }
        it->stageFlags |= b.stageFlags;
    }
    std::sort(into.bindings.begin(), into.bindings.end(), bindingLess);
    into.stageFlags |= other.stageFlags;
    into.pushConstantBytes = std::max(into.pushConstantBytes, other.pushConstantBytes);
    return true;
}

bool sameLayout(const ShaderReflection& a, const ShaderReflection& b) {
    if (a.pushConstantBytes != b.pushConstantBytes || a.bindings.size() != b.bindings.size()) {
        return false;
    }
    for (usize i = 0; i < a.bindings.size(); ++i) {
        const ShaderBindingReflection& x = a.bindings[i];
        const ShaderBindingReflection& y = b.bindings[i];
        if (x.set != y.set || x.binding != y.binding || x.descriptorType != y.descriptorType || x.count != y.count) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::renderer
