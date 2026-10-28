#pragma once

#include <string_view>

namespace fuse::relight {

/// True when `key` length is 1..32, the first character is an ASCII letter, and the key contains no spaces.
[[nodiscard]] inline bool export_key_ok(std::string_view key) {
    if (key.size() < 1 || key.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(key.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    return key.find(' ') == std::string_view::npos;
}

} // namespace fuse::relight
