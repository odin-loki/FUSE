#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fuse::editor {

inline std::string truncate_console_line(std::string_view line, std::size_t maxChars) {
    if (maxChars == 0) {
        return {};
    }
    if (line.size() <= maxChars) {
        return std::string(line);
    }
    if (maxChars < 3) {
        return std::string(line.substr(0, maxChars));
    }
    std::string out(line.substr(0, maxChars - 3));
    out.append("...");
    return out;
}

} // namespace fuse::editor
