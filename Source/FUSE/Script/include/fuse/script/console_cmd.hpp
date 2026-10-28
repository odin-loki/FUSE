#pragma once

#include <string_view>

namespace fuse::script {

[[nodiscard]] inline bool console_cmd_ok(std::string_view command) {
    if (command.empty() || command.size() > 48) {
        return false;
    }
    const char c = command.front();
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
        return false;
    }
    for (const char ch : command) {
        if (ch == ' ') {
            return false;
        }
    }
    return true;
}

} // namespace fuse::script
