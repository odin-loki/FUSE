#pragma once

#include <string_view>

namespace fuse::profiler {

[[nodiscard]] inline bool assert_message_ok(std::string_view message) {
    return message.size() >= 1 && message.size() <= 256;
}

} // namespace fuse::profiler
