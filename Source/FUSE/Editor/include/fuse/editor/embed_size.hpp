#pragma once

namespace fuse::editor {

/// Client size of an embedded viewport, in pixels.
struct EmbedSize {
    int w;
    int h;
};

/// Clamp an embed client size so each edge lies in `[0, maxEdge]`.
/// `maxEdge` below 1 is treated as 1. A negative width or height becomes 0.
[[nodiscard]] inline EmbedSize clamp_embed_size(int w, int h, int maxEdge) {
    if (maxEdge < 1) {
        maxEdge = 1;
    }
    if (w < 0) {
        w = 0;
    } else if (w > maxEdge) {
        w = maxEdge;
    }
    if (h < 0) {
        h = 0;
    } else if (h > maxEdge) {
        h = maxEdge;
    }
    return EmbedSize{w, h};
}

/// True when both edges are positive, so the embed client has area.
[[nodiscard]] inline bool embed_size_visible(EmbedSize size) {
    return size.w > 0 && size.h > 0;
}

} // namespace fuse::editor
