#pragma once

#include <string_view>

namespace fuse::platform {

/// True when `title` length is 1..128 and every character is printable ASCII (code 32..126).
[[nodiscard]] inline bool window_title_ok(std::string_view title) {
    if (title.size() < 1 || title.size() > 128) {
        return false;
    }
    for (const char ch : title) {
        const auto code = static_cast<unsigned char>(ch);
        if (code < 32u || code > 126u) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::platform
