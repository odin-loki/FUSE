#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer {

/// One descriptor binding used by a shader. `descriptorType` is the numeric VkDescriptorType,
/// `stageFlags` the VkShaderStageFlags of the stages that declare it.
struct ShaderBindingReflection {
    u32 set = 0;
    u32 binding = 0;
    u32 descriptorType = 0;
    /// Array size; 1 for a single descriptor, 0 for a runtime-sized (bindless) array.
    u32 count = 1;
    u32 stageFlags = 0;
    std::string name;
};

/// Layout-relevant facts of a SPIR-V module, language independent (the same for a GLSL and a
/// Slang build of one shader). Built by reflectSpirv; merged across stages by mergeShaderReflection.
struct ShaderReflection {
    bool valid = false;
    std::string message;
    std::string entryPoint;
    u32 stageFlags = 0;
    u32 localSize[3] = {1, 1, 1};
    /// Push-constant block size in bytes (0 = none).
    u32 pushConstantBytes = 0;
    /// Sorted by (set, binding).
    std::vector<ShaderBindingReflection> bindings;

    /// Highest descriptor set index + 1 (0 when there are no bindings).
    u32 setCount() const;
    const ShaderBindingReflection* find(u32 set, u32 binding) const;
};

/// Reflects the first entry point of a SPIR-V module: execution model, LocalSize, push-constant
/// size and every DescriptorSet/Binding-decorated variable (storage/uniform buffers, sampled and
/// storage images, texel buffers, samplers, combined image samplers, acceleration structures).
ShaderReflection reflectSpirv(const u32* words, usize wordCount);

/// Adds `other`'s bindings and stages to `into` (union; the same set/binding must agree on type and
/// count, else returns false with `error`). Push-constant size is the max of both.
bool mergeShaderReflection(ShaderReflection& into, const ShaderReflection& other, std::string* error = nullptr);

/// True when both describe the same pipeline layout (bindings by set/binding/type/count, push
/// constant size). Names and entry points are ignored.
bool sameLayout(const ShaderReflection& a, const ShaderReflection& b);

} // namespace fuse::renderer
