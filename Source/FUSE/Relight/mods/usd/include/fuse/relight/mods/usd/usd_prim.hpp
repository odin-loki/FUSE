// FUSE Relight: USD prim name shape.
//
// A prim name is accepted when its length is 1..48, the first character is an ASCII letter, and it
// contains no spaces.
#pragma once

#include <string_view>

namespace fuse::relight {

/// True when `name` length is 1..48, the first character is an ASCII letter, and `name` contains no spaces.
[[nodiscard]] inline bool usd_prim_ok(std::string_view name) {
    if (name.empty() || name.size() > 48) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    return name.find(' ') == std::string_view::npos;
}

} // namespace fuse::relight
