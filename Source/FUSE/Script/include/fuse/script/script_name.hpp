#pragma once

#include <string_view>

namespace fuse::script {

/// Script name: length 1..64. The first character is an ASCII letter or '_'.
/// Remaining characters are ASCII letters, digits, or '_'.
[[nodiscard]] inline bool script_name_valid(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!is_letter(first) && first != '_') {
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
