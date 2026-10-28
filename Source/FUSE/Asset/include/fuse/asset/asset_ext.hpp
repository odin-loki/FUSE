#pragma once

#include <string_view>

namespace fuse::asset {

/// True when the filename extension is `.png`, `.obj`, `.wav`, or `.fuse` (ASCII case-insensitive).
/// A path with no extension is false.
[[nodiscard]] inline bool asset_ext_ok(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const std::string_view filename =
        slash == std::string_view::npos ? path : path.substr(slash + 1);

    const auto fold = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
    };
    const auto equals_ci = [&](std::string_view text, std::string_view expected) {
        if (text.size() != expected.size()) {
            return false;
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (fold(static_cast<unsigned char>(text[i])) != fold(static_cast<unsigned char>(expected[i]))) {
                return false;
            }
        }
        return true;
    };

    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= filename.size()) {
        return false;
    }
    const std::string_view ext = filename.substr(dot);
    return equals_ci(ext, ".png") || equals_ci(ext, ".obj") || equals_ci(ext, ".wav") ||
           equals_ci(ext, ".fuse");
}

} // namespace fuse::asset
