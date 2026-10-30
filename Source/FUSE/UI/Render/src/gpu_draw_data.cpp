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
void put4(f32 (&d)[4], f32 a, f32 b, f32 c, f32 e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}
} // namespace

void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas, u32 atlasSize,
                  const Image* const* images, u32 imageCount, Rect scissor) {
    out.cmds.clear();
    out.tiles.clear();
    out.images.clear();
    out.passes.clear();
    out.atlasGeneration = ~0u;
    out.atlas = atlas;
    out.atlasSize = atlas ? atlasSize : 0;
    const u32 tilesX = (width + kUiTile - 1) / kUiTile, tilesY = (height + kUiTile - 1) / kUiTile;
    const u32 tileCount = tilesX * tilesY;
    out.push = {{width, height}, tilesX, out.atlasSize, tileCount, imageCount, {0, 0}, {width, height}, 0, 0};

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
    {
        const u32 sx0 = u32(std::max(0.f, std::floor(scissor.x))), sy0 = u32(std::max(0.f, std::floor(scissor.y)));
        const u32 sx1 = std::min(width, u32(std::max(0.f, std::ceil(scissor.right()))));
        const u32 sy1 = std::min(height, u32(std::max(0.f, std::ceil(scissor.bottom()))));
        out.push.origin[0] = sx0;
        out.push.origin[1] = sy0;
        out.push.size[0] = sx1 > sx0 ? sx1 - sx0 : 0;
        out.push.size[1] = sy1 > sy0 ? sy1 - sy0 : 0;
    }
    Clip stack[32];
    int depth = 0;
    stack[0] = {scissor, 0.f};

    // commands + their pixel areas; a Backdrop closes the current draw segment (its tile table is built from the
    // segment's commands) and becomes a blur pass over what was drawn so far
    struct Area {
        int x0, y0, x1, y1;
        u32 id; ///< tile entry: a cmds index, or kCompactGlyph | word offset of a compact glyph record
    };
    thread_local std::vector<Area> areas;
    thread_local std::vector<u32> cursor;
    areas.clear();
    areas.reserve(list.cmds().size()); // one growth step per frame at most (text pages are 10^4+ glyph quads)
    out.cmds.reserve(list.cmds().size() + 1);
    u32 segBegin = 0;
    u32 clipRef = ~0u; // carrier cmd of the current glyph clip
    Clip lastClip{};
    auto closeSegment = [&] {
        const u32 tileBase = u32(out.tiles.size());
        out.tiles.resize(std::size_t(tileBase) + tileCount + 1, 0u);
        u32* off = out.tiles.data() + tileBase;
        for (u32 i = segBegin; i < areas.size(); ++i) {
            const Area& a = areas[i];
            if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
            const int tx0 = a.x0 / int(kUiTile), tx1 = (a.x1 - 1) / int(kUiTile);
            const int ty0 = a.y0 / int(kUiTile), ty1 = (a.y1 - 1) / int(kUiTile);
            if (tx0 == tx1 && ty0 == ty1) { // glyphs and small widgets: one tile
                ++off[std::size_t(ty0) * tilesX + tx0];
                continue;
            }
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) ++off[std::size_t(ty) * tilesX + tx];
        }
        u32 sum = 0; // prefix sums -> offsets; then ids in draw order
        for (u32 t = 0; t < tileCount; ++t) {
            const u32 n = off[t];
            off[t] = sum;
            sum += n;
        }
        off[tileCount] = sum;
        cursor.assign(off, off + tileCount);
        const std::size_t base = std::size_t(tileBase) + tileCount + 1;
        out.tiles.resize(base + sum, 0u);
        for (u32 i = segBegin; i < areas.size(); ++i) {
            const Area& a = areas[i];
            if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
            const int tx0 = a.x0 / int(kUiTile), tx1 = (a.x1 - 1) / int(kUiTile);
            const int ty0 = a.y0 / int(kUiTile), ty1 = (a.y1 - 1) / int(kUiTile);
            u32* ids = out.tiles.data() + base;
            if (tx0 == tx1 && ty0 == ty1) {
                ids[cursor[std::size_t(ty0) * tilesX + tx0]++] = a.id;
                continue;
            }
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) ids[cursor[std::size_t(ty) * tilesX + tx]++] = a.id;
        }
        GpuPass p;
        p.kind = kPassDraw;
        p.tileBase = tileBase;
        out.passes.push_back(p);
        segBegin = u32(areas.size());
    };
    auto pack = [&](const DrawCmd& c, const Clip& clip) {
        GpuCmd& g = out.cmds.emplace_back(); // built in place (every field is written below)
        put4(g.rect, c.rect.x, c.rect.y, c.rect.w, c.rect.h);
        put4(g.fill, c.fill.r, c.fill.g, c.fill.b, c.fill.a);
        put4(g.fill2, c.fill2.r, c.fill2.g, c.fill2.b, c.fill2.a);
        put4(g.border, c.border.r, c.border.g, c.border.b, c.border.a);
        put4(g.radius, c.radius[0], c.radius[1], c.radius[2], c.radius[3]);
        put4(g.clip, clip.r.x, clip.r.y, clip.r.w, clip.r.h);
        put4(g.params, c.borderWidth, c.blur, clip.radius, 0.f);
        g.info[0] = u32(c.op) | (u32(c.gradient) << 8);
        g.info[1] = u32(c.u) | (u32(c.v) << 16);
        g.info[2] = u32(c.tw) | (u32(c.th) << 16);
        g.info[3] = c.image;
    };
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
        if (c.op == DrawOp::Backdrop) {
            // same box as CpuRasterizer's backdropBlur: area = rect & clip & target, source = area + 3r margin
            const Rect area = c.rect.intersect(clip.r).intersect(full);
            if (area.empty()) continue;
            const int r = std::max(1, int(std::lround(c.blur / 3.f)));
            const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
            const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
            const int m = 3 * r;
            const int sx0 = std::max(0, x0 - m), sy0 = std::max(0, y0 - m);
            const int sx1 = std::min(int(width), x1 + m), sy1 = std::min(int(height), y1 + m);
            closeSegment();
            const u32 id = u32(out.cmds.size());
            pack(c, clip);
            areas.push_back({0, 0, 0, 0, id}); // never binned
            segBegin = u32(areas.size());
            GpuPass p;
            p.kind = kPassBackdrop;
            p.cmd = id;
            p.src[0] = sx0;
            p.src[1] = sy0;
            p.src[2] = sx1 - sx0;
            p.src[3] = sy1 - sy0;
            p.area[0] = x0;
            p.area[1] = y0;
            p.area[2] = x1;
            p.area[3] = y1;
            p.radius = u32(r);
            p.inv = 1.f / f32(2 * r + 1);
            out.passes.push_back(p);
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
        if (c.op == DrawOp::Image && c.image >= imageCount) continue;
        const Area pixels{std::max(0, int(std::floor(area.x))), std::max(0, int(std::floor(area.y))),
                          std::min(int(width), int(std::ceil(area.right()))), std::min(int(height), int(std::ceil(area.bottom()))), 0u};
        if (c.op == DrawOp::Glyph) {
            // compact glyph (44 bytes in the images buffer instead of a 128-byte GpuCmd): rect, uv, size, fill and a
            // reference to a clip carrier cmd (one per distinct clip, never binned)
            if (clipRef == ~0u || !(clip.r == lastClip.r) || clip.radius != lastClip.radius) {
                clipRef = u32(out.cmds.size());
                pack(c, clip);
                out.cmds.back().info[0] = kClipCarrier;
                lastClip = clip;
            }
            const u32 at = u32(out.images.size());
            out.images.resize(out.images.size() + kCompactGlyphWords);
            u32* w = out.images.data() + at;
            const f32 f[4] = {c.rect.x, c.rect.y, c.rect.w, c.rect.h};
            std::memcpy(w, f, sizeof f);
            w[4] = u32(c.u) | (u32(c.v) << 16);
            w[5] = u32(c.tw) | (u32(c.th) << 16);
            const f32 col[4] = {c.fill.r, c.fill.g, c.fill.b, c.fill.a};
            std::memcpy(w + 6, col, sizeof col);
            w[10] = clipRef;
            Area g = pixels;
            g.id = kCompactGlyph | at;
            areas.push_back(g);
            continue;
        }
        if (c.op == DrawOp::Path) {
            // edges ride in the images buffer as float words (no extra binding): info.w = word offset
            const std::size_t n = std::size_t(c.tw) | (std::size_t(c.th) << 16);
            if (n == 0 || std::size_t(c.image) + n > list.pathEdges().size()) continue;
            const u32 at = u32(out.images.size());
            const PathEdge* e = list.pathEdges().data() + c.image;
            out.images.resize(out.images.size() + n * 5);
            std::memcpy(out.images.data() + at, e, n * sizeof(PathEdge));
            // tile-row edge lists: edges whose y-range (grown by the stroke reach) touches the 16-px row. Dropped
            // edges cannot change the winding sub-samples or a non-zero stroke coverage there, so results are identical
            const f32 reach = c.borderWidth * 0.5f + 1.f;
            const u32 r0 = u32(pixels.y0) / kUiTile, r1 = u32(std::max(pixels.y1 - 1, pixels.y0)) / kUiTile;
            const u32 rows = r1 - r0 + 1;
            const std::size_t tb = out.images.size();
            out.images.resize(tb + 2 + rows + 1);
            out.images[tb] = r0;
            out.images[tb + 1] = rows;
            const std::size_t lb = out.images.size();
            for (u32 r = 0; r < rows; ++r) {
                out.images[tb + 2 + r] = u32(out.images.size() - lb);
                const f32 top = f32((r0 + r) * kUiTile) - reach, bottom = f32((r0 + r + 1) * kUiTile) + reach;
                for (std::size_t k = 0; k < n; ++k) {
                    const f32 lo = std::min(e[k].y0, e[k].y1), hi = std::max(e[k].y0, e[k].y1);
                    if (hi >= top && lo <= bottom) out.images.push_back(u32(k));
                }
            }
            out.images[tb + 2 + rows] = u32(out.images.size() - lb);
            pack(c, clip);
            out.cmds.back().info[3] = at;
        } else {
            pack(c, clip);
        }
        Area pa = pixels;
        pa.id = u32(out.cmds.size() - 1);
        areas.push_back(pa);
    }
    closeSegment();
    // drop a trailing empty draw segment after a backdrop (nothing to draw); passes[0] always stays
    if (out.passes.size() > 1 && out.passes.back().kind == kPassDraw &&
        out.tiles[out.passes.back().tileBase + tileCount] == 0) {
        out.tiles.resize(out.passes.back().tileBase);
        out.passes.pop_back();
    }
    out.tiles.push_back(0u); // never empty
    if (out.cmds.empty()) out.cmds.push_back(GpuCmd{}); // non-empty buffer; no tile references it
}

} // namespace fuse::ui::render
