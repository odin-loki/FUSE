// FUSE Relight RL-3.2: lexical cleanup of an asset path authored in a mod import.
// '\\' becomes '/', a leading "./" and repeated '/' are dropped, and only the extension
// (the last dot segment of the final component) is lowercased. Empty input stays empty.
// ".." is not resolved.
#pragma once

#include <string>
#include <string_view>

namespace fuse::relight::mods::import {

/// '\\' -> '/', drop a leading "./" and repeated '/', lowercase only the final component's extension.
inline std::string normalize_import_path(std::string_view path) {
    if (path.empty()) {
        return {};
    }

    std::string slash;
    slash.reserve(path.size());
    for (const char c : path) {
        const char n = c == '\\' ? '/' : c;
        if (n == '/' && !slash.empty() && slash.back() == '/') {
            continue;
        }
        slash.push_back(n);
    }

    std::string_view view = slash;
    while (view.size() >= 2 && view[0] == '.' && view[1] == '/') {
        view.remove_prefix(2);
    }

    std::string out(view);
    const std::size_t slashAt = out.rfind('/');
    const std::size_t dotAt = out.rfind('.');
    if (dotAt != std::string::npos && (slashAt == std::string::npos || dotAt > slashAt)) {
        for (std::size_t i = dotAt + 1; i < out.size(); ++i) {
            const unsigned char u = static_cast<unsigned char>(out[i]);
            if (u >= 'A' && u <= 'Z') {
                out[i] = static_cast<char>(u - 'A' + 'a');
            }
        }
    }
    return out;
}

} // namespace fuse::relight::mods::import
