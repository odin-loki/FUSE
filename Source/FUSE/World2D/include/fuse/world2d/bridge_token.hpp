#pragma once

#include <string_view>

namespace fuse::world2d {

/// Bridge token: length 1..32. The first character is an ASCII letter.
/// The token contains no spaces.
[[nodiscard]] inline bool bridge_token_ok(std::string_view token) {
    if (token.empty() || token.size() > 32) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(token.front());
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    for (const char raw : token) {
        if (raw == ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::world2d
