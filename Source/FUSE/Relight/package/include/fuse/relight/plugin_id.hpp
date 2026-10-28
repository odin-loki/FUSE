#pragma once

#include <string_view>

namespace fuse::relight {

/// Plugin id: length 1..32. The first character is an ASCII letter. The id contains no spaces.
[[nodiscard]] inline bool plugin_id_ok(std::string_view id) {
    if (id.empty() || id.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(id.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    for (const char raw : id) {
        if (raw == ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::relight
