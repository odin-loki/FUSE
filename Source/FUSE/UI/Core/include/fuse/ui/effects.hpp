// FUSE UI custom shader effects (plan §4.9 "custom shaders"): a user effect runs over a node's rect (or any draw-list
// region) after what is under it has been drawn, reading a snapshot of the target around the rect and writing the
// result back, masked by the rounded rect and the clip.
//
// An effect has two implementations that must agree:
//   * GPU: a GLSL compute snippet defining `vec4 fx_main(ivec2 p)`, spliced into Render/src/shaders/ui_effect.comp.in
//     and compiled to SPIR-V at build time (CMake: fuse_ui_add_effect, Render/cmake/fuse_ui_effects.cmake). The
//     fuse_ui_vk backend runs it between draw-list segments (the backdrop-blur mechanism): copy the source region to
//     scratch, run the effect pipeline over the rect, continue drawing.
//   * CPU: a C++ callback with the same arithmetic (the reference, and the CPU rasteriser's path).
// The helpers below (fx::sin, fx::hash, ...) exist in both languages with identical float operations, so effects built
// from add / mul / floor / divRn / sqrtRn and these helpers are bit-exact on the GPU. Effects that call native
// transcendental functions (exp, pow, GLSL sin) register `exact = false` and are compared within a tolerance.
//
// Style hookup: `effect: <name>(<p0>, <p1>, ...)` (up to kEffectParams numbers; missing ones take the registered
// defaults; `effect: none` clears). UiContext::setEffects attaches a registry. Built-ins (EffectRegistry::addBuiltins):
//   desaturate(amount = 1, r = 1, g = 1, b = 1)    luminance (Rec. 709) times the tint, mixed in by `amount`
//   ripple(amplitude = 4, wavelength = 24, speed = 40)   radial ripple from the rect centre, px; animated
//   grain(amount = 0.08, fps = 24)                 per-pixel hash noise added to the colour, re-seeded `fps` times / s
// Draw lists: DrawList::effect. DrawCmd layout of DrawOp::Effect: `u` effect id, `image` sampled image index or ~0u,
// `fill` / `fill2` params 0..7, `blur` time in seconds, `borderWidth` sample margin (px), `radius` the mask corners.
#pragma once

#include "fuse/ui/draw_list.hpp"

#include <array>
#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

inline constexpr int kEffectParams = 8;
using EffectColor = std::array<f32, 4>; ///< premultiplied RGBA, 0..255 per channel

/// Snapshot of the target around an effect (the source region), 0..255 premultiplied channels.
struct EffectSource {
    const f32* px = nullptr;
    int x0 = 0, y0 = 0, w = 0, h = 0;
    /// Texel at target coordinates, clamped to the region (fx_src in GLSL).
    EffectColor at(int x, int y) const {
        x = std::clamp(x - x0, 0, w - 1);
        y = std::clamp(y - y0, 0, h - 1);
        const f32* p = px + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
        return {p[0], p[1], p[2], p[3]};
    }
};

/// What a CPU effect sees for one pixel (GLSL: fx_main's `p` and the fx_* globals).
struct EffectPixel {
    int x = 0, y = 0;            ///< target pixel
    Rect rect;                   ///< the effect rect (fx_rect)
    const f32* params = nullptr; ///< kEffectParams values (fx_param(i))
    f32 time = 0.f;              ///< seconds (fx_time)
    const EffectSource* src = nullptr;
    const Image* image = nullptr; ///< optional sampled image (fx_image*)
};
using CpuEffectFn = EffectColor (*)(const EffectPixel&);

struct EffectDesc {
    std::string name;
    CpuEffectFn cpu = nullptr;
    std::span<const u32> spirv;                   ///< compute SPIR-V from ui_effect.comp.in (empty: built-in lookup by name)
    std::array<f32, kEffectParams> defaults{};    ///< values of parameters the style does not give
    f32 margin = 0.f;                             ///< px the effect may read beyond its own pixel (fixed part)
    i32 marginParam = -1;                         ///< parameter whose |value| is added to `margin` (e.g. an amplitude)
    bool animated = false;                        ///< reads the time: nodes using it repaint every update
    bool exact = true;                            ///< GPU and CPU agree bit for bit
};

/// Name -> effect. Ids are indices (stable; up to kMaxEffects).
class EffectRegistry {
public:
    static constexpr u32 kMaxEffects = 32;
    /// Adds (or replaces, same name) an effect. Returns its id, or ~0u when full.
    u32 add(EffectDesc d);
    /// desaturate, ripple, grain.
    void addBuiltins();
    i32 find(std::string_view name) const { return findHash(hashId(name)); }
    i32 findHash(u32 nameHash) const;
    u32 count() const { return u32(m_effects.size()); }
    const EffectDesc& at(u32 id) const { return m_effects[id]; }
    /// Sample margin of effect `id` for the given parameters.
    f32 margin(u32 id, const f32* params) const;

private:
    std::vector<EffectDesc> m_effects;
    std::vector<u32> m_hashes;
};

/// Shared effect math: the same float operations as the GLSL helpers in ui_effect.comp.in.
namespace fx {
inline f32 bits(u32 b) { return std::bit_cast<f32>(b); }
/// sin with a fixed polynomial (|error| < 1e-6 on any finite input that is not huge): Cody-Waite reduction to
/// [-pi, pi], folded to [-pi/2, pi/2], odd Taylor polynomial to x^11.
inline f32 sin(f32 x) {
    const f32 k = std::floor(x * bits(0x3e22f983u) + 0.5f); // 1 / 2pi
    f32 r = x - k * bits(0x40c90000u);                       // 6.28125
    r = r - k * bits(0x3afdaa22u);                           // 2pi - 6.28125
    if (r > bits(0x3fc90fdbu)) r = bits(0x40490fdbu) - r;    // pi/2, pi
    if (r < -bits(0x3fc90fdbu)) r = -bits(0x40490fdbu) - r;
    const f32 r2 = r * r;
    f32 p = bits(0xb2d7322bu);
    p = p * r2 + bits(0x3638ef1du);
    p = p * r2 + bits(0xb9500d01u);
    p = p * r2 + bits(0x3c088889u);
    p = p * r2 + bits(0xbe2aaaabu);
    return r + (r * r2) * p;
}
inline f32 cos(f32 x) { return sin(x + bits(0x3fc90fdbu)); }
inline constexpr f32 kTwoPi = 6.28318548202514648f; // bits 0x40c90fdb
/// Integer hash of a pixel / seed (fx_hash).
inline u32 hash(u32 x, u32 y, u32 z) {
    u32 h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (z * 0xcb1ab31fu);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
/// hash -> [0, 1), exact (24 bits).
inline f32 hash01(u32 x, u32 y, u32 z) { return f32(hash(x, y, z) >> 8) * 5.9604644775390625e-8f; }
/// Premultiplied 0..255 texel of an image (nearest, clamped) — fx_imageTexel.
inline EffectColor imageTexel(const Image* img, int x, int y) {
    if (!img || img->width <= 0 || img->height <= 0) return {0.f, 0.f, 0.f, 0.f};
    const u32 s = img->at(std::clamp(x, 0, img->width - 1), std::clamp(y, 0, img->height - 1));
    return {f32(s & 0xFFu), f32((s >> 8) & 0xFFu), f32((s >> 16) & 0xFFu), f32(s >> 24)};
}
} // namespace fx

} // namespace fuse::ui
