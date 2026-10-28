#pragma once

#include <string_view>

namespace fuse::project {

/// True when `path` is non-empty, at most 180 characters, and contains no ".."
/// segment. Segments split on '/' or '\\'. A drive letter or leading slash is allowed.
[[nodiscard]] inline bool legacy_path_ok(std::string_view path) {
    if (path.empty() || path.size() > 180) {
        return false;
    }
    std::size_t begin = 0;
    for (;;) {
        const std::size_t sep = path.find_first_of("/\\", begin);
        const std::size_t end = sep == std::string_view::npos ? path.size() : sep;
        if (path.substr(begin, end - begin) == "..") {
            return false;
        }
        if (sep == std::string_view::npos) {
            return true;
        }
        begin = sep + 1;
    }
}

} // namespace fuse::project
