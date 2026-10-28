#pragma once

#include <string>
#include <string_view>

namespace fuse::asset {

inline std::string normalize_asset_key(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    bool pendingSlash = false;
    for (unsigned char raw : key) {
        if (raw == ' ' || raw == '\t') {
            continue;
        }
        char c = static_cast<char>(raw);
        if (c == '\\') {
            c = '/';
        }
        if (c == '/') {
            if (!out.empty()) {
                pendingSlash = true;
            }
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (pendingSlash) {
            if (out.size() + 2 > 128) {
                break;
            }
            out.push_back('/');
            pendingSlash = false;
        }
        if (out.size() >= 128) {
            break;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace fuse::asset
