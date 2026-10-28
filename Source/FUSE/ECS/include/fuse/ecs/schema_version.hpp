#pragma once

namespace fuse::ecs {

/// True when `version` is in the inclusive range 1..32.
[[nodiscard]] inline bool schema_version_ok(int version) {
    return version >= 1 && version <= 32;
}

} // namespace fuse::ecs
