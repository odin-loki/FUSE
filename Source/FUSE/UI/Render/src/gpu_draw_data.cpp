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

bool sameRest(const DrawCmd& p, const DrawCmd& q) { // every field but fill (copies need not keep padding bytes)
    const auto bits = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof x) == 0; };
    return p.op == q.op && p.gradient == q.gradient && p.u == q.u && p.v == q.v && p.tw == q.tw && p.th == q.th &&
           p.image == q.image && p.node == q.node && bits(p.rect.x, q.rect.x) && bits(p.rect.y, q.rect.y) &&
           bits(p.rect.w, q.rect.w) && bits(p.rect.h, q.rect.h) && bits(p.fill2, q.fill2) && bits(p.border, q.border) &&
           bits(p.borderWidth, q.borderWidth) && bits(p.radius, q.radius) && bits(p.blur, q.blur) &&
           bits(p.gradXf, q.gradXf) && p.stops == q.stops && p.stopCount == q.stopCount && bits(p.focal, q.focal);
}
bool sameFill(const DrawCmd& p, const DrawCmd& q) { return std::memcmp(&p.fill, &q.fill, sizeof p.fill) == 0; }
bool sameCmd(const DrawCmd& p, const DrawCmd& q) { return sameRest(p, q) && sameFill(p, q); }
/// plain draw op: packed from the command and its clip alone (no images-buffer words, no clip state change)
bool plainOp(const DrawCmd& c) {
    return c.op == DrawOp::Rect ? (c.gradient & 15u) < kGradientLinearStops
                                : c.op == DrawOp::Shadow || c.op == DrawOp::Glyph || c.op == DrawOp::Image || c.op == DrawOp::Line;
}
} // namespace

void packDrawList(const DrawList& list, u32 width, u32 height, GpuDrawData& out, const u8* atlas, u32 atlasSize,
                  const Image* const* images, u32 imageCount, Rect scissor, u32 atlasGeneration) {
    const DrawCmd* cur = list.cmds().data();
    const std::size_t n = list.cmds().size(), m = out.prevCmds.size();
    // ---- input diff: one changed range [p, m - s) -> [p, n - s) of the command list, plus the other inputs ----
    std::size_t p = 0, s = 0;
    bool inputs = false, genSame = false, structural = false;
    bool others = false; // inputs apart from the edge / stop tables (edgeDiff / stopDiff: their first changed entry)
    std::size_t edgeDiff = ~std::size_t(0), stopDiff = ~std::size_t(0);
    const std::vector<PathEdge>* edgesOld = nullptr;
    const std::vector<GradientStop>* stopsOld = nullptr;
    // same table words (edges / stops) behind a command, wherever they sit in the old and new tables
    const auto sameWords = [&](const DrawCmd& a, const DrawCmd& b) {
        if (a.op == DrawOp::Path || a.op == DrawOp::ClipPath) {
            const std::size_t ne = std::size_t(a.tw) | (std::size_t(a.th) << 16);
            const bool ia = std::size_t(a.image) + ne <= edgesOld->size(), ib = std::size_t(b.image) + ne <= list.pathEdges().size();
            if (ia != ib || (ia && ne && std::memcmp(edgesOld->data() + a.image, list.pathEdges().data() + b.image, ne * sizeof(PathEdge)) != 0))
                return false;
        }
        if (a.stopCount) {
            const bool ia = std::size_t(a.stops) + a.stopCount <= stopsOld->size(), ib = std::size_t(b.stops) + a.stopCount <= list.gradientStops().size();
            if (ia != ib || (ia && std::memcmp(stopsOld->data() + a.stops, list.gradientStops().data() + b.stops, a.stopCount * sizeof(GradientStop)) != 0))
                return false;
        }
        return true;
    };
    // equal commands; table indices may differ when the words behind them are equal (an inserted path does not
    // shift the commands after it)
    const auto same = [&](const DrawCmd& a, const DrawCmd& b) {
        if (sameCmd(a, b)) return edgeDiff == ~std::size_t(0) && stopDiff == ~std::size_t(0) ? true : sameWords(a, b);
        const bool edges = a.op == DrawOp::Path || a.op == DrawOp::ClipPath;
        if (!edges && !a.stopCount) return false;
        DrawCmd t = b;
        if (edges) t.image = a.image;
        if (a.stopCount) t.stops = a.stops;
        return sameCmd(a, t) && sameWords(a, b);
    };
    {
        u32 key[9] = {width, height, atlasSize, imageCount, atlasGeneration};
        const f32 sc[4] = {scissor.x, scissor.y, scissor.w, scissor.h};
        std::memcpy(&key[5], sc, sizeof sc);
        // compare a table with its copy from the last frame; on a difference the copy moves to `old` (content
        // compares of commands whose table index moved) and is refreshed (at = first differing entry)
        const auto sameVec = [](auto& prev, auto& old, const auto& cv, std::size_t& at) {
            using T = typename std::decay_t<decltype(cv)>::value_type;
            const std::size_t k = std::min(prev.size(), cv.size());
            std::size_t i = 0;
            constexpr std::size_t B = 256;
            while (i < k && std::memcmp(prev.data() + i, cv.data() + i, std::min(B, k - i) * sizeof(T)) == 0) i += B;
            i = std::min(i, k);
            while (i < k && std::memcmp(prev.data() + i, cv.data() + i, sizeof(T)) == 0) ++i;
            if (i == k && prev.size() == cv.size()) return at = ~std::size_t(0), true;
            at = i;
            old.swap(prev);
            prev.assign(cv.begin(), cv.end());
            return false;
        };
        inputs = out.contentVersion != 0 && out.prevAtlas == atlas && std::memcmp(key, out.prevKey, 16) == 0 &&
                 std::memcmp(key + 5, out.prevKey + 5, 16) == 0;
        genSame = atlasGeneration != ~0u && out.atlasGeneration == atlasGeneration;
        // always run both (each refreshes its copy)
        thread_local std::vector<PathEdge> oldEdges;
        thread_local std::vector<GradientStop> oldStops;
        const bool tables = sameVec(out.prevEdges, oldEdges, list.pathEdges(), edgeDiff) &
                            sameVec(out.prevStops, oldStops, list.gradientStops(), stopDiff);
        edgesOld = edgeDiff == ~std::size_t(0) ? &out.prevEdges : &oldEdges;
        stopsOld = stopDiff == ~std::size_t(0) ? &out.prevStops : &oldStops;
        if (inputs) // image pixels: compare with the copies already in the packed buffer
            for (u32 i = 0; i < imageCount && inputs; ++i) {
                const Image* img = images ? images[i] : nullptr;
                const u32* t = out.images.data() + i * 4;
                const bool ext = img && img->externalSlot >= 0 && img->externalSlot < 4;
                const bool hdr = !ext && img && img->hasHdr();
                if (t[1] != (img ? u32(img->width) : 1u) || t[2] != (img ? u32(img->height) : 1u) ||
                    t[3] != (ext ? u32(img->externalSlot) + 1u : hdr ? kImageHalf : 0u))
                    inputs = false;
                else if (hdr)
                    inputs = std::size_t(t[0]) + img->hdrPixels.size() * 2 <= out.images.size() &&
                             std::memcmp(out.images.data() + t[0], img->hdrPixels.data(), img->hdrPixels.size() * 8) == 0;
                else if (!ext && img && !img->pixels.empty())
                    inputs = std::size_t(t[0]) + img->pixels.size() <= out.images.size() &&
                             std::memcmp(out.images.data() + t[0], img->pixels.data(), img->pixels.size() * 4) == 0;
            }
        std::memcpy(out.prevKey, key, sizeof key);
        out.prevAtlas = atlas;
        others = inputs;
        inputs = inputs && tables;
        // common prefix / suffix (block memcmp, field compare on a mismatching block)
        const DrawCmd* prev = out.prevCmds.data();
        const std::size_t mn = std::min(n, m);
        constexpr std::size_t B = 64;
        while (p < mn) {
            const std::size_t k = std::min(B, mn - p);
            if (std::memcmp(prev + p, cur + p, k * sizeof(DrawCmd)) != 0) {
                std::size_t j = p;
                while (j < p + k && same(prev[j], cur[j])) ++j;
                if (j < p + k) { p = j; break; }
            }
            p += k;
        }
        s = std::min(s, mn - p);
        while (s < mn - p) {
            const std::size_t k = std::min(B, mn - p - s);
            if (std::memcmp(prev + m - s - k, cur + n - s - k, k * sizeof(DrawCmd)) != 0) {
                std::size_t j = 0;
                while (j < k && same(prev[m - s - 1 - j], cur[n - s - 1 - j])) ++j;
                if (j < k) { s += j; break; }
            }
            s += k;
        }
        // a same-length change touching only fills is a colour patch; anything else is structural
        out.patches.clear();
        structural = n != m;
        if (!structural)
            for (std::size_t i = p; i < n - s && !structural; ++i) {
                if (!sameRest(prev[i], cur[i])) structural = true;
                else if (!sameFill(prev[i], cur[i])) out.patches.push_back(u32(i));
            }
    }
    const std::size_t ep = m - s, ec = n - s; // changed range: old [p, ep), new [p, ec)
    const auto syncPrev = [&] {
        if (n == m) std::memcpy(out.prevCmds.data() + p, cur + p, (ec - p) * sizeof(DrawCmd));
        else {
            out.prevCmds.erase(out.prevCmds.begin() + std::ptrdiff_t(p), out.prevCmds.begin() + std::ptrdiff_t(ep));
            out.prevCmds.insert(out.prevCmds.begin() + std::ptrdiff_t(p), cur + p, cur + ec);
        }
        if (edgeDiff != ~std::size_t(0) || stopDiff != ~std::size_t(0)) // moved table indices of the suffix
            for (std::size_t i = ec; i < n; ++i) {
                DrawCmd& q = out.prevCmds[i];
                if (q.op == DrawOp::Path || q.op == DrawOp::ClipPath) q.image = cur[i].image;
                if (q.stopCount) q.stops = cur[i].stops;
            }
    };
    out.reused = inputs && genSame && !structural && out.patches.empty();
    out.patched = false;
    out.incremental = false;
    if (out.reused) return;
    // colour-only frame: patch the packed fills in place. Glyph records get their fill words appended at the end
    // of `images` (deduplicated against earlier patches), so no other word moves and the upload page diff writes
    // only the touched records / commands and the tail.
    if (inputs && genSame && !structural && out.cmdSlot.size() == n &&
        out.images.size() + out.patches.size() * 4 <= out.packedImageWords + 4096) {
        for (u32 idx : out.patches) {
            const u32 slot = out.cmdSlot[idx];
            if (slot == ~0u) continue; // culled
            const Color f = cur[idx].fill;
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
        syncPrev();
        out.patched = true;
        static std::atomic<u64> pv{1ull << 62}; // own range, still process-unique
        out.contentVersion = ++pv;
        return;
    }
    static std::atomic<u64> versions{0}; // process-unique, so a slot never mistakes another GpuDrawData's content
    out.contentVersion = ++versions;
    out.atlasGeneration = atlasGeneration;
    out.atlas = atlas;

    const u32 tilesX = (width + kUiTile - 1) / kUiTile, tilesY = (height + kUiTile - 1) / kUiTile;
    const u32 tileCount = tilesX * tilesY;
    const Rect full{0.f, 0.f, f32(width), f32(height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);

    // glyph ink boxes: the tight rect of non-zero atlas coverage inside each glyph bitmap (x0 | y0<<8 | x1<<16 | y1<<24,
    // relative to the bitmap; ~0u = blank). Glyph records shrink to it, so tiles their padding / blank margin merely
    // touches never list them (per-tile culling) and run column ranges narrow; outside the ink a == 0, so bit-exact.
    const u32 atlasS = atlas ? atlasSize : 0;
    // kept across frames while the atlas is the same (pointer, size, generation); else rescanned
    if (out.inkKeys.size() < 1024) out.inkKeys.assign(1024, ~0ull), out.inkBoxes.assign(1024, 0u), out.inkCount = 0;
    else if (atlasGeneration == ~0u || out.inkAtlas != atlas || out.inkSize != atlasS || out.inkGen != atlasGeneration)
        std::fill(out.inkKeys.begin(), out.inkKeys.end(), ~0ull), out.inkCount = 0;
    out.inkAtlas = atlas, out.inkSize = atlasS, out.inkGen = atlasGeneration;
    std::size_t& inkCount = out.inkCount;
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
            const u32 S = atlasS;
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

    struct Area {
        int x0, y0, x1, y1;
        u32 id; ///< tile entry: a cmds index, or kCompactGlyph | word offset of a compact glyph record
    };
    thread_local std::vector<Area> areas;
    thread_local std::vector<u32> cursor, csr, entT, runT, rawT;
    // pixel box of an area (u16 each: x0 | y0 << 16 | x1 << 32 | y1 << 48); ~0 = nothing binned
    const auto boxOf = [](const Area& a) -> u64 {
        if (a.x1 <= a.x0 || a.y1 <= a.y0) return ~0ull;
        return u64(u32(a.x0)) | (u64(u32(a.y0)) << 16) | (u64(u32(a.x1)) << 32) | (u64(u32(a.y1)) << 48);
    };
    // a compact glyph record (in the slot arena, addressed by its absolute images word offset)
    const auto rec = [&](u32 abs) -> const u32* { return out.arena.data() + (abs - out.arenaBase); };
    // tile content from its ids in paint order: entries, then glyph run records (consecutive compact glyphs with
    // non-decreasing x0 become one entry, so the kernel binary-searches the few glyphs that can cover a pixel);
    // run entries hold kGlyphRun | offset within runT until placed
    const auto buildTile = [&](const u32* ids, u32 cnt, u32 t) {
        entT.clear();
        runT.clear();
        const auto x0 = [&](u32 id) { return rec(id & ~kCompactGlyph)[4] & 0xFFFFu; };
        for (u32 k = 0; k < cnt;) {
            u32 j = k;
            if ((ids[k] & kGlyphRun) == kCompactGlyph)
                while (j + 1 < cnt && (ids[j + 1] & kGlyphRun) == kCompactGlyph && x0(ids[j + 1]) >= x0(ids[j])) ++j;
            if (j - k + 1 < kMinGlyphRun) {
                for (; k <= j; ++k) entT.push_back(ids[k]);
                continue;
            }
            const u32 nr = j - k + 1, at = u32(runT.size());
            runT.resize(runT.size() + 2 + kUiTile + std::size_t(nr));
            u32* r = runT.data() + at;
            u32 y0 = 0xFFFFu, y1 = 0, maxW = 0;
            for (u32 q = 0; q < nr; ++q) {
                const u32 g = ids[k + q] & ~kCompactGlyph;
                const u32 b0 = rec(g)[4], b1 = rec(g)[5];
                y0 = std::min(y0, b0 >> 16);
                y1 = std::max(y1, b1 >> 16);
                maxW = std::max(maxW, (b1 & 0xFFFFu) - (b0 & 0xFFFFu));
                r[2 + kUiTile + q] = g;
            }
            r[0] = nr;
            r[1] = y0 | (y1 << 16);
            // per tile column: [first, end) of the glyphs whose x0 lies in (x - maxW, x] (keys are sorted)
            const u32 cx0 = (t % tilesX) * kUiTile + 32768u;
            u32 lo = 0, hi = 0;
            for (u32 col = 0; col < kUiTile; ++col) {
                const u32 x = cx0 + col;
                while (hi < nr && x0(ids[k + hi]) <= x) ++hi;
                while (lo < hi && x0(ids[k + lo]) + maxW <= x) ++lo;
                r[2 + col] = lo | (hi << 16);
            }
            entT.push_back(kGlyphRun | at);
            k = j + 1;
        }
    };
    // tile buffer: tiles[0] = 0 (the empty slot); per pass a header of one word per tile (its slot offset, 0 =
    // empty) at passHeader[pass]; slots: count, entries, glyph run records
    const auto tileHeader = [&](u32 pass) { return out.passHeader[pass]; };
    // writes entT / runT into the (pass, tile) slot: in place while it fits, else a new slot at the end
    const auto placeTileFrom = [&](u32 pass, u32 t, const u32* ent, u32 E, const u32* run, u32 R) {
        const u32 si = pass * tileCount + t;
        if (E == 0) {
            out.tiles[tileHeader(pass) + t] = 0u;
            return;
        }
        const u32 size = 1 + E + R;
        if (size > out.tileSlotCap[si]) {
            out.tileHoles += out.tileSlotCap[si];
            out.tileSlotCap[si] = size + size / 8;
            out.tileSlotOff[si] = u32(out.tiles.size());
            out.tiles.resize(out.tiles.size() + out.tileSlotCap[si], 0u);
        }
        const u32 off = out.tileSlotOff[si];
        u32* d = out.tiles.data() + off;
        d[0] = E;
        for (u32 k = 0; k < E; ++k)
            d[1 + k] = (ent[k] & kGlyphRun) == kGlyphRun ? kGlyphRun | (ent[k] - kGlyphRun + off + 1 + E) : ent[k];
        if (R) std::memcpy(d + 1 + E, run, std::size_t(R) * 4);
        out.tiles[tileHeader(pass) + t] = off;
    };
    const auto placeTile = [&](u32 pass, u32 t) {
        placeTileFrom(pass, t, entT.data(), u32(entT.size()), runT.data(), u32(runT.size()));
    };
    // bins areas (paint order) into the tiles of one draw pass
    const auto binAreas = [&](const Area* as, std::size_t na, u32 pass) {
        csr.assign(std::size_t(tileCount) + 1, 0u);
        u32* off = csr.data();
        for (std::size_t i = 0; i < na; ++i) {
            const Area& a = as[i];
            if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
            const int tx0 = a.x0 / int(kUiTile), tx1 = (a.x1 - 1) / int(kUiTile);
            const int ty0 = a.y0 / int(kUiTile), ty1 = (a.y1 - 1) / int(kUiTile);
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) ++off[std::size_t(ty) * tilesX + tx];
        }
        u32 sum = 0; // prefix sums -> offsets; then ids in draw order
        for (u32 t = 0; t < tileCount; ++t) {
            const u32 k = off[t];
            off[t] = sum;
            sum += k;
        }
        off[tileCount] = sum;
        cursor.assign(off, off + tileCount);
        csr.resize(std::size_t(tileCount) + 1 + sum);
        off = csr.data();
        u32* ids = csr.data() + tileCount + 1;
        for (std::size_t i = 0; i < na; ++i) {
            const Area& a = as[i];
            if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
            const int tx0 = a.x0 / int(kUiTile), tx1 = (a.x1 - 1) / int(kUiTile);
            const int ty0 = a.y0 / int(kUiTile), ty1 = (a.y1 - 1) / int(kUiTile);
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) ids[cursor[std::size_t(ty) * tilesX + tx]++] = a.id;
        }
        u32 live = 0;
        for (u32 t = 0; t < tileCount; ++t) {
            buildTile(ids + off[t], off[t + 1] - off[t], t);
            placeTile(pass, t);
            live += out.tileSlotCap[pass * tileCount + t];
        }
        out.tileLive += live;
    };
    // passes: draw pass k over passHeader[k], blurPasses[k] after it; a trailing draw pass with no entries is dropped
    const auto rebuildPasses = [&] {
        out.passes.clear();
        const u32 np = out.tilePasses;
        for (u32 k = 0; k < np; ++k) {
            const bool last = k + 1 == np;
            bool empty = false;
            if (last && k > 0) {
                empty = true;
                const u32* h = out.tiles.data() + out.passHeader[k];
                for (u32 t = 0; t < tileCount && empty; ++t) empty = h[t] == 0u;
            }
            if (!empty) {
                GpuPass dp;
                dp.kind = kPassDraw;
                dp.tileBase = out.passHeader[k];
                out.passes.push_back(dp);
            }
            if (!last) out.passes.push_back(out.blurPasses[k]);
        }
    };
    // volatile words (edges, clip lists, stops) are appended to *V at absolute offset vb + V->size(): `images`
    // in a full pack, the arena tail in an incremental one
    std::vector<u32>* V = &out.images;
    u32 vb = 0;
    const auto vsz = [&] { return vb + u32(V->size()); };
    const auto vr = [&](u32 abs) { return abs < vb ? out.images[abs] : (*V)[abs - vb]; };

    Clip stack[32];
    int depth = 0;
    stack[0] = {scissor, 0.f, 0u};
    u32 clipRef = ~0u; // carrier cmd of the current glyph clip
    Clip lastClip{};
    Color lastFill{};
    u32 fillAt = ~0u;
    const auto pack = [&](const DrawCmd& c, const Clip& clip, std::size_t ci) {
        GpuCmd& g = out.cmdScratch.emplace_back(); // built in place (every field is written below)
        out.cmdScratchNode.push_back(c.node);
        out.cmdScratchSrc.push_back(u32(ci));
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
            const u32 ns = std::size_t(c.stops) + c.stopCount <= stops.size() ? c.stopCount : 0u;
            if ((c.gradient & 15u) == kGradientRadialFocal) // the focal point rides just before the stops
                for (f32 f : c.focal) { u32 u; std::memcpy(&u, &f, 4); V->push_back(u); }
            const u32 at = vsz();
            for (u32 i = 0; i < ns; ++i) { // 5 float words per stop, after the image / edge words
                const GradientStop& st = stops[c.stops + i];
                const f32 w[5] = {st.offset, st.color.r, st.color.g, st.color.b, st.color.a};
                for (f32 f : w) { u32 u; std::memcpy(&u, &f, 4); V->push_back(u); }
            }
            put4(g.grad, c.gradXf[0], c.gradXf[1], c.gradXf[2], c.gradXf[3]);
            u32 bits[2] = {at, ns};
            f32 fb[2];
            std::memcpy(fb, bits, 8);
            put4(g.grad2, c.gradXf[4], c.gradXf[5], fb[0], fb[1]);
        } else if ((c.op == DrawOp::Backdrop || c.op == DrawOp::PushFilter || c.op == DrawOp::PopFilter) &&
                   (c.stopCount == 4 || c.stopCount == 6 || isFilterProgram(c)) &&
                   std::size_t(c.stops) + c.stopCount <= list.gradientStops().size()) {
            // filter colour matrix: 4 rows x (r, g, b, a, offset) words, as the CPU's filterPixel reads them; then with
            // a drop shadow: dx, dy (whole pixels), colour r g b a (straight)
            const u32 at = vsz();
            const bool prog = isFilterProgram(c); // SVG program (encodeFilterProgram): every stop, same 5-word layout
            for (u32 i = 0; i < (prog ? u32(c.stopCount) : 4u); ++i) {
                const GradientStop& st = list.gradientStops()[c.stops + i];
                const f32 w[5] = {st.color.r, st.color.g, st.color.b, st.color.a, st.offset};
                for (f32 f : w) { u32 u; std::memcpy(&u, &f, 4); V->push_back(u); }
            }
            if (c.stopCount == 6 && !prog) {
                const GradientStop* sh = list.gradientStops().data() + c.stops + 4;
                const f32 w[6] = {f32(std::lround(sh[1].color.r)), f32(std::lround(sh[1].color.g)), sh[0].color.r, sh[0].color.g,
                                  sh[0].color.b, sh[0].color.a};
                for (f32 f : w) { u32 u; std::memcpy(&u, &f, 4); V->push_back(u); }
            }
            u32 bits[2] = {at, u32(c.stopCount)};
            f32 fb[2];
            std::memcpy(fb, bits, 8);
            put4(g.grad2, c.op != DrawOp::Backdrop && c.v ? 1.f : 0.f, 0.f, fb[0], fb[1]); // x: chained layer
        }
    };
    // full pack, direct glyphs: a record goes straight into its node's arena slot while the slot (from the last
    // frame) has room, with its final id; a node that outgrows its slot moves its records so far, and the rest, to
    // the overflow scratch (glyphScratch + ovArea / ovOld), which the glyph slot pass places as before
    bool directGlyphs = false;
    thread_local std::vector<u32> gTouched, ovArea, ovOld, refLocal;
    u32 dNode = 0, glyphTotal = 0;
    GpuDrawData::GlyphSlot* dSl = nullptr;
    const auto scratchGlyph = [&](const u32* rec7, u32 node, u32 src, u32 areaIdx, u32 oldAbs) {
        const std::size_t at = out.glyphScratch.size();
        out.glyphScratch.resize(at + 9);
        u32* w = out.glyphScratch.data() + at;
        std::memcpy(w, rec7, kCompactGlyphWords * 4);
        w[7] = node;
        w[8] = src;
        ovArea.push_back(areaIdx);
        ovOld.push_back(oldAbs);
    };
    // one draw command (not clip / backdrop / path): a compact glyph record (glyphScratch, 7 words + node + source)
    // or a GpuCmd (cmdScratch); its area goes to `areas` with a scratch id (kCompactGlyph | glyph index, or the
    // cmdScratch index) that the slot passes turn into the final one (direct glyphs: the final id at once)
    const auto emit = [&](const DrawCmd& c, std::size_t ci, const Clip& clip, u32& slotOut, u64& boxOut) {
        Rect bounds = c.rect;
        if (c.op == DrawOp::Shadow)
            bounds = {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
        if (c.op == DrawOp::Line) {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            bounds = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw,
                      std::abs(c.rect.w - c.rect.x) + 2 * hw, std::abs(c.rect.h - c.rect.y) + 2 * hw};
        }
        const Rect area = bounds.intersect(clip.r);
        if (area.empty()) return;
        if (c.op == DrawOp::Glyph && !atlas) return;
        if (c.op == DrawOp::Image && c.image >= imageCount) return;
        const Area pixels{std::max(0, int(std::floor(area.x))), std::max(0, int(std::floor(area.y))),
                          std::min(int(width), int(std::ceil(area.right()))), std::min(int(height), int(std::ceil(area.bottom()))), 0u};
        if (c.op == DrawOp::Glyph && clip.pathList == 0) {
            // compact glyph (28 bytes in the images buffer instead of a 160-byte GpuCmd): integer pixel bounds already
            // clipped (same floor/ceil of rect & clip as the kGlyph branch), atlas origin, uv, size, fill, and a clip
            // carrier cmd only when the clip is rounded (never binned)
            const f32 gx0 = std::max(c.rect.x, clip.r.x), gy0 = std::max(c.rect.y, clip.r.y);
            const f32 gx1 = std::min(c.rect.x + c.rect.w, clip.r.x + clip.r.w);
            const f32 gy1 = std::min(c.rect.y + c.rect.h, clip.r.y + clip.r.h);
            if (gx1 <= gx0 || gy1 <= gy0) return;
            u32 ref = ~0u;
            if (clip.radius > 0.f) {
                if (clipRef == ~0u || !(clip.r == lastClip.r) || clip.radius != lastClip.radius) {
                    clipRef = u32(out.cmdScratch.size());
                    pack(c, clip, ci);
                    out.cmdScratch.back().info[0] = kClipCarrier;
                    lastClip = clip;
                }
                ref = clipRef;
            }
            // fast record: an unclipped glyph on integer pixels whose rect is its bitmap size; the kernel's bitmap
            // bounds test is then exactly the kGlyph pixel-box test, so the box words are not read
            const bool fast = gx0 == c.rect.x && gy0 == c.rect.y && gx1 == c.rect.x + c.rect.w && gy1 == c.rect.y + c.rect.h &&
                              c.rect.x == std::floor(c.rect.x) && c.rect.y == std::floor(c.rect.y) && c.rect.w == f32(c.tw) &&
                              c.rect.h == f32(c.th);
            // ink trim: origin / uv / size shrink to the bitmap's ink box (pixel boxes intersected with it)
            int ox = int(c.rect.x), oy = int(c.rect.y); // int() truncation, as the kernel's int(r.x)
            u32 gu = c.u, gv = c.v, gtw = c.tw, gth = c.th;
            int bx0 = int(std::floor(gx0)), by0 = int(std::floor(gy0)), bx1 = int(std::ceil(gx1)), by1 = int(std::ceil(gy1));
            const u32 ink = inkBox(c.u, c.v, c.tw, c.th);
            if (ink == ~0u) return; // blank bitmap (space): no coverage anywhere
            if (ink != 0xFFFFFFFEu) {
                const u32 ix0 = ink & 0xFFu, iy0 = (ink >> 8) & 0xFFu, ix1 = (ink >> 16) & 0xFFu, iy1 = ink >> 24;
                ox += int(ix0), oy += int(iy0);
                gu += ix0, gv += iy0, gtw = ix1 - ix0, gth = iy1 - iy0;
                bx0 = std::max(bx0, ox), by0 = std::max(by0, oy);
                bx1 = std::min(bx1, ox + int(gtw)), by1 = std::min(by1, oy + int(gth));
                if (bx1 <= bx0 || by1 <= by0) return; // the clip keeps only blank margin
            }
            Area g = pixels;
            g.x0 = std::max(g.x0, bx0), g.y0 = std::max(g.y0, by0);
            g.x1 = std::min(g.x1, bx1), g.y1 = std::min(g.y1, by1);
            if (g.x1 <= g.x0 || g.y1 <= g.y0) return;
            if (!(c.fill == lastFill) || fillAt == ~0u) { // consecutive glyphs share a colour; else the arena table
                fillAt = out.fillSlot(c.fill);
                lastFill = c.fill;
            }
            const auto u16i = [](int v) { return u32(std::clamp(v, -32768, 32767) + 32768); };
            if (directGlyphs) {
                const u32 rw[kCompactGlyphWords] = {
                    u16i(ox) | (u16i(oy) << 16), gu | (gv << 16),
                    std::min<u32>(gtw, 0xFFFFu) | (std::min<u32>(gth, 0x3FFFu) << 16) | (fast ? 0u : kGlyphBoxed) |
                        (ref != ~0u ? kGlyphRounded : 0u),
                    fillAt, u16i(bx0) | (u16i(by0) << 16), u16i(bx1) | (u16i(by1) << 16), ref};
                if (!dSl || c.node != dNode) dNode = c.node, dSl = &out.glyphSlots[c.node];
                GpuDrawData::GlyphSlot& sl = *dSl;
                if (sl.stamp != out.frameStamp) {
                    sl.stamp = out.frameStamp;
                    sl.count = sl.cursor = 0;
                    gTouched.push_back(c.node);
                }
                ++sl.count;
                ++glyphTotal;
                if (sl.cursor < sl.cap) {
                    const u32 local = sl.off + sl.cursor++ * kCompactGlyphWords;
                    std::memcpy(out.arena.data() + local, rw, sizeof rw);
                    out.arenaSrc[local] = u32(ci);
                    if (ref != ~0u) refLocal.push_back(local), refLocal.push_back(c.node);
                    g.id = kCompactGlyph | (out.arenaBase + local);
                    slotOut = g.id;
                } else {
                    if (sl.cursor != ~0u) { // outgrown: its records so far join the overflow scratch, in order
                        const bool undo = out.prevArenaBase == out.arenaBase;
                        for (u32 k = 0; k < sl.cursor; ++k) {
                            const u32 local = sl.off + k * kCompactGlyphWords;
                            scratchGlyph(out.arena.data() + local, c.node, out.arenaSrc[local], ~0u, out.arenaBase + local);
                            const std::size_t abs = std::size_t(out.arenaBase) + local;
                            if (undo && abs + kCompactGlyphWords <= out.prevImages.size()) // the hole keeps last frame's words
                                std::memcpy(out.arena.data() + local, out.prevImages.data() + abs, kCompactGlyphWords * 4);
                        }
                        sl.cursor = ~0u;
                    }
                    scratchGlyph(rw, c.node, u32(ci), u32(areas.size()), ~0u);
                    g.id = kCompactGlyph;
                    slotOut = g.id; // final id set by the glyph slot pass
                }
                boxOut = boxOf(g);
                areas.push_back(g);
                return;
            }
            const u32 gi = u32(out.glyphScratch.size() / 9);
            out.glyphScratch.resize(out.glyphScratch.size() + 9);
            u32* w = out.glyphScratch.data() + std::size_t(gi) * 9;
            w[0] = u16i(ox) | (u16i(oy) << 16);
            w[1] = gu | (gv << 16);
            w[2] = std::min<u32>(gtw, 0xFFFFu) | (std::min<u32>(gth, 0x3FFFu) << 16) | (fast ? 0u : kGlyphBoxed) |
                   (ref != ~0u ? kGlyphRounded : 0u);
            w[3] = fillAt;
            w[4] = u16i(bx0) | (u16i(by0) << 16);
            w[5] = u16i(bx1) | (u16i(by1) << 16);
            w[6] = ref;
            w[7] = c.node;
            w[8] = u32(ci);
            g.id = kCompactGlyph | gi;
            slotOut = g.id;
            boxOut = boxOf(g);
            areas.push_back(g);
            return;
        }
        Area pa = pixels;
        pa.id = u32(out.cmdScratch.size());
        pack(c, clip, ci);
        slotOut = pa.id;
        boxOut = boxOf(pa);
        areas.push_back(pa);
    };
    // one DrawList command in paint order: clip ops update the stack, a Backdrop closes the draw segment, paths
    // carry their edges in the volatile words, the rest go through emit
    struct Segment {
        u32 end;      ///< areas index where the draw segment closes
        u32 src;      ///< DrawList index of the backdrop
        GpuPass blur; ///< the backdrop pass that follows it
    };
    thread_local std::vector<Segment> segments;
    // CSS filter layers: open layer boxes and the save area top (floats in the blur scratch buffer)
    struct LayerBox {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        u32 save = 0;
    };
    thread_local std::vector<LayerBox> layerBoxes;
    layerBoxes.clear();
    u32 saveTop = 0;
    thread_local std::vector<u32> partner;
    partner.clear();
    bool hasFilters = false;
    for (std::size_t i = 0; i < n && !hasFilters; ++i) hasFilters = cur[i].op == DrawOp::PushFilter;
    if (hasFilters) matchFilters(list, partner);
    const auto filterPass = [&](const DrawCmd& c, GpuPass& bp) {
        bp.flags = 0;
        bp.planes = 2;
        bp.margin = 3 * bp.radius; // radius is set before filterPass
        if (std::size_t(c.stops) + c.stopCount > list.gradientStops().size()) return;
        if (isFilterProgram(c)) { // SVG program: margin = its reach
            const GradientStop* h = list.gradientStops().data() + c.stops;
            bp.flags = 4u;
            bp.ops = u32(h[0].offset);
            bp.planes = u32(h[0].color.r);
            bp.margin = u32(h[0].color.g);
            for (u32 i = 0; i < bp.ops && i < 32; ++i)
                if (filterProgramOp(h, i) == FeOp::Blur) bp.blurMask |= 1u << i;
            return;
        }
        if (c.stopCount != 4 && c.stopCount != 6) return;
        bp.flags = 1u;
        if (c.stopCount == 6) {
            const f32 sb = list.gradientStops()[c.stops + 4].offset;
            const int r2 = sb > 0.f ? std::max(1, int(std::lround(sb / 6.f))) : 0;
            bp.flags |= 2u;
            bp.radius2 = u32(r2);
            bp.inv2 = 1.f / f32(2 * r2 + 1);
            bp.planes = 4;
            const GradientStop& d = list.gradientStops()[c.stops + 5];
            bp.margin += u32(3 * r2) + u32(std::max(std::abs(std::lround(d.color.r)), std::abs(std::lround(d.color.g))));
        }
    };
    u32 clipVer = 1;
    int clipFloor = 0;     // incremental: the range may not pop below its starting depth
    bool unbalanced = false;
    const auto step = [&](std::size_t ci, u32& slotOut, u64& boxOut) {
        const DrawCmd& c = cur[ci];
        if (c.op == DrawOp::PushClip) {
            if (depth < 31) {
                ++depth, ++clipVer;
                stack[depth] = {stack[depth - 1].r.intersect(c.rect), c.radius[0], stack[depth - 1].pathList};
                if (stack[depth - 1].radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            return;
        }
        if (c.op == DrawOp::ClipPath) {
            // same clip state as CpuRasterizer: bounds (+1 px) intersect the rect clip; the edges and the list of
            // every active clip path ride in the volatile words
            if (depth < 31) {
                ++depth, ++clipVer;
                stack[depth] = stack[depth - 1];
                const std::size_t ne = std::size_t(c.tw) | (std::size_t(c.th) << 16);
                if (ne == 0 || std::size_t(c.image) + ne > list.pathEdges().size()) {
                    stack[depth].r = {};
                    return;
                }
                stack[depth].r = stack[depth].r.intersect({c.rect.x - 1.f, c.rect.y - 1.f, c.rect.w + 2.f, c.rect.h + 2.f});
                const u32 at = vsz();
                const std::size_t vl = V->size();
                V->resize(vl + ne * 5);
                std::memcpy(V->data() + vl, list.pathEdges().data() + c.image, ne * sizeof(PathEdge));
                const u32 parent = stack[depth - 1].pathList;
                const u32 pc = parent ? vr(parent) : 0u;
                const u32 lo = vsz();
                V->push_back(pc + 1);
                for (u32 q = 0; q < pc * 3; ++q) V->push_back(vr(parent + 1 + q));
                V->push_back(at);
                V->push_back(u32(ne));
                V->push_back(c.v ? 1u : 0u);
                stack[depth].pathList = lo;
            }
            return;
        }
        if (c.op == DrawOp::PopClip) {
            if (depth > clipFloor) --depth, ++clipVer;
            else if (clipFloor > 0) unbalanced = true;
            return;
        }
        const Clip& clip = stack[depth];
        if (c.op == DrawOp::Backdrop) {
            // same box as CpuRasterizer's backdropBlur: area = rect & clip & target, source = area + 3r margin
            const Rect area = c.rect.intersect(clip.r).intersect(full);
            if (area.empty()) return;
            const int r = c.blur > 0.f ? std::max(1, int(std::lround(c.blur / 3.f))) : 0; // 0: filter only
            int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
            int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
            const int mg = 3 * r;
            int sx0 = std::max(0, x0 - mg), sy0 = std::max(0, y0 - mg);
            int sx1 = std::min(int(width), x1 + mg), sy1 = std::min(int(height), y1 + mg);
            if (isFilterProgram(c) && std::size_t(c.stops) + c.stopCount <= list.gradientStops().size()) {
                const Color& g = list.gradientStops()[c.stops + 1].color; // SVG program: source = filter region
                sx0 = std::max(0, int(std::floor(g.r))), sy0 = std::max(0, int(std::floor(g.g)));
                sx1 = std::min(int(width), int(std::ceil(g.r + g.b))), sy1 = std::min(int(height), int(std::ceil(g.g + g.a)));
                x0 = std::max(x0, sx0), y0 = std::max(y0, sy0), x1 = std::min(x1, sx1), y1 = std::min(y1, sy1);
                if (x1 <= x0 || y1 <= y0) return;
            }
            const u32 id = u32(out.cmdScratch.size());
            pack(c, clip, ci);
            slotOut = id;
            GpuPass bp;
            bp.kind = kPassBackdrop;
            bp.cmd = id;
            bp.src[0] = sx0;
            bp.src[1] = sy0;
            bp.src[2] = sx1 - sx0;
            bp.src[3] = sy1 - sy0;
            bp.area[0] = x0;
            bp.area[1] = y0;
            bp.area[2] = x1;
            bp.area[3] = y1;
            bp.radius = u32(r);
            bp.inv = 1.f / f32(2 * r + 1);
            filterPass(c, bp);
            bp.saveTop = saveTop;
            segments.push_back({u32(areas.size()), u32(ci), bp}); // the draw segment closes here
            return;
        }
        if (c.op == DrawOp::PushFilter || c.op == DrawOp::PopFilter) {
            // CSS filter layer: begin saves + clears the box, end composites (CpuRasterizer layerBegin / layerEnd)
            if (ci >= partner.size() || partner[ci] == ~0u) return;
            LayerBox lb{};
            if (c.op == DrawOp::PushFilter) {
                const Rect area = c.rect.intersect(clip.r).intersect(full);
                if (!area.empty()) {
                    lb.x0 = int(std::floor(area.x)), lb.y0 = int(std::floor(area.y));
                    lb.x1 = int(std::ceil(area.right())), lb.y1 = int(std::ceil(area.bottom()));
                }
                lb.save = saveTop;
                saveTop += u32(std::max(0, lb.x1 - lb.x0) * std::max(0, lb.y1 - lb.y0) * 4);
                layerBoxes.push_back(lb);
            } else {
                if (layerBoxes.empty()) return;
                lb = layerBoxes.back();
                layerBoxes.pop_back();
            }
            const u32 top = lb.save + u32(std::max(0, lb.x1 - lb.x0) * std::max(0, lb.y1 - lb.y0) * 4);
            if (c.op == DrawOp::PopFilter) saveTop = lb.save;
            if (lb.x1 <= lb.x0 || lb.y1 <= lb.y0) return;
            const u32 id = u32(out.cmdScratch.size());
            pack(c, clip, ci);
            slotOut = id;
            GpuPass bp;
            bp.kind = c.op == DrawOp::PushFilter ? kPassLayerBegin : kPassLayerEnd;
            bp.cmd = id;
            bp.src[0] = bp.area[0] = lb.x0;
            bp.src[1] = bp.area[1] = lb.y0;
            bp.src[2] = lb.x1 - lb.x0;
            bp.src[3] = lb.y1 - lb.y0;
            bp.area[2] = lb.x1;
            bp.area[3] = lb.y1;
            const int r = c.blur > 0.f ? std::max(1, int(std::lround(c.blur / 3.f))) : 0;
            bp.radius = u32(r);
            bp.inv = 1.f / f32(2 * r + 1);
            filterPass(c, bp);
            bp.saveTop = top;
            segments.push_back({u32(areas.size()), u32(ci), bp});
            return;
        }
        if (c.op == DrawOp::Path) {
            const Rect area = c.rect.intersect(clip.r);
            if (area.empty()) return;
            const Area pixels{std::max(0, int(std::floor(area.x))), std::max(0, int(std::floor(area.y))),
                              std::min(int(width), int(std::ceil(area.right()))), std::min(int(height), int(std::ceil(area.bottom()))), 0u};
            // edges ride in the volatile words as floats (no extra binding): info.w = word offset
            const std::size_t ne = std::size_t(c.tw) | (std::size_t(c.th) << 16);
            if (ne == 0 || std::size_t(c.image) + ne > list.pathEdges().size()) return;
            const u32 at = vsz();
            const PathEdge* e = list.pathEdges().data() + c.image;
            const std::size_t vl = V->size();
            V->resize(vl + ne * 5);
            std::memcpy(V->data() + vl, e, ne * sizeof(PathEdge));
            // tile-row edge lists: edges whose y-range (grown by the stroke reach) touches the 16-px row. Dropped
            // edges cannot change the winding sub-samples or a non-zero stroke coverage there, so results are identical
            const f32 reach = c.borderWidth * 0.5f + 1.f;
            const u32 r0 = u32(pixels.y0) / kUiTile, r1 = u32(std::max(pixels.y1 - 1, pixels.y0)) / kUiTile;
            const u32 rows = r1 - r0 + 1;
            std::vector<u32>& W = *V;
            const std::size_t tb = W.size();
            W.resize(tb + 2 + rows + 1);
            W[tb] = r0;
            W[tb + 1] = rows;
            const std::size_t lb = W.size();
            for (u32 r = 0; r < rows; ++r) {
                W[tb + 2 + r] = u32(W.size() - lb);
                const f32 top = f32((r0 + r) * kUiTile) - reach, bottom = f32((r0 + r + 1) * kUiTile) + reach;
                for (std::size_t k = 0; k < ne; ++k) {
                    const f32 lo = std::min(e[k].y0, e[k].y1), hi = std::max(e[k].y0, e[k].y1);
                    if (hi >= top && lo <= bottom) W.push_back(u32(k));
                }
            }
            W[tb + 2 + rows] = u32(W.size() - lb);
            Area pa = pixels;
            pa.id = u32(out.cmdScratch.size());
            pack(c, clip, ci);
            out.cmdScratch.back().info[3] = at;
            slotOut = pa.id;
            boxOut = boxOf(pa);
            areas.push_back(pa);
            return;
        }
        emit(c, ci, clip, slotOut, boxOut);
    };
    // record placement outside a full pack: a free spot of the node's slot, its headroom, or the arena end
    const auto allocRec = [&](u32 node) -> u32 {
        for (std::size_t i = out.freeRecs.size(); i-- > 0;)
            if (out.freeRecs[i].first == node) {
                const u32 at = out.freeRecs[i].second;
                out.freeRecs[i] = out.freeRecs.back();
                out.freeRecs.pop_back();
                return at;
            }
        const auto it = out.glyphSlots.find(node);
        if (it != out.glyphSlots.end() && it->second.cursor < it->second.cap)
            return out.arenaBase + it->second.off + it->second.cursor++ * kCompactGlyphWords;
        const u32 at = out.arenaBase + u32(out.arena.size()); // overflow: re-slotted by the next full pack
        out.arena.resize(out.arena.size() + kCompactGlyphWords, 0u);
        out.arenaHoles += kCompactGlyphWords;
        return at;
    };
    const auto allocCmd = [&](u32 node) -> u32 {
        for (std::size_t i = out.freeCmds.size(); i-- > 0;)
            if (out.freeCmds[i].first == node) {
                const u32 at = out.freeCmds[i].second;
                out.freeCmds[i] = out.freeCmds.back();
                out.freeCmds.pop_back();
                return at;
            }
        const auto it = out.cmdSlots.find(node);
        if (it != out.cmdSlots.end() && it->second.cursor < it->second.cap) return it->second.off + it->second.cursor++;
        out.cmds.emplace_back();
        ++out.cmdHoles;
        return u32(out.cmds.size() - 1);
    };
    const auto srcOf = [&](u32 id) -> u32 {
        return (id & kCompactGlyph) ? out.arenaSrc[(id & ~kCompactGlyph) - out.arenaBase] : out.cmdSrc[id];
    };

    // commands outside the changed range keep their packed words: the table words behind them must be unchanged
    // (their index may have moved)
    const auto tablesKept = [&] {
        if (edgeDiff == ~std::size_t(0) && stopDiff == ~std::size_t(0)) return true;
        for (std::size_t i = 0; i < p; ++i)
            if (!sameWords(out.prevCmds[i], cur[i])) return false;
        for (std::size_t i = ec; i < n; ++i)
            if (!sameWords(out.prevCmds[i - ec + ep], cur[i])) return false;
        return true;
    };

    // ---- incremental structural pack ----
    // one changed range [p, ep) -> [p, ec): re-emit only the range (clip ops inside it must be balanced; backdrops
    // keep their count), put its records into free spots of their node slots, and rebuild only the tiles it
    // touches (one draw segment) or the segments it spans (backdrops in it)
    // (filter layers: full pack; their save areas and pass order span the whole list)
    bool prevFilters = false;
    for (std::size_t i = 0; i < m && !prevFilters && !hasFilters; ++i) prevFilters = out.prevCmds[i].op == DrawOp::PushFilter;
    if (others && structural && !hasFilters && !prevFilters && out.tilePasses > 0 && out.tileCountPrev == tileCount && out.cmdSlot.size() == m &&
        out.cmdBox.size() == m && out.images.size() == out.packedImageWords &&
        (out.arena.empty() || out.images.size() == out.arenaBase + out.arena.size()) && (ep - p) + (ec - p) <= 4096 &&
        !(out.arenaHoles > out.arenaLive && out.arenaHoles > 4096) && out.cmdVol.size() == m && tablesKept()) {
        bool ok = true;
        const auto cr = std::upper_bound(out.clipRuns.begin(), out.clipRuns.end(), u32(p),
                                         [](u32 v, const GpuDrawData::ClipRun& r) { return v < r.from; });
        if (cr == out.clipRuns.begin()) ok = false;
        Clip c0{};
        int d0 = 0;
        u32 oldBd = 0;
        if (ok) {
            c0 = {(cr - 1)->r, (cr - 1)->radius, (cr - 1)->pathList};
            d0 = int((cr - 1)->depth);
            int d = d0; // the old range must be balanced too (the suffix was packed under the clip after it)
            for (std::size_t i = p; i < ep && ok; ++i) {
                const DrawOp op = out.prevCmds[i].op;
                if (op == DrawOp::PushClip || op == DrawOp::ClipPath) { if (d < 31) ++d; }
                else if (op == DrawOp::PopClip) { if (d > d0) --d; else if (d0 > 0) ok = false; }
                else if (op == DrawOp::Backdrop && out.cmdSlot[i] != ~0u) ++oldBd;
            }
            ok = ok && d == d0;
        }
        // segment of the range: backdrops before p; the range's old backdrops are the next oldBd
        const u32 k0 = u32(std::lower_bound(out.backdropSrc.begin(), out.backdropSrc.end(), u32(p)) - out.backdropSrc.begin());
        if (ok && k0 + oldBd >= out.tilePasses) ok = false;
        if (ok) {
            // the old range's volatile words at the arena tail are given back (repeated insert / restore keeps the
            // arena size); others stay holes until the next full pack
            {
                thread_local std::vector<u64> blocks;
                blocks.clear();
                for (std::size_t i = p; i < ep; ++i)
                    if ((out.cmdVol[i] >> 32) && u32(out.cmdVol[i]) >= out.arenaBase) blocks.push_back(out.cmdVol[i]);
                std::sort(blocks.begin(), blocks.end(), [](u64 a, u64 b) { return u32(a) > u32(b); });
                for (u64 b : blocks) {
                    const u32 off = u32(b) - out.arenaBase, len = u32(b >> 32);
                    if (off + len != out.arena.size()) break;
                    out.arena.resize(off);
                    out.arenaHoles -= std::min(out.arenaHoles, len);
                }
            }
            if (out.arena.empty()) { // volatile words and records go to the arena tail: give it room
                if (out.images.size() > out.arenaBase) out.arenaBase = u32(out.images.size() + out.images.size() / 8 + 256);
                out.images.resize(out.arenaBase, 0u);
            }
            const u32 arena0 = u32(out.arena.size());
            V = &out.arena;
            vb = out.arenaBase;
            areas.clear();
            segments.clear();
            out.cmdScratch.clear();
            out.cmdScratchNode.clear();
            out.cmdScratchSrc.clear();
            out.glyphScratch.clear();
            depth = d0;
            stack[depth] = c0;
            clipFloor = d0;
            unbalanced = false;
            clipRef = ~0u;
            lastClip = {};
            lastFill = {};
            fillAt = ~0u;
            thread_local std::vector<u32> newSlot, areaOf;
            thread_local std::vector<u64> newBox, newVol;
            newVol.assign(ec - p, 0ull);
            thread_local std::vector<GpuDrawData::ClipRun> newRuns;
            newSlot.assign(ec - p, ~0u);
            newBox.assign(ec - p, ~0ull);
            areaOf.assign(ec - p, ~0u);
            newRuns.clear();
            const u32 ver0 = clipVer;
            u32 clipRec = clipVer;
            for (std::size_t i = p; i < ec; ++i) {
                if (clipRec != clipVer) {
                    newRuns.push_back({u32(i), stack[depth].r, stack[depth].radius, stack[depth].pathList, u32(depth)});
                    clipRec = clipVer;
                }
                const std::size_t before = areas.size();
                const u32 v0 = vsz();
                step(i, newSlot[i - p], newBox[i - p]);
                newVol[i - p] = u64(v0) | (u64(vsz() - v0) << 32);
                if (areas.size() > before) areaOf[i - p] = u32(before);
            }
            if (clipVer != ver0 && ec < n) newRuns.push_back({u32(ec), c0.r, c0.radius, c0.pathList, u32(d0)});
            ok = !unbalanced && depth == d0;
            const u32 nb = u32(segments.size()); // the range's backdrops now (oldBd before)
            // affected tiles of segment k0 (no backdrop in the range): old and new boxes
            thread_local std::vector<u32> mark, affected;
            if (mark.size() < tileCount) mark.assign(tileCount, 0u);
            affected.clear();
            const auto addBox = [&](u64 b) {
                if (b == ~0ull) return;
                const u32 tx0 = u32(b & 0xFFFF) / kUiTile, ty0 = u32((b >> 16) & 0xFFFF) / kUiTile;
                const u32 tx1 = (u32((b >> 32) & 0xFFFF) - 1) / kUiTile, ty1 = (u32(b >> 48) - 1) / kUiTile;
                for (u32 ty = ty0; ty <= ty1; ++ty)
                    for (u32 tx = tx0; tx <= tx1; ++tx) {
                        const u32 t = ty * tilesX + tx;
                        if (!mark[t]) mark[t] = 1, affected.push_back(t);
                    }
            };
            bool merge = ok && nb == 0 && oldBd == 0;
            if (merge) {
                for (std::size_t i = p; i < ep; ++i) addBox(out.cmdBox[i]);
                for (u64 b : newBox) addBox(b);
                for (u32 t : affected) mark[t] = 0;
                merge = affected.size() <= tileCount / 2 + 16; // else rebuild the whole segment
            }
            struct Span { u32 t, pre0, pre1, suf1; };
            thread_local std::vector<Span> spans;
            spans.clear();
            if (merge) { // old tile lists, runs expanded, split around the range: prefix | (removed) | suffix
                rawT.clear();
                const u32 hb = tileHeader(k0);
                for (u32 t : affected) {
                    const u32 h = out.tiles[hb + t], e = h ? out.tiles[h] : 0u;
                    Span sp{t, u32(rawT.size()), u32(rawT.size()), 0};
                    bool pre = true;
                    const auto one = [&](u32 x) {
                        const u32 src = srcOf(x);
                        if (src < p) { rawT.push_back(x); if (pre) sp.pre1 = u32(rawT.size()); }
                        else if (src >= ep) { pre = false; rawT.push_back(x); }
                    };
                    for (u32 k = h + 1; k < h + 1 + e; ++k) {
                        const u32 id = out.tiles[k];
                        if ((id & kGlyphRun) == kGlyphRun) {
                            const u32 o = id & ~kGlyphRun, nr = out.tiles[o];
                            for (u32 q = 0; q < nr; ++q) one(kCompactGlyph | out.tiles[o + 2 + kUiTile + q]);
                        } else one(id);
                    }
                    sp.suf1 = u32(rawT.size());
                    spans.push_back(sp);
                }
            }
            if (ok) {
                // free the old range's records / cmds, place the new ones (free spots of their node's slots first)
                for (std::size_t i = p; i < ep; ++i) {
                    const u32 sl = out.cmdSlot[i];
                    if (sl == ~0u) continue;
                    if (sl & kCompactGlyph) out.freeRecs.push_back({out.prevCmds[i].node, sl & ~kCompactGlyph});
                    else out.freeCmds.push_back({out.prevCmds[i].node, sl});
                }
                thread_local std::vector<u32> written;
                written.clear();
                for (std::size_t gi = 0; gi < out.glyphScratch.size() / 9; ++gi) {
                    const u32* w = out.glyphScratch.data() + gi * 9;
                    const u32 abs = allocRec(w[7]);
                    std::memcpy(out.arena.data() + (abs - out.arenaBase), w, kCompactGlyphWords * 4);
                    if (out.arenaSrc.size() < out.arena.size()) out.arenaSrc.resize(out.arena.size(), ~0u);
                    out.arenaSrc[abs - out.arenaBase] = w[8];
                    out.glyphScratch[gi * 9 + 7] = abs;
                    if (abs - out.arenaBase < arena0) written.push_back(abs);
                }
                out.cmdMap.resize(out.cmdScratch.size());
                for (std::size_t q = 0; q < out.cmdScratch.size(); ++q) {
                    const u32 at = allocCmd(out.cmdScratchNode[q]);
                    out.cmdMap[q] = at;
                }
                for (std::size_t q = 0; q < out.cmdScratch.size(); ++q) { // carriers referenced by new records
                    GpuCmd& g = out.cmds[out.cmdMap[q]];
                    g = out.cmdScratch[q];
                    if (out.cmdSrc.size() < out.cmds.size()) out.cmdSrc.resize(out.cmds.size(), ~0u);
                    out.cmdSrc[out.cmdMap[q]] = out.cmdScratchSrc[q];
                }
                for (std::size_t gi = 0; gi < out.glyphScratch.size() / 9; ++gi) {
                    const u32 abs = out.glyphScratch[gi * 9 + 7];
                    u32& ref = out.arena[abs - out.arenaBase + 6];
                    if (ref != ~0u) ref = out.cmdMap[ref];
                }
                const auto finalId = [&](u32 id) {
                    return (id & kCompactGlyph) ? kCompactGlyph | out.glyphScratch[std::size_t(id & ~kCompactGlyph) * 9 + 7] : out.cmdMap[id];
                };
                for (Area& a : areas) a.id = finalId(a.id);
                for (u32& sl : newSlot)
                    if (sl != ~0u) sl = finalId(sl);
                // backdrop passes of the range replace the old ones; a changed count adds / drops draw segments
                // after k0 (new headers at the end of the tile buffer)
                if (nb != oldBd) {
                    const std::size_t a = std::size_t(k0 + 1) * tileCount, b = std::size_t(k0 + 1 + oldBd) * tileCount;
                    for (std::size_t q = a; q < b; ++q) out.tileHoles += out.tileSlotCap[q];
                    out.tileHoles += oldBd * tileCount;
                    out.tileSlotOff.erase(out.tileSlotOff.begin() + std::ptrdiff_t(a), out.tileSlotOff.begin() + std::ptrdiff_t(b));
                    out.tileSlotCap.erase(out.tileSlotCap.begin() + std::ptrdiff_t(a), out.tileSlotCap.begin() + std::ptrdiff_t(b));
                    out.tileSlotOff.insert(out.tileSlotOff.begin() + std::ptrdiff_t(a), std::size_t(nb) * tileCount, 0u);
                    out.tileSlotCap.insert(out.tileSlotCap.begin() + std::ptrdiff_t(a), std::size_t(nb) * tileCount, 0u);
                    out.passHeader.erase(out.passHeader.begin() + k0 + 1, out.passHeader.begin() + k0 + 1 + oldBd);
                    for (u32 j = 0; j < nb; ++j) {
                        out.passHeader.insert(out.passHeader.begin() + k0 + 1 + j, u32(out.tiles.size()));
                        out.tiles.resize(out.tiles.size() + tileCount, 0u);
                    }
                    out.tilePasses = u32(out.passHeader.size());
                }
                out.blurPasses.erase(out.blurPasses.begin() + k0, out.blurPasses.begin() + k0 + oldBd);
                for (u32 j = 0; j < nb; ++j) {
                    GpuPass bp = segments[j].blur;
                    bp.cmd = out.cmdMap[bp.cmd];
                    out.blurPasses.insert(out.blurPasses.begin() + k0 + j, bp);
                }
                if (merge) // the touched tiles: prefix + the range's new entries covering the tile + suffix
                    for (const Span& sp : spans) {
                        thread_local std::vector<u32> merged;
                        merged.assign(rawT.begin() + sp.pre0, rawT.begin() + sp.pre1);
                        const u32 tx = sp.t % tilesX, ty = sp.t / tilesX;
                        for (std::size_t i = 0; i < ec - p; ++i) {
                            const u64 b = newBox[i];
                            if (b == ~0ull || areaOf[i] == ~0u) continue;
                            const Area& a = areas[areaOf[i]];
                            if (int(tx) < a.x0 / int(kUiTile) || int(ty) < a.y0 / int(kUiTile) ||
                                int(tx) > (a.x1 - 1) / int(kUiTile) || int(ty) > (a.y1 - 1) / int(kUiTile))
                                continue;
                            merged.push_back(a.id);
                        }
                        merged.insert(merged.end(), rawT.begin() + sp.pre1, rawT.begin() + sp.suf1);
                        buildTile(merged.data(), u32(merged.size()), sp.t);
                        placeTile(k0, sp.t);
                    }
                // per-command tables follow the range; the suffix's source indices shift by the size change
                const i64 delta = i64(ec) - i64(ep);
                if (n == m) {
                    std::copy(newSlot.begin(), newSlot.end(), out.cmdSlot.begin() + std::ptrdiff_t(p));
                    std::copy(newBox.begin(), newBox.end(), out.cmdBox.begin() + std::ptrdiff_t(p));
                    std::copy(newVol.begin(), newVol.end(), out.cmdVol.begin() + std::ptrdiff_t(p));
                } else {
                    out.cmdVol.erase(out.cmdVol.begin() + std::ptrdiff_t(p), out.cmdVol.begin() + std::ptrdiff_t(ep));
                    out.cmdVol.insert(out.cmdVol.begin() + std::ptrdiff_t(p), newVol.begin(), newVol.end());
                    out.cmdSlot.erase(out.cmdSlot.begin() + std::ptrdiff_t(p), out.cmdSlot.begin() + std::ptrdiff_t(ep));
                    out.cmdSlot.insert(out.cmdSlot.begin() + std::ptrdiff_t(p), newSlot.begin(), newSlot.end());
                    out.cmdBox.erase(out.cmdBox.begin() + std::ptrdiff_t(p), out.cmdBox.begin() + std::ptrdiff_t(ep));
                    out.cmdBox.insert(out.cmdBox.begin() + std::ptrdiff_t(p), newBox.begin(), newBox.end());
                    for (std::size_t i = ec; i < n; ++i) {
                        const u32 sl = out.cmdSlot[i];
                        if (sl == ~0u) continue;
                        if (sl & kCompactGlyph) out.arenaSrc[(sl & ~kCompactGlyph) - out.arenaBase] = u32(i);
                        else out.cmdSrc[sl] = u32(i);
                    }
                }
                { // clip runs: drop the old range's, shift the suffix's, add the new range's
                    auto& runs = out.clipRuns;
                    std::size_t w = 0, at = runs.size();
                    for (std::size_t r = 0; r < runs.size(); ++r) {
                        GpuDrawData::ClipRun cr2 = runs[r];
                        if (cr2.from > p && cr2.from <= ep) continue;
                        if (cr2.from > ep) cr2.from = u32(i64(cr2.from) + delta), at = std::min(at, w);
                        runs[w++] = cr2;
                    }
                    runs.resize(w);
                    at = std::min(at, runs.size());
                    runs.insert(runs.begin() + std::ptrdiff_t(at), newRuns.begin(), newRuns.end());
                }
                { // backdrop source indices
                    auto& bs = out.backdropSrc;
                    bs.erase(bs.begin() + k0, bs.begin() + k0 + oldBd);
                    for (std::size_t j = k0; j < bs.size(); ++j) bs[j] = u32(i64(bs[j]) + delta);
                    for (u32 j = 0; j < nb; ++j) bs.insert(bs.begin() + k0 + j, segments[j].src);
                }
                // segments spanned by backdrops in the range: rebinned from the stored boxes (no re-emission)
                if (!merge) {
                    for (u32 j = k0; j <= k0 + nb; ++j) {
                        const std::size_t from = j == 0 ? 0 : std::size_t(out.backdropSrc[j - 1]) + 1;
                        const std::size_t to = j < out.backdropSrc.size() ? out.backdropSrc[j] : n;
                        areas.clear();
                        for (std::size_t i = from; i < to; ++i) {
                            const u64 b = out.cmdBox[i];
                            if (b == ~0ull || out.cmdSlot[i] == ~0u) continue;
                            areas.push_back({int(b & 0xFFFF), int((b >> 16) & 0xFFFF), int((b >> 32) & 0xFFFF), int(b >> 48), out.cmdSlot[i]});
                        }
                        for (u32 t = 0; t < tileCount; ++t) out.tileLive -= std::min(out.tileLive, out.tileSlotCap[std::size_t(j) * tileCount + t]);
                        binAreas(areas.data(), areas.size(), j);
                    }
                }
                // images mirror the arena: rewritten records, then whatever the range appended (records, words)
                out.arenaHoles += u32(out.arena.size()) - arena0; // re-slotted by the next full pack
                for (u32 abs : written) std::memcpy(out.images.data() + abs, rec(abs), kCompactGlyphWords * 4);
                out.images.resize(std::size_t(out.arenaBase) + arena0); // (the tail may have been given back)
                out.images.insert(out.images.end(), out.arena.begin() + arena0, out.arena.end());
                rebuildPasses();
                out.packedImageWords = u32(out.images.size());
                syncPrev();
                out.incremental = true;
                return;
            }
            V = &out.images;
            vb = 0;
            clipFloor = 0;
        }
    }

    // ---- full pack ----
    syncPrev();
    out.freeRecs.clear();
    out.freeCmds.clear();
    // slot arenas: compacted when the holes left by moved / vanished nodes outweigh the live slots
    if ((out.arenaHoles > out.arenaLive && out.arenaHoles > 4096) || out.fillSlots.size() > 8192) out.resetArena();
    if (out.cmdHoles > out.cmdLive && out.cmdHoles > 256) {
        out.cmds.clear();
        out.cmdSlots.clear();
        out.cmdHoles = out.cmdLive = 0;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
    ++out.frameStamp;
    out.cmdSlot.assign(n, ~0u);
    out.cmdBox.assign(n, ~0ull);
    out.cmdVol.assign(n, 0ull); // volatile words of a full pack live in images (rebuilt every full pack)
    out.clipRuns.clear();
    out.glyphScratch.clear();
    out.cmdScratch.clear();
    out.cmdScratchNode.clear();
    out.cmdScratchSrc.clear();
    if (attempt == 0) { // keep the last images: outgrown glyph slots put back their old (dead) words from it
        out.images.swap(out.prevImages);
        out.prevArenaBase = out.arenaBase;
    } else out.prevImages.clear();
    out.images.clear();
    out.passes.clear();
    out.atlasSize = atlasS;
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

    {
        const u32 sx0 = u32(std::max(0.f, std::floor(scissor.x))), sy0 = u32(std::max(0.f, std::floor(scissor.y)));
        const u32 sx1 = std::min(width, u32(std::max(0.f, std::ceil(scissor.right()))));
        const u32 sy1 = std::min(height, u32(std::max(0.f, std::ceil(scissor.bottom()))));
        out.push.origin[0] = sx0;
        out.push.origin[1] = sy0;
        out.push.size[0] = sx1 > sx0 ? sx1 - sx0 : 0;
        out.push.size[1] = sy1 > sy0 ? sy1 - sy0 : 0;
    }
    depth = 0;
    stack[0] = {scissor, 0.f, 0u};
    clipRef = ~0u;
    lastClip = {};
    lastFill = {};
    fillAt = ~0u;

    // commands + their pixel areas; a Backdrop closes the current draw segment (its tile table is built from the
    // segment's commands) and becomes a blur pass over what was drawn so far
    areas.clear();
    areas.reserve(n); // one growth step per frame at most (text pages are 10^4+ glyph quads)
    out.cmdScratch.reserve(n + 1);
    out.glyphScratch.reserve(n * 9);
    segments.clear();
    u32 clipRec = 0;
    clipVer = 1;
    gTouched.clear(), ovArea.clear(), ovOld.clear(), refLocal.clear();
    dSl = nullptr, glyphTotal = 0;
    out.arenaSrc.resize(out.arena.size(), ~0u);
    directGlyphs = true;
    for (std::size_t ci = 0; ci < n; ++ci) {
        if (clipRec != clipVer) { // clip runs: the clip in force from here on (incremental packs start from it)
            out.clipRuns.push_back({u32(ci), stack[depth].r, stack[depth].radius, stack[depth].pathList, u32(depth)});
            clipRec = clipVer;
        }
        step(ci, out.cmdSlot[ci], out.cmdBox[ci]);
    }
    directGlyphs = false;
    // command slot pass: GpuCmds of each source node keep their cmds slot while they fit
    {
        const std::size_t nc = out.cmdScratch.size();
        out.cmdSlotOf.resize(nc);
        out.cmdMap.resize(nc);
        out.touched.clear();
        u32 lastNode = ~0u; // runs of one node: one map lookup (node values are arbitrary, so the first is real)
        GpuDrawData::GlyphSlot* lastSl = nullptr;
        for (std::size_t q = 0; q < nc; ++q) {
            if (!lastSl || out.cmdScratchNode[q] != lastNode) lastNode = out.cmdScratchNode[q], lastSl = &out.cmdSlots[lastNode];
            GpuDrawData::GlyphSlot& sl = *lastSl;
            if (sl.stamp != out.frameStamp) {
                sl.stamp = out.frameStamp;
                sl.count = sl.cursor = 0;
                out.touched.push_back(out.cmdScratchNode[q]);
            }
            ++sl.count;
            out.cmdSlotOf[q] = &sl;
        }
        for (auto it = out.cmdSlots.begin(); it != out.cmdSlots.end();) {
            if (it->second.stamp != out.frameStamp) {
                out.cmdHoles += it->second.cap;
                it = out.cmdSlots.erase(it);
            } else ++it;
        }
        out.cmdLive = 0;
        for (u32 node : out.touched) {
            GpuDrawData::GlyphSlot& sl = out.cmdSlots[node];
            if (sl.count > sl.cap) {
                out.cmdHoles += sl.cap;
                sl.cap = sl.count + sl.count / 8;
                sl.off = u32(out.cmds.size());
                out.cmds.resize(out.cmds.size() + sl.cap);
            }
            out.cmdLive += sl.cap;
        }
        out.cmdSrc.resize(out.cmds.size(), ~0u);
        for (std::size_t q = 0; q < nc; ++q) {
            GpuDrawData::GlyphSlot& sl = *out.cmdSlotOf[q];
            const u32 at = sl.off + sl.cursor++;
            out.cmds[at] = out.cmdScratch[q];
            out.cmdSrc[at] = out.cmdScratchSrc[q];
            out.cmdMap[q] = at;
        }
        for (Area& a : areas)
            if (!(a.id & kCompactGlyph)) a.id = out.cmdMap[a.id];
        for (u32& sl : out.cmdSlot)
            if (sl != ~0u && !(sl & kCompactGlyph)) sl = out.cmdMap[sl];
        for (auto& sg : segments) sg.blur.cmd = out.cmdMap[sg.blur.cmd];
        for (std::size_t g = 0; g < out.glyphScratch.size(); g += 9)
            if (out.glyphScratch[g + 6] != ~0u) out.glyphScratch[g + 6] = out.cmdMap[out.glyphScratch[g + 6]];
        for (std::size_t q = 0; q < refLocal.size(); q += 2) // not in a slot its node outgrew (restored hole words)
            if (out.glyphSlots[refLocal[q + 1]].cursor != ~0u) out.arena[refLocal[q] + 6] = out.cmdMap[out.arena[refLocal[q] + 6]];
    }
    // glyph slot pass: each source node keeps its arena slot while its glyphs fit, so added / removed / moved commands
    // of one node rewrite only that slot; other nodes' records keep their words (the upload page diff skips them).
    // Records that fit were written in place by emit; outgrown nodes get a new slot at the arena end with headroom
    // (in first-appearance order) and take their records from the overflow scratch
    {
        for (auto it = out.glyphSlots.begin(); it != out.glyphSlots.end();) { // vanished nodes free their slots
            if (it->second.stamp != out.frameStamp) {
                out.arenaHoles += it->second.cap * kCompactGlyphWords;
                it = out.glyphSlots.erase(it);
            } else ++it;
        }
        out.arenaLive = 0;
        for (u32 node : gTouched) {
            GpuDrawData::GlyphSlot& sl = out.glyphSlots[node];
            if (sl.count > sl.cap) { // grow: the old slot becomes a hole
                out.arenaHoles += sl.cap * kCompactGlyphWords;
                sl.cap = sl.count + sl.count / 8;
                sl.off = u32(out.arena.size());
                sl.cursor = 0;
                out.arena.resize(out.arena.size() + std::size_t(sl.cap) * kCompactGlyphWords, 0u);
            }
            out.arenaLive += sl.cap * kCompactGlyphWords;
        }
        out.arenaSrc.resize(out.arena.size(), ~0u);
        const std::size_t ng = out.glyphScratch.size() / 9;
        thread_local std::vector<std::pair<u32, u32>> moved; // old -> new absolute offset of outgrown records
        moved.clear();
        u32 lastNode = ~0u;
        GpuDrawData::GlyphSlot* lastSl = nullptr;
        for (std::size_t g = 0; g < ng; ++g) {
            const u32* w = out.glyphScratch.data() + g * 9;
            if (!lastSl || w[7] != lastNode) lastNode = w[7], lastSl = &out.glyphSlots[lastNode];
            const u32 local = lastSl->off + lastSl->cursor++ * kCompactGlyphWords;
            std::memcpy(out.arena.data() + local, w, kCompactGlyphWords * 4);
            out.arenaSrc[local] = w[8];
            const u32 id = kCompactGlyph | (out.arenaBase + local);
            out.cmdSlot[w[8]] = id;
            if (ovArea[g] != ~0u) areas[ovArea[g]].id = id;
            else moved.push_back({ovOld[g], out.arenaBase + local});
        }
        if (!moved.empty()) { // ids handed out before their node outgrew its slot
            std::sort(moved.begin(), moved.end());
            for (Area& a : areas)
                if ((a.id & kGlyphRun) == kCompactGlyph) {
                    const u32 abs = a.id & ~kCompactGlyph;
                    const auto it = std::lower_bound(moved.begin(), moved.end(), std::pair<u32, u32>{abs, 0u});
                    if (it != moved.end() && it->first == abs) a.id = kCompactGlyph | it->second;
                }
        }
        if (glyphTotal == 0) out.resetArena(); // no glyphs: drop the arena (nothing references it)
    }
    // volatile words (images, edges, stops, clip lists) must end before the arena; else move the arena up
    if (!out.arena.empty() && out.images.size() > out.arenaBase) {
        out.arenaBase = u32(out.images.size() + out.images.size() / 8 + 256);
        out.resetArena();
        continue; // repack once against the new base
    }
    // tile binning: one draw pass per segment, always segments + 1 in the layout (an empty trailing one is left out
    // of `passes`); slots per (pass, tile) keep their place while their content fits
    {
        const u32 passCount = u32(segments.size()) + 1u;
        if (passCount != out.tilePasses || tileCount != out.tileCountPrev ||
            (out.tileHoles > out.tileLive && out.tileHoles > 4096)) {
            out.tiles.assign(1 + std::size_t(passCount) * tileCount, 0u);
            out.tileSlotOff.assign(std::size_t(passCount) * tileCount, 0u);
            out.tileSlotCap.assign(std::size_t(passCount) * tileCount, 0u);
            out.passHeader.resize(passCount);
            for (u32 k = 0; k < passCount; ++k) out.passHeader[k] = 1u + k * tileCount;
            out.tilePasses = passCount;
            out.tileCountPrev = tileCount;
            out.tileHoles = 0;
        }
        out.tileLive = 0;
        out.backdropSrc.clear();
        out.blurPasses.clear();
        u32 segBegin = 0, pass = 0;
        for (const auto& sg : segments) {
            binAreas(areas.data() + segBegin, sg.end - segBegin, pass++);
            out.blurPasses.push_back(sg.blur);
            out.backdropSrc.push_back(sg.src);
            segBegin = sg.end;
        }
        binAreas(areas.data() + segBegin, areas.size() - segBegin, pass);
        rebuildPasses();
    }
    if (!out.arena.empty()) { // no glyphs: no arena, no padding
        out.images.resize(out.arenaBase, 0u);
        out.images.insert(out.images.end(), out.arena.begin(), out.arena.end());
    }
    if (out.cmds.empty()) out.cmds.push_back(GpuCmd{}); // non-empty buffer; no tile references it
    out.packedImageWords = u32(out.images.size());
    break;
    }
}

} // namespace fuse::ui::render
