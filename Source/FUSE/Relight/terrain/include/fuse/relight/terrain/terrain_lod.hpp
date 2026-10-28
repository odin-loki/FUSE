// Terrain LOD: a level is valid only when it lies in [0, 8].
#pragma once

namespace fuse::relight {

/// True when `lod` lies in the inclusive range 0..8.
[[nodiscard]] inline bool terrain_lod_ok(int lod) {
    return lod >= 0 && lod <= 8;
}

} // namespace fuse::relight
