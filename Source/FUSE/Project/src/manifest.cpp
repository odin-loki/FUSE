#include <fuse/project/manifest.hpp>

#include <cstdio>

namespace fuse::project {

hybrid::DimensionFlags toDimensionFlags(const DimensionSettings& settings) {
    hybrid::DimensionFlags flags;
    flags.enable3D = settings.enable3D;
    flags.enable2D = settings.enable2D;
    flags.enableUI = settings.enableUI;
    return flags;
}

std::string escapeJsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2u);
    for (const char ch : text) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20u) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
            } else {
                out += ch; // UTF-8 bytes pass through unchanged
            }
            break;
        }
    }
    return out;
}

namespace {

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

void appendUtf8(std::string& out, u32 codePoint) {
    if (codePoint < 0x80u) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800u) {
        out += static_cast<char>(0xC0u | (codePoint >> 6));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else if (codePoint < 0x10000u) {
        out += static_cast<char>(0xE0u | (codePoint >> 12));
        out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (codePoint >> 18));
        out += static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
    }
}

bool readHex4(std::string_view text, usize at, u32& out) {
    if (at + 4u > text.size()) {
        return false;
    }
    u32 value = 0;
    for (usize i = 0; i < 4u; ++i) {
        const int digit = hexValue(text[at + i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<u32>(digit);
    }
    out = value;
    return true;
}

} // namespace

std::string unescapeJsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch != '\\' || i + 1u >= text.size()) {
            out += ch;
            continue;
        }
        const char esc = text[++i];
        switch (esc) {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            u32 codePoint = 0;
            if (!readHex4(text, i + 1u, codePoint)) {
                out += "\\u";
                break;
            }
            i += 4u;
            // Surrogate pair (\\uD83D\\uDE00) -> one code point.
            if (codePoint >= 0xD800u && codePoint <= 0xDBFFu && i + 6u < text.size() &&
                text[i + 1u] == '\\' && text[i + 2u] == 'u') {
                u32 low = 0;
                if (readHex4(text, i + 3u, low) && low >= 0xDC00u && low <= 0xDFFFu) {
                    codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) + (low - 0xDC00u);
                    i += 6u;
                }
            }
            appendUtf8(out, codePoint);
            break;
        }
        default:
            out += '\\';
            out += esc;
            break;
        }
    }
    return out;
}

std::string writeManifestJson(const ProjectManifest& manifest) {
    const auto boolText = [](bool value) { return value ? "true" : "false"; };
    std::string json;
    json.reserve(512);
    json += "{\n";
    json += "  \"schemaVersion\": " + std::to_string(kProjectSchemaVersion) + ",\n";
    json += "  \"name\": \"" + escapeJsonString(manifest.name) + "\",\n";
    json += "  \"dimensions\": {\n";
    json += std::string("    \"enable3D\": ") + boolText(manifest.dimensions.enable3D) + ",\n";
    json += std::string("    \"enable2D\": ") + boolText(manifest.dimensions.enable2D) + ",\n";
    json += std::string("    \"enableUI\": ") + boolText(manifest.dimensions.enableUI) + "\n";
    json += "  },\n";
    json += "  \"modules\": {\n";
    json += std::string("    \"ai\": ") + boolText(manifest.modules.ai) + ",\n";
    json += std::string("    \"cinematics\": ") + boolText(manifest.modules.cinematics) + ",\n";
    json += std::string("    \"fx\": ") + boolText(manifest.modules.fx) + ",\n";
    json += std::string("    \"mechanics\": ") + boolText(manifest.modules.mechanics) + ",\n";
    json += std::string("    \"adventure\": ") + boolText(manifest.modules.adventure) + "\n";
    json += "  },\n";
    json += "  \"defaultWorld3D\": \"" + escapeJsonString(manifest.defaultWorld3D) + "\",\n";
    json += "  \"defaultWorld2D\": \"" + escapeJsonString(manifest.defaultWorld2D) + "\",\n";
    json += "  \"workerCap\": " + std::to_string(manifest.workerCap) + "\n";
    json += "}\n";
    return json;
}

} // namespace fuse::project
