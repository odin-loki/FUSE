#pragma once

#include <string_view>

namespace fuse::ecs {

/// Component name: length 1..32. The first character is an uppercase ASCII letter.
/// Remaining characters are ASCII letters or digits.
[[nodiscard]] inline bool component_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (first < 'A' || first > 'Z') {
        return false;
    }
    for (const char raw : name.substr(1)) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::ecs
