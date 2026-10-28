#pragma once

#include <string_view>

namespace fuse::scene {

[[nodiscard]] inline bool pack_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const char c = name.front();
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
        return false;
    }
    for (const char ch : name) {
        if (ch == ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::scene
