#pragma once

// RT effect name: length 1..32. The first character is an ASCII letter.
// The name contains no spaces.

#include <string_view>

namespace fuse::renderer {

[[nodiscard]] inline bool rt_effect_ok(std::string_view name) {
    if (name.size() < 1u || name.size() > 32u) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    const bool asciiLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!asciiLetter) {
        return false;
    }
    return name.find(' ') == std::string_view::npos;
}

} // namespace fuse::renderer
