#pragma once

#include <string_view>

namespace fuse::project {

/// True when `key` is 1..64 lowercase hex digits (`0-9`, `a-f`).
[[nodiscard]] inline bool cache_key_ok(std::string_view key) {
    if (key.empty() || key.size() > 64) {
        return false;
    }
    for (const char c : key) {
        const bool digit = c >= '0' && c <= '9';
        const bool lowerHex = c >= 'a' && c <= 'f';
        if (!digit && !lowerHex) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::project
