// FUSE Relight RL-6.1: CPU rasterisation of the overlay UI and the compose pass's CPU reference (see raster.hpp).
#include <fuse/relight/overlay/raster.hpp>

#include <fuse/relight/overlay/font5x7.hpp>

#include <algorithm>

namespace fuse::relight::overlay {

namespace {

void fillRect(std::vector<Color>& layer, const Rect& panel, std::int32_t x0, std::int32_t y0, std::int32_t x1,
              std::int32_t y1, Color color) {
    // Back-buffer coordinates -> panel-local, clipped to the panel.
    x0 = std::max(x0, panel.x) - panel.x;
    y0 = std::max(y0, panel.y) - panel.y;
    x1 = std::min(x1, panel.x + panel.w) - panel.x;
    y1 = std::min(y1, panel.y + panel.h) - panel.y;
    for (std::int32_t y = y0; y < y1; ++y) {
        Color* row = layer.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(panel.w);
        for (std::int32_t x = x0; x < x1; ++x) {
            row[x] = blendOver(row[x], color);
        }
    }
}

} // namespace

void rasterize(const DrawList& draws, const Rect& panel, std::vector<Color>& layer) {
    const std::size_t n = panel.empty() ? 0u : static_cast<std::size_t>(panel.w) * static_cast<std::size_t>(panel.h);
    if (layer.size() < n) {
        layer.resize(n);
    }
    std::fill(layer.begin(), layer.begin() + static_cast<std::ptrdiff_t>(n), Color{0});
    if (n == 0) {
        return;
    }
    for (const DrawCmd& c : draws.cmds()) {
        if (c.kind == DrawCmd::Kind::Fill) {
            fillRect(layer, panel, c.x, c.y, c.x + c.w, c.y + c.h, c.color);
            continue;
        }
        const std::string_view s = draws.text(c);
        const std::int32_t sc = c.scale;
        const std::int32_t clipRight = c.x + c.w;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const std::int32_t gx = c.x + static_cast<std::int32_t>(i) * kCellWidth * sc;
            const std::uint8_t* rows = glyph5x7(s[i]);
            for (std::int32_t r = 0; r < kGlyphHeight; ++r) {
                for (std::int32_t col = 0; col < kGlyphWidth; ++col) {
                    if ((rows[r] >> (kGlyphWidth - 1 - col)) & 1u) {
                        const std::int32_t px = gx + col * sc;
                        const std::int32_t py = c.y + r * sc;
                        fillRect(layer, panel, px, py, std::min(px + sc, clipRight), py + sc, c.color);
                    }
                }
            }
        }
    }
}

std::uint32_t debugTexel(const float texel[4], const DebugMapping& m, bool bgra, std::uint32_t alphaRaw) {
    float v[4];
    for (int i = 0; i < 4; ++i) {
        const float x = texel[i] * m.scale[i] + m.bias[i];
        v[i] = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    }
    float rgb[3] = {v[0], v[1], v[2]};
    if (m.swizzle == kOverlaySwizzleRrr) {
        rgb[1] = rgb[2] = v[0];
    } else if (m.swizzle == kOverlaySwizzleRg0) {
        rgb[2] = 0.f;
    }
    std::uint32_t out = toRgba(alphaRaw, bgra) & 0xff000000u;
    for (int i = 0; i < 3; ++i) {
        out |= (static_cast<std::uint32_t>(rgb[i] * 255.f + 0.5f) & 0xffu) << (8 * i);
    }
    return fromRgba(out, bgra);
}

void composeRegionCpu(const OverlayPush& p, const DebugMapping& mapping, const float* debugTexels,
                      const std::vector<Color>& layer, std::vector<std::uint32_t>& region) {
    const bool bgra = (p.flags & kOverlayFlagBgra) != 0;
    const bool debug = (p.flags & kOverlayFlagDebug) != 0 && debugTexels && p.debugW && p.debugH && p.frameW &&
                       p.frameH;
    for (std::uint32_t y = 0; y < p.regionH; ++y) {
        for (std::uint32_t x = 0; x < p.regionW; ++x) {
            std::uint32_t& t = region[static_cast<std::size_t>(y) * p.regionW + x];
            const std::uint32_t fx = static_cast<std::uint32_t>(p.regionX) + x;
            const std::uint32_t fy = static_cast<std::uint32_t>(p.regionY) + y;
            if (debug) {
                const std::uint32_t sx = std::min(p.debugW - 1u, (fx * p.debugW) / p.frameW);
                const std::uint32_t sy = std::min(p.debugH - 1u, (fy * p.debugH) / p.frameH);
                t = debugTexel(debugTexels + (static_cast<std::size_t>(sy) * p.debugW + sx) * 4u, mapping, bgra, t);
            }
            const std::int32_t lx = static_cast<std::int32_t>(fx) - p.panelX;
            const std::int32_t ly = static_cast<std::int32_t>(fy) - p.panelY;
            if (lx >= 0 && ly >= 0 && static_cast<std::uint32_t>(lx) < p.panelW &&
                static_cast<std::uint32_t>(ly) < p.panelH) {
                t = composeTexel(t, layer[static_cast<std::size_t>(ly) * p.panelW + static_cast<std::uint32_t>(lx)],
                                 bgra);
            }
        }
    }
}

} // namespace fuse::relight::overlay
