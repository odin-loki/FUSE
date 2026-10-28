#pragma once

#include <string_view>

namespace fuse::adventure {

/// True when `title` is 1..48 characters and is not only spaces.
[[nodiscard]] inline bool examine_title_ok(std::string_view title) {
    if (title.empty() || title.size() > 48) {
        return false;
    }
    for (const char c : title) {
        if (c != ' ') {
            return true;
        }
    }
    return false;
}

} // namespace fuse::adventure
