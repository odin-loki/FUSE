#pragma once

#include <string_view>

namespace fuse::world3d {

/// Level name: length 1..32. The first character is an ASCII letter.
/// The name contains no spaces.
[[nodiscard]] inline bool level_name_ok(std::string_view name) {
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

} // namespace fuse::world3d
