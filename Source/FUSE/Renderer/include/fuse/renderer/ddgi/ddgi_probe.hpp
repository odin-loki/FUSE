#pragma once

namespace fuse::renderer {

/// True when `edge` is a power of two in the closed interval [8, 64].
[[nodiscard]] inline bool ddgi_probe_ok(int edge) {
    if (edge < 8 || edge > 64) {
        return false;
    }
    return (edge & (edge - 1)) == 0;
}

} // namespace fuse::renderer
