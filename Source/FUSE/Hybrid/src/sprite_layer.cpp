// E03 hybrid sprite layer: CPU reference (see include/fuse/hybrid/sprite_layer.hpp). Built with -ffp-contract=off
// (Hybrid/CMakeLists.txt) so it matches the `precise` GLSL kernel bit for bit before the half conversion.
#include <fuse/hybrid/sprite_layer.hpp>

#include <cmath>

namespace fuse::hybrid {

SpriteQuad spriteQuadFromPlaceholder(f32 x, f32 y, f32 rotation, u8 r, u8 g, u8 b, u32 width, u32 height) {
    const f32 w = static_cast<f32>(width > 0u ? width : kPlaceholderWidth);
    const f32 h = static_cast<f32>(height > 0u ? height : kPlaceholderHeight);
    const f32 scale = h / static_cast<f32>(kPlaceholderHeight);
    SpriteQuad q{};
    q.center[0] = w * 0.5f + x * scale;
    q.center[1] = h * 0.5f + y * scale;
    q.halfExtent[0] = kPlaceholderSpriteHalfExtent * scale;
    q.halfExtent[1] = kPlaceholderSpriteHalfExtent * scale;
    q.cosSin[0] = std::cos(rotation);
    q.cosSin[1] = std::sin(rotation);
    q.color[0] = static_cast<f32>(r) / 255.f;
    q.color[1] = static_cast<f32>(g) / 255.f;
    q.color[2] = static_cast<f32>(b) / 255.f;
    q.color[3] = 1.f;
    return q;
}

void spriteLayerTexel(const SpriteQuad* quads, u32 count, u32 px, u32 py, f32 out[4]) {
    f32 acc[4] = {0.f, 0.f, 0.f, 0.f};
    const f32 cx = static_cast<f32>(px) + 0.5f;
    const f32 cy = static_cast<f32>(py) + 0.5f;
    for (u32 i = 0; i < count; ++i) {
        const SpriteQuad& q = quads[i];
        const f32 dx = cx - q.center[0];
        const f32 dy = cy - q.center[1];
        const f32 ax = dx * q.cosSin[0];
        const f32 ay = dy * q.cosSin[1];
        const f32 lx = ax + ay;
        const f32 bx = dx * q.cosSin[1];
        const f32 by = dy * q.cosSin[0];
        const f32 ly = by - bx;
        if (std::fabs(lx) <= q.halfExtent[0] && std::fabs(ly) <= q.halfExtent[1]) {
            const f32 k = 1.f - q.color[3];
            for (u32 c = 0; c < 4u; ++c) {
                const f32 t = k * acc[c];
                acc[c] = q.color[c] + t;
            }
        }
    }
    for (u32 c = 0; c < 4u; ++c) {
        out[c] = acc[c];
    }
}

} // namespace fuse::hybrid
