#pragma once

#include <string_view>

namespace fuse::project {

/// True when `name` length is 1..48, the first character is an ASCII letter, and the name contains no spaces.
[[nodiscard]] inline bool script_cook_ok(std::string_view name) {
    if (name.size() < 1 || name.size() > 48) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    return name.find(' ') == std::string_view::npos;
}

} // namespace fuse::project
