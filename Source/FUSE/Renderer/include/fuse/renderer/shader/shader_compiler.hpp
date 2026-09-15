#pragma once

#include <fuse/renderer/shader/shader_types.hpp>

namespace fuse::renderer {

/// Offline-first shader compiler scaffold (B2.4).
/// CI loads checked-in `.spv` fixtures; optional glslang path behind FUSE_SHADER_GLSLANG.
class ShaderCompiler {
public:
    /// Loads precompiled SPIR-V for `desc.sourcePath` (`.spv` sibling or explicit `.spv` path).
    static CompiledShader compileOffline(const ShaderDesc& desc);

    /// When glslang is enabled at build time, compiles GLSL/HLSL source; otherwise falls back to offline.
    static CompiledShader compile(const ShaderDesc& desc);
};

} // namespace fuse::renderer
