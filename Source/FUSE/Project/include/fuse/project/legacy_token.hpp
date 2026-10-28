#pragma once

#include <string_view>

namespace fuse::project {

/// True when `token` length is 1..32, the first character is an ASCII letter, and the token contains no spaces.
[[nodiscard]] inline bool legacy_token_ok(std::string_view token) {
    if (token.size() < 1 || token.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(token.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    return token.find(' ') == std::string_view::npos;
}

} // namespace fuse::project
