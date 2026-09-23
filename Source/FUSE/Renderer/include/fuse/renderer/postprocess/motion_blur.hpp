#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Motion-blur parameters (B5.10 — P5 §5.10).
struct MotionBlurParams {
    u32 max_samples = 16;
    f32 shutter_angle = 180.f;
    f32 max_blur_px = 32.f;
    /// Depth range (metres) over which foreground/background classification blends.
    f32 soft_depth_extent = 0.5f;
    bool enabled = true;
};

/// Screen-space blur vector (pixels) for a per-frame velocity (pixels/frame): v * shutter/360,
/// length clamped to `max_blur_px`. The blur is centred on the pixel (half on each side).
fuse::math::Vec2 motion_blur_vector(const fuse::math::Vec2& velocity_px, const MotionBlurParams& params);

/// Tile size (pixels) used for the tile-max / neighbour-max velocity reduction.
u32 motion_blur_tile_size(const MotionBlurParams& params);

/// Neighbour-max half blur vector per tile (max-length half vector over the 3x3 tile neighbourhood).
void motion_blur_neighbor_max(const fuse::math::Vec2* velocity_px, u32 width, u32 height,
                              const MotionBlurParams& params, std::vector<fuse::math::Vec2>& out_tiles,
                              u32& tiles_x, u32& tiles_y);

/// CPU reference for the reconstruction-filter motion blur (tile max + neighbour max, depth-aware
/// cone/cylinder weights). Each pixel gathers `max_samples` taps along its neighbour-max vector, so
/// a moving object smears into the static pixels it swept over and nowhere else. Pixels whose
/// neighbourhood has no motion are copied unchanged. `velocity_px` is in pixels per frame;
/// `linear_depth_m` may be null (everything treated as the same depth).
void motion_blur_pass(const fuse::math::Vec3* color, const fuse::math::Vec2* velocity_px, const f32* linear_depth_m,
                      u32 width, u32 height, const MotionBlurParams& params, std::vector<fuse::math::Vec3>& out);

} // namespace fuse::renderer
