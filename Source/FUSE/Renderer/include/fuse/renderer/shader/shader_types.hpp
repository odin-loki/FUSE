#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer {

enum class ShaderStage : u8 {
    Vertex,
    Fragment,
    Compute,
    Mesh,
    Task,
    RayGen,
    RayMiss,
    RayClosestHit,
    RayAnyHit,
};

struct ShaderDesc {
    const char* sourcePath = nullptr;
    const char* entryPoint = "main";
    ShaderStage stage = ShaderStage::Vertex;
    const char** defines = nullptr;
    u32 defineCount = 0;
    const char** includePaths = nullptr;
    u32 includePathCount = 0;
    /// Renderer tier this permutation targets (0..3 = T0..T3), or -1 for tier-agnostic. When set,
    /// runtime compiles add `FUSE_RENDER_TIER=<tier>` and the tier enters the permutation key.
    i32 tier = -1;
};

struct CompiledShader {
    std::vector<u32> spirv;
    ShaderStage stage = ShaderStage::Vertex;
    std::string sourcePath;
    std::string entryPoint;
    std::string message;
    std::vector<std::string> defines;
    std::vector<std::string> includePaths;
    u64 spirvHash = 0;
    u32 defineCount = 0;
    u32 includePathCount = 0;
    bool valid = false;
    /// shaderPermutationKey(desc) of the request that produced this module (0 when unknown).
    u64 permutationKey = 0;
    i32 tier = -1;
    /// Files the source pulled in (#include / Slang import), from the compiler's depfile. Runtime
    /// compiles only; hot reload watches these next to the source itself.
    std::vector<std::string> dependencies;
    /// Where the SPIR-V was loaded from (sibling .spv, cooked .fuseshader or the permutation cache).
    std::string spirvPath;
};

/// Source language, from the source path extension: `.slang` -> Slang, `.spv`/`.fuseshader` ->
/// precompiled, anything else (`.comp`, `.vert`, `.glsl`, ...) -> GLSL.
enum class ShaderLanguage : u8 { Glsl, Slang, Precompiled };
ShaderLanguage shaderLanguageForPath(const char* sourcePath);

/// Permutation key: FNV-1a over source path, entry point, stage, the define set (order
/// independent: defines are sorted first), include paths and tier. Stable across runs and
/// processes; independent of file contents (combine with a content hash for cache validity).
u64 shaderPermutationKey(const ShaderDesc& desc);

/// FNV-1a over SPIR-V words. Empty or null input hashes to 0.
inline u64 hashSpirvWords(const u32* words, u32 wordCount) {
    if (words == nullptr || wordCount == 0u) {
        return 0;
    }

    constexpr u64 kFnvOffset = 14695981039346656037ull;
    constexpr u64 kFnvPrime = 1099511628211ull;
    u64 hash = kFnvOffset;
    for (u32 i = 0; i < wordCount; ++i) {
        hash ^= static_cast<u64>(words[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

/// Continue FNV-1a over the bytes of `text`. Empty or null input leaves `hash` unchanged.
inline u64 hashMixDefineBytes(u64 hash, const char* text, u32 byteCount) {
    if (text == nullptr || byteCount == 0u) {
        return hash;
    }

    constexpr u64 kFnvPrime = 1099511628211ull;
    for (u32 i = 0; i < byteCount; ++i) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(text[i]));
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace fuse::renderer
