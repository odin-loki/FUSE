#pragma once

namespace fuse::renderer {

/// True only for channel counts 1 through 4 inclusive.
[[nodiscard]] inline bool image_channel_ok(int channels) {
    return channels >= 1 && channels <= 4;
}

} // namespace fuse::renderer
