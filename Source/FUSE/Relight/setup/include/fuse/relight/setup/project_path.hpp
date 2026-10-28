// FUSE Relight setup: project-root path check.
//
// project_path_status() accepts a path as a project root when it has a non-whitespace character, is at
// most 240 characters, and has no ".." segment. Segments split on '/' and '\\'. A leading slash or a
// drive letter is a normal path and is Ok.
#pragma once

#include <string_view>

namespace fuse::relight::setup {

enum class ProjectPathStatus { Ok, Empty, RelativeEscape, TooLong };

/// Empty or whitespace-only is Empty. Longer than 240 characters is TooLong. A ".." segment is
/// RelativeEscape. Otherwise Ok.
inline ProjectPathStatus project_path_status(std::string_view path) {
    const auto whitespace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    bool any = false;
    for (const char c : path) {
        if (!whitespace(c)) {
            any = true;
            break;
        }
    }
    if (!any) {
        return ProjectPathStatus::Empty;
    }
    if (path.size() > 240) {
        return ProjectPathStatus::TooLong;
    }
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/' && path[i] != '\\') {
            continue;
        }
        if (path.substr(begin, i - begin) == "..") {
            return ProjectPathStatus::RelativeEscape;
        }
        begin = i + 1;
    }
    return ProjectPathStatus::Ok;
}

} // namespace fuse::relight::setup
