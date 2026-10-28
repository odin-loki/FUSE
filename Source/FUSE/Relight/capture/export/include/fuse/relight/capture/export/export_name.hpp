#pragma once

#include <string_view>

namespace fuse::relight {

/// True when `name` length is 1..64 and the first character is an ASCII letter.
[[nodiscard]] inline bool export_name_ok(std::string_view name) {
    if (name.size() < 1 || name.size() > 64) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
}

} // namespace fuse::relight
