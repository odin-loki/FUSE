// Effect registry and the CPU side of the built-in effects (effects.hpp). The GLSL twins live in
// Render/src/shaders/effects/*.glsl and must keep the same operation order.
#include "fuse/ui/effects.hpp"

#include <cmath>

namespace fuse::ui {

u32 EffectRegistry::add(EffectDesc d) {
    const u32 h = hashId(d.name);
    const i32 at = findHash(h);
    if (at >= 0) {
        m_effects[std::size_t(at)] = std::move(d);
        return u32(at);
    }
    if (m_effects.size() >= kMaxEffects) return ~0u;
    m_effects.push_back(std::move(d));
    m_hashes.push_back(h);
    return u32(m_effects.size() - 1);
}

i32 EffectRegistry::findHash(u32 h) const {
    for (std::size_t i = 0; i < m_hashes.size(); ++i)
        if (m_hashes[i] == h) return i32(i);
    return -1;
}

f32 EffectRegistry::margin(u32 id, const f32* params) const {
    if (id >= count()) return 0.f;
    const EffectDesc& d = m_effects[id];
    f32 m = d.margin;
    if (d.marginParam >= 0 && d.marginParam < kEffectParams && params) m += std::abs(params[d.marginParam]);
    return std::ceil(m);
}

namespace {
// desaturate(amount, r, g, b): Rec. 709 luminance of the premultiplied colour, times the tint, mixed by amount
EffectColor desaturate(const EffectPixel& p) {
    const EffectColor c = p.src->at(p.x, p.y);
    const f32 y = c[0] * fx::bits(0x3e59b3d0u) + c[1] * fx::bits(0x3f371759u) + c[2] * fx::bits(0x3d93dd98u);
    const f32 a = p.params[0];
    EffectColor o;
    for (int k = 0; k < 3; ++k) o[std::size_t(k)] = c[std::size_t(k)] + (y * p.params[1 + k] - c[std::size_t(k)]) * a;
    o[3] = c[3];
    return o;
}

// ripple(amplitude, wavelength, speed): samples displaced along the ray from the rect centre by amplitude * sin
EffectColor ripple(const EffectPixel& p) {
    const f32 cx = p.rect.x + p.rect.w * 0.5f, cy = p.rect.y + p.rect.h * 0.5f;
    const f32 dx = (f32(p.x) + 0.5f) - cx, dy = (f32(p.y) + 0.5f) - cy;
    const f32 d = std::sqrt(dx * dx + dy * dy);
    const f32 wl = std::max(p.params[1], 1.f);
    const f32 phase = ((d - p.time * p.params[2]) / wl) * fx::kTwoPi;
    const f32 off = p.params[0] * fx::sin(phase);
    const f32 k = d > 0.5f ? off / d : 0.f;
    const int sx = p.x + int(std::floor(dx * k + 0.5f)), sy = p.y + int(std::floor(dy * k + 0.5f));
    return p.src->at(sx, sy);
}

// grain(amount, fps): hash noise in [-1, 1) * amount * 255 added to the colour (kept <= alpha), re-seeded fps x / s
EffectColor grain(const EffectPixel& p) {
    const EffectColor c = p.src->at(p.x, p.y);
    const u32 frame = p.params[1] > 0.f ? u32(std::max(0.f, std::floor(p.time * p.params[1]))) : 0u;
    const f32 n = fx::hash01(u32(p.x), u32(p.y), frame) * 2.f - 1.f;
    const f32 delta = n * p.params[0] * 255.f;
    EffectColor o;
    for (int k = 0; k < 3; ++k) o[std::size_t(k)] = std::clamp(c[std::size_t(k)] + delta, 0.f, c[3]);
    o[3] = c[3];
    return o;
}
} // namespace

void EffectRegistry::addBuiltins() {
    add({.name = "desaturate", .cpu = &desaturate, .defaults = {1.f, 1.f, 1.f, 1.f}});
    add({.name = "ripple", .cpu = &ripple, .defaults = {4.f, 24.f, 40.f}, .margin = 1.f, .marginParam = 0, .animated = true});
    add({.name = "grain", .cpu = &grain, .defaults = {0.08f, 24.f}, .animated = true});
}

} // namespace fuse::ui
