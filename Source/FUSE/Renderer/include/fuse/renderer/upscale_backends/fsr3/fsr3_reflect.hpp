#pragma once

// WP-4.2: minimal SPIR-V reflection for the FSR 3.1 passes. The SDK binds its resources by the shader's variable
// names (ffx_fsr3upscaler.cpp patchResourceBindings over the FidelityFX-SC reflection); the FUSE host does the
// same from the embedded SPIR-V: descriptor bindings (set, binding, kind, variable name), uniform / push-constant
// block member offsets and sizes (the layout gates compare them with the C++ records) and the workgroup size.
// Vulkan-free; used at init only (allocates).

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::fsr3 {

enum class SpirvDescriptorKind : u8 {
    Unknown = 0,
    SampledImage,  ///< texture2D / Texture2D
    StorageImage,  ///< image2D / RWTexture2D
    Sampler,       ///< sampler
    CombinedImageSampler,
    UniformBuffer, ///< uniform block
    StorageBuffer, ///< buffer block / StructuredBuffer
};

struct SpirvBlockMember {
    std::string name;
    u32 offset = 0;
    u32 size = 0; ///< scalar / vector / matrix / fixed array of those; 0 when unknown
};

struct SpirvBinding {
    std::string name; ///< variable name (OpName), e.g. "r_input_color_jittered"
    std::string typeName; ///< block type name for buffers
    u32 set = 0;
    u32 binding = 0;
    SpirvDescriptorKind kind = SpirvDescriptorKind::Unknown;
    u32 imageFormat = 0; ///< SPIR-V ImageFormat of storage images (0 = Unknown)
    std::vector<SpirvBlockMember> members; ///< uniform / storage blocks
};

struct SpirvReflection {
    std::vector<SpirvBinding> bindings;
    std::vector<SpirvBlockMember> pushConstants; ///< members of the push-constant block, if any
    u32 pushConstantBytes = 0;                    ///< end of the last push-constant member
    u32 localSize[3] = {1u, 1u, 1u};
    bool valid = false;
};

/// Parses `wordCount` SPIR-V words. False (out.valid = false) for a malformed module.
bool reflect_spirv(const u32* words, usize wordCount, SpirvReflection& out);

} // namespace fuse::renderer::fsr3
