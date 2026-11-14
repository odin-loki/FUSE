// Internal: pieces of the CPU reference rasteriser shared by draw_list.cpp (classic ops) and draw_vector.cpp (paths,
// gradients, 9-slice, clip-path and opacity groups).
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::ui::detail {

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

/// The Image blend equation: every vector op composites its source texel `s` (premultiplied RGBA8) with this.
inline void compositeTexel(u32& dst, u32 s, f32 k) {
    if (k >= 1.f && (s >> 24) == 255u) { // opaque texel: blend() would store it
        dst = s;
        return;
    }
    blend(dst, {f32(s & 0xFF) / 255.f * k, f32((s >> 8) & 0xFF) / 255.f * k, f32((s >> 16) & 0xFF) / 255.f * k,
                f32(s >> 24) / 255.f * k});
}

/// A pixel window: covers the device rect [ox, ox + w) x [oy, oy + h), rows of `w` pixels.
struct Surface {
    u32* px = nullptr;
    int ox = 0, oy = 0, w = 0, h = 0;
    u32* row(int y) const { return px + std::size_t(y - oy) * std::size_t(w); } ///< index with [x - ox]
    Rect rect() const { return {f32(ox), f32(oy), f32(w), f32(h)}; }
};

struct RasterEnv {
    RasterSources src;
    int rootW = 0;
};

/// Renders commands [first, last) over `surf`, clipped to `scissor` (inside the surface).
void renderRange(const RasterEnv& env, const DrawList& list, std::size_t first, std::size_t last, const Surface& surf,
                 Rect scissor, int level, bool groupOpacityOne);
// draw_vector.cpp
void drawPathCmd(const RasterEnv& env, const DrawList& list, const DrawCmd& c, const Clip& clip, const Surface& surf);
void drawPatchCmd(const RasterEnv& env, const DrawList& list, const DrawCmd& c, const Clip& clip, const Surface& surf);
/// Renders the group that starts at `i` (PushClipPath / PushLayer); returns the index of its PopClip (or `last`).
std::size_t drawGroup(const RasterEnv& env, const DrawList& list, std::size_t i, std::size_t last, const Clip& clip,
                      const Surface& surf, int level, bool opacityOne);

/// Renders the effect group that starts at `i` (PushTransform / PushFilter / PushMask); returns the index of its PopClip.
std::size_t drawFxGroup(const RasterEnv& env, const DrawList& list, std::size_t i, std::size_t last, const Clip& clip,
                        const Surface& surf, int level);

} // namespace fuse::ui::detail
