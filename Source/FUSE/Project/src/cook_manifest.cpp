#include <fuse/project/cook_manifest.hpp>

#include <fuse/project/json_reader.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace fuse::project {

namespace {

std::string readFileToString(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

CookAssetKind parseCookAssetKind(std::string_view token) {
    if (token == "mesh") {
        return CookAssetKind::Mesh;
    }
    if (token == "texture") {
        return CookAssetKind::Texture;
    }
    if (token == "audio") {
        return CookAssetKind::Audio;
    }
    if (token == "shader") {
        return CookAssetKind::Shader;
    }
    return CookAssetKind::Mesh;
}

std::string_view trimToken(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string parseQuotedString(std::string_view token) {
    token = trimToken(token);
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
        return std::string(token.substr(1, token.size() - 2));
    }
    return std::string(token);
}

bool parseAssetObject(std::string_view objectBody, CookManifestEntry& out) {
    const auto readField = [&](std::string_view key, std::string& value) -> bool {
        const std::string needle = std::string("\"") + std::string(key) + "\":";
        const std::size_t keyPos = objectBody.find(needle);
        if (keyPos == std::string_view::npos) {
            return false;
        }

        std::size_t cursor = keyPos + needle.size();
        while (cursor < objectBody.size() && std::isspace(static_cast<unsigned char>(objectBody[cursor]))) {
            ++cursor;
        }

        std::size_t end = cursor;
        if (end < objectBody.size() && objectBody[end] == '"') {
            ++end;
            while (end < objectBody.size() && objectBody[end] != '"') {
                if (objectBody[end] == '\\' && end + 1 < objectBody.size()) {
                    end += 2;
                } else {
                    ++end;
                }
            }
            if (end < objectBody.size()) {
                ++end;
            }
        } else {
            while (end < objectBody.size() && objectBody[end] != ',' && objectBody[end] != '}') {
                ++end;
            }
        }

        value = parseQuotedString(objectBody.substr(cursor, end - cursor));
        return !value.empty();
    };

    std::string kindToken;
    if (readField("kind", kindToken)) {
        out.kind = parseCookAssetKind(trimToken(kindToken));
    }

    if (!readField("sourcePath", out.source_path) && !readField("source_path", out.source_path)) {
        return false;
    }

    if (!readField("outputPath", out.output_path) && !readField("output_path", out.output_path)) {
        return false;
    }

    return true;
}

void parseAssetsArray(std::string_view source, CookManifest& manifest) {
    const std::string needle = "\"assets\":";
    const std::size_t keyPos = source.find(needle);
    if (keyPos == std::string_view::npos) {
        return;
    }

    std::size_t cursor = keyPos + needle.size();
    while (cursor < source.size() && source[cursor] != '[') {
        ++cursor;
    }
    if (cursor >= source.size()) {
        return;
    }

    ++cursor;
    while (cursor < source.size()) {
        while (cursor < source.size() && source[cursor] != '{') {
            if (source[cursor] == ']') {
                return;
            }
            ++cursor;
        }
        if (cursor >= source.size()) {
            return;
        }

        std::size_t depth = 0;
        const std::size_t objectStart = cursor;
        for (std::size_t i = cursor; i < source.size(); ++i) {
            const char ch = source[i];
            if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    CookManifestEntry entry;
                    if (parseAssetObject(source.substr(objectStart, i - objectStart + 1), entry)) {
                        manifest.assets.push_back(std::move(entry));
                    }
                    cursor = i + 1;
                    break;
                }
            }
        }
    }
}

} // namespace

const char* cookAssetKindName(CookAssetKind kind) {
    switch (kind) {
    case CookAssetKind::Mesh:
        return "mesh";
    case CookAssetKind::Texture:
        return "texture";
    case CookAssetKind::Audio:
        return "audio";
    case CookAssetKind::Shader:
        return "shader";
    }
    return "unknown";
}

const char* cookStatusName(CookStatus status) {
    switch (status) {
    case CookStatus::Ok:
        return "ok";
    case CookStatus::InvalidInput:
        return "invalid_input";
    case CookStatus::SourceMissing:
        return "source_missing";
    case CookStatus::OutputError:
        return "output_error";
    case CookStatus::UnsupportedKind:
        return "unsupported_kind";
    }
    return "unknown";
}

CookManifest makeDefaultCookManifest(const std::string& project_root) {
    CookManifest manifest;
    manifest.schema_version = 1;
    manifest.project_root = project_root;

    CookManifestEntry meshEntry;
    meshEntry.kind = CookAssetKind::Mesh;
    meshEntry.source_path = "art/mesh.obj";
    meshEntry.output_path = "cooked/mesh.fusemesh";
    manifest.assets.push_back(meshEntry);

    CookManifestEntry textureEntry;
    textureEntry.kind = CookAssetKind::Texture;
    textureEntry.source_path = "art/albedo.png";
    textureEntry.output_path = "cooked/albedo.fusetex";
    manifest.assets.push_back(textureEntry);

    return manifest;
}

CookManifestLoadResult parseCookManifest(const std::string& json, const std::string& project_root) {
    CookManifestLoadResult result;
    result.manifest.project_root = project_root;

    json::Reader reader(json);
    if (!reader.readU32("schemaVersion", result.manifest.schema_version)) {
        result.status = CookManifestLoadStatus::ParseError;
        result.error = "missing schemaVersion";
        return result;
    }

    if (result.manifest.schema_version != 1u) {
        result.status = CookManifestLoadStatus::UnsupportedSchema;
        result.error = "unsupported cook manifest schema";
        return result;
    }

    parseAssetsArray(json, result.manifest);
    result.status = CookManifestLoadStatus::Ok;
    return result;
}

CookManifestLoadResult loadCookManifestFromFile(const std::string& path) {
    CookManifestLoadResult result;
    const std::string contents = readFileToString(path);
    if (contents.empty()) {
        result.status = CookManifestLoadStatus::FileNotFound;
        result.error = "cook manifest not found or empty: " + path;
        return result;
    }

    std::string project_root = path;
    const std::size_t slash = project_root.find_last_of("/\\");
    if (slash != std::string::npos) {
        project_root = project_root.substr(0, slash);
    } else {
        project_root.clear();
    }

    result = parseCookManifest(contents, project_root);
    if (result.status == CookManifestLoadStatus::Ok) {
        result.manifest.project_root = project_root;
    }
    return result;
}

} // namespace fuse::project
