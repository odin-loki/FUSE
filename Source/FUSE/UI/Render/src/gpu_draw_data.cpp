#include "fuse/ui/render/gpu_draw_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::ui::render {

namespace {
struct Clip {
    Rect r;       ///< device pixels
    f32 radius;
    f32 dx = 0.f, dy = 0.f; ///< where the commands under it are moved to (translation groups)
};
/// A transform group the CPU rasteriser draws in place, moved by whole pixels (drawFxGroup): a pure translation under
/// a rect clip. Packed the same way: its commands are emitted moved; nothing is baked for the group itself.
bool movedInPlace(const DrawList& list, const DrawCmd& c, f32 clipRadius, f32& dx, f32& dy) {
    if (c.op != DrawOp::PushTransform || c.data >= list.transforms().size() || clipRadius > 0.f) return false;
    const DrawTransform& t = list.transforms()[c.data];
    if (!(t.xf.a == 1.f && t.xf.b == 0.f && t.xf.c == 0.f && t.xf.d == 1.f) || t.src.empty()) return false;
    if (!std::isfinite(t.xf.tx) || !std::isfinite(t.xf.ty)) return false;
    dx = f32(std::lround(t.xf.tx));
    dy = f32(std::lround(t.xf.ty));
    return true;
}
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
    out.atlas = atlas;
    out.atlasSize = atlas ? atlasSize : 0;
    const u32 tilesX = (width + kUiTile - 1) / kUiTile, tilesY = (height + kUiTile - 1) / kUiTile;
    const u32 tileCount = tilesX * tilesY;
    out.bakedOps = out.bakedPixels = out.unsupportedOps = out.skippedGlyphs = 0;
    // vector ops are drawn as pre-rasterised images: one extra image table entry per top-level vector op / group
    u32 bakeSlots = 0;
    if (list.hasVectorOps()) { // the same walk as pass 1 below, counting only
        const auto& cmds = list.cmds();
        f32 radius[32];
        int d = 0;
        radius[0] = 0.f;
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const DrawOp op = cmds[i].op;
            f32 dx, dy;
            if (op == DrawOp::PushClip) {
                if (d < 31) {
                    ++d;
                    radius[d] = radius[d - 1] > 0.f && cmds[i].radius[0] <= 0.f ? 0.f : cmds[i].radius[0];
                }
            } else if (op == DrawOp::PopClip) {
                if (d > 0) --d;
            } else if (d < 31 && movedInPlace(list, cmds[i], radius[d], dx, dy)) {
                ++d;
                radius[d] = 0.f;
            } else if (op == DrawOp::Path || op == DrawOp::NinePatch) ++bakeSlots;
            else if (isLayerGroup(op)) {
                ++bakeSlots;
                i = matchingPop(list, i);
            }
        }
    }
    const u32 tableCount = imageCount + bakeSlots;
    out.push = {{width, height}, tilesX, out.atlasSize, tileCount, tableCount, {0, 0}};

    // images: table then pixels
    out.images.assign(std::size_t(tableCount) * 4, 0u);
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

    // pass 1: commands + their pixel areas (counted per tile)
    out.tiles.assign(std::size_t(tileCount) + 1, 0u);
    struct Area {
        int x0, y0, x1, y1;
    };
    thread_local std::vector<Area> areas;
    areas.clear();
    const RasterSources sources{atlas, int(atlas ? atlasSize : 0), images, images ? imageCount : 0u};
    u32 bakeIndex = 0;
    const auto& listCmds = list.cmds();
    for (std::size_t ci = 0; ci < listCmds.size(); ++ci) {
        const DrawCmd& c = listCmds[ci];
        if (f32 mx, my; depth < 31 && movedInPlace(list, c, stack[depth].radius, mx, my)) {
            ++depth; // ends at its PopClip like a clip; the clip itself stays
            stack[depth] = {stack[depth - 1].r, 0.f, stack[depth - 1].dx + mx, stack[depth - 1].dy + my};
            continue;
        }
        if (c.op == DrawOp::Path || c.op == DrawOp::NinePatch || isLayerGroup(c.op)) {
            // a vector op, or a whole group: CPU pre-rasterised, drawn by the kernel as an image
            const bool group = isLayerGroup(c.op); // effect groups (transform / filter / mask) bake like the others
            const std::size_t end = group ? matchingPop(list, ci) : ci;
            const std::size_t first = ci, count = std::min(end, listCmds.size() - 1) - ci + 1;
            const u32 slot = imageCount + bakeIndex++;
            const Clip& clip = stack[depth];
            const f32 opacity = c.op == DrawOp::PushLayer ? c.fill.a : 1.f;
            const Rect local{clip.r.x - clip.dx, clip.r.y - clip.dy, clip.r.w, clip.r.h}; // the clip in the commands' space
            const Rect bounds = (c.op == DrawOp::PushLayer && c.rect.empty()) ? local : list.groupBounds(c).intersect(local);
            ci = end;
            if (bounds.empty() || !(opacity > 0.f)) continue;
            const Area a{int(std::floor(bounds.x)), int(std::floor(bounds.y)), int(std::ceil(bounds.right())), int(std::ceil(bounds.bottom()))};
            const u32 pixels = u32(a.x1 - a.x0) * u32(a.y1 - a.y0);
            if (out.bakedPixels + pixels > kMaxBakedPixels) { // over budget: dropped, and reported
                ++out.unsupportedOps;
                continue;
            }
            const Rect area{f32(a.x0), f32(a.y0), f32(a.x1 - a.x0), f32(a.y1 - a.y0)};
            rasterizeCommands(list, first, count, area, int(width), sources, out.bakeScratch, true);
            ++out.bakedOps;
            out.bakedPixels += pixels;
            out.images[slot * 4 + 0] = u32(out.images.size());
            out.images[slot * 4 + 1] = u32(a.x1 - a.x0);
            out.images[slot * 4 + 2] = u32(a.y1 - a.y0);
            out.images[slot * 4 + 3] = 0u;
            out.images.insert(out.images.end(), out.bakeScratch.pixels.begin(), out.bakeScratch.pixels.end());
            GpuCmd g{};
            put4(g.rect, area.x + clip.dx, area.y + clip.dy, area.w, area.h);
            put4(g.fill, 1.f, 1.f, 1.f, opacity);
            put4(g.clip, clip.r.x, clip.r.y, clip.r.w, clip.r.h);
            put4(g.params, 0.f, 0.f, clip.radius, 0.f);
            g.info[0] = u32(DrawOp::Image);
            g.info[3] = slot;
            out.cmds.push_back(g);
            const Area moved{a.x0 + int(clip.dx), a.y0 + int(clip.dy), a.x1 + int(clip.dx), a.y1 + int(clip.dy)};
            areas.push_back(moved);
            for (int ty = moved.y0 / int(kUiTile); ty <= (moved.y1 - 1) / int(kUiTile); ++ty)
                for (int tx = moved.x0 / int(kUiTile); tx <= (moved.x1 - 1) / int(kUiTile); ++tx) ++out.tiles[std::size_t(ty) * tilesX + tx];
            continue;
        }
        if (c.op == DrawOp::PushClip) {
            if (depth < 31) {
                const Clip& up = stack[depth];
                ++depth;
                stack[depth] = {up.r.intersect({c.rect.x + up.dx, c.rect.y + up.dy, c.rect.w, c.rect.h}), c.radius[0], up.dx, up.dy};
                if (up.radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            continue;
        }
        if (c.op == DrawOp::PopClip) {
            if (depth > 0) --depth;
            continue;
        }
        if (c.op == DrawOp::Backdrop) continue; // single-pass tile kernel: no blur pass yet (panel drawn unblurred)
        const Clip& clip = stack[depth];
        Rect rect = c.rect; // moved with its translation group (a Line holds two points)
        rect.x += clip.dx;
        rect.y += clip.dy;
        if (c.op == DrawOp::Line) { rect.w += clip.dx; rect.h += clip.dy; }
        Rect bounds = rect;
        if (c.op == DrawOp::Shadow)
            bounds = {rect.x - c.blur * 2.f, rect.y - c.blur * 2.f, rect.w + c.blur * 4.f, rect.h + c.blur * 4.f};
        if (c.op == DrawOp::Line) {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            bounds = {std::min(rect.x, rect.w) - hw, std::min(rect.y, rect.h) - hw,
                      std::abs(rect.w - rect.x) + 2 * hw, std::abs(rect.h - rect.y) + 2 * hw};
        }
        const Rect area = bounds.intersect(clip.r);
        if (area.empty()) continue;
        if (c.op == DrawOp::Glyph && !atlas) continue;
        // Colour-image and MSDF glyphs live in the atlas' RGBA page, which this backend does not upload: not drawn on
        // the GPU path (draw_list.hpp, kGlyphColor / kGlyphMsdf). The CPU rasteriser is the reference for them.
        // (Inside a baked group they are part of the group's image; only loose ones get here. Counted, never silent.)
        if (c.op == DrawOp::Glyph && c.gradient != kGlyphCoverage) {
            ++out.skippedGlyphs;
            continue;
        }
        if (c.op == DrawOp::Image && c.image >= imageCount) continue;
        GpuCmd g{};
        put4(g.rect, rect.x, rect.y, rect.w, rect.h);
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
        out.cmds.push_back(g);
        const Area a{std::max(0, int(std::floor(area.x))), std::max(0, int(std::floor(area.y))),
                     std::min(int(width), int(std::ceil(area.right()))), std::min(int(height), int(std::ceil(area.bottom())))};
        areas.push_back(a);
        if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
        for (int ty = a.y0 / int(kUiTile); ty <= (a.y1 - 1) / int(kUiTile); ++ty)
            for (int tx = a.x0 / int(kUiTile); tx <= (a.x1 - 1) / int(kUiTile); ++tx) ++out.tiles[std::size_t(ty) * tilesX + tx];
    }
    // prefix sums -> offsets; then fill ids in draw order
    u32 sum = 0;
    for (u32 t = 0; t <= tileCount; ++t) {
        const u32 n = out.tiles[t];
        out.tiles[t] = sum;
        sum += t < tileCount ? n : 0;
    }
    out.tiles[tileCount] = sum;
    const std::size_t base = std::size_t(tileCount) + 1;
    out.tiles.resize(base + sum + 1, 0u); // +1: never empty
    thread_local std::vector<u32> cursor;
    cursor.assign(out.tiles.begin(), out.tiles.begin() + tileCount);
    for (u32 i = 0; i < areas.size(); ++i) {
        const Area& a = areas[i];
        if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
        for (int ty = a.y0 / int(kUiTile); ty <= (a.y1 - 1) / int(kUiTile); ++ty)
            for (int tx = a.x0 / int(kUiTile); tx <= (a.x1 - 1) / int(kUiTile); ++tx)
                out.tiles[base + cursor[std::size_t(ty) * tilesX + tx]++] = i;
    }
    if (out.cmds.empty()) out.cmds.push_back(GpuCmd{}); // non-empty buffer; no tile references it
}

} // namespace fuse::ui::render
