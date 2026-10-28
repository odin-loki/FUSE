#pragma once

// CPU check for Vulkan debug object names. Length 1..64; each character is an
// ASCII letter, digit, underscore, or slash.

#include <string_view>

namespace fuse::renderer {

[[nodiscard]] inline bool rhi_object_name_ok(std::string_view name) {
    if (name.size() < 1u || name.size() > 64u) {
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
