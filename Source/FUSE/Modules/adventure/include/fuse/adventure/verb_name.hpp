#pragma once

#include <string_view>

namespace fuse::adventure {

/// True when `name` length is 1..24, the first character is an ASCII letter, and the name contains no spaces.
[[nodiscard]] inline bool verb_name_ok(std::string_view name) {
    if (name.size() < 1 || name.size() > 24) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    return name.find(' ') == std::string_view::npos;
}

} // namespace fuse::adventure
