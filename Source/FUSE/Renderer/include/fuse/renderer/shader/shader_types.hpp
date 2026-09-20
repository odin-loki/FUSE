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
};

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
