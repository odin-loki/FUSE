#pragma once

#include <string_view>

namespace fuse::script {

/// Route name: length 1..64. The first character is an ASCII letter.
/// Remaining characters are ASCII letters, digits, or '_'.
/// Empty, a leading digit, a slash, or a dot is invalid.
[[nodiscard]] inline bool script_route_ok(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (!is_letter(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    for (const char raw : name.substr(1)) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool digit = c >= '0' && c <= '9';
        if (!is_letter(c) && !digit && c != '_') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::script
