#pragma once

namespace fuse::editor {

/// Per-host dirty bits for scene, selection, and assets.
/// Flags start clean. Marking ORs bits on; clearing turns every bit off.
struct HostDirty {
    bool scene = false;
    bool selection = false;
    bool assets = false;
};

inline void host_mark_dirty(HostDirty& dirty, bool scene, bool selection, bool assets) {
    dirty.scene = dirty.scene || scene;
    dirty.selection = dirty.selection || selection;
    dirty.assets = dirty.assets || assets;
}

inline void host_clear_dirty(HostDirty& dirty) {
    dirty.scene = false;
    dirty.selection = false;
    dirty.assets = false;
}

[[nodiscard]] inline bool host_any_dirty(HostDirty dirty) {
    return dirty.scene || dirty.selection || dirty.assets;
}

} // namespace fuse::editor
