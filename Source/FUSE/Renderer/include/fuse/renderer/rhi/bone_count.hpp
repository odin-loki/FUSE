#pragma once

// CPU check for skinning bone counts. Valid range is 1..256.

namespace fuse::renderer {

[[nodiscard]] inline bool bone_count_ok(int count) {
    return count >= 1 && count <= 256;
}

} // namespace fuse::renderer
