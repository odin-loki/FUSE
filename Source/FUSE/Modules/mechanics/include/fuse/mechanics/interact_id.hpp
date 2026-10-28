#pragma once

#include <string_view>

namespace fuse::mechanics {

/// Interact id: length 1..32. The first character is a lowercase ASCII letter.
/// Remaining characters are lowercase ASCII letters, digits, or '_'.
[[nodiscard]] inline bool interact_id_ok(std::string_view id) {
    if (id.empty() || id.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(id.front());
    if (first < 'a' || first > 'z') {
        return false;
    }
    for (const char raw : id.substr(1)) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !digit && c != '_') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::mechanics
