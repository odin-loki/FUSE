#include <fuse/renderer/atmosphere/transmittance_lut.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kMinCosZenith = 1e-4f;

f32 clampCosZenith(f32 cos_zenith) {
    return std::max(-1.f, std::min(1.f, cos_zenith));
}

f32 extinctionAtAltitude(f32 altitude_m, const AtmosphereParams& params) {
    const f32 height = std::max(0.f, altitude_m - params.earth_radius);
    const f32 rayleigh_density = std::exp(-height / params.rayleigh_scale_h);
    const f32 mie_density = std::exp(-height / params.mie_scale_h);
    const f32 rayleigh_ext =
        params.rayleigh_coeff.x + params.rayleigh_coeff.y + params.rayleigh_coeff.z;
    return rayleigh_ext * rayleigh_density + params.mie_coeff * mie_density;
}

} // namespace

usize transmittance_lut_flat_index(u32 alt_bin, u32 cos_bin, u32 cos_bins) {
    return static_cast<usize>(alt_bin) * static_cast<usize>(cos_bins) + static_cast<usize>(cos_bin);
}

usize transmittance_lut_entry_count(u32 alt_bins, u32 cos_bins) {
    return static_cast<usize>(std::max(2u, alt_bins)) * static_cast<usize>(std::max(2u, cos_bins));
}

bool transmittance_lut_decode_index(usize flat_index, u32 alt_bins, u32 cos_bins, u32& alt_bin,
                                    u32& cos_bin) {
    if (alt_bins < 2u || cos_bins < 2u) {
        return false;
    }

    const usize total = transmittance_lut_entry_count(alt_bins, cos_bins);
    if (flat_index >= total) {
        return false;
    }

    alt_bin = static_cast<u32>(flat_index / static_cast<usize>(cos_bins));
    cos_bin = static_cast<u32>(flat_index % static_cast<usize>(cos_bins));
    return alt_bin < alt_bins;
}

TransmittanceLutIndex transmittance_lut_index_from_samples(f32 altitude_m, f32 cos_zenith,
                                                           const TransmittanceLutDesc& desc) {
    TransmittanceLutIndex result{};

    const u32 alt_bins = std::max(2u, desc.altitude_bins);
    const u32 cos_bins = std::max(2u, desc.cos_zenith_bins);

    const f32 min_alt = desc.atmosphere.earth_radius;
    const f32 max_alt = desc.atmosphere.atmo_radius;
    const f32 alt_t = std::max(0.f, std::min(1.f, (altitude_m - min_alt) / (max_alt - min_alt)));
    const f32 cos_t = (clampCosZenith(cos_zenith) + 1.f) * 0.5f;

    result.alt_bin = static_cast<u32>(alt_t * static_cast<f32>(alt_bins - 1u));
    result.cos_bin = static_cast<u32>(cos_t * static_cast<f32>(cos_bins - 1u));
    result.flat_index = transmittance_lut_flat_index(result.alt_bin, result.cos_bin, cos_bins);
    return result;
}

f32 transmittance_lut_altitude_for_bin(u32 alt_bin, const TransmittanceLutDesc& desc) {
    const u32 alt_bins = std::max(2u, desc.altitude_bins);
    const f32 alt_t = static_cast<f32>(std::min(alt_bin, alt_bins - 1u)) / static_cast<f32>(alt_bins - 1u);
    const f32 min_alt = desc.atmosphere.earth_radius;
    const f32 max_alt = desc.atmosphere.atmo_radius;
    return min_alt + alt_t * (max_alt - min_alt);
}

f32 transmittance_lut_cos_zenith_for_bin(u32 cos_bin, const TransmittanceLutDesc& desc) {
    const u32 cos_bins = std::max(2u, desc.cos_zenith_bins);
    const f32 cos_t = static_cast<f32>(std::min(cos_bin, cos_bins - 1u)) / static_cast<f32>(cos_bins - 1u);
    return cos_t * 2.f - 1.f;
}

math::Vec3 compute_transmittance(f32 altitude_m, f32 cos_zenith, const AtmosphereParams& params) {
    const f32 cos_clamped = std::max(kMinCosZenith, std::fabs(clampCosZenith(cos_zenith)));
    const f32 path_length = (params.atmo_radius - std::max(params.earth_radius, altitude_m)) / cos_clamped;
    const f32 extinction = extinctionAtAltitude(altitude_m, params);
    const f32 optical_depth = extinction * path_length;

    const f32 transmittance = std::exp(-optical_depth);
    const f32 rayleigh_weight = params.rayleigh_coeff.z /
                                std::max(1e-8f, params.rayleigh_coeff.x + params.rayleigh_coeff.y +
                                                     params.rayleigh_coeff.z);
    const f32 mie_weight = params.mie_coeff / std::max(1e-8f, extinction);

    return math::Vec3{transmittance * rayleigh_weight, transmittance,
                        transmittance * (1.f - 0.5f * mie_weight)};
}

bool TransmittanceLut::build(const TransmittanceLutDesc& desc) {
    m_desc = desc;
    m_desc.altitude_bins = std::max(2u, desc.altitude_bins);
    m_desc.cos_zenith_bins = std::max(2u, desc.cos_zenith_bins);

    const usize entry_count = transmittance_lut_entry_count(m_desc.altitude_bins, m_desc.cos_zenith_bins);
    m_entries.assign(entry_count, math::Vec3{});

    for (u32 alt_bin = 0; alt_bin < m_desc.altitude_bins; ++alt_bin) {
        const f32 altitude = transmittance_lut_altitude_for_bin(alt_bin, m_desc);
        for (u32 cos_bin = 0; cos_bin < m_desc.cos_zenith_bins; ++cos_bin) {
            const f32 cos_zenith = transmittance_lut_cos_zenith_for_bin(cos_bin, m_desc);
            m_entries[transmittance_lut_flat_index(alt_bin, cos_bin, m_desc.cos_zenith_bins)] =
                compute_transmittance(altitude, cos_zenith, m_desc.atmosphere);
        }
    }

    m_ready = !m_entries.empty();
    return m_ready;
}

math::Vec3 TransmittanceLut::sample(f32 altitude_m, f32 cos_zenith) const {
    if (!m_ready || m_entries.empty()) {
        return {};
    }

    const TransmittanceLutIndex index = transmittance_lut_index_from_samples(altitude_m, cos_zenith, m_desc);
    return m_entries[index.flat_index];
}

} // namespace fuse::renderer
