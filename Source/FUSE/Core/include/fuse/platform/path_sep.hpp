#pragma once

#include <string_view>

namespace fuse::platform {

/// True when `path` contains '/' or '\\'.
[[nodiscard]] inline bool path_has_sep(std::string_view path) {
    return path.find_first_of("/\\") != std::string_view::npos;
}

} // namespace fuse::platform
