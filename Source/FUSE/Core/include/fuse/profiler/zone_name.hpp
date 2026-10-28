#pragma once

#include <string_view>

namespace fuse::profiler {

[[nodiscard]] inline bool zone_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 48) {
        return false;
    }
    const char first = name.front();
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
}

} // namespace fuse::profiler
