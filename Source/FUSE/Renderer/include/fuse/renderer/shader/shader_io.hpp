#pragma once

#include <fuse/renderer/shader/shader_types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer {

/// SPIR-V file magic (first u32 word).
constexpr u32 kSpirvMagic = 0x07230203u;

bool isValidSpirvHeader(const u32* words, u32 wordCount);
bool isValidSpirvHeader(const u8* bytes, u32 byteCount);

/// Loads raw SPIR-V words from a `.spv` file. Returns empty vector on failure.
std::vector<u32> loadSpirvFile(const char* path, std::string* errorOut = nullptr);

/// Resolves `sourcePath` to a sibling `.spv` when offline fixtures are used.
std::string spirvPathForSource(const char* sourcePath);

} // namespace fuse::renderer
