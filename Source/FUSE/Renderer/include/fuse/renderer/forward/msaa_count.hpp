#pragma once

namespace fuse::renderer {

/// True only for forward MSAA sample counts 1, 2, 4, and 8.
[[nodiscard]] inline bool forward_msaa_ok(int samples) {
    return samples == 1 || samples == 2 || samples == 4 || samples == 8;
}

} // namespace fuse::renderer
