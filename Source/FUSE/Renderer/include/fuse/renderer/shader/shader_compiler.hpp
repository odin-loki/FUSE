#pragma once

#include <fuse/renderer/shader/shader_types.hpp>
#include <fuse/renderer/shader/shader_watch.hpp>

#include <string>
#include <vector>

namespace fuse::renderer {

/// Offline-first shader compiler scaffold (B2.4).
/// CI loads checked-in `.spv` fixtures; optional glslang path behind FUSE_SHADER_GLSLANG.
class ShaderCompiler {
public:
    /// Loads precompiled SPIR-V for `desc.sourcePath` (`.spv` sibling or explicit `.spv` path).
    static CompiledShader compileOffline(const ShaderDesc& desc);

    /// When glslang is enabled at build time, compiles GLSL/HLSL source; otherwise falls back to offline.
    static CompiledShader compile(const ShaderDesc& desc);

    /// Watch `desc.sourcePath`, define strings, and include paths (copied into owned storage).
    /// Returns false if path is null/empty.
    bool watch(const ShaderDesc& desc);

    /// Polls ShaderFileWatch and recompiles only watched entries whose paths match the
    /// changed files from this poll. Returns number of successful recompiles this poll.
    u32 pollHotReload();

    u32 watchedCount() const;
    const CompiledShader* lastCompiled(const char* path) const; // nullptr if unknown

private:
    struct WatchedEntry {
        std::string path;
        std::vector<std::string> defineStorage;
        std::vector<const char*> definePtrs;
        std::vector<std::string> includePathStorage;
        std::vector<const char*> includePathPtrs;
        ShaderDesc desc{};
        CompiledShader last{};
    };

    static void bindOwnedPointers(WatchedEntry& entry);

    ShaderFileWatch m_watch;
    std::vector<WatchedEntry> m_entries;
};

} // namespace fuse::renderer
