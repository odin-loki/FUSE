// FUSE UI (UI2): CPU reference rasteriser — the specification for every draw-list primitive (FUSE_UI_PLAN.md
// §4.9) and the headless fallback. Pixels are premultiplied RGBA8, source-over. Rounded rects use an analytic
// signed-distance coverage with a 1 px ramp; text is the built-in 5x7 font at integer scale.
#pragma once

#include <fuse/ui/draw_list.hpp>

#include <string>
#include <vector>

namespace fuse::ui {

struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba; ///< premultiplied RGBA8, row-major, top row first

    void resize(u32 w, u32 h, Color clearColor = {});
    Color at(u32 x, u32 y) const;
    u64 hash() const; ///< FNV-1a over the pixels (golden checks)
};

/// Rasterise `list` into `target`. When `damage` is non-empty only pixels inside it are touched.
void rasterize(const DrawList& list, Image& target, const Rect& damage = {});

/// Write `image` as a binary PPM (P6, alpha dropped). Returns false on I/O failure.
bool writePpm(const Image& image, const std::string& path);

} // namespace fuse::ui
