// SVG -> .fuseicons cooker (see icon_cook.hpp). nanosvg is compiled into this file only.
#include "fuse/ui/icons/icon_cook.hpp"
#include "fuse/ui/icons.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fuse::ui::icons {

namespace {
struct ImageDel { void operator()(NSVGimage* p) const { nsvgDelete(p); } };
struct RastDel { void operator()(NSVGrasterizer* p) const { nsvgDeleteRasterizer(p); } };

bool validName(std::string_view n) {
    if (n.empty() || n.size() > 255) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

struct Writer {
    std::vector<u8> b;
    void raw(const void* p, std::size_t n) { const u8* c = static_cast<const u8*>(p); b.insert(b.end(), c, c + n); }
    void u16_(u16 v) { raw(&v, 2); }
    void u32_(u32 v) { raw(&v, 4); }
};

struct Bitmap { // trimmed coverage of one (icon, layer, size)
    u32 icon = 0, layer = 0, sizeIndex = 0;
    int w = 0, h = 0, ox = 0, oy = 0;
    std::vector<u8> px;
    int u = 0, v = 0;
};

// currentColor parses as an unknown colour name in nanosvg: swap it for a sentinel first (case-insensitive)
constexpr u32 kSentinel = 0xe3ffc0u; // NSVG_RGB(0xc0, 0xff, 0xe3): "#c0ffe3"
std::string withSentinel(std::string_view svg) {
    std::string s(svg);
    static constexpr std::string_view kWord = "currentcolor";
    for (std::size_t i = 0; i + kWord.size() <= s.size(); ++i) {
        bool eq = true;
        for (std::size_t k = 0; k < kWord.size() && eq; ++k)
            eq = char(std::tolower(static_cast<unsigned char>(s[i + k]))) == kWord[k];
        if (eq) s.replace(i, kWord.size(), "#c0ffe3");
    }
    return s;
}

// Layer colour of a paint: kCurrent for currentColor, RGBA8 (R low byte, A = 255) otherwise; gradients: their mean stop
// colour (the stops' alpha stays in the coverage).
constexpr u32 kCurrent = 0u;
u32 paintColor(const NSVGpaint& p) {
    if (p.type == NSVG_PAINT_COLOR) {
        const u32 rgb = p.color & 0xFFFFFFu;
        return rgb == kSentinel ? kCurrent : rgb | 0xFF000000u;
    }
    if ((p.type == NSVG_PAINT_LINEAR_GRADIENT || p.type == NSVG_PAINT_RADIAL_GRADIENT) && p.gradient && p.gradient->nstops > 0) {
        u32 sum[3] = {};
        for (int i = 0; i < p.gradient->nstops; ++i)
            for (int k = 0; k < 3; ++k) sum[k] += (p.gradient->stops[i].color >> (8 * k)) & 0xFFu;
        const u32 n = u32(p.gradient->nstops);
        return ((sum[0] + n / 2) / n) | (((sum[1] + n / 2) / n) << 8) | (((sum[2] + n / 2) / n) << 16) | 0xFF000000u;
    }
    return kCurrent;
}

// False for shapes that cannot cover anything when filled (a <line>: nanosvg still gives it the default black fill)
bool fillable(const NSVGshape* s) {
    for (const NSVGpath* p = s->paths; p; p = p->next) {
        double a = 0.0; // shoelace over the points (control points included: zero only for degenerate outlines)
        for (int i = 0; i < p->npts; ++i) {
            const int j = (i + 1) % p->npts;
            a += double(p->pts[i * 2]) * p->pts[j * 2 + 1] - double(p->pts[j * 2]) * p->pts[i * 2 + 1];
        }
        if (std::abs(a) > 1e-9) return true;
    }
    return false;
}

struct Part { // a paint of a shape that belongs to a layer
    NSVGshape* shape;
    bool fill;
};
struct LayerPlan {
    u32 color = kCurrent;
    std::vector<Part> parts;
};
struct Parsed {
    std::unique_ptr<NSVGimage, ImageDel> img;
    std::vector<LayerPlan> layers; ///< one layer (kCurrent) for a monochrome icon
};

std::expected<Parsed, std::string> parseIcon(std::string_view svg) {
    std::string text = withSentinel(svg); // nsvgParse edits its input
    Parsed p;
    p.img.reset(nsvgParse(text.data(), "px", 96.f));
    if (!p.img) return std::unexpected("SVG parse failed");
    if (!(p.img->width > 0.f) || !(p.img->height > 0.f)) return std::unexpected("SVG has no size (width / height / viewBox)");
    if (!p.img->shapes) return std::unexpected("SVG has no painted shapes");
    // paints in paint order, consecutive equal colours merged
    bool multi = false;
    for (NSVGshape* s = p.img->shapes; s; s = s->next) {
        if (!(s->flags & NSVG_FLAGS_VISIBLE)) continue;
        for (int j = 0; j < 3; ++j) {
            const int order = (s->paintOrder >> (2 * j)) & 0x03;
            const bool fill = order == NSVG_PAINT_FILL;
            if (!fill && order != NSVG_PAINT_STROKE) continue;
            const NSVGpaint& paint = fill ? s->fill : s->stroke;
            if (paint.type == NSVG_PAINT_NONE || paint.type == NSVG_PAINT_UNDEF) continue;
            if (!fill && !(s->strokeWidth > 0.f)) continue;
            if (fill && !fillable(s)) continue;
            const u32 c = paintColor(paint);
            multi |= c != kCurrent && c != 0xFF000000u; // black (the SVG default fill) alone is still ink
            if (p.layers.empty() || p.layers.back().color != c) p.layers.push_back({c, {}});
            p.layers.back().parts.push_back({s, fill});
        }
    }
    if (p.layers.empty()) return std::unexpected("SVG has no painted shapes");
    if (!multi) { // monochrome: every paint is ink
        LayerPlan all;
        for (const LayerPlan& l : p.layers) all.parts.insert(all.parts.end(), l.parts.begin(), l.parts.end());
        p.layers.assign(1, std::move(all));
    }
    if (p.layers.size() > 255) return std::unexpected("more than 255 colour layers");
    return p;
}

// size x size coverage of the parts of `layer` (null: every shape), centred like the cell
std::vector<u8> rasteriseParsed(Parsed& p, const LayerPlan* layer, int size, NSVGrasterizer* rast) {
    NSVGimage* img = p.img.get();
    struct Saved {
        NSVGshape* s;
        signed char fill, stroke;
        unsigned char flags;
    };
    std::vector<Saved> saved;
    if (layer) { // hide every paint that is not in the layer (restored below: nsvgDelete frees gradients by type)
        for (NSVGshape* s = img->shapes; s; s = s->next) {
            saved.push_back({s, s->fill.type, s->stroke.type, s->flags});
            bool f = false, st = false;
            for (const Part& part : layer->parts)
                if (part.shape == s) (part.fill ? f : st) = true;
            if (!f) s->fill.type = NSVG_PAINT_NONE;
            if (!st) s->stroke.type = NSVG_PAINT_NONE;
            if (!f && !st) s->flags = u8(s->flags & ~NSVG_FLAGS_VISIBLE);
        }
    }
    const float scale = float(size) / std::max(img->width, img->height);
    const float tx = (float(size) - img->width * scale) * 0.5f, ty = (float(size) - img->height * scale) * 0.5f;
    std::vector<unsigned char> rgba(std::size_t(size) * std::size_t(size) * 4, 0);
    nsvgRasterize(rast, img, tx, ty, scale, rgba.data(), size, size, size * 4);
    for (const Saved& s : saved) {
        s.s->fill.type = s.fill;
        s.s->stroke.type = s.stroke;
        s.s->flags = s.flags;
    }
    std::vector<u8> cov(std::size_t(size) * std::size_t(size));
    for (std::size_t i = 0; i < cov.size(); ++i) cov[i] = rgba[i * 4 + 3];
    return cov;
}
} // namespace

std::expected<std::vector<u8>, std::string> rasteriseSvg(std::string_view svg, int size) {
    if (size < 1 || size > 512) return std::unexpected("bad size");
    auto p = parseIcon(svg);
    if (!p) return std::unexpected(p.error());
    std::unique_ptr<NSVGrasterizer, RastDel> rast(nsvgCreateRasterizer());
    if (!rast) return std::unexpected("rasteriser allocation failed");
    return rasteriseParsed(*p, nullptr, size, rast.get());
}

std::expected<std::vector<SvgLayer>, std::string> rasteriseSvgLayers(std::string_view svg, int size) {
    if (size < 1 || size > 512) return std::unexpected("bad size");
    auto p = parseIcon(svg);
    if (!p) return std::unexpected(p.error());
    std::unique_ptr<NSVGrasterizer, RastDel> rast(nsvgCreateRasterizer());
    if (!rast) return std::unexpected("rasteriser allocation failed");
    std::vector<SvgLayer> out;
    const bool mono = p->layers.size() == 1 && p->layers[0].color == kCurrent;
    for (const LayerPlan& l : p->layers) out.push_back({l.color, rasteriseParsed(*p, mono ? nullptr : &l, size, rast.get())});
    return out;
}

std::expected<std::vector<u8>, std::string> cookIcons(std::vector<SvgSource> sources, const CookOptions& opt, CookStats* stats) {
    if (opt.sizes.empty() || opt.sizes.size() > 64) return std::unexpected("need 1..64 sizes");
    for (int s : opt.sizes)
        if (s < 1 || s > 512) return std::unexpected("size out of range (1..512): " + std::to_string(s));
    std::sort(sources.begin(), sources.end(), [](const SvgSource& a, const SvgSource& b) { return a.name < b.name; });
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!validName(sources[i].name)) return std::unexpected("bad icon name '" + sources[i].name + "' (use [a-z0-9_.-])");
        if (i && sources[i].name == sources[i - 1].name) return std::unexpected("duplicate icon name '" + sources[i].name + "'");
    }
    std::vector<u32> hashes;
    for (const auto& s : sources) hashes.push_back(hashId(s.name));
    {
        std::vector<u32> h = hashes;
        std::sort(h.begin(), h.end());
        if (std::adjacent_find(h.begin(), h.end()) != h.end()) return std::unexpected("two icon names share a hash; rename one");
    }
    // rasterise every (icon, layer, size) + trim
    std::vector<Bitmap> bms;
    std::vector<std::vector<u32>> colors(sources.size()); ///< layer colours per icon
    u32 multi = 0;
    for (u32 i = 0; i < u32(sources.size()); ++i)
        for (u32 k = 0; k < u32(opt.sizes.size()); ++k) {
            const int size = opt.sizes[k];
            auto layers = rasteriseSvgLayers(sources[i].svg, size);
            if (!layers) return std::unexpected(sources[i].name + ": " + layers.error());
            if (k == 0) {
                for (const SvgLayer& l : *layers) colors[i].push_back(l.color);
                multi += layers->size() > 1 || (*layers)[0].color != 0u;
            }
            for (u32 l = 0; l < u32(layers->size()); ++l) {
                const std::vector<u8>& cov = (*layers)[l].coverage;
                int x0 = size, y0 = size, x1 = -1, y1 = -1;
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                        if (cov[std::size_t(y) * std::size_t(size) + std::size_t(x)]) {
                            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                        }
                Bitmap b;
                b.icon = i;
                b.layer = l;
                b.sizeIndex = k;
                if (x1 >= 0) {
                    b.w = x1 - x0 + 1; b.h = y1 - y0 + 1; b.ox = x0; b.oy = y0;
                    b.px.resize(std::size_t(b.w) * std::size_t(b.h));
                    for (int y = 0; y < b.h; ++y)
                        std::memcpy(&b.px[std::size_t(y) * std::size_t(b.w)], &cov[std::size_t(y0 + y) * std::size_t(size) + std::size_t(x0)], std::size_t(b.w));
                }
                bms.push_back(std::move(b));
            }
        }
    // (icon, layer, size) order, so the writer can walk bms in step
    std::stable_sort(bms.begin(), bms.end(), [](const Bitmap& a, const Bitmap& b) {
        if (a.icon != b.icon) return a.icon < b.icon;
        if (a.layer != b.layer) return a.layer < b.layer;
        return a.sizeIndex < b.sizeIndex;
    });
    // shelf pack, tallest first (ties: index: deterministic), 1 px gutters
    std::vector<u32> order(bms.size());
    for (u32 i = 0; i < u32(order.size()); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (bms[a].h != bms[b].h) return bms[a].h > bms[b].h;
        if (bms[a].w != bms[b].w) return bms[a].w > bms[b].w;
        return a < b;
    });
    std::size_t area = 0;
    int maxW = 1;
    for (const auto& b : bms) { area += std::size_t(b.w + 1) * std::size_t(b.h + 1); maxW = std::max(maxW, b.w + 2); }
    int W = 64;
    while (W < maxW || std::size_t(W) * std::size_t(W) < area + area / 8) W *= 2;
    int x = 1, y = 1, shelfH = 0;
    for (u32 i : order) {
        Bitmap& b = bms[i];
        if (b.w == 0) continue;
        if (x + b.w + 1 > W) { x = 1; y += shelfH + 1; shelfH = 0; }
        b.u = x;
        b.v = y;
        x += b.w + 1;
        shelfH = std::max(shelfH, b.h);
    }
    const int H = std::max(4, (y + shelfH + 1 + 3) / 4 * 4);
    if (W > 65535 || H > 65535) return std::unexpected("atlas too large");
    std::vector<u8> atlas(std::size_t(W) * std::size_t(H), 0);
    for (const Bitmap& b : bms)
        for (int r = 0; r < b.h; ++r)
            std::memcpy(&atlas[std::size_t(b.v + r) * std::size_t(W) + std::size_t(b.u)], &b.px[std::size_t(r) * std::size_t(b.w)], std::size_t(b.w));
    // write (version 2: per icon a layer count, per layer its colour and per-size entries)
    Writer w;
    w.raw(kIconPackMagic, 4);
    w.u32_(kIconPackVersion);
    w.u16_(u16(W));
    w.u16_(u16(H));
    w.u16_(u16(opt.sizes.size()));
    for (int s : opt.sizes) w.u16_(u16(s));
    w.u32_(u32(sources.size()));
    std::size_t at = 0;
    for (u32 i = 0; i < u32(sources.size()); ++i) {
        w.u32_(hashes[i]);
        w.u16_(u16(sources[i].name.size()));
        w.raw(sources[i].name.data(), sources[i].name.size());
        w.u16_(u16(colors[i].size()));
        for (u32 c : colors[i]) {
            w.u32_(c);
            for (u32 k = 0; k < u32(opt.sizes.size()); ++k) {
                const Bitmap& b = bms[at++];
                for (int v : {b.u, b.v, b.w, b.h, b.ox, b.oy}) w.u16_(u16(v));
            }
        }
    }
    w.raw(atlas.data(), atlas.size());
    if (stats) *stats = {u32(sources.size()), W, H, w.b.size(), multi};
    return std::move(w.b);
}

std::expected<SvgSource, std::string> loadSvgFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("cannot read " + path);
    SvgSource s;
    s.svg.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    s.name = std::filesystem::path(path).stem().string();
    return s;
}

std::expected<std::vector<SvgSource>, std::string> loadSvgDirectory(const std::string& dir) {
    std::error_code ec;
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".svg") files.push_back(e.path().string());
    if (ec) return std::unexpected("cannot list " + dir + ": " + ec.message());
    std::vector<SvgSource> out;
    for (const auto& f : files) {
        auto s = loadSvgFile(f);
        if (!s) return std::unexpected(s.error());
        out.push_back(std::move(*s));
    }
    std::sort(out.begin(), out.end(), [](const SvgSource& a, const SvgSource& b) { return a.name < b.name; });
    return out;
}

} // namespace fuse::ui::icons
