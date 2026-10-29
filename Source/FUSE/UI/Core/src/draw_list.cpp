#include "fuse/ui/draw_list.hpp"

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

namespace {

struct Px {
    f32 r, g, b, a; // premultiplied 0..1
};

inline void blend(u32& dst, Px s) {
    if (s.a <= 0.f && s.r <= 0.f && s.g <= 0.f && s.b <= 0.f) return;
    const f32 inv = 1.f - s.a;
    const f32 dr = f32(dst & 0xFF) / 255.f, dg = f32((dst >> 8) & 0xFF) / 255.f, db = f32((dst >> 16) & 0xFF) / 255.f,
              da = f32(dst >> 24) / 255.f;
    auto q = [](f32 v) { return u32(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
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

} // namespace

void CpuRasterizer::render(const DrawList& list, Image& target, Rect scissor) const {
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
        for (int y = y0; y < y1; ++y) {
            u32* row = target.pixels.data() + std::size_t(y) * std::size_t(target.width);
            for (int x = x0; x < x1; ++x) {
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
                    const int gx = x - int(c.rect.x), gy = y - int(c.rect.y);
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
