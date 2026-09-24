#include <fuse/renderer/atmosphere/transmittance_lut.hpp>
#include <fuse/renderer/atmosphere/sky_scatter.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

/// Log-linear segments per transmittance ray (LUT build and CPU reference).
constexpr u32 kTransmittanceSteps = 128u;

f32 clampCosZenith(f32 cos_zenith) {
    return std::max(-1.f, std::min(1.f, cos_zenith));
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

math::Vec3 compute_transmittance(f32 radius_m, f32 cos_zenith, const AtmosphereParams& params) {
    const f32 radius = std::max(params.earth_radius, std::min(params.atmo_radius, radius_m));
    return atmosphere_transmittance_to_top(radius, clampCosZenith(cos_zenith), kTransmittanceSteps, params);
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

math::Vec3 TransmittanceLut::sampleBilinear(f32 altitude_m, f32 cos_zenith) const {
    if (!m_ready || m_entries.empty()) {
        return {};
    }
    const u32 alt_bins = m_desc.altitude_bins;
    const u32 cos_bins = m_desc.cos_zenith_bins;
    const f32 min_alt = m_desc.atmosphere.earth_radius;
    const f32 max_alt = m_desc.atmosphere.atmo_radius;
    const f32 alt_t = std::max(0.f, std::min(1.f, (altitude_m - min_alt) / (max_alt - min_alt)));
    const f32 cos_t = (clampCosZenith(cos_zenith) + 1.f) * 0.5f;
    const f32 fa = alt_t * static_cast<f32>(alt_bins - 1u);
    const f32 fc = cos_t * static_cast<f32>(cos_bins - 1u);
    const u32 a0 = std::min(static_cast<u32>(fa), alt_bins - 2u);
    const u32 c0 = std::min(static_cast<u32>(fc), cos_bins - 2u);
    const f32 ta = fa - static_cast<f32>(a0);
    const f32 tc = fc - static_cast<f32>(c0);
    const math::Vec3& v00 = m_entries[transmittance_lut_flat_index(a0, c0, cos_bins)];
    const math::Vec3& v01 = m_entries[transmittance_lut_flat_index(a0, c0 + 1u, cos_bins)];
    const math::Vec3& v10 = m_entries[transmittance_lut_flat_index(a0 + 1u, c0, cos_bins)];
    const math::Vec3& v11 = m_entries[transmittance_lut_flat_index(a0 + 1u, c0 + 1u, cos_bins)];
    const math::Vec3 low = v00 + (v01 - v00) * tc;
    const math::Vec3 high = v10 + (v11 - v10) * tc;
    return low + (high - low) * ta;
}

} // namespace fuse::renderer
