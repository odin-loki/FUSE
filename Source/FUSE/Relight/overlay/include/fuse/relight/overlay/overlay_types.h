// FUSE Relight RL-6.1: the developer overlay's GPU interface, shared by C++ and the compose shaders.
//
// shaders/overlay_compose.comp (GLSL) and shaders/overlay_compose.slang declare the same push-constant block member
// for member; rl_overlay_layout (tests) parses both and checks every offset against this struct.
//
// Bindings (set 0):  0  region   r32ui storage image: the back buffer's texels as raw 32-bit words
//                    1  layer    std430 uint[]: the UI layer, premultiplied RGBA8 (raster.hpp), panelW * panelH
//                    2  debug    combined image sampler: the debug buffer (texelFetch, nearest); a 1x1 dummy image
//                                when no debug view is shown
#pragma once

#include <cstdint>

namespace fuse::relight::overlay {

inline constexpr std::uint32_t kOverlayFlagBgra = 1u;  ///< the back buffer is B8G8R8A8 (else R8G8B8A8)
inline constexpr std::uint32_t kOverlayFlagDebug = 2u; ///< the region shows the debug buffer under the layer

inline constexpr std::uint32_t kOverlaySwizzleRgb = 0u; ///< rgb
inline constexpr std::uint32_t kOverlaySwizzleRrr = 1u; ///< r as grey (depth)
inline constexpr std::uint32_t kOverlaySwizzleRg0 = 2u; ///< r, g, 0 (motion)

inline constexpr std::uint32_t kOverlayGroupSize = 8u; ///< local_size_x = local_size_y

struct OverlayPush {
    std::int32_t regionX = 0, regionY = 0;   ///< region origin in the back buffer
    std::uint32_t regionW = 0, regionH = 0;  ///< region extent (the storage image's used part)
    std::int32_t panelX = 0, panelY = 0;     ///< layer origin in the back buffer
    std::uint32_t panelW = 0, panelH = 0;    ///< layer extent (0: no layer)
    std::uint32_t frameW = 0, frameH = 0;    ///< back-buffer extent
    std::uint32_t flags = 0;                 ///< kOverlayFlag*
    std::uint32_t debugSwizzle = 0;          ///< kOverlaySwizzle*
    std::uint32_t debugW = 0, debugH = 0;    ///< debug buffer extent
    std::uint32_t pad0 = 0, pad1 = 0;
    float debugScale[4] = {1.f, 1.f, 1.f, 1.f};
    float debugBias[4] = {0.f, 0.f, 0.f, 0.f};
};
static_assert(sizeof(OverlayPush) == 96, "OverlayPush must match the shaders' push-constant block");

} // namespace fuse::relight::overlay
