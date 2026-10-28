#pragma once

#include <string_view>

namespace fuse::profiler {

[[nodiscard]] inline bool trace_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const char first = name.front();
    return (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
}

} // namespace fuse::profiler
