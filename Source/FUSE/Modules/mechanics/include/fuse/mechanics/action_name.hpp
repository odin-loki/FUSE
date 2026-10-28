#pragma once

#include <string_view>

namespace fuse::mechanics {

/// Action name: length 1..32. The first character is a lowercase ASCII letter.
/// Remaining characters are lowercase letters, digits, or underscore.
[[nodiscard]] inline bool action_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (first < 'a' || first > 'z') {
        return false;
    }
    for (const char raw : name.substr(1)) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool letter = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '_') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::mechanics
