#pragma once

#include <string_view>

namespace fuse::project {

/// Cook output path: non-empty, at most 240 characters, and no ".." segment.
/// Both '/' and '\\' split segments.
[[nodiscard]] inline bool cook_output_ok(std::string_view path) {
    if (path.empty() || path.size() > 240) {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t slash = path.find_first_of("/\\", begin);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (path.substr(begin, end - begin) == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1;
    }
    return true;
}

} // namespace fuse::project
