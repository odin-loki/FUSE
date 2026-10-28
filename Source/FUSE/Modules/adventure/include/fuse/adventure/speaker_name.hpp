#pragma once

#include <string_view>

namespace fuse::adventure {

/// Speaker name: length 1..24. The first character is an ASCII letter.
/// Remaining characters are ASCII letters, spaces, or apostrophes.
[[nodiscard]] inline bool speaker_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 24) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(name.front());
    if ((first < 'A' || first > 'Z') && (first < 'a' || first > 'z')) {
        return false;
    }
    for (const char raw : name.substr(1)) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!letter && c != ' ' && c != '\'') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::adventure
