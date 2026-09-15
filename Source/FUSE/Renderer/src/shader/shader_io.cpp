#include <fuse/renderer/shader/shader_io.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace fuse::renderer {

bool isValidSpirvHeader(const u32* words, u32 wordCount) {
    return words != nullptr && wordCount > 0 && words[0] == kSpirvMagic;
}

bool isValidSpirvHeader(const u8* bytes, u32 byteCount) {
    if (bytes == nullptr || byteCount < sizeof(u32)) {
        return false;
    }

    u32 magic = 0;
    std::memcpy(&magic, bytes, sizeof(u32));
    return magic == kSpirvMagic;
}

std::vector<u32> loadSpirvFile(const char* path, std::string* errorOut) {
    if (path == nullptr || path[0] == '\0') {
        if (errorOut) {
            *errorOut = "SPIR-V path is empty";
        }
        return {};
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (errorOut) {
            *errorOut = std::string("failed to open SPIR-V file: ") + path;
        }
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % static_cast<std::streamsize>(sizeof(u32))) != 0) {
        if (errorOut) {
            *errorOut = std::string("invalid SPIR-V size for: ") + path;
        }
        return {};
    }

    file.seekg(0, std::ios::beg);
    std::vector<u32> words(static_cast<std::size_t>(size / sizeof(u32)));
    file.read(reinterpret_cast<char*>(words.data()), size);
    if (!file) {
        if (errorOut) {
            *errorOut = std::string("failed to read SPIR-V file: ") + path;
        }
        return {};
    }

    if (!isValidSpirvHeader(words.data(), static_cast<u32>(words.size()))) {
        if (errorOut) {
            *errorOut = std::string("SPIR-V magic mismatch for: ") + path;
        }
        return {};
    }

    return words;
}

std::string spirvPathForSource(const char* sourcePath) {
    if (sourcePath == nullptr) {
        return {};
    }

    const std::string path(sourcePath);
    if (path.size() >= 4u && path.compare(path.size() - 4u, 4u, ".spv") == 0) {
        return path;
    }

    return path + ".spv";
}

} // namespace fuse::renderer
