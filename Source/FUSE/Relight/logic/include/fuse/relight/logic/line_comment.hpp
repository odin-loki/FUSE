#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fuse::relight::logic {

/// Drop a // comment that is outside double quotes. A quote toggles string state; backslashes are
/// ordinary characters. Trailing ASCII spaces left by the cut are removed. An empty result stays empty.
[[nodiscard]] inline std::string strip_line_comment(std::string_view line) {
    bool in_string = false;
    const std::size_t n = line.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char c = line[i];
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && c == '/' && i + 1 < n && line[i + 1] == '/') {
            std::size_t end = i;
            while (end > 0 && line[end - 1] == ' ') {
                --end;
            }
            return std::string(line.substr(0, end));
        }
    }
    return std::string(line);
}

} // namespace fuse::relight::logic
