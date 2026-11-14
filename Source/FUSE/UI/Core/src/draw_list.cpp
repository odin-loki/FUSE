#include <algorithm>
#include "fuse/ui/draw_list.hpp"

#include "draw_internal.hpp"

#include <cmath>
#include <cstring>

namespace fuse::ui {

void DrawList::rect(const Rect& r, Color fill, f32 radius) { roundedRect(r, fill, {radius, radius, radius, radius}); }

void DrawList::roundedRect(const Rect& r, Color fill, const std::array<f32, 4>& radii, Color border, f32 bw) {
    DrawCmd c;
    c.op = DrawOp::Rect;
    c.rect = r;
    c.fill = fill;
    c.border = border;
    c.borderWidth = bw;
    c.radius = radii;
    m_cmds.push_back(c);
}

void DrawList::gradientRect(const Rect& r, Color from, Color to, bool vertical, const std::array<f32, 4>& radii) {
    DrawCmd c;
    c.op = DrawOp::Rect;
    c.rect = r;
    c.fill = from;
    c.fill2 = to;
    c.gradient = vertical ? 1 : 2;
    c.radius = radii;
    m_cmds.push_back(c);
}

void DrawList::shadow(const Rect& r, Color col, f32 blur, f32 radius) {
    DrawCmd c;
    c.op = DrawOp::Shadow;
    c.rect = r;
    c.fill = col;
    c.blur = blur;
    c.radius = {radius, radius, radius, radius};
    m_cmds.push_back(c);
}

void DrawList::translate(Vec2 d) {
    for (DrawCmd& c : m_cmds) {
        c.rect.x += d.x;
        c.rect.y += d.y;
        if (c.op == DrawOp::Line) { c.rect.w += d.x; c.rect.h += d.y; }
    }
    if (m_vector) translateSide(d);
}

void DrawList::scale(f32 s) {
    if (s == 1.f) return;
    const bool snap = s != std::floor(s);
    for (DrawCmd& c : m_cmds) {
        if (c.op == DrawOp::PopClip) continue;
        if (c.op == DrawOp::Glyph) {
            if (c.devicePx) { c.devicePx = 0; continue; }
            if (c.gradient == kGlyphMsdf) { // a distance field is resolution independent: the quad scales
                c.rect = {c.rect.x * s, c.rect.y * s, c.rect.w * s, c.rect.h * s};
                continue;
            }
            c.rect.x = std::round(c.rect.x * s);
            c.rect.y = std::round(c.rect.y * s);
            continue;
        }
        const bool box = c.op == DrawOp::Rect || c.op == DrawOp::Image || c.op == DrawOp::Backdrop ||
                         c.op == DrawOp::PushClip || c.op == DrawOp::NinePatch;
        if (snap && box) {
            const f32 x0 = std::round(c.rect.x * s), y0 = std::round(c.rect.y * s);
            f32 x1 = std::round(c.rect.right() * s), y1 = std::round(c.rect.bottom() * s);
            if (c.rect.w > 0.f && x1 <= x0) x1 = x0 + 1.f;
            if (c.rect.h > 0.f && y1 <= y0) y1 = y0 + 1.f;
            c.rect = {x0, y0, x1 - x0, y1 - y0};
            if (c.borderWidth > 0.f) c.borderWidth = std::max(1.f, std::round(c.borderWidth * s));
        } else { // Line: (x0, y0, x1, y1) scales the same way
            c.rect.x *= s;
            c.rect.y *= s;
            c.rect.w *= s;
            c.rect.h *= s;
            c.borderWidth *= s;
        }
        for (f32& r : c.radius) r *= s;
        c.blur *= s;
    }
    if (m_vector) scaleSide(s, snap);
}

void DrawList::backdrop(const Rect& r, f32 blur, const std::array<f32, 4>& radii, Color tint) {
    DrawCmd c;
    c.op = DrawOp::Backdrop;
    c.rect = r;
    c.blur = blur;
    c.radius = radii;
    c.fill = tint;
    m_cmds.push_back(c);
}

void DrawList::line(Vec2 a, Vec2 b, Color col, f32 width) {
    DrawCmd c;
    c.op = DrawOp::Line;
    c.rect = {a.x, a.y, b.x, b.y};
    c.fill = col;
    c.borderWidth = width;
    m_cmds.push_back(c);
}

void DrawList::pushClip(const Rect& r, f32 radius) {
    DrawCmd c;
    c.op = DrawOp::PushClip;
    c.rect = r;
    c.radius = {radius, radius, radius, radius};
    m_cmds.push_back(c);
}

void DrawList::popClip() {
    DrawCmd c;
    c.op = DrawOp::PopClip;
    m_cmds.push_back(c);
}

namespace {
inline u64 fnv(u64 h, const void* p, std::size_t n) {
    const auto* b = static_cast<const u8*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
} // namespace

u64 DrawList::hash() const {
    u64 h = 1469598103934665603ull;
    for (const auto& c : m_cmds) {
        h = fnv(h, &c.op, 1);
        h = fnv(h, &c.gradient, 1);
        h = fnv(h, &c.u, 8);
        h = fnv(h, &c.image, 4);
        h = fnv(h, &c.rect, sizeof(Rect));
        h = fnv(h, &c.fill, sizeof(Color) * 3);
        h = fnv(h, &c.borderWidth, 4);
        h = fnv(h, c.radius.data(), 16);
        h = fnv(h, &c.blur, 4);
    }
    if (m_vector) { // side records of the vector ops (lists without them hash as before)
        for (const auto& c : m_cmds) h = fnv(h, &c.data, 4);
        h = fnv(h, m_verbs.data(), m_verbs.size());
        h = fnv(h, m_points.data(), m_points.size() * sizeof(Vec2));
        h = fnv(h, m_stops.data(), m_stops.size() * sizeof(GradientStop));
        for (const DrawPath& p : m_paths) {
            h = fnv(h, &p.firstVerb, 16);
            h = fnv(h, &p.paint, 4);
            const u8 flags[4]{u8(p.rule), u8(p.stroked), u8(p.stroke.cap), u8(p.stroke.join)};
            h = fnv(h, flags, 4);
            if (p.stroked) {
                const f32 v[3]{p.stroke.width, p.stroke.miterLimit, p.stroke.dashOffset};
                h = fnv(h, v, 12);
                h = fnv(h, p.stroke.dashes.data(), std::size_t(p.stroke.dashCount) * 4);
            }
        }
        for (const DrawPaint& p : m_paints) {
            const u32 v[4]{u32(p.kind), u32(p.spread), p.firstStop, p.stopCount};
            h = fnv(h, v, 16);
            h = fnv(h, &p.opacity, 4);
            h = fnv(h, &p.toUnit, sizeof(Affine));
        }
        for (const DrawPatch& p : m_patches) {
            h = fnv(h, &p.src, sizeof(Rect) + 2 * sizeof(Edges));
            const u8 sm = p.smooth;
            h = fnv(h, &sm, 1);
        }
    }
    return h;
}

u64 Image::hash() const { return fnv(1469598103934665603ull, pixels.data(), pixels.size() * 4); }

u32 packPremultiplied(Color c) {
    auto q = [](f32 v) { return u32(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    const f32 a = std::clamp(c.a, 0.f, 1.f);
    return q(c.r * a) | (q(c.g * a) << 8) | (q(c.b * a) << 16) | (q(a) << 24);
}

u32 CpuRasterizer::addImage(const Image* img) {
    m_images.push_back(img);
    return u32(m_images.size() - 1);
}

using namespace detail;

namespace {
// Backdrop blur, the reference: three box passes per axis (~Gaussian, sigma ~ blur / 2) over the pixels under the
// rounded rect, sampled from a margin of `blur` around it (clamped to the target), then mixed in by coverage.
void backdropBlur(const DrawCmd& c, const Rect& clipR, f32 clipRadius, const Surface& target) {
    const Rect area = c.rect.intersect(clipR).intersect(target.rect());
    if (area.empty()) return;
    const int r = std::max(1, int(std::lround(c.blur / 3.f)));
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
    const int m = 3 * r;
    const int sx0 = std::max(target.ox, x0 - m), sy0 = std::max(target.oy, y0 - m);
    const int sx1 = std::min(target.ox + target.w, x1 + m), sy1 = std::min(target.oy + target.h, y1 + m);
    const int w = sx1 - sx0, h = sy1 - sy0;
    std::vector<f32> buf(std::size_t(w) * std::size_t(h) * 4), tmp(buf.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 p = target.row(sy0 + y)[sx0 + x - target.ox];
            for (int k = 0; k < 4; ++k) buf[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + std::size_t(k)] = f32((p >> (8 * k)) & 0xFFu);
        }
    auto pass = [&](bool vertical) {
        const int n = vertical ? h : w, lines = vertical ? w : h;
        const f32 inv = 1.f / f32(2 * r + 1);
        for (int l = 0; l < lines; ++l)
            for (int k = 0; k < 4; ++k) {
                auto at = [&](int i) -> f32& {
                    i = std::clamp(i, 0, n - 1);
                    const std::size_t px = vertical ? std::size_t(i) * std::size_t(w) + std::size_t(l) : std::size_t(l) * std::size_t(w) + std::size_t(i);
                    return buf[px * 4 + std::size_t(k)];
                };
                f32 acc = 0.f;
                for (int i = -r; i <= r; ++i) acc += at(i);
                for (int i = 0; i < n; ++i) {
                    const std::size_t px = vertical ? std::size_t(i) * std::size_t(w) + std::size_t(l) : std::size_t(l) * std::size_t(w) + std::size_t(i);
                    tmp[px * 4 + std::size_t(k)] = acc * inv;
                    acc += at(i + r + 1) - at(i - r);
                }
            }
        buf.swap(tmp);
    };
    for (int it = 0; it < 3; ++it) { pass(false); pass(true); }
    const std::array<f32, 4> clipRad{clipRadius, clipRadius, clipRadius, clipRadius};
    const Px tint = premul(c.fill, 1.f);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
            f32 cov = std::clamp(0.5f - sdRoundRect(px, py, c.rect, c.radius), 0.f, 1.f);
            if (clipRadius > 0.f) cov *= std::clamp(0.5f - sdRoundRect(px, py, clipR, clipRad), 0.f, 1.f);
            if (cov <= 0.f) continue;
            u32& d = target.row(y)[x - target.ox];
            const f32* b = &buf[(std::size_t(y - sy0) * std::size_t(w) + std::size_t(x - sx0)) * 4];
            u32 o = 0;
            for (int k = 0; k < 4; ++k) {
                const f32 dv = f32((d >> (8 * k)) & 0xFFu);
                o |= u32(std::clamp(std::lround(dv + (b[k] - dv) * cov), 0l, 255l)) << (8 * k);
            }
            d = o;
            if (c.fill.a > 0.f) blend(d, premul(c.fill, cov));
        }
    (void)tint;
}
} // namespace

void CpuRasterizer::render(const DrawList& list, Image& target, Rect scissor) const {
    if (target.width <= 0 || target.height <= 0) return;
    Rect full{0.f, 0.f, f32(target.width), f32(target.height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    const RasterEnv env{{m_atlas, m_atlasSize, m_images.data(), u32(m_images.size())}, target.width};
    renderRange(env, list, 0, list.size(), {target.pixels.data(), 0, 0, target.width, target.height}, scissor, 0, false);
}

void rasterizeCommands(const DrawList& list, std::size_t first, std::size_t count, const Rect& area, int rootWidth,
                       const RasterSources& src, Image& out, bool groupOpacityOne) {
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int w = std::max(0, int(std::ceil(area.right())) - x0), h = std::max(0, int(std::ceil(area.bottom())) - y0);
    out.resize(w, h, 0);
    if (w == 0 || h == 0) return;
    const std::size_t last = std::min(list.size(), first + count);
    renderRange({src, rootWidth}, list, first, last, {out.pixels.data(), x0, y0, w, h}, {f32(x0), f32(y0), f32(w), f32(h)}, 0,
                groupOpacityOne);
}

std::size_t matchingPop(const DrawList& list, std::size_t i) {
    const auto& cmds = list.cmds();
    int depth = 0;
    for (std::size_t j = i; j < cmds.size(); ++j) {
        const DrawOp op = cmds[j].op;
        if (op == DrawOp::PushClip || isLayerGroup(op)) ++depth;
        else if (op == DrawOp::PopClip && --depth <= 0) return j;
    }
    return cmds.size();
}

namespace {
// ---- colour / MSDF glyphs (atlas colour page: side size / 2, RGBA8, after the coverage page) ----
inline const u8* colorTexel(const u8* atlas, int size, int x, int y) {
    const int cs = size / 2;
    return atlas + std::size_t(size) * std::size_t(size) + (std::size_t(y) * std::size_t(cs) + std::size_t(x)) * 4;
}

// MSDF coverage at pixel centre (px, py): the glyph's texels (tw x th at u, v) stretched over `rect`, sampled
// bilinearly (edge-clamped), median of RGB, scaled by the screen-space distance range (`blur` = range in texels).
inline f32 msdfCoverage(const DrawCmd& c, const u8* atlas, int size, f32 px, f32 py) {
    if (c.tw == 0 || c.th == 0 || c.rect.w <= 0.f || c.rect.h <= 0.f) return 0.f;
    const f32 tw = f32(c.tw), th = f32(c.th);
    const f32 sx = (px - c.rect.x) * tw / c.rect.w - 0.5f;
    const f32 sy = (py - c.rect.y) * th / c.rect.h - 0.5f;
    const f32 fx0 = std::floor(sx), fy0 = std::floor(sy);
    const f32 fx = sx - fx0, fy = sy - fy0;
    const int x0 = std::clamp(int(fx0), 0, c.tw - 1), x1 = std::clamp(int(fx0) + 1, 0, c.tw - 1);
    const int y0 = std::clamp(int(fy0), 0, c.th - 1), y1 = std::clamp(int(fy0) + 1, 0, c.th - 1);
    const u8* t00 = colorTexel(atlas, size, c.u + x0, c.v + y0);
    const u8* t10 = colorTexel(atlas, size, c.u + x1, c.v + y0);
    const u8* t01 = colorTexel(atlas, size, c.u + x0, c.v + y1);
    const u8* t11 = colorTexel(atlas, size, c.u + x1, c.v + y1);
    f32 ch[3];
    for (int k = 0; k < 3; ++k) {
        const f32 a00 = f32(t00[k]) / 255.f, a10 = f32(t10[k]) / 255.f, a01 = f32(t01[k]) / 255.f, a11 = f32(t11[k]) / 255.f;
        const f32 top = a00 + (a10 - a00) * fx;
        const f32 bot = a01 + (a11 - a01) * fx;
        ch[k] = top + (bot - top) * fy;
    }
    const f32 med = std::max(std::min(ch[0], ch[1]), std::min(std::max(ch[0], ch[1]), ch[2]));
    const f32 spr = c.blur * c.rect.w / tw;
    return std::clamp(spr * (med - 0.5f) + 0.5f, 0.f, 1.f);
}
} // namespace

void detail::renderRange(const RasterEnv& env, const DrawList& list, std::size_t first, std::size_t last, const Surface& target,
                         Rect scissor, int level, bool groupOpacityOne) {
    const u8* const atlas = env.src.atlas;
    const int atlasSize = env.src.atlasSize;
    const std::span<const Image* const> images{env.src.images, env.src.images ? env.src.imageCount : 0u};
    const int ox = target.ox;
    Clip stack[32];
    int depth = 0;
    stack[0] = {scissor, 0.f};
    const auto& cmds = list.cmds();
    for (std::size_t ci = first; ci < last; ++ci) {
        const DrawCmd& c = cmds[ci];
        const Clip& clip = stack[depth];
        switch (c.op) {
        case DrawOp::PushClip:
            if (depth < 31) {
                ++depth;
                stack[depth] = {stack[depth - 1].r.intersect(c.rect), c.radius[0]};
                if (stack[depth - 1].radius > 0.f && c.radius[0] <= 0.f) stack[depth].radius = 0.f;
            }
            continue;
        case DrawOp::PopClip:
            if (depth > 0) --depth;
            continue;
        case DrawOp::Backdrop: backdropBlur(c, clip.r, clip.radius, target); continue;
        case DrawOp::Path: drawPathCmd(env, list, c, clip, target); continue;
        case DrawOp::NinePatch: drawPatchCmd(env, list, c, clip, target); continue;
        case DrawOp::PushClipPath:
        case DrawOp::PushLayer: // a group: rendered to a layer, composited once; resumes after its PopClip
            ci = drawGroup(env, list, ci, last, clip, target, level, groupOpacityOne && ci == first);
            continue;
        case DrawOp::PushTransform:
        case DrawOp::PushFilter:
        case DrawOp::PushMask: ci = drawFxGroup(env, list, ci, last, clip, target, level); continue;
        default: break;
        }
        Rect bounds = c.rect;
        if (c.op == DrawOp::Shadow) bounds = {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
        if (c.op == DrawOp::Line) {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            bounds = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw,
                      std::abs(c.rect.w - c.rect.x) + 2 * hw, std::abs(c.rect.h - c.rect.y) + 2 * hw};
        }
        const Rect area = bounds.intersect(clip.r);
        if (area.empty()) continue;
        const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
        const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
        if (c.op == DrawOp::Image && clip.radius <= 0.f) {
            // Images under a rectangular clip (the editor viewport, icons): the per-pixel loop below computed the
            // source column with a float divide per pixel. Same texel mapping and blend, the column of each device x
            // is computed once per command (in chunks, no allocation) and the row once per y.
            if (c.image >= images.size() || !images[c.image]) continue;
            const Image& img = *images[c.image];
            const f32 k = c.fill.a > 0.f ? c.fill.a : 1.f;
            constexpr int kChunk = 256;
            int sxs[kChunk];
            for (int cx0 = x0; cx0 < x1; cx0 += kChunk) {
                const int cx1 = std::min(x1, cx0 + kChunk);
                for (int x = cx0; x < cx1; ++x) {
                    const f32 px = f32(x) + 0.5f;
                    sxs[x - cx0] = std::clamp(int((px - c.rect.x) / c.rect.w * f32(img.width)), 0, img.width - 1);
                }
                for (int y = y0; y < y1; ++y) {
                    const f32 py = f32(y) + 0.5f;
                    const int sy = std::clamp(int((py - c.rect.y) / c.rect.h * f32(img.height)), 0, img.height - 1);
                    const u32* src = img.pixels.data() + std::size_t(sy) * std::size_t(img.width);
                    u32* row = target.row(y) - ox; // indexed by device x
                    for (int x = cx0; x < cx1; ++x) {
                        const u32 t = src[sxs[x - cx0]];
                        if (k >= 1.f && (t >> 24) == 255u) { row[x] = t; continue; } // opaque texel: blend() would store it
                        blend(row[x], {f32(t & 0xFF) / 255.f * k, f32((t >> 8) & 0xFF) / 255.f * k,
                                       f32((t >> 16) & 0xFF) / 255.f * k, f32(t >> 24) / 255.f * k});
                    }
                }
            }
            continue;
        }
        const Rect clipOuter = clip.r; // the rounded clip uses the top clip rect's own shape
        const std::array<f32, 4> clipRad{clip.radius, clip.radius, clip.radius, clip.radius};
        // fast path: the interior of an opaque, ungraded rect (pixel centres at least max radius + border + 0.5 inside
        // every edge have full coverage) is a plain store of the packed fill (same bytes blend() would produce)
        int ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;
        u32 solid = 0;
        if (c.op == DrawOp::Rect && !c.gradient && c.fill.a >= 1.f && clip.radius <= 0.f) {
            const f32 m = std::max({c.radius[0], c.radius[1], c.radius[2], c.radius[3], 0.f}) +
                          (c.borderWidth > 0.f && c.border.a > 0.f ? c.borderWidth : 0.f) + 0.5f;
            ix0 = int(std::ceil(c.rect.x + m - 0.5f)), ix1 = int(std::floor(c.rect.right() - m - 0.5f)) + 1;
            iy0 = int(std::ceil(c.rect.y + m - 0.5f)), iy1 = int(std::floor(c.rect.bottom() - m - 0.5f)) + 1;
            u32 probe = 0;
            blend(probe, premul(c.fill, 1.f));
            solid = probe;
        }
        for (int y = y0; y < y1; ++y) {
            u32* row = target.row(y) - ox; // indexed by device x
            // the solid interior span of this row: one fill, the per-pixel loop covers only the edges around it
            const int sx0 = std::max(ix0, x0), sx1 = std::min(ix1, x1);
            const bool solidRow = y >= iy0 && y < iy1 && sx0 < sx1;
            if (solidRow) std::fill(row + sx0, row + sx1, solid);
            for (int x = x0; x < x1; ++x) {
                if (solidRow && x == sx0) {
                    x = sx1 - 1; // already stored
                    continue;
                }
                const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
                f32 clipCov = 1.f;
                if (clip.radius > 0.f) clipCov = std::clamp(0.5f - sdRoundRect(px, py, clipOuter, clipRad), 0.f, 1.f);
                if (clipCov <= 0.f) continue;
                switch (c.op) {
                case DrawOp::Rect: {
                    const f32 d = sdRoundRect(px, py, c.rect, c.radius);
                    const f32 outer = std::clamp(0.5f - d, 0.f, 1.f) * clipCov;
                    if (outer <= 0.f) break;
                    Color fill = c.fill;
                    if (c.gradient) {
                        const f32 t = c.gradient == 1 ? std::clamp((py - c.rect.y) / std::max(c.rect.h, 1.f), 0.f, 1.f)
                                                      : std::clamp((px - c.rect.x) / std::max(c.rect.w, 1.f), 0.f, 1.f);
                        fill = {fill.r + (c.fill2.r - fill.r) * t, fill.g + (c.fill2.g - fill.g) * t,
                                fill.b + (c.fill2.b - fill.b) * t, fill.a + (c.fill2.a - fill.a) * t};
                    }
                    if (c.borderWidth > 0.f && c.border.a > 0.f) {
                        const f32 inner = std::clamp(0.5f - (d + c.borderWidth), 0.f, 1.f) * clipCov;
                        const Px pf = premul(fill, inner);
                        const Px pb = premul(c.border, outer - inner);
                        blend(row[x], {pf.r + pb.r, pf.g + pb.g, pf.b + pb.b, pf.a + pb.a});
                    } else {
                        blend(row[x], premul(fill, outer));
                    }
                    break;
                }
                case DrawOp::Shadow: {
                    const f32 d = sdRoundRect(px, py, c.rect, c.radius);
                    const f32 b = std::max(c.blur, 0.5f);
                    const f32 t = std::clamp((d + b) / (2.f * b), 0.f, 1.f);
                    const f32 cov = (1.f - t * t * (3.f - 2.f * t)) * clipCov;
                    blend(row[x], premul(c.fill, cov));
                    break;
                }
                case DrawOp::Glyph: {
                    if (!atlas) break;
                    if (c.gradient == kGlyphMsdf) {
                        const f32 cov = msdfCoverage(c, atlas, atlasSize, px, py) * clipCov;
                        if (cov > 0.f) blend(row[x], premul(c.fill, cov));
                        break;
                    }
                    const int gx = x - int(c.rect.x), gy = y - int(c.rect.y);
                    if (c.gradient == kGlyphColor) {
                        if (gx < 0 || gy < 0 || gx >= c.tw || gy >= c.th) break;
                        const u8* t = colorTexel(atlas, atlasSize, c.u + gx, c.v + gy);
                        const f32 k = std::clamp(c.fill.a, 0.f, 1.f) * clipCov;
                        if (t[3]) blend(row[x], {f32(t[0]) / 255.f * k, f32(t[1]) / 255.f * k, f32(t[2]) / 255.f * k, f32(t[3]) / 255.f * k});
                        break;
                    }
                    if (gx < 0 || gy < 0 || gx >= c.tw || gy >= c.th) break;
                    const u8 a = atlas[std::size_t(c.v + gy) * std::size_t(atlasSize) + std::size_t(c.u + gx)];
                    if (a) blend(row[x], premul(c.fill, f32(a) / 255.f * clipCov));
                    break;
                }
                case DrawOp::Image: {
                    if (c.image >= images.size() || !images[c.image]) break;
                    const Image& img = *images[c.image];
                    const int sx = std::clamp(int((px - c.rect.x) / c.rect.w * f32(img.width)), 0, img.width - 1);
                    const int sy = std::clamp(int((py - c.rect.y) / c.rect.h * f32(img.height)), 0, img.height - 1);
                    const u32 s = img.at(sx, sy);
                    const f32 k = clipCov * (c.fill.a > 0.f ? c.fill.a : 1.f);
                    if (k >= 1.f && (s >> 24) == 255u) { row[x] = s; break; } // opaque texel: blend() would store it
                    blend(row[x], {f32(s & 0xFF) / 255.f * k, f32((s >> 8) & 0xFF) / 255.f * k,
                                   f32((s >> 16) & 0xFF) / 255.f * k, f32(s >> 24) / 255.f * k});
                    break;
                }
                case DrawOp::Line: {
                    const f32 ax = c.rect.x, ay = c.rect.y, bx = c.rect.w, by = c.rect.h;
                    const f32 vx = bx - ax, vy = by - ay;
                    const f32 len2 = std::max(vx * vx + vy * vy, 1e-6f);
                    const f32 t = std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.f, 1.f);
                    const f32 ex = px - (ax + vx * t), ey = py - (ay + vy * t);
                    const f32 d = std::sqrt(ex * ex + ey * ey) - c.borderWidth * 0.5f;
                    const f32 cov = std::clamp(0.5f - d, 0.f, 1.f) * clipCov;
                    if (cov > 0.f) blend(row[x], premul(c.fill, cov));
                    break;
                }
                default: break;
                }
            }
        }
    }
}

} // namespace fuse::ui
