#pragma once

#include <string_view>

namespace fuse::net {

/// True only when the length is 1..32, the first character is an ASCII letter, and the name contains no spaces.
[[nodiscard]] inline bool suite_name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))) {
        return false;
    }
    for (const char raw : name) {
        if (raw == ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::net
