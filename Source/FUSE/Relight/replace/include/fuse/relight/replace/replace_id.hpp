#pragma once

#include <string_view>

namespace fuse::relight {

/// Replacement id: length 1..32. The first character is an ASCII letter.
[[nodiscard]] inline bool replace_id_ok(std::string_view id) {
    if (id.size() < 1 || id.size() > 32) {
        return false;
    }
    const auto c = static_cast<unsigned char>(id.front());
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::relight
