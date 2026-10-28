#pragma once

#include <string_view>

namespace fuse::platform {

/// True when `id` length is 1..32, the first character is an ASCII letter, and `id` contains no spaces.
[[nodiscard]] inline bool action_id_ok(std::string_view id) {
    if (id.size() < 1 || id.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(id.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    return id.find(' ') == std::string_view::npos;
}

} // namespace fuse::platform
