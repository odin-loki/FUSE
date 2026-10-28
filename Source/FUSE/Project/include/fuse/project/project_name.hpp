#pragma once

#include <string_view>

namespace fuse::project {

/// Project name: length 1..48. The first character is an ASCII letter.
/// Remaining characters are ASCII letters, digits, space, underscore, or hyphen.
[[nodiscard]] inline bool project_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 48) {
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
        if (!is_letter(c) && !digit && c != ' ' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::project
