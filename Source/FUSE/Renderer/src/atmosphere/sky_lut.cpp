#include <fuse/renderer/atmosphere/sky_lut.hpp>
#include <fuse/renderer/atmosphere/sky_scatter.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kHalfPi = kPi * 0.5f;

f32 elevationFromDirection(const math::Vec3& direction) {
    return std::asin(std::max(-1.f, std::min(1.f, direction.y)));
}

usize lutIndex(u32 sun_bin, u32 view_bin, u32 view_bins) {
    return static_cast<usize>(sun_bin) * static_cast<usize>(view_bins) + static_cast<usize>(view_bin);
}

} // namespace

bool SkyLut::build(const SkyLutDesc& desc, const math::Vec3& sun_direction) {
    m_desc = desc;
    m_desc.sun_elevation_bins = std::max(2u, desc.sun_elevation_bins);
    m_desc.view_elevation_bins = std::max(2u, desc.view_elevation_bins);

    const usize entry_count =
        static_cast<usize>(m_desc.sun_elevation_bins) * static_cast<usize>(m_desc.view_elevation_bins);
    m_entries.assign(entry_count, math::Vec3{});

    const math::Vec3 sun_dir = sun_direction.normalized();
    const f32 sun_elevation = elevationFromDirection(sun_dir);
    const f32 sun_azimuth = std::atan2(sun_dir.x, sun_dir.z);

    for (u32 sun_bin = 0; sun_bin < m_desc.sun_elevation_bins; ++sun_bin) {
        const f32 sun_t = static_cast<f32>(sun_bin) / static_cast<f32>(m_desc.sun_elevation_bins - 1u);
        const f32 table_sun_elevation = -kHalfPi + sun_t * kPi;

        const f32 cos_sun_el = std::cos(table_sun_elevation);
        const f32 sin_sun_el = std::sin(table_sun_elevation);
        const math::Vec3 table_sun_dir{sin_sun_el * std::sin(sun_azimuth), cos_sun_el,
                                       sin_sun_el * std::cos(sun_azimuth)};

        for (u32 view_bin = 0; view_bin < m_desc.view_elevation_bins; ++view_bin) {
            const f32 view_t = static_cast<f32>(view_bin) / static_cast<f32>(m_desc.view_elevation_bins - 1u);
            const f32 view_elevation = -kHalfPi + view_t * kPi;
            const math::Vec3 view_dir{0.f, std::sin(view_elevation), std::cos(view_elevation)};

            m_entries[lutIndex(sun_bin, view_bin, m_desc.view_elevation_bins)] =
                compute_sky_colour({}, view_dir, table_sun_dir, m_desc.atmosphere);
        }
    }

    m_ready = !m_entries.empty();
    return m_ready;
}

math::Vec3 SkyLut::sample(f32 sun_elevation_rad, f32 view_elevation_rad) const {
    if (!m_ready || m_entries.empty()) {
        return {};
    }

    const f32 sun_t = std::max(0.f, std::min(1.f, (sun_elevation_rad + kHalfPi) / kPi));
    const f32 view_t = std::max(0.f, std::min(1.f, (view_elevation_rad + kHalfPi) / kPi));

    const u32 sun_bin = static_cast<u32>(sun_t * static_cast<f32>(m_desc.sun_elevation_bins - 1u));
    const u32 view_bin = static_cast<u32>(view_t * static_cast<f32>(m_desc.view_elevation_bins - 1u));

    return m_entries[lutIndex(sun_bin, view_bin, m_desc.view_elevation_bins)];
}

} // namespace fuse::renderer
