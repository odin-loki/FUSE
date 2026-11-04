// SVG -> .fuseicons cooker (see icon_cook.hpp). nanosvg is compiled into this file only.
#include "fuse/ui/icons/icon_cook.hpp"
#include "fuse/ui/icons.hpp"

#include <algorithm>
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
#include "nanosvg.h"
#include "nanosvgrast.h"
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

struct Bitmap { // trimmed coverage of one (icon, size)
    u32 icon = 0, sizeIndex = 0;
    int w = 0, h = 0, ox = 0, oy = 0;
    std::vector<u8> px;
    int u = 0, v = 0;
};
} // namespace

std::expected<std::vector<u8>, std::string> rasteriseSvg(std::string_view svg, int size) {
    if (size < 1 || size > 512) return std::unexpected("bad size");
    std::string text(svg); // nsvgParse edits its input
    std::unique_ptr<NSVGimage, ImageDel> img(nsvgParse(text.data(), "px", 96.f));
    if (!img) return std::unexpected("SVG parse failed");
    if (!(img->width > 0.f) || !(img->height > 0.f)) return std::unexpected("SVG has no size (width / height / viewBox)");
    if (!img->shapes) return std::unexpected("SVG has no painted shapes");
    std::unique_ptr<NSVGrasterizer, RastDel> rast(nsvgCreateRasterizer());
    if (!rast) return std::unexpected("rasteriser allocation failed");
    const float scale = float(size) / std::max(img->width, img->height);
    const float tx = (float(size) - img->width * scale) * 0.5f, ty = (float(size) - img->height * scale) * 0.5f;
    std::vector<unsigned char> rgba(std::size_t(size) * std::size_t(size) * 4, 0);
    nsvgRasterize(rast.get(), img.get(), tx, ty, scale, rgba.data(), size, size, size * 4);
    std::vector<u8> cov(std::size_t(size) * std::size_t(size));
    for (std::size_t i = 0; i < cov.size(); ++i) cov[i] = rgba[i * 4 + 3];
    return cov;
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
    // rasterise + trim
    std::vector<Bitmap> bms;
    for (u32 i = 0; i < u32(sources.size()); ++i)
        for (u32 k = 0; k < u32(opt.sizes.size()); ++k) {
            const int size = opt.sizes[k];
            auto cov = rasteriseSvg(sources[i].svg, size);
            if (!cov) return std::unexpected(sources[i].name + ": " + cov.error());
            int x0 = size, y0 = size, x1 = -1, y1 = -1;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    if ((*cov)[std::size_t(y) * std::size_t(size) + std::size_t(x)]) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
            Bitmap b;
            b.icon = i;
            b.sizeIndex = k;
            if (x1 >= 0) {
                b.w = x1 - x0 + 1; b.h = y1 - y0 + 1; b.ox = x0; b.oy = y0;
                b.px.resize(std::size_t(b.w) * std::size_t(b.h));
                for (int y = 0; y < b.h; ++y)
                    std::memcpy(&b.px[std::size_t(y) * std::size_t(b.w)], &(*cov)[std::size_t(y0 + y) * std::size_t(size) + std::size_t(x0)], std::size_t(b.w));
            }
            bms.push_back(std::move(b));
        }
    // shelf pack, tallest first (ties: icon, size: deterministic), 1 px gutters
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
    // write
    Writer w;
    w.raw(kIconPackMagic, 4);
    w.u32_(kIconPackVersion);
    w.u16_(u16(W));
    w.u16_(u16(H));
    w.u16_(u16(opt.sizes.size()));
    for (int s : opt.sizes) w.u16_(u16(s));
    w.u32_(u32(sources.size()));
    for (u32 i = 0; i < u32(sources.size()); ++i) {
        w.u32_(hashes[i]);
        w.u16_(u16(sources[i].name.size()));
        w.raw(sources[i].name.data(), sources[i].name.size());
        for (u32 k = 0; k < u32(opt.sizes.size()); ++k) {
            const Bitmap& b = bms[std::size_t(i) * opt.sizes.size() + k];
            for (int v : {b.u, b.v, b.w, b.h, b.ox, b.oy}) w.u16_(u16(v));
        }
    }
    w.raw(atlas.data(), atlas.size());
    if (stats) *stats = {u32(sources.size()), W, H, w.b.size()};
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
