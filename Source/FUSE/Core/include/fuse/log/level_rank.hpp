#pragma once

#include <string_view>

namespace fuse::log {

[[nodiscard]] inline int log_level_rank(std::string_view name) {
    if (name == "Trace") {
        return 0;
    }
    if (name == "Debug") {
        return 1;
    }
    if (name == "Info") {
        return 2;
    }
    if (name == "Warn") {
        return 3;
    }
    if (name == "Error") {
        return 4;
    }
    if (name == "Fatal") {
        return 5;
    }
    return -1;
}

} // namespace fuse::log
