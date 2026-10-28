#pragma once

#include <string_view>

namespace fuse::project {

/// True when the filename extension is `.lua` or `.fuse.script` (ASCII case-insensitive).
/// A path with no extension is false.
[[nodiscard]] inline bool script_ext_ok(std::string_view path) {
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
    if (equals_ci(filename.substr(dot), ".lua")) {
        return true;
    }
    constexpr std::string_view kFuseScript = ".fuse.script";
    if (filename.size() < kFuseScript.size()) {
        return false;
    }
    return equals_ci(filename.substr(filename.size() - kFuseScript.size()), kFuseScript);
}

} // namespace fuse::project
