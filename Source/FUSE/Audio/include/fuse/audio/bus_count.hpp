#pragma once

namespace fuse::audio {

[[nodiscard]] inline bool bus_count_ok(int count) {
    return count >= 1 && count <= 32;
}

} // namespace fuse::audio
