#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Precomputed sun/sky lookup table dimensions (stub — no GPU upload yet).
struct SkyLutDesc {
    u32 sun_elevation_bins = 32;
    u32 view_elevation_bins = 32;
    AtmosphereParams atmosphere{};
};

/// CPU sun/sky LUT — caches `compute_sky_colour` for sun elevation × view elevation, both on linear
/// bins over [-90°, 90°]. View rays look along +z (azimuth 0); the table sun keeps the azimuth of the
/// `sun_direction` passed to `build`.
class SkyLut {
public:
    bool build(const SkyLutDesc& desc, const math::Vec3& sun_direction);

    bool isReady() const { return m_ready; }
    const SkyLutDesc& desc() const { return m_desc; }
    u32 sunElevationBins() const { return m_desc.sun_elevation_bins; }
    u32 viewElevationBins() const { return m_desc.view_elevation_bins; }
    const std::vector<math::Vec3>& entries() const { return m_entries; }

    /// Sample LUT by sun and view elevation in radians (clamped to table range, nearest bin).
    math::Vec3 sample(f32 sun_elevation_rad, f32 view_elevation_rad) const;

private:
    SkyLutDesc m_desc{};
    std::vector<math::Vec3> m_entries;
    bool m_ready = false;
};

} // namespace fuse::renderer
