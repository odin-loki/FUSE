#pragma once

#include <string_view>

namespace fuse::project {

/// True when both names contain a non-space character and the names differ.
/// Spaces are not trimmed; a name with no non-space character is empty.
[[nodiscard]] inline bool dep_edge_ok(std::string_view from, std::string_view to) {
    const auto has_non_space = [](std::string_view name) {
        for (const char ch : name) {
            if (ch != ' ') {
                return true;
            }
        }
        return false;
    };
    return has_non_space(from) && has_non_space(to) && from != to;
}

} // namespace fuse::project
