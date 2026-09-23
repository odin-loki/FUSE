#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Depth-of-field parameters (B5.10 — P5 §5.10). Distances in metres, lens/sensor in millimetres.
struct DOFParams {
    f32 focal_distance = 10.f;
    f32 focal_length = 50.f;
    f32 f_stop = 2.8f;
    f32 sensor_width = 36.f;
    u32 bokeh_blades = 6;
    bool near_blur = true;
    /// Clamp on the blur radius the pass will gather (pixels).
    f32 max_coc_radius_px = 32.f;
    bool enabled = true;
};

/// Aperture diameter A = f / N in millimetres.
f32 dof_aperture_diameter_mm(const DOFParams& params);

/// Signed thin-lens circle-of-confusion diameter on the sensor (mm) for an object at
/// `object_distance_m`, lens focused at `params.focal_distance`:
///   CoC = A f (S2 - S1) / (S2 (S1 - f))   (S1 = focus distance, S2 = object distance)
/// Sign convention: negative = near field (object in front of the focal plane), positive = far
/// field, zero exactly at the focus distance. |CoC| is the textbook |A f (S1 - S2)| / (S2 (S1 - f)).
f32 dof_coc_diameter_mm(f32 object_distance_m, const DOFParams& params);

/// Signed CoC radius in pixels: 0.5 * CoC_mm / sensor_width_mm * image_width_px. Near-field CoC is
/// forced to zero when `near_blur` is false. Not clamped (see `dof_pass`).
f32 dof_coc_radius_px(f32 object_distance_m, const DOFParams& params, u32 image_width_px);

/// Linear view distance (metres) from a stored depth value.
///  - reversed_z && far is infinite (far <= 0): z = near / d   (engine default projection)
///  - reversed_z finite:  z = far * near / (near + d (far - near))
///  - forward [0,1]:      z = far * near / (far - d (far - near))
f32 dof_linear_depth(f32 stored_depth, f32 near_plane, f32 far_plane, bool reversed_z);

/// CPU reference for the DoF pass: every pixel spreads its colour uniformly over a disc of radius
/// |CoC| (clamped to max_coc_radius_px); each output pixel is the normalised sum of the discs that
/// cover it. In-focus pixels (radius < 0.5 px) only cover themselves, so an in-focus frame passes
/// through unchanged. `linear_depth_m` is the linear view distance per pixel.
void dof_pass(const fuse::math::Vec3* color, const f32* linear_depth_m, u32 width, u32 height,
              const DOFParams& params, std::vector<fuse::math::Vec3>& out);

} // namespace fuse::renderer
