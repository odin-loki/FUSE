#pragma once

// Material name: length 1..48. Every character is an ASCII letter, digit,
// underscore, or slash.

#include <string_view>

namespace fuse::renderer {

[[nodiscard]] inline bool material_name_ok(std::string_view name) {
    if (name.size() < 1u || name.size() > 48u) {
        return false;
    }
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        const bool asciiLetter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool asciiDigit = c >= '0' && c <= '9';
        if (!asciiLetter && !asciiDigit && c != '_' && c != '/') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::renderer
