#include "fuse/ui/extras/pdf.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::ui::pdf {
namespace {
std::string num(f32 v) {
    char b[32];
    if (std::abs(v - std::round(v)) < 1e-4f) std::snprintf(b, sizeof b, "%d", int(std::lround(v)));
    else { std::snprintf(b, sizeof b, "%.3f", double(v)); std::string s(b); while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); return s; }
    return b;
}
const char* fontName(Font f) {
    switch (f) {
    case Font::Helvetica: return "Helvetica"; case Font::HelveticaBold: return "Helvetica-Bold";
    case Font::HelveticaOblique: return "Helvetica-Oblique"; case Font::Times: return "Times-Roman";
    case Font::TimesBold: return "Times-Bold"; case Font::TimesItalic: return "Times-Italic";
    case Font::Courier: return "Courier"; case Font::CourierBold: return "Courier-Bold";
    }
    return "Helvetica";
}
constexpr int kFonts = 8;
// Latin-1 bytes from UTF-8
std::string latin1(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (n == 1) cp = c;
        else {
            cp = c & (0x3F >> (n - 1));
            for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        i += std::size_t(n);
        if (cp == 0x2019) cp = '\'';
        if (cp == 0x2013 || cp == 0x2014) cp = '-';
        o += cp < 256 ? char(cp) : '?';
    }
    return o;
}
std::string pdfString(std::string_view latin) {
    std::string o = "(";
    for (char c : latin) { if (c == '(' || c == ')' || c == '\\') o += '\\'; o += c; }
    return o + ")";
}
// Helvetica widths (1/1000 em) for ASCII 32..126 (AFM).
constexpr short kHelv[95] = {278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,556,556,333,500,278,556,500,722,500,500,500,334,260,334,584};
} // namespace

PdfDocument::PdfDocument() = default;
PdfDocument::Page& PdfDocument::cur() { if (m_pages.empty()) newPage(); return m_pages.back(); }
void PdfDocument::newPage(PageSize size) {
    if (!m_pages.empty()) while (m_pages.back().clipDepth > 0) popClip();
    m_pages.push_back({size, {}, {}, 0, false});
    // flip to a top-left, y-down coordinate system
    m_pages.back().content = "1 0 0 -1 0 " + num(size.h) + " cm\n";
}
PageSize PdfDocument::pageSize() const { return m_pages.empty() ? A4 : m_pages.back().size; }

void PdfDocument::color(Color c, bool stroke) {
    cur().content += num(std::clamp(c.r, 0.f, 1.f)) + " " + num(std::clamp(c.g, 0.f, 1.f)) + " " + num(std::clamp(c.b, 0.f, 1.f)) + (stroke ? " RG\n" : " rg\n");
}
void PdfDocument::roundPath(const Rect& r, f32 rad) {
    std::string& s = cur().content;
    rad = std::min({rad, r.w * 0.5f, r.h * 0.5f});
    if (rad <= 0.f) { s += num(r.x) + " " + num(r.y) + " " + num(r.w) + " " + num(r.h) + " re\n"; return; }
    const f32 k = rad * 0.5523f, x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    auto P = [&](f32 x, f32 y) { return num(x) + " " + num(y) + " "; };
    s += P(x0 + rad, y0) + "m\n" + P(x1 - rad, y0) + "l\n" + P(x1 - rad + k, y0) + P(x1, y0 + rad - k) + P(x1, y0 + rad) + "c\n";
    s += P(x1, y1 - rad) + "l\n" + P(x1, y1 - rad + k) + P(x1 - rad + k, y1) + P(x1 - rad, y1) + "c\n";
    s += P(x0 + rad, y1) + "l\n" + P(x0 + rad - k, y1) + P(x0, y1 - rad + k) + P(x0, y1 - rad) + "c\n";
    s += P(x0, y0 + rad) + "l\n" + P(x0, y0 + rad - k) + P(x0 + rad - k, y0) + P(x0 + rad, y0) + "c h\n";
}
namespace {
std::string alphaGs(f32 a, bool stroke) {
    if (a >= 0.999f) return {};
    const int q = std::clamp(int(std::lround(a * 20.f)), 0, 20); // 21 predeclared /GaN states
    return "/Ga" + std::to_string(q) + (stroke ? "" : "") + " gs\n";
}
} // namespace
void PdfDocument::fillRect(const Rect& r, Color c, f32 radius) {
    if (c.a <= 0.f || r.empty()) return;
    cur().content += "q\n" + alphaGs(c.a, false);
    color(c, false);
    roundPath(r, radius);
    cur().content += "f\nQ\n";
}
void PdfDocument::strokeRect(const Rect& r, Color c, f32 w, f32 radius) {
    if (c.a <= 0.f || w <= 0.f) return;
    cur().content += "q\n" + alphaGs(c.a, true) + num(w) + " w\n";
    color(c, true);
    roundPath({r.x + w * 0.5f, r.y + w * 0.5f, r.w - w, r.h - w}, std::max(0.f, radius - w * 0.5f));
    cur().content += "S\nQ\n";
}
void PdfDocument::line(Vec2 a, Vec2 b, Color c, f32 w) {
    if (c.a <= 0.f) return;
    cur().content += "q\n" + alphaGs(c.a, true) + num(w) + " w 1 J\n";
    color(c, true);
    cur().content += num(a.x) + " " + num(a.y) + " m " + num(b.x) + " " + num(b.y) + " l S\nQ\n";
}
void PdfDocument::pushClip(const Rect& r) {
    Page& p = cur();
    p.content += "q " + num(r.x) + " " + num(r.y) + " " + num(r.w) + " " + num(r.h) + " re W n\n";
    ++p.clipDepth;
}
void PdfDocument::popClip() {
    Page& p = cur();
    if (p.clipDepth <= 0) return;
    p.content += "Q\n";
    --p.clipDepth;
}
f32 PdfDocument::textWidth(std::string_view utf8, f32 size, Font f) {
    const std::string l = latin1(utf8);
    f32 w = 0.f;
    const bool courier = f == Font::Courier || f == Font::CourierBold;
    const f32 bold = (f == Font::HelveticaBold || f == Font::TimesBold) ? 1.06f : (f == Font::Times || f == Font::TimesItalic) ? 0.92f : 1.f;
    for (char ch : l) {
        const unsigned char c = (unsigned char)ch;
        w += courier ? 600.f : (c >= 32 && c < 127 ? f32(kHelv[c - 32]) : 556.f) * bold;
    }
    return w * size / 1000.f;
}
void PdfDocument::text(Vec2 pos, std::string_view utf8, f32 size, Color c, Font f) {
    if (c.a <= 0.f) return;
    std::string& s = cur().content;
    s += "q\n" + alphaGs(c.a, false);
    color(c, false);
    // undo the y flip locally so glyphs are upright
    s += "BT /F" + std::to_string(int(f)) + " " + num(size) + " Tf 1 0 0 -1 " + num(pos.x) + " " + num(pos.y) + " Tm " +
         pdfString(latin1(utf8)) + " Tj ET\nQ\n";
}
f32 PdfDocument::paragraph(const Rect& box, std::string_view utf8, f32 size, Color c, Font f, f32 leading) {
    f32 y = box.y + size;
    std::string lineBuf;
    auto flush = [&] { if (!lineBuf.empty()) { text({box.x, y}, lineBuf, size, c, f); lineBuf.clear(); } y += size * leading; };
    std::size_t i = 0;
    while (i <= utf8.size()) {
        std::size_t j = utf8.find_first_of(" \n", i);
        if (j == std::string_view::npos) j = utf8.size();
        std::string_view word = utf8.substr(i, j - i);
        const std::string cand = lineBuf.empty() ? std::string(word) : lineBuf + " " + std::string(word);
        if (!lineBuf.empty() && textWidth(cand, size, f) > box.w) { flush(); lineBuf = std::string(word); }
        else lineBuf = cand;
        if (j < utf8.size() && utf8[j] == '\n') flush();
        i = j + 1;
    }
    if (!lineBuf.empty()) flush();
    return y - size;
}
void PdfDocument::image(const Image& img, const Rect& r) {
    if (img.width <= 0 || img.height <= 0 || r.empty()) return;
    XObject x{img.width, img.height, {}, {}, false};
    x.rgb.resize(std::size_t(img.width) * img.height * 3);
    x.alpha.resize(std::size_t(img.width) * img.height);
    bool anyAlpha = false;
    for (std::size_t i = 0; i < img.pixels.size(); ++i) {
        const u32 p = img.pixels[i];
        const u32 a = p >> 24;
        auto un = [&](u32 v) { return char(a ? std::min<u32>(255, (v * 255 + a / 2) / a) : 0); };
        x.rgb[i * 3] = un(p & 0xFF);
        x.rgb[i * 3 + 1] = un((p >> 8) & 0xFF);
        x.rgb[i * 3 + 2] = un((p >> 16) & 0xFF);
        x.alpha[i] = char(a);
        anyAlpha |= a != 255;
    }
    if (!anyAlpha) x.alpha.clear();
    m_images.push_back(std::move(x));
    Page& p = cur();
    const int id = int(m_images.size()) - 1;
    p.xobjects.push_back(id);
    // image space is unit square with y up; with our flipped CTM map (0,1)->top-left
    p.content += "q " + num(r.w) + " 0 0 " + num(-r.h) + " " + num(r.x) + " " + num(r.bottom()) + " cm /Im" + std::to_string(id) + " Do Q\n";
}

void PdfDocument::renderDrawList(const DrawList& dl, Vec2 o, f32 k, const u8* atlas, int atlasSize, const Image* const* images, u32 imageCount) {
    int atlasId = -1;
    auto R = [&](const Rect& r) { return Rect{o.x + r.x * k, o.y + r.y * k, r.w * k, r.h * k}; };
    int depth = 0;
    for (const DrawCmd& c : dl.cmds()) {
        switch (c.op) {
        case DrawOp::Rect: {
            Color fill = c.fill;
            if (c.gradient) fill = {(c.fill.r + c.fill2.r) * 0.5f, (c.fill.g + c.fill2.g) * 0.5f, (c.fill.b + c.fill2.b) * 0.5f, (c.fill.a + c.fill2.a) * 0.5f};
            const f32 rad = c.radius[0] * k;
            fillRect(R(c.rect), fill, rad);
            if (c.borderWidth > 0.f) strokeRect(R(c.rect), c.border, c.borderWidth * k, rad);
            break;
        }
        case DrawOp::Line:
            line({o.x + c.rect.x * k, o.y + c.rect.y * k}, {o.x + c.rect.w * k, o.y + c.rect.h * k}, c.fill, std::max(0.1f, c.borderWidth * k));
            break;
        case DrawOp::PushClip: pushClip(R(c.rect)); ++depth; break;
        case DrawOp::PopClip: if (depth > 0) { popClip(); --depth; } break;
        case DrawOp::Glyph: {
            if (!atlas || atlasSize <= 0 || c.fill.a <= 0.f) break;
            if (atlasId < 0) {
                XObject x{atlasSize, atlasSize, {}, {}, true};
                const int rowBytes = (atlasSize + 7) / 8;
                x.rgb.assign(std::size_t(rowBytes) * atlasSize, 0);
                for (int y = 0; y < atlasSize; ++y)
                    for (int xx = 0; xx < atlasSize; ++xx)
                        if (atlas[std::size_t(y) * atlasSize + xx] >= 110) x.rgb[std::size_t(y) * rowBytes + xx / 8] |= char(0x80 >> (xx & 7));
                m_images.push_back(std::move(x));
                atlasId = int(m_images.size()) - 1;
                cur().xobjects.push_back(atlasId);
            }
            const Rect g = R(c.rect);
            std::string& s = cur().content;
            s += "q\n" + alphaGs(c.fill.a, false);
            color(c.fill, false);
            s += num(g.x) + " " + num(g.y) + " " + num(g.w) + " " + num(g.h) + " re W n\n";
            const f32 S = f32(atlasSize) * k, ax = g.x - f32(c.u) * k, ay = g.y - f32(c.v) * k;
            s += num(S) + " 0 0 " + num(-S) + " " + num(ax) + " " + num(ay + S) + " cm /Im" + std::to_string(atlasId) + " Do Q\n";
            break;
        }
        case DrawOp::Image:
            if (images && c.image < imageCount && images[c.image]) image(*images[c.image], R(c.rect));
            break;
        case DrawOp::Shadow: {
            Color sc = c.fill;
            sc.a *= 0.35f;
            fillRect(R(c.rect), sc, c.radius[0] * k);
            break;
        }
        case DrawOp::Backdrop: fillRect(R(c.rect), c.fill, c.radius[0] * k); break;
        }
    }
    while (depth-- > 0) popClip();
}

std::string PdfDocument::finish() const {
    std::vector<std::string> objs; // objs[i] is object i+1
    auto add = [&](std::string body) { objs.push_back(std::move(body)); return int(objs.size()); };
    auto stream = [](std::string dict, const std::string& data) {
        return "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
    };
    const int catalog = add(""), pagesId = add("");
    int fonts[kFonts];
    for (int f = 0; f < kFonts; ++f) fonts[f] = add(std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + fontName(Font(f)) + " /Encoding /WinAnsiEncoding >>");
    std::string gs = "<<";
    for (int q = 0; q <= 20; ++q) gs += " /Ga" + std::to_string(q) + " " + std::to_string(add("<< /Type /ExtGState /ca " + num(q / 20.f) + " /CA " + num(q / 20.f) + " >>")) + " 0 R";
    gs += " >>";
    std::vector<int> imgIds(m_images.size());
    for (std::size_t i = 0; i < m_images.size(); ++i) {
        const XObject& x = m_images[i];
        const std::string wh = "/Width " + std::to_string(x.w) + " /Height " + std::to_string(x.h);
        if (x.mask) { imgIds[i] = add(stream("/Type /XObject /Subtype /Image " + wh + " /ImageMask true /BitsPerComponent 1 /Decode [1 0]", x.rgb)); continue; }
        std::string smask;
        if (!x.alpha.empty()) smask = " /SMask " + std::to_string(add(stream("/Type /XObject /Subtype /Image " + wh + " /ColorSpace /DeviceGray /BitsPerComponent 8", x.alpha))) + " 0 R";
        imgIds[i] = add(stream("/Type /XObject /Subtype /Image " + wh + " /ColorSpace /DeviceRGB /BitsPerComponent 8" + smask, x.rgb));
    }
    std::string fontDict = "<<";
    for (int f = 0; f < kFonts; ++f) fontDict += " /F" + std::to_string(f) + " " + std::to_string(fonts[f]) + " 0 R";
    fontDict += " >>";
    std::string kids;
    const std::vector<Page> pages = m_pages.empty() ? std::vector<Page>{{A4, "", {}, 0, false}} : m_pages;
    for (const Page& p : pages) {
        std::string content = p.content;
        for (int d = 0; d < p.clipDepth; ++d) content += "Q\n";
        const int cs = add(stream("", content));
        std::string xo = "<<";
        for (int id : p.xobjects) xo += " /Im" + std::to_string(id) + " " + std::to_string(imgIds[std::size_t(id)]) + " 0 R";
        xo += " >>";
        const int pg = add("<< /Type /Page /Parent " + std::to_string(pagesId) + " 0 R /MediaBox [0 0 " + num(p.size.w) + " " + num(p.size.h) +
                           "] /Contents " + std::to_string(cs) + " 0 R /Resources << /Font " + fontDict + " /ExtGState " + gs + " /XObject " + xo + " >> >>");
        kids += std::to_string(pg) + " 0 R ";
    }
    objs[std::size_t(pagesId - 1)] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>";
    objs[std::size_t(catalog - 1)] = "<< /Type /Catalog /Pages " + std::to_string(pagesId) + " 0 R >>";
    auto esc = [](const std::string& s) { return pdfString(latin1(s)); };
    const int info = add("<< /Producer (FUSE GUI) /Title " + esc(m_title) + " /Author " + esc(m_author) + " >>");
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offs;
    for (std::size_t i = 0; i < objs.size(); ++i) {
        offs.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char b[24];
    for (std::size_t o : offs) { std::snprintf(b, sizeof b, "%010zu 00000 n \n", o); out += b; }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " + std::to_string(info) +
           " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}
bool PdfDocument::save(std::string_view path) const { return os::writeFileAtomic(path, finish()).has_value(); }

std::vector<f32> PrintPreview::paginate(f32 h, f32 page) {
    std::vector<f32> v;
    if (page <= 0.f) return v;
    for (f32 y = 0.f; y < std::max(h, 1.f); y += page) v.push_back(y);
    return v;
}

} // namespace fuse::ui::pdf
