#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// 2D transmittance LUT layout — altitude × cos(zenith angle).
struct TransmittanceLutDesc {
    u32 altitude_bins = 32;
    u32 cos_zenith_bins = 32;
    AtmosphereParams atmosphere{};
};

/// Resolved bin coordinates for a transmittance LUT sample.
struct TransmittanceLutIndex {
    u32 alt_bin = 0;
    u32 cos_bin = 0;
    usize flat_index = 0;
};

/// Flat row-major index: `alt_bin * cos_bins + cos_bin`.
usize transmittance_lut_flat_index(u32 alt_bin, u32 cos_bin, u32 cos_bins);

/// Total entry count for a 2D transmittance table.
usize transmittance_lut_entry_count(u32 alt_bins, u32 cos_bins);

/// Decode a flat index back into `(alt_bin, cos_bin)` — returns false when out of range.
bool transmittance_lut_decode_index(usize flat_index, u32 alt_bins, u32 cos_bins, u32& alt_bin, u32& cos_bin);

/// Map physical samples to clamped bin indices (nearest bin). `altitude_m` is the radial distance
/// from the planet centre in `[earth_radius, atmo_radius]`, matching the bin axis.
TransmittanceLutIndex transmittance_lut_index_from_samples(f32 altitude_m, f32 cos_zenith,
                                                           const TransmittanceLutDesc& desc);

/// Altitude in metres for a bin centre in `[earth_radius, atmo_radius]`.
f32 transmittance_lut_altitude_for_bin(u32 alt_bin, const TransmittanceLutDesc& desc);

/// cos(zenith) in `[-1, 1]` for a bin centre.
f32 transmittance_lut_cos_zenith_for_bin(u32 cos_bin, const TransmittanceLutDesc& desc);

/// CPU reference transmittance from `radius_m` (distance from the planet centre, the same axis the
/// LUT altitude bins use) along cos(zenith) to the top of the atmosphere, integrated through the
/// spherical exponential Rayleigh + Mie shells. Zero when the planet occludes the ray.
math::Vec3 compute_transmittance(f32 radius_m, f32 cos_zenith, const AtmosphereParams& params);

/// Precomputed transmittance LUT — caches analytic samples for altitude × cos(zenith).
class TransmittanceLut {
public:
    bool build(const TransmittanceLutDesc& desc);

    bool isReady() const { return m_ready; }
    const TransmittanceLutDesc& desc() const { return m_desc; }
    u32 altitudeBins() const { return m_desc.altitude_bins; }
    u32 cosZenithBins() const { return m_desc.cos_zenith_bins; }
    const std::vector<math::Vec3>& entries() const { return m_entries; }

    /// Sample LUT by radial distance from the planet centre (metres) and cos(zenith), nearest bin.
    math::Vec3 sample(f32 altitude_m, f32 cos_zenith) const;
    /// Bilinear sample between the four surrounding bins (bin i sits at i / (bins - 1) of each axis; clamped).
    math::Vec3 sampleBilinear(f32 altitude_m, f32 cos_zenith) const;

private:
    TransmittanceLutDesc m_desc{};
    std::vector<math::Vec3> m_entries;
    bool m_ready = false;
};

} // namespace fuse::renderer
