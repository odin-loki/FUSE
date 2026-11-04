#include "fuse/ui/render/gpu_draw_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::ui::render {

namespace {
struct Clip {
    Rect r;
    f32 radius;
};
struct Area {
    int x0, y0, x1, y1;
};
void put4(f32 (&d)[4], f32 a, f32 b, f32 c, f32 e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}

// Bins commands [first, areas.size()) into a new tile block at the end of out.tiles and appends the segment.
void closeSegment(GpuDrawData& out, const std::vector<Area>& areas, std::size_t first, u32 tilesX, u32 tileCount,
                  u32 backdrop, u32 blurPixels) {
    GpuSegment seg;
    seg.backdrop = backdrop;
    seg.blurPixels = blurPixels;
    seg.cmdBegin = u32(first);
    seg.cmdEnd = u32(areas.size());
    seg.tileBase = u32(out.tiles.size());
    const std::size_t base = out.tiles.size();
    out.tiles.resize(base + tileCount + 1, 0u);
    u32* counts = out.tiles.data() + base;
    int tx0 = int(tilesX), ty0 = 1 << 16, tx1 = 0, ty1 = 0;
    for (std::size_t i = first; i < areas.size(); ++i) {
        const Area& a = areas[i];
        if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
        const int ax0 = a.x0 / int(kUiTile), ay0 = a.y0 / int(kUiTile);
        const int ax1 = (a.x1 - 1) / int(kUiTile), ay1 = (a.y1 - 1) / int(kUiTile);
        tx0 = std::min(tx0, ax0);
        ty0 = std::min(ty0, ay0);
        tx1 = std::max(tx1, ax1 + 1);
        ty1 = std::max(ty1, ay1 + 1);
        for (int ty = ay0; ty <= ay1; ++ty)
            for (int tx = ax0; tx <= ax1; ++tx) ++counts[std::size_t(ty) * tilesX + tx];
    }
    if (tx1 > tx0 && ty1 > ty0) {
        seg.tile0 = u32(tx0) | (u32(ty0) << 16);
        seg.tile1 = u32(tx1) | (u32(ty1) << 16);
    }
    // prefix sums -> offsets; then ids in draw order
    u32 sum = 0;
    for (u32 t = 0; t < tileCount; ++t) {
        const u32 n = counts[t];
        counts[t] = sum;
        sum += n;
    }
    counts[tileCount] = sum;
    const std::size_t ids = base + tileCount + 1;
    out.tiles.resize(ids + sum, 0u);
    thread_local std::vector<u32> cursor;
    cursor.assign(out.tiles.begin() + std::ptrdiff_t(base), out.tiles.begin() + std::ptrdiff_t(base + tileCount));
    for (std::size_t i = first; i < areas.size(); ++i) {
        const Area& a = areas[i];
        if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
        for (int ty = a.y0 / int(kUiTile); ty <= (a.y1 - 1) / int(kUiTile); ++ty)
            for (int tx = a.x0 / int(kUiTile); tx <= (a.x1 - 1) / int(kUiTile); ++tx)
                out.tiles[ids + cursor[std::size_t(ty) * tilesX + tx]++] = u32(i);
    }
    out.segments.push_back(seg);
}
} // namespace

void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas, u32 atlasSize,
                  const Image* const* images, u32 imageCount, Rect scissor, const Color* clear) {
    out.cmds.clear();
    out.tiles.clear();
    out.images.clear();
    out.segments.clear();
    out.atlas = atlas;
    out.atlasSize = atlas ? atlasSize : 0;
    out.atlasColor = false;
    const u32 tilesX = (width + kUiTile - 1) / kUiTile, tilesY = (height + kUiTile - 1) / kUiTile;
    const u32 tileCount = tilesX * tilesY;
    out.push = GpuPush{};
    out.push.extent[0] = width;
    out.push.extent[1] = height;
    out.push.tilesX = tilesX;
    out.push.atlasSize = out.atlasSize;
    out.push.tileCount = tileCount;
    out.push.imageCount = imageCount;

    // images: table then pixels
    out.images.resize(std::size_t(imageCount) * 4);
    for (u32 i = 0; i < imageCount; ++i) {
        const Image* img = images ? images[i] : nullptr;
        out.images[i * 4 + 0] = u32(out.images.size());
        out.images[i * 4 + 1] = img ? u32(img->width) : 1u;
        out.images[i * 4 + 2] = img ? u32(img->height) : 1u;
        // external GPU texture: the kernel samples the bound slot; the CPU pixels stay the rasteriser's fallback
        const bool ext = img && img->externalSlot >= 0 && img->externalSlot < 4;
        out.images[i * 4 + 3] = ext ? u32(img->externalSlot) + 1u : 0u;
        if (ext) {
            out.images.push_back(0u);
            continue;
        }
        if (img && !img->pixels.empty()) out.images.insert(out.images.end(), img->pixels.begin(), img->pixels.end());
        else out.images.push_back(0u);
    }
    if (out.images.empty()) out.images.push_back(0u); // a storage buffer may not be empty

    const Rect full{0.f, 0.f, f32(width), f32(height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    Clip stack[32];
    int depth = 0;
    stack[0] = {scissor, 0.f};

    // commands + their pixel areas; a Backdrop closes the current segment
    thread_local std::vector<Area> areas;
    areas.clear();
    std::size_t segFirst = 0;
    if (clear && !scissor.empty()) { // an opaque rect over the scissor stores the colour exactly
        GpuCmd g{};
        put4(g.rect, scissor.x, scissor.y, scissor.w, scissor.h);
        put4(g.fill, clear->r, clear->g, clear->b, 1.f);
        put4(g.clip, scissor.x, scissor.y, scissor.w, scissor.h);
        out.cmds.push_back(g);
        areas.push_back({std::max(0, int(std::floor(scissor.x))), std::max(0, int(std::floor(scissor.y))),
                         std::min(int(width), int(std::ceil(scissor.right()))),
                         std::min(int(height), int(std::ceil(scissor.bottom())))});
    }
    for (const DrawCmd& c : list.cmds()) {
        if (c.op == DrawOp::PushClip) {
            if (depth < 31) {
                ++depth;
                stack[depth] = {stack[depth - 1].r.intersect(c.rect), c.radius[0]};
                if (stack[depth - 1].radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            continue;
        }
        if (c.op == DrawOp::PopClip) {
            if (depth > 0) --depth;
            continue;
        }
        const Clip& clip = stack[depth];
        GpuCmd g{};
        put4(g.rect, c.rect.x, c.rect.y, c.rect.w, c.rect.h);
        put4(g.fill, c.fill.r, c.fill.g, c.fill.b, c.fill.a);
        put4(g.fill2, c.fill2.r, c.fill2.g, c.fill2.b, c.fill2.a);
        put4(g.border, c.border.r, c.border.g, c.border.b, c.border.a);
        put4(g.radius, c.radius[0], c.radius[1], c.radius[2], c.radius[3]);
        put4(g.clip, clip.r.x, clip.r.y, clip.r.w, clip.r.h);
        put4(g.params, c.borderWidth, c.blur, clip.radius, 0.f);
        if (c.op == DrawOp::Backdrop) {
            // same area / radius / source region as CpuRasterizer's backdropBlur
            const Rect area = c.rect.intersect(clip.r).intersect(full);
            if (area.empty()) continue;
            const int r = std::max(1, int(std::lround(c.blur / 3.f)));
            const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
            const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
            const int m = 3 * r;
            const int sw = std::min(int(width), x1 + m) - std::max(0, x0 - m);
            const int sh = std::min(int(height), y1 + m) - std::max(0, y0 - m);
            g.info[0] = u32(c.op);
            g.info[1] = u32(x0) | (u32(y0) << 16);
            g.info[2] = u32(x1) | (u32(y1) << 16);
            g.info[3] = u32(r);
            out.cmds.push_back(g);
            areas.push_back({0, 0, 0, 0}); // not binned
            closeSegment(out, areas, segFirst, tilesX, tileCount, u32(out.cmds.size() - 1), u32(sw) * u32(sh));
            segFirst = areas.size();
            continue;
        }
        if (c.op == DrawOp::Effect) {
            // same pixel area / source region as CpuRasterizer's effectPass
            const Rect area = c.rect.intersect(clip.r).intersect(full);
            if (area.empty()) continue;
            const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
            const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
            const int m = std::clamp(int(std::ceil(c.borderWidth)), 0, 0xFFFF);
            const int sw = std::min(int(width), x1 + m) - std::max(0, x0 - m);
            const int sh = std::min(int(height), y1 + m) - std::max(0, y0 - m);
            g.info[0] = u32(c.op);
            g.info[1] = u32(x0) | (u32(y0) << 16);
            g.info[2] = u32(x1) | (u32(y1) << 16);
            g.info[3] = u32(m) | (u32(c.u) << 16);
            put4(g.border, c.image < imageCount ? f32(c.image) : -1.f, 0.f, 0.f, 0.f);
            out.cmds.push_back(g);
            areas.push_back({0, 0, 0, 0}); // not binned
            closeSegment(out, areas, segFirst, tilesX, tileCount, u32(out.cmds.size() - 1), u32(sw) * u32(sh));
            segFirst = areas.size();
            continue;
        }
        Rect bounds = c.rect;
        if (c.op == DrawOp::Shadow)
            bounds = {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
        if (c.op == DrawOp::Line) {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            bounds = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw,
                      std::abs(c.rect.w - c.rect.x) + 2 * hw, std::abs(c.rect.h - c.rect.y) + 2 * hw};
        }
        const Rect area = bounds.intersect(clip.r);
        if (area.empty()) continue;
        if (c.op == DrawOp::Glyph && !atlas) continue;
        if (c.op == DrawOp::Glyph && c.gradient != kGlyphCoverage) out.atlasColor = true;
        if (c.op == DrawOp::Image && c.image >= imageCount) continue;
        g.info[0] = u32(c.op) | (u32(c.gradient) << 8);
        g.info[1] = u32(c.u) | (u32(c.v) << 16);
        g.info[2] = u32(c.tw) | (u32(c.th) << 16);
        g.info[3] = c.image;
        out.cmds.push_back(g);
        areas.push_back({std::max(0, int(std::floor(area.x))), std::max(0, int(std::floor(area.y))),
                         std::min(int(width), int(std::ceil(area.right()))),
                         std::min(int(height), int(std::ceil(area.bottom())))});
    }
    if (areas.size() > segFirst || out.segments.empty())
        closeSegment(out, areas, segFirst, tilesX, tileCount, ~0u, 0);
    out.tiles.push_back(0u);                            // never empty
    if (out.cmds.empty()) out.cmds.push_back(GpuCmd{}); // non-empty buffer; no tile references it
}

} // namespace fuse::ui::render
