#include <fuse/renderer/shader/shader_io.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace fuse::renderer {

namespace {

bool startsWith(const std::string& text, const char* prefix) {
    if (prefix == nullptr) {
        return false;
    }
    const std::string needle(prefix);
    return text.size() >= needle.size() && text.compare(0, needle.size(), needle) == 0;
}

std::vector<u32> loadSpirvBytes(const char* bytes, std::size_t byteCount, const char* pathForError,
                                std::string* errorOut) {
    if (bytes == nullptr || byteCount == 0u || (byteCount % sizeof(u32)) != 0u) {
        if (errorOut) {
            *errorOut = std::string("invalid SPIR-V size for: ") + (pathForError != nullptr ? pathForError : "");
        }
        return {};
    }

    std::vector<u32> words(byteCount / sizeof(u32));
    std::memcpy(words.data(), bytes, byteCount);
    if (!isValidSpirvHeader(words.data(), static_cast<u32>(words.size()))) {
        if (errorOut) {
            *errorOut = std::string("SPIR-V magic mismatch for: ") + (pathForError != nullptr ? pathForError : "");
        }
        return {};
    }

    return words;
}

} // namespace

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
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.read(bytes.data(), size);
    if (!file) {
        if (errorOut) {
            *errorOut = std::string("failed to read SPIR-V file: ") + path;
        }
        return {};
    }

    return loadSpirvBytes(bytes.data(), bytes.size(), path, errorOut);
}

std::vector<u32> loadCookedFuseshaderSpirv(const char* path, std::string* errorOut) {
    if (path == nullptr || path[0] == '\0') {
        if (errorOut) {
            *errorOut = "cooked shader path is empty";
        }
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (errorOut) {
            *errorOut = std::string("failed to open cooked shader: ") + path;
        }
        return {};
    }

    std::string marker;
    if (!std::getline(file, marker)) {
        if (errorOut) {
            *errorOut = std::string("cooked shader header missing: ") + path;
        }
        return {};
    }

    if (!startsWith(marker, "FUSESHADER_SPIV") && !startsWith(marker, "FUSESHADER_GLSLANG")) {
        if (errorOut) {
            *errorOut = std::string("cooked shader has no SPIR-V payload: ") + path;
        }
        return {};
    }

    std::string line;
    bool foundData = false;
    while (std::getline(file, line)) {
        if (line == "DATA") {
            foundData = true;
            break;
        }
    }

    if (!foundData) {
        if (errorOut) {
            *errorOut = std::string("cooked shader DATA section missing: ") + path;
        }
        return {};
    }

    const std::streampos payloadStart = file.tellg();
    file.seekg(0, std::ios::end);
    const std::streampos payloadEnd = file.tellg();
    if (payloadStart < 0 || payloadEnd < payloadStart) {
        if (errorOut) {
            *errorOut = std::string("cooked shader payload unreadable: ") + path;
        }
        return {};
    }

    const std::streamsize payloadSize = payloadEnd - payloadStart;
    if (payloadSize <= 0 || (payloadSize % static_cast<std::streamsize>(sizeof(u32))) != 0) {
        if (errorOut) {
            *errorOut = std::string("cooked shader payload size invalid: ") + path;
        }
        return {};
    }

    file.seekg(payloadStart, std::ios::beg);
    std::vector<char> bytes(static_cast<std::size_t>(payloadSize));
    file.read(bytes.data(), payloadSize);
    if (!file) {
        if (errorOut) {
            *errorOut = std::string("failed to read cooked shader payload: ") + path;
        }
        return {};
    }

    return loadSpirvBytes(bytes.data(), bytes.size(), path, errorOut);
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

std::string fuseshaderPathForSource(const char* sourcePath) {
    if (sourcePath == nullptr) {
        return {};
    }

    const std::string path(sourcePath);
    if (path.size() >= 11u && path.compare(path.size() - 11u, 11u, ".fuseshader") == 0) {
        return path;
    }

    if (path.size() >= 4u && path.compare(path.size() - 4u, 4u, ".spv") == 0) {
        return path.substr(0, path.size() - 4u) + ".fuseshader";
    }

    return path + ".fuseshader";
}

} // namespace fuse::renderer
