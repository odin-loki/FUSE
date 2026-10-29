// FULL text profile (FUSE UI plan §4.8, decision D2): HarfBuzz shaping and FreeType rasterisation.
// Spike scope: shaping (ligatures, kerning via GPOS, marks, complex scripts in logical order) and unhinted grayscale
// FreeType coverage. Bidi reordering (SheenBidi) and UAX #14/#29 segmentation (libunibreak) are not wired yet.
#include "text_full.hpp"

#include <hb.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <cstring>

namespace fuse::text::detail {

namespace {
FT_Library ftLibrary() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        if (FT_Init_FreeType(&l) != 0) return FT_Library(nullptr);
        return l;
    }();
    return lib;
}
} // namespace

struct FullFace {
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    hb_buffer_t* buffer = nullptr; // reused: no allocation once it has grown
    FT_Face ft = nullptr;
    FT_F26Dot6 ftSize = -1;        // last FT_Set_Char_Size (26.6)
};

FullFace* fullCreate(std::span<const std::uint8_t> bytes) {
    auto* f = new FullFace;
    f->blob = hb_blob_create(reinterpret_cast<const char*>(bytes.data()), unsigned(bytes.size()),
                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f->face = hb_face_create(f->blob, 0);
    f->font = hb_font_create(f->face); // scale defaults to upem: positions come back in font units
    f->buffer = hb_buffer_create();
    FT_Library lib = ftLibrary();
    if (!lib || FT_New_Memory_Face(lib, bytes.data(), FT_Long(bytes.size()), 0, &f->ft) != 0 ||
        !hb_buffer_allocation_successful(f->buffer)) {
        fullDestroy(f);
        return nullptr;
    }
    return f;
}

void fullDestroy(FullFace* f) {
    if (!f) return;
    if (f->ft) FT_Done_Face(f->ft);
    hb_buffer_destroy(f->buffer);
    hb_font_destroy(f->font);
    hb_face_destroy(f->face);
    hb_blob_destroy(f->blob);
    delete f;
}

float fullShape(FullFace* f, float unitScale, std::string_view s, std::vector<ShapedGlyph>& out) {
    out.clear();
    hb_buffer_clear_contents(f->buffer);
    hb_buffer_add_utf8(f->buffer, s.data(), int(s.size()), 0, int(s.size()));
    hb_buffer_guess_segment_properties(f->buffer);
    hb_shape(f->font, f->buffer, nullptr, 0);
    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buffer, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buffer, &n);
    float pen = 0.f;
    for (unsigned i = 0; i < n; ++i) {
        const float adv = float(pos[i].x_advance) * unitScale;
        out.push_back({info[i].codepoint, info[i].cluster, pen + float(pos[i].x_offset) * unitScale, adv});
        pen += adv;
    }
    return pen;
}

namespace {
bool loadRendered(FullFace* f, std::uint32_t glyph, float emPx) {
    const FT_F26Dot6 size = FT_F26Dot6(std::lround(emPx * 64.f));
    if (size != f->ftSize) {
        if (FT_Set_Char_Size(f->ft, 0, size, 72, 72) != 0) return false;
        f->ftSize = size;
    }
    if (FT_Load_Glyph(f->ft, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return false;
    return FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL) == 0;
}
} // namespace

void fullGlyphBox(FullFace* f, std::uint32_t glyph, float emPx, int& x0, int& y0, int& x1, int& y1) {
    x0 = y0 = x1 = y1 = 0;
    if (!loadRendered(f, glyph, emPx)) return;
    const FT_GlyphSlot g = f->ft->glyph;
    x0 = g->bitmap_left;
    y0 = -g->bitmap_top;
    x1 = x0 + int(g->bitmap.width);
    y1 = y0 + int(g->bitmap.rows);
}

void fullRasterise(FullFace* f, std::uint32_t glyph, float emPx, std::uint8_t* dst, int w, int h, int stride) {
    if (!loadRendered(f, glyph, emPx)) return;
    const FT_Bitmap& b = f->ft->glyph->bitmap;
    const int rows = std::min(h, int(b.rows)), cols = std::min(w, int(b.width));
    for (int y = 0; y < rows; ++y) std::memcpy(dst + y * stride, b.buffer + y * b.pitch, std::size_t(cols));
}

} // namespace fuse::text::detail
