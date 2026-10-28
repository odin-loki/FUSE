#pragma once

#include <string_view>

namespace fuse::platform {

/// True when `name` length is 5..64 and the extension is `.dmp` (ASCII case-insensitive).
[[nodiscard]] inline bool minidump_name_ok(std::string_view name) {
    if (name.size() < 5 || name.size() > 64) {
        return false;
    }
    constexpr std::string_view kExt = ".dmp";
    const std::string_view ext = name.substr(name.size() - kExt.size());
    for (std::size_t i = 0; i < kExt.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(ext[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        if (c != static_cast<unsigned char>(kExt[i])) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::platform
