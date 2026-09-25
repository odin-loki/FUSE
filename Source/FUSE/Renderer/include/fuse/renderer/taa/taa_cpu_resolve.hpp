#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// RGB -> YCoCg (x = Y, y = Co, z = Cg) used for neighbourhood clipping.
math::Vec3 taaRgbToYCoCg(const math::Vec3& rgb);
math::Vec3 taaYCoCgToRgb(const math::Vec3& ycocg);

/// Clip `history` towards the AABB centre `(boxMin + boxMax) / 2` so it lies inside the box (Karis/Playdead).
math::Vec3 taaClipToAabb(const math::Vec3& history, const math::Vec3& boxMin, const math::Vec3& boxMax);

/// Catmull-Rom (16-tap) sample of an RGB image at continuous pixel coordinates (edge-clamped).
math::Vec3 taaSampleCatmullRom(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py);
/// Bilinear sample of an RGB image at continuous pixel coordinates (edge-clamped).
math::Vec3 taaSampleBilinear(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py);

/// Inputs for one CPU reference resolve. All arrays are row-major `width * height`.
struct TaaCpuFrameInputs {
    /// Jittered current-frame linear RGB.
    const math::Vec3* current = nullptr;
    /// Per-pixel screen motion in pixels (current position - previous position, jitter excluded).
    /// Null means a static scene and camera.
    const math::Vec2* velocity = nullptr;
    /// Linear view depth of the current frame. Null disables depth (disocclusion) rejection.
    const f32* depth = nullptr;
    u32 width = 0;
    u32 height = 0;
};

/// CPU-resolve policy knobs that are not part of the GPU-facing `TAAParams`.
struct TaaCpuResolveOptions {
    /// YCoCg variance clipping of history against the current 3x3 neighbourhood.
    bool neighborhood_clip = true;
    /// Use the velocity of the closest-depth 3x3 neighbour (keeps moving edges anti-aliased).
    bool dilate_velocity = true;
    /// Motion (px/frame, max of own speed and velocity disagreement) at which clipping reaches full strength.
    /// Static pixels relax the clip so jittered sub-pixel detail accumulates instead of flickering; moving
    /// content and disoccluded pixels are clipped fully.
    f32 clip_full_motion_px = 0.25f;
    /// Clip strength floor applied even to perfectly static pixels.
    f32 static_clip_strength = 0.f;
    /// Velocity disagreement (px) between current and reprojected history motion that fully rejects history
    /// when `TAAParams::velocity_rejection == 1`.
    f32 velocity_disagreement_px = 1.f;
};

/// Bookkeeping from the last CPU resolve.
struct TaaCpuResolveStats {
    bool resolved = false;
    bool first_frame = false;
    u32 offscreen_rejections = 0;
    u32 depth_rejections = 0;
    /// Pixels whose velocity-disagreement weight exceeded 0.5.
    u32 velocity_rejections = 0;
    /// Pixels whose history moved by more than 1e-3 when clipped.
    u32 clipped_pixels = 0;
};

/// CPU reference implementation of `taa_resolve_kernel` (B5.9): Halton-jittered accumulation with
/// velocity reprojection, Catmull-Rom history sampling, YCoCg variance clipping (`clamp_gamma`), depth-based
/// disocclusion rejection (`depth_rejection`, relative) and velocity-disagreement rejection
/// (`velocity_rejection`). Owns its own history colour/velocity/depth images.
class TaaCpuResolver {
public:
    explicit TaaCpuResolver(const TaaCpuResolveOptions& options = {}) : m_options(options) {}

    /// Allocates history for `width x height`; invalidates history when dimensions change.
    bool resize(u32 width, u32 height);
    void invalidate();

    /// Resolve one frame into `output` (`width * height`) and update history. False on bad input.
    bool resolve(const TaaCpuFrameInputs& inputs, const TAAParams& params, math::Vec3* output);

    bool hasHistory() const { return m_valid; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const std::vector<math::Vec3>& history() const { return m_history; }
    const TaaCpuResolveStats& lastStats() const { return m_stats; }
    TaaCpuResolveOptions& options() { return m_options; }
    const TaaCpuResolveOptions& options() const { return m_options; }

private:
    TaaCpuResolveOptions m_options{};
    TaaCpuResolveStats m_stats{};
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_valid = false;
    std::vector<math::Vec3> m_history;
    /// Dilated velocity used by each pixel of the previous resolve (history motion for disagreement tests).
    std::vector<math::Vec2> m_historyVelocity;
    std::vector<math::Vec2> m_frameVelocity;
    std::vector<f32> m_historyDepth;
    bool m_historyHasDepth = false;
};

} // namespace fuse::renderer
