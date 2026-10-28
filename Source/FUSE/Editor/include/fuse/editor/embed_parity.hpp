#pragma once

namespace fuse::editor {

/// Editor versus play-in-editor counts for the embed parity gate.
struct EmbedParity {
    int editorEntities;
    int pieEntities;
    int editorComponents;
    int pieComponents;
};

/// True when both entity counts are equal and greater than zero, and both component
/// counts are equal and at least the entity counts. Any negative count is not ok.
[[nodiscard]] inline bool embed_parity_ok(EmbedParity p) {
    if (p.editorEntities < 0 || p.pieEntities < 0 || p.editorComponents < 0 ||
        p.pieComponents < 0) {
        return false;
    }
    if (p.editorEntities != p.pieEntities || p.editorEntities <= 0) {
        return false;
    }
    return p.editorComponents == p.pieComponents && p.editorComponents >= p.editorEntities &&
           p.pieComponents >= p.pieEntities;
}

/// Signed entity gap: editorEntities minus pieEntities.
[[nodiscard]] inline int embed_parity_delta(EmbedParity p) {
    return p.editorEntities - p.pieEntities;
}

} // namespace fuse::editor
