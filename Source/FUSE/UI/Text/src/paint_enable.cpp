// The opt-in colour glyph and MSDF rasterisers (glyph_paint.hpp): referencing them only from here keeps them out of
// binaries that never call enableColorGlyphs / enableMsdf. COOKED builds have them pre-rendered in the .fusefont
// packs, so enabling only switches the cooked images on.
#include "glyph_paint.hpp"

namespace fuse::text {

void enableColorGlyphs() {
    detail::PaintHooks& h = detail::paintHooks();
    h.colorEnabled = true;
#if !FUSE_TEXT_COOKED
    h.colrRender = &detail::colrV1Render;
    h.bitmapRender = &detail::bitmapGlyphRender;
    h.bitmapList = &detail::bitmapGlyphs;
#endif
}
bool colorGlyphsEnabled() { return detail::paintHooks().colorEnabled; }

void enableMsdf() {
    detail::PaintHooks& h = detail::paintHooks();
    h.msdfEnabled = true;
#if !FUSE_TEXT_COOKED
    h.msdf = &detail::generateMsdf;
#endif
}
bool msdfEnabled() { return detail::paintHooks().msdfEnabled; }

} // namespace fuse::text
