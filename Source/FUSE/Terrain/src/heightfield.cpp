#include <fuse/terrain/heightfield.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

void Heightfield::init(const TerrainDesc& desc) {
    destroy();
    m_desc = desc;
    resize(desc.resolution);
    m_initialized = true;
}

void Heightfield::destroy() {
    m_heights.clear();
    m_initialized = false;
}

void Heightfield::resize(u32 resolution) {
    m_desc.resolution = resolution;
    m_heights.assign(static_cast<usize>(resolution) * static_cast<usize>(resolution), 0.f);
}

void Heightfield::fill(f32 height) {
    std::fill(m_heights.begin(), m_heights.end(), height);
}

void Heightfield::set_height(u32 x, u32 z, f32 height) {
    if (!in_bounds(x, z)) {
        return;
    }
    m_heights[static_cast<usize>(z) * m_desc.resolution + x] = height;
}

f32 Heightfield::get_height_texel(u32 x, u32 z) const {
    if (!in_bounds(x, z)) {
        return 0.f;
    }
    return m_heights[static_cast<usize>(z) * m_desc.resolution + x];
}

f32 Heightfield::sample_height(f32 world_x, f32 world_z) const {
    if (!m_initialized || m_desc.resolution < 2) {
        return 0.f;
    }

    const f32 meters_per_texel = m_desc.world_size / static_cast<f32>(m_desc.resolution - 1);
    const f32 u = world_x / meters_per_texel;
    const f32 v = world_z / meters_per_texel;
    return sample_bilinear(u, v);
}

vec3 Heightfield::sample_normal(f32 world_x, f32 world_z) const {
    if (!m_initialized || m_desc.resolution < 2) {
        return {0.f, 1.f, 0.f};
    }

    const f32 meters_per_texel = m_desc.world_size / static_cast<f32>(m_desc.resolution - 1);
    const f32 du = meters_per_texel;
    const f32 hL = sample_height(world_x - du, world_z);
    const f32 hR = sample_height(world_x + du, world_z);
    const f32 hD = sample_height(world_x, world_z - du);
    const f32 hU = sample_height(world_x, world_z + du);

    const vec3 tangent_x = {2.f * du, hR - hL, 0.f};
    const vec3 tangent_z = {0.f, hU - hD, 2.f * du};
    const vec3 normal = {
        tangent_z.y * tangent_x.z - tangent_z.z * tangent_x.y,
        tangent_z.z * tangent_x.x - tangent_z.x * tangent_x.z,
        tangent_z.x * tangent_x.y - tangent_z.y * tangent_x.x,
    };
    return normal.normalized();
}

f32 Heightfield::sample_bilinear(f32 u, f32 v) const {
    const f32 max_coord = static_cast<f32>(m_desc.resolution - 1);
    const f32 clamped_u = std::clamp(u, 0.f, max_coord);
    const f32 clamped_v = std::clamp(v, 0.f, max_coord);

    const u32 x0 = static_cast<u32>(clamped_u);
    const u32 z0 = static_cast<u32>(clamped_v);
    const u32 x1 = std::min(x0 + 1, m_desc.resolution - 1);
    const u32 z1 = std::min(z0 + 1, m_desc.resolution - 1);

    const f32 tx = clamped_u - static_cast<f32>(x0);
    const f32 tz = clamped_v - static_cast<f32>(z0);

    const f32 h00 = get_height_texel(x0, z0);
    const f32 h10 = get_height_texel(x1, z0);
    const f32 h01 = get_height_texel(x0, z1);
    const f32 h11 = get_height_texel(x1, z1);

    const f32 h0 = h00 + (h10 - h00) * tx;
    const f32 h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * tz;
}

bool Heightfield::in_bounds(u32 x, u32 z) const {
    return x < m_desc.resolution && z < m_desc.resolution;
}

} // namespace fuse::terrain
