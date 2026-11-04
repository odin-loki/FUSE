// The opt-in colour glyph and MSDF rasterisers (glyph_paint.hpp): referencing them only from here keeps them out of
// binaries that never call enableColorGlyphs / enableMsdf.
#include "glyph_paint.hpp"

namespace fuse::text {

void enableColorGlyphs() {
    detail::PaintHooks& h = detail::paintHooks();
    h.colrRender = &detail::colrV1Render;
    h.bitmapRender = &detail::bitmapGlyphRender;
    h.bitmapList = &detail::bitmapGlyphs;
}
bool colorGlyphsEnabled() { return detail::paintHooks().colrRender != nullptr; }

void enableMsdf() { detail::paintHooks().msdf = &detail::generateMsdf; }
bool msdfEnabled() { return detail::paintHooks().msdf != nullptr; }

} // namespace fuse::text
