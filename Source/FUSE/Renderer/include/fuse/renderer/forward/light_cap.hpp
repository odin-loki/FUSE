#pragma once

namespace fuse::renderer {

[[nodiscard]] inline bool forward_light_cap_ok(int lights) {
    return lights >= 0 && lights <= 32;
}

} // namespace fuse::renderer
