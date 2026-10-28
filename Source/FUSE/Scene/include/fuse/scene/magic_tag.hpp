#pragma once

#include <string_view>

namespace fuse::scene {

[[nodiscard]] inline bool magic_tag_ok(std::string_view tag) {
    return tag == "FUSE" || tag == "fuse";
}

} // namespace fuse::scene
