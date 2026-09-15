#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Physical atmosphere constants for Rayleigh/Mie sky (B5.8 — P5 §5.8).
struct AtmosphereParams {
    f32 earth_radius = 6371000.f;   // metres
    f32 atmo_radius = 6471000.f;
    math::Vec3 rayleigh_coeff{5.8e-6f, 13.5e-6f, 33.1e-6f};
    f32 mie_coeff = 21e-6f;
    f32 rayleigh_scale_h = 8000.f;
    f32 mie_scale_h = 1200.f;
    f32 mie_scatter_dir = 0.758f;   // Henyey-Greenstein g
    u32 view_samples = 16;
    u32 light_samples = 8;
};

} // namespace fuse::renderer
