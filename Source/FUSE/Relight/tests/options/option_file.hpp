// FUSE Relight: option file name shape.
//
// A name is accepted when its length is 1..32, the first character is an ASCII letter, and it
// contains no spaces.
#pragma once

#include <string_view>

namespace fuse::relight {

/// True when `name` length is 1..32, the first character is an ASCII letter, and `name` contains no spaces.
[[nodiscard]] inline bool option_file_ok(std::string_view name) {
    if (name.size() < 1 || name.size() > 32) {
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
