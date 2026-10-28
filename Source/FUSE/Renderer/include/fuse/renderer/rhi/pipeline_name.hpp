#pragma once

// CPU check for graphics pipeline names. Length 1..32; the first character is
// an ASCII letter; the name contains no spaces.

#include <string_view>

namespace fuse::renderer {

[[nodiscard]] inline bool pipeline_name_ok(std::string_view name) {
    if (name.size() < 1u || name.size() > 32u) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    const bool asciiLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!asciiLetter) {
        return false;
    }
    for (const char ch : name) {
        if (ch == ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::renderer
