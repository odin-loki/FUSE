#pragma once

namespace fuse::scene {

[[nodiscard]] inline bool serial_version_ok(int version) {
    return version >= 1 && version <= 16;
}

} // namespace fuse::scene
