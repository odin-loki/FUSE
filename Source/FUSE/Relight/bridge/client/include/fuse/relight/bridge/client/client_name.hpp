#pragma once

#include <string_view>

namespace fuse::relight {

/// Client name: length 1..32. The first character is an ASCII letter.
[[nodiscard]] inline bool client_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
}

} // namespace fuse::relight
