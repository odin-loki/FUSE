// COLRv1 renderer: walks a base glyph's paint graph (OpenType COLR v1, paint formats 1..32) and renders it into a
// premultiplied RGBA8 bitmap. Variable formats use their default values (no deltas applied). Every table read is
// bounds-checked against the font file; recursion depth and visited nodes are capped (cycles, huge graphs).
//
// Internals: float premultiplied RGBA layers in bitmap pixel coordinates; each layer covers a sub-rectangle of the
// bitmap (the region that can still change the result). A transform T maps font units of the current paint to bitmap
// pixels (y down). Groups (PaintGlyph children, composites, clipped PaintColrGlyph) render into temporary layers.
#include "glyph_paint.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <vector>

namespace fuse::text::detail {
namespace {

constexpr int kMaxDepth = 32;
constexpr int kMaxNodes = 10000;
constexpr int kMaxDim = 1024;                          // bitmap side
constexpr std::int64_t kMaxPixels = std::int64_t(1) << 18; // bitmap area (512 x 512)
constexpr std::int64_t kWorkPerPixel = 512;            // pixel operations per bitmap pixel
constexpr std::int64_t kLivePerPixel = 16;             // temporary layer pixels alive at once per bitmap pixel

struct Rgba {
    float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
};

Rgba operator*(const Rgba& c, float k) { return {c.r * k, c.g * k, c.b * k, c.a * k}; }

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

Rect intersect(const Rect& a, const Rect& b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

// A premultiplied layer covering rect r of the bitmap.
struct Layer {
    Rect r;
    std::vector<Rgba> px;
    explicit Layer(const Rect& rect) : r(rect), px(rect.empty() ? 0 : std::size_t(rect.w()) * std::size_t(rect.h())) {}
    Rgba& at(int x, int y) { return px[std::size_t(y - r.y0) * std::size_t(r.w()) + std::size_t(x - r.x0)]; }
};

// Coverage (0..1) over rect r of the bitmap.
struct Mask {
    Rect r;
    std::vector<float> cov;
    float at(int x, int y) const { return cov[std::size_t(y - r.y0) * std::size_t(r.w()) + std::size_t(x - r.x0)]; }
};

// Axis-aligned bounds accumulator (doubles, empty until the first point).
struct Box {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    bool empty() const { return x1 < x0 || y1 < y0; }
    void add(double x, double y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
};

Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
Affine scaling(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
Affine around(const Affine& m, double cx, double cy) { return translate(cx, cy) * m * translate(-cx, -cy); }

bool invert(const Affine& m, Affine& out) {
    const double det = m.xx * m.yy - m.xy * m.yx;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    const double id = 1.0 / det;
    out.xx = m.yy * id;
    out.xy = -m.xy * id;
    out.yx = -m.yx * id;
    out.yy = m.xx * id;
    out.dx = -(out.xx * m.dx + out.xy * m.dy);
    out.dy = -(out.yx * m.dx + out.yy * m.dy);
    return true;
}

struct Stop {
    float off;
    Rgba c;
};

enum class Extend : std::uint8_t { Pad, Repeat, Reflect };

struct ColorLine {
    std::vector<Stop> stops;
    Extend extend = Extend::Pad;

    Rgba at(double t) const {
        const double lo = stops.front().off, hi = stops.back().off;
        if (hi - lo > 1e-9) {
            double u = (t - lo) / (hi - lo);
            if (extend == Extend::Repeat) u -= std::floor(u);
            else if (extend == Extend::Reflect) {
                u = std::fmod(std::abs(u), 2.0);
                if (u > 1.0) u = 2.0 - u;
            }
            t = lo + u * (hi - lo);
        }
        if (t <= lo) return stops.front().c;
        if (t >= hi) return stops.back().c;
        const auto it = std::upper_bound(stops.begin(), stops.end(), t, [](double v, const Stop& s) { return v < s.off; });
        const Stop& b = *it;
        const Stop& a = *(it - 1);
        const float span = b.off - a.off;
        const float f = span > 0.f ? float((t - a.off) / span) : 1.f;
        return {a.c.r + (b.c.r - a.c.r) * f, a.c.g + (b.c.g - a.c.g) * f, a.c.b + (b.c.b - a.c.b) * f,
                a.c.a + (b.c.a - a.c.a) * f};
    }
};

// Per-pixel colour source: a solid or one of the three gradients, evaluated in its own font space.
struct Shader {
    enum Kind : std::uint8_t { Solid, Linear, Radial, Sweep } kind = Solid;
    Rgba solid;
    ColorLine line;
    Affine inv; // bitmap -> font space
    double x0 = 0, y0 = 0, dx = 0, dy = 0;          // linear: t = (p - p0) . (dx, dy); sweep: centre (x0, y0)
    double cdx = 0, cdy = 0, r0 = 0, dr = 0, a = 0; // radial (two-circle) parameters
    double start = 0, end = 0;                      // sweep angles (degrees)

    // false: the pixel is not painted (e.g. no valid radial t).
    bool eval(double px, double py, Rgba& out) const {
        if (kind == Solid) {
            out = solid;
            return true;
        }
        const double x = inv.xx * px + inv.xy * py + inv.dx, y = inv.yx * px + inv.yy * py + inv.dy;
        double t = 0;
        if (kind == Linear) {
            t = (x - x0) * dx + (y - y0) * dy;
        } else if (kind == Radial) {
            // largest w with |p - c(w)| = r(w) >= 0, c(w) = c0 + w cd, r(w) = r0 + w dr
            const double pdx = x - x0, pdy = y - y0;
            const double b = pdx * cdx + pdy * cdy + r0 * dr;
            const double c = pdx * pdx + pdy * pdy - r0 * r0;
            if (std::abs(a) < 1e-9) {
                if (std::abs(b) < 1e-12) return false;
                t = c / (2.0 * b);
                if (r0 + t * dr < 0.0) return false;
            } else {
                const double disc = b * b - a * c;
                if (disc < 0.0) return false;
                const double sq = std::sqrt(disc);
                const double t1 = (b + sq) / a, t2 = (b - sq) / a;
                const double hiT = std::max(t1, t2), loT = std::min(t1, t2);
                if (r0 + hiT * dr >= 0.0) t = hiT;
                else if (r0 + loT * dr >= 0.0) t = loT;
                else return false;
            }
        } else { // Sweep: counter-clockwise degrees in [0, 360)
            double ang = std::atan2(y - y0, x - x0) * (180.0 / std::numbers::pi);
            if (ang < 0.0) ang += 360.0;
            if (ang >= 360.0) ang = 0.0;
            if (end == start) {
                if (line.extend != Extend::Pad) return false;
                out = ang < start ? line.stops.front().c : line.stops.back().c;
                return true;
            }
            t = (ang - start) / (end - start);
        }
        out = line.at(t);
        return true;
    }
};

// W3C Compositing and Blending Level 1 helpers (unpremultiplied colours).
struct Rgb {
    float r, g, b;
};
float lum(const Rgb& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }
Rgb clipColor(Rgb c) {
    const float l = lum(c);
    const float n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0.f && l - n > 0.f) {
        c = {l + (c.r - l) * l / (l - n), l + (c.g - l) * l / (l - n), l + (c.b - l) * l / (l - n)};
    }
    if (x > 1.f && x - l > 0.f) {
        c = {l + (c.r - l) * (1.f - l) / (x - l), l + (c.g - l) * (1.f - l) / (x - l), l + (c.b - l) * (1.f - l) / (x - l)};
    }
    return c;
}
Rgb setLum(const Rgb& c, float l) {
    const float d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}
float sat(const Rgb& c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }
Rgb setSat(Rgb c, float s) {
    float* ch[3] = {&c.r, &c.g, &c.b};
    std::sort(ch, ch + 3, [](const float* p, const float* q) { return *p < *q; });
    float& mn = *ch[0];
    float& mid = *ch[1];
    float& mx = *ch[2];
    if (mx > mn) {
        mid = (mid - mn) * s / (mx - mn);
        mx = s;
    } else {
        mid = mx = 0.f;
    }
    mn = 0.f;
    return c;
}

float blendChannel(int mode, float cb, float cs) {
    switch (mode) {
    case 13: return cb + cs - cb * cs;                                                       // screen
    case 14: return cb <= 0.5f ? cs * 2.f * cb : cs + (2.f * cb - 1.f) - cs * (2.f * cb - 1.f); // overlay
    case 15: return std::min(cb, cs);                                                        // darken
    case 16: return std::max(cb, cs);                                                        // lighten
    case 17:                                                                                 // color-dodge
        if (cb <= 0.f) return 0.f;
        if (cs >= 1.f) return 1.f;
        return std::min(1.f, cb / (1.f - cs));
    case 18: // color-burn
        if (cb >= 1.f) return 1.f;
        if (cs <= 0.f) return 0.f;
        return 1.f - std::min(1.f, (1.f - cb) / cs);
    case 19: return cs <= 0.5f ? cb * 2.f * cs : cb + (2.f * cs - 1.f) - cb * (2.f * cs - 1.f); // hard-light
    case 20: {                                                                               // soft-light
        if (cs <= 0.5f) return cb - (1.f - 2.f * cs) * cb * (1.f - cb);
        const float d = cb <= 0.25f ? ((16.f * cb - 12.f) * cb + 4.f) * cb : std::sqrt(cb);
        return cb + (2.f * cs - 1.f) * (d - cb);
    }
    case 21: return std::abs(cb - cs);         // difference
    case 22: return cb + cs - 2.f * cb * cs;   // exclusion
    default: return cb * cs;                   // 23 multiply
    }
}

// Composites premultiplied source s over backdrop b with CompositeMode `mode`.
Rgba composite(int mode, const Rgba& s, const Rgba& b) {
    const float as = s.a, ab = b.a;
    switch (mode) {
    case 1: return s;
    case 2: return b;
    case 3: return {s.r + b.r * (1 - as), s.g + b.g * (1 - as), s.b + b.b * (1 - as), as + ab * (1 - as)};
    case 4: return {b.r + s.r * (1 - ab), b.g + s.g * (1 - ab), b.b + s.b * (1 - ab), ab + as * (1 - ab)};
    case 5: return s * ab;
    case 6: return b * as;
    case 7: return s * (1 - ab);
    case 8: return b * (1 - as);
    case 9: return {s.r * ab + b.r * (1 - as), s.g * ab + b.g * (1 - as), s.b * ab + b.b * (1 - as), ab};
    case 10: return {b.r * as + s.r * (1 - ab), b.g * as + s.g * (1 - ab), b.b * as + s.b * (1 - ab), as};
    case 11:
        return {s.r * (1 - ab) + b.r * (1 - as), s.g * (1 - ab) + b.g * (1 - as), s.b * (1 - ab) + b.b * (1 - as),
                as * (1 - ab) + ab * (1 - as)};
    case 12:
        return {std::min(1.f, s.r + b.r), std::min(1.f, s.g + b.g), std::min(1.f, s.b + b.b), std::min(1.f, as + ab)};
    default: break;
    }
    if (mode < 13 || mode > 27) return {}; // 0 clear, unknown modes -> clear
    if (as <= 0.f) return b;
    if (ab <= 0.f) return s;
    const Rgb cs{s.r / as, s.g / as, s.b / as}, cb{b.r / ab, b.g / ab, b.b / ab};
    Rgb m;
    if (mode <= 23) {
        m = {blendChannel(mode, cb.r, cs.r), blendChannel(mode, cb.g, cs.g), blendChannel(mode, cb.b, cs.b)};
    } else if (mode == 24) {
        m = setLum(setSat(cs, sat(cb)), lum(cb)); // hue
    } else if (mode == 25) {
        m = setLum(setSat(cb, sat(cs)), lum(cb)); // saturation
    } else if (mode == 26) {
        m = setLum(cs, lum(cb)); // color
    } else {
        m = setLum(cb, lum(cs)); // luminosity
    }
    const float both = as * ab;
    return {s.r * (1 - ab) + b.r * (1 - as) + both * m.r, s.g * (1 - ab) + b.g * (1 - as) + both * m.g,
            s.b * (1 - ab) + b.b * (1 - as) + both * m.b, as + ab * (1 - as)};
}

class Renderer {
public:
    Renderer(const ColrSource& src, std::uint32_t foreground) : m_src(src), m_d(src.data.data()), m_n(src.data.size()) {
        m_fg = {float(foreground & 0xFFu) / 255.f, float((foreground >> 8) & 0xFFu) / 255.f,
                float((foreground >> 16) & 0xFFu) / 255.f, float((foreground >> 24) & 0xFFu) / 255.f};
    }

    // Parses the COLR v1 header and CPAL palette 0 (false: no usable v1 data).
    bool init() {
        const std::uint32_t colr = m_src.colr;
        if (!colr || !ok(colr, 34) || u16(colr) < 1) return false;
        if (const std::uint32_t bl = u32(colr + 14); bl && ok(std::uint64_t(colr) + bl, 4)) {
            m_baseList = colr + bl;
            m_numBase = u32(m_baseList);
            if (!ok(std::uint64_t(m_baseList) + 4, std::uint64_t(m_numBase) * 6)) return false;
        }
        if (const std::uint32_t ll = u32(colr + 18); ll && ok(std::uint64_t(colr) + ll, 4)) {
            m_layerList = colr + ll;
            m_numLayers = u32(m_layerList);
            if (!ok(std::uint64_t(m_layerList) + 4, std::uint64_t(m_numLayers) * 4)) m_layerList = m_numLayers = 0;
        }
        if (const std::uint32_t cl = u32(colr + 22); cl && ok(std::uint64_t(colr) + cl, 5) && m_d[colr + cl] == 1) {
            m_clipList = colr + cl;
            m_numClips = u32(m_clipList + 1);
            if (!ok(std::uint64_t(m_clipList) + 5, std::uint64_t(m_numClips) * 7)) m_clipList = m_numClips = 0;
        }
        const std::uint32_t cpal = m_src.cpal;
        if (cpal && ok(cpal, 14)) {
            const std::uint16_t numPalettes = u16(cpal + 4);
            m_numEntries = u16(cpal + 2);
            m_numRecords = u16(cpal + 6);
            m_recOff = cpal + u32(cpal + 8);
            m_first = u16(cpal + 12);
            if (numPalettes == 0 || !ok(m_recOff, std::uint64_t(m_numRecords) * 4)) m_numEntries = 0;
        }
        return m_baseList != 0;
    }

    void glyphs(std::vector<std::uint32_t>& out) const {
        for (std::uint32_t i = 0; i < m_numBase; ++i) out.push_back(u16(m_baseList + 4 + i * 6));
        std::sort(out.begin(), out.end());
    }

    bool render(std::uint32_t glyph, ColorBitmap& out) {
        const std::uint32_t root = basePaint(glyph);
        if (!root) return false;
        const double s = m_src.scale;
        if (!(s > 0.0) || !std::isfinite(s)) return false;
        const Affine dev = scaling(s, -s); // font units -> device pixels (y down)
        Box box;
        double clip[4];
        const bool hasClip = clipBox(glyph, clip);
        if (hasClip) {
            addRect(box, dev, clip);
        } else {
            m_budget = kMaxNodes;
            bounds(root, dev, 0, box);
        }
        if (box.empty() || !std::isfinite(box.x0) || !std::isfinite(box.y0) || !std::isfinite(box.x1) ||
            !std::isfinite(box.y1))
            return false;
        const double fx0 = std::floor(box.x0), fy0 = std::floor(box.y0);
        const double fw = std::ceil(box.x1) - fx0, fh = std::ceil(box.y1) - fy0;
        if (fw < 1.0 || fh < 1.0 || fw > kMaxDim || fh > kMaxDim || fw * fh > double(kMaxPixels)) return false;
        const int x0 = int(fx0), y0 = int(fy0), w = int(fw), h = int(fh);
        const Affine toBitmap = translate(-fx0, -fy0) * dev;

        Layer layer(Rect{0, 0, w, h});
        m_budget = kMaxNodes;
        m_work = std::int64_t(w) * h * kWorkPerPixel;
        m_liveMax = std::int64_t(w) * h * kLivePerPixel;
        m_live = 0;
        m_abort = false;
        paint(root, toBitmap, layer, 0);
        if (hasClip) {
            Mask m;
            if (!rectMask(clip, toBitmap, layer.r, m)) return false;
            applyMask(layer, m);
        }
        if (m_abort) return false; // pathological graph: too much work or memory

        out.w = w;
        out.h = h;
        out.x0 = x0;
        out.y0 = y0;
        out.rgba.assign(std::size_t(w) * std::size_t(h) * 4, 0);
        for (std::size_t i = 0; i < layer.px.size(); ++i) {
            const Rgba& c = layer.px[i];
            const auto q = [](float v) { return v > 0.f ? int(std::min(v, 1.f) * 255.f + 0.5f) : 0; }; // NaN -> 0
            const int a = q(c.a);
            out.rgba[i * 4 + 0] = std::uint8_t(std::min(q(c.r), a));
            out.rgba[i * 4 + 1] = std::uint8_t(std::min(q(c.g), a));
            out.rgba[i * 4 + 2] = std::uint8_t(std::min(q(c.b), a));
            out.rgba[i * 4 + 3] = std::uint8_t(a);
        }
        return true;
    }

private:
    // ---- bounds-checked big-endian reads ----
    bool ok(std::uint64_t off, std::uint64_t len) const { return off <= m_n && len <= m_n - off; }
    std::uint16_t u16(std::uint64_t p) const { return std::uint16_t((m_d[p] << 8) | m_d[p + 1]); }
    std::int16_t i16(std::uint64_t p) const { return std::int16_t(u16(p)); }
    std::uint32_t u24(std::uint64_t p) const {
        return (std::uint32_t(m_d[p]) << 16) | (std::uint32_t(m_d[p + 1]) << 8) | m_d[p + 2];
    }
    std::uint32_t u32(std::uint64_t p) const {
        return (std::uint32_t(m_d[p]) << 24) | (std::uint32_t(m_d[p + 1]) << 16) | (std::uint32_t(m_d[p + 2]) << 8) |
               m_d[p + 3];
    }
    double f2dot14(std::uint64_t p) const { return double(i16(p)) / 16384.0; }
    double fixed(std::uint64_t p) const { return double(std::int32_t(u32(p))) / 65536.0; }
    // Offset24 child at byte `at` of paint p (0 when null or out of range).
    std::uint32_t child(std::uint32_t p, std::uint32_t at = 1) const {
        const std::uint32_t off = u24(std::uint64_t(p) + at);
        const std::uint64_t q = std::uint64_t(p) + off;
        return off && q < m_n && q <= 0xFFFFFFFFu ? std::uint32_t(q) : 0;
    }

    std::uint32_t basePaint(std::uint32_t glyph) const {
        if (!m_baseList) return 0;
        std::uint32_t lo = 0, hi = m_numBase;
        while (lo < hi) { // records are sorted by glyph id
            const std::uint32_t m = lo + (hi - lo) / 2;
            const std::uint32_t r = m_baseList + 4 + m * 6;
            const std::uint16_t g = u16(r);
            if (g == glyph) {
                const std::uint64_t q = std::uint64_t(m_baseList) + u32(r + 2);
                return q < m_n ? std::uint32_t(q) : 0;
            }
            if (g < glyph) lo = m + 1;
            else hi = m;
        }
        return 0;
    }

    // Clip box of `glyph` in font units (xMin, yMin, xMax, yMax).
    bool clipBox(std::uint32_t glyph, double out[4]) const {
        if (!m_clipList) return false;
        std::uint32_t lo = 0, hi = m_numClips;
        while (lo < hi) { // sorted by startGlyphID, ranges disjoint
            const std::uint32_t m = lo + (hi - lo) / 2;
            const std::uint32_t r = m_clipList + 5 + m * 7;
            if (glyph < u16(r)) hi = m;
            else if (glyph > u16(r + 2)) lo = m + 1;
            else {
                const std::uint64_t b = std::uint64_t(m_clipList) + u24(r + 4);
                if (!ok(b, 9) || (m_d[b] != 1 && m_d[b] != 2)) return false;
                out[0] = i16(b + 1);
                out[1] = i16(b + 3);
                out[2] = i16(b + 5);
                out[3] = i16(b + 7);
                return out[2] > out[0] && out[3] > out[1];
            }
        }
        return false;
    }

    Rgba colour(std::uint16_t index, double alpha) const {
        alpha = std::clamp(alpha, 0.0, 1.0);
        Rgba c;
        if (index == 0xFFFFu || index >= m_numEntries || std::uint32_t(m_first) + index >= m_numRecords) {
            c = m_fg;
        } else {
            const std::uint32_t r = m_recOff + (std::uint32_t(m_first) + index) * 4; // B G R A
            c = {float(m_d[r + 2]) / 255.f, float(m_d[r + 1]) / 255.f, float(m_d[r]) / 255.f, float(m_d[r + 3]) / 255.f};
        }
        c.a *= float(alpha);
        return {c.r * c.a, c.g * c.a, c.b * c.a, c.a}; // premultiplied
    }

    bool readColorLine(std::uint32_t at, bool var, ColorLine& cl) const {
        if (!ok(at, 3)) return false;
        const std::uint8_t ext = m_d[at];
        cl.extend = ext == 1 ? Extend::Repeat : ext == 2 ? Extend::Reflect : Extend::Pad;
        const std::uint16_t count = u16(at + 1);
        const std::uint32_t size = var ? 10 : 6;
        if (count == 0 || !ok(std::uint64_t(at) + 3, std::uint64_t(count) * size)) return false;
        cl.stops.clear();
        cl.stops.reserve(count);
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t s = at + 3 + k * size;
            cl.stops.push_back({float(f2dot14(s)), colour(u16(s + 2), f2dot14(s + 4))});
        }
        std::stable_sort(cl.stops.begin(), cl.stops.end(), [](const Stop& a, const Stop& b) { return a.off < b.off; });
        return true;
    }

    // Shader of a solid / gradient paint (formats 2..9) under transform T; false: paints nothing.
    bool shader(std::uint32_t p, const Affine& T, Shader& sh) const {
        const std::uint8_t f = m_d[p];
        if (f == 2 || f == 3) {
            if (!ok(p, 5)) return false;
            sh.kind = Shader::Solid;
            sh.solid = colour(u16(p + 1), f2dot14(p + 3));
            return sh.solid.a > 0.f;
        }
        if (!invert(T, sh.inv)) return false;
        const bool var = (f & 1) != 0;
        if (f == 4 || f == 5) {
            if (!ok(p, 16) || !readColorLine(child(p), var, sh.line)) return false;
            const double x0 = i16(p + 4), y0 = i16(p + 6), x1 = i16(p + 8), y1 = i16(p + 10), x2 = i16(p + 12),
                         y2 = i16(p + 14);
            // p3: p1 projected onto the line through p0 perpendicular to p0p2 (colour lines are parallel to p0p2)
            const double v02x = x2 - x0, v02y = y2 - y0, v01x = x1 - x0, v01y = y1 - y0;
            const double n02 = v02x * v02x + v02y * v02y, n01 = v01x * v01x + v01y * v01y;
            if (n02 == 0.0 || n01 == 0.0) return false;
            const double cross = v01x * v02y - v01y * v02x;
            if (std::abs(cross) <= 1e-6 * std::sqrt(n01 * n02)) return false; // parallel: ill-formed
            const double k = (v01x * v02x + v01y * v02y) / n02;
            const double v03x = v01x - k * v02x, v03y = v01y - k * v02y;
            const double n03 = v03x * v03x + v03y * v03y;
            sh.kind = Shader::Linear;
            sh.x0 = x0;
            sh.y0 = y0;
            sh.dx = v03x / n03;
            sh.dy = v03y / n03;
            return true;
        }
        if (f == 6 || f == 7) {
            if (!ok(p, 16) || !readColorLine(child(p), var, sh.line)) return false;
            sh.kind = Shader::Radial;
            sh.x0 = i16(p + 4);
            sh.y0 = i16(p + 6);
            sh.r0 = u16(p + 8);
            const double x1 = i16(p + 10), y1 = i16(p + 12), r1 = u16(p + 14);
            if (sh.r0 == 0.0 && r1 == 0.0) return false;
            sh.cdx = x1 - sh.x0;
            sh.cdy = y1 - sh.y0;
            sh.dr = r1 - sh.r0;
            sh.a = sh.cdx * sh.cdx + sh.cdy * sh.cdy - sh.dr * sh.dr;
            return true;
        }
        if (f == 8 || f == 9) {
            if (!ok(p, 12) || !readColorLine(child(p), var, sh.line)) return false;
            sh.kind = Shader::Sweep;
            sh.x0 = i16(p + 4);
            sh.y0 = i16(p + 6);
            sh.start = (f2dot14(p + 8) + 1.0) * 180.0; // biased angles
            sh.end = (f2dot14(p + 10) + 1.0) * 180.0;
            return true;
        }
        return false;
    }

    // Child paint and transform of formats 12..31.
    bool transformOf(std::uint32_t p, Affine& m) const {
        const std::uint8_t f = m_d[p];
        const auto deg = [](double v) { return v * std::numbers::pi; }; // 1.0 = 180 degrees
        switch (f) {
        case 12:
        case 13: {
            if (!ok(p, 7)) return false;
            const std::uint64_t t = std::uint64_t(p) + u24(std::uint64_t(p) + 4);
            if (!ok(t, 24)) return false;
            m = {fixed(t), fixed(t + 4), fixed(t + 8), fixed(t + 12), fixed(t + 16), fixed(t + 20)};
            return true;
        }
        case 14:
        case 15:
            if (!ok(p, 8)) return false;
            m = translate(i16(p + 4), i16(p + 6));
            return true;
        case 16:
        case 17:
            if (!ok(p, 8)) return false;
            m = scaling(f2dot14(p + 4), f2dot14(p + 6));
            return true;
        case 18:
        case 19:
            if (!ok(p, 12)) return false;
            m = around(scaling(f2dot14(p + 4), f2dot14(p + 6)), i16(p + 8), i16(p + 10));
            return true;
        case 20:
        case 21:
            if (!ok(p, 6)) return false;
            m = scaling(f2dot14(p + 4), f2dot14(p + 4));
            return true;
        case 22:
        case 23:
            if (!ok(p, 10)) return false;
            m = around(scaling(f2dot14(p + 4), f2dot14(p + 4)), i16(p + 6), i16(p + 8));
            return true;
        case 24:
        case 25:
        case 26:
        case 27: {
            if (!ok(p, f >= 26 ? 10 : 6)) return false;
            const double a = deg(f2dot14(p + 4)), c = std::cos(a), s = std::sin(a);
            m = {c, s, -s, c, 0, 0};
            if (f >= 26) m = around(m, i16(p + 6), i16(p + 8));
            return true;
        }
        case 28:
        case 29:
        case 30:
        case 31: {
            if (!ok(p, f >= 30 ? 12 : 8)) return false;
            m = {1, std::tan(deg(f2dot14(p + 6))), -std::tan(deg(f2dot14(p + 4))), 1, 0, 0};
            if (f >= 30) m = around(m, i16(p + 8), i16(p + 10));
            return true;
        }
        default: return false;
        }
    }

    const std::vector<OutlineCmd>& outline(std::uint32_t glyph) {
        auto it = m_outlines.find(glyph);
        if (it == m_outlines.end()) {
            std::vector<OutlineCmd> path;
            if (m_src.font) glyphOutline(*m_src.font, glyph, path);
            it = m_outlines.emplace(glyph, std::move(path)).first;
        }
        return it->second;
    }

    static void addPoint(Box& b, const Affine& T, double x, double y) {
        b.add(T.xx * x + T.xy * y + T.dx, T.yx * x + T.yy * y + T.dy);
    }
    static void addRect(Box& b, const Affine& T, const double r[4]) {
        addPoint(b, T, r[0], r[1]);
        addPoint(b, T, r[2], r[1]);
        addPoint(b, T, r[0], r[3]);
        addPoint(b, T, r[2], r[3]);
    }
    static void addPath(Box& b, const Affine& T, const std::vector<OutlineCmd>& path) {
        for (const OutlineCmd& c : path) {
            addPoint(b, T, c.x, c.y);
            if (c.kind == OutlineCmd::Quad || c.kind == OutlineCmd::Cubic) addPoint(b, T, c.cx, c.cy);
            if (c.kind == OutlineCmd::Cubic) addPoint(b, T, c.cx1, c.cy1);
        }
    }

    // Device bounds of everything paint p can draw: the union of its glyph outlines (and clip boxes of
    // referenced colour glyphs).
    void bounds(std::uint32_t p, const Affine& T, int depth, Box& box) {
        if (depth > kMaxDepth || --m_budget < 0 || !ok(p, 1)) return;
        const std::uint8_t f = m_d[p];
        if (f == 1) {
            forLayers(p, [&](std::uint32_t q) { bounds(q, T, depth + 1, box); });
        } else if (f == 10) {
            if (ok(p, 6)) addPath(box, T, outline(u16(p + 4)));
        } else if (f == 11) {
            if (!ok(p, 3)) return;
            double clip[4];
            if (clipBox(u16(p + 1), clip)) addRect(box, T, clip);
            else if (const std::uint32_t q = basePaint(u16(p + 1))) bounds(q, T, depth + 1, box);
        } else if (f >= 12 && f <= 31) {
            Affine m;
            if (ok(p, 4) && transformOf(p, m))
                if (const std::uint32_t q = child(p)) bounds(q, T * m, depth + 1, box);
        } else if (f == 32) {
            if (!ok(p, 8)) return;
            if (const std::uint32_t q = child(p)) bounds(q, T, depth + 1, box);
            if (const std::uint32_t q = child(p, 5)) bounds(q, T, depth + 1, box);
        }
    }

    template <class Fn> void forLayers(std::uint32_t p, Fn&& fn) {
        if (!ok(p, 6) || !m_layerList) return;
        const std::uint32_t count = m_d[p + 1];
        const std::uint64_t first = u32(p + 2);
        for (std::uint32_t k = 0; k < count; ++k) {
            if (first + k >= m_numLayers) break;
            const std::uint64_t q = std::uint64_t(m_layerList) + u32(m_layerList + 4 + (first + k) * 4);
            if (q < m_n) fn(std::uint32_t(q));
        }
    }

    // Charges n pixel operations (false once the work budget is spent).
    bool spend(std::int64_t n) {
        m_work -= n;
        if (m_work < 0) m_abort = true;
        return !m_abort;
    }
    // Reserves `count` temporary layers of rect r (false: over the memory or work budget).
    bool acquire(const Rect& r, int count) {
        const std::int64_t n = std::int64_t(r.w()) * r.h() * count;
        if (m_live + n > m_liveMax) m_abort = true;
        if (!spend(n)) return false;
        m_live += n;
        return true;
    }
    void release(const Rect& r, int count) { m_live -= std::int64_t(r.w()) * r.h() * count; }

    // Coverage of `path` under T, limited to `limit` (false: empty).
    bool pathMask(const std::vector<OutlineCmd>& path, const Affine& T, const Rect& limit, Mask& m) {
        if (path.empty() || limit.empty()) return false;
        Box b;
        addPath(b, T, path);
        if (b.empty() || !std::isfinite(b.x0) || !std::isfinite(b.y0) || !std::isfinite(b.x1) || !std::isfinite(b.y1))
            return false;
        const auto cl = [](double v, int lo, int hi) { return int(std::clamp(v, double(lo), double(hi))); };
        const Rect r{cl(std::floor(b.x0), limit.x0, limit.x1), cl(std::floor(b.y0), limit.y0, limit.y1),
                     cl(std::ceil(b.x1), limit.x0, limit.x1), cl(std::ceil(b.y1), limit.y0, limit.y1)};
        if (r.empty()) return false;
        if (!spend(std::int64_t(r.w()) * r.h() + std::int64_t(path.size()) * 8)) return false;
        m.r = r;
        m.cov.assign(std::size_t(r.w()) * std::size_t(r.h()), 0.f);
        rasterisePath(path, translate(-r.x0, -r.y0) * T, m.cov.data(), r.w(), r.h());
        return true;
    }

    bool rectMask(const double c[4], const Affine& T, const Rect& limit, Mask& m) {
        std::vector<OutlineCmd> path(4);
        path[0] = {OutlineCmd::Move, float(c[0]), float(c[1])};
        path[1] = {OutlineCmd::Line, float(c[2]), float(c[1])};
        path[2] = {OutlineCmd::Line, float(c[2]), float(c[3])};
        path[3] = {OutlineCmd::Line, float(c[0]), float(c[3])};
        return pathMask(path, T, limit, m);
    }

    // dst = src * mask over dst (mask optional; only src's rect).
    static void srcOver(Layer& dst, const Layer& src, const Mask* m) {
        const Rect r = intersect(dst.r, m ? intersect(src.r, m->r) : src.r);
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                Rgba s = src.px[std::size_t(y - src.r.y0) * std::size_t(src.r.w()) + std::size_t(x - src.r.x0)];
                if (m) s = s * m->at(x, y);
                if (s.a <= 0.f) continue;
                Rgba& d = dst.at(x, y);
                const float k = 1.f - s.a;
                d = {s.r + d.r * k, s.g + d.g * k, s.b + d.b * k, s.a + d.a * k};
            }
    }

    // Multiplies the layer by the mask (zero outside it).
    static void applyMask(Layer& l, const Mask& m) {
        for (int y = l.r.y0; y < l.r.y1; ++y)
            for (int x = l.r.x0; x < l.r.x1; ++x) {
                const bool in = x >= m.r.x0 && x < m.r.x1 && y >= m.r.y0 && y < m.r.y1;
                Rgba& c = l.at(x, y);
                c = c * (in ? m.at(x, y) : 0.f);
            }
    }

    // Fills dst (or the mask's rect, weighted by coverage) with a solid / gradient.
    void fill(std::uint32_t p, const Affine& T, Layer& dst, const Mask* m) {
        Shader sh;
        if (!shader(p, T, sh)) return;
        const Rect r = m ? intersect(dst.r, m->r) : dst.r;
        if (r.empty() || !spend(std::int64_t(r.w()) * r.h())) return;
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const float cov = m ? m->at(x, y) : 1.f;
                if (cov <= 0.f) continue;
                Rgba s;
                if (!sh.eval(x + 0.5, y + 0.5, s)) continue;
                s = s * cov;
                if (s.a <= 0.f) continue;
                Rgba& d = dst.at(x, y);
                const float k = 1.f - s.a;
                d = {s.r + d.r * k, s.g + d.g * k, s.b + d.b * k, s.a + d.a * k};
            }
    }

    // Draws paint p (under T) src-over into dst.
    void paint(std::uint32_t p, const Affine& T, Layer& dst, int depth) {
        if (m_abort || depth > kMaxDepth || --m_budget < 0 || !ok(p, 1) || dst.r.empty()) return;
        const std::uint8_t f = m_d[p];
        if (f == 1) { // PaintColrLayers: bottom-up, src-over (associative, so drawn straight into dst)
            forLayers(p, [&](std::uint32_t q) { paint(q, T, dst, depth + 1); });
        } else if (f >= 2 && f <= 9) {
            fill(p, T, dst, nullptr);
        } else if (f == 10) { // PaintGlyph: child clipped by the outline
            if (!ok(p, 6)) return;
            const std::uint32_t q = child(p);
            Mask m;
            if (!q || !pathMask(outline(u16(p + 4)), T, dst.r, m)) return;
            // fast path: transforms down to a solid / gradient fill straight through the mask
            Affine tc = T;
            std::uint32_t leaf = q;
            for (int k = depth; leaf && k <= kMaxDepth && m_d[leaf] >= 12 && m_d[leaf] <= 31; ++k) {
                Affine mt;
                if (!ok(leaf, 4) || !transformOf(leaf, mt)) return;
                tc = tc * mt;
                leaf = child(leaf);
            }
            if (leaf && m_d[leaf] >= 2 && m_d[leaf] <= 9) {
                if (--m_budget >= 0) fill(leaf, tc, dst, &m);
                return;
            }
            if (!acquire(m.r, 1)) return;
            Layer tmp(m.r);
            paint(q, T, tmp, depth + 1);
            srcOver(dst, tmp, &m);
            release(m.r, 1);
        } else if (f == 11) { // PaintColrGlyph: another base glyph, clipped by its clip box
            if (!ok(p, 3)) return;
            const std::uint32_t q = basePaint(u16(p + 1));
            if (!q) return;
            double clip[4];
            if (!clipBox(u16(p + 1), clip)) {
                paint(q, T, dst, depth + 1);
                return;
            }
            Mask m;
            if (!rectMask(clip, T, dst.r, m)) return;
            if (!acquire(m.r, 1)) return;
            Layer tmp(m.r);
            paint(q, T, tmp, depth + 1);
            srcOver(dst, tmp, &m);
            release(m.r, 1);
        } else if (f >= 12 && f <= 31) {
            Affine m;
            if (!ok(p, 4) || !transformOf(p, m)) return;
            if (const std::uint32_t q = child(p)) paint(q, T * m, dst, depth + 1);
        } else if (f == 32) { // PaintComposite: source composited onto backdrop, the result src-over dst
            if (!ok(p, 8)) return;
            const int mode = m_d[p + 4];
            if (!acquire(dst.r, 2)) return;
            Layer backdrop(dst.r), source(dst.r);
            if (const std::uint32_t q = child(p, 5)) paint(q, T, backdrop, depth + 1);
            if (const std::uint32_t q = child(p)) paint(q, T, source, depth + 1);
            for (std::size_t i = 0; i < backdrop.px.size(); ++i)
                backdrop.px[i] = composite(mode, source.px[i], backdrop.px[i]);
            srcOver(dst, backdrop, nullptr);
            release(dst.r, 2);
        }
    }

    const ColrSource& m_src;
    const std::uint8_t* m_d;
    std::size_t m_n;
    Rgba m_fg;
    std::uint32_t m_baseList = 0, m_numBase = 0, m_layerList = 0, m_numLayers = 0, m_clipList = 0, m_numClips = 0;
    std::uint32_t m_recOff = 0;
    std::uint16_t m_first = 0, m_numEntries = 0, m_numRecords = 0;
    int m_budget = kMaxNodes;
    std::int64_t m_work = 0, m_live = 0, m_liveMax = 0; // pixel-operation and live temporary-pixel budgets
    bool m_abort = false;
    std::unordered_map<std::uint32_t, std::vector<OutlineCmd>> m_outlines;
};

} // namespace

bool colrV1Render(const ColrSource& src, std::uint32_t glyph, std::uint32_t foreground, ColorBitmap& out) {
    out = {};
    Renderer r(src, foreground);
    if (!r.init()) return false;
    if (!r.render(glyph, out)) {
        out = {};
        return false;
    }
    return true;
}

void colrV1Glyphs(const ColrSource& src, std::vector<std::uint32_t>& out) {
    out.clear();
    Renderer r(src, 0);
    if (r.init()) r.glyphs(out);
}

} // namespace fuse::text::detail
