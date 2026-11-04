// Frozen copy of the CPU reference rasteriser as it was before the span / SIMD / banded-thread rewrite (draw_list.cpp
// at 5dcee19e1). fuse_ui_cpu_raster_bench renders its scenes and a fuzzed command mix through both and requires equal
// bytes: the optimised CpuRasterizer must stay bit-identical to this.
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fuse::ui::test::ref {

namespace {
struct Px {
    f32 r, g, b, a; // premultiplied 0..1
};

inline void blend(u32& dst, Px s) {
    if (s.a <= 0.f && s.r <= 0.f && s.g <= 0.f && s.b <= 0.f) return;
    auto q = [](f32 v) { return u32(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }; // == lround for v in [0, 1]
    if (s.a >= 1.f) { // opaque: the destination does not contribute
        dst = q(s.r) | (q(s.g) << 8) | (q(s.b) << 16) | (q(s.a) << 24);
        return;
    }
    const f32 inv = 1.f - s.a;
    const f32 dr = f32(dst & 0xFF) / 255.f, dg = f32((dst >> 8) & 0xFF) / 255.f, db = f32((dst >> 16) & 0xFF) / 255.f,
              da = f32(dst >> 24) / 255.f;
    dst = q(s.r + dr * inv) | (q(s.g + dg * inv) << 8) | (q(s.b + db * inv) << 16) | (q(s.a + da * inv) << 24);
}

inline Px premul(Color c, f32 cov) {
    const f32 a = std::clamp(c.a, 0.f, 1.f) * cov;
    return {c.r * a, c.g * a, c.b * a, a};
}

/// Signed distance from p to a rounded rect (negative inside).
inline f32 sdRoundRect(f32 px, f32 py, const Rect& r, const std::array<f32, 4>& rad) {
    const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const f32 hx = r.w * 0.5f, hy = r.h * 0.5f;
    const f32 dx = px - cx, dy = py - cy;
    // corner by quadrant: tl, tr, br, bl
    f32 rr = dx < 0 ? (dy < 0 ? rad[0] : rad[3]) : (dy < 0 ? rad[1] : rad[2]);
    rr = std::min(rr, std::min(hx, hy));
    const f32 qx = std::abs(dx) - hx + rr, qy = std::abs(dy) - hy + rr;
    const f32 ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - rr;
}

struct Clip {
    Rect r;
    f32 radius;
};

// ---- colour / MSDF glyphs (atlas colour page: side size / 2, RGBA8, after the coverage page) ----
inline const u8* colorTexel(const u8* atlas, int size, int x, int y) {
    const int cs = size / 2;
    return atlas + std::size_t(size) * std::size_t(size) + (std::size_t(y) * std::size_t(cs) + std::size_t(x)) * 4;
}

// MSDF coverage at pixel centre (px, py): the glyph's texels (tw x th at u, v) stretched over `rect`, sampled
// bilinearly (edge-clamped), median of RGB, scaled by the screen-space distance range (`blur` = range in texels).
// ui_draw_list.comp and ui_sdf_quad.frag repeat this operation for operation.
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

namespace {
// Backdrop blur, the reference: three box passes per axis (~Gaussian, sigma ~ blur / 2) over the pixels under the
// rounded rect, sampled from a margin of `blur` around it (clamped to the target), then mixed in by coverage.
void backdropBlur(const DrawCmd& c, const Rect& clipR, f32 clipRadius, Image& target) {
    const Rect area = c.rect.intersect(clipR).intersect({0.f, 0.f, f32(target.width), f32(target.height)});
    if (area.empty()) return;
    const int r = std::max(1, int(std::lround(c.blur / 3.f)));
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
    const int m = 3 * r;
    const int sx0 = std::max(0, x0 - m), sy0 = std::max(0, y0 - m);
    const int sx1 = std::min(target.width, x1 + m), sy1 = std::min(target.height, y1 + m);
    const int w = sx1 - sx0, h = sy1 - sy0;
    std::vector<f32> buf(std::size_t(w) * std::size_t(h) * 4), tmp(buf.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 p = target.pixels[std::size_t(sy0 + y) * std::size_t(target.width) + std::size_t(sx0 + x)];
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
                // each output is its own window sum in a fixed order (not a running sum): order-independent per
                // pixel, so the GPU kernel computes the same bits in parallel
                for (int i = 0; i < n; ++i) {
                    f32 acc = 0.f;
                    for (int j = -r; j <= r; ++j) acc += at(i + j);
                    const std::size_t px = vertical ? std::size_t(i) * std::size_t(w) + std::size_t(l) : std::size_t(l) * std::size_t(w) + std::size_t(i);
                    tmp[px * 4 + std::size_t(k)] = acc * inv;
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
            u32& d = target.pixels[std::size_t(y) * std::size_t(target.width) + std::size_t(x)];
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

// Custom effect, the reference (effects.hpp): snapshot of the source region (the pixel area plus the margin, clamped to
// the target), the callback per pixel, mixed in by the mask coverage (same composite as the backdrop).
void effectPass(const DrawCmd& c, const Rect& clipR, f32 clipRadius, Image& target, const EffectRegistry* fx,
                const std::vector<const Image*>& images) {
    if (!fx || c.u >= fx->count() || !fx->at(c.u).cpu) return;
    const Rect area = c.rect.intersect(clipR).intersect({0.f, 0.f, f32(target.width), f32(target.height)});
    if (area.empty()) return;
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
    const int m = std::max(0, int(std::ceil(c.borderWidth)));
    const int sx0 = std::max(0, x0 - m), sy0 = std::max(0, y0 - m);
    const int w = std::min(target.width, x1 + m) - sx0, h = std::min(target.height, y1 + m) - sy0;
    thread_local std::vector<f32> buf;
    buf.resize(std::size_t(w) * std::size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 p = target.pixels[std::size_t(sy0 + y) * std::size_t(target.width) + std::size_t(sx0 + x)];
            for (int k = 0; k < 4; ++k) buf[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + std::size_t(k)] = f32((p >> (8 * k)) & 0xFFu);
        }
    const EffectSource src{buf.data(), sx0, sy0, w, h};
    const f32 params[kEffectParams] = {c.fill.r, c.fill.g, c.fill.b, c.fill.a, c.fill2.r, c.fill2.g, c.fill2.b, c.fill2.a};
    EffectPixel in;
    in.rect = c.rect;
    in.params = params;
    in.time = c.blur;
    in.src = &src;
    in.image = c.image < images.size() ? images[c.image] : nullptr;
    const CpuEffectFn fn = fx->at(c.u).cpu;
    const std::array<f32, 4> clipRad{clipRadius, clipRadius, clipRadius, clipRadius};
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const f32 px = f32(x) + 0.5f, py = f32(y) + 0.5f;
            f32 cov = std::clamp(0.5f - sdRoundRect(px, py, c.rect, c.radius), 0.f, 1.f);
            if (clipRadius > 0.f) cov *= std::clamp(0.5f - sdRoundRect(px, py, clipR, clipRad), 0.f, 1.f);
            if (cov <= 0.f) continue;
            in.x = x;
            in.y = y;
            const EffectColor e = fn(in);
            u32& d = target.pixels[std::size_t(y) * std::size_t(target.width) + std::size_t(x)];
            u32 o = 0;
            for (int k = 0; k < 4; ++k) {
                const f32 dv = f32((d >> (8 * k)) & 0xFFu);
                const f32 ev = std::clamp(e[std::size_t(k)], 0.f, 255.f);
                o |= u32(std::clamp(std::lround(dv + (ev - dv) * cov), 0l, 255l)) << (8 * k);
            }
            d = o;
        }
}
} // namespace


struct Rasterizer {
    const u8* m_atlas = nullptr;
    int m_atlasSize = 0;
    std::vector<const Image*> m_images;
    const EffectRegistry* m_effects = nullptr;
    void render(const DrawList& list, Image& target, Rect scissor = {}) const;
};

inline void Rasterizer::render(const DrawList& list, Image& target, Rect scissor) const {
    if (target.width <= 0 || target.height <= 0) return;
    Rect full{0.f, 0.f, f32(target.width), f32(target.height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    Clip stack[32];
    int depth = 0;
    stack[0] = {scissor, 0.f};
    for (const DrawCmd& c : list.cmds()) {
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
        case DrawOp::Effect: effectPass(c, clip.r, clip.radius, target, m_effects, m_images); continue;
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
            u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
            const bool solidRow = y >= iy0 && y < iy1;
            for (int x = x0; x < x1; ++x) {
                if (solidRow && x >= ix0 && x < ix1) {
                    row[x] = solid;
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
                    if (!m_atlas) break;
                    if (c.gradient == kGlyphMsdf) {
                        const f32 cov = msdfCoverage(c, m_atlas, m_atlasSize, px, py) * clipCov;
                        if (cov > 0.f) blend(row[x], premul(c.fill, cov));
                        break;
                    }
                    const int gx = x - int(c.rect.x), gy = y - int(c.rect.y);
                    if (c.gradient == kGlyphColor) {
                        if (gx < 0 || gy < 0 || gx >= c.tw || gy >= c.th) break;
                        const u8* t = colorTexel(m_atlas, m_atlasSize, c.u + gx, c.v + gy);
                        const f32 k = std::clamp(c.fill.a, 0.f, 1.f) * clipCov;
                        blend(row[x], {f32(t[0]) / 255.f * k, f32(t[1]) / 255.f * k, f32(t[2]) / 255.f * k, f32(t[3]) / 255.f * k});
                        break;
                    }
                    if (gx < 0 || gy < 0 || gx >= c.tw || gy >= c.th) break;
                    const u8 a = m_atlas[std::size_t(c.v + gy) * std::size_t(m_atlasSize) + std::size_t(c.u + gx)];
                    if (a) blend(row[x], premul(c.fill, f32(a) / 255.f * clipCov));
                    break;
                }
                case DrawOp::Image: {
                    if (c.image >= m_images.size() || !m_images[c.image]) break;
                    const Image& img = *m_images[c.image];
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


} // namespace fuse::ui::test::ref
