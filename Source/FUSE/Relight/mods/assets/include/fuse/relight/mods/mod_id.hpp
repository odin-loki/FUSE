#pragma once

#include <string_view>

namespace fuse::relight::mods {

// 1..32 characters: a leading lowercase letter, then lowercase letters, digits, or '_'.
[[nodiscard]] inline bool mod_id_valid(std::string_view id) {
    if (id.empty() || id.size() > 32) {
        return false;
    }
    if (id.front() < 'a' || id.front() > 'z') {
        return false;
    }
    for (const char c : id.substr(1)) {
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !digit && c != '_') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::relight::mods
