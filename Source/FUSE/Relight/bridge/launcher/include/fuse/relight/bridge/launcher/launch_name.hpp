// FUSE Relight: launch name check (length 1..32, leading ASCII letter).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0).
#pragma once

#include <cstddef>
#include <string_view>

namespace fuse::relight {

/// True when `name` has length 1..32 and its first character is an ASCII letter.
[[nodiscard]] inline bool launch_name_ok(std::string_view name) {
    const std::size_t n = name.size();
    if (n < 1 || n > 32) {
        return false;
    }
    const unsigned char c = static_cast<unsigned char>(name.front());
    const bool upper = c >= 'A' && c <= 'Z';
    const bool lower = c >= 'a' && c <= 'z';
    return upper || lower;
}

}  // namespace fuse::relight
