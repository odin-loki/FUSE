#pragma once

#include <string_view>

namespace fuse::physics {

/// True when length is 1..32 and the first character is an ASCII letter.
[[nodiscard]] inline bool deliverable_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
}

} // namespace fuse::physics
