// PDF output and printing (Qt QPdfWriter / QPrinter / QPrintPreviewWidget parity). PdfDocument writes PDF 1.4 with
// vector rectangles, rounded rectangles, lines, clipping, the 14 standard fonts (WinAnsi text), images with alpha and
// pages of any size. renderDrawList() turns a FUSE DrawList (any widget tree's paint output) into a vector page:
// rects / lines / clips stay vector, glyphs become stencil masks of the glyph atlas (crisp at print resolution),
// images keep their alpha. Units are PDF points (1/72 in); helpers convert from mm and px.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::pdf {

struct PageSize { f32 w, h; };
inline constexpr PageSize A4{595.276f, 841.89f}, A3{841.89f, 1190.55f}, A5{419.528f, 595.276f}, Letter{612.f, 792.f}, Legal{612.f, 1008.f};
constexpr f32 mm(f32 v) { return v * 72.f / 25.4f; }
inline PageSize landscape(PageSize p) { return {p.h, p.w}; }

enum class Font : u8 { Helvetica, HelveticaBold, HelveticaOblique, Times, TimesBold, TimesItalic, Courier, CourierBold };

/// Coordinates are top-left origin, y down (like the UI); the writer flips them.
class PdfDocument {
public:
    PdfDocument();
    void setTitle(std::string t) { m_title = std::move(t); }
    void setAuthor(std::string a) { m_author = std::move(a); }
    void newPage(PageSize size = A4);
    int pageCount() const { return int(m_pages.size()); }
    PageSize pageSize() const;

    void fillRect(const Rect& r, Color c, f32 radius = 0.f);
    void strokeRect(const Rect& r, Color c, f32 width, f32 radius = 0.f);
    void line(Vec2 a, Vec2 b, Color c, f32 width);
    void pushClip(const Rect& r);
    void popClip();
    /// Text at baseline `pos` (Latin-1 range; other code points become '?').
    void text(Vec2 pos, std::string_view utf8, f32 size, Color c, Font f = Font::Helvetica);
    /// Width of `utf8` in points for the standard font (Helvetica / Times metrics approximated, Courier exact).
    static f32 textWidth(std::string_view utf8, f32 size, Font f = Font::Helvetica);
    /// Simple word-wrapped paragraph in `box`; returns the y after the last line.
    f32 paragraph(const Rect& box, std::string_view utf8, f32 size, Color c, Font f = Font::Helvetica, f32 leading = 1.25f);
    /// RGBA8 (premultiplied, 0xAABBGGRR as in fuse::ui::Image) drawn into `r`.
    void image(const Image& img, const Rect& r);
    /// A DrawList (UI coordinates in px) at `origin` scaled by `scale` (pt per px), using `atlas` for glyphs and
    /// `images` for Image commands (the CpuRasterizer's registrations).
    void renderDrawList(const DrawList& dl, Vec2 origin, f32 scale, const u8* atlas, int atlasSize,
                        const Image* const* images = nullptr, u32 imageCount = 0);
    /// Serialised file.
    std::string finish() const;
    bool save(std::string_view path) const;

private:
    struct Page { PageSize size; std::string content; std::vector<int> xobjects; int clipDepth = 0; bool usesAtlas = false; };
    struct XObject { int w, h; std::string rgb, alpha; bool mask; int alphaRef = -1; bool alphaOnly = false; };
    Page& cur();
    void color(Color c, bool stroke);
    void roundPath(const Rect& r, f32 rad);
    std::string m_title, m_author;
    std::vector<Page> m_pages;
    std::vector<XObject> m_images;
    int m_atlasXo = -1;
    std::vector<std::pair<u32, int>> m_glyphInks; // packed colour -> xobject (1x1 ink soft-masked by the atlas)
    const u8* m_atlasPtr = nullptr;
};

/// Paginated preview model (QPrintPreviewWidget): page images rendered from a DrawList-producing callback.
struct PrintPreview {
    PageSize page = A4;
    f32 marginPt = mm(15.f);
    /// Splits a content height (px) into page bands of `pageContentPx` and returns band origins.
    static std::vector<f32> paginate(f32 contentHeightPx, f32 pageContentPx);
};

} // namespace fuse::ui::pdf
