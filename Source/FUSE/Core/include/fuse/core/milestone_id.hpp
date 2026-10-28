#pragma once

#include <string_view>

namespace fuse::core {

inline bool milestone_id_ok(std::string_view id) {
    return id == "U1" || id == "U2" || id == "U3" || id == "U4" || id == "U5" || id == "U6" ||
           id == "U7" || id == "U8";
}

} // namespace fuse::core
