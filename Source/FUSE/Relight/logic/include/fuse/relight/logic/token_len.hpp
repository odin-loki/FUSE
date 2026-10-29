#pragma once

#include <cstddef>
#include <string_view>

namespace fuse::relight::logic {

/// True when \p token has length 1..64 and its first character is an ASCII letter.
[[nodiscard]] inline bool logic_token_ok(std::string_view token) {
    const std::size_t n = token.size();
    if (n < 1 || n > 64) {
        return false;
    }
    const char c = token.front();
    const bool upper = c >= 'A' && c <= 'Z';
    const bool lower = c >= 'a' && c <= 'z';
    return upper || lower;
}

} // namespace fuse::relight::logic
