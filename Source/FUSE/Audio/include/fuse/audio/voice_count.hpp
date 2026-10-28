#pragma once

namespace fuse::audio {

[[nodiscard]] inline bool voice_count_ok(int count) {
    return count >= 1 && count <= 256;
}

} // namespace fuse::audio
