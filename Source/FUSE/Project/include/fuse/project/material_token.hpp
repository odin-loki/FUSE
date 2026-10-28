#pragma once

#include <string_view>

namespace fuse::project {

/// Material token: length 1..40. The first character is an ASCII letter.
/// Remaining characters are ASCII letters, digits, underscore, or slash.
/// A token containing ".." is rejected.
[[nodiscard]] inline bool material_token_ok(std::string_view token) {
    if (token.empty() || token.size() > 40) {
        return false;
    }
    if (token.find("..") != std::string_view::npos) {
        return false;
    }
    const auto is_letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (!is_letter(static_cast<unsigned char>(token.front()))) {
        return false;
    }
    for (const char raw : token.substr(1)) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool digit = c >= '0' && c <= '9';
        if (!is_letter(c) && !digit && c != '_' && c != '/') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::project
