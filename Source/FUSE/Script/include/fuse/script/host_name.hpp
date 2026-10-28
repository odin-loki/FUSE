#pragma once

#include <string_view>

namespace fuse::script {

[[nodiscard]] inline bool host_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    const char c = name.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace fuse::script
