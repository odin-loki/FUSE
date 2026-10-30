#include "fuse/ui/render/gpu_draw_data.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace fuse::ui::render {

// the colour-only patch compares DrawCmd field by field: a new field must be added there
static_assert(sizeof(DrawCmd) == 148, "DrawCmd changed: update the field compare in packDrawList");

namespace {
struct Clip {
    Rect r;
    f32 radius;
    u32 pathList = 0; ///< images word offset of the active clip-path list (count, then at / n / evenOdd each); 0 = none
};
void put4(f32 (&d)[4], f32 a, f32 b, f32 c, f32 e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}
} // namespace

void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas, u32 atlasSize,
                  const Image* const* images, u32 imageCount, Rect scissor, u32 atlasGeneration) {
    // persistent pack: identical inputs keep the previous buffers (contentVersion unchanged -> no upload)
    {
        u32 key[9] = {width, height, atlasSize, imageCount, atlasGeneration};
        const f32 sc[4] = {scissor.x, scissor.y, scissor.w, scissor.h};
        std::memcpy(&key[5], sc, sizeof sc);
        const auto sameVec = [](auto& prev, const auto& cur) { // compare; on a difference refresh prev from there on
            using T = typename std::decay_t<decltype(cur)>::value_type;
            const std::size_t n = std::min(prev.size(), cur.size());
            std::size_t i = 0;
            constexpr std::size_t B = 256;
            while (i < n && std::memcmp(prev.data() + i, cur.data() + i, std::min(B, n - i) * sizeof(T)) == 0) i += B;
            i = std::min(i, n);
            if (i == n && prev.size() == cur.size()) return true;
            prev.resize(cur.size());
            std::memcpy(prev.data() + i, cur.data() + i, (cur.size() - i) * sizeof(T));
            return false;
        };
        bool same = atlasGeneration != ~0u && out.atlasGeneration == atlasGeneration && out.prevAtlas == atlas &&
                    std::memcmp(key, out.prevKey, sizeof key) == 0 && out.contentVersion != 0;
        // commands: block compare; a differing command whose only change is its fill colour becomes a patch (the
        // packed geometry, tiles and runs do not depend on it), anything else forces a full repack
        out.patches.clear();
        bool structural = out.prevCmds.size() != list.cmds().size();
        {
            const DrawCmd* cur = list.cmds().data();
            const std::size_t n = list.cmds().size();
            if (structural) {
                out.prevCmds.assign(cur, cur + n);
            } else {
                constexpr std::size_t B = 64;
                for (std::size_t i = 0; i < n; i += B) {
                    const std::size_t k = std::min(B, n - i);
                    if (std::memcmp(out.prevCmds.data() + i, cur + i, k * sizeof(DrawCmd)) == 0) continue;
                    for (std::size_t j = i; j < i + k && !structural; ++j) {
                        // field by field: copies need not keep padding bytes, so a byte compare can differ on equal commands
                        const DrawCmd& p = out.prevCmds[j];
                        const DrawCmd& q = cur[j];
                        const auto bits = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof x) == 0; };
                        const bool rest = p.op == q.op && p.gradient == q.gradient && p.u == q.u && p.v == q.v && p.tw == q.tw &&
                                          p.th == q.th && p.image == q.image && p.node == q.node && bits(p.rect.x, q.rect.x) &&
                                          bits(p.rect.y, q.rect.y) && bits(p.rect.w, q.rect.w) && bits(p.rect.h, q.rect.h) &&
                                          bits(p.fill2, q.fill2) && bits(p.border, q.border) && bits(p.borderWidth, q.borderWidth) &&
                                          bits(p.radius, q.radius) && bits(p.blur, q.blur) && bits(p.gradXf, q.gradXf) &&
                                          p.stops == q.stops && p.stopCount == q.stopCount && p.focal == q.focal;
                        if (rest && bits(p.fill, q.fill)) continue;
                        if (rest) out.patches.push_back(u32(j));
                        else structural = true;
                    }
                    std::memcpy(out.prevCmds.data() + i, cur + i, k * sizeof(DrawCmd));
                }
            }
        }
        // always run both (each refreshes its copy)
        same = sameVec(out.prevEdges, list.pathEdges()) && same;
        same = sameVec(out.prevStops, list.gradientStops()) && same;
        same = same && !structural;
        if (same) // image pixels: compare with the copies already in the packed buffer
            for (u32 i = 0; i < imageCount && same; ++i) {
                const Image* img = images ? images[i] : nullptr;
                const u32* t = out.images.data() + i * 4;
                const bool ext = img && img->externalSlot >= 0 && img->externalSlot < 4;
                const bool hdr = !ext && img && img->hasHdr();
                if (t[1] != (img ? u32(img->width) : 1u) || t[2] != (img ? u32(img->height) : 1u) ||
                    t[3] != (ext ? u32(img->externalSlot) + 1u : hdr ? kImageHalf : 0u))
                    same = false;
                else if (hdr)
                    same = std::size_t(t[0]) + img->hdrPixels.size() * 2 <= out.images.size() &&
                           std::memcmp(out.images.data() + t[0], img->hdrPixels.data(), img->hdrPixels.size() * 8) == 0;
                else if (!ext && img && !img->pixels.empty())
                    same = std::size_t(t[0]) + img->pixels.size() <= out.images.size() &&
                           std::memcmp(out.images.data() + t[0], img->pixels.data(), img->pixels.size() * 4) == 0;
            }
        std::memcpy(out.prevKey, key, sizeof key);
        out.prevAtlas = atlas;
        out.reused = same && out.patches.empty();
        out.patched = false;
        if (out.reused) return;
        // colour-only frame: patch the packed fills in place. Glyph records get their fill words appended at the end
        // of `images` (deduplicated against earlier patches), so no other word moves and the upload page diff writes
        // only the touched records / commands and the tail.
        if (same && out.cmdSlot.size() == list.cmds().size() &&
            out.images.size() + out.patches.size() * 4 <= out.packedImageWords + 4096) {
            for (u32 idx : out.patches) {
                const u32 slot = out.cmdSlot[idx];
                if (slot == ~0u) continue; // culled
                const Color f = list.cmds()[idx].fill;
                if (slot & kCompactGlyph) {
                    u32 at = ~0u;
                    const f32 col[4] = {f.r, f.g, f.b, f.a};
                    for (u32 q = out.packedImageWords; q + 4 <= out.images.size(); q += 4)
                        if (std::memcmp(out.images.data() + q, col, sizeof col) == 0) { at = q; break; }
                    if (at == ~0u) {
                        at = u32(out.images.size());
                        out.images.resize(out.images.size() + 4);
                        std::memcpy(out.images.data() + at, col, sizeof col);
                    }
                    out.images[(slot & ~kCompactGlyph) + 3] = at;
                } else {
                    put4(out.cmds[slot].fill, f.r, f.g, f.b, f.a);
                }
            }
            out.patched = true;
            static std::atomic<u64> pv{1ull << 62}; // own range, still process-unique
            out.contentVersion = ++pv;
            return;
        }
        static std::atomic<u64> versions{0}; // process-unique, so a slot never mistakes another GpuDrawData's content
        out.contentVersion = ++versions;
    }
    // glyph slot arena: compacted when the holes left by moved / vanished nodes outweigh the live slots
    if ((out.arenaHoles > out.arenaLive && out.arena.size() > 65536) || out.fillSlots.size() > 8192) out.resetArena();
    for (int attempt = 0; attempt < 2; ++attempt) {
    ++out.frameStamp;
    out.cmdSlot.assign(list.cmds().size(), ~0u);
    out.glyphScratch.clear();
    out.glyphSlotOf.clear();
    out.cmds.clear();
    out.tiles.clear();
    out.images.clear();
    out.passes.clear();
    out.atlasGeneration = atlasGeneration;
    out.atlas = atlas;
    out.atlasSize = atlas ? atlasSize : 0;
    const u32 tilesX = (width + kUiTile - 1) / kUiTile, tilesY = (height + kUiTile - 1) / kUiTile;
    const u32 tileCount = tilesX * tilesY;
    out.push = {{width, height}, tilesX, out.atlasSize, tileCount, imageCount, {0, 0}, {width, height}, 0, 0};
    // glyph ink boxes: the tight rect of non-zero atlas coverage inside each glyph bitmap (x0 | y0<<8 | x1<<16 | y1<<24,
    // relative to the bitmap; ~0u = blank). Glyph records shrink to it, so tiles their padding / blank margin merely
    // touches never list them (per-tile culling) and run column ranges narrow; outside the ink a == 0, so bit-exact.
    if (out.inkKeys.size() < 1024) out.inkKeys.assign(1024, ~0ull), out.inkBoxes.assign(1024, 0u);
    else std::fill(out.inkKeys.begin(), out.inkKeys.end(), ~0ull);
    std::size_t inkCount = 0;
    const auto inkBox = [&](u32 u, u32 v, u32 tw, u32 th) -> u32 {
        const u64 key = u64(u) | (u64(v) << 16) | (u64(tw) << 32) | (u64(th) << 48);
        for (;;) {
            const std::size_t mask = out.inkKeys.size() - 1;
            std::size_t i = std::size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
            while (out.inkKeys[i] != ~0ull && out.inkKeys[i] != key) i = (i + 1) & mask;
            if (out.inkKeys[i] == key) return out.inkBoxes[i];
            if ((inkCount + 1) * 2 > out.inkKeys.size()) { // grow (first frames only), rehash
                std::vector<u64> keys(out.inkKeys.size() * 2, ~0ull);
                std::vector<u32> boxes(keys.size(), 0u);
                for (std::size_t j = 0; j < out.inkKeys.size(); ++j) {
                    if (out.inkKeys[j] == ~0ull) continue;
                    std::size_t q = std::size_t((out.inkKeys[j] * 0x9E3779B97F4A7C15ull) >> 40) & (keys.size() - 1);
                    while (keys[q] != ~0ull) q = (q + 1) & (keys.size() - 1);
                    keys[q] = out.inkKeys[j];
                    boxes[q] = out.inkBoxes[j];
                }
                out.inkKeys.swap(keys);
                out.inkBoxes.swap(boxes);
                continue;
            }
            u32 x0 = tw, y0 = th, x1 = 0, y1 = 0;
            const u32 S = out.atlasSize;
            for (u32 y = 0; y < th && v + y < S; ++y) {
                const u8* row = atlas + std::size_t(v + y) * S + u;
                for (u32 x = 0; x < tw && u + x < S; ++x)
                    if (row[x]) {
                        x0 = std::min(x0, x), x1 = std::max(x1, x + 1);
                        y0 = std::min(y0, y), y1 = std::max(y1, y + 1);
                    }
            }
            // ~0u: blank; 0xFFFFFFFE: too large for 8-bit fields (kept untrimmed); else the packed box
            const u32 box = x1 <= x0 ? ~0u : (tw > 255 || th > 255) ? 0xFFFFFFFEu : x0 | (y0 << 8) | (x1 << 16) | (y1 << 24);
            out.inkKeys[i] = key;
            out.inkBoxes[i] = box;
            ++inkCount;
            return box;
        }
    };

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
        if (img && img->hasHdr()) { // 2 words per pixel: r | g << 16, b | a << 16 (half floats)
            out.images[i * 4 + 3] = kImageHalf;
            const std::size_t at = out.images.size();
            out.images.resize(at + img->hdrPixels.size() * 2);
            std::memcpy(out.images.data() + at, img->hdrPixels.data(), img->hdrPixels.size() * 8);
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
    stack[0] = {scissor, 0.f, 0u};

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
    struct Segment {
        u32 end;   ///< areas index where the draw segment closes
        GpuPass blur; ///< the backdrop pass that follows it
    };
    thread_local std::vector<Segment> segments;
    segments.clear();
    u32 clipRef = ~0u; // carrier cmd of the current glyph clip
    Clip lastClip{};
    Color lastFill{};
    u32 fillAt = ~0u;
    // a compact glyph record (in the slot arena, addressed by its absolute images word offset)
    const auto rec = [&](u32 abs) -> const u32* { return out.arena.data() + (abs - out.arenaBase); };
    auto closeSegment = [&](u32 segEnd) {
        const u32 tileBase = u32(out.tiles.size());
        out.tiles.resize(std::size_t(tileBase) + tileCount + 1, 0u);
        u32* off = out.tiles.data() + tileBase;
        for (u32 i = segBegin; i < segEnd; ++i) {
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
        for (u32 i = segBegin; i < segEnd; ++i) {
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
        // glyph runs: consecutive compact glyphs of a tile with non-decreasing x0 become one entry, so the kernel
        // binary-searches the few glyphs that can cover a pixel instead of testing every glyph in the tile
        {
            off = out.tiles.data() + tileBase; // the resize above moved the buffer
            u32* ids = out.tiles.data() + base;
            u32 w = 0;
            for (u32 t = 0; t < tileCount; ++t) {
                const u32 b = off[t], e = off[t + 1];
                off[t] = w;
                for (u32 k = b; k < e;) {
                    u32 j = k;
                    auto x0 = [&](u32 id) { return rec(id & ~kCompactGlyph)[4] & 0xFFFFu; };
                    if ((ids[k] & kGlyphRun) == kCompactGlyph)
                        while (j + 1 < e && (ids[j + 1] & kGlyphRun) == kCompactGlyph && x0(ids[j + 1]) >= x0(ids[j])) ++j;
                    if (j - k + 1 < kMinGlyphRun) {
                        for (; k <= j; ++k) ids[w++] = ids[k];
                        continue;
                    }
                    const u32 n = j - k + 1, at = u32(out.images.size());
                    out.images.resize(out.images.size() + 2 + kUiTile + std::size_t(n));
                    u32* r = out.images.data() + at;
                    u32 y0 = 0xFFFFu, y1 = 0, maxW = 0;
                    for (u32 q = 0; q < n; ++q) {
                        const u32 g = ids[k + q] & ~kCompactGlyph;
                        const u32 b0 = rec(g)[4], b1 = rec(g)[5];
                        y0 = std::min(y0, b0 >> 16);
                        y1 = std::max(y1, b1 >> 16);
                        maxW = std::max(maxW, (b1 & 0xFFFFu) - (b0 & 0xFFFFu));
                        r[2 + kUiTile + q] = g;
                    }
                    r[0] = n;
                    r[1] = y0 | (y1 << 16);
                    // per tile column: [first, end) of the glyphs whose x0 lies in (x - maxW, x] (keys are sorted)
                    const u32 cx0 = (t % tilesX) * kUiTile + 32768u;
                    u32 lo = 0, hi = 0;
                    for (u32 col = 0; col < kUiTile; ++col) {
                        const u32 x = cx0 + col;
                        while (hi < n && x0(ids[k + hi]) <= x) ++hi;
                        while (lo < hi && x0(ids[k + lo]) + maxW <= x) ++lo;
                        r[2 + col] = lo | (hi << 16);
                    }
                    ids[w++] = kGlyphRun | at;
                    k = j + 1;
                }
            }
            off[tileCount] = w;
            out.tiles.resize(base + w);
        }
        GpuPass p;
        p.kind = kPassDraw;
        p.tileBase = tileBase;
        out.passes.push_back(p);
        segBegin = segEnd;
    };
    auto pack = [&](const DrawCmd& c, const Clip& clip) {
        GpuCmd& g = out.cmds.emplace_back(); // built in place (every field is written below)
        put4(g.rect, c.rect.x, c.rect.y, c.rect.w, c.rect.h);
        put4(g.fill, c.fill.r, c.fill.g, c.fill.b, c.fill.a);
        put4(g.fill2, c.fill2.r, c.fill2.g, c.fill2.b, c.fill2.a);
        put4(g.border, c.border.r, c.border.g, c.border.b, c.border.a);
        put4(g.radius, c.radius[0], c.radius[1], c.radius[2], c.radius[3]);
        put4(g.clip, clip.r.x, clip.r.y, clip.r.w, clip.r.h);
        {
            f32 pl;
            std::memcpy(&pl, &clip.pathList, 4);
            put4(g.params, c.borderWidth, c.blur, clip.radius, pl);
        }
        g.info[0] = u32(c.op) | (u32(c.gradient) << 8);
        g.info[1] = u32(c.u) | (u32(c.v) << 16);
        g.info[2] = u32(c.tw) | (u32(c.th) << 16);
        g.info[3] = c.image;
        put4(g.grad, 0.f, 0.f, 0.f, 0.f);
        put4(g.grad2, 0.f, 0.f, 0.f, 0.f);
        if ((c.gradient & 15u) >= kGradientLinearStops) {
            const auto& stops = list.gradientStops();
            const u32 n = std::size_t(c.stops) + c.stopCount <= stops.size() ? c.stopCount : 0u;
            if ((c.gradient & 15u) == kGradientRadialFocal) // the focal point rides just before the stops
                for (f32 f : c.focal) { u32 u; std::memcpy(&u, &f, 4); out.images.push_back(u); }
            const u32 at = u32(out.images.size());
            for (u32 i = 0; i < n; ++i) { // 5 float words per stop, after the image / edge words
                const GradientStop& st = stops[c.stops + i];
                const f32 w[5] = {st.offset, st.color.r, st.color.g, st.color.b, st.color.a};
                for (f32 f : w) { u32 u; std::memcpy(&u, &f, 4); out.images.push_back(u); }
            }
            put4(g.grad, c.gradXf[0], c.gradXf[1], c.gradXf[2], c.gradXf[3]);
            u32 bits[2] = {at, n};
            f32 fb[2];
            std::memcpy(fb, bits, 8);
            put4(g.grad2, c.gradXf[4], c.gradXf[5], fb[0], fb[1]);
        }
    };
    for (const DrawCmd& c : list.cmds()) {
        const std::size_t ci = std::size_t(&c - list.cmds().data());
        if (c.op == DrawOp::PushClip) {
            if (depth < 31) {
                ++depth;
                stack[depth] = {stack[depth - 1].r.intersect(c.rect), c.radius[0], stack[depth - 1].pathList};
                if (stack[depth - 1].radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            continue;
        }
        if (c.op == DrawOp::ClipPath) {
            // same clip state as CpuRasterizer: bounds (+1 px) intersect the rect clip; the edges and the list of
            // every active clip path ride in the images buffer
            if (depth < 31) {
                ++depth;
                stack[depth] = stack[depth - 1];
                const std::size_t n = std::size_t(c.tw) | (std::size_t(c.th) << 16);
                if (n == 0 || std::size_t(c.image) + n > list.pathEdges().size()) {
                    stack[depth].r = {};
                    continue;
                }
                stack[depth].r = stack[depth].r.intersect({c.rect.x - 1.f, c.rect.y - 1.f, c.rect.w + 2.f, c.rect.h + 2.f});
                if (out.images.empty()) out.images.push_back(0u); // list offsets are never 0
                const u32 at = u32(out.images.size());
                out.images.resize(out.images.size() + n * 5);
                std::memcpy(out.images.data() + at, list.pathEdges().data() + c.image, n * sizeof(PathEdge));
                const u32 parent = stack[depth - 1].pathList;
                const u32 pc = parent ? out.images[parent] : 0u;
                const u32 lo = u32(out.images.size());
                out.images.push_back(pc + 1);
                for (u32 q = 0; q < pc * 3; ++q) out.images.push_back(out.images[parent + 1 + q]);
                out.images.push_back(at);
                out.images.push_back(u32(n));
                out.images.push_back(c.v ? 1u : 0u);
                stack[depth].pathList = lo;
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
            const u32 segEnd = u32(areas.size()); // the draw segment closes here (binned after the slot pass)
            const u32 id = u32(out.cmds.size());
            pack(c, clip);
            out.cmdSlot[ci] = id;
            areas.push_back({0, 0, 0, 0, id}); // never binned
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
            segments.push_back({segEnd, p});
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
        if (c.op == DrawOp::Glyph && clip.pathList == 0) {
            // compact glyph (40 bytes in the images buffer instead of a 128-byte GpuCmd): integer pixel bounds already
            // clipped (same floor/ceil of rect & clip as the kGlyph branch), atlas origin, uv, size, fill, and a clip
            // carrier cmd only when the clip is rounded (never binned)
            const f32 gx0 = std::max(c.rect.x, clip.r.x), gy0 = std::max(c.rect.y, clip.r.y);
            const f32 gx1 = std::min(c.rect.x + c.rect.w, clip.r.x + clip.r.w);
            const f32 gy1 = std::min(c.rect.y + c.rect.h, clip.r.y + clip.r.h);
            if (gx1 <= gx0 || gy1 <= gy0) continue;
            u32 ref = ~0u;
            if (clip.radius > 0.f) {
                if (clipRef == ~0u || !(clip.r == lastClip.r) || clip.radius != lastClip.radius) {
                    clipRef = u32(out.cmds.size());
                    pack(c, clip);
                    out.cmds.back().info[0] = kClipCarrier;
                    lastClip = clip;
                }
                ref = clipRef;
            }
            // fast record: an unclipped glyph on integer pixels whose rect is its bitmap size; the kernel's bitmap
            // bounds test is then exactly the kGlyph pixel-box test, so the box words are not read
            const bool fast = gx0 == c.rect.x && gy0 == c.rect.y && gx1 == c.rect.x + c.rect.w && gy1 == c.rect.y + c.rect.h &&
                              c.rect.x == std::floor(c.rect.x) && c.rect.y == std::floor(c.rect.y) && c.rect.w == f32(c.tw) &&
                              c.rect.h == f32(c.th);
            if (!(c.fill == lastFill) || fillAt == ~0u) { // consecutive glyphs share a colour; else the arena table
                fillAt = out.fillSlot(c.fill);
                lastFill = c.fill;
            }
            // ink trim: origin / uv / size shrink to the bitmap's ink box (pixel boxes intersected with it)
            int ox = int(c.rect.x), oy = int(c.rect.y); // int() truncation, as the kernel's int(r.x)
            u32 gu = c.u, gv = c.v, gtw = c.tw, gth = c.th;
            int bx0 = int(std::floor(gx0)), by0 = int(std::floor(gy0)), bx1 = int(std::ceil(gx1)), by1 = int(std::ceil(gy1));
            const u32 ink = inkBox(c.u, c.v, c.tw, c.th);
            if (ink == ~0u) continue; // blank bitmap (space): no coverage anywhere
            if (ink != 0xFFFFFFFEu) {
                const u32 ix0 = ink & 0xFFu, iy0 = (ink >> 8) & 0xFFu, ix1 = (ink >> 16) & 0xFFu, iy1 = ink >> 24;
                ox += int(ix0), oy += int(iy0);
                gu += ix0, gv += iy0, gtw = ix1 - ix0, gth = iy1 - iy0;
                bx0 = std::max(bx0, ox), by0 = std::max(by0, oy);
                bx1 = std::min(bx1, ox + int(gtw)), by1 = std::min(by1, oy + int(gth));
                if (bx1 <= bx0 || by1 <= by0) continue; // the clip keeps only blank margin
            }
            const auto u16i = [](int v) { return u32(std::clamp(v, -32768, 32767) + 32768); };
            // record -> scratch (7 words + source node); placed in the node's slot after the command pass
            const u32 gi = u32(out.glyphScratch.size() / 8);
            out.glyphScratch.resize(out.glyphScratch.size() + 8);
            u32* w = out.glyphScratch.data() + std::size_t(gi) * 8;
            w[7] = c.node;
            w[0] = u16i(ox) | (u16i(oy) << 16);
            w[1] = gu | (gv << 16);
            w[2] = std::min<u32>(gtw, 0xFFFFu) | (std::min<u32>(gth, 0x3FFFu) << 16) | (fast ? 0u : kGlyphBoxed) |
                   (ref != ~0u ? kGlyphRounded : 0u);
            w[3] = fillAt;
            w[4] = u16i(bx0) | (u16i(by0) << 16);
            w[5] = u16i(bx1) | (u16i(by1) << 16);
            w[6] = ref;
            Area g = pixels;
            g.x0 = std::max(g.x0, bx0), g.y0 = std::max(g.y0, by0);
            g.x1 = std::min(g.x1, bx1), g.y1 = std::min(g.y1, by1);
            if (g.x1 <= g.x0 || g.y1 <= g.y0) continue;
            g.id = kCompactGlyph | gi; // glyph index until the slot pass
            out.cmdSlot[ci] = kCompactGlyph | gi;
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
        out.cmdSlot[ci] = pa.id;
        areas.push_back(pa);
    }
    // slot pass: each source node keeps its arena slot while its glyphs fit, so added / removed / moved commands of
    // one node rewrite only that slot; other nodes' records keep their words (the upload page diff skips them)
    {
        const std::size_t ng = out.glyphScratch.size() / 8;
        out.glyphSlotOf.resize(ng);
        out.touched.clear();
        for (std::size_t g = 0; g < ng; ++g) {
            GpuDrawData::GlyphSlot& sl = out.glyphSlots[out.glyphScratch[g * 8 + 7]];
            if (sl.stamp != out.frameStamp) {
                sl.stamp = out.frameStamp;
                sl.count = sl.cursor = 0;
                out.touched.push_back(out.glyphScratch[g * 8 + 7]);
            }
            ++sl.count;
            out.glyphSlotOf[g] = &sl;
        }
        for (auto it = out.glyphSlots.begin(); it != out.glyphSlots.end();) { // vanished nodes free their slots
            if (it->second.stamp != out.frameStamp) {
                out.arenaHoles += it->second.cap * kCompactGlyphWords;
                it = out.glyphSlots.erase(it);
            } else ++it;
        }
        out.arenaLive = 0;
        for (u32 node : out.touched) {
            GpuDrawData::GlyphSlot& sl = out.glyphSlots[node];
            if (sl.count > sl.cap) { // grow: a new slot at the arena end with headroom; the old one becomes a hole
                out.arenaHoles += sl.cap * kCompactGlyphWords;
                sl.cap = sl.count + sl.count / 8 + 2;
                sl.off = u32(out.arena.size());
                out.arena.resize(out.arena.size() + std::size_t(sl.cap) * kCompactGlyphWords, 0u);
            }
            out.arenaLive += sl.cap * kCompactGlyphWords;
        }
        for (std::size_t g = 0; g < ng; ++g) {
            GpuDrawData::GlyphSlot& sl = *out.glyphSlotOf[g];
            const u32 local = sl.off + sl.cursor++ * kCompactGlyphWords;
            std::memcpy(out.arena.data() + local, out.glyphScratch.data() + g * 8, kCompactGlyphWords * 4);
            out.glyphScratch[g * 8 + 7] = out.arenaBase + local; // absolute record offset
        }
        if (ng == 0) out.resetArena(); // no glyphs: drop the arena (nothing references it)
        for (Area& a : areas)
            if ((a.id & kGlyphRun) == kCompactGlyph) a.id = kCompactGlyph | out.glyphScratch[std::size_t(a.id & ~kCompactGlyph) * 8 + 7];
        for (u32& sl : out.cmdSlot)
            if (sl != ~0u && (sl & kCompactGlyph)) sl = kCompactGlyph | out.glyphScratch[std::size_t(sl & ~kCompactGlyph) * 8 + 7];
    }
    for (const auto& sg : segments) {
        closeSegment(sg.end);
        out.passes.push_back(sg.blur);
    }
    closeSegment(u32(areas.size()));
    // volatile words (images, edges, stops, clip lists, runs) must end before the arena; else move the arena up
    if (!out.arena.empty() && out.images.size() > out.arenaBase) {
        out.arenaBase = u32(out.images.size() + out.images.size() / 8 + 256);
        out.resetArena();
        continue; // repack once against the new base
    }
    if (!out.arena.empty()) { // no glyphs: no arena, no padding
        out.images.resize(out.arenaBase, 0u);
        out.images.insert(out.images.end(), out.arena.begin(), out.arena.end());
    }
    // drop a trailing empty draw segment after a backdrop (nothing to draw); passes[0] always stays
    if (out.passes.size() > 1 && out.passes.back().kind == kPassDraw &&
        out.tiles[out.passes.back().tileBase + tileCount] == 0) {
        out.tiles.resize(out.passes.back().tileBase);
        out.passes.pop_back();
    }
    out.tiles.push_back(0u); // never empty
    if (out.cmds.empty()) out.cmds.push_back(GpuCmd{}); // non-empty buffer; no tile references it
    out.packedImageWords = u32(out.images.size());
    break;
    }
}

} // namespace fuse::ui::render
