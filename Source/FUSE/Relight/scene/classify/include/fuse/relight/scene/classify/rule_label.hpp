#pragma once

#include <string_view>

namespace fuse::relight {

/// A rule label is 1..24 characters and starts with an ASCII letter.
[[nodiscard]] inline bool rule_label_ok(std::string_view label) {
    if (label.size() < 1 || label.size() > 24) {
        return false;
    }
    const auto c = static_cast<unsigned char>(label.front());
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::relight
