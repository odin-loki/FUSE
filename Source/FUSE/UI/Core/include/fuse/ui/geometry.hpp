// FUSE UI (UI2): basic geometry and colour value types. Units are logical pixels; snapping to device
// pixels happens in the rasteriser.
#pragma once

#include <fuse/types.hpp>

#include <algorithm>

namespace fuse::ui {

struct Vec2 {
    f32 x = 0.f;
    f32 y = 0.f;
    friend bool operator==(const Vec2&, const Vec2&) = default;
};

struct Insets {
    f32 left = 0.f;
    f32 top = 0.f;
    f32 right = 0.f;
    f32 bottom = 0.f;
    static constexpr Insets all(f32 v) { return {v, v, v, v}; }
    constexpr f32 horizontal() const { return left + right; }
    constexpr f32 vertical() const { return top + bottom; }
    friend bool operator==(const Insets&, const Insets&) = default;
};

struct Rect {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 w = 0.f;
    f32 h = 0.f;

    constexpr f32 right() const { return x + w; }
    constexpr f32 bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0.f || h <= 0.f; }
    constexpr bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    constexpr Rect inset(const Insets& i) const {
        return {x + i.left, y + i.top, std::max(0.f, w - i.horizontal()), std::max(0.f, h - i.vertical())};
    }
    constexpr Rect intersect(const Rect& o) const {
        const f32 l = std::max(x, o.x), t = std::max(y, o.y);
        const f32 r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.f, r - l), std::max(0.f, b - t)};
    }
    constexpr Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const f32 l = std::min(x, o.x), t = std::min(y, o.y);
        return {l, t, std::max(right(), o.right()) - l, std::max(bottom(), o.bottom()) - t};
    }
    friend bool operator==(const Rect&, const Rect&) = default;
};

/// Straight (non-premultiplied) 8-bit RGBA. The rasteriser premultiplies when blending.
struct Color {
    u8 r = 0, g = 0, b = 0, a = 0;
    static constexpr Color rgb(u32 hex, u8 alpha = 255) {
        return {u8((hex >> 16) & 0xff), u8((hex >> 8) & 0xff), u8(hex & 0xff), alpha};
    }
    constexpr u32 packed() const { return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a; }
    friend bool operator==(const Color&, const Color&) = default;
};

constexpr Color lerp(Color a, Color b, f32 t) {
    const auto mix = [t](u8 x, u8 y) { return u8(f32(x) + (f32(y) - f32(x)) * t + 0.5f); };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

} // namespace fuse::ui
