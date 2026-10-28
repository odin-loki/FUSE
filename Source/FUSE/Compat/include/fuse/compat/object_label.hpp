#pragma once

#include <string_view>

namespace fuse::compat {

/// True when `label` length is 1..64 and the first character is an ASCII letter.
[[nodiscard]] inline bool object_label_ok(std::string_view label) {
    if (label.empty() || label.size() > 64) {
        return false;
    }
    const unsigned char c = static_cast<unsigned char>(label.front());
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::compat
