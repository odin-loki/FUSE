#pragma once

namespace fuse::renderer {

/// True when bounce count is in the inclusive range 1..8.
[[nodiscard]] inline bool rt_bounce_ok(int bounces) {
    return bounces >= 1 && bounces <= 8;
}

} // namespace fuse::renderer
