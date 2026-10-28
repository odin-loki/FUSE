#pragma once

#include <string_view>

namespace fuse::core {

/// Service name: length 1..32. The first character is an ASCII letter.
/// Remaining characters are ASCII letters, digits, or underscore.
[[nodiscard]] inline bool service_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const auto is_letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!is_letter(first)) {
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

} // namespace fuse::core
