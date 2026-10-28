#pragma once

#include <string_view>

namespace fuse::platform {

/// True when `title` length is 1..128 and the first character is an ASCII letter.
[[nodiscard]] inline bool rename_title_ok(std::string_view title) {
    if (title.size() < 1 || title.size() > 128) {
        return false;
    }
    const auto code = static_cast<unsigned char>(title.front());
    return (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z');
}

} // namespace fuse::platform
