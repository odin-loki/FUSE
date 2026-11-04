#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/effects.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || (defined(__SSE2__) && !defined(__aarch64__))
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

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

void DrawList::effect(const Rect& r, u32 id, std::span<const f32> params, f32 time, f32 margin,
                      const std::array<f32, 4>& radii, u32 image) {
    DrawCmd c;
    c.op = DrawOp::Effect;
    c.rect = r;
    c.u = u16(id);
    c.image = image;
    f32 p[kEffectParams] = {};
    for (std::size_t i = 0; i < params.size() && i < std::size_t(kEffectParams); ++i) p[i] = params[i];
    c.fill = {p[0], p[1], p[2], p[3]};
    c.fill2 = {p[4], p[5], p[6], p[7]};
    c.blur = time;
    c.borderWidth = std::max(margin, 0.f);
    c.radius = radii;
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
    return h;
}

Rect DrawList::redrawRegion(Rect damage, Rect bounds) const {
    auto align = [&](Rect r) {
        r = r.intersect(bounds);
        if (r.empty()) return Rect{};
        const f32 x0 = std::floor(r.x), y0 = std::floor(r.y);
        return Rect{x0, y0, std::ceil(r.right()) - x0, std::ceil(r.bottom()) - y0};
    };
    damage = align(damage);
    if (damage.empty()) return damage;
    // grow until no backdrop's / effect's sampled region touches the damage only partly (chains of backdrops converge)
    for (bool grown = true; grown;) {
        grown = false;
        for (const DrawCmd& c : m_cmds) {
            if (c.op != DrawOp::Backdrop && c.op != DrawOp::Effect) continue;
            const f32 m = c.op == DrawOp::Effect ? std::ceil(c.borderWidth) + 1.f
                                                 : f32(3 * std::max(1, int(std::lround(c.blur / 3.f))) + 1);
            const Rect region = align({c.rect.x - m, c.rect.y - m, c.rect.w + 2 * m, c.rect.h + 2 * m});
            if (region.empty() || damage.intersect(region).empty()) continue;
            const Rect u = align(damage.unite(region));
            if (u.x < damage.x || u.y < damage.y || u.right() > damage.right() || u.bottom() > damage.bottom()) {
                damage = u;
                grown = true;
            }
        }
    }
    return damage;
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

// ---- CPU rasteriser ---------------------------------------------------------------------------------------------
//
// Every pixel gets exactly the float operations of the per-pixel reference (tests/cpu_raster_reference.hpp holds the
// original); speed comes from not evaluating pixels whose result is known:
//   * runs: per row, the pixels of a rect / shadow (and of a rounded clip) whose coverage is exactly 1 or exactly 0
//     follow from the exact offset shapes of the rounded rect, with kSpanEps to spare over float error (RowShape);
//     a full run blends one constant colour: a fill when opaque, else per-channel tables of the 256 results
//   * tighter bounds: a shadow is zero beyond blur + 1 px, a line beyond its half width + 1 px of the segment (per row)
//   * bands: the target is cut into horizontal bands rasterised in parallel (each band runs the whole list over its
//     rows); backdrops and effects read neighbouring pixels, so they run alone between parallel segments
namespace {

struct Px {
    f32 r, g, b, a; // premultiplied 0..1
};

// f32(i) / 255.f for every byte: the quotients blend() divides out, looked up
const std::array<f32, 256> kUnorm8 = [] {
    std::array<f32, 256> t{};
    for (int i = 0; i < 256; ++i) t[std::size_t(i)] = f32(i) / 255.f;
    return t;
}();

inline u32 q8(f32 v) { return u32(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); } // == lround for v in [0, 1]

inline void blend(u32& dst, Px s) {
    if (s.a <= 0.f && s.r <= 0.f && s.g <= 0.f && s.b <= 0.f) return;
    if (s.a >= 1.f) { // opaque: the destination does not contribute
        dst = q8(s.r) | (q8(s.g) << 8) | (q8(s.b) << 16) | (q8(s.a) << 24);
        return;
    }
    const f32 inv = 1.f - s.a;
    const f32 dr = kUnorm8[dst & 0xFF], dg = kUnorm8[(dst >> 8) & 0xFF], db = kUnorm8[(dst >> 16) & 0xFF],
              da = kUnorm8[dst >> 24];
    dst = q8(s.r + dr * inv) | (q8(s.g + dg * inv) << 8) | (q8(s.b + db * inv) << 16) | (q8(s.a + da * inv) << 24);
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

/// Gradient colour at a pixel centre (Rect `gradient` 1 vertical / 2 horizontal).
inline Color gradientAt(const DrawCmd& c, f32 px, f32 py) {
    const f32 t = c.gradient == 1 ? std::clamp((py - c.rect.y) / std::max(c.rect.h, 1.f), 0.f, 1.f)
                                  : std::clamp((px - c.rect.x) / std::max(c.rect.w, 1.f), 0.f, 1.f);
    const Color& f = c.fill;
    return {f.r + (c.fill2.r - f.r) * t, f.g + (c.fill2.g - f.g) * t, f.b + (c.fill2.b - f.b) * t,
            f.a + (c.fill2.a - f.a) * t};
}

struct Clip {
    Rect r;
    f32 radius;
};

struct ClipStack {
    Clip s[32];
    int depth = 0;
    const Clip& top() const { return s[depth]; }
    void apply(const DrawCmd& c) {
        if (c.op == DrawOp::PushClip) {
            if (depth < 31) {
                ++depth;
                s[depth] = {s[depth - 1].r.intersect(c.rect), c.radius[0]};
                if (s[depth - 1].radius > 0.f && c.radius[0] <= 0.f) s[depth].radius = 0.f;
            }
        } else if (depth > 0) {
            --depth;
        }
    }
};

// span classification needs the float error of sdRoundRect to stay far below kSpanEps: coordinates are bounded
constexpr f32 kSpanEps = 0.125f;
constexpr f32 kSpanLimit = 32768.f;
inline bool spanSafe(const Rect& r, f32 reach) {
    return std::abs(r.x) < kSpanLimit && std::abs(r.y) < kSpanLimit && r.w < kSpanLimit && r.h < kSpanLimit &&
           reach < kSpanLimit;
}

/// blend() of one source colour over a run of pixels: a fill when opaque, else per-channel tables of the 256 results
/// (kept while the source repeats: the same rect over many rows).
struct SpanBlender {
    Px s{-1.f, -1.f, -1.f, -1.f};
    bool table = false;
    u8 lut[4][256];

    void run(u32* p, int n, Px src) {
        if (n <= 0) return;
        if (src.a <= 0.f && src.r <= 0.f && src.g <= 0.f && src.b <= 0.f) return;
        if (src.a >= 1.f) {
            u32 v = 0;
            blend(v, src);
            std::fill_n(p, n, v);
            return;
        }
        const bool same = table && src.r == s.r && src.g == s.g && src.b == s.b && src.a == s.a;
        if (!same && n < 96) {
            for (int i = 0; i < n; ++i) blend(p[i], src);
            return;
        }
        if (!same) {
            const f32 inv = 1.f - src.a;
            const f32 ch[4] = {src.r, src.g, src.b, src.a};
            for (int k = 0; k < 4; ++k)
                for (int v = 0; v < 256; ++v) lut[k][v] = u8(q8(ch[k] + kUnorm8[std::size_t(v)] * inv));
            s = src;
            table = true;
        }
        for (int i = 0; i < n; ++i) {
            const u32 d = p[i];
            p[i] = u32(lut[0][d & 0xFF]) | (u32(lut[1][(d >> 8) & 0xFF]) << 8) | (u32(lut[2][(d >> 16) & 0xFF]) << 16) |
                   (u32(lut[3][d >> 24]) << 24);
        }
    }
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
    thread_local std::vector<f32> buf, tmp;
    buf.resize(std::size_t(w) * std::size_t(h) * 4);
    tmp.resize(buf.size());
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

// ---- 4 pixels at a time (SSE2 on x64, NEON on arm64) --------------------------------------------------------------
// Each lane repeats the scalar code operation for operation; min / max / clamp keep std's operand order (they decide
// which zero / NaN comes out), so a lane equals the scalar pixel bit for bit.
#if defined(_M_X64) || defined(__x86_64__) || (defined(__SSE2__) && !defined(__aarch64__))
#define FUSE_RASTER_SIMD 1
struct F4 {
    __m128 v;
};
struct I4 {
    __m128i v;
};
inline F4 splat(f32 s) { return {_mm_set1_ps(s)}; }
inline F4 operator+(F4 a, F4 b) { return {_mm_add_ps(a.v, b.v)}; }
inline F4 operator-(F4 a, F4 b) { return {_mm_sub_ps(a.v, b.v)}; }
inline F4 operator*(F4 a, F4 b) { return {_mm_mul_ps(a.v, b.v)}; }
inline F4 operator/(F4 a, F4 b) { return {_mm_div_ps(a.v, b.v)}; }
inline F4 vsqrt(F4 a) { return {_mm_sqrt_ps(a.v)}; }
inline F4 vabs(F4 a) { return {_mm_andnot_ps(_mm_set1_ps(-0.f), a.v)}; }
inline F4 vmin(F4 a, F4 b) { return {_mm_min_ps(b.v, a.v)}; } // std::min(a, b): b < a ? b : a
inline F4 vmax(F4 a, F4 b) { return {_mm_max_ps(b.v, a.v)}; } // std::max(a, b): a < b ? b : a
inline F4 lt(F4 a, F4 b) { return {_mm_cmplt_ps(a.v, b.v)}; } // lane masks are F4 (all bits set / clear)
inline F4 le(F4 a, F4 b) { return {_mm_cmple_ps(a.v, b.v)}; }
inline F4 ge(F4 a, F4 b) { return {_mm_cmpge_ps(a.v, b.v)}; }
inline F4 mand(F4 a, F4 b) { return {_mm_and_ps(a.v, b.v)}; }
inline F4 mor(F4 a, F4 b) { return {_mm_or_ps(a.v, b.v)}; }
inline F4 mnot(F4 a) { return {_mm_xor_ps(a.v, _mm_castsi128_ps(_mm_set1_epi32(-1)))}; }
inline bool any(F4 m) { return _mm_movemask_ps(m.v) != 0; }
inline F4 sel(F4 m, F4 a, F4 b) { return {_mm_or_ps(_mm_and_ps(m.v, a.v), _mm_andnot_ps(m.v, b.v))}; }
inline I4 seli(F4 m, I4 a, I4 b) {
    const __m128i mi = _mm_castps_si128(m.v);
    return {_mm_or_si128(_mm_and_si128(mi, a.v), _mm_andnot_si128(mi, b.v))};
}
inline F4 centres(int x) { return {_mm_add_ps(_mm_cvtepi32_ps(_mm_setr_epi32(x, x + 1, x + 2, x + 3)), _mm_set1_ps(0.5f))}; }
inline I4 load4(const u32* p) { return {_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))}; }
inline void store4(u32* p, I4 v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v.v); }
inline F4 channel(I4 p, int k) { return {_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(p.v, 8 * k), _mm_set1_epi32(0xFF)))}; }
inline I4 truncToInt(F4 a) { return {_mm_cvttps_epi32(a.v)}; }
inline I4 shl(I4 a, int k) { return {_mm_sll_epi32(a.v, _mm_cvtsi32_si128(k))}; }
inline I4 ior(I4 a, I4 b) { return {_mm_or_si128(a.v, b.v)}; }
#elif defined(__aarch64__) || defined(_M_ARM64)
#define FUSE_RASTER_SIMD 1
struct F4 {
    float32x4_t v;
};
struct I4 {
    uint32x4_t v;
};
inline F4 splat(f32 s) { return {vdupq_n_f32(s)}; }
inline F4 operator+(F4 a, F4 b) { return {vaddq_f32(a.v, b.v)}; }
inline F4 operator-(F4 a, F4 b) { return {vsubq_f32(a.v, b.v)}; }
inline F4 operator*(F4 a, F4 b) { return {vmulq_f32(a.v, b.v)}; }
inline F4 operator/(F4 a, F4 b) { return {vdivq_f32(a.v, b.v)}; }
inline F4 vsqrt(F4 a) { return {vsqrtq_f32(a.v)}; }
inline F4 vabs(F4 a) { return {vabsq_f32(a.v)}; }
inline F4 fromMask(uint32x4_t m) { return {vreinterpretq_f32_u32(m)}; }
inline uint32x4_t toMask(F4 m) { return vreinterpretq_u32_f32(m.v); }
inline F4 vmin(F4 a, F4 b) { return {vbslq_f32(vcltq_f32(b.v, a.v), b.v, a.v)}; } // std::min(a, b): b < a ? b : a
inline F4 vmax(F4 a, F4 b) { return {vbslq_f32(vcltq_f32(a.v, b.v), b.v, a.v)}; } // std::max(a, b): a < b ? b : a
inline F4 lt(F4 a, F4 b) { return fromMask(vcltq_f32(a.v, b.v)); }
inline F4 le(F4 a, F4 b) { return fromMask(vcleq_f32(a.v, b.v)); }
inline F4 ge(F4 a, F4 b) { return fromMask(vcgeq_f32(a.v, b.v)); }
inline F4 mand(F4 a, F4 b) { return fromMask(vandq_u32(toMask(a), toMask(b))); }
inline F4 mor(F4 a, F4 b) { return fromMask(vorrq_u32(toMask(a), toMask(b))); }
inline F4 mnot(F4 a) { return fromMask(vmvnq_u32(toMask(a))); }
inline bool any(F4 m) { return vmaxvq_u32(toMask(m)) != 0; }
inline F4 sel(F4 m, F4 a, F4 b) { return {vbslq_f32(toMask(m), a.v, b.v)}; }
inline I4 seli(F4 m, I4 a, I4 b) { return {vbslq_u32(toMask(m), a.v, b.v)}; }
inline F4 centres(int x) {
    const int32_t i[4] = {x, x + 1, x + 2, x + 3};
    return {vaddq_f32(vcvtq_f32_s32(vld1q_s32(i)), vdupq_n_f32(0.5f))};
}
inline I4 load4(const u32* p) { return {vld1q_u32(p)}; }
inline void store4(u32* p, I4 v) { vst1q_u32(p, v.v); }
inline F4 channel(I4 p, int k) { return {vcvtq_f32_u32(vandq_u32(vshlq_u32(p.v, vdupq_n_s32(-8 * k)), vdupq_n_u32(0xFF)))}; }
inline I4 truncToInt(F4 a) { return {vreinterpretq_u32_s32(vcvtq_s32_f32(a.v))}; }
inline I4 shl(I4 a, int k) { return {vshlq_u32(a.v, vdupq_n_s32(k))}; }
inline I4 ior(I4 a, I4 b) { return {vorrq_u32(a.v, b.v)}; }
#endif

#if defined(FUSE_RASTER_SIMD)
inline F4 vclamp(F4 v, F4 lo, F4 hi) { return vmin(vmax(v, lo), hi); } // std::clamp

struct Px4 {
    F4 r, g, b, a;
};
struct Color4 {
    F4 r, g, b, a;
};
inline Color4 splat(const Color& c) { return {splat(c.r), splat(c.g), splat(c.b), splat(c.a)}; }

inline Px4 premul4(const Color4& c, F4 cov) {
    const F4 a = vclamp(c.a, splat(0.f), splat(1.f)) * cov;
    return {c.r * a, c.g * a, c.b * a, a};
}

inline I4 q8x4(F4 v) { return truncToInt(vclamp(v, splat(0.f), splat(1.f)) * splat(255.f) + splat(0.5f)); }

/// blend() on 4 pixels; lanes outside `active` keep their value.
inline void blend4(u32* p, const Px4& s, F4 active) {
    const F4 zero = splat(0.f);
    const F4 skip = mor(mnot(active), mand(mand(le(s.a, zero), le(s.r, zero)), mand(le(s.g, zero), le(s.b, zero))));
    if (!any(mnot(skip))) return;
    const I4 dst = load4(p);
    const F4 opaque = ge(s.a, splat(1.f));
    const F4 inv = splat(1.f) - s.a;
    const F4 k255 = splat(255.f);
    const F4 src[4] = {s.r, s.g, s.b, s.a};
    I4 out = {};
    for (int k = 0; k < 4; ++k) {
        const I4 mixed = q8x4(src[k] + (channel(dst, k) / k255) * inv);
        const I4 c = seli(opaque, q8x4(src[k]), mixed);
        out = k == 0 ? c : ior(out, shl(c, 8 * k));
    }
    store4(p, seli(skip, dst, out));
}

/// sdRoundRect at 4 pixel centres of one row.
inline F4 sdRoundRect4(F4 px, f32 py, const Rect& r, const std::array<f32, 4>& rad) {
    const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const f32 hx = r.w * 0.5f, hy = r.h * 0.5f;
    const F4 dx = px - splat(cx);
    const f32 dy = py - cy;
    F4 rr = sel(lt(dx, splat(0.f)), splat(dy < 0 ? rad[0] : rad[3]), splat(dy < 0 ? rad[1] : rad[2]));
    rr = vmin(rr, splat(std::min(hx, hy)));
    const F4 qx = (vabs(dx) - splat(hx)) + rr, qy = splat(std::abs(dy) - hy) + rr;
    const F4 ox = vmax(qx, splat(0.f)), oy = vmax(qy, splat(0.f));
    return (vsqrt(ox * ox + oy * oy) + vmin(vmax(qx, qy), splat(0.f))) - rr;
}

/// ClipCov::at on 4 pixels.
inline F4 clipCov4(const Clip& clip, const std::array<f32, 4>& rad, F4 px, f32 py) {
    if (clip.radius > 0.f) return vclamp(splat(0.5f) - sdRoundRect4(px, py, clip.r, rad), splat(0.f), splat(1.f));
    return splat(1.f);
}
/// gradientAt on 4 pixels of one row.
inline Color4 gradient4(const DrawCmd& c, F4 px, f32 py) {
    if (c.gradient == 1) return splat(gradientAt(c, 0.f, py));
    const F4 t = vclamp((px - splat(c.rect.x)) / splat(std::max(c.rect.w, 1.f)), splat(0.f), splat(1.f));
    const Color& f = c.fill;
    return {splat(f.r) + splat(c.fill2.r - f.r) * t, splat(f.g) + splat(c.fill2.g - f.g) * t,
            splat(f.b) + splat(c.fill2.b - f.b) * t, splat(f.a) + splat(c.fill2.a - f.a) * t};
}
inline F4 allLanes() { return lt(splat(0.f), splat(1.f)); }
#endif

struct Raster {
    Image* target;
    const u8* atlas;
    int atlasSize;
    const std::vector<const Image*>* images;
};

/// Rounded-clip coverage at a pixel centre (1 without a rounded clip).
struct ClipCov {
    const Clip& clip;
    std::array<f32, 4> rad;
    explicit ClipCov(const Clip& c) : clip(c), rad{c.radius, c.radius, c.radius, c.radius} {}
    f32 at(f32 px, f32 py) const {
        if (clip.radius > 0.f) return std::clamp(0.5f - sdRoundRect(px, py, clip.r, rad), 0.f, 1.f);
        return 1.f;
    }
};

/// Per-row pixel ranges of a rounded rect as sdRoundRect sees it (each half of a row uses its quadrant's radius):
/// `in` holds centres where sd <= -inner, outside `out` sd >= outer (both with kSpanEps to spare). The bounds come
/// from the exact offset shapes (a rounded rect grown / shrunk by g is a rounded rect with radius max(r + g, 0)).
struct RowShape {
    bool ok = false;
    f32 cx = 0.f, cy = 0.f, hx = 0.f, hy = 0.f, gin = 0.f, gout = 0.f;
    std::array<f32, 4> rad{};
    RowShape() = default;
    RowShape(const Rect& r, const std::array<f32, 4>& radii, f32 inner, f32 outer) {
        const f32 rmax = std::max({radii[0], radii[1], radii[2], radii[3]});
        ok = r.w > 0.f && r.h > 0.f && radii[0] >= 0.f && radii[1] >= 0.f && radii[2] >= 0.f && radii[3] >= 0.f &&
             spanSafe(r, rmax + std::max(inner, outer));
        hx = r.w * 0.5f;
        hy = r.h * 0.5f;
        cx = r.x + hx;
        cy = r.y + hy;
        for (int i = 0; i < 4; ++i) rad[std::size_t(i)] = std::min(radii[std::size_t(i)], std::min(hx, hy));
        gin = -(inner + kSpanEps);
        gout = outer + kSpanEps;
    }
    /// Half width at row offset `ady` of the shape grown by g (< 0: the row misses it).
    f32 extent(f32 rc, f32 g, f32 ady) const {
        const f32 ex = hx + g, ey = hy + g;
        if (ady > ey || ex < 0.f) return -1.f;
        const f32 rho = std::max(rc + g, 0.f);
        const f32 k = ady - (ey - rho);
        return k <= 0.f ? ex : ex - rho + std::sqrt(std::max(rho * rho - k * k, 0.f));
    }
    /// Narrows [lo, hi) to the pixels that may be covered and returns the full-coverage run [s0, s1) inside it.
    void row(f32 py, int& lo, int& hi, int& s0, int& s1) const {
        s0 = s1 = hi;
        if (!ok) return;
        const f32 dy = py - cy, ady = std::abs(dy);
        const f32 rl = dy < 0 ? rad[0] : rad[3], rr = dy < 0 ? rad[1] : rad[2];
        const f32 ol = extent(rl, gout, ady), orr = extent(rr, gout, ady);
        if (ol < 0.f || orr < 0.f) {
            lo = hi;
            return;
        }
        lo = std::max(lo, int(std::floor(cx - ol - 0.75f)));
        hi = std::max(lo, std::min(hi, int(std::ceil(cx + orr + 0.25f)) + 1));
        const f32 il = extent(rl, gin, ady), ir = extent(rr, gin, ady);
        if (il < 0.f || ir < 0.f) {
            s0 = s1 = hi;
            return;
        }
        s0 = std::clamp(int(std::ceil(cx - il - 0.25f)), lo, hi);
        s1 = std::clamp(int(std::floor(cx + ir - 0.75f)) + 1, s0, hi);
    }
};

/// The row's pixels that may be drawn [lo, hi) and its full-coverage run [s0, s1) for a shape under an optional
/// rounded clip.
inline void rowRuns(const RowShape& shape, const RowShape* clip, f32 py, int& lo, int& hi, int& s0, int& s1) {
    shape.row(py, lo, hi, s0, s1);
    if (!clip) return;
    int c0 = hi, c1 = hi;
    clip->row(py, lo, hi, c0, c1);
    if (!clip->ok) c0 = c1 = hi;
    s0 = std::clamp(std::max(s0, c0), lo, hi);
    s1 = std::clamp(std::min(s1, c1), s0, hi);
}

void drawRect(const Raster& R, const DrawCmd& c, const Clip& clip, int x0, int x1, int y0, int y1, SpanBlender& sb) {
    Image& target = *R.target;
    const ClipCov cc(clip);
    const bool hasBorder = c.borderWidth > 0.f && c.border.a > 0.f;
    const f32 rmax = std::max({c.radius[0], c.radius[1], c.radius[2], c.radius[3], 0.f});
    // opaque, ungraded: the interior (pixel centres at least max radius + border + 0.5 inside every edge) is a plain
    // store of the packed fill (same bytes blend() would produce)
    const bool solidFill = !c.gradient && c.fill.a >= 1.f && clip.radius <= 0.f;
    int ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;
    u32 solid = 0;
    if (solidFill) {
        const f32 m = rmax + (hasBorder ? c.borderWidth : 0.f) + 0.5f;
        ix0 = int(std::ceil(c.rect.x + m - 0.5f)), ix1 = int(std::floor(c.rect.right() - m - 0.5f)) + 1;
        iy0 = int(std::ceil(c.rect.y + m - 0.5f)), iy1 = int(std::floor(c.rect.bottom() - m - 0.5f)) + 1;
        blend(solid, premul(c.fill, 1.f));
    }
    // coverage (and the border's inner coverage) is exactly 1 at least border + 0.5 inside, 0 from 0.5 outside; under
    // a rounded clip its coverage is 1 at least 0.5 inside the clip, 0 from 0.5 outside
    const RowShape shape(c.rect, c.radius, (hasBorder ? c.borderWidth : 0.f) + 0.5f, 0.5f);
#if defined(FUSE_RASTER_SIMD)
    const Color4 fill4 = splat(c.fill), border4 = splat(c.border);
#endif
    const RowShape clipShape = clip.radius > 0.f ? RowShape(clip.r, cc.rad, 0.5f, 0.5f) : RowShape();
    for (int y = y0; y < y1; ++y) {
        u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
        const f32 py = f32(y) + 0.5f;
        int lo = x0, hi = x1, s0 = x1, s1 = x1;
        rowRuns(shape, clip.radius > 0.f ? &clipShape : nullptr, py, lo, hi, s0, s1);
        if (solidFill && y >= iy0 && y < iy1 && ix0 < ix1) { // the reference's own run, joined with the exact one
            const int a = std::clamp(ix0, x0, x1), b = std::clamp(ix1, a, x1);
            if (s0 >= s1 || b < s0 || a > s1) {
                s0 = a;
                s1 = b;
            } else {
                s0 = std::min(s0, a);
                s1 = std::max(s1, b);
            }
            lo = std::min(lo, s0);
            hi = std::max(hi, s1);
        }
        auto edge = [&](int xa, int xb) {
            int x = xa;
#if defined(FUSE_RASTER_SIMD)
            for (; x + 4 <= xb; x += 4) {
                const F4 px = centres(x), zero = splat(0.f), one = splat(1.f), half = splat(0.5f);
                const F4 clipCov = clipCov4(clip, cc.rad, px, py);
                const F4 d = sdRoundRect4(px, py, c.rect, c.radius);
                const F4 outer = vclamp(half - d, zero, one) * clipCov;
                const F4 active = mand(mnot(le(clipCov, zero)), mnot(le(outer, zero)));
                if (!any(active)) continue;
                const Color4 fill = c.gradient ? gradient4(c, px, py) : fill4;
                if (hasBorder) {
                    const F4 inner = vclamp(half - (d + splat(c.borderWidth)), zero, one) * clipCov;
                    const Px4 pf = premul4(fill, inner), pb = premul4(border4, outer - inner);
                    blend4(row + x, {pf.r + pb.r, pf.g + pb.g, pf.b + pb.b, pf.a + pb.a}, active);
                } else {
                    blend4(row + x, premul4(fill, outer), active);
                }
            }
#endif
            for (; x < xb; ++x) {
                const f32 px = f32(x) + 0.5f;
                const f32 clipCov = cc.at(px, py);
                if (clipCov <= 0.f) continue;
                const f32 d = sdRoundRect(px, py, c.rect, c.radius);
                const f32 outer = std::clamp(0.5f - d, 0.f, 1.f) * clipCov;
                if (outer <= 0.f) continue;
                const Color fill = c.gradient ? gradientAt(c, px, py) : c.fill;
                if (hasBorder) {
                    const f32 inner = std::clamp(0.5f - (d + c.borderWidth), 0.f, 1.f) * clipCov;
                    const Px pf = premul(fill, inner);
                    const Px pb = premul(c.border, outer - inner);
                    blend(row[x], {pf.r + pb.r, pf.g + pb.g, pf.b + pb.b, pf.a + pb.a});
                } else {
                    blend(row[x], premul(fill, outer));
                }
            }
        };
        edge(lo, s0);
        if (s0 < s1) {
            if (solidFill) {
                std::fill(row + s0, row + s1, solid);
            } else if (c.gradient == 2) { // horizontal gradient: full coverage, the colour moves per pixel
                int x = s0;
#if defined(FUSE_RASTER_SIMD)
                for (; x + 4 <= s1; x += 4) blend4(row + x, premul4(gradient4(c, centres(x), py), splat(1.f)), allLanes());
#endif
                for (; x < s1; ++x) blend(row[x], premul(gradientAt(c, f32(x) + 0.5f, py), 1.f));
            } else {
                sb.run(row + s0, s1 - s0, premul(c.gradient ? gradientAt(c, 0.f, py) : c.fill, 1.f));
            }
        }
        edge(s1, hi);
    }
}

void drawShadow(const Raster& R, const DrawCmd& c, const Clip& clip, int x0, int x1, int y0, int y1, SpanBlender& sb) {
    Image& target = *R.target;
    const ClipCov cc(clip);
    const f32 b = std::max(c.blur, 0.5f);
    // zero beyond b of the rect (the reference's bounds reach 2 * blur): skip those pixels
    const f32 reach = std::min(c.blur * 2.f, b + 1.f);
    x0 = std::max(x0, int(std::floor(c.rect.x - reach)));
    x1 = std::min(x1, int(std::ceil(c.rect.right() + reach)));
    y0 = std::max(y0, int(std::floor(c.rect.y - reach)));
    y1 = std::min(y1, int(std::ceil(c.rect.bottom() + reach)));
    // full coverage (t == 0) from b inside, none (t == 1) from b outside
    const RowShape shape(c.rect, c.radius, b, b);
    const RowShape clipShape = clip.radius > 0.f ? RowShape(clip.r, cc.rad, 0.5f, 0.5f) : RowShape();
    const Px inside = premul(c.fill, 1.f);
#if defined(FUSE_RASTER_SIMD)
    const Color4 fill4 = splat(c.fill);
#endif
    for (int y = y0; y < y1; ++y) {
        u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
        const f32 py = f32(y) + 0.5f;
        int lo = x0, hi = x1, s0 = x1, s1 = x1;
        rowRuns(shape, clip.radius > 0.f ? &clipShape : nullptr, py, lo, hi, s0, s1);
        auto edge = [&](int xa, int xb) {
            int x = xa;
#if defined(FUSE_RASTER_SIMD)
            for (; x + 4 <= xb; x += 4) {
                const F4 px = centres(x), zero = splat(0.f), one = splat(1.f);
                const F4 clipCov = clipCov4(clip, cc.rad, px, py);
                const F4 active = mnot(le(clipCov, zero));
                if (!any(active)) continue;
                const F4 d = sdRoundRect4(px, py, c.rect, c.radius);
                const F4 t = vclamp((d + splat(b)) / splat(2.f * b), zero, one);
                const F4 cov = (one - t * t * (splat(3.f) - splat(2.f) * t)) * clipCov;
                blend4(row + x, premul4(fill4, cov), active);
            }
#endif
            for (; x < xb; ++x) {
                const f32 px = f32(x) + 0.5f;
                const f32 clipCov = cc.at(px, py);
                if (clipCov <= 0.f) continue;
                const f32 d = sdRoundRect(px, py, c.rect, c.radius);
                const f32 t = std::clamp((d + b) / (2.f * b), 0.f, 1.f);
                const f32 cov = (1.f - t * t * (3.f - 2.f * t)) * clipCov;
                blend(row[x], premul(c.fill, cov));
            }
        };
        edge(lo, s0);
        sb.run(row + s0, s1 - s0, inside);
        edge(s1, hi);
    }
}

void drawLine(const Raster& R, const DrawCmd& c, const Clip& clip, int x0, int x1, int y0, int y1) {
    Image& target = *R.target;
    const ClipCov cc(clip);
    const f32 ax = c.rect.x, ay = c.rect.y, bx = c.rect.w, by = c.rect.h;
    const f32 vx = bx - ax, vy = by - ay;
    const f32 len2 = std::max(vx * vx + vy * vy, 1e-6f);
    // coverage needs a centre within half width + 0.5 of the segment: per row, the x range of the segment part within
    // that reach vertically, widened by the reach (+ 1 px of slack)
    const f32 reach = c.borderWidth * 0.5f + 1.5f;
    const bool ranged = std::abs(ax) < kSpanLimit && std::abs(ay) < kSpanLimit && std::abs(bx) < kSpanLimit &&
                        std::abs(by) < kSpanLimit && reach < kSpanLimit;
    for (int y = y0; y < y1; ++y) {
        u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
        const f32 py = f32(y) + 0.5f;
        int xa = x0, xb = x1;
        if (ranged) {
            f32 lo = 0.f, hi = 1.f;
            if (std::abs(vy) > 1e-3f) {
                const f32 ta = (py - reach - ay) / vy, tb = (py + reach - ay) / vy;
                lo = std::max(lo, std::min(ta, tb));
                hi = std::min(hi, std::max(ta, tb));
            } else if (std::abs(py - ay) > reach + std::abs(vy)) {
                continue;
            }
            if (lo > hi) continue;
            const f32 xl = ax + vx * lo, xh = ax + vx * hi;
            xa = std::max(x0, int(std::floor(std::min(xl, xh) - reach)));
            xb = std::min(x1, int(std::ceil(std::max(xl, xh) + reach)) + 1);
        }
        int x = xa;
#if defined(FUSE_RASTER_SIMD)
        const f32 tyv = (py - ay) * vy;
        const Color4 fill4 = splat(c.fill);
        for (; x + 4 <= xb; x += 4) {
            const F4 px = centres(x), zero = splat(0.f), one = splat(1.f);
            const F4 clipCov = clipCov4(clip, cc.rad, px, py);
            const F4 t = vclamp(((px - splat(ax)) * splat(vx) + splat(tyv)) / splat(len2), zero, one);
            const F4 ex = px - (splat(ax) + splat(vx) * t), ey = splat(py) - (splat(ay) + splat(vy) * t);
            const F4 d = vsqrt(ex * ex + ey * ey) - splat(c.borderWidth * 0.5f);
            const F4 cov = vclamp(splat(0.5f) - d, zero, one) * clipCov;
            const F4 active = mand(mnot(le(clipCov, zero)), lt(zero, cov));
            if (any(active)) blend4(row + x, premul4(fill4, cov), active);
        }
#endif
        for (; x < xb; ++x) {
            const f32 px = f32(x) + 0.5f;
            const f32 clipCov = cc.at(px, py);
            if (clipCov <= 0.f) continue;
            const f32 t = std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.f, 1.f);
            const f32 ex = px - (ax + vx * t), ey = py - (ay + vy * t);
            const f32 d = std::sqrt(ex * ex + ey * ey) - c.borderWidth * 0.5f;
            const f32 cov = std::clamp(0.5f - d, 0.f, 1.f) * clipCov;
            if (cov > 0.f) blend(row[x], premul(c.fill, cov));
        }
    }
}

void drawGlyph(const Raster& R, const DrawCmd& c, const Clip& clip, int x0, int x1, int y0, int y1) {
    if (!R.atlas) return;
    Image& target = *R.target;
    const ClipCov cc(clip);
    const int ox = int(c.rect.x), oy = int(c.rect.y);
    if (c.gradient != kGlyphMsdf) { // texels map 1:1 from (ox, oy): only those pixels can draw
        x0 = std::max(x0, ox);
        x1 = std::min(x1, ox + int(c.tw));
        y0 = std::max(y0, oy);
        y1 = std::min(y1, oy + int(c.th));
    }
    for (int y = y0; y < y1; ++y) {
        u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
        const f32 py = f32(y) + 0.5f;
        for (int x = x0; x < x1; ++x) {
            const f32 px = f32(x) + 0.5f;
            const f32 clipCov = cc.at(px, py);
            if (clipCov <= 0.f) continue;
            if (c.gradient == kGlyphMsdf) {
                const f32 cov = msdfCoverage(c, R.atlas, R.atlasSize, px, py) * clipCov;
                if (cov > 0.f) blend(row[x], premul(c.fill, cov));
                continue;
            }
            const int gx = x - ox, gy = y - oy;
            if (c.gradient == kGlyphColor) {
                const u8* t = colorTexel(R.atlas, R.atlasSize, c.u + gx, c.v + gy);
                const f32 k = std::clamp(c.fill.a, 0.f, 1.f) * clipCov;
                blend(row[x], {f32(t[0]) / 255.f * k, f32(t[1]) / 255.f * k, f32(t[2]) / 255.f * k, f32(t[3]) / 255.f * k});
                continue;
            }
            const u8 a = R.atlas[std::size_t(c.v + gy) * std::size_t(R.atlasSize) + std::size_t(c.u + gx)];
            if (a) blend(row[x], premul(c.fill, f32(a) / 255.f * clipCov));
        }
    }
}

void drawImage(const Raster& R, const DrawCmd& c, const Clip& clip, int x0, int x1, int y0, int y1) {
    if (c.image >= R.images->size() || !(*R.images)[c.image]) return;
    const Image& img = *(*R.images)[c.image];
    Image& target = *R.target;
    const ClipCov cc(clip);
    for (int y = y0; y < y1; ++y) {
        u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
        const f32 py = f32(y) + 0.5f;
        const int sy = std::clamp(int((py - c.rect.y) / c.rect.h * f32(img.height)), 0, img.height - 1);
        for (int x = x0; x < x1; ++x) {
            const f32 px = f32(x) + 0.5f;
            const f32 clipCov = cc.at(px, py);
            if (clipCov <= 0.f) continue;
            const int sx = std::clamp(int((px - c.rect.x) / c.rect.w * f32(img.width)), 0, img.width - 1);
            const u32 s = img.at(sx, sy);
            const f32 k = clipCov * (c.fill.a > 0.f ? c.fill.a : 1.f);
            if (k >= 1.f && (s >> 24) == 255u) { row[x] = s; continue; } // opaque texel: blend() would store it
            blend(row[x], {f32(s & 0xFF) / 255.f * k, f32((s >> 8) & 0xFF) / 255.f * k,
                           f32((s >> 16) & 0xFF) / 255.f * k, f32(s >> 24) / 255.f * k});
        }
    }
}

/// One drawing command over rows [bandY0, bandY1).
void drawCmd(const Raster& R, const DrawCmd& c, const Clip& clip, int bandY0, int bandY1, SpanBlender& sb) {
    Rect bounds = c.rect;
    if (c.op == DrawOp::Shadow) bounds = {c.rect.x - c.blur * 2.f, c.rect.y - c.blur * 2.f, c.rect.w + c.blur * 4.f, c.rect.h + c.blur * 4.f};
    if (c.op == DrawOp::Line) {
        const f32 hw = c.borderWidth * 0.5f + 1.f;
        bounds = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw,
                  std::abs(c.rect.w - c.rect.x) + 2 * hw, std::abs(c.rect.h - c.rect.y) + 2 * hw};
    }
    const Rect area = bounds.intersect(clip.r);
    if (area.empty()) return;
    const int x0 = int(std::floor(area.x)), x1 = int(std::ceil(area.right()));
    const int y0 = std::max(bandY0, int(std::floor(area.y))), y1 = std::min(bandY1, int(std::ceil(area.bottom())));
    if (y0 >= y1) return;
    switch (c.op) {
    case DrawOp::Rect: drawRect(R, c, clip, x0, x1, y0, y1, sb); break;
    case DrawOp::Shadow: drawShadow(R, c, clip, x0, x1, y0, y1, sb); break;
    case DrawOp::Line: drawLine(R, c, clip, x0, x1, y0, y1); break;
    case DrawOp::Glyph: drawGlyph(R, c, clip, x0, x1, y0, y1); break;
    case DrawOp::Image: drawImage(R, c, clip, x0, x1, y0, y1); break;
    default: break;
    }
}

/// Commands [begin, end) (no backdrop / effect among them) over rows [y0, y1), from clip state `clips`.
void drawRange(const Raster& R, const DrawCmd* cmds, std::size_t begin, std::size_t end, ClipStack clips, int y0, int y1) {
    SpanBlender sb;
    for (std::size_t i = begin; i < end; ++i) {
        const DrawCmd& c = cmds[i];
        if (c.op == DrawOp::PushClip || c.op == DrawOp::PopClip) {
            clips.apply(c);
            continue;
        }
        drawCmd(R, c, clips.top(), y0, y1, sb);
    }
}

// ---- bands over a small persistent thread pool -------------------------------------------------------------------
// fuse_ui_core links no engine (apps use it without fuse_core), so it cannot submit to JobScheduler: a few workers of
// its own start on the first parallel render and live until the process ends (never joined: no shutdown ordering).
// One render uses them at a time; a concurrent render runs on its own thread. Nothing is allocated per render.
// FUSE_JOBS_SINGLE_THREAD builds (and Emscripten without pthreads) render on the calling thread only.
#if (defined(FUSE_JOBS_SINGLE_THREAD) && FUSE_JOBS_SINGLE_THREAD) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))
constexpr bool kBandThreads = false;
#else
constexpr bool kBandThreads = true;
#endif
class RasterPool {
public:
    using Fn = void (*)(void* ctx, int band);
    static RasterPool& get() {
        static RasterPool* pool = new RasterPool();
        return *pool;
    }
    int workers() const { return int(m_threads.size()); }
    /// Runs fn(ctx, band) for band in [0, n) on up to `helpers` workers plus the caller. False: busy, nothing ran.
    bool run(Fn fn, void* ctx, int n, int helpers) {
        std::unique_lock submit(m_submit, std::try_to_lock);
        if (!submit.owns_lock()) return false;
        {
            std::lock_guard l(m_mutex);
            m_fn = fn;
            m_ctx = ctx;
            m_n = n;
            m_helpers = helpers;
            m_next.store(0, std::memory_order_relaxed);
            m_done.store(0, std::memory_order_relaxed);
            m_generation.fetch_add(1, std::memory_order_release);
        }
        m_cv.notify_all();
        work(fn, ctx, n);
        // close the job, then wait for every worker that joined it
        {
            std::lock_guard l(m_mutex);
            m_fn = nullptr;
        }
        for (int spin = 0; m_done.load(std::memory_order_acquire) < n || m_active.load(std::memory_order_acquire) > 0; ++spin)
            if (spin > 64) std::this_thread::yield();
        return true;
    }

private:
    RasterPool() {
        const unsigned hw = std::thread::hardware_concurrency();
        const int n = std::clamp(int(hw) - 1, 0, 15);
        m_threads.reserve(std::size_t(n));
        for (int i = 0; i < n; ++i) m_threads.emplace_back([this, i] { loop(i); }).detach();
    }
    void work(Fn fn, void* ctx, int n) {
        for (int b; (b = m_next.fetch_add(1, std::memory_order_relaxed)) < n;) {
            fn(ctx, b);
            m_done.fetch_add(1, std::memory_order_release);
        }
    }
    void loop(int index) {
        u64 seen = 0;
        for (;;) {
            // a short spin catches the next segment of the same frame without a sleep / wake round trip
            for (int spin = 0; spin < 256 && m_generation.load(std::memory_order_acquire) == seen; ++spin)
                std::this_thread::yield();
            Fn fn = nullptr;
            void* ctx = nullptr;
            int n = 0;
            {
                std::unique_lock l(m_mutex);
                m_cv.wait(l, [&] { return m_generation.load(std::memory_order_relaxed) != seen; });
                seen = m_generation.load(std::memory_order_relaxed);
                if (!m_fn || index >= m_helpers) continue;
                fn = m_fn;
                ctx = m_ctx;
                n = m_n;
                m_active.fetch_add(1, std::memory_order_relaxed);
            }
            work(fn, ctx, n);
            m_active.fetch_sub(1, std::memory_order_release);
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_submit, m_mutex;
    std::condition_variable m_cv;
    std::atomic<u64> m_generation{0};
    std::atomic<int> m_next{0}, m_done{0}, m_active{0};
    Fn m_fn = nullptr;
    void* m_ctx = nullptr;
    int m_n = 0, m_helpers = 0;
};

struct BandJob {
    const Raster* R;
    const DrawCmd* cmds;
    std::size_t begin, end;
    const ClipStack* clips;
    int y0, y1, bandH;
    static void run(void* p, int band) {
        const auto& j = *static_cast<const BandJob*>(p);
        const int a = j.y0 + band * j.bandH;
        drawRange(*j.R, j.cmds, j.begin, j.end, *j.clips, a, std::min(j.y1, a + j.bandH));
    }
};

} // namespace

void CpuRasterizer::render(const DrawList& list, Image& target, Rect scissor) const {
    if (target.width <= 0 || target.height <= 0) return;
    Rect full{0.f, 0.f, f32(target.width), f32(target.height)};
    if (scissor.empty()) scissor = full;
    scissor = scissor.intersect(full);
    if (scissor.empty()) return;
    const Raster R{&target, m_atlas, m_atlasSize, &m_images};
    ClipStack clips;
    clips.s[0] = {scissor, 0.f};
    const int y0 = std::max(0, int(std::floor(scissor.y))), y1 = std::min(target.height, int(std::ceil(scissor.bottom())));
    // bands when the region is large enough to pay for the hand-off (~16 rows each, a few per thread)
    const double pixels = double(scissor.w) * double(scissor.h);
    int helpers = 0, bandH = y1 - y0;
    if (kBandThreads && m_threads != 1 && pixels >= 128.0 * 1024.0) {
        RasterPool& pool = RasterPool::get();
        helpers = m_threads > 1 ? std::min(m_threads - 1, pool.workers()) : pool.workers();
        if (helpers > 0) bandH = std::max(8, (y1 - y0 + 4 * (helpers + 1) - 1) / (4 * (helpers + 1)));
    }
    const DrawCmd* cmds = list.cmds().data();
    const std::size_t n = list.size();
    auto segment = [&](std::size_t b, std::size_t e, const ClipStack& at) {
        if (b >= e) return;
        const int bands = (y1 - y0 + bandH - 1) / bandH;
        BandJob job{&R, cmds, b, e, &at, y0, y1, bandH};
        if (bands > 1 && RasterPool::get().run(&BandJob::run, &job, bands, helpers)) return;
        drawRange(R, cmds, b, e, at, y0, y1);
    };
    // backdrops and effects read pixels around them: everything before must be done, and they run alone
    std::size_t start = 0;
    ClipStack startClips = clips;
    for (std::size_t i = 0; i < n; ++i) {
        const DrawCmd& c = cmds[i];
        if (c.op == DrawOp::PushClip || c.op == DrawOp::PopClip) {
            clips.apply(c);
            continue;
        }
        if (c.op != DrawOp::Backdrop && c.op != DrawOp::Effect) continue;
        segment(start, i, startClips);
        const Clip& clip = clips.top();
        if (c.op == DrawOp::Backdrop) backdropBlur(c, clip.r, clip.radius, target);
        else effectPass(c, clip.r, clip.radius, target, m_effects, m_images);
        start = i + 1;
        startClips = clips;
    }
    segment(start, n, startClips);
}

} // namespace fuse::ui
