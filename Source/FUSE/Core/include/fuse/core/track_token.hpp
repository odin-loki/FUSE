#pragma once

#include <string_view>

namespace fuse::core {

inline bool track_token_ok(std::string_view token) {
    return token == "B1" || token == "B2" || token == "B3" || token == "B4" || token == "B5" ||
           token == "B6" || token == "B7";
}

} // namespace fuse::core
