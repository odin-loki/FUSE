// Bitmap colour glyphs: CBDT / CBLC (Google) and sbix (Apple) PNG strikes, decoded and resampled to the requested
// size. Fonts are untrusted: every read is bounds-checked and anything malformed makes the glyph fail (return false).
#include "glyph_paint.hpp"

// The PNG decoder lives only in this translation unit (static functions).
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4245 4267 4456 4457 4505 4701 4702 4996 6001 6011 6262 6385 6386)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fuse::text::detail {
namespace {

constexpr int kMaxImage = 1024;  // decoded PNG side cap
constexpr int kMaxOutput = 4096; // output bitmap side cap
constexpr std::uint32_t kMaxStrikes = 256;
constexpr std::uint32_t kMaxSubtables = 65536;

constexpr std::uint32_t tag(char a, char b, char c, char d) {
    return (std::uint32_t(std::uint8_t(a)) << 24) | (std::uint32_t(std::uint8_t(b)) << 16) |
           (std::uint32_t(std::uint8_t(c)) << 8) | std::uint32_t(std::uint8_t(d));
}
constexpr std::uint32_t kPng = tag('p', 'n', 'g', ' ');
constexpr std::uint32_t kDupe = tag('d', 'u', 'p', 'e');

/// Big-endian reads over the font file. Callers check in() before reading.
struct Reader {
    std::span<const std::uint8_t> d;
    bool in(std::uint64_t off, std::uint64_t len) const { return off <= d.size() && len <= d.size() - off; }
    std::uint8_t u8(std::uint64_t o) const { return d[static_cast<std::size_t>(o)]; }
    std::int8_t i8(std::uint64_t o) const { return static_cast<std::int8_t>(u8(o)); }
    std::uint16_t u16(std::uint64_t o) const { return static_cast<std::uint16_t>((u8(o) << 8) | u8(o + 1)); }
    std::int16_t i16(std::uint64_t o) const { return static_cast<std::int16_t>(u16(o)); }
    std::uint32_t u32(std::uint64_t o) const {
        return (std::uint32_t(u8(o)) << 24) | (std::uint32_t(u8(o + 1)) << 16) | (std::uint32_t(u8(o + 2)) << 8) |
               std::uint32_t(u8(o + 3));
    }
};

/// A located PNG and where it sits, in strike pixels relative to the glyph origin.
struct Located {
    std::uint64_t png = 0;
    std::uint32_t pngLen = 0;
    int left = 0;          ///< x of the image's left edge
    int vert = 0;          ///< CBDT: top edge above the baseline; sbix: bottom edge above the baseline
    bool fromBottom = false;
};

/// Strike handle: ppem plus the absolute offset of its BitmapSize record (CBLC) or strike (sbix).
struct Strike {
    float ppem = 0.f;
    std::uint64_t at = 0;
};

// ---- CBLC / CBDT ----

/// Reads the CBDT glyph record at `off` (`len` bytes) in `imageFormat`. `big` holds index-subtable big metrics
/// (formats 2 / 5) for image format 19.
bool cbdtImage(const Reader& r, std::uint64_t off, std::uint64_t len, std::uint16_t imageFormat, const std::uint8_t* big,
               Located& out) {
    if (!r.in(off, len)) return false;
    std::uint64_t head = 0;
    int bx = 0, by = 0;
    switch (imageFormat) {
    case 17: // smallGlyphMetrics (height, width, bearingX, bearingY, advance)
        head = 5;
        if (len < head + 4) return false;
        bx = r.i8(off + 2);
        by = r.i8(off + 3);
        break;
    case 18: // bigGlyphMetrics (height, width, horiBearingX, horiBearingY, ...)
        head = 8;
        if (len < head + 4) return false;
        bx = r.i8(off + 2);
        by = r.i8(off + 3);
        break;
    case 19:
        if (!big) return false;
        bx = static_cast<std::int8_t>(big[2]);
        by = static_cast<std::int8_t>(big[3]);
        if (len < 4) return false;
        break;
    default: return false;
    }
    const std::uint32_t dataLen = r.u32(off + head);
    if (dataLen == 0 || dataLen > len - head - 4) return false;
    out.png = off + head + 4;
    out.pngLen = dataLen;
    out.left = bx;
    out.vert = by;
    out.fromBottom = false;
    return true;
}

/// Finds `glyph` in the strike whose BitmapSize record is at `rec`.
bool cblcFind(const Reader& r, std::uint64_t cblc, std::uint64_t cbdt, std::uint64_t rec, std::uint32_t glyph,
              Located& out) {
    if (!r.in(rec, 48)) return false;
    const std::uint64_t arr = cblc + r.u32(rec);
    const std::uint32_t n = r.u32(rec + 8);
    if (n > kMaxSubtables || !r.in(arr, std::uint64_t(n) * 8)) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint64_t e = arr + std::uint64_t(i) * 8;
        const std::uint16_t first = r.u16(e), last = r.u16(e + 2);
        if (glyph < first || glyph > last) continue;
        const std::uint64_t sub = arr + r.u32(e + 4);
        if (!r.in(sub, 8)) return false;
        const std::uint16_t indexFormat = r.u16(sub), imageFormat = r.u16(sub + 2);
        const std::uint64_t data = cbdt + r.u32(sub + 4);
        const std::uint32_t idx = glyph - first;
        std::uint64_t off = 0, len = 0;
        std::uint8_t big[8] = {};
        bool hasBig = false;
        switch (indexFormat) {
        case 1: {
            const std::uint64_t p = sub + 8 + std::uint64_t(idx) * 4;
            if (!r.in(p, 8)) return false;
            const std::uint32_t o0 = r.u32(p), o1 = r.u32(p + 4);
            if (o1 <= o0) return false; // no bitmap for this glyph
            off = data + o0;
            len = o1 - o0;
            break;
        }
        case 3: {
            const std::uint64_t p = sub + 8 + std::uint64_t(idx) * 2;
            if (!r.in(p, 4)) return false;
            const std::uint16_t o0 = r.u16(p), o1 = r.u16(p + 2);
            if (o1 <= o0) return false;
            off = data + o0;
            len = std::uint64_t(o1 - o0);
            break;
        }
        case 2: {
            if (!r.in(sub + 8, 12)) return false;
            const std::uint32_t size = r.u32(sub + 8);
            for (int k = 0; k < 8; ++k) big[k] = r.u8(sub + 12 + std::uint64_t(k));
            hasBig = true;
            off = data + std::uint64_t(idx) * size;
            len = size;
            break;
        }
        case 4: {
            if (!r.in(sub + 8, 4)) return false;
            const std::uint32_t num = r.u32(sub + 8);
            const std::uint64_t pairs = sub + 12;
            if (num == 0 || num > 65536 || !r.in(pairs, (std::uint64_t(num) + 1) * 4)) return false;
            std::uint32_t lo = 0, hi = num; // binary search over glyph ids (sorted)
            while (lo < hi) {
                const std::uint32_t mid = (lo + hi) / 2;
                if (r.u16(pairs + std::uint64_t(mid) * 4) < glyph) lo = mid + 1;
                else hi = mid;
            }
            if (lo >= num || r.u16(pairs + std::uint64_t(lo) * 4) != glyph) return false;
            const std::uint16_t o0 = r.u16(pairs + std::uint64_t(lo) * 4 + 2);
            const std::uint16_t o1 = r.u16(pairs + std::uint64_t(lo + 1) * 4 + 2);
            if (o1 <= o0) return false;
            off = data + o0;
            len = std::uint64_t(o1 - o0);
            break;
        }
        case 5: {
            if (!r.in(sub + 8, 16)) return false;
            const std::uint32_t size = r.u32(sub + 8);
            for (int k = 0; k < 8; ++k) big[k] = r.u8(sub + 12 + std::uint64_t(k));
            hasBig = true;
            const std::uint32_t num = r.u32(sub + 20);
            const std::uint64_t ids = sub + 24;
            if (num > 65536 || !r.in(ids, std::uint64_t(num) * 2)) return false;
            std::uint32_t lo = 0, hi = num;
            while (lo < hi) {
                const std::uint32_t mid = (lo + hi) / 2;
                if (r.u16(ids + std::uint64_t(mid) * 2) < glyph) lo = mid + 1;
                else hi = mid;
            }
            if (lo >= num || r.u16(ids + std::uint64_t(lo) * 2) != glyph) return false;
            off = data + std::uint64_t(lo) * size;
            len = size;
            break;
        }
        default: return false;
        }
        return cbdtImage(r, off, len, imageFormat, hasBig ? big : nullptr, out);
    }
    return false;
}

void cblcStrikes(const Reader& r, std::uint64_t cblc, std::vector<Strike>& out) {
    if (!r.in(cblc, 8)) return;
    const std::uint32_t num = r.u32(cblc + 4);
    if (num > kMaxStrikes || !r.in(cblc + 8, std::uint64_t(num) * 48)) return;
    for (std::uint32_t i = 0; i < num; ++i) {
        const std::uint64_t rec = cblc + 8 + std::uint64_t(i) * 48;
        const std::uint8_t ppem = r.u8(rec + 45); // ppemY
        if (ppem) out.push_back({float(ppem), rec});
    }
}

// ---- sbix ----

void sbixStrikes(const Reader& r, std::uint64_t sbix, std::uint16_t numGlyphs, std::vector<Strike>& out) {
    if (numGlyphs == 0 || !r.in(sbix, 8)) return;
    const std::uint32_t num = r.u32(sbix + 4);
    if (num > kMaxStrikes || !r.in(sbix + 8, std::uint64_t(num) * 4)) return;
    for (std::uint32_t i = 0; i < num; ++i) {
        const std::uint64_t s = sbix + r.u32(sbix + 8 + std::uint64_t(i) * 4);
        if (!r.in(s, 4 + (std::uint64_t(numGlyphs) + 1) * 4)) continue;
        const std::uint16_t ppem = r.u16(s);
        if (ppem) out.push_back({float(ppem), s});
    }
}

/// Glyph record of `glyph` in the strike at `s`: absolute offset and length (false when empty / out of range).
bool sbixRecord(const Reader& r, std::uint64_t s, std::uint16_t numGlyphs, std::uint32_t glyph, std::uint64_t& rec,
                std::uint64_t& len) {
    if (glyph >= numGlyphs) return false;
    const std::uint64_t p = s + 4 + std::uint64_t(glyph) * 4;
    const std::uint32_t o0 = r.u32(p), o1 = r.u32(p + 4);
    if (o1 <= o0 || o1 - o0 < 8) return false;
    rec = s + o0;
    len = o1 - o0;
    return r.in(rec, len);
}

// sbix placement: the spec puts the image's bottom-left corner at (originOffsetX, originOffsetY) strike pixels from
// the glyph origin, y up. FreeType additionally shifts by the outline's bbox (xMin, yMin) when the glyph also has
// contours; we have no outline access here and follow the spec. Apple's emoji glyphs have no contours, so both agree.
bool sbixFind(const Reader& r, std::uint64_t s, std::uint16_t numGlyphs, std::uint32_t glyph, Located& out) {
    std::uint64_t rec = 0, len = 0;
    if (!sbixRecord(r, s, numGlyphs, glyph, rec, len)) return false;
    std::uint32_t type = r.u32(rec + 4);
    if (type == kDupe) { // follow once
        if (len < 10) return false;
        if (!sbixRecord(r, s, numGlyphs, r.u16(rec + 8), rec, len)) return false;
        type = r.u32(rec + 4);
    }
    if (type != kPng || len <= 8 || len - 8 > 0x7fffffffu) return false;
    out.left = r.i16(rec);
    out.vert = r.i16(rec + 2);
    out.fromBottom = true;
    out.png = rec + 8;
    out.pngLen = static_cast<std::uint32_t>(len - 8);
    return true;
}

// ---- decode and resample ----

struct StbiFree {
    void operator()(stbi_uc* p) const { stbi_image_free(p); }
};

/// Decodes a PNG into premultiplied float RGBA (0..1).
bool decodePng(std::span<const std::uint8_t> png, int& w, int& h, std::vector<float>& px) {
    if (png.size() > 0x7fffffffu) return false;
    const int n = static_cast<int>(png.size());
    int comp = 0;
    if (!stbi_info_from_memory(png.data(), n, &w, &h, &comp)) return false;
    if (w <= 0 || h <= 0 || w > kMaxImage || h > kMaxImage) return false;
    int dw = 0, dh = 0;
    const std::unique_ptr<stbi_uc, StbiFree> img(stbi_load_from_memory(png.data(), n, &dw, &dh, &comp, 4));
    if (!img || dw != w || dh != h) return false;
    const std::size_t count = std::size_t(w) * std::size_t(h);
    px.resize(count * 4);
    const stbi_uc* s = img.get();
    for (std::size_t i = 0; i < count; ++i) {
        const float a = float(s[i * 4 + 3]) * (1.f / 255.f);
        px[i * 4 + 0] = float(s[i * 4 + 0]) * (1.f / 255.f) * a;
        px[i * 4 + 1] = float(s[i * 4 + 1]) * (1.f / 255.f) * a;
        px[i * 4 + 2] = float(s[i * 4 + 2]) * (1.f / 255.f) * a;
        px[i * 4 + 3] = a;
    }
    return true;
}

struct Tap {
    int src = 0;
    float w = 0.f;
};

/// One axis of the resampler. The source (n texels) spans [start, start + n * s) in device pixels; output texel o
/// covers [dst0 + o, dst0 + o + 1). Downscaling (s <= 1) is an exact box (area) filter with transparent outside;
/// upscaling is bilinear (clamped to the edge) times the texel's coverage of the image span, so edges stay crisp.
void axisTaps(double start, double s, int n, int dst0, int count, std::vector<std::size_t>& first, std::vector<Tap>& taps) {
    first.assign(std::size_t(count) + 1, 0);
    taps.clear();
    const double end = start + double(n) * s;
    for (int o = 0; o < count; ++o) {
        first[std::size_t(o)] = taps.size();
        const double x = double(dst0 + o);
        if (s <= 1.0) {
            const double a = (x - start) / s, b = (x + 1.0 - start) / s;
            const int k0 = std::max(0, int(std::floor(a)));
            const int k1 = std::min(n - 1, int(std::ceil(b)) - 1);
            for (int k = k0; k <= k1; ++k) {
                const double w = (std::min(b, double(k + 1)) - std::max(a, double(k))) * s;
                if (w > 0.0) taps.push_back({k, float(w)});
            }
        } else {
            const double cover = std::min(x + 1.0, end) - std::max(x, start);
            if (cover <= 0.0) continue;
            const double u = std::clamp((x + 0.5 - start) / s - 0.5, 0.0, double(n - 1));
            const int k0 = int(std::floor(u));
            const int k1 = std::min(k0 + 1, n - 1);
            const double f = u - double(k0);
            taps.push_back({k0, float((1.0 - f) * cover)});
            if (f > 0.0 && k1 != k0) taps.push_back({k1, float(f * cover)});
        }
    }
    first[std::size_t(count)] = taps.size();
}

bool renderLocated(const Reader& r, const Located& loc, float strikePpem, float ppem, ColorBitmap& out) {
    int iw = 0, ih = 0;
    std::vector<float> src;
    if (!r.in(loc.png, loc.pngLen) || !decodePng(r.d.subspan(static_cast<std::size_t>(loc.png), loc.pngLen), iw, ih, src))
        return false;
    const double s = double(ppem) / double(strikePpem);
    // Image rectangle in device pixels (y down, pen at the origin on the baseline).
    const double top = loc.fromBottom ? -(double(loc.vert) + double(ih)) * s : -double(loc.vert) * s;
    const double left = double(loc.left) * s;
    const double right = left + double(iw) * s, bottom = top + double(ih) * s;
    constexpr double eps = 1e-6; // keeps exact edges from spilling into an empty texel
    const double fx0 = std::floor(left + eps), fy0 = std::floor(top + eps);
    const double fw = std::ceil(right - eps) - fx0, fh = std::ceil(bottom - eps) - fy0;
    if (!(fw >= 1.0 && fh >= 1.0 && fw <= kMaxOutput && fh <= kMaxOutput) || std::fabs(fx0) > 1e6 || std::fabs(fy0) > 1e6)
        return false;
    const int x0 = int(fx0), y0 = int(fy0), w = int(fw), h = int(fh);

    std::vector<std::size_t> fx, fy;
    std::vector<Tap> tx, ty;
    axisTaps(left, s, iw, x0, w, fx, tx);
    axisTaps(top, s, ih, y0, h, fy, ty);

    // Horizontal pass: ih rows x w columns.
    std::vector<float> tmp(std::size_t(ih) * std::size_t(w) * 4, 0.f);
    for (int y = 0; y < ih; ++y) {
        const float* row = src.data() + std::size_t(y) * std::size_t(iw) * 4;
        float* dst = tmp.data() + std::size_t(y) * std::size_t(w) * 4;
        for (int o = 0; o < w; ++o) {
            float acc[4] = {0.f, 0.f, 0.f, 0.f};
            for (std::size_t t = fx[std::size_t(o)]; t < fx[std::size_t(o) + 1]; ++t) {
                const float* p = row + std::size_t(tx[t].src) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * tx[t].w;
            }
            for (int c = 0; c < 4; ++c) dst[std::size_t(o) * 4 + std::size_t(c)] = acc[c];
        }
    }
    // Vertical pass and quantisation.
    out.w = w;
    out.h = h;
    out.x0 = x0;
    out.y0 = y0;
    out.rgba.assign(std::size_t(w) * std::size_t(h) * 4, 0);
    for (int o = 0; o < h; ++o) {
        std::uint8_t* dst = out.rgba.data() + std::size_t(o) * std::size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0.f, 0.f, 0.f, 0.f};
            for (std::size_t t = fy[std::size_t(o)]; t < fy[std::size_t(o) + 1]; ++t) {
                const float* p = tmp.data() + (std::size_t(ty[t].src) * std::size_t(w) + std::size_t(x)) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * ty[t].w;
            }
            std::uint8_t q[4];
            for (int c = 0; c < 4; ++c)
                q[c] = static_cast<std::uint8_t>(std::floor(std::clamp(acc[c], 0.f, 1.f) * 255.f + 0.5f));
            for (int c = 0; c < 3; ++c) q[c] = std::min(q[c], q[3]); // stay premultiplied after rounding
            for (int c = 0; c < 4; ++c) dst[std::size_t(x) * 4 + std::size_t(c)] = q[c];
        }
    }
    return true;
}

/// Strike preference for `ppem`: the smallest strike >= ppem, then larger ones, then smaller ones (largest first).
void orderStrikes(std::vector<Strike>& strikes, float ppem) {
    std::stable_sort(strikes.begin(), strikes.end(), [ppem](const Strike& a, const Strike& b) {
        const bool ua = a.ppem >= ppem, ub = b.ppem >= ppem;
        if (ua != ub) return ua;
        return ua ? a.ppem < b.ppem : a.ppem > b.ppem;
    });
}

} // namespace

bool bitmapGlyphRender(const BitmapSource& src, std::uint32_t glyph, float ppem, ColorBitmap& out) {
    if (!(ppem > 0.f) || !std::isfinite(ppem) || glyph > 0xFFFF) return false;
    const Reader r{src.data};
    std::vector<Strike> strikes;
    if (src.cblc && src.cbdt) {
        cblcStrikes(r, src.cblc, strikes);
        orderStrikes(strikes, ppem);
        // The first strike holding the glyph wins (a glyph missing from the preferred strike falls back).
        for (const Strike& st : strikes) {
            Located loc;
            if (cblcFind(r, src.cblc, src.cbdt, st.at, glyph, loc)) return renderLocated(r, loc, st.ppem, ppem, out);
        }
    }
    if (src.sbix) {
        strikes.clear();
        sbixStrikes(r, src.sbix, src.numGlyphs, strikes);
        orderStrikes(strikes, ppem);
        for (const Strike& st : strikes) {
            Located loc;
            if (sbixFind(r, st.at, src.numGlyphs, glyph, loc)) return renderLocated(r, loc, st.ppem, ppem, out);
        }
    }
    return false;
}

void bitmapGlyphs(const BitmapSource& src, std::vector<std::uint32_t>& out) {
    out.clear();
    const Reader r{src.data};
    std::vector<std::uint8_t> seen(0x10000, 0);
    // Work budget: a hostile CBLC could repeat full-range subtables; real fonts need a tiny fraction of this.
    std::uint64_t budget = std::uint64_t(1) << 24;
    auto mark = [&](std::uint32_t g) { seen[g & 0xFFFF] = 1; };
    std::vector<Strike> strikes;
    if (src.cblc && src.cbdt) {
        cblcStrikes(r, src.cblc, strikes);
        for (const Strike& st : strikes) {
            const std::uint64_t arr = src.cblc + r.u32(st.at);
            const std::uint32_t n = r.u32(st.at + 8);
            if (n > kMaxSubtables || !r.in(arr, std::uint64_t(n) * 8)) continue;
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint64_t e = arr + std::uint64_t(i) * 8;
                const std::uint16_t first = r.u16(e), last = r.u16(e + 2);
                const std::uint64_t sub = arr + r.u32(e + 4);
                if (last < first || !r.in(sub, 8)) continue;
                const std::uint16_t indexFormat = r.u16(sub), imageFormat = r.u16(sub + 2);
                if (imageFormat < 17 || imageFormat > 19) continue;
                const std::uint32_t count = std::uint32_t(last - first) + 1;
                if (budget < count + 1u) break;
                budget -= count + 1u;
                switch (indexFormat) {
                case 1:
                case 3: {
                    const std::uint64_t step = indexFormat == 1 ? 4 : 2;
                    if (!r.in(sub + 8, (std::uint64_t(count) + 1) * step)) break;
                    for (std::uint32_t k = 0; k < count; ++k) {
                        const std::uint64_t p = sub + 8 + std::uint64_t(k) * step;
                        const std::uint32_t o0 = step == 4 ? r.u32(p) : r.u16(p);
                        const std::uint32_t o1 = step == 4 ? r.u32(p + 4) : r.u16(p + 2);
                        if (o1 > o0) mark(first + k);
                    }
                    break;
                }
                case 2:
                    for (std::uint32_t k = 0; k < count; ++k) mark(first + k);
                    break;
                case 4: {
                    if (!r.in(sub + 8, 4)) break;
                    const std::uint32_t num = std::min(r.u32(sub + 8), count);
                    if (!r.in(sub + 12, (std::uint64_t(num) + 1) * 4)) break;
                    for (std::uint32_t k = 0; k < num; ++k) {
                        const std::uint64_t p = sub + 12 + std::uint64_t(k) * 4;
                        if (r.u16(p + 6) > r.u16(p + 2)) mark(r.u16(p));
                    }
                    break;
                }
                case 5: {
                    if (!r.in(sub + 8, 16)) break;
                    const std::uint32_t num = std::min(r.u32(sub + 20), count);
                    if (!r.in(sub + 24, std::uint64_t(num) * 2)) break;
                    for (std::uint32_t k = 0; k < num; ++k) mark(r.u16(sub + 24 + std::uint64_t(k) * 2));
                    break;
                }
                default: break;
                }
            }
        }
    }
    if (src.sbix) {
        strikes.clear();
        sbixStrikes(r, src.sbix, src.numGlyphs, strikes);
        for (const Strike& st : strikes) {
            for (std::uint32_t g = 0; g < src.numGlyphs; ++g) {
                std::uint64_t rec = 0, len = 0;
                if (!sbixRecord(r, st.at, src.numGlyphs, g, rec, len)) continue;
                const std::uint32_t type = r.u32(rec + 4);
                if (type == kPng || type == kDupe) mark(g);
            }
        }
    }
    const std::uint32_t limit = src.numGlyphs ? src.numGlyphs : 0x10000u;
    for (std::uint32_t g = 0; g < limit; ++g)
        if (seen[g]) out.push_back(g);
}

} // namespace fuse::text::detail
