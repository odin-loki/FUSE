#pragma once

#include <string_view>

namespace fuse::log {

[[nodiscard]] inline bool log_channel_ok(std::string_view name) {
    if (name == "Core") {
        return true;
    }
    if (name == "Renderer") {
        return true;
    }
    if (name == "Physics") {
        return true;
    }
    if (name == "Audio") {
        return true;
    }
    if (name == "Net") {
        return true;
    }
    return false;
}

} // namespace fuse::log
