#pragma once

#include <string_view>

namespace fuse::editor {

/// True when `title` length is 1..32 and every character is an ASCII letter, digit, or space.
[[nodiscard]] inline bool panel_title_ok(std::string_view title) {
    if (title.empty() || title.size() > 32) {
        return false;
    }
    for (const unsigned char c : title) {
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::editor
