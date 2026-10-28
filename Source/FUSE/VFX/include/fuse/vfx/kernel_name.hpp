#pragma once

#include <string_view>

namespace fuse::vfx {

/// True when `name` length is 1..48 and the first character is an ASCII letter.
[[nodiscard]] inline bool vfx_kernel_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 48) {
        return false;
    }
    const unsigned char c = static_cast<unsigned char>(name.front());
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::vfx
