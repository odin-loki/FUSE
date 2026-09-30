#pragma once
// E03 hybrid 2D sprite + UI layer (U4-HYBRID-FRAME.md U4-1): the quads HybridComposer draws over the 3D frame. On the
// GPU path they are rasterised by the "hybrid.sprite_layer" compute kernel (sprite_layer_gpu.hpp,
// src/shaders/hybrid_sprite_layer.comp) into the premultiplied-RGBA image the E02 UI stage ("frame.ui_composite")
// alpha-overs onto the post-processed 3D output. This header is the layout both sides share and the kernel's CPU
// reference (the gate compares the two); it has no Vulkan dependency.
//
// Coverage is binary per pixel centre (x + 0.5, y + 0.5): the centre, rotated into the quad's frame, must satisfy
// |local.x| <= halfExtent.x and |local.y| <= halfExtent.y. Quads are composited in order (sprites first, then UI):
//   acc = quad.rgba + (1 - quad.a) * acc        (premultiplied "over", display space)
// The arithmetic is written without FMA contraction on both sides (`precise` in GLSL, -ffp-contract=off here).
#include <fuse/types.hpp>

namespace fuse::hybrid {

/// One quad (std430, 48 bytes).
struct SpriteQuad {
    f32 center[2] = {0.f, 0.f};     ///< pixels, origin top-left
    f32 halfExtent[2] = {0.f, 0.f}; ///< pixels
    f32 cosSin[2] = {1.f, 0.f};     ///< rotation (cos, sin) of the quad's local x axis
    f32 pad[2] = {0.f, 0.f};
    f32 color[4] = {0.f, 0.f, 0.f, 0.f}; ///< premultiplied RGBA, display space
};
static_assert(sizeof(SpriteQuad) == 48u, "SpriteQuad must match the kernel's std430 layout");

/// Reference extent of the placeholder / command-list coordinates (sprite x, y are pixels from the centre of a
/// 320 x 240 frame; a sprite is a 25 x 25 pixel square). spriteQuadFromPlaceholder scales them to any target.
inline constexpr u32 kPlaceholderWidth = 320u;
inline constexpr u32 kPlaceholderHeight = 240u;
inline constexpr f32 kPlaceholderSpriteHalfExtent = 12.5f;

/// Placeholder-space sprite (x, y from the frame centre, rotation in radians, RGB8 opaque) -> a quad of a
/// `width` x `height` target (uniform scale height / 240, centred).
SpriteQuad spriteQuadFromPlaceholder(f32 x, f32 y, f32 rotation, u8 r, u8 g, u8 b, u32 width, u32 height);

/// CPU reference of one texel of "hybrid.sprite_layer": premultiplied RGBA of pixel (px, py) after `count` quads.
void spriteLayerTexel(const SpriteQuad* quads, u32 count, u32 px, u32 py, f32 out[4]);

} // namespace fuse::hybrid
