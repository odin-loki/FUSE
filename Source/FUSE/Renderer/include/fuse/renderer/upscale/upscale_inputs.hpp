#pragma once

// Render / display resolution split and the per-frame input contract of a temporal upscaler
// (docs/research/upscaling-framegen-and-post-injectors.md, recommendations 1-3).
//
// Everything a temporal upscaler (native TAAU, FSR 2/3, DLSS SR, XeSS) consumes is gathered in
// `UpscaleInputs`: the jittered render-resolution colour, depth, motion vectors (dynamic objects included),
// the exposure, reactive / transparency-composition masks, the unjittered camera, the jitter and the
// display-resolution UI target that is composited AFTER the upscale (never fed to the resolve).
//
// Conventions (all images row-major, row 0 = top, texel centres at integer + 0.5):
//   * jitter      `jitter_px` is the sub-pixel offset of every render sample from its pixel centre, in RENDER
//                 pixels, +x right, +y down, in [-0.5, 0.5). Render pixel (i, j) samples the (unjittered) scene
//                 at continuous render coordinate (i + 0.5 + jitter.x, j + 0.5 + jitter.y), so the jittered
//                 projection moves every point by -jitter render pixels. In Vulkan clip space (NDC y down,
//                 pixel = (ndc + 1) / 2 * size with a positive-height viewport) that is the NDC offset
//                 (-2 jitter.x / render_w, -2 jitter.y / render_h) (`upscaleJitterNdc`), i.e. the matrix
//                 temporal::jitter_view_proj builds. Every jitter consumer (IJitterProvider::offset_ndc, the
//                 WP-4.1 visibility buffer, FSR 3's projection_jitter_ndc) uses this one offset; gate
//                 fuse_rp_fsr3_jitter pins them against each other.
//   * motion      `motion` is the screen motion of the surface seen by each render pixel in UV units
//                 (current_uv - previous_uv, both from UNJITTERED projections; uv (0,0) = top-left,
//                 (1,1) = bottom-right). It covers static geometry (camera motion), dynamic / skinned objects
//                 (object motion + camera) and the sky (camera rotation only, the sky is at infinity).
//                 FSR 2/3 want previous - current in render pixels: `mv_fsr = -motion * render_size`; DLSS takes
//                 `mvecScale = (-render_w, -render_h)` for the same buffer.
//   * depth       linear view depth (> 0, metres along the camera forward axis). 0 (or any value <= 0) marks
//                 the sky / no geometry. Device (hyperbolic) depth for GPU backends is derived from
//                 `camera.near_plane` / `far_plane`.
//   * colour      linear, scene-referred RGB. `exposure` maps it to display-referred values (colour * exposure
//                 ~ [0, 1] before tone mapping); an upscaler normalises by it so its clamps/weights behave the
//                 same at any scene brightness (FSR "exposure" texture / DLSS "exposure" scale).
//   * reactive    [0, 1] per render pixel: how much the pixel must trust the current frame (particles, animated
//                 textures, alpha-blended surfaces that do not write motion). 1 = ignore history.
//   * transparency_composition [0, 1] per render pixel: coverage of transparent / composited layers
//                 (FSR "transparency and composition mask"): history is kept but clamped harder.
//   * ui          display-resolution premultiplied RGBA HUD, composited after the upscale with
//                 `compositeUpscaledWithUi`; a frame-generation backend receives it as the "UI colour + alpha".
//
// Texture LOD bias: textures sampled while rendering at render resolution must use
//     mip_bias = log2(render_width / display_width) - 1        (e.g. -1.58 at 1.5x, -2.0 at 2x)
// so each render sample fetches detail at display density; the `-1` sharpens further because the temporal
// accumulation integrates many jittered samples per display pixel (FSR 2 integration guide recommendation;
// DLSS recommends the same log2 term with a ~0 offset). `upscaleTextureMipBias` computes it; the reference
// scenes apply it to their filtered procedural textures (footprint scale 2^mip_bias).

#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Render vs display resolution for one view.
struct UpscaleResolution {
    u32 render_width = 0;
    u32 render_height = 0;
    u32 display_width = 0;
    u32 display_height = 0;

    bool valid() const {
        return render_width != 0u && render_height != 0u && display_width != 0u && display_height != 0u &&
               render_width <= display_width && render_height <= display_height;
    }
    /// display / render per axis (>= 1).
    f32 scaleX() const { return static_cast<f32>(display_width) / static_cast<f32>(render_width); }
    f32 scaleY() const { return static_cast<f32>(display_height) / static_cast<f32>(render_height); }
    bool isNative() const { return render_width == display_width && render_height == display_height; }
};

/// Standard per-axis upscale ratios (FSR 2/3 / DLSS quality modes).
enum class UpscaleQualityMode : u8 {
    NativeAA = 0,         ///< 1.0x (TAA / DLAA)
    UltraQuality = 1,     ///< 1.3x
    Quality = 2,          ///< 1.5x
    Balanced = 3,         ///< 1.7x
    Performance = 4,      ///< 2.0x
    UltraPerformance = 5, ///< 3.0x
};

f32 upscaleRatioForMode(UpscaleQualityMode mode);
const char* upscaleQualityModeLabel(UpscaleQualityMode mode);

/// Render resolution for a display resolution and per-axis ratio (>= 1): round(display / ratio), at least 1.
UpscaleResolution makeUpscaleResolution(u32 displayWidth, u32 displayHeight, f32 ratio);
UpscaleResolution makeUpscaleResolution(u32 displayWidth, u32 displayHeight, UpscaleQualityMode mode);

/// Texture mip bias for rendering at render resolution: log2(render_width / display_width) + `offset`
/// (offset -1 by default; see the header comment). 0 + offset at native resolution.
f32 upscaleTextureMipBias(const UpscaleResolution& resolution, f32 offset = -1.f);

/// Jitter phase count for a ratio: ceil(8 * (display_w / render_w)^2) (FSR 2 "base phase count 8 scaled by
/// the upscale area"), so every display pixel receives several samples per cycle. 8 at native resolution.
u32 upscaleJitterPhaseCount(const UpscaleResolution& resolution);

/// Halton(2, 3) jitter for `frameIndex` (wraps over `phaseCount`), in render pixels in [-0.5, 0.5).
/// Index 0 of the cycle uses Halton index 1 (Halton index 0 is the degenerate (0, 0)).
math::Vec2 upscaleJitterOffset(u32 frameIndex, u32 phaseCount);

/// NDC translation for a render-pixel jitter (Vulkan NDC y down): (-2 jx / w, -2 jy / h) — moves every point by
/// -jitter pixels (the sample convention above); jitterProjection(P, upscaleJitterNdc(j)) == jitter_view_proj(P, j).
math::Vec2 upscaleJitterNdc(const math::Vec2& jitterPx, u32 renderWidth, u32 renderHeight);

/// Applies an NDC jitter to any projection matrix (column-major, clip = P * view): clip.xy += ndc * clip.w,
/// i.e. row0 += ndc.x * row3 and row1 += ndc.y * row3. Works for perspective and orthographic projections.
math::Mat4 jitterProjection(const math::Mat4& projection, const math::Vec2& jitterNdc);

/// Unjittered camera of one frame (what DLSS / FSR call "camera matrices without jitter").
struct UpscaleCamera {
    math::Mat4 view{};       ///< World -> view (right-handed, camera looks down -Z).
    math::Mat4 projection{}; ///< View -> clip, UNJITTERED.
    math::Vec3 position{};
    f32 vertical_fov_rad = 1.f;
    f32 aspect = 1.f; ///< width / height
    f32 near_plane = 0.05f;
    f32 far_plane = 1000.f;
};

/// Per-frame input contract of a temporal upscaler (see the header comment for units and conventions).
/// Pointers are non-owning host arrays; optional inputs may be null.
struct UpscaleInputs {
    UpscaleResolution resolution{};

    // Render resolution (render_width * render_height).
    const math::Vec3* color = nullptr;                 ///< Required. Jittered linear scene colour.
    const f32* depth = nullptr;                        ///< Required. Linear view depth, <= 0 = sky.
    const math::Vec2* motion = nullptr;                ///< Required. UV motion (current - previous), unjittered.
    const f32* reactive = nullptr;                     ///< Optional reactive mask [0, 1].
    const f32* transparency_composition = nullptr;     ///< Optional transparency / composition mask [0, 1].

    // Display resolution (display_width * display_height).
    const math::Vec4* ui = nullptr; ///< Optional premultiplied RGBA HUD, composited after the upscale.

    math::Vec2 jitter_px{};   ///< Render-pixel jitter of this frame.
    u32 jitter_phase = 0;     ///< Index inside the jitter cycle.
    u32 jitter_phase_count = 8;
    f32 exposure = 1.f;       ///< colour * exposure = display-referred (pre tone map).
    f32 mip_bias = 0.f;       ///< Texture LOD bias the frame was rendered with.
    f32 frame_time_s = 1.f / 60.f;
    u32 frame_index = 0;
    bool reset_history = false; ///< Camera cut / teleport: drop all history.

    UpscaleCamera camera{};          ///< This frame, unjittered.
    UpscaleCamera previous_camera{}; ///< Previous frame, unjittered.

    usize renderPixelCount() const {
        return static_cast<usize>(resolution.render_width) * resolution.render_height;
    }
    usize displayPixelCount() const {
        return static_cast<usize>(resolution.display_width) * resolution.display_height;
    }
};

/// Why `UpscaleInputs` validation failed.
enum class UpscaleInputsError : u8 {
    None = 0,
    InvalidResolution,
    MissingColor,
    MissingDepth,
    MissingMotion,
    InvalidJitter,
    InvalidExposure,
};
const char* upscaleInputsErrorLabel(UpscaleInputsError error);
/// Structural validation (resolution, required surfaces, jitter range, exposure > 0 and finite).
UpscaleInputsError validateUpscaleInputs(const UpscaleInputs& inputs);

/// Camera-only UV motion of a world point (static geometry): uv(current, unjittered) - uv(previous).
/// `worldPos` must be in front of both cameras; returns false otherwise.
bool upscaleStaticMotion(const UpscaleCamera& current, const UpscaleCamera& previous, const math::Vec3& worldPos,
                         math::Vec2& outMotion);
/// UV of a world point under an (unjittered) camera; false when behind the camera.
bool upscaleProjectToUv(const UpscaleCamera& camera, const math::Vec3& worldPos, math::Vec2& outUv);
/// Sky / infinitely distant direction: rotation-only motion (camera translation does not move the sky).
bool upscaleSkyMotion(const UpscaleCamera& current, const UpscaleCamera& previous, const math::Vec3& worldDir,
                      math::Vec2& outMotion);

/// Composites the display-resolution premultiplied UI over the upscaled scene: out = ui.rgb + scene * (1 - ui.a).
/// `scene` and `out` may alias. Null `ui` copies `scene`.
void compositeUpscaledWithUi(const math::Vec3* scene, const math::Vec4* ui, u32 width, u32 height, math::Vec3* out);

} // namespace fuse::renderer
