#pragma once

#include <string_view>

namespace fuse::scene {

[[nodiscard]] inline bool parity_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 48) {
        return false;
    }
    const char c = name.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::scene
