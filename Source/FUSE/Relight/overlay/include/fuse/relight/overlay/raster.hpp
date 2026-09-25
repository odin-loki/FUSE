// FUSE Relight RL-6.1: the developer overlay's CPU half of drawing, and the CPU reference of its GPU compose pass.
//
// The UI is rasterised on the CPU into a *layer*: the panel rectangle as premultiplied RGBA8 (R in the low byte),
// row-major, panel-local. The GPU pass (overlay_gpu.hpp, shaders/overlay_compose.*) composites the layer over the
// back buffer's texels with the integer rule below, so the GPU result equals composeTexel() bit for bit:
//
//   out.c = layer.c + (base.c * (255 - layer.a) + 127) / 255     (c = r, g, b; the back buffer's alpha is kept)
//
// `base` is the back buffer's texel, or with a debug view the debug buffer's texel mapped to display values
// (debugTexel: saturate(texel * scale + bias), swizzled, round to 8 bits). The back buffer is read and written as raw
// 32-bit words (any 8-bit RGBA / BGRA format, UNORM or SRGB: the overlay draws in the encoded values, as the game's
// own 2D draws do); `bgra` gives the channel order.
#pragma once

#include <fuse/relight/overlay/overlay_types.h>
#include <fuse/relight/overlay/ui.hpp>

#include <cstdint>
#include <vector>

namespace fuse::relight::overlay {

/// Premultiplied `src` over premultiplied `dst` (every channel, alpha included).
constexpr Color blendOver(Color dst, Color src) {
    const std::uint32_t ia = 255u - (src >> 24);
    Color out = 0;
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        const std::uint32_t s = (src >> shift) & 0xffu;
        const std::uint32_t d = (dst >> shift) & 0xffu;
        out |= ((s + (d * ia + 127u) / 255u) & 0xffu) << shift;
    }
    return out;
}

/// Rasterises `draws` into `layer` (panel.w * panel.h texels, resized without shrinking; cleared first).
void rasterize(const DrawList& draws, const Rect& panel, std::vector<Color>& layer);

/// A raw back-buffer word -> {r, g, b, a} bytes packed R-low (and back).
constexpr std::uint32_t toRgba(std::uint32_t raw, bool bgra) {
    return bgra ? ((raw & 0xff00ff00u) | ((raw >> 16) & 0xffu) | ((raw & 0xffu) << 16)) : raw;
}
constexpr std::uint32_t fromRgba(std::uint32_t rgbaWord, bool bgra) { return toRgba(rgbaWord, bgra); }

/// The compose rule on one texel (raw in, raw out).
constexpr std::uint32_t composeTexel(std::uint32_t raw, Color layer, bool bgra) {
    const std::uint32_t base = toRgba(raw, bgra);
    const std::uint32_t ia = 255u - (layer >> 24);
    std::uint32_t out = base & 0xff000000u;
    for (std::uint32_t shift = 0; shift < 24; shift += 8) {
        const std::uint32_t l = (layer >> shift) & 0xffu;
        const std::uint32_t b = (base >> shift) & 0xffu;
        out |= ((l + (b * ia + 127u) / 255u) & 0xffu) << shift;
    }
    return fromRgba(out, bgra);
}

/// How a debug buffer's texel becomes a displayed colour (frame_renderer.hpp DebugImage carries it).
struct DebugMapping {
    float scale[4] = {1.f, 1.f, 1.f, 1.f};
    float bias[4] = {0.f, 0.f, 0.f, 0.f};
    std::uint32_t swizzle = kOverlaySwizzleRgb;
};

/// The debug view's displayed texel (raw, `alphaRaw`'s alpha kept) for a source texel.
std::uint32_t debugTexel(const float texel[4], const DebugMapping& mapping, bool bgra, std::uint32_t alphaRaw);

/// CPU reference of the whole compose pass over a region (raw words, region-local rows of `push.regionW`): the
/// debug view (when push.flags has kOverlayFlagDebug, `debugTexels` = debugW * debugH RGBA floats, nearest sampling
/// as the shader) and the layer.
void composeRegionCpu(const OverlayPush& push, const DebugMapping& mapping, const float* debugTexels,
                      const std::vector<Color>& layer, std::vector<std::uint32_t>& region);

} // namespace fuse::relight::overlay
