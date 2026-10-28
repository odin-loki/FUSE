#pragma once

// Volk symbol name: length 1..64. The first character is an ASCII letter.
// The name contains no spaces.

#include <string_view>

namespace fuse::renderer {

[[nodiscard]] inline bool volk_symbol_ok(std::string_view name) {
    if (name.size() < 1u || name.size() > 64u) {
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
