#pragma once

#include <string_view>

namespace fuse::editor {

/// True when `name` length is 1..32 and the first character is an ASCII letter.
[[nodiscard]] inline bool slice_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const unsigned char c = static_cast<unsigned char>(name.front());
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::editor
